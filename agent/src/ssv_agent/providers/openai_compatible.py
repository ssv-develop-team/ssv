"""OpenAI-compatible provider for external LLM/VLM services.

The provider calls a Chat Completions compatible HTTP endpoint, such as
Alibaba Cloud DashScope compatible-mode. Secrets are read from environment
variables only; config files should store variable names and non-secret values.
"""

from __future__ import annotations

import base64
import json
import mimetypes
import os
import time
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from ssv_agent.context.pack import ContextPack
from ssv_agent.models.event import ReviewContext, ReviewStrategy
from ssv_agent.providers.base import BaseProvider


_IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


class OpenAICompatibleProvider(BaseProvider):
    """Call an OpenAI Chat Completions compatible model endpoint."""

    def __init__(
        self,
        base_url: str,
        text_model: str,
        api_key_env: str = "SSV_AGENT_API_KEY",
        vision_model: str = "",
        timeout_seconds: float = 60.0,
        temperature: float = 0.2,
        max_tokens: int = 2048,
        max_images: int = 1,
    ) -> None:
        self._base_url = base_url.rstrip("/")
        self._text_model = text_model
        self._api_key_env = api_key_env
        self._vision_model = vision_model
        self._timeout_seconds = timeout_seconds
        self._temperature = temperature
        self._max_tokens = max_tokens
        self._max_images = max_images
        self._call_count = 0

    @property
    def provider_name(self) -> str:
        return "openai-compatible-provider"

    @property
    def call_count(self) -> int:
        return self._call_count

    def analyze(self, context) -> str:
        """Analyze a T4 context via a Chat Completions compatible API."""
        self._call_count += 1
        if not self._base_url:
            raise RuntimeError("SSV_AGENT_API_BASE_URL is required for openai_compatible provider")
        if not self._text_model:
            raise RuntimeError("SSV_AGENT_TEXT_MODEL is required for openai_compatible provider")

        started = time.time()
        model, messages = self._build_messages(context)
        content, latency_ms = self._chat_completion(model=model, messages=messages, started=started)
        if _is_unhelpful_content(content):
            retry_started = time.time()
            content, retry_latency = self._chat_completion(
                model=model,
                messages=self._fallback_messages(context),
                started=retry_started,
            )
            latency_ms += retry_latency
        return f"{content}\n\n[provider={self.provider_name}, model={model}, latency_ms={latency_ms:.1f}]"

    def _chat_completion(
        self,
        model: str,
        messages: list[dict[str, Any]],
        started: float,
    ) -> tuple[str, float]:
        payload = {
            "model": model,
            "messages": messages,
            "temperature": self._temperature,
            "max_tokens": self._max_tokens,
        }
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = Request(
            f"{self._base_url}/chat/completions",
            data=data,
            headers={
                "Authorization": f"Bearer {self._api_key()}",
                "Content-Type": "application/json",
            },
            method="POST",
        )

        try:
            with urlopen(request, timeout=self._timeout_seconds) as response:
                raw = response.read().decode("utf-8")
        except HTTPError as exc:
            body = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"openai_compatible provider HTTP {exc.code}: {body[:500]}"
            ) from exc
        except URLError as exc:
            raise RuntimeError(f"openai_compatible provider request failed: {exc}") from exc

        content = self._parse_response(raw)
        latency_ms = (time.time() - started) * 1000.0
        return content, latency_ms

    def _api_key(self) -> str:
        key = os.environ.get(self._api_key_env) or os.environ.get("DASHSCOPE_API_KEY")
        if not key:
            raise RuntimeError(
                f"{self._api_key_env} or DASHSCOPE_API_KEY is required for "
                "openai_compatible provider"
            )
        return key

    def _build_messages(self, context) -> tuple[str, list[dict[str, Any]]]:
        system_text, user_text, event, strategy = self._extract_context(context)
        image_paths = self._select_images(event)
        use_vision = (
            self._vision_model
            and image_paths
            and str(strategy) == ReviewStrategy.VISUAL_REVIEW.value
        )
        model = self._vision_model if use_vision else self._text_model

        messages: list[dict[str, Any]] = []
        provider_instruction = (
            "你是 T4 安全帽事件复核模型。请只输出面向安全员的中文复核结论，"
            "不要输出 tool_call、XML、JSON 或函数调用。"
            "如果信息不足，请说明需要人工复核和原因。"
        )
        if system_text:
            messages.append({"role": "system", "content": f"{system_text}\n\n{provider_instruction}"})
        else:
            messages.append({"role": "system", "content": provider_instruction})
        if use_vision:
            content: list[dict[str, Any]] = [{"type": "text", "text": user_text}]
            for path in image_paths[: self._max_images]:
                content.append({"type": "image_url", "image_url": {"url": _image_data_url(path)}})
            messages.append({"role": "user", "content": content})
        else:
            if image_paths:
                user_text = (
                    f"{user_text}\n\n"
                    "关键帧证据路径（当前文本模型不会读取图片本体，仅用于审计引用）:\n"
                    + "\n".join(f"- {path}" for path in image_paths)
                )
            messages.append({"role": "user", "content": user_text})
        return model, messages

    def _fallback_messages(self, context) -> list[dict[str, Any]]:
        _system_text, user_text, event, _strategy = self._extract_context(context)
        image_paths = self._select_images(event)
        if image_paths:
            user_text = (
                f"{user_text}\n\n关键帧证据路径:\n"
                + "\n".join(f"- {path}" for path in image_paths[: self._max_images])
            )
        return [
            {
                "role": "system",
                "content": (
                    "你是施工现场安全帽复核助手。禁止输出 tool_call/XML/JSON。"
                    "请基于事件、检测结果和规则依据，用中文直接给出："
                    "1. 复核结论；2. 风险判断；3. 处置建议。"
                ),
            },
            {"role": "user", "content": user_text},
        ]

    @staticmethod
    def _extract_context(context) -> tuple[str, str, Any, str]:
        if isinstance(context, ContextPack):
            event = context.metadata.get("event")
            strategy = str(context.metadata.get("strategy", ""))
            system_text = context.system_prompt.content
            user_text = context.user_input.event_context
            if context.retrieval_context.items:
                rules = "\n".join(
                    f"- {item.source}: {item.content}"
                    for item in context.retrieval_context.items[:5]
                )
                user_text = f"{user_text}\n\n可用规则依据:\n{rules}"
            return system_text, user_text, event, strategy

        if isinstance(context, ReviewContext):
            return "", _render_review_context(context), context.event, context.strategy.value

        event = getattr(context, "event", None)
        return "", str(context), event, ""

    @staticmethod
    def _select_images(event) -> list[Path]:
        paths: list[Path] = []
        for raw_path in getattr(event, "evidence_paths", []) or []:
            path = Path(raw_path)
            if path.exists() and path.suffix.lower() in _IMAGE_SUFFIXES:
                paths.append(path)
        return paths

    @staticmethod
    def _parse_response(raw: str) -> str:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"openai_compatible provider returned invalid JSON: {raw[:500]}") from exc

        choices = data.get("choices") or []
        if not choices:
            raise RuntimeError(f"openai_compatible provider returned no choices: {raw[:500]}")
        message = choices[0].get("message") or {}
        content = message.get("content", "")
        if isinstance(content, str):
            return content.strip()
        if isinstance(content, list):
            parts = []
            for item in content:
                if isinstance(item, dict):
                    parts.append(str(item.get("text") or item.get("content") or ""))
                else:
                    parts.append(str(item))
            return "\n".join(part for part in parts if part).strip()
        return str(content).strip()


def _image_data_url(path: Path) -> str:
    mime_type, _encoding = mimetypes.guess_type(str(path))
    mime_type = mime_type or "image/jpeg"
    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{mime_type};base64,{encoded}"


def _is_unhelpful_content(content: str) -> bool:
    normalized = content.strip().lower()
    return (
        not normalized
        or "<tool_call>" in normalized
        or "tool_call" in normalized
        or "模型无法生成答案" in content
    )


def _render_review_context(context: ReviewContext) -> str:
    event = context.event
    lines = [
        f"事件来源: {event.source}",
        f"帧编号: {event.frame_id}",
        f"严重程度: {event.severity.value}",
        f"触发原因: {event.trigger_reason.value}",
        f"复核策略: {context.strategy.value}",
        f"检测目标数: {len(event.detections)}",
    ]
    for index, detection in enumerate(event.detections):
        lines.append(
            f"检测[{index}]: class={detection.class_name}, "
            f"conf={detection.confidence:.2f}, "
            f"bbox=[{detection.bbox[0]:.2f},{detection.bbox[1]:.2f},"
            f"{detection.bbox[2]:.2f},{detection.bbox[3]:.2f}], "
            f"track_id={detection.track_id}"
        )
    if context.evidence_summary:
        lines.append(f"证据: {context.evidence_summary}")
    if context.rule_snippets:
        lines.append("相关规则:")
        lines.extend(f"- {snippet}" for snippet in context.rule_snippets)
    return "\n".join(lines)
