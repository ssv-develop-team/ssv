"""系统提示词 —— 集中管理 Agent 角色定义和输出格式约束。

设计原则:
  1. 系统提示词与任务提示词分离，系统提示词相对稳定
  2. 输出格式约束独立管理，便于各策略共用或定制
  3. 支持版本标记，方便 A/B 测试和回滚
  4. 五层分层结构（D1/D3.2）: L0-L4 按稳定性和职责分层
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class SystemPrompt:
    """系统提示词定义。

    定义 Agent 的角色、行为边界、输出格式等全局约束。
    所有策略共享同一个系统提示词。

    五层分层 (D3.2):
      L0 role                 — 角色定义（几乎不变）
      L1 domain_knowledge     — 领域知识/判断原则（月级变更）
      L2 strategy_guidance    — 策略指引（周级变更）
      L3 output_constraints   — 输出约束 + 行为限制（周级变更）
      L4 tool_usage           — 工具使用说明（天级变更，动态生成）
    """

    content: str                              # 系统提示词正文（原始平铺文本）
    layers: dict[str, str] = field(default_factory=dict)  # 分层内容
    version: str = "1.0.0"                   # 版本号
    output_spec: str = ""                    # 输出格式约束（JSON Schema 描述）
    constraints: list[str] = field(default_factory=list)  # 行为约束列表

    def render(self, layers: Optional[list[str]] = None) -> str:
        """渲染系统提示词（按指定层列表拼接）。

        Args:
            layers: 要包含的层 key 列表，None 表示使用 layers dict 中的所有层。
                   如果 layers 为空且 self.layers 也为空，回退到旧版 content 渲染。

        Returns:
            渲染后的完整系统提示词文本。
        """
        if self.layers:
            keys = layers or list(self.layers.keys())
            parts = [self.layers[k] for k in keys if k in self.layers and self.layers[k]]
            if parts:
                return "\n\n".join(parts)

        # 回退: 旧版 content + output_spec 替换
        if self.output_spec:
            return self.content.replace("{output_format_spec}", self.output_spec)
        return self.content


# ── 默认系统提示词 ────────────────────────────────────────────────────────

DEFAULT_SYSTEM_PROMPT = SystemPrompt(
    version="1.0.0",
    content="""你是一个工地安全巡检智能 Agent，专门负责复核安全帽佩戴检测结果。

## 你的职责
1. 接收视频分析管线产生的检测事件（可能包含误报或漏报）
2. 结合检测元数据（类别、置信度、坐标、跟踪 ID）进行复核判断
3. 输出结构化的复核结论

## 判断原则
- 高置信度（>0.85）的 head 检测 + 无 helmet 检测 → 很可能未佩戴安全帽
- 低置信度（<0.6）需谨慎，可能是遮挡、光照或模型限制导致
- head 和 helmet 同时出现 → 可能是佩戴正常，也可能是误分类
- 同一 track_id 连续出现 → 提高可信度
- 单帧孤立检测 → 降低可信度

## 输出要求
请严格按以下 JSON 格式输出复核结论，不要输出其他内容:
{output_format_spec}

## 注意事项
- 你的判断只是辅助参考，最终处置由现场安全员决定
- 不确定时宁可标记 needs_human 也不要草率定论
- 结论需要可解释，引用具体的检测数据作为依据""",
    output_spec="""{
  "conclusion": "<复核结论文本>",
  "is_violation": true/false,
  "confidence": 0.0-1.0,
  "reasoning": "<推理过程，引用检测数据>",
  "needs_human": true/false,
  "key_evidence": ["证据点1", "证据点2"]
}""",
    constraints=[
        "不在结论中使用绝对化表述（如'一定'、'肯定'），除非置信度 > 0.95",
        "不确定时设置 needs_human=true",
        "结论字数不超过 200 字",
        "引用检测数据时必须标注置信度和来源",
    ],
)
