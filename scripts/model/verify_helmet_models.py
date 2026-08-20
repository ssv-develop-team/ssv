#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


def normalize_names(names):
    if isinstance(names, dict):
        return {str(k): str(v) for k, v in names.items()}
    return {str(i): str(name) for i, name in enumerate(names)}


def verify_model(path, source, export):
    from ultralytics import YOLO

    item = {
        "path": str(path),
        "load_ok": False,
        "predict_ok": False,
        "names": {},
        "detections": [],
        "exported_onnx": "",
    }
    model = YOLO(str(path))
    item["load_ok"] = True
    item["names"] = normalize_names(model.names)

    if source:
        results = model.predict(source=str(source), verbose=False)
        item["predict_ok"] = True
        for result in results:
            boxes = getattr(result, "boxes", None)
            if boxes is None:
                continue
            for box in boxes:
                cls_id = int(box.cls[0].item())
                conf = float(box.conf[0].item())
                item["detections"].append({
                    "class_id": cls_id,
                    "class_name": item["names"].get(str(cls_id), f"class_{cls_id}"),
                    "confidence": conf,
                })

    if export:
        exported = model.export(format="onnx", imgsz=640, simplify=True)
        item["exported_onnx"] = str(exported)

    return item


def main():
    parser = argparse.ArgumentParser(description="Verify helmet YOLO .pt models.")
    parser.add_argument("--models-dir", default="models")
    parser.add_argument("--source", default="")
    parser.add_argument("--export", default="")
    parser.add_argument("--output", default="artifacts/model-verification/helmet-model-summary.json")
    args = parser.parse_args()

    models_dir = Path(args.models_dir)
    model_paths = sorted(models_dir.glob("comp-*.pt"))
    if not model_paths:
        raise SystemExit(f"No comp-*.pt models found in {models_dir}")

    export_path = Path(args.export).resolve() if args.export else None
    source = Path(args.source).resolve() if args.source else None

    summary = {"models": []}
    for path in model_paths:
        should_export = export_path is not None and path.resolve() == export_path
        summary["models"].append(verify_model(path, source, should_export))

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {output}")


if __name__ == "__main__":
    main()
