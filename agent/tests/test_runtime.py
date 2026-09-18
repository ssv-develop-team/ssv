from __future__ import annotations

import json
from pathlib import Path

from ssv_agent.config import SsvConfig
from ssv_agent.runtime import (
    agent_root,
    discover_config_path,
    runtime_environment,
    resolve_agent_path,
    select_qdrant_target,
)


def test_resolve_agent_path_keeps_absolute_paths_and_anchors_relative_paths(
    tmp_path: Path,
) -> None:
    assert resolve_agent_path("data/events.db") == agent_root() / "data/events.db"
    assert resolve_agent_path(tmp_path / "events.db") == tmp_path / "events.db"


def test_runtime_environment_projects_configured_runtime_values() -> None:
    config = SsvConfig.model_validate(
        {
            "agent": {
                "event_db_path": "state/events.db",
                "output_dir": "state/outputs",
                "evidence_roots": ["/var/lib/ssv/evidence"],
                "indexing": {
                    "embedding_backend": "openai_compatible",
                    "embedding_model": "text-embedding-3-small",
                    "embedding_base_url": "https://embedding.example.test/v1",
                    "query_text_type": "query",
                },
                "knowledge": {
                    "backend": "qdrant",
                    "rules_dir": "knowledge/custom-rules",
                    "qdrant_path": "state/qdrant",
                    "qdrant_url": "https://qdrant.example.test",
                    "min_score": 0.7,
                },
            }
        }
    )

    values = runtime_environment(config)

    assert values["SSV_EMBEDDING_BACKEND"] == "openai_compatible"
    assert values["SSV_EMBEDDING_MODEL"] == "text-embedding-3-small"
    assert values["SSV_EMBEDDING_BASE_URL"] == "https://embedding.example.test/v1"
    assert values["SSV_EMBEDDING_QUERY_TEXT_TYPE"] == "query"
    assert values["SSV_EVENT_DB_PATH"] == str((agent_root() / "state/events.db").resolve())
    assert values["SSV_OUTPUTS_DIR"] == str((agent_root() / "state/outputs").resolve())
    assert values["SSV_KNOWLEDGE_BACKEND"] == "qdrant"
    assert values["SSV_KNOWLEDGE_RULES_DIR"] == str(
        (agent_root() / "knowledge/custom-rules").resolve()
    )
    assert values["SSV_QDRANT_PATH"] == str((agent_root() / "state/qdrant").resolve())
    assert values["SSV_QDRANT_URL"] == "https://qdrant.example.test"
    assert values["SSV_KNOWLEDGE_MIN_SCORE"] == "0.7"
    assert json.loads(values["SSV_EVIDENCE_ROOTS"]) == ["/var/lib/ssv/evidence"]


def test_discover_config_path_prefers_environment_path(tmp_path: Path, monkeypatch) -> None:
    environment_path = tmp_path / "environment.yaml"
    environment_path.touch()
    monkeypatch.chdir(tmp_path)
    Path("ssv.yaml").touch()
    monkeypatch.setenv("SSV_CONFIG_PATH", str(environment_path))

    assert discover_config_path() == environment_path


def test_select_qdrant_target_prefers_cli_then_config() -> None:
    config = SsvConfig.model_validate(
        {
            "agent": {
                "knowledge": {
                    "qdrant_path": "from-config",
                    "qdrant_url": "https://qdrant.example.test",
                }
            }
        }
    )

    assert select_qdrant_target(config, Path("from-cli")) == (Path("from-cli"), None)
    assert select_qdrant_target(config, None) == (
        Path("from-config"),
        "https://qdrant.example.test",
    )
    assert select_qdrant_target(None, None) == (Path("data/qdrant"), None)
