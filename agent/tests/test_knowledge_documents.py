from __future__ import annotations

import hashlib
from itertools import pairwise
from pathlib import Path

import pytest

from ssv_agent.knowledge.documents import ChunkingConfig, KnowledgeDocument, split_document


def test_short_text_and_normalized_identity_preserve_original_content() -> None:
    document = KnowledgeDocument.from_text("\ufeff一句话。\r\n", source="note")
    assert document == KnowledgeDocument.from_text("一句话。\n", source="note")
    assert document.document_id != KnowledgeDocument.from_text("一句话。\n").document_id
    passages = split_document(document, ChunkingConfig())
    assert len(passages) == 1
    assert passages[0].content == "一句话。\n"
    assert (passages[0].line_start, passages[0].line_end) == (1, 1)


@pytest.mark.parametrize("suffix", [".md", ".MD", ".markdown", ".txt"])
@pytest.mark.parametrize("encoding", ["utf-8-sig", "gb18030"])
def test_read_text_files_without_metadata(tmp_path: Path, suffix: str, encoding: str) -> None:
    content = "# 标题\n\n123\n- 条目\n```python\nx = 42\n```\n| 名称 | 数量 |\n| 电缆 | 3 |\n"
    path = tmp_path / f"document{suffix}"
    path.write_bytes(content.encode(encoding))
    document = KnowledgeDocument.from_file(path)
    passages = split_document(document, ChunkingConfig(200, 20))
    assert document.source == str(path.resolve())
    assert passages[0].content == content
    assert passages[0].line_end == 9


@pytest.mark.parametrize("text", ["", " \n\t", "\ufeff", "abc\x00def", "abc\x01def"])
def test_reject_empty_or_binary_text(text: str) -> None:
    with pytest.raises(ValueError):
        KnowledgeDocument.from_text(text)


@pytest.mark.parametrize(
    ("name", "data", "error"),
    [
        ("empty.md", b"", "empty"),
        ("binary.md", b"abc\x00def", "binary"),
        ("invalid.txt", b"\xff", "decode"),
        ("unsupported.pdf", b"%PDF", "unsupported"),
    ],
)
def test_reject_invalid_file(tmp_path: Path, name: str, data: bytes, error: str) -> None:
    path = tmp_path / name
    path.write_bytes(data)
    with pytest.raises(ValueError, match=error):
        KnowledgeDocument.from_file(path)


def test_missing_file_and_blank_source(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="cannot read"):
        KnowledgeDocument.from_file(tmp_path / "missing.md")
    with pytest.raises(ValueError, match="source"):
        KnowledgeDocument.from_text("text", source=" ")


@pytest.mark.parametrize("size,overlap", [(40, 0), (40, 8), (4, 3), (1, 0)])
def test_hard_limit_overlap_progress_and_reconstruction(size: int, overlap: int) -> None:
    content = "甲乙丙丁戊己庚辛壬癸" * 23
    config = ChunkingConfig(size, overlap, strategy="text")
    document = KnowledgeDocument.from_text(content)
    passages = split_document(document, config)
    assert len({p.chunk_id for p in passages}) == len(passages)
    assert all(0 < len(p.content) <= size for p in passages)
    reconstructed = passages[0].content
    for previous, passage in pairwise(passages):
        if overlap:
            assert previous.content[-overlap:] == passage.content[:overlap]
        reconstructed += passage.content[overlap:]
    assert reconstructed == content
    assert passages == split_document(document, config)
    assert (
        passages[0].chunk_id
        != split_document(document, ChunkingConfig(size + 1, 0, strategy="text"))[0].chunk_id
    )


def test_paragraph_boundaries_and_line_locations() -> None:
    document = KnowledgeDocument.from_text("# 标题\n\n第一段完整。\n\n第二段完整。\n\n第三段完整。")
    passages = split_document(document, ChunkingConfig(20, 3))
    lines = document.content.splitlines(keepends=True)
    assert len(passages) > 1
    for passage in passages:
        assert passage.content in "".join(lines[passage.line_start - 1 : passage.line_end])
        assert len(passage.content) <= 20


@pytest.mark.parametrize("size,overlap", [(0, 0), (8001, 0), (3, 3), (3, -1), (True, 0)])
def test_invalid_chunking(size: int, overlap: int) -> None:
    with pytest.raises(ValueError):
        ChunkingConfig(size, overlap)


def test_markdown_sections_have_ancestor_context_and_original_lines() -> None:
    content = (
        "# 服务指南\n\n## 采购\n\n### 联系方式\n\n找林悦。\n\n## 维修\n\n### 联系方式\n\n找陈墨。\n"
    )
    document = KnowledgeDocument.from_text(content)
    passages = split_document(document, ChunkingConfig())
    assert len(passages) == 2
    assert passages[0].heading_path == ("服务指南", "采购", "联系方式")
    assert passages[1].heading_path == ("服务指南", "维修", "联系方式")
    assert "找陈墨" not in passages[0].content
    assert "服务指南 > 维修 > 联系方式" in passages[1].embedding_text
    assert passages[1].content.startswith("## 维修")
    lines = content.splitlines(keepends=True)
    for passage in passages:
        assert passage.content == "".join(lines[passage.line_start - 1 : passage.line_end])


@pytest.mark.parametrize("fence", ["```", "~~~", "````"])
def test_fenced_headings_do_not_change_section(fence: str) -> None:
    content = f"# 真标题\n\n{fence}python\n# 不是标题\nx = 1\n{fence}\n\n正文。\n"
    passages = split_document(KnowledgeDocument.from_text(content), ChunkingConfig(80, 10))
    assert all(passage.heading_path == ("真标题",) for passage in passages)
    assert any(f"{fence}python\n# 不是标题\nx = 1\n{fence}" in p.content for p in passages)


@pytest.mark.parametrize(
    "block",
    [
        "```python\n# 不切分\na = 1\nb = 2\n```\n",
        "- 第一项及其说明\n- 第二项及其说明\n- 第三项及其说明\n",
        "| 型号 | 周期 |\n| --- | --- |\n| A | 90 |\n| B | 180 |\n",
    ],
)
def test_fitting_structures_stay_in_one_chunk(block: str) -> None:
    content = "# 标题\n\n" + "说明文字。" * 10 + "\n\n" + block + "\n结束。"
    document = KnowledgeDocument.from_text(content)
    passages = split_document(document, ChunkingConfig(80, 16))
    assert any(block in passage.content for passage in passages)
    assert all(len(passage.content) <= 80 for passage in passages)


def test_oversized_code_and_long_headings_keep_hard_limits() -> None:
    content = "# " + "长标题" * 200 + "\n\n```\n" + "x=123456\n" * 40 + "```\n"
    document = KnowledgeDocument.from_text(content)
    passages = split_document(document, ChunkingConfig(60, 12))
    assert all(len(p.content) <= 60 and len(p.embedding_text) <= 60 + 258 for p in passages)
    assert all(p.content in content for p in passages)
    # 分块是原文的连续切片；除空白外，所有原文字符都被至少一个片段覆盖。
    covered = set()
    cursor = 0
    for passage in passages:
        start = content.index(passage.content, cursor)
        covered.update(range(start, start + len(passage.content)))
        cursor = max(0, start + len(passage.content) - 12)
    assert all(index in covered or char.isspace() for index, char in enumerate(content))


def test_text_strategy_keeps_legacy_hash_and_does_not_inject_context() -> None:
    config = ChunkingConfig(1000, 100, strategy="text")
    assert config.processing_hash == hashlib.sha256(b"text-boundaries-v1:1000:100").hexdigest()
    document = KnowledgeDocument.from_text("# 采购\n找林悦。\n# 维修\n找陈墨。")
    passages = split_document(document, config)
    assert len(passages) == 1
    assert passages[0].content == passages[0].embedding_text == document.content
    assert not passages[0].heading_path
    assert config.processing_hash != ChunkingConfig().processing_hash
