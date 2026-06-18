"""Video-to-T4 demo runner.

This module is a demo adapter: it samples a local video/stream with a YOLO
model, writes one representative detection event to Redis, starts the T4 Agent,
and waits for the review result. It is not the production T1/T2/T3 pipeline.
"""

from __future__ import annotations

import argparse
import json
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
class VideoDetection:
    class_name: str
    raw_name: str
    class_id: int
    confidence: float
    bbox: list[float]


@dataclass
class SelectedFrame:
    frame_id: int
    frame: Any
    detections: list[VideoDetection]
    score: float


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a video -> Redis -> T4 demo.")
    parser.add_argument("--video", required=True, help="Local video path, RTSP URL, or camera id")
    parser.add_argument(
        "--model",
        default="/home/lzy/work-code/comp-2-freeze10.pt",
        help="Ultralytics YOLO .pt model path",
    )
    parser.add_argument("--config", default=None, help="Path to ssv.default.yaml")
    parser.add_argument("--output-dir", default=None, help="Directory for demo artifacts")
    parser.add_argument("--conf", type=float, default=0.25, help="YOLO detection confidence")
    parser.add_argument("--device", default="cpu", help="YOLO device, for example cpu or cuda:0")
    parser.add_argument("--sample-every", type=int, default=15, help="Run YOLO every N frames")
    parser.add_argument("--max-frames", type=int, default=240, help="Maximum frames to scan")
    parser.add_argument("--source-name", default="t4-video-demo", help="Event source field")
    parser.add_argument(
        "--agent-provider",
        choices=["local_yolo", "openai_compatible"],
        default="local_yolo",
        help="Provider used by T4 Agent for the review stage",
    )
    parser.add_argument(
        "--preserve-confidence",
        action="store_true",
        help="Keep detector confidence in the Redis event instead of capping for review demo",
    )
    parser.add_argument("--log-level", default="ERROR", help="Agent log level during demo")
    args = parser.parse_args(argv)

    setup_logging(args.log_level)

    root = Path(__file__).resolve().parents[3]
    output_dir = Path(args.output_dir or root / "output" / "t4-video-demo")
    output_dir.mkdir(parents=True, exist_ok=True)

    cfg = load_config(args.config or root / "config" / "ssv.default.yaml")
    _pin_config(cfg, root, args.model, args.agent_provider)

    redis = Redis(
        host=cfg.redis.host,
        port=cfg.redis.port,
        db=cfg.redis.db,
        decode_responses=True,
    )
    redis.ping()

    print("T4 video demo started")
    print(f"Video: {args.video}")
    print(f"Model: {args.model}")
    print(f"Redis: {cfg.redis.host}:{cfg.redis.port}/{cfg.redis.db}")
    print(f"Artifacts: {output_dir}")
    print("")

    selected = _scan_video(
        source=args.video,
        model_path=args.model,
        conf=args.conf,
        device=args.device,
        sample_every=args.sample_every,
        max_frames=args.max_frames,
    )

    keyframe_path = output_dir / "keyframe.jpg"
    annotated_path = output_dir / "keyframe-annotated.jpg"
    _write_keyframes(selected, keyframe_path, annotated_path)

    event = _build_event(
        selected=selected,
        source_name=args.source_name,
        keyframe_path=keyframe_path,
        preserve_confidence=args.preserve_confidence,
    )
    event_path = output_dir / "event.json"
    event_path.write_text(json.dumps(event, ensure_ascii=False, indent=2), encoding="utf-8")

    stamp = int(time.time() * 1000)
    cfg.redis.stream_key = f"ssv:t4-video-demo:events:{stamp}"
    cfg.redis.consumer_group = f"ssv-t4-video-demo-{stamp}"
    cfg.agent.review_result_stream = f"ssv:t4-video-demo:reviews:{stamp}"
    cfg.agent.mock_provider = False

    redis.delete(cfg.redis.stream_key)
    redis.delete(cfg.agent.review_result_stream)

    service = AgentService(cfg, mock_provider=False, redis_writer=True)
    thread = threading.Thread(target=service.run, name="t4-video-demo-agent", daemon=True)
    thread.start()
    time.sleep(0.3)

    try:
        msg_id = redis.xadd(cfg.redis.stream_key, {"event": json.dumps(event, ensure_ascii=False)})
        review = _wait_for_review(redis, cfg.agent.review_result_stream)
    finally:
        service.stop()
        thread.join(timeout=2.0)

    review_path = output_dir / "review.json"
    review_path.write_text(
        review.model_dump_json(indent=2),
        encoding="utf-8",
    )

    _print_summary(
        selected=selected,
        event=event,
        review=review,
        msg_id=msg_id,
        input_stream=cfg.redis.stream_key,
        result_stream=cfg.agent.review_result_stream,
        output_dir=output_dir,
    )
    return 0


def _pin_config(cfg: SsvConfig, root: Path, model_path: str, agent_provider: str) -> None:
    cfg.agent.provider_type = agent_provider
    cfg.agent.local_yolo_model_path = model_path
    cfg.agent.rules_yaml_path = str(root / "config" / "rules.yaml")


def _scan_video(
    source: str,
    model_path: str,
    conf: float,
    device: str,
    sample_every: int,
    max_frames: int,
) -> SelectedFrame:
    try:
        import cv2
        from ultralytics import YOLO
    except Exception as exc:
        raise RuntimeError(
            "video demo requires ultralytics and opencv; run uv sync --extra vision"
        ) from exc

    model_path_obj = Path(model_path)
    if not model_path_obj.exists():
        raise FileNotFoundError(f"YOLO model not found: {model_path_obj}")

    capture_source: str | int = int(source) if source.isdigit() else source
    cap = cv2.VideoCapture(capture_source)
    if not cap.isOpened():
        raise RuntimeError(f"failed to open video source: {source}")

    model = YOLO(str(model_path_obj))
    best: SelectedFrame | None = None
    frame_id = -1
    try:
        while frame_id + 1 < max_frames:
            ok, frame = cap.read()
            if not ok:
                break
            frame_id += 1
            if frame_id % max(1, sample_every) != 0:
                continue

            results = model.predict(frame, conf=conf, device=device, verbose=False)
            detections = _parse_detections(results, frame.shape)
            score = _frame_score(detections)
            candidate = SelectedFrame(
                frame_id=frame_id,
                frame=frame.copy(),
                detections=detections,
                score=score,
            )
            if best is None or candidate.score > best.score:
                best = candidate
            if _has_head_without_helmet(detections) and score >= 100.5:
                break
    finally:
        cap.release()

    if best is None:
        raise RuntimeError(f"no frame could be read from video source: {source}")
    return best


def _parse_detections(results, frame_shape: tuple[int, int, int]) -> list[VideoDetection]:
    if not results:
        return []
    result = results[0]
    names = getattr(result, "names", None) or {}
    boxes = getattr(result, "boxes", None)
    if boxes is None:
        return []

    height, width = frame_shape[:2]
    detections: list[VideoDetection] = []
    for box in boxes:
        cls_id = int(_scalar(getattr(box, "cls", -1)))
        confidence = float(_scalar(getattr(box, "conf", 0.0)))
        raw_name = str(names.get(cls_id, cls_id)).lower()
        class_name = _normalize_class(raw_name)
        if class_name not in {"helmet", "head"}:
            continue

        xyxy = getattr(box, "xyxy", None)
        coords = xyxy[0].tolist() if xyxy is not None else [0, 0, 0, 0]
        x1, y1, x2, y2 = coords
        detections.append(
            VideoDetection(
                class_name=class_name,
                raw_name=raw_name,
                class_id=cls_id,
                confidence=confidence,
                bbox=[
                    _clamp01(float(x1) / width),
                    _clamp01(float(y1) / height),
                    _clamp01(float(x2) / width),
                    _clamp01(float(y2) / height),
                ],
            )
        )
    return detections


def _normalize_class(raw_name: str) -> str:
    name = raw_name.replace(" ", "_").replace("-", "_").lower()
    if "no_helmet" in name or "nohelmet" in name or "without_helmet" in name:
        return "head"
    if name in {"head", "person", "worker"} or "head" in name or "person" in name:
        return "head"
    if "helmet" in name or "hardhat" in name or "hard_hat" in name:
        return "helmet"
    return name


def _frame_score(detections: list[VideoDetection]) -> float:
    if not detections:
        return 0.0
    best_conf = max(d.confidence for d in detections)
    has_head = any(d.class_name == "head" for d in detections)
    has_helmet = any(d.class_name == "helmet" for d in detections)
    if has_head and not has_helmet:
        return 100.0 + best_conf
    if has_head and has_helmet:
        return 80.0 + best_conf
    return 50.0 + best_conf


def _has_head_without_helmet(detections: list[VideoDetection]) -> bool:
    return (
        any(d.class_name == "head" for d in detections)
        and not any(d.class_name == "helmet" for d in detections)
    )


def _write_keyframes(selected: SelectedFrame, keyframe_path: Path, annotated_path: Path) -> None:
    import cv2

    cv2.imwrite(str(keyframe_path), selected.frame)
    annotated = selected.frame.copy()
    height, width = annotated.shape[:2]
    for detection in selected.detections:
        x1, y1, x2, y2 = detection.bbox
        pt1 = (int(x1 * width), int(y1 * height))
        pt2 = (int(x2 * width), int(y2 * height))
        color = (0, 180, 0) if detection.class_name == "helmet" else (0, 0, 220)
        cv2.rectangle(annotated, pt1, pt2, color, 2)
        label = f"{detection.class_name} {detection.confidence:.2f}"
        cv2.putText(
            annotated,
            label,
            (pt1[0], max(20, pt1[1] - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.6,
            color,
            2,
        )
    cv2.imwrite(str(annotated_path), annotated)


def _build_event(
    selected: SelectedFrame,
    source_name: str,
    keyframe_path: Path,
    preserve_confidence: bool,
) -> dict[str, Any]:
    detections = []
    for index, detection in enumerate(selected.detections):
        confidence = detection.confidence if preserve_confidence else min(detection.confidence, 0.55)
        detections.append(
            {
                "class": detection.class_name,
                "class_id": detection.class_id,
                "confidence": round(confidence, 4),
                "bbox": [round(v, 6) for v in detection.bbox],
                "track_id": index + 1,
            }
        )
    return {
        "type": "detection",
        "source": source_name,
        "timestamp_ms": int(time.time() * 1000),
        "frame_id": selected.frame_id,
        "detections": detections,
        "evidence_paths": [str(keyframe_path)],
    }


def _wait_for_review(redis: Redis, stream: str, timeout_seconds: float = 120.0) -> ReviewResult:
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


def _print_summary(
    selected: SelectedFrame,
    event: dict[str, Any],
    review: ReviewResult,
    msg_id: str,
    input_stream: str,
    result_stream: str,
    output_dir: Path,
) -> None:
    detections = event["detections"]
    print("Chain flow")
    print(f"  video/input: {event['source']} frame={selected.frame_id}")
    print(f"  yolo/model: detections={len(detections)}")
    print(f"  evidence: {event['evidence_paths'][0] if event.get('evidence_paths') else '-'}")
    print(f"  redis/event: {input_stream} msg_id={msg_id}")
    print("  t4/context: DetectionEvent -> ContextEngine -> StateMachine")
    print(f"  t4/tools: {', '.join(t.tool_name for t in review.tool_results) or '-'}")
    print(f"  redis/review: {result_stream}")
    print("")
    print("Video detection summary")
    print(f"  selected_frame: {selected.frame_id}")
    print(f"  detections: {len(detections)}")
    for detection in detections:
        print(
            "  - "
            f"{detection['class']} conf={detection['confidence']:.2f} "
            f"bbox={detection['bbox']}"
        )
    print("")
    print("Redis handoff")
    print(f"  input_stream: {input_stream}")
    print(f"  event_msg_id: {msg_id}")
    print(f"  result_stream: {result_stream}")
    print("")
    print("T4 review result")
    print(f"  state: {review.final_state.value}")
    print(f"  strategy: {review.strategy.value}")
    print(f"  provider: {review.provider_used}")
    print(f"  conclusion: {review.conclusion}")
    print("")
    print("T4 tool outputs")
    for tool in review.tool_results:
        status = "ok" if tool.success else "fail"
        text = tool.output if tool.success else tool.error
        print(f"  [{tool.tool_name}:{status}]")
        print(_indent(_shorten(text, limit=1800), prefix="    "))
    print("")
    print("Artifacts")
    print(f"  {output_dir / 'keyframe.jpg'}")
    print(f"  {output_dir / 'keyframe-annotated.jpg'}")
    print(f"  {output_dir / 'event.json'}")
    print(f"  {output_dir / 'review.json'}")


def _scalar(value) -> float:
    if hasattr(value, "item"):
        return value.item()
    if isinstance(value, (list, tuple)) and value:
        return _scalar(value[0])
    return value


def _clamp01(value: float) -> float:
    return max(0.0, min(1.0, value))


def _shorten(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit].rstrip() + "\n...（已截断，完整内容见 review.json）"


def _indent(text: str, prefix: str) -> str:
    if not text:
        return prefix + "-"
    return "\n".join(prefix + line for line in text.splitlines())


if __name__ == "__main__":
    sys.exit(main())
