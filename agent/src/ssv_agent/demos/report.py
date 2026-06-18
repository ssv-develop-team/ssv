"""Reporting helpers for T4 demo scripts.

The shell entrypoints keep process orchestration, while this module owns JSON
summaries and the HTML report. Keeping report formatting here makes the demo
layer easier to test and keeps shell scripts readable.
"""

from __future__ import annotations

import argparse
import html
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="T4 demo report helper.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    case_parser = subparsers.add_parser("case-summary", help="Print one video demo case summary.")
    case_parser.add_argument("title")
    case_parser.add_argument("folder")

    report_parser = subparsers.add_parser("generate", help="Generate summary.md and report.html.")
    report_parser.add_argument("output_dir")

    ready_parser = subparsers.add_parser("openai-ready", help="Check local .env OpenAI-compatible config.")
    ready_parser.add_argument("--env-file", default=".env")

    args = parser.parse_args(argv)
    if args.command == "case-summary":
        print_case_summary(args.title, Path(args.folder))
        return 0
    if args.command == "generate":
        print(generate_report(Path(args.output_dir)))
        return 0
    if args.command == "openai-ready":
        print("yes" if openai_ready(Path(args.env_file)) else "no")
        return 0
    raise AssertionError(f"unhandled command: {args.command}")


def print_case_summary(title: str, folder: Path) -> None:
    event = _load_json(folder / "event.json")
    review = _load_json(folder / "review.json")
    if not event or not review:
        print(f"{title}: skipped")
        return

    counts = Counter(item.get("class", "unknown") for item in event.get("detections", []))
    tools = _tool_line(review)

    print("")
    print(f"【{title}】")
    print(f"  帧号: {event.get('frame_id')}")
    print(
        f"  检测: {len(event.get('detections', []))} 个 "
        f"({', '.join(f'{k}={v}' for k, v in sorted(counts.items()))})"
    )
    print(f"  终态: {review.get('final_state')} / 策略: {review.get('strategy')}")
    print(f"  Provider: {review.get('provider_used')}")
    print(f"  工具: {tools}")
    print("  复核输出:")
    for line in _shorten(_tool_output(review, "rule_explain"), limit=900).splitlines():
        print(f"    {line}")
    print(f"  带框图: {folder / 'keyframe-annotated.jpg'}")
    print(f"  结果: {folder / 'review.json'}")


def generate_report(output_dir: Path) -> str:
    output_dir.mkdir(parents=True, exist_ok=True)
    local_event = _load_json(output_dir / "local_yolo" / "event.json")
    local_review = _load_json(output_dir / "local_yolo" / "review.json")
    openai_event = _load_json(output_dir / "openai" / "event.json")
    openai_review = _load_json(output_dir / "openai" / "review.json")

    summary_text = _build_summary_md(
        output_dir=output_dir,
        local_event=local_event,
        local_review=local_review,
        openai_event=openai_event,
        openai_review=openai_review,
    )
    (output_dir / "summary.md").write_text(summary_text, encoding="utf-8")

    html_text = _build_report_html(
        local_event=local_event,
        local_review=local_review,
        openai_event=openai_event,
        openai_review=openai_review,
    )
    (output_dir / "report.html").write_text(html_text, encoding="utf-8")

    lines = [
        summary_text.rstrip(),
        f"汇总文件: {output_dir / 'summary.md'}",
        f"演示页: {output_dir / 'report.html'}",
    ]
    return "\n".join(lines)


def openai_ready(env_file: Path) -> bool:
    cfg: dict[str, str] = {}
    if env_file.exists():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            if "=" not in line or line.strip().startswith("#"):
                continue
            key, value = line.split("=", 1)
            cfg[key.strip()] = value.strip()

    base_url = cfg.get("SSV_AGENT_API_BASE_URL", "")
    api_key = cfg.get("SSV_AGENT_API_KEY") or cfg.get("DASHSCOPE_API_KEY") or ""
    model = cfg.get("SSV_AGENT_TEXT_MODEL", "")
    bad_base = (
        not base_url
        or "{WorkspaceId}" in base_url
        or "你的WorkspaceId" in base_url
        or "请填写" in base_url
    )
    bad_key = not api_key or "请填写" in api_key or "你的真实API_KEY" in api_key
    return not bad_base and not bad_key and bool(model)


def _build_summary_md(
    *,
    output_dir: Path,
    local_event: dict[str, Any] | None,
    local_review: dict[str, Any] | None,
    openai_event: dict[str, Any] | None,
    openai_review: dict[str, Any] | None,
) -> str:
    lines = [
        "# T4 一键演示汇总",
        "",
        "| 阶段 | 状态 | 策略 | Provider | 检测 | 工具 | 产物 |",
        "|:--|:--|:--|:--|:--|:--|:--|",
    ]
    if (output_dir / "internal").exists():
        lines.append(
            "| T4 内部闭环 | passed | visual_review / rule_explain | "
            "mock-text-provider | mock events | "
            "visual_review / rule_retrieval / rule_explain | `internal/` |"
        )
    for name, event, review, folder in [
        ("真实视频 + 本地 YOLO", local_event, local_review, "local_yolo"),
        ("真实视频 + 外部 AI", openai_event, openai_review, "openai"),
    ]:
        if review:
            lines.append(
                "| "
                + " | ".join(
                    [
                        name,
                        review.get("final_state", "-"),
                        review.get("strategy", "-"),
                        review.get("provider_used", "-"),
                        _det_summary(event),
                        _tool_line(review),
                        f"`{folder}/review.json`",
                    ]
                )
                + " |"
            )
        else:
            lines.append(f"| {name} | skipped | - | - | - | - | `{folder}/` |")

    lines.extend(
        [
            "",
            "## 链路数据流",
            "",
            "```text",
            "视频文件 / RTSP",
            "  -> Python 演示适配器抽帧",
            "  -> YOLO 安全帽模型生成 detections",
            "  -> 保存 keyframe.jpg / keyframe-annotated.jpg",
            "  -> 构造 DetectionEvent(event.json)",
            "  -> 写入 Redis event stream",
            "  -> T4 Agent 消费事件",
            "  -> ContextEngine 构造上下文",
            "  -> StateMachine 选择复核策略",
            "  -> RuleRetrievalTool 检索规则",
            "  -> provider(local_yolo 或 openai_compatible) 复核",
            "  -> ReviewResult 写回 Redis 并保存 review.json",
            "```",
            "",
            "## 复核输出摘录",
            "",
            "### 本地 YOLO provider",
            "",
            "```text",
            _tool_output(local_review, "rule_explain") or "-",
            "```",
            "",
            "### 外部 AI provider",
            "",
            "```text",
            _tool_output(openai_review, "rule_explain") or "-",
            "```",
            "",
            "## 建议现场展示",
            "",
            "- `local_yolo/keyframe-annotated.jpg`",
            "- `local_yolo/review.json`",
            "- `openai/review.json`（如果已配置外部 AI provider）",
        ]
    )
    return "\n".join(lines) + "\n"


def _build_report_html(
    *,
    local_event: dict[str, Any] | None,
    local_review: dict[str, Any] | None,
    openai_event: dict[str, Any] | None,
    openai_review: dict[str, Any] | None,
) -> str:
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>T4 一键演示</title>
  <style>
    body {{ font-family: sans-serif; margin: 24px; line-height: 1.55; color: #1f2937; }}
    h1 {{ margin-bottom: 8px; }}
    .flow, pre {{ background: #f3f4f6; padding: 12px; border-radius: 6px; white-space: pre-wrap; }}
    .card {{ border: 1px solid #d1d5db; border-radius: 8px; padding: 16px; margin: 18px 0; }}
    .grid {{ display: grid; grid-template-columns: minmax(320px, 1fr) minmax(320px, 1fr); gap: 18px; align-items: start; }}
    img {{ max-width: 100%; border: 1px solid #d1d5db; border-radius: 6px; }}
    table {{ border-collapse: collapse; width: 100%; }}
    td, th {{ border: 1px solid #d1d5db; padding: 8px; text-align: left; }}
    @media (max-width: 900px) {{ .grid {{ grid-template-columns: 1fr; }} }}
  </style>
</head>
<body>
  <h1>T4 一键完整演示</h1>
  <p>这页用于现场展示：视频检测、关键帧证据、Redis 事件、T4 状态机、规则检索、provider 复核和结果回写。</p>
  <h2>链路数据流</h2>
  <div class="flow">视频文件 / RTSP
  -> Python 演示适配器抽帧
  -> YOLO 安全帽模型生成 detections
  -> 保存 keyframe.jpg / keyframe-annotated.jpg
  -> 构造 DetectionEvent(event.json)
  -> 写入 Redis event stream
  -> T4 Agent 消费事件
  -> ContextEngine 构造上下文
  -> StateMachine 选择复核策略
  -> RuleRetrievalTool 检索规则
  -> provider(local_yolo 或 openai_compatible) 复核
  -> ReviewResult 写回 Redis 并保存 review.json</div>
  {_card("真实视频 + 本地 YOLO provider", local_event, local_review, "local_yolo/keyframe-annotated.jpg", _tool_output(local_review, "rule_explain"))}
  {_card("真实视频 + 外部 AI provider", openai_event, openai_review, "openai/keyframe-annotated.jpg", _tool_output(openai_review, "rule_explain"))}
</body>
</html>
"""


def _card(
    title: str,
    event: dict[str, Any] | None,
    review: dict[str, Any] | None,
    image_rel: str,
    output: str,
) -> str:
    if not review:
        return f"<section><h2>{_esc(title)}</h2><p>未运行或未配置。</p></section>"
    return f"""
    <section class="card">
      <h2>{_esc(title)}</h2>
      <div class="grid">
        <div>
          <p><b>状态</b>：{_esc(review.get('final_state'))}</p>
          <p><b>策略</b>：{_esc(review.get('strategy'))}</p>
          <p><b>Provider</b>：{_esc(review.get('provider_used'))}</p>
          <p><b>检测</b>：{_esc(_det_summary(event))}</p>
          <p><b>工具</b>：{_esc(_tool_line(review))}</p>
          <h3>复核输出</h3>
          <pre>{_esc(output)}</pre>
        </div>
        <div>
          <img src="{_esc(image_rel)}" alt="{_esc(title)} 带框关键帧">
        </div>
      </div>
    </section>
    """


def _load_json(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def _tool_output(review: dict[str, Any] | None, name: str) -> str:
    if not review:
        return ""
    for item in review.get("tool_results", []):
        if item.get("tool_name") == name:
            return item.get("output") if item.get("success") else item.get("error", "")
    return ""


def _tool_line(review: dict[str, Any] | None) -> str:
    if not review:
        return "-"
    parts = []
    for item in review.get("tool_results", []):
        parts.append(f"{item.get('tool_name')}:{'ok' if item.get('success') else 'fail'}")
    return ", ".join(parts) or "-"


def _det_summary(event: dict[str, Any] | None) -> str:
    if not event:
        return "-"
    detections = event.get("detections", [])
    counts = Counter(item.get("class", "unknown") for item in detections)
    return f"{len(detections)} 个 / " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))


def _shorten(text: str, limit: int) -> str:
    text = (text or "").strip()
    return text if len(text) <= limit else text[:limit].rstrip() + "\n...（完整内容见 review.json）"


def _esc(value: Any) -> str:
    return html.escape(str(value or ""))


if __name__ == "__main__":
    sys.exit(main())
