"""LLM 状态机 —— 驱动事件复核流程的状态迁移引擎。

状态流转:
  PENDING → PARSING → CONTEXT_BUILDING → STRATEGY_SELECTING
    → TOOL_CALLING → RESULT_AGGREGATING → RESULT_WRITING
    → COMPLETED / FAILED / NEEDS_HUMAN

四类复核策略:
  - DIRECT_CONFIRM: 高置信度违规 → 跳过工具调用，直接确认
  - VISUAL_REVIEW: 低置信度/遮挡 → 调用 VLM provider 复核关键帧
  - RULE_EXPLAIN: 复杂场景 → 结合安全规则解释
  - NOTIFY_REPORT: 严重事件 → 生成通知内容
"""

from __future__ import annotations

import structlog
from typing import Optional, Protocol

from ssv_agent.models.event import (
    DetectionEvent,
    EventState,
    ReviewContext,
    ReviewResult,
    ReviewStrategy,
    ToolResult,
)
from ssv_agent.state_machine.states import StateContext

logger = structlog.get_logger()


# ── Provider 协议（避免循环导入） ─────────────────────────────────────────


class ProviderProtocol(Protocol):
    """Provider 协议 — StateMachine 只依赖此协议，不依赖具体实现。"""

    def analyze(self, context: ReviewContext) -> str:
        """分析复核上下文，返回结论文本。"""
        ...


# ── 工具路由器协议 ────────────────────────────────────────────────────────


class ToolRouterProtocol(Protocol):
    """工具路由器协议 — StateMachine 只依赖此协议。"""

    def call_tool(self, name: str, params: dict) -> ToolResult:
        """调用指定工具，返回结果。"""
        ...


# ── 结果回写器协议 ────────────────────────────────────────────────────────


class ResultWriterProtocol(Protocol):
    """结果回写器协议。"""

    def write(self, result: ReviewResult) -> None:
        """回写复核结果。"""
        ...


# ── 状态机 ─────────────────────────────────────────────────────────────────


class StateMachine:
    """Agent 复核状态机。

    接收 DetectionEvent，按四类策略驱动状态流转，最终产生 ReviewResult。

    用法:
        machine = StateMachine(provider, tool_router, result_writer, timeout=300)
        result = machine.execute(event)

    M4 上下文工程:
        machine = StateMachine(
            provider=provider,
            tool_router=router,
            context_engine=engine,  # 新增
            ...
        )
    """

    def __init__(
        self,
        provider: Optional[ProviderProtocol] = None,
        tool_router: Optional[ToolRouterProtocol] = None,
        result_writer: Optional[ResultWriterProtocol] = None,
        context_engine: Optional[object] = None,  # ContextEngine (避免循环导入)
        timeout: int = 300,
    ) -> None:
        self._provider = provider
        self._tool_router = tool_router
        self._result_writer = result_writer
        self._context_engine = context_engine
        self._timeout = timeout

    # ── 主入口 ─────────────────────────────────────────────────────────

    def execute(self, event: DetectionEvent) -> ReviewResult:
        """执行完整复核流程，返回 ReviewResult。"""
        ctx = StateContext(event=event, timeout_seconds=self._timeout)
        ctx.provider_used = (
            getattr(self._provider, "provider_name", "unknown")
            if self._provider is not None
            else "none"
        )

        logger.info(
            "state machine started",
            source=event.source,
            frame_id=event.frame_id,
            detections=len(event.detections),
        )

        try:
            self._run_states(ctx)
        except Exception as exc:
            logger.exception("state machine error", error=str(exc))
            ctx.transition(EventState.FAILED)
            ctx.set_error(str(exc))

        result = ctx.build_result()
        logger.info(
            "state machine finished",
            state=result.final_state.value,
            strategy=result.strategy.value,
            elapsed=ctx.elapsed_seconds,
        )
        return result

    def _run_states(self, ctx: StateContext) -> None:
        """按顺序驱动状态流转。

        COMPLETED / FAILED / NEEDS_HUMAN 都要进入回写，保证下游能观察终态。
        """
        steps = [
            self._do_parsing,
            self._do_context_building,
            self._do_strategy_selection,
            self._do_tool_calling,
            self._do_result_aggregation,
        ]
        for step in steps:
            step(ctx)
            if ctx.state in (EventState.FAILED, EventState.NEEDS_HUMAN):
                break

        self._do_result_writing(ctx)

    # ── 各状态实现 ─────────────────────────────────────────────────────

    def _do_parsing(self, ctx: StateContext) -> None:
        """解析事件：自动推断严重程度和触发原因。"""
        ctx.transition(EventState.PARSING)
        # 始终自动推断（上游 event 可能未设置 severity/trigger_reason）
        ctx.event.severity = ctx.event.infer_severity()
        ctx.event.trigger_reason = ctx.event.infer_trigger_reason()
        logger.debug(
            "event parsed",
            severity=ctx.event.severity.value,
            trigger=ctx.event.trigger_reason.value,
        )

    def _do_context_building(self, ctx: StateContext) -> None:
        """构造复核上下文 —— M4 集成 ContextEngine。

        降级: ContextEngine 未初始化时回退为简单 ReviewContext。
        """
        ctx.transition(EventState.CONTEXT_BUILDING)

        if self._context_engine is not None:
            # M4: 通过 ContextEngine.collect() 产出 ContextPack
            try:
                strategy = ctx.event.select_strategy().value
                tool_definitions = (
                    getattr(self._tool_router, "tool_definitions", [])
                    if self._tool_router is not None
                    else []
                )
                pack = self._context_engine.collect(
                    event=ctx.event,
                    strategy=strategy,
                    tool_definitions=tool_definitions,
                )
                ctx.context = pack  # 存储 ContextPack（替代 ReviewContext）
                logger.debug(
                    "context pack collected via ContextEngine",
                    context_id=pack.context_id,
                    strategy=strategy,
                )
                return
            except Exception as exc:
                logger.warning(
                    "ContextEngine.collect() failed, falling back to ReviewContext",
                    error=str(exc),
                )

        # 降级: 使用旧 ReviewContext
        ctx.context = ReviewContext(
            event=ctx.event,
            strategy=ctx.strategy,
        )

    def _do_strategy_selection(self, ctx: StateContext) -> None:
        """选择复核策略。"""
        ctx.transition(EventState.STRATEGY_SELECTING)
        ctx.strategy = ctx.event.select_strategy()

        # 检测 ctx.context 类型（M4 兼容 ContextPack 和 ReviewContext）
        from ssv_agent.context.pack import ContextPack

        if isinstance(ctx.context, ContextPack):
            # ContextPack 已包含 strategy 元数据，无需重建
            ctx.context.metadata["strategy"] = ctx.strategy.value
        else:
            # 旧 ReviewContext 路径
            ctx.context = ReviewContext(
                event=ctx.event,
                strategy=ctx.strategy,
                evidence_summary=ctx.context.evidence_summary if ctx.context else "",
                rule_snippets=ctx.context.rule_snippets if ctx.context else [],
            )
        logger.info("strategy selected", strategy=ctx.strategy.value)

    def _do_tool_calling(self, ctx: StateContext) -> None:
        """根据策略执行工具调用。"""
        ctx.transition(EventState.TOOL_CALLING)

        if ctx.strategy == ReviewStrategy.DIRECT_CONFIRM:
            # 直接确认：无需工具调用
            logger.info("direct confirm — skipping tool calls")
            return

        if ctx.strategy == ReviewStrategy.VISUAL_REVIEW:
            self._handle_visual_review(ctx)
        elif ctx.strategy == ReviewStrategy.RULE_EXPLAIN:
            self._handle_rule_explain(ctx)
        elif ctx.strategy == ReviewStrategy.NOTIFY_REPORT:
            self._handle_notify_report(ctx)

    def _handle_visual_review(self, ctx: StateContext) -> None:
        """视觉复核：调用 VLM provider 复核关键帧。"""
        if self._provider:
            try:
                conclusion = self._provider.analyze(ctx.context)
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="visual_review",
                        success=True,
                        output=conclusion,
                    )
                )
                logger.info("visual review completed", conclusion=conclusion[:80])
            except Exception as exc:
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="visual_review",
                        success=False,
                        error=str(exc),
                    )
                )
                logger.warning("visual review failed", error=str(exc))
        else:
            ctx.record_tool_result(
                ToolResult(
                    tool_name="visual_review",
                    success=False,
                    error="provider unavailable",
                )
            )

    def _handle_rule_explain(self, ctx: StateContext) -> None:
        """规则解释：调用 tool_router 检索规则 + LLM provider 解释。"""
        # 先尝试检索规则
        if self._tool_router:
            try:
                result = self._tool_router.call_tool(
                    "rule_retrieval",
                    {"event_type": ctx.event.trigger_reason.value},
                )
                ctx.record_tool_result(result)
            except Exception as exc:
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="rule_retrieval",
                        success=False,
                        error=str(exc),
                    )
                )

        # 再调用 LLM provider 解释
        if self._provider:
            try:
                conclusion = self._provider.analyze(ctx.context)
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="rule_explain",
                        success=True,
                        output=conclusion,
                    )
                )
            except Exception as exc:
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="rule_explain",
                        success=False,
                        error=str(exc),
                    )
                )
        else:
            ctx.record_tool_result(
                ToolResult(
                    tool_name="rule_explain",
                    success=False,
                    error="provider unavailable",
                )
            )

    def _handle_notify_report(self, ctx: StateContext) -> None:
        """通知报告：生成通知内容。"""
        if self._tool_router:
            try:
                result = self._tool_router.call_tool(
                    "generate_notification",
                    {
                        "severity": ctx.event.severity.value,
                        "source": ctx.event.source,
                        "summary": ctx.event.detections,
                    },
                )
                ctx.record_tool_result(result)
            except Exception as exc:
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="generate_notification",
                        success=False,
                        error=str(exc),
                    )
                )

        if self._provider:
            try:
                report = self._provider.analyze(ctx.context)
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="report_generation",
                        success=True,
                        output=report,
                    )
                )
            except Exception as exc:
                ctx.record_tool_result(
                    ToolResult(
                        tool_name="report_generation",
                        success=False,
                        error=str(exc),
                    )
                )
        else:
            ctx.record_tool_result(
                ToolResult(
                    tool_name="report_generation",
                    success=False,
                    error="provider unavailable",
                )
            )

    def _do_result_aggregation(self, ctx: StateContext) -> None:
        """汇总结果：检查超时和工具调用失败。"""
        ctx.transition(EventState.RESULT_AGGREGATING)

        if ctx.has_timed_out:
            ctx.transition(EventState.NEEDS_HUMAN)
            ctx.set_error(f"超时: {ctx.elapsed_seconds:.0f}s > {ctx.timeout_seconds}s")
            logger.warning("state machine timed out", elapsed=ctx.elapsed_seconds)
            return

        # 所有工具都失败 → 需要人工
        if ctx.tool_results and all(not t.success for t in ctx.tool_results):
            ctx.transition(EventState.NEEDS_HUMAN)
            ctx.set_error("所有工具调用均失败")
            return

        ctx.transition(EventState.COMPLETED)

    def _do_result_writing(self, ctx: StateContext) -> None:
        """回写结果（不修改终态，由 aggregation 已设置）。"""
        result = ctx.build_result()

        if self._result_writer:
            try:
                self._result_writer.write(result)
            except Exception as exc:
                logger.error("result write failed", error=str(exc))
