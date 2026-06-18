"""Agent 服务入口 —— 编排 Redis 消费 → 状态机 → 结果回写的完整链路。"""

from __future__ import annotations

import json
import signal

import structlog

from ssv_agent.config import SsvConfig
from ssv_agent.context.budget import BudgetEngine
from ssv_agent.context.engine import ContextEngine
from ssv_agent.context.history_manager import HistoryManager
from ssv_agent.context.retrieval_manager import RetrievalManager
from ssv_agent.context.system_prompt import SystemPromptManager
from ssv_agent.context.tool_definitions import ToolDefinitionsRenderer
from ssv_agent.context.user_input import UserInputBuilder
from ssv_agent.event_consumer import EventConsumer
from ssv_agent.factories import build_provider, build_tool_router
from ssv_agent.integrations.evidence import EventEvidenceProvider
from ssv_agent.integrations.mocks import (
    MockTrackContextProvider,
    MockSourceMetadataProvider,
    MockEventSequenceProvider,
)
from ssv_agent.models.event import DetectionEvent, ReviewResult
from ssv_agent.prompts.assembler import PromptAssembler
from ssv_agent.prompts.logger import PromptLogger
from ssv_agent.state_machine.machine import StateMachine
from ssv_agent.tools.router import ToolRouter
from ssv_agent.writer.result_writer import ResultWriter

logger = structlog.get_logger()


class AgentService:
    """Agent 主服务 —— 消费 Redis 事件，驱动状态机复核，回写结果。

    用法:
        config = load_config()
        service = AgentService(config, mock_provider=True)
        service.run()
    """

    def __init__(
        self,
        config: SsvConfig,
        mock_provider: bool = True,
        redis_writer: bool = True,
    ) -> None:
        self._config = config
        self._mock_provider = mock_provider
        self._running = False

        # 构造依赖链
        self._consumer = EventConsumer(config)
        self._writer = ResultWriter(
            redis_client=self._consumer.redis_client if redis_writer else None,
            stream_key=config.agent.review_result_stream,
        )
        self._tool_router = build_tool_router(config)

        # ── M4 上下文工程模块 ────────────────────────────────────────────
        self._system_prompt_mgr = SystemPromptManager(
            debug_mode=config.agent.context_engine_debug,
        )
        self._tool_renderer = ToolDefinitionsRenderer()
        self._history_mgr = HistoryManager(
            max_window_size=config.agent.context_history_max_window,
        )
        self._retrieval_mgr = RetrievalManager(
            rules_path=config.agent.rules_yaml_path,
        )
        self._ui_builder = UserInputBuilder()

        # 集成接口：证据来自事件路径，其他上游上下文待 T1/T2/T3 合流后替换。
        self._evidence_provider = EventEvidenceProvider()
        self._track_provider = MockTrackContextProvider()
        self._metadata_provider = MockSourceMetadataProvider()
        self._sequence_provider = MockEventSequenceProvider()

        self._context_engine = ContextEngine(
            system_prompt_manager=self._system_prompt_mgr,
            tool_renderer=self._tool_renderer,
            history_manager=self._history_mgr,
            retrieval_manager=self._retrieval_mgr,
            user_input_builder=self._ui_builder,
            evidence_provider=self._evidence_provider,
            track_provider=self._track_provider,
            metadata_provider=self._metadata_provider,
            sequence_provider=self._sequence_provider,
        )
        self._budget_engine = BudgetEngine()

        # 提示词模块
        self._assembler = PromptAssembler(
            max_tokens=config.agent.prompt_max_tokens,
        )
        # mock 模式下不启用提示词日志（避免无意义的日志堆积）
        self._prompt_logger = PromptLogger(
            enabled=(not mock_provider and config.agent.prompt_log_enabled),
        )

        provider = build_provider(config, mock_provider)

        self._machine = StateMachine(
            provider=provider,
            tool_router=self._tool_router,
            result_writer=self._writer,
            context_engine=self._context_engine,  # M4 新增
            timeout=config.agent.state_machine_timeout,
        )

    @property
    def consumer(self) -> EventConsumer:
        return self._consumer

    @property
    def machine(self) -> StateMachine:
        return self._machine

    @property
    def assembler(self) -> PromptAssembler:
        return self._assembler

    @property
    def prompt_logger(self) -> PromptLogger:
        return self._prompt_logger

    @property
    def context_engine(self) -> ContextEngine:
        return self._context_engine

    @property
    def budget_engine(self) -> BudgetEngine:
        return self._budget_engine

    @property
    def history_manager(self) -> HistoryManager:
        return self._history_mgr

    @property
    def tool_router(self) -> ToolRouter:
        return self._tool_router

    def run(self) -> None:
        """启动 Agent 主循环。"""
        logger.info(
            "agent service starting",
            version=self._config.version,
            redis=f"{self._config.redis.host}:{self._config.redis.port}",
            stream=self._config.redis.stream_key,
            review_stream=self._config.agent.review_result_stream,
            mock_provider=self._mock_provider,
            provider_type=self._config.agent.provider_type,
            local_yolo_model=self._config.agent.local_yolo_model_path,
            prompt_max_tokens=self._config.agent.prompt_max_tokens,
            prompt_log_enabled=self._prompt_logger.enabled,
        )

        self._running = True
        self._consumer.start(self._handle_event)

    def stop(self) -> None:
        """优雅停止。"""
        self._running = False
        self._consumer.stop()
        logger.info("agent service stopped")

    def _handle_event(self, msg_id: str, fields: dict[str, str]) -> None:
        """处理单条 Redis 事件：解析 → 状态机 → 回写 → ACK。"""
        raw = fields.get("event", "{}")
        try:
            data = json.loads(raw)
            self.process_event_data(data, msg_id=msg_id)
        except Exception as exc:
            logger.warning("malformed event", msg_id=msg_id, error=str(exc))
            return

        # 结果已由 StateMachine 内部通过 ResultWriter 回写
        # ACK 事件
        self._consumer.ack(msg_id)

    def process_event_data(
        self,
        data: dict,
        msg_id: str = "manual-0",
    ) -> ReviewResult:
        """处理单条事件数据，供 Redis 回调和 one-shot 调试入口复用。"""
        event = DetectionEvent.model_validate(data)

        logger.info(
            "processing event",
            msg_id=msg_id,
            source=event.source,
            frame_id=event.frame_id,
            detections=len(event.detections),
        )

        # 状态机驱动复核
        result = self._machine.execute(event)
        self._history_mgr.record(
            event_id=result.event_id,
            track_ids=[d.track_id for d in event.detections],
            timestamp_ms=event.timestamp_ms,
            severity=result.severity.value,
            trigger_reason=event.trigger_reason.value,
            strategy=result.strategy.value,
            conclusion_summary=result.conclusion,
        )
        logger.info(
            "review complete",
            msg_id=msg_id,
            state=result.final_state.value,
            strategy=result.strategy.value,
        )

        return result


def run(config: SsvConfig, mock_provider: bool = True) -> None:
    """旧版兼容入口 —— 创建 AgentService 并运行。"""
    service = AgentService(config, mock_provider=mock_provider)

    def _shutdown(sig: int, _frame: object) -> None:
        logger.info("received signal, stopping service", signal=sig)
        service.stop()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    service.run()
