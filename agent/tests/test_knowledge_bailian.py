"""在 IDE 点击运行的百炼真实接口测试；密钥放在本地 .env，不写入测试代码。"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import pytest

    from ssv_agent.knowledge.base import KnowledgeBase

MODEL = "qwen3.7-text-embedding-flash"


def _configure_bailian(monkeypatch: pytest.MonkeyPatch) -> None:
    import pytest
    from dotenv import dotenv_values

    agent_dir = Path(__file__).resolve().parents[1]
    settings = {
        **dotenv_values(agent_dir.parent / ".env"),
        **dotenv_values(agent_dir / ".env"),
        **os.environ,
    }
    if settings.get("SSV_RUN_BAILIAN_TEST") != "1":
        pytest.skip("在仓库根目录 .env 设置 SSV_RUN_BAILIAN_TEST=1 后点击运行")
    for name in ("SSV_EMBEDDING_BASE_URL", "SSV_EMBEDDING_API_KEY"):
        value = settings.get(name)
        if not value or not value.strip():
            pytest.skip(f"请先在仓库根目录 .env 填写 {name}，再点击运行")
        monkeypatch.setenv(name, value.strip())
    # 固定已确认有额度的 Flash 模型，避免终端遗留配置切回非 Flash 版本。
    monkeypatch.delenv("SSV_EMBEDDING_QUERY_TEXT_TYPE", raising=False)


def test_bailian_flash_retrieves_sentences(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """两句话入库后，用不同问法分别检出原句；真实请求会消耗百炼额度。"""
    from ssv_agent.knowledge.base import KnowledgeBase

    _configure_bailian(monkeypatch)
    manual = "设备保养手册放在资料室第二层书架。"
    cafeteria = "员工餐厅每天中午十二点开放。"
    cases = [
        ("去哪里找设备的保养资料？", manual),
        ("午饭几点可以去餐厅吃？", cafeteria),
    ]
    knowledge_path = tmp_path / "bailian_knowledge"

    async def exercise_knowledge_base() -> None:
        with KnowledgeBase(
            path=knowledge_path,
            embedding_backend="openai_compatible",
            embedding_model=MODEL,
        ) as knowledge:
            first = await knowledge.ingest_text(manual, source="IDE测试")
            second = await knowledge.ingest_text(cafeteria, source="IDE测试")
            duplicate = await knowledge.ingest_text(manual, source="IDE测试")
            assert first.success and first.chunks_count == 1
            assert second.success and second.chunks_count == 1
            assert duplicate.skipped and duplicate.document_id == first.document_id

        # 重新打开同一临时存储，验证结果来自持久化的知识，而非内存缓存。
        with KnowledgeBase(
            path=knowledge_path,
            embedding_backend="openai_compatible",
            embedding_model=MODEL,
        ) as reopened:
            for query, expected in cases:
                hits = await reopened.search(query, top_k=2)
                with capsys.disabled():
                    print(
                        json.dumps(
                            {"模型": MODEL, "问题": query, "预期原文": expected, "检索结果": hits},
                            ensure_ascii=False,
                            indent=2,
                        )
                    )
                assert len(hits) == 2, "两条知识均应进入向量检索候选"
                assert hits[0]["content"] == expected, f"首位结果未命中预期原文：{query}"
                assert hits[0]["source"] == "IDE测试"

    asyncio.run(exercise_knowledge_base())


async def _assess_retrieval(knowledge: KnowledgeBase, cases: list[dict]) -> dict:
    """固定证据标注和诊断阈值；不将相似度分数当作答案正确率。"""
    rows = []
    for case in cases:
        hits = await knowledge.search(case["query"], top_k=3)
        evidence = case["evidence"]
        rank = next(
            (index for index, hit in enumerate(hits, 1) if evidence and evidence in hit["content"]),
            None,
        )
        rows.append(
            {
                "query": case["query"],
                "category": case["category"],
                "answerable": evidence is not None,
                "rank": rank,
                "top_score": hits[0]["score"] if hits else None,
                "top_content": hits[0]["content"] if hits else None,
                "heading_path": hits[0].get("heading_path", []) if hits else [],
            }
        )
    answered = [row for row in rows if row["answerable"]]
    unanswered = [row for row in rows if not row["answerable"]]
    return {
        "hit_at_1": sum(row["rank"] == 1 for row in answered) / len(answered),
        "recall_at_3": sum(row["rank"] is not None for row in answered) / len(answered),
        "mrr_at_3": sum(1 / row["rank"] for row in answered if row["rank"]) / len(answered),
        "no_answer_false_positive_at_0_5": sum(
            row["top_score"] is not None and row["top_score"] >= 0.5 for row in unanswered
        )
        / len(unanswered),
        "rows": rows,
    }


def test_bailian_markdown_retrieval_quality(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """固定合成样本上的同模型 A/B；不能替代真实业务语料评估。"""
    from ssv_agent.embedding.backends.openai_compatible import OpenAICompatibleEmbeddingProvider
    from ssv_agent.embedding.provider import EmbeddingProvider
    from ssv_agent.embedding.registry import resolve_embedding_identity
    from ssv_agent.knowledge.base import KnowledgeBase
    from ssv_agent.knowledge.documents import ChunkingConfig

    _configure_bailian(monkeypatch)
    fixture_dir = Path(__file__).parent / "fixtures" / "knowledge"
    cases = json.loads((fixture_dir / "retrieval_cases.json").read_text(encoding="utf-8"))
    real_provider = OpenAICompatibleEmbeddingProvider(model=MODEL)

    class CachedQueries(EmbeddingProvider):
        backend_name = "openai_compatible"

        def __init__(self) -> None:
            self.queries: dict[str, list[float]] = {}

        async def embed_texts(self, texts: list[str]) -> list[list[float]]:
            return await real_provider.embed_texts(texts)

        async def embed_query(self, query: str) -> list[float]:
            if query not in self.queries:
                self.queries[query] = await real_provider.embed_query(query)
            return self.queries[query]

    async def compare() -> None:
        provider = CachedQueries()
        reports = {}
        for strategy in ("text", "markdown", "structured"):
            with KnowledgeBase(
                path=tmp_path / strategy,
                provider=provider,
                embedding_identity=resolve_embedding_identity("openai_compatible", MODEL),
                chunking=ChunkingConfig(160, 24, strategy=strategy),
            ) as knowledge:
                ingestion = await knowledge.ingest_file(fixture_dir / "handbook.md")
                reports[strategy] = {
                    "chunks_count": ingestion.chunks_count,
                    **await _assess_retrieval(knowledge, cases),
                }
        with capsys.disabled():
            print("\nMarkdown 检索质量对比（固定虚构样本，非通用准确率）")
            for strategy, report in reports.items():
                print(
                    f"{strategy}: 块数={report['chunks_count']}, "
                    f"首位命中={report['hit_at_1']:.1%}, "
                    f"前三条召回={report['recall_at_3']:.1%}, "
                    f"MRR@3={report['mrr_at_3']:.3f}, "
                    f"无答案误召回@0.5={report['no_answer_false_positive_at_0_5']:.1%}"
                )
            for before, after in zip(
                reports["markdown"]["rows"], reports["structured"]["rows"], strict=True
            ):
                print(f"\n[{after['category']}] {after['query']}")
                if after["answerable"]:
                    print(
                        f"  证据排名：{before['rank'] or '未进前三'} -> {after['rank'] or '未进前三'}"
                    )
                else:
                    print("  文档无答案；相似度高也不表示有答案")
                scores = [
                    f"{row['top_score']:.4f}" if row["top_score"] is not None else "无结果"
                    for row in (before, after)
                ]
                print(f"  首位分数：{scores[0]} -> {scores[1]}")
                print(f"  新首位章节：{' > '.join(after['heading_path'])}")
                print(f"  新首位原文：{after['top_content']}")
        assert len(provider.queries) == len(cases), "A/B 应复用完全相同的查询向量"
        assert reports["structured"]["hit_at_1"] >= reports["markdown"]["hit_at_1"]
        assert reports["structured"]["recall_at_3"] >= reports["markdown"]["recall_at_3"]

    asyncio.run(compare())


def _run_as_script(test_file: Path | None = None) -> int:
    """允许 IDE 运行 Python 文件，也允许尚未安装 pytest 的全局 Python 启动。"""
    selected_file = (test_file or Path(__file__)).resolve()
    required_modules = ("pytest", "dotenv", "ssv_agent", "openai", "qdrant_client")
    if not all(importlib.util.find_spec(name) is not None for name in required_modules):
        candidates = []
        if local_app_data := os.getenv("LOCALAPPDATA"):
            candidates.append(Path(local_app_data) / "teamssv/knowledge-venv/Scripts/python.exe")
        candidates.append(
            Path(__file__).resolve().parents[1]
            / ".venv"
            / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        )
        for interpreter in candidates:
            if interpreter.is_file() and interpreter.resolve() != Path(sys.executable).resolve():
                probe = subprocess.run(
                    [
                        str(interpreter),
                        "-c",
                        "import pytest, dotenv, ssv_agent, openai, qdrant_client",
                    ],
                    check=False,
                    capture_output=True,
                )
                if probe.returncode != 0:
                    continue
                print(f"使用知识库测试环境：{interpreter}", flush=True)
                return subprocess.run(
                    [str(interpreter), "-X", "utf8", str(selected_file), *sys.argv[1:]],
                    check=False,
                ).returncode
        print(
            "当前 Python 缺少测试依赖，请在 IDE 选择已安装知识库和 pytest 的解释器。",
            file=sys.stderr,
        )
        return 1

    import pytest

    return int(
        pytest.main([str(selected_file), "-v", "-s", "-p", "no:cacheprovider", *sys.argv[1:]])
    )


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(_run_as_script())
