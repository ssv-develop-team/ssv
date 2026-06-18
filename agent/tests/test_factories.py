from ssv_agent.config import SsvConfig
from ssv_agent.factories import build_provider, build_tool_router
from ssv_agent.providers.local_yolo import LocalYoloProvider
from ssv_agent.providers.mock import MockProvider
from ssv_agent.providers.openai_compatible import OpenAICompatibleProvider


def test_build_tool_router_registers_default_tools() -> None:
    router = build_tool_router(SsvConfig())

    assert set(router.registered_tools) == {
        "rule_retrieval",
        "generate_notification",
    }


def test_build_provider_returns_mock_when_enabled() -> None:
    provider = build_provider(SsvConfig(), mock_provider=True)

    assert isinstance(provider, MockProvider)


def test_build_provider_returns_local_yolo() -> None:
    cfg = SsvConfig()
    cfg.agent.provider_type = "local_yolo"
    cfg.agent.local_yolo_model_path = "/tmp/model.pt"
    cfg.agent.local_yolo_device = "cpu"

    provider = build_provider(cfg, mock_provider=False)

    assert isinstance(provider, LocalYoloProvider)


def test_build_provider_returns_openai_compatible() -> None:
    cfg = SsvConfig()
    cfg.agent.provider_type = "openai_compatible"
    cfg.agent.openai_api_base_url = "https://example.test/compatible-mode/v1"
    cfg.agent.openai_text_model = "qwen3.7-plus"

    provider = build_provider(cfg, mock_provider=False)

    assert isinstance(provider, OpenAICompatibleProvider)


def test_build_provider_returns_none_for_unknown_provider() -> None:
    cfg = SsvConfig()
    cfg.agent.provider_type = "missing-provider"

    provider = build_provider(cfg, mock_provider=False)

    assert provider is None
