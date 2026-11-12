from __future__ import annotations

import hashlib

import pytest

from ssv_agent.knowledge.documents import ChunkingConfig, KnowledgeDocument, split_document


def passages(text: str, size: int = 1000):
    return split_document(
        KnowledgeDocument.from_text(text), ChunkingConfig(size, min(20, size - 1), "structured")
    )


def test_numbered_sections_restore_missing_markdown_headings() -> None:
    text = "# 手册\n\n4颜色表征\n\n4.1安全色\n\n4.1.3蓝色\n\n传递指令性信息。\n\n4.1.4绿色\n\n传递提示性信息。\n"
    chunks = passages(text)
    blue = next(p for p in chunks if "传递指令性信息" in p.content)
    green = next(p for p in chunks if "传递提示性信息" in p.content)
    assert blue.heading_path == ("手册", "4 颜色表征", "4.1 安全色", "4.1.3 蓝色")
    assert green.heading_path[-1] == "4.1.4 绿色"
    assert "传递提示性信息" not in blue.content
    lines = text.splitlines(keepends=True)
    for p in chunks:
        assert p.content == "".join(lines[p.line_start - 1 : p.line_end])


def test_inline_clause_uses_identifier_without_turning_sentence_into_title() -> None:
    text = "# 手册\n\n7使用要求\n\n7.3设置要求\n\n7.3.1应设在醒目位置。\n\n7.3.2平面与视线应接近直角。\n"
    chunk = next(p for p in passages(text) if "应接近直角" in p.content)
    assert chunk.heading_path == ("手册", "7 使用要求", "7.3 设置要求", "7.3.2")
    assert "7.3.1" not in chunk.content


def test_plain_numbers_dates_and_ordered_lists_are_not_removed() -> None:
    text = "123\n\n2025-05-30发布\n\n1. 普通列表\n2. 第二项\n"
    chunks = passages(text)
    assert len(chunks) == 1
    assert chunks[0].content == chunks[0].embedding_text == text
    assert not chunks[0].heading_path


def test_pdf_noise_navigation_and_repeated_headers_only_affect_embedding() -> None:
    text = (
        "# 操作手册\n\n<!-- PDF 第 1 页 -->\n\n重复页眉\n\n### 目 次\n\n| 假目录 | 8 |\n| --- | --- |\n"
        "### 前 言\n前言仍可检索。\n\n<!-- PDF 第 2 页 -->\n\n重复页眉\n\n"
        "#### 1正文\n\n正文内容。\n\n1\n\n![PDF 第 2 页原图](page-002.png)\n\n"
        "<!-- PDF 第 3 页 -->\n\n重复页眉\n\n#### 索 引\n#### A\n索引噪声。\n"
    )
    document = KnowledgeDocument.from_text(text)
    chunks = split_document(document, ChunkingConfig(strategy="structured"))
    rendered = "\n".join(p.embedding_text for p in chunks)
    assert "正文内容" in rendered and "前言仍可检索" in rendered
    assert not any(
        noise in rendered
        for noise in ("重复页眉", "假目录", "索引噪声", "page-002", "<!--", "\n1\n")
    )
    assert document.content == text
    assert all(p.content in text for p in chunks)


def test_fenced_code_is_not_cleaned_or_parsed_as_clauses() -> None:
    text = "# 代码\n\n```\n4.1.3不是章节\n<!-- PDF 第 9 页 -->\n<br>\n\n\n123\n```\n"
    chunks = passages(text)
    assert len(chunks) == 1
    assert chunks[0].heading_path == ("代码",)
    assert "<!-- PDF 第 9 页 -->\n<br>\n\n\n123" in chunks[0].embedding_content


def test_table_rows_have_caption_headers_and_field_values() -> None:
    text = "# 管道\n\n#### 表2 基本识别色\n\n|物质|颜色|编号|\n|---|---|---|\n|氧|淡蓝|PB06|\n|水|艳绿|G03|\n"
    chunks = passages(text)
    assert len(chunks) == 2
    oxygen = chunks[0]
    assert oxygen.content == "|氧|淡蓝|PB06|\n"
    assert oxygen.heading_path == ("管道",)
    assert oxygen.table_name == "表2 基本识别色"
    assert oxygen.table_headers == ("物质", "颜色", "编号")
    assert "物质：氧\n颜色：淡蓝\n编号：PB06" in oxygen.embedding_text
    assert "物质：水" not in oxygen.embedding_text


@pytest.mark.parametrize("row", ["||蓝|PB06|", "|氧|蓝|PB06|额外列|", r"|氧\|混合|蓝|PB06|"])
def test_misaligned_or_escaped_table_rows_are_not_guessed(row: str) -> None:
    text = f"# 表格\n|物质|颜色|编号|\n|---|---|---|\n{row}\n"
    chunk = next(p for p in passages(text) if row in p.content)
    if row.startswith("||"):
        assert "物质：蓝" not in chunk.embedding_text
        assert "颜色：蓝" in chunk.embedding_text
    else:
        assert chunk.embedding_content == row + "\n"
        assert "颜色：" not in chunk.embedding_content


def test_ambiguous_headers_fall_back_and_do_not_drop_damaged_header_data() -> None:
    text = "|编号<br>1-04|名称|名称|\n|---|---|---|\n|1-05|甲|乙|\n"
    chunks = passages(text)
    assert len(chunks) == 2
    assert not any(p.table_headers for p in chunks)
    assert "1-04" in chunks[0].embedding_content
    assert "1-05" in chunks[1].embedding_content


def test_long_table_rows_keep_raw_hard_limit_and_context() -> None:
    row = "|A|" + "很长的说明" * 100 + "|\n"
    text = "# 长表\n\n表1 参数\n|型号|说明|\n|---|---|\n" + row
    chunks = passages(text, 80)
    assert len(chunks) > 1
    assert all(len(p.content) <= 80 and p.content in row for p in chunks)
    assert all(p.table_headers == ("型号", "说明") for p in chunks)
    assert all("表1 参数" in p.embedding_text for p in chunks)


def test_annex_survives_generic_heading_and_caption_is_not_a_section() -> None:
    text = "# 规范\n<!-- PDF 第 1 页 -->\n#### 附 录 C (规范性)\n#### 安全标志\nC.1警告标志\n#### 表 C.1 警告标志\n|编号|名称|\n|---|---|\n|1-01|当心危险|\n"
    chunk = next(p for p in passages(text) if "|1-01|" in p.content)
    assert any("附 录 C" in heading for heading in chunk.heading_path)
    assert chunk.heading_path[-1] == "C.1 警告标志"
    assert not any(heading.startswith("表 C.1") for heading in chunk.heading_path)


def test_strategy_identity_is_separate_and_legacy_versions_stay_compatible() -> None:
    assert (
        ChunkingConfig(strategy="markdown").processing_hash
        == hashlib.sha256(b"markdown-sections-v2:1000:100").hexdigest()
    )
    assert (
        len(
            {ChunkingConfig(strategy=s).processing_hash for s in ("text", "markdown", "structured")}
        )
        == 3
    )
