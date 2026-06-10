"""提示词调试日志 —— 记录每次 LLM 调用的完整 prompt 和 response。

设计原则:
  1. PromptLogger 是调试工具而非业务模块，不影响核心流程
  2. 在 mock 模式下不启用（避免无意义的日志堆积）
  3. 在真实 LLM 模式下默认启用，可通过配置关闭
  4. 使用现有 structlog，不引入新依赖
  5. CallRecord 输出为结构化日志字段，支持 grep/jq 快速过滤
"""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Optional

import structlog

from ssv_agent.prompts.assembler import PromptAssembly, PromptMessage

logger = structlog.get_logger()


@dataclass
class CallRecord:
    """单次 LLM 调用的完整记录。

    包含发送的 prompt、模型返回的 response、元信息等，
    是调试 bad case 的基础数据单元。
    """

    call_id: str                                          # 唯一标识
    timestamp: float = field(default_factory=time.time)   # 调用时间戳
    strategy: str = ""                                    # 使用的复核策略
    template_version: str = ""                            # 模板版本号
    messages: list[PromptMessage] = field(default_factory=list)  # 完整发送的 prompt
    response: str = ""                                    # 模型原始返回文本
    parsed_result: Optional[dict] = None                  # 解析后的 ReviewResult（如有）
    latency_ms: float = 0.0                               # 调用耗时（毫秒）
    model_name: str = ""                                  # 使用的模型名称
    error: str = ""                                       # 错误信息（成功时为空）
    truncated: bool = False                               # 是否触发了 token 截断

    # ── M4 新增字段（10.3 节） ───────────────────────────────────────────
    system_prompt_versions: dict[str, str] = field(default_factory=dict)
    """各层系统提示词版本快照，来源: SystemPromptManager.render()"""
    context_pack_id: str = ""
    """来源 ContextPack 的唯一标识"""
    budget_allocations: dict[str, int] = field(default_factory=dict)
    """各要素 token 配额，来源: BudgetEngine.allocate()"""
    compression_summary: dict = field(default_factory=dict)
    """历史压缩摘要: 压缩级别、丢弃条数、压缩比，来源: HistoryManager.get_window()"""
    truncation_details: dict = field(default_factory=dict)
    """截断详情: 哪些要素被截断、截断前后大小，来源: PromptAssembler._apply_token_budget()"""


class PromptLogger:
    """提示词调试日志器。

    记录每次 LLM 调用的完整交互，支持按 call_id 回溯。

    用法:
        plog = PromptLogger(enabled=True, max_records=500)
        call_id = plog.log_call(assembly, response, latency_ms=1234, model_name="gpt-4")
        record = plog.get_call(call_id)

    Mock 模式下应设置 enabled=False，避免无意义的日志堆积。
    """

    def __init__(self, enabled: bool = True, max_records: int = 1000) -> None:
        self._enabled = enabled
        self._max_records = max_records
        self._records: dict[str, CallRecord] = {}

    # ── 公共接口 ───────────────────────────────────────────────────────

    def log_call(
        self,
        assembly: PromptAssembly,
        response: str,
        latency_ms: float = 0.0,
        model_name: str = "",
        parsed_result: Optional[dict] = None,
    ) -> str:
        """记录一次成功的 LLM 调用。

        Args:
            assembly: PromptAssembler.assemble() 的返回结果。
            response: 模型原始返回文本。
            latency_ms: 调用耗时（毫秒）。
            model_name: 使用的模型名称。
            parsed_result: 解析后的结构化结果（如有）。

        Returns:
            call_id: 本次调用的唯一标识（disabled 时返回空字符串）。
        """
        if not self._enabled:
            return ""

        call_id = self._generate_call_id()
        record = CallRecord(
            call_id=call_id,
            strategy=assembly.strategy,
            template_version=assembly.template_version,
            messages=assembly.messages,
            response=response,
            parsed_result=parsed_result,
            latency_ms=latency_ms,
            model_name=model_name,
            truncated=assembly.truncated,
            # M4 新增字段
            system_prompt_versions=assembly.metadata.get(
                "system_prompt_versions", {}
            ),
            context_pack_id=assembly.metadata.get("context_pack_id", ""),
            budget_allocations=assembly.metadata.get("budget_allocations", {}),
            truncation_details=assembly.metadata.get("truncation_details", {}),
        )
        self._store(call_id, record)
        self._emit_log(record)
        return call_id

    def log_error(
        self,
        assembly: PromptAssembly,
        error: str,
        latency_ms: float = 0.0,
        model_name: str = "",
    ) -> str:
        """记录一次失败的 LLM 调用。

        Args:
            assembly: PromptAssembler.assemble() 的返回结果。
            error: 错误信息。
            latency_ms: 调用耗时（毫秒）。
            model_name: 使用的模型名称。

        Returns:
            call_id: 本次调用的唯一标识（disabled 时返回空字符串）。
        """
        if not self._enabled:
            return ""

        call_id = self._generate_call_id()
        record = CallRecord(
            call_id=call_id,
            strategy=assembly.strategy,
            template_version=assembly.template_version,
            messages=assembly.messages,
            error=error,
            latency_ms=latency_ms,
            model_name=model_name,
            truncated=assembly.truncated,
            # M4 新增字段
            system_prompt_versions=assembly.metadata.get(
                "system_prompt_versions", {}
            ),
            context_pack_id=assembly.metadata.get("context_pack_id", ""),
            budget_allocations=assembly.metadata.get("budget_allocations", {}),
            truncation_details=assembly.metadata.get("truncation_details", {}),
        )
        self._store(call_id, record)
        self._emit_log(record, is_error=True)
        return call_id

    def get_call(self, call_id: str) -> Optional[CallRecord]:
        """按 call_id 获取调用记录。

        Returns:
            CallRecord 或 None（如果 call_id 不存在）。
        """
        return self._records.get(call_id)

    @property
    def enabled(self) -> bool:
        """是否已启用日志记录。"""
        return self._enabled

    @property
    def record_count(self) -> int:
        """当前存储的记录数。"""
        return len(self._records)

    # ── 内部方法 ───────────────────────────────────────────────────────

    def _generate_call_id(self) -> str:
        """生成唯一调用标识。"""
        return f"call_{uuid.uuid4().hex[:12]}"

    def _store(self, call_id: str, record: CallRecord) -> None:
        """存储记录，超过上限时淘汰最旧记录。"""
        if len(self._records) >= self._max_records:
            # 淘汰最旧的一条（按插入顺序）
            oldest = next(iter(self._records))
            del self._records[oldest]
        self._records[call_id] = record

    def _emit_log(self, record: CallRecord, is_error: bool = False) -> None:
        """通过 structlog 输出结构化日志。

        输出字段支持后续 grep/jq 快速过滤。
        """
        log_data = {
            "call_id": record.call_id,
            "strategy": record.strategy,
            "template_version": record.template_version,
            "model_name": record.model_name,
            "latency_ms": record.latency_ms,
            "truncated": record.truncated,
            "response_preview": record.response[:200] if record.response else "",
            "message_count": len(record.messages),
        }

        if is_error:
            log_data["error"] = record.error
            logger.error("llm call failed", **log_data)
        else:
            logger.info("llm call recorded", **log_data)
