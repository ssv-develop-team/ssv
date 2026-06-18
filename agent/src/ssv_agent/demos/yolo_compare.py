"""YOLO video comparison demo for helmet/no-helmet visualization."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Display and save YOLO helmet comparison video.")
    parser.add_argument("--video", default="/home/lzy/work-code/30sec.mp4", help="Video path")
    parser.add_argument(
        "--model",
        default="/home/lzy/work-code/comp-2-freeze10.pt",
        help="Ultralytics YOLO .pt model path",
    )
    parser.add_argument(
        "--output-dir",
        default="/home/lzy/work-code/ssv-private/output/t4-demo",
        help="Output directory",
    )
    parser.add_argument("--conf", type=float, default=0.25, help="YOLO confidence threshold")
    parser.add_argument("--device", default="cpu", help="YOLO device, for example cpu or cuda:0")
    parser.add_argument(
        "--stride",
        type=int,
        default=1,
        help="Run inference every N frames; reused detections are shown between inference frames",
    )
    parser.add_argument("--max-frames", type=int, default=0, help="Limit processed frames; 0 = full video")
    parser.add_argument("--no-display", action="store_true", help="Do not show OpenCV window")
    parser.add_argument("--save-comparison", action="store_true", default=True)
    parser.add_argument("--window-name", default="T4 YOLO helmet compare")
    args = parser.parse_args(argv)

    return run_compare(args)


def run_compare(args: argparse.Namespace) -> int:
    try:
        import cv2
        from ultralytics import YOLO
    except Exception as exc:
        raise RuntimeError("opencv and ultralytics are required; run uv sync --extra vision") from exc

    video_path = Path(args.video)
    model_path = Path(args.model)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if not video_path.exists():
        raise FileNotFoundError(f"video not found: {video_path}")
    if not model_path.exists():
        raise FileNotFoundError(f"model not found: {model_path}")

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        raise RuntimeError(f"failed to open video: {video_path}")

    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 0)
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 0)
    fps = float(cap.get(cv2.CAP_PROP_FPS) or 25.0)
    if width <= 0 or height <= 0:
        raise RuntimeError("video has invalid width/height")

    model = YOLO(str(model_path))
    annotated_path = output_dir / "helmet_compare_annotated.mp4"
    comparison_path = output_dir / "helmet_compare_side_by_side.mp4"
    summary_path = output_dir / "helmet_compare_summary.json"
    first_no_helmet_path = output_dir / "first_no_helmet_frame.jpg"
    latest_frame_path = output_dir / "latest_compare_frame.jpg"

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    annotated_writer = cv2.VideoWriter(str(annotated_path), fourcc, fps, (width, height))
    comparison_writer = cv2.VideoWriter(str(comparison_path), fourcc, fps, (width * 2, height))

    frame_id = -1
    processed_frames = 0
    no_helmet_frames = 0
    helmet_count = 0
    no_helmet_count = 0
    last_detections: list[dict[str, Any]] = []
    first_no_helmet_saved = False
    display_enabled = not args.no_display
    display_failed = False
    started = time.time()

    print("T4 YOLO comparison demo started")
    print(f"video: {video_path}")
    print(f"model: {model_path}")
    print(f"output: {output_dir}")
    print("red boxes = no helmet/head, green boxes = helmet")
    print("press q or ESC in the OpenCV window to stop")
    print("")

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            frame_id += 1
            if args.max_frames and frame_id >= args.max_frames:
                break

            if frame_id % max(1, args.stride) == 0:
                results = model.predict(frame, conf=args.conf, device=args.device, verbose=False)
                last_detections = _parse_detections(results)

            annotated = _draw_detections(frame.copy(), last_detections)
            comparison = _build_comparison(frame, annotated)

            frame_helmet_count = sum(1 for d in last_detections if d["class_name"] == "helmet")
            frame_no_helmet_count = sum(1 for d in last_detections if d["class_name"] == "head")
            helmet_count += frame_helmet_count
            no_helmet_count += frame_no_helmet_count
            if frame_no_helmet_count:
                no_helmet_frames += 1
                if not first_no_helmet_saved:
                    cv2.imwrite(str(first_no_helmet_path), annotated)
                    first_no_helmet_saved = True

            annotated_writer.write(annotated)
            comparison_writer.write(comparison)
            processed_frames += 1

            if processed_frames % 30 == 0:
                cv2.imwrite(str(latest_frame_path), comparison)

            if display_enabled and not display_failed:
                try:
                    cv2.imshow(args.window_name, comparison)
                    key = cv2.waitKey(max(1, int(1000 / max(fps, 1)))) & 0xFF
                    if key in (27, ord("q")):
                        print("stopped by user")
                        break
                except Exception as exc:
                    display_failed = True
                    print(f"OpenCV display unavailable, continue saving video: {exc}")

    finally:
        cap.release()
        annotated_writer.release()
        comparison_writer.release()
        if display_enabled and not display_failed:
            try:
                cv2.destroyAllWindows()
            except Exception:
                pass

    elapsed = time.time() - started
    summary = {
        "video": str(video_path),
        "model": str(model_path),
        "processed_frames": processed_frames,
        "no_helmet_frames": no_helmet_frames,
        "helmet_detections_accumulated": helmet_count,
        "no_helmet_detections_accumulated": no_helmet_count,
        "elapsed_seconds": round(elapsed, 3),
        "fps_effective": round(processed_frames / elapsed, 3) if elapsed > 0 else 0,
        "outputs": {
            "annotated_video": str(annotated_path),
            "comparison_video": str(comparison_path),
            "first_no_helmet_frame": str(first_no_helmet_path) if first_no_helmet_saved else "",
            "latest_compare_frame": str(latest_frame_path),
            "summary": str(summary_path),
        },
    }
    summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    print("T4 YOLO comparison demo finished")
    print(f"processed_frames: {processed_frames}")
    print(f"no_helmet_frames: {no_helmet_frames}")
    print(f"helmet_detections_accumulated: {helmet_count}")
    print(f"no_helmet_detections_accumulated: {no_helmet_count}")
    print(f"annotated_video: {annotated_path}")
    print(f"comparison_video: {comparison_path}")
    if first_no_helmet_saved:
        print(f"first_no_helmet_frame: {first_no_helmet_path}")
    print(f"summary: {summary_path}")
    return 0


def _parse_detections(results) -> list[dict[str, Any]]:
    if not results:
        return []
    result = results[0]
    names = getattr(result, "names", None) or {}
    boxes = getattr(result, "boxes", None)
    if boxes is None:
        return []

    detections: list[dict[str, Any]] = []
    for box in boxes:
        cls_id = int(_scalar(getattr(box, "cls", -1)))
        confidence = float(_scalar(getattr(box, "conf", 0.0)))
        raw_name = str(names.get(cls_id, cls_id)).lower()
        class_name = _normalize_class(raw_name)
        if class_name not in {"helmet", "head"}:
            continue
        xyxy = getattr(box, "xyxy", None)
        coords = xyxy[0].tolist() if xyxy is not None else [0, 0, 0, 0]
        detections.append(
            {
                "class_name": class_name,
                "raw_name": raw_name,
                "confidence": confidence,
                "xyxy": [int(float(v)) for v in coords],
            }
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


def _draw_detections(frame, detections: list[dict[str, Any]]):
    import cv2

    no_helmet_count = 0
    helmet_count = 0
    for detection in detections:
        x1, y1, x2, y2 = detection["xyxy"]
        class_name = detection["class_name"]
        confidence = detection["confidence"]
        if class_name == "head":
            no_helmet_count += 1
            color = (0, 0, 255)
            label = f"NO HELMET {confidence:.2f}"
        else:
            helmet_count += 1
            color = (0, 180, 0)
            label = f"helmet {confidence:.2f}"
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        cv2.putText(
            frame,
            label,
            (x1, max(22, y1 - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.58,
            color,
            2,
            cv2.LINE_AA,
        )

    banner = f"helmet={helmet_count}  no_helmet/head={no_helmet_count}"
    cv2.rectangle(frame, (0, 0), (frame.shape[1], 34), (20, 20, 20), -1)
    cv2.putText(
        frame,
        banner,
        (12, 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.72,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    return frame


def _build_comparison(original, annotated):
    import cv2

    left = original.copy()
    cv2.rectangle(left, (0, 0), (left.shape[1], 34), (20, 20, 20), -1)
    cv2.putText(
        left,
        "Original",
        (12, 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.72,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    return cv2.hconcat([left, annotated])


def _scalar(value) -> float:
    if hasattr(value, "item"):
        return value.item()
    if isinstance(value, (list, tuple)) and value:
        return _scalar(value[0])
    return value


if __name__ == "__main__":
    sys.exit(main())
