"""独立知识入库和向量检索命令，不启动业务服务。"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from dataclasses import asdict
from pathlib import Path

from dotenv import load_dotenv

from ssv_agent.knowledge.base import KnowledgeBase, KnowledgeError
from ssv_agent.knowledge.documents import ChunkingConfig


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="纯文本知识入库与向量检索")
    parser.add_argument("--env-file", type=Path, help="显式加载环境文件；已有环境变量优先")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--qdrant-path", type=Path, help="本地路径，优先于环境中的服务 URL")
    target.add_argument("--qdrant-url", help="Qdrant 服务 URL")
    parser.add_argument("--embedding-backend", choices=("bge_m3", "openai_compatible"))
    parser.add_argument("--embedding-model")
    commands = parser.add_subparsers(dest="command", required=True)
    text = commands.add_parser("add-text", help="输入一句话或多段文字")
    text.add_argument("text", nargs="?", help="省略时从标准输入读取")
    text.add_argument("--source", default="text")
    file = commands.add_parser("add-file", help="导入 Markdown 或 TXT 文件")
    file.add_argument("path", type=Path)
    for ingest in (text, file):
        ingest.add_argument("--chunk-size", type=int, default=1000)
        ingest.add_argument("--chunk-overlap", type=int, default=100)
        ingest.add_argument(
            "--chunk-strategy", choices=("markdown", "text", "structured"), default="markdown"
        )
    search = commands.add_parser("search", help="向量检索，返回正文、相似度和来源")
    search.add_argument("query")
    search.add_argument("--top-k", type=int, default=5)
    search.add_argument("--min-score", type=float)
    search.add_argument("--document-id")
    return parser


async def _run(args: argparse.Namespace) -> dict[str, object]:
    chunking = (
        ChunkingConfig(args.chunk_size, args.chunk_overlap, strategy=args.chunk_strategy)
        if args.command != "search"
        else None
    )
    with KnowledgeBase(
        path=args.qdrant_path,
        url=args.qdrant_url,
        embedding_backend=args.embedding_backend,
        embedding_model=args.embedding_model,
        chunking=chunking,
    ) as knowledge:
        if args.command == "add-text":
            if args.text is None and sys.stdin.isatty():
                raise KnowledgeError(
                    "input", "provide text as an argument or pipe it through stdin"
                )
            text = args.text if args.text is not None else sys.stdin.read()
            return asdict(await knowledge.ingest_text(text, source=args.source))
        if args.command == "add-file":
            return asdict(await knowledge.ingest_file(args.path))
        hits = await knowledge.search(
            args.query,
            top_k=args.top_k,
            min_score=args.min_score,
            document_id=args.document_id,
        )
        return {"success": True, "query": args.query, "chunks": hits}


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.env_file is not None:
            if not args.env_file.is_file():
                raise KnowledgeError(
                    "configuration", f"environment file not found: {args.env_file}"
                )
            load_dotenv(args.env_file, override=False)
        result = asyncio.run(_run(args))
    except (KnowledgeError, ValueError, OSError) as exc:
        print(
            json.dumps(
                {"success": False, "stage": getattr(exc, "stage", "input"), "error": str(exc)},
                ensure_ascii=False,
            )
        )
        return 1
    print(json.dumps(result, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
