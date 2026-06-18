"""T4 内置工具 —— 规则检索和通知文本生成。"""

from __future__ import annotations

from pathlib import Path

import yaml

from ssv_agent.models.event import ToolResult
from ssv_agent.tools.base import BaseTool


class RuleRetrievalTool(BaseTool):
    """按触发原因从 YAML 规则库检索安全规则。"""

    def __init__(self, rules_path: str = "config/rules.yaml") -> None:
        self._rules_path = rules_path

    @property
    def name(self) -> str:
        return "rule_retrieval"

    @property
    def description(self) -> str:
        return "输入事件触发原因，返回匹配的安全帽复核规则。"

    @property
    def parameters_schema(self) -> dict:
        return {
            "type": "object",
            "properties": {
                "event_type": {
                    "type": "string",
                    "enum": [
                        "no_helmet",
                        "low_confidence",
                        "consecutive_hits",
                        "rule_conflict",
                        "manual",
                    ],
                }
            },
            "required": ["event_type"],
        }

    @property
    def returns_description(self) -> str:
        return "返回匹配规则文本；无匹配时返回说明文本，不视为工具失败。"

    @property
    def side_effects(self) -> str:
        return ""

    def execute(self, params: dict) -> ToolResult:
        event_type = str(params.get("event_type", ""))
        rules = self._load_rules()
        matched = [
            rule
            for rule in rules
            if not rule.get("trigger") or event_type in rule.get("trigger", [])
        ]
        if not matched:
            return ToolResult(
                tool_name=self.name,
                success=True,
                output=f"未匹配到 event_type={event_type} 的规则",
            )
        lines = [
            f"{rule.get('source', '规则库')}: {rule.get('content', '').strip()}"
            for rule in matched[:5]
        ]
        return ToolResult(tool_name=self.name, success=True, output="\n".join(lines))

    def _load_rules(self) -> list[dict]:
        path = Path(self._rules_path)
        if not path.exists():
            return []
        with open(path) as f:
            data = yaml.safe_load(f) or {}
        return data.get("rules", [])


class NotificationDraftTool(BaseTool):
    """生成现场通知文本草稿，不发送外部通知。"""

    @property
    def name(self) -> str:
        return "generate_notification"

    @property
    def description(self) -> str:
        return "输入事件严重程度、来源和摘要，生成现场安全员通知草稿。"

    @property
    def parameters_schema(self) -> dict:
        return {
            "type": "object",
            "properties": {
                "severity": {"type": "string"},
                "source": {"type": "string"},
                "summary": {"type": "array"},
            },
            "required": ["severity", "source"],
        }

    @property
    def returns_description(self) -> str:
        return "返回一段通知文本草稿；不会调用短信、工单或其他外部系统。"

    @property
    def side_effects(self) -> str:
        return ""

    def execute(self, params: dict) -> ToolResult:
        severity = params.get("severity", "unknown")
        source = params.get("source", "unknown")
        return ToolResult(
            tool_name=self.name,
            success=True,
            output=(
                f"安全告警草稿: 来源 {source} 发生 {severity} 级别安全帽复核事件，"
                "请现场安全员核查关键帧证据并确认处置。"
            ),
        )
