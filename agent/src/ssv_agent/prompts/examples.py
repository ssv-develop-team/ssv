"""Few-shot 示例库 —— 收集典型场景的（输入→输出）对，注入提示词提升推理质量。

设计原则:
  1. 按策略维度组织示例，不同策略使用不同的示例集
  2. 每个示例包含: 场景描述、输入上下文、期望输出
  3. 支持示例数量控制（token 预算约束）
  4. 示例来源于真实案例积累或人工构造

TODO: 当前示例为人工构造的典型场景，后续应:
  - 从真实复核结果中筛选高质量案例
  - 支持按相似度检索最相关示例（接入向量检索后）
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class FewShotExample:
    """单条 few-shot 示例。

    示例:
        example = FewShotExample(
            strategy="direct_confirm",
            scenario="高置信度单人未佩戴安全帽",
            input_context="检测到 1 个 head (conf=0.95), 0 个 helmet",
            expected_output='{"conclusion": "确认未佩戴安全帽", "is_violation": true, ...}',
            tags=["high_confidence", "single_person"],
        )
    """

    strategy: str                        # 适用策略
    scenario: str                        # 场景描述（中文）
    input_context: str                   # 模拟输入上下文
    expected_output: str                 # 期望输出（JSON 字符串）
    tags: list[str] = field(default_factory=list)   # 标签（用于检索和筛选）
    quality_score: float = 1.0          # 示例质量评分 [0, 1]
    source: str = "manual"              # 来源: manual / real_case / curated

    def render_for_prompt(self) -> str:
        """将该示例渲染为提示词中的 assistant/user 消息对。"""
        return f"""## 示例场景: {self.scenario}
输入: {self.input_context}
输出: {self.expected_output}"""


@dataclass
class ExampleBank:
    """示例库 —— 管理一条策略的所有 few-shot 示例。"""

    strategy: str
    examples: list[FewShotExample] = field(default_factory=list)

    def select_by_tags(self, tags: list[str], max_count: int = 3) -> list[FewShotExample]:
        """按标签筛选最相关的示例。"""
        scored = []
        for ex in self.examples:
            score = len(set(ex.tags) & set(tags))
            if score > 0:
                scored.append((score, ex))
        scored.sort(key=lambda x: x[0], reverse=True)
        return [ex for _, ex in scored[:max_count]]

    def top_quality(self, max_count: int = 3) -> list[FewShotExample]:
        """取质量最高的 N 条示例。"""
        sorted_examples = sorted(self.examples, key=lambda e: e.quality_score, reverse=True)
        return sorted_examples[:max_count]


# ── 预置 Few-shot 示例 ───────────────────────────────────────────────────

DIRECT_CONFIRM_EXAMPLES: list[FewShotExample] = [
    FewShotExample(
        strategy="direct_confirm",
        scenario="单人高置信度未佩戴安全帽",
        input_context=(
            "事件: source=pipeline-0, frame_id=42\n"
            "检测: head conf=0.95, bbox=[0.3,0.2,0.5,0.7], track_id=3\n"
            "无 helmet 检测\n"
            "严重程度: HIGH, 触发原因: no_helmet"
        ),
        expected_output=(
            '{"conclusion": "确认未佩戴安全帽", '
            '"is_violation": true, '
            '"confidence": 0.93, '
            '"reasoning": "head 置信度 0.95 且跟踪 ID=3 表明检测稳定，无 helmet 检测确认违规", '
            '"needs_human": false, '
            '"key_evidence": ["head conf=0.95", "track_id=3 稳定跟踪", "无安全帽检测"]}'
        ),
        tags=["high_confidence", "single_person", "no_helmet"],
        quality_score=0.9,
    ),
    FewShotExample(
        strategy="direct_confirm",
        scenario="多人中一人未佩戴",
        input_context=(
            "事件: source=pipeline-0, frame_id=100\n"
            "检测: head conf=0.88 bbox=[0.1,0.1,0.3,0.5] track_id=1, "
            "helmet conf=0.92 bbox=[0.5,0.2,0.7,0.6] track_id=2\n"
            "严重程度: MEDIUM, 触发原因: no_helmet"
        ),
        expected_output=(
            '{"conclusion": "确认存在未佩戴安全帽人员（track_id=1）", '
            '"is_violation": true, '
            '"confidence": 0.85, '
            '"reasoning": "track_id=1 仅有 head 无 helmet，与 track_id=2 有 helmet 对比明显", '
            '"needs_human": false, '
            '"key_evidence": ["track_id=1 head conf=0.88", "track_id=2 helmet conf=0.92 对比"]}'
        ),
        tags=["high_confidence", "multi_person", "no_helmet"],
        quality_score=0.85,
    ),
]

VISUAL_REVIEW_EXAMPLES: list[FewShotExample] = [
    FewShotExample(
        strategy="visual_review",
        scenario="低置信度单人检测，可能是光照不足",
        input_context=(
            "事件: source=pipeline-0, frame_id=55\n"
            "检测: head conf=0.52, bbox=[0.4,0.1,0.6,0.5], track_id=-1\n"
            "无 helmet 检测\n"
            "严重程度: LOW, 触发原因: low_confidence\n"
            "证据: 关键帧路径 /data/evidence/frame_55.jpg"
        ),
        expected_output=(
            '{"conclusion": "低置信度检测，建议人工复核", '
            '"is_violation": false, '
            '"confidence": 0.35, '
            '"reasoning": "head conf 仅 0.52 且无跟踪 ID，可能是误检或光照干扰", '
            '"needs_human": true, '
            '"key_evidence": ["head conf=0.52 < 阈值", "无 track_id", "isolated 检测"]}'
        ),
        tags=["low_confidence", "single_person", "needs_human"],
        quality_score=0.8,
    ),
]

RULE_EXPLAIN_EXAMPLES: list[FewShotExample] = [
    FewShotExample(
        strategy="rule_explain",
        scenario="head 和 helmet 同时存在但可能误分类",
        input_context=(
            "事件: source=pipeline-0, frame_id=78\n"
            "检测: head conf=0.72 bbox=[0.2,0.1,0.4,0.5] track_id=5, "
            "helmet conf=0.61 bbox=[0.22,0.12,0.42,0.48] track_id=5\n"
            "严重程度: LOW, 触发原因: rule_conflict\n"
            "适用规则: 冲突判定规则"
        ),
        expected_output=(
            '{"conclusion": "同一目标同时检测到 head 和 helmet，倾向于判定为已佩戴", '
            '"is_violation": false, '
            '"confidence": 0.55, '
            '"reasoning": "两个检测框重叠且同一 track_id，helmet 置信度较低但存在，可能是安全帽部分可见", '
            '"needs_human": false, '
            '"key_evidence": ["head+helmet 同一 track_id=5", "bbox 高度重叠", "helmet conf=0.61 偏低"]}'
        ),
        tags=["rule_conflict", "overlapping_boxes", "same_track"],
        quality_score=0.75,
    ),
]

NOTIFY_REPORT_EXAMPLES: list[FewShotExample] = [
    FewShotExample(
        strategy="notify_report",
        scenario="连续多帧严重违规",
        input_context=(
            "事件: source=pipeline-0, frame_id=200\n"
            "检测: head conf=0.96 bbox=[0.3,0.2,0.5,0.7] track_id=7\n"
            "连续帧数: 15\n"
            "严重程度: CRITICAL, 触发原因: consecutive_hits\n"
            "无 helmet 检测"
        ),
        expected_output=(
            '{"conclusion": "严重违规: track_id=7 连续 15 帧未佩戴安全帽，需立即通知现场安全员", '
            '"is_violation": true, '
            '"confidence": 0.97, '
            '"reasoning": "高置信度 0.96 + 连续 15 帧 + 稳定跟踪 ID=7，排除误检可能", '
            '"needs_human": false, '
            '"key_evidence": ["head conf=0.96", "连续 15 帧", "track_id=7 稳定"]}'
        ),
        tags=["critical", "consecutive_hits", "notify"],
        quality_score=0.9,
    ),
]
