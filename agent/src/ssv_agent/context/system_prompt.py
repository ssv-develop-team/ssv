"""系统提示词管理器 —— 分层渲染 + 版本管理 + 硬约束检查。

设计原则（D1/D7）:
  prompts/system.py 的 DEFAULT_SYSTEM_PROMPT 为 canonical source。
  prompts/manager.py 废弃，功能合并到本模块。
"""

from __future__ import annotations

import re
from typing import Optional

import structlog

from ssv_agent.prompts.system import DEFAULT_SYSTEM_PROMPT, SystemPrompt
from ssv_agent.prompts.templates import STRATEGY_SYSTEM_TEMPLATES

logger = structlog.get_logger()

# ── 硬约束阈值 ──────────────────────────────────────────────────────────────

MAX_TOTAL_CHARS = 2000        # L0-L3 总字符数上限（不含工具层）
MAX_LAYER_CHARS = 500         # 每层字符数上限
MAX_PRINCIPLES = 8            # 判断原则条数上限
MAX_PRINCIPLE_CHARS = 80      # 每条原则字符数上限

# ── 层 key 常量 ─────────────────────────────────────────────────────────────

LAYER_ROLE = "role"
LAYER_DOMAIN_KNOWLEDGE = "domain_knowledge"
LAYER_STRATEGY_GUIDANCE = "strategy_guidance"
LAYER_OUTPUT_CONSTRAINTS = "output_constraints"
LAYER_TOOL_USAGE = "tool_usage"

# 默认层顺序
DEFAULT_LAYER_ORDER = [
    LAYER_ROLE,
    LAYER_DOMAIN_KNOWLEDGE,
    LAYER_STRATEGY_GUIDANCE,
    LAYER_OUTPUT_CONSTRAINTS,
    LAYER_TOOL_USAGE,
]

# 各层版本号
DEFAULT_VERSIONS = {
    LAYER_ROLE: "1.0.0",
    LAYER_DOMAIN_KNOWLEDGE: "1.2.0",
    LAYER_STRATEGY_GUIDANCE: "1.1.0",
    LAYER_OUTPUT_CONSTRAINTS: "1.0.1",
    LAYER_TOOL_USAGE: "auto",
}


# ── SystemPrompt 扩展 ───────────────────────────────────────────────────────


def _split_system_prompt_layers(content: str) -> dict[str, str]:
    """将当前平铺的 system prompt 文本按 ## 标题切分为五层。

    当前 DEFAULT_SYSTEM_PROMPT.content 的结构:
      ## 你的职责        → L0 (role)
      ## 判断原则        → L1 (domain_knowledge)
      ## 输出要求        → L3 (output_constraints)
      ## 注意事项        → L3 (output_constraints, 合并)

    L2 (strategy_guidance) 由 templates.py 的 STRATEGY_SYSTEM_TEMPLATES 提供。
    L4 (tool_usage) 由 ToolDefinitionsRenderer 动态生成。
    """
    layers: dict[str, str] = {}

    # 按 ## 标题拆分
    sections = re.split(r"\n(?=## )", content)
    role_parts: list[str] = []
    knowledge_parts: list[str] = []
    constraint_parts: list[str] = []

    for section in sections:
        stripped = section.strip()
        if stripped.startswith("## 你的职责"):
            role_parts.append(stripped)
        elif stripped.startswith("## 判断原则"):
            knowledge_parts.append(stripped)
        elif stripped.startswith("## 输出要求"):
            constraint_parts.append(stripped)
        elif stripped.startswith("## 注意事项"):
            constraint_parts.append(stripped)
        else:
            # 未匹配的内容归入 role 层（如开头的角色陈述）
            if stripped:
                role_parts.append(stripped)

    layers[LAYER_ROLE] = "\n\n".join(role_parts)
    layers[LAYER_DOMAIN_KNOWLEDGE] = "\n\n".join(knowledge_parts)
    layers[LAYER_OUTPUT_CONSTRAINTS] = "\n\n".join(constraint_parts)
    # L2 和 L4 由外部注入
    layers[LAYER_STRATEGY_GUIDANCE] = ""
    layers[LAYER_TOOL_USAGE] = ""

    return layers


# ── SystemPromptManager ──────────────────────────────────────────────────────


class SystemPromptManager:
    """系统提示词管理器 —— 分层渲染、版本快照、硬约束检查。

    用法:
        mgr = SystemPromptManager()
        block = mgr.render(strategy="direct_confirm")
        # block.content → 完整系统提示词
        # block.versions → 各层版本快照
    """

    def __init__(
        self,
        system_prompt: Optional[SystemPrompt] = None,
        debug_mode: bool = False,
    ) -> None:
        self._system_prompt = system_prompt or DEFAULT_SYSTEM_PROMPT
        self._debug = debug_mode
        # 缓存分层结果
        self._layers: dict[str, str] = _split_system_prompt_layers(
            self._system_prompt.content
        )
        self._versions: dict[str, str] = dict(DEFAULT_VERSIONS)
        self._render_count = 0

    # ── 公共接口 ─────────────────────────────────────────────────────────

    def render(
        self,
        strategy: str = "",
        tool_definitions_text: str = "",
        layers: Optional[list[str]] = None,
    ) -> tuple[str, dict[str, str]]:
        """分层渲染系统提示词。

        Args:
            strategy: 复核策略名（用于注入 L2 策略指引）。
            tool_definitions_text: 工具定义的文本形式（L4）。
            layers: 要包含的层列表，None 表示全部。

        Returns:
            (渲染后的文本, 版本快照)
        """
        self._render_count += 1

        # 注入 L2 策略指引
        if strategy and strategy in STRATEGY_SYSTEM_TEMPLATES:
            self._layers[LAYER_STRATEGY_GUIDANCE] = STRATEGY_SYSTEM_TEMPLATES[strategy]
        else:
            self._layers[LAYER_STRATEGY_GUIDANCE] = ""

        # 注入 L4 工具说明
        self._layers[LAYER_TOOL_USAGE] = tool_definitions_text

        # 按指定层顺序渲染
        layer_order = layers or DEFAULT_LAYER_ORDER
        parts: list[str] = []
        for key in layer_order:
            if key in self._layers and self._layers[key]:
                parts.append(self._layers[key])

        rendered = "\n\n".join(parts)

        # 硬约束检查
        self._check_constraints(rendered, layer_order)

        # 版本快照
        versions = {k: self._versions.get(k, "auto") for k in layer_order}

        logger.debug(
            "system prompt rendered",
            strategy=strategy,
            layer_count=len(parts),
            total_chars=len(rendered),
            versions=versions,
            render_count=self._render_count,
        )
        return rendered, versions

    @property
    def layers(self) -> dict[str, str]:
        """获取当前分层内容（只读）。"""
        return dict(self._layers)

    @property
    def versions(self) -> dict[str, str]:
        """获取当前版本快照（只读）。"""
        return dict(self._versions)

    @property
    def render_count(self) -> int:
        """已渲染次数。"""
        return self._render_count

    # ── A/B 变体预留接口 ─────────────────────────────────────────────────

    def select_version(self, variant: str) -> None:
        """预留：按变体名选择提示词版本。

        当前阶段仅记录日志，M5 接入真实 LLM 后激活。
        """
        logger.info(
            "version variant selected (reserved interface)",
            variant=variant,
            current_versions=self._versions,
        )

    # ── 硬约束检查 ───────────────────────────────────────────────────────

    def _check_constraints(self, rendered: str, layer_order: list[str]) -> None:
        """检查渲染结果是否超限。"""
        issues: list[str] = []

        # L0-L3 总字符数检查（不含工具层）
        l0_l3_keys = [k for k in layer_order if k != LAYER_TOOL_USAGE]
        l0_l3_chars = sum(
            len(self._layers.get(k, "")) for k in l0_l3_keys
        )
        if l0_l3_chars > MAX_TOTAL_CHARS:
            msg = (
                f"系统提示词总字符数 {l0_l3_chars} 超过上限 "
                f"{MAX_TOTAL_CHARS}（L0-L3）"
            )
            issues.append(msg)

        # 每层字符数检查
        for key in l0_l3_keys:
            layer_chars = len(self._layers.get(key, ""))
            if layer_chars > MAX_LAYER_CHARS:
                msg = (
                    f"层 '{key}' 字符数 {layer_chars} 超过上限 {MAX_LAYER_CHARS}"
                )
                issues.append(msg)

        # 判断原则条数检查
        domain_text = self._layers.get(LAYER_DOMAIN_KNOWLEDGE, "")
        principles = [
            line for line in domain_text.split("\n")
            if line.strip().startswith("- ")
        ]
        if len(principles) > MAX_PRINCIPLES:
            msg = (
                f"判断原则条数 {len(principles)} 超过上限 {MAX_PRINCIPLES}"
            )
            issues.append(msg)

        # 每条原则字符数检查
        for p in principles:
            p_chars = len(p)
            if p_chars > MAX_PRINCIPLE_CHARS:
                msg = (
                    f"原则 '{p[:30]}...' 字符数 {p_chars} 超过上限 "
                    f"{MAX_PRINCIPLE_CHARS}"
                )
                issues.append(msg)

        if issues:
            for msg in issues:
                logger.warning("system prompt constraint violation", detail=msg)
            if self._debug:
                raise ValueError(
                    f"System prompt constraint violations: {'; '.join(issues)}"
                )
