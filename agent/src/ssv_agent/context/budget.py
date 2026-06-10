"""上下文预算管理 —— BudgetEngine 负责跨要素配额分配，PromptAssembler 负责执行截断。

设计原则（D2）:
  BudgetEngine 分配配额（策略相关），PromptAssembler 执行截断（token 相关）。
  分离后各自可独立测试。
"""

from __future__ import annotations

from dataclasses import dataclass

import structlog

logger = structlog.get_logger()

# ── 按策略的要素 token 上限 ─────────────────────────────────────────────────
#
# 注: 当前 M4 阶段使用字符数/2 估算 token 数，M5 接入 tiktoken/Anthropic API 精确计数。

BUDGET_ALLOCATIONS: dict[str, dict[str, int]] = {
    "direct_confirm": {
        "system_prompt": 800,
        "tool_definitions": 200,
        "user_input_base": 400,
        "user_input_details": 600,
        "retrieval_context": 200,
        "history": 300,
        "few_shot": 300,
    },
    "visual_review": {
        "system_prompt": 800,
        "tool_definitions": 400,
        "user_input_base": 400,
        "user_input_details": 600,
        "retrieval_context": 400,
        "history": 400,
        "few_shot": 300,
    },
    "rule_explain": {
        "system_prompt": 800,
        "tool_definitions": 400,
        "user_input_base": 400,
        "user_input_details": 400,
        "retrieval_context": 800,     # 需要规则条文，给最多预算
        "history": 400,
        "few_shot": 200,
    },
    "notify_report": {
        "system_prompt": 800,
        "tool_definitions": 400,
        "user_input_base": 400,
        "user_input_details": 600,
        "retrieval_context": 400,
        "history": 300,
        "few_shot": 300,
    },
}


@dataclass
class TokenBudget:
    """上下文 token 预算 —— BudgetEngine.allocate() 的输出。"""

    allocations: dict[str, int]    # 要素名 → token 上限
    total: int                      # 总预算
    strategy: str                   # 使用的策略


class BudgetEngine:
    """上下文预算引擎 —— 按策略为五要素分配 token 上限。

    用法:
        engine = BudgetEngine()
        budget = engine.allocate(pack, strategy)
        pack.token_budget = budget
    """

    def allocate(self, strategy: str) -> TokenBudget:
        """按策略分配各要素 token 上限。

        Args:
            strategy: 复核策略名。

        Returns:
            TokenBudget: 各要素 token 配额。
        """
        allocations = BUDGET_ALLOCATIONS.get(strategy)
        if allocations is None:
            # 未知策略 → 使用 direct_confirm 的保守默认值
            logger.warning(
                "unknown strategy, using default budget",
                strategy=strategy,
            )
            allocations = BUDGET_ALLOCATIONS["direct_confirm"]

        total = sum(allocations.values())
        budget = TokenBudget(
            allocations=dict(allocations),
            total=total,
            strategy=strategy,
        )

        logger.debug(
            "budget allocated",
            strategy=strategy,
            total=total,
            allocations=allocations,
        )
        return budget
