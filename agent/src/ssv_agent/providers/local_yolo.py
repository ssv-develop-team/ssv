"""本地 YOLO provider —— 用安全帽检测模型对关键帧做异步复核。

该 provider 只在 Agent 消费事件后的复核阶段读取证据图片，不进入每帧同步检测链路。
依赖 ultralytics/torch，未安装时抛出清晰错误，由状态机降级到人工复核。
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from ssv_agent.context.pack import ContextPack
from ssv_agent.models.event import ReviewContext
from ssv_agent.providers.base import BaseProvider, BaseVLMProvider, ReviewConclusion


_IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


class LocalYoloProvider(BaseProvider, BaseVLMProvider):
    """使用本地 Ultralytics YOLO 权重对证据关键帧进行复核。"""

    def __init__(
        self,
        model_path: str,
        confidence_threshold: float = 0.25,
        device: str = "cpu",
        helmet_class_names: tuple[str, ...] = ("helmet", "hardhat", "safety_helmet"),
        head_class_names: tuple[str, ...] = ("head", "person", "no_helmet"),
    ) -> None:
        self._model_path = model_path
        self._confidence_threshold = confidence_threshold
        self._device = device
        self._helmet_classes = {name.lower() for name in helmet_class_names}
        self._head_classes = {name.lower() for name in head_class_names}
        self._model: Any = None
        self._call_count = 0

    @property
    def provider_name(self) -> str:
        return "local-yolo-provider"

    @property
    def call_count(self) -> int:
        return self._call_count

    def analyze(self, context) -> str:
        """从上下文中找到关键帧路径并返回中文复核结论。"""
        event = self._extract_event(context)
        image_path = self._select_keyframe(event)
        conclusion = self.review_keyframe(str(image_path), context)
        worn = "已佩戴安全帽" if conclusion.is_helmet_worn else "未确认佩戴安全帽"
        observations = "；".join(conclusion.key_observations)
        return (
            f"本地 YOLO 复核: {worn}，置信度={conclusion.confidence:.2f}。"
            f"{conclusion.explanation}"
            f"{' 关键观察: ' + observations if observations else ''}"
        )

    def review_keyframe(self, image_path: str, context) -> ReviewConclusion:
        """对单张关键帧图片执行 YOLO 推理并归纳复核结论。"""
        self._call_count += 1
        path = Path(image_path)
        if not path.exists():
            raise FileNotFoundError(f"keyframe not found: {path}")
        if path.suffix.lower() not in _IMAGE_SUFFIXES:
            raise ValueError(f"unsupported keyframe image type: {path}")

        model = self._ensure_model()
        started = time.time()
        results = model(
            str(path),
            conf=self._confidence_threshold,
            device=self._device,
            verbose=False,
        )
        latency_ms = (time.time() - started) * 1000.0

        detections = self._parse_results(results)
        helmet_hits = [d for d in detections if d["name"] in self._helmet_classes]
        head_hits = [d for d in detections if d["name"] in self._head_classes]
        best_helmet = max((d["confidence"] for d in helmet_hits), default=0.0)
        best_head = max((d["confidence"] for d in head_hits), default=0.0)

        if helmet_hits:
            explanation = f"关键帧中检测到 {len(helmet_hits)} 个 helmet 目标"
            confidence = best_helmet
            is_helmet_worn = True
        elif head_hits:
            explanation = f"关键帧中检测到 {len(head_hits)} 个 head/person 目标，但未检测到 helmet"
            confidence = best_head
            is_helmet_worn = False
        else:
            raise RuntimeError(
                "local YOLO found no helmet/head targets in keyframe; "
                f"latency_ms={latency_ms:.1f}"
            )

        observations = [
            f"{d['name']} conf={d['confidence']:.2f}"
            for d in sorted(detections, key=lambda item: item["confidence"], reverse=True)[:5]
        ]
        observations.append(f"latency_ms={latency_ms:.1f}")

        return ReviewConclusion(
            is_helmet_worn=is_helmet_worn,
            explanation=explanation,
            confidence=confidence,
            key_observations=observations,
        )

    def _ensure_model(self):
        if self._model is not None:
            return self._model
        model_path = Path(self._model_path)
        if not model_path.exists():
            raise FileNotFoundError(f"local YOLO model not found: {model_path}")
        try:
            from ultralytics import YOLO
        except Exception as exc:
            raise RuntimeError(
                "ultralytics is required for local_yolo provider; "
                "install the agent vision extra before running with --no-mock"
            ) from exc
        self._model = YOLO(str(model_path))
        return self._model

    @staticmethod
    def _extract_event(context):
        if isinstance(context, ContextPack):
            return context.metadata.get("event")
        if isinstance(context, ReviewContext):
            return context.event
        return getattr(context, "event", None)

    @staticmethod
    def _select_keyframe(event) -> Path:
        if event is None:
            raise ValueError("event is required for local YOLO review")
        for raw_path in getattr(event, "evidence_paths", []) or []:
            path = Path(raw_path)
            if path.suffix.lower() in _IMAGE_SUFFIXES:
                return path
        raise ValueError("no keyframe evidence path available in event.evidence_paths")

    @staticmethod
    def _parse_results(results) -> list[dict[str, Any]]:
        if not results:
            return []
        result = results[0]
        names = getattr(result, "names", None) or {}
        detections: list[dict[str, Any]] = []
        boxes = getattr(result, "boxes", None)
        if boxes is None:
            return detections
        for box in boxes:
            cls_id = int(_scalar(getattr(box, "cls", -1)))
            confidence = float(_scalar(getattr(box, "conf", 0.0)))
            name = str(names.get(cls_id, cls_id)).lower()
            detections.append(
                {
                    "class_id": cls_id,
                    "name": name,
                    "confidence": confidence,
                }
            )
        return detections


def _scalar(value) -> float:
    """Convert torch/numpy/list scalar-like values to a Python float."""
    if hasattr(value, "item"):
        return value.item()
    if isinstance(value, (list, tuple)) and value:
        return _scalar(value[0])
    return value
