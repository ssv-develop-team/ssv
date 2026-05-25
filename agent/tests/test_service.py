from __future__ import annotations

from ssv_agent.config import SsvConfig
from ssv_agent import service


def test_run_delegates_to_event_consumer(monkeypatch) -> None:
    calls: list[SsvConfig] = []
    monkeypatch.setattr(service, "run_consumer", lambda cfg: calls.append(cfg))
    cfg = SsvConfig()

    service.run(cfg)

    assert calls == [cfg]
