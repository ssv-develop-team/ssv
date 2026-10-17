from __future__ import annotations

import os
import json
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path
from urllib.error import URLError
from urllib.request import urlopen

import yaml
from pydantic import ValidationError

from ssv_agent.config import AgentWebConfig
from ssv_agent.event_store.sqlite_store import SsvEventStore

AGENT_ROOT = Path(__file__).resolve().parents[1]


def test_web_config_defaults_to_loopback_and_validates_port() -> None:
    config = AgentWebConfig()

    assert config.host == "127.0.0.1"
    assert config.port == 8765
    try:
        AgentWebConfig(port=65536)
    except ValidationError:
        pass
    else:
        raise AssertionError("invalid web port was accepted")


def test_web_command_starts_independent_http_service(tmp_path: Path) -> None:
    evidence_root = tmp_path / "evidence"
    evidence_root.mkdir()
    database_path = tmp_path / "events.db"
    store = SsvEventStore(database_path)
    store.close()

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]

    config_path = tmp_path / "ssv.yaml"
    config_path.write_text(
        yaml.safe_dump(
            {
                "version": "2.0",
                "agent": {
                    "event_db_path": str(database_path),
                    "evidence_roots": [str(evidence_root)],
                    "web": {"host": "127.0.0.1", "port": port},
                },
                "evidence_cache": {"directory": str(evidence_root / "cache")},
            }
        ),
        encoding="utf-8",
    )
    environment = os.environ.copy()
    source_path = str(AGENT_ROOT / "src")
    environment["PYTHONPATH"] = os.pathsep.join(
        part for part in (source_path, environment.get("PYTHONPATH")) if part
    )
    executable = shutil.which("ssv-agent")
    command = (
        [executable, "web", "--config", str(config_path)]
        if executable is not None
        else [sys.executable, "-m", "ssv_agent", "web", "--config", str(config_path)]
    )
    process = subprocess.Popen(
        command,
        cwd=AGENT_ROOT,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        url = f"http://127.0.0.1:{port}/api/v1/overview"
        deadline = time.monotonic() + 10
        response_body: bytes | None = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError("ssv-agent web exited before serving requests")
            try:
                with urlopen(url, timeout=0.5) as response:
                    response_body = response.read()
                break
            except URLError:
                time.sleep(0.05)

        assert response_body is not None, "ssv-agent web did not become ready"
        assert json.loads(response_body)["total_events"] == 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
