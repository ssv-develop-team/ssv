"""Factories for Agent runtime dependencies."""

from __future__ import annotations

import structlog

from ssv_agent.config import SsvConfig
from ssv_agent.providers.base import BaseProvider
from ssv_agent.providers.local_yolo import LocalYoloProvider
from ssv_agent.providers.mock import MockProvider
from ssv_agent.providers.openai_compatible import OpenAICompatibleProvider
from ssv_agent.tools.builtin import NotificationDraftTool, RuleRetrievalTool
from ssv_agent.tools.router import ToolRouter

logger = structlog.get_logger()


def build_tool_router(config: SsvConfig) -> ToolRouter:
    """Build the default T4 tool router."""
    router = ToolRouter()
    router.register(RuleRetrievalTool(rules_path=config.agent.rules_yaml_path))
    router.register(NotificationDraftTool())
    return router


def build_provider(config: SsvConfig, mock_provider: bool) -> BaseProvider | None:
    """Build the configured T4 provider.

    Returning ``None`` is intentional for unknown provider types: the state
    machine will keep the event observable and degrade to human review.
    """
    if mock_provider:
        return MockProvider()
    if config.agent.provider_type == "local_yolo":
        return LocalYoloProvider(
            model_path=config.agent.local_yolo_model_path,
            confidence_threshold=config.agent.local_yolo_confidence_threshold,
            device=config.agent.local_yolo_device,
        )
    if config.agent.provider_type == "openai_compatible":
        return OpenAICompatibleProvider(
            base_url=config.agent.openai_api_base_url,
            api_key_env=config.agent.openai_api_key_env,
            text_model=config.agent.openai_text_model,
            vision_model=config.agent.openai_vision_model,
            timeout_seconds=config.agent.openai_timeout_seconds,
            temperature=config.agent.openai_temperature,
            max_tokens=config.agent.openai_max_tokens,
        )
    logger.warning(
        "unknown agent provider type; provider disabled",
        provider_type=config.agent.provider_type,
    )
    return None
