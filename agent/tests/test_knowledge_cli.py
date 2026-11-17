from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

import ssv_agent.knowledge.base as base_module
from ssv_agent import knowledge_cli
from ssv_agent.embedding.provider import EmbeddingProvider


class CliEmbedding(EmbeddingProvider):
    backend_name = "test"

    async def embed_texts(self, texts: list[str]) -> list[list[float]]:
        return [[1.0, 0.0] for _ in texts]

    async def embed_query(self, query: str) -> list[float]:
        return [1.0, 0.0]


def test_cli_add_file_stdin_search_and_duplicate(tmp_path: Path, monkeypatch, capsys) -> None:
    monkeypatch.setattr(base_module, "get_configured_provider", lambda *args: CliEmbedding())
    prefix = ["--qdrant-path", str(tmp_path / "db"), "--embedding-backend", "bge_m3"]
    monkeypatch.setattr("sys.stdin", io.StringIO("一句话知识"))
    assert knowledge_cli.main([*prefix, "add-text"]) == 0
    first = json.loads(capsys.readouterr().out)
    assert first["chunks_count"] == 1 and not first["skipped"]
    assert knowledge_cli.main([*prefix, "add-text", "一句话知识"]) == 0
    assert json.loads(capsys.readouterr().out)["skipped"]
    file = tmp_path / "notes.md"
    file.write_text("# 新知识\n测试。", encoding="utf-8")
    assert knowledge_cli.main([*prefix, "add-file", str(file), "--chunk-strategy", "text"]) == 0
    assert json.loads(capsys.readouterr().out)["success"]
    assert (
        knowledge_cli.main([*prefix, "add-file", str(file), "--chunk-strategy", "structured"]) == 0
    )
    structured = json.loads(capsys.readouterr().out)
    assert structured["success"] and not structured["skipped"]
    assert (
        knowledge_cli.main([*prefix, "search", "知识", "--document-id", first["document_id"]]) == 0
    )
    result = json.loads(capsys.readouterr().out)
    assert result["success"] and len(result["chunks"]) == 1
    assert result["chunks"][0]["content"] == "一句话知识"
    assert result["chunks"][0]["line_start"] == 1


@pytest.mark.parametrize(
    "command",
    [
        ["add-text", ""],
        ["add-text", "text", "--chunk-size", "0"],
        ["search", "query", "--min-score", "nan"],
        ["add-file", "not-supported.pdf"],
    ],
)
def test_cli_errors_are_json_with_nonzero_exit(
    tmp_path: Path, monkeypatch, capsys, command
) -> None:
    monkeypatch.setattr(base_module, "get_configured_provider", lambda *args: CliEmbedding())
    assert (
        knowledge_cli.main(
            ["--qdrant-path", str(tmp_path), "--embedding-backend", "bge_m3", *command]
        )
        == 1
    )
    result = json.loads(capsys.readouterr().out)
    assert not result["success"] and result["stage"] == "input"


def test_cli_explicit_env_and_parameter_precedence(tmp_path: Path, monkeypatch, capsys) -> None:
    environment = tmp_path / "config.env"
    environment.write_text("SSV_EMBEDDING_BACKEND=mock\n", encoding="utf-8")
    monkeypatch.delenv("SSV_EMBEDDING_BACKEND", raising=False)
    monkeypatch.setattr(base_module, "get_configured_provider", lambda *args: CliEmbedding())
    assert (
        knowledge_cli.main(
            [
                "--env-file",
                str(environment),
                "--qdrant-path",
                str(tmp_path / "db"),
                "--embedding-backend",
                "bge_m3",
                "add-text",
                "text",
            ]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["success"]
    assert knowledge_cli.main(["--env-file", str(tmp_path / "missing.env"), "search", "text"]) == 1
    assert json.loads(capsys.readouterr().out)["stage"] == "configuration"


def test_cli_processes_use_openai_adapter_and_persist_between_runs(tmp_path: Path) -> None:
    """真实 CLI 进程与 HTTP 适配器测试；服务向量为测试数据，不是语义模型。"""
    requests: list[list[str]] = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(body["input"])
            data = [
                {
                    "object": "embedding",
                    "index": index,
                    "embedding": [1.0, 0.0] if "电缆" in text else [0.0, 1.0],
                }
                for index, text in enumerate(body["input"])
            ]
            response = json.dumps({"object": "list", "data": data, "model": body["model"]})
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response.encode())))
            self.end_headers()
            self.wfile.write(response.encode())

        def log_message(self, format, *args) -> None:
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = {
        **os.environ,
        "PYTHONPATH": str(Path(__file__).resolve().parents[1] / "src"),
        "PYTHONUTF8": "1",
        "SSV_EMBEDDING_BACKEND": "openai_compatible",
        "SSV_EMBEDDING_MODEL": "http-test-v1",
        "SSV_EMBEDDING_BASE_URL": f"http://127.0.0.1:{server.server_port}/v1",
        "SSV_EMBEDDING_API_KEY": "test-key",
    }

    def run(*args: str) -> dict:
        process = subprocess.run(
            [
                sys.executable,
                "-m",
                "ssv_agent.knowledge_cli",
                "--qdrant-path",
                str(tmp_path / "db"),
                *args,
            ],
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=30,
            check=False,
        )
        assert process.returncode == 0, process.stdout + process.stderr
        return json.loads(process.stdout)

    try:
        first = run("add-text", "电缆每周检查。", "--source", "随手记录")
        file = tmp_path / "notes.md"
        file.write_text("# 联系方式\n服务台在一楼。", encoding="utf-8")
        run("add-file", str(file))
        assert run("add-text", "电缆每周检查。", "--source", "随手记录")["skipped"]
        result = run("search", "电缆怎么检查", "--min-score", "0.8")
        assert len(result["chunks"]) == 1
        assert result["chunks"][0]["document_id"] == first["document_id"]
        assert result["chunks"][0]["content"] == "电缆每周检查。"
        assert len(requests) == 3  # 两次入库和一次查询，重复入库没有 HTTP 请求。
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
