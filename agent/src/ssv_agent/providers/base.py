"""Provider 抽象基类 —— 屏蔽具体模型厂商和部署形态。

设计上不把业务流程绑定到单一 provider，便于后续切换模型。
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field


@dataclass
class ProviderResult:
    """Provider 分析结果。"""

    conclusion: str = ""           # 复核结论
    confidence: float = 0.0       # provider 自身置信度
    model_used: str = ""          # 使用的模型名称
    latency_ms: float = 0.0       # 调用耗时（毫秒）


@dataclass
class ReviewConclusion:
    """视觉复核结论文本。"""

    is_helmet_worn: bool = False       # 是否佩戴安全帽
    explanation: str = ""              # 复核解释
    confidence: float = 0.0            # 置信度
    key_observations: list[str] = field(default_factory=list)


class BaseProvider(ABC):
    """文本模型 provider 抽象。

    接收 ReviewContext，返回分析结论文本。
    """

    @property
    @abstractmethod
    def provider_name(self) -> str:
        """Provider 标识名。"""
        ...

    @abstractmethod
    def analyze(self, context) -> str:
        """分析复核上下文，返回结论文本。

        Args:
            context: ReviewContext，包含事件信息、证据摘要、规则片段等。

        Returns:
            str: 分析结论文本。
        """
        ...


class BaseVLMProvider(ABC):
    """多模态视觉语言模型 provider 抽象。

    接收关键帧图像路径和事件上下文，返回视觉复核结论。
    """

    @property
    @abstractmethod
    def provider_name(self) -> str:
        """Provider 标识名。"""
        ...

    @abstractmethod
    def review_keyframe(
        self, image_path: str, context
    ) -> ReviewConclusion:
        """对关键帧进行视觉复核。

        Args:
            image_path: 关键帧文件路径。
            context: ReviewContext，包含事件元数据。

        Returns:
            ReviewConclusion: 复核结论。
        """
        ...
