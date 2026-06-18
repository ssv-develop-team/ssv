import json
from pathlib import Path

from ssv_agent.demos.report import generate_report, openai_ready


def test_openai_ready_rejects_placeholders(tmp_path: Path) -> None:
    env_file = tmp_path / ".env"
    env_file.write_text(
        "\n".join(
            [
                "SSV_AGENT_API_BASE_URL=https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1",
                "SSV_AGENT_API_KEY=请填写真实API_KEY",
                "SSV_AGENT_TEXT_MODEL=qwen3.7-plus",
            ]
        ),
        encoding="utf-8",
    )

    assert openai_ready(env_file) is False


def test_openai_ready_accepts_complete_config(tmp_path: Path) -> None:
    env_file = tmp_path / ".env"
    env_file.write_text(
        "\n".join(
            [
                "SSV_AGENT_API_BASE_URL=https://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1",
                "SSV_AGENT_API_KEY=secret-from-local-env",
                "SSV_AGENT_TEXT_MODEL=qwen3.7-plus",
            ]
        ),
        encoding="utf-8",
    )

    assert openai_ready(env_file) is True


def test_generate_report_writes_summary_and_html(tmp_path: Path) -> None:
    case_dir = tmp_path / "local_yolo"
    case_dir.mkdir(parents=True)
    (case_dir / "event.json").write_text(
        json.dumps(
            {
                "frame_id": 75,
                "detections": [
                    {"class": "helmet"},
                    {"class": "head"},
                ],
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )
    (case_dir / "review.json").write_text(
        json.dumps(
            {
                "final_state": "completed",
                "strategy": "rule_explain",
                "provider_used": "local-yolo-provider",
                "tool_results": [
                    {"tool_name": "rule_retrieval", "success": True, "output": "规则"},
                    {"tool_name": "rule_explain", "success": True, "output": "复核通过"},
                ],
            },
            ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    text = generate_report(tmp_path)

    assert "真实视频 + 本地 YOLO" in text
    assert "local-yolo-provider" in (tmp_path / "summary.md").read_text(encoding="utf-8")
    assert "复核通过" in (tmp_path / "report.html").read_text(encoding="utf-8")
