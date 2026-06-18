"""T4 demo runner: Redis event -> Agent state machine -> review result.

The demo is intentionally scoped to T4. It does not depend on the live
GStreamer pipeline; instead it injects representative Redis events and
verifies that the Agent consumes, reviews, and writes back results.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from redis import Redis

from ssv_agent.config import SsvConfig, load_config
from ssv_agent.logging import setup_logging
from ssv_agent.models.event import ReviewResult
from ssv_agent.service import AgentService


@dataclass
class DemoCase:
    name: str
    title: str
    mode: str
    payload: dict[str, Any]
    mock_provider: bool
    expected: str


@dataclass
class DemoResult:
    name: str
    title: str
    mode: str
    expected: str
    passed: bool
    review: ReviewResult | None = None
    error: str = ""
    input_stream: str = ""
    result_stream: str = ""


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a reproducible T4 chain demo.")
    parser.add_argument("--config", default=None, help="Path to ssv.default.yaml")
    parser.add_argument("--output-dir", default=None, help="Directory for demo artifacts")
    parser.add_argument("--log-level", default="ERROR", help="Agent log level during demo")
    parser.add_argument("--skip-local-yolo", action="store_true", help="Skip real model smoke case")
    args = parser.parse_args(argv)

    setup_logging(args.log_level)

    root = _project_root()
    output_dir = Path(args.output_dir or root / "output" / "t4-demo")
    output_dir.mkdir(parents=True, exist_ok=True)

    cfg = load_config(args.config or root / "config" / "ssv.default.yaml")
    _pin_project_paths(cfg, root)
    redis = Redis(
        host=cfg.redis.host,
        port=cfg.redis.port,
        db=cfg.redis.db,
        decode_responses=True,
    )
    redis.ping()

    stamp = int(time.time() * 1000)
    cases = _mock_cases(stamp)
    if not args.skip_local_yolo:
        keyframe = _create_synthetic_keyframe(output_dir / "synthetic-keyframe.jpg")
        cases.append(_local_yolo_case(stamp, keyframe))

    print("T4 demo started")
    print(f"Redis: {cfg.redis.host}:{cfg.redis.port}/{cfg.redis.db}")
    print(f"Artifacts: {output_dir}")
    print("")

    results: list[DemoResult] = []
    for index, case in enumerate(cases, start=1):
        demo_cfg = cfg.model_copy(deep=True)
        demo_cfg.redis.stream_key = f"ssv:t4-demo:events:{stamp}:{index}"
        demo_cfg.redis.consumer_group = f"ssv-t4-demo-{stamp}-{index}"
        demo_cfg.agent.review_result_stream = f"ssv:t4-demo:reviews:{stamp}:{index}"
        demo_cfg.agent.mock_provider = case.mock_provider

        print(f"[{index}/{len(cases)}] {case.title}")
        result = _run_case(redis, demo_cfg, case)
        results.append(result)
        _write_result(output_dir, result)
        _print_case_result(result)
        print("")

    print(_format_summary(results))
    return 0 if all(result.passed for result in results) else 1


def _project_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _pin_project_paths(cfg: SsvConfig, root: Path) -> None:
    cfg.agent.rules_yaml_path = str(root / "config" / "rules.yaml")
    model_from_env = os.environ.get("SSV_AGENT_MODEL_PATH")
    if model_from_env:
        cfg.agent.local_yolo_model_path = model_from_env


def _mock_cases(stamp: int) -> list[DemoCase]:
    return [
        DemoCase(
            name="visual_review",
            title="低置信度事件: Redis 消费 + 视觉复核 + 结果回写",
            mode="mock-provider",
            expected="completed",
            mock_provider=True,
            payload={
                "type": "detection",
                "source": "t4-demo-cam-a",
                "timestamp_ms": stamp,
                "frame_id": 101,
                "detections": [
                    {
                        "class": "head",
                        "class_id": 1,
                        "confidence": 0.52,
                        "bbox": [0.35, 0.16, 0.49, 0.34],
                        "track_id": 11,
                    }
                ],
                "evidence_paths": [],
            },
        ),
        DemoCase(
            name="rule_explain",
            title="规则冲突事件: 规则检索工具 + Provider 解释 + 结果回写",
            mode="mock-provider",
            expected="completed",
            mock_provider=True,
            payload={
                "type": "detection",
                "source": "t4-demo-cam-b",
                "timestamp_ms": stamp + 1,
                "frame_id": 202,
                "detections": [
                    {
                        "class": "head",
                        "class_id": 1,
                        "confidence": 0.74,
                        "bbox": [0.31, 0.13, 0.43, 0.32],
                        "track_id": 21,
                    },
                    {
                        "class": "helmet",
                        "class_id": 0,
                        "confidence": 0.71,
                        "bbox": [0.30, 0.08, 0.45, 0.19],
                        "track_id": 21,
                    },
                ],
                "evidence_paths": [],
            },
        ),
    ]


def _local_yolo_case(stamp: int, keyframe: Path) -> DemoCase:
    return DemoCase(
        name="local_yolo_smoke",
        title="真实模型烟测: local_yolo 读取证据关键帧并走降级分支",
        mode="local-yolo",
        expected="needs_human",
        mock_provider=False,
        payload={
            "type": "detection",
            "source": "t4-demo-cam-c",
            "timestamp_ms": stamp + 2,
            "frame_id": 303,
            "detections": [
                {
                    "class": "head",
                    "class_id": 1,
                    "confidence": 0.51,
                    "bbox": [0.38, 0.18, 0.52, 0.37],
                    "track_id": 31,
                }
            ],
            "evidence_paths": [str(keyframe)],
        },
    )


def _create_synthetic_keyframe(path: Path) -> Path:
    try:
        from PIL import Image, ImageDraw
    except Exception as exc:
        raise RuntimeError(
            "Pillow is required for the local_yolo smoke case; "
            "run with --skip-local-yolo or install the agent vision extra"
        ) from exc

    image = Image.new("RGB", (640, 480), color=(238, 241, 244))
    draw = ImageDraw.Draw(image)
    draw.rectangle((220, 140, 420, 340), outline=(90, 100, 112), width=3)
    draw.text((232, 354), "synthetic keyframe", fill=(55, 65, 81))
    image.save(path, quality=92)
    return path


def _run_case(redis: Redis, cfg: SsvConfig, case: DemoCase) -> DemoResult:
    redis.delete(cfg.redis.stream_key)
    redis.delete(cfg.agent.review_result_stream)
    service = AgentService(cfg, mock_provider=case.mock_provider, redis_writer=True)
    thread = threading.Thread(target=service.run, name=f"t4-demo-{case.name}", daemon=True)
    thread.start()
    time.sleep(0.3)

    result = DemoResult(
        name=case.name,
        title=case.title,
        mode=case.mode,
        expected=case.expected,
        passed=False,
        input_stream=cfg.redis.stream_key,
        result_stream=cfg.agent.review_result_stream,
    )
    try:
        redis.xadd(cfg.redis.stream_key, {"event": json.dumps(case.payload, ensure_ascii=False)})
        review = _wait_for_review(redis, cfg.agent.review_result_stream)
        result.review = review
        result.passed = review.final_state.value == case.expected
    except Exception as exc:
        result.error = str(exc)
    finally:
        service.stop()
        thread.join(timeout=2.0)
    return result


def _wait_for_review(redis: Redis, stream: str, timeout_seconds: float = 90.0) -> ReviewResult:
    deadline = time.time() + timeout_seconds
    last_id = "0-0"
    while time.time() < deadline:
        entries = redis.xread({stream: last_id}, count=1, block=500)
        for _stream_name, messages in entries:
            for msg_id, fields in messages:
                last_id = msg_id
                raw = fields.get("review")
                if raw:
                    return ReviewResult.model_validate_json(raw)
    raise TimeoutError(f"timed out waiting for review result on {stream}")


def _write_result(output_dir: Path, result: DemoResult) -> None:
    payload: dict[str, Any] = {
        "name": result.name,
        "title": result.title,
        "mode": result.mode,
        "expected": result.expected,
        "passed": result.passed,
        "error": result.error,
        "input_stream": result.input_stream,
        "result_stream": result.result_stream,
    }
    if result.review is not None:
        payload["review"] = result.review.model_dump(mode="json")
    path = output_dir / f"{result.name}.json"
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def _print_case_result(result: DemoResult) -> None:
    if result.review is None:
        print(f"  FAIL: {result.error}")
        return
    review = result.review
    tools = ", ".join(
        f"{tool.tool_name}:{'ok' if tool.success else 'fail'}"
        for tool in review.tool_results
    ) or "-"
    print(f"  state: {review.final_state.value} (expected: {result.expected})")
    print(f"  strategy: {review.strategy.value}")
    print(f"  provider: {review.provider_used}")
    print(f"  tools: {tools}")
    print(f"  conclusion: {review.conclusion}")


def _format_summary(results: list[DemoResult]) -> str:
    passed = sum(1 for result in results if result.passed)
    lines = [
        "T4 demo summary",
        f"passed: {passed}/{len(results)}",
        "case | mode | state | strategy | provider",
        "---- | ---- | ----- | -------- | --------",
    ]
    for result in results:
        if result.review is None:
            lines.append(f"{result.name} | {result.mode} | fail | - | -")
            continue
        review = result.review
        lines.append(
            f"{result.name} | {result.mode} | {review.final_state.value} | "
            f"{review.strategy.value} | {review.provider_used}"
        )
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
