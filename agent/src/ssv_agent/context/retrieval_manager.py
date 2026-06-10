"""检索上下文管理器 —— 规则匹配 + YAML 外部化 + 降级兜底。

设计原则:
  1. 四级处理管线: 精确匹配 → MD5 去重 → source_type 标注 → priority 排序截断
  2. 查找顺序: config/rules.yaml → $SSV_RULES_PATH → 硬编码默认规则
  3. 空结果显式告知 LLM，防止编造规则
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
from typing import Optional

import structlog
import yaml

from ssv_agent.context.pack import RetrievalBlock, RetrievalItem

logger = structlog.get_logger()

# 默认规则文件路径（相对于项目根）
DEFAULT_RULES_PATH = "config/rules.yaml"

# 降级硬编码默认规则（原 ContextBuilder._lookup_rules() 逻辑）
_FALLBACK_RULES: dict[str, list[dict]] = {
    "no_helmet": [
        {
            "content": "安全帽佩戴规范: 进入施工区域必须正确佩戴安全帽",
            "source": "默认安全规则",
            "source_type": "regulation",
            "priority": 1,
        }
    ],
    "low_confidence": [
        {
            "content": "安全帽佩戴规范: 进入施工区域必须正确佩戴安全帽",
            "source": "默认安全规则",
            "source_type": "regulation",
            "priority": 1,
        }
    ],
    "consecutive_hits": [
        {
            "content": "连续违规判定: 同一目标连续3帧以上未佩戴安全帽触发告警",
            "source": "默认安全规则",
            "source_type": "regulation",
            "priority": 2,
        }
    ],
    "rule_conflict": [
        {
            "content": "冲突判定规则: 人员框存在但安全帽框不稳定时需人工复核",
            "source": "默认安全规则",
            "source_type": "expert",
            "priority": 2,
        }
    ],
}


class RetrievalManager:
    """检索上下文管理器 —— 规则匹配 + 降级兜底。

    用法:
        mgr = RetrievalManager()
        block = mgr.search(event, strategy="rule_explain")
    """

    def __init__(self, rules_path: Optional[str] = None) -> None:
        self._rules: list[dict] = []
        self._loaded = False
        self._load_error: str = ""
        self._search_count = 0

        # 按查找顺序加载规则
        self._load_rules(rules_path)

    @property
    def search_count(self) -> int:
        return self._search_count

    @property
    def loaded(self) -> bool:
        return self._loaded

    @property
    def load_error(self) -> str:
        return self._load_error

    # ── 公共接口 ─────────────────────────────────────────────────────────

    def search(self, event, strategy: str = "") -> RetrievalBlock:
        """搜索与当前事件相关的规则/知识。

        Args:
            event: 当前 DetectionEvent。
            strategy: 复核策略名（rule_explain 策略会返回更多结果）。

        Returns:
            RetrievalBlock: 匹配的检索结果。
        """
        self._search_count += 1

        trigger = event.trigger_reason.value if event.trigger_reason else ""

        # 精确匹配: 按 trigger_reason 过滤
        matched = self._match_by_trigger(trigger)

        # MD5 去重（按 content 去重）
        matched = self._deduplicate(matched)

        # priority 排序截断
        matched.sort(key=lambda r: r.priority)
        max_items = 8 if strategy == "rule_explain" else 4
        matched = matched[:max_items]

        # 空结果处理
        if not matched:
            logger.debug(
                "no rules matched, returning empty block",
                trigger=trigger,
                strategy=strategy,
            )

        return RetrievalBlock(items=matched)

    def _match_by_trigger(self, trigger: str) -> list[RetrievalItem]:
        """按触发原因精确匹配规则。"""
        results: list[RetrievalItem] = []
        for rule in self._rules:
            triggers = rule.get("trigger", [])
            if not triggers or trigger in triggers:
                results.append(
                    RetrievalItem(
                        content=rule.get("content", ""),
                        source=rule.get("source", ""),
                        source_type=rule.get("source_type", "regulation"),
                        priority=rule.get("priority", 0),
                    )
                )
        return results

    @staticmethod
    def _deduplicate(items: list[RetrievalItem]) -> list[RetrievalItem]:
        """按 content MD5 去重，保留首次出现。"""
        seen: set[str] = set()
        result: list[RetrievalItem] = []
        for item in items:
            h = hashlib.md5(item.content.encode()).hexdigest()
            if h not in seen:
                seen.add(h)
                result.append(item)
        return result

    # ── 规则加载 ─────────────────────────────────────────────────────────

    def _load_rules(self, rules_path: Optional[str]) -> None:
        """按查找顺序加载规则:
        1. 显式传入路径
        2. $SSV_RULES_PATH 环境变量
        3. config/rules.yaml（默认路径）
        4. 硬编码默认规则（降级）
        """
        # 尝试加载 YAML
        yaml_path = None
        if rules_path:
            yaml_path = Path(rules_path)
        elif env_path := os.environ.get("SSV_RULES_PATH"):
            yaml_path = Path(env_path)
        else:
            yaml_path = Path(DEFAULT_RULES_PATH)

        if yaml_path and yaml_path.exists():
            try:
                with open(yaml_path) as f:
                    data = yaml.safe_load(f) or {}
                self._rules = data.get("rules", [])
                self._loaded = True
                logger.info(
                    "rules loaded from yaml",
                    path=str(yaml_path),
                    rule_count=len(self._rules),
                )
                return
            except Exception as exc:
                self._load_error = f"Failed to load rules from {yaml_path}: {exc}"
                logger.error("rules yaml load failed", path=str(yaml_path), error=str(exc))

        # 降级为硬编码默认规则
        self._loaded = False
        for trigger, rule_list in _FALLBACK_RULES.items():
            for rule in rule_list:
                self._rules.append({
                    "trigger": [trigger],
                    **rule,
                })

        if self._load_error:
            logger.warning(
                "falling back to hardcoded default rules",
                error=self._load_error,
                rule_count=len(self._rules),
            )
        else:
            logger.debug(
                "using hardcoded default rules (no yaml found)",
                rule_count=len(self._rules),
            )
