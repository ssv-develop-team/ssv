"""无业务模板的纯文本文档、稳定身份和带原文定位的分块。"""

from __future__ import annotations

import hashlib
import json
import re
from bisect import bisect_right
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from ssv_agent.knowledge.structured import StructuredBlock, analyze_structured

SUPPORTED_TEXT_EXTENSIONS = frozenset({".md", ".markdown", ".txt"})
TEXT_CHUNKER_VERSION = "text-boundaries-v1"
CHUNKER_VERSION = "markdown-sections-v2"
STRUCTURED_CHUNKER_VERSION = "structured-blocks-v1"
MAX_HEADING_CONTEXT = 256
_HEADING = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*))?$")
_FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
_LIST_ITEM = re.compile(r"^\s*(?:[-+*]|\d+[.)])[ \t]+")


def _digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class ChunkingConfig:
    chunk_size: int = 1000
    chunk_overlap: int = 100
    strategy: Literal["text", "markdown", "structured"] = "markdown"

    def __post_init__(self) -> None:
        if type(self.chunk_size) is not int or not 1 <= self.chunk_size <= 8000:
            raise ValueError("chunk_size must be an integer between 1 and 8000")
        if type(self.chunk_overlap) is not int or not 0 <= self.chunk_overlap < self.chunk_size:
            raise ValueError("chunk_overlap must be an integer in [0, chunk_size)")
        if self.strategy not in ("text", "markdown", "structured"):
            raise ValueError("strategy must be text, markdown or structured")

    @property
    def processing_hash(self) -> str:
        version = TEXT_CHUNKER_VERSION if self.strategy == "text" else CHUNKER_VERSION
        if self.strategy == "structured":
            version = STRUCTURED_CHUNKER_VERSION
        return _digest(f"{version}:{self.chunk_size}:{self.chunk_overlap}")


@dataclass(frozen=True)
class KnowledgeDocument:
    document_id: str
    source: str
    content_hash: str
    content: str

    @classmethod
    def from_text(cls, text: str, *, source: str = "text") -> KnowledgeDocument:
        if not isinstance(text, str) or not text.strip():
            raise ValueError("text must not be empty")
        if not isinstance(source, str) or not source.strip():
            raise ValueError("source must not be empty")
        content = text.removeprefix("\ufeff").replace("\r\n", "\n").replace("\r", "\n")
        if not content.strip():
            raise ValueError("text must not be empty")
        if any(ord(char) < 32 and char not in "\n\t" for char in content):
            raise ValueError("text contains binary control characters")
        content_hash = "sha256:" + _digest(content)
        source = source.strip()
        identity = json.dumps([source, content_hash], ensure_ascii=False)
        return cls(_digest(identity), source, content_hash, content)

    @classmethod
    def from_file(cls, path: str | Path) -> KnowledgeDocument:
        file_path = Path(path).expanduser().resolve()
        if file_path.suffix.lower() not in SUPPORTED_TEXT_EXTENSIONS:
            raise ValueError("unsupported file format; use .md, .markdown or .txt")
        try:
            data = file_path.read_bytes()
        except OSError as exc:
            raise ValueError(f"cannot read file: {file_path}: {exc}") from exc
        if b"\x00" in data:
            raise ValueError("file contains binary data or an unsupported text encoding")
        for encoding in ("utf-8-sig", "gb18030"):
            try:
                text = data.decode(encoding)
            except UnicodeDecodeError:
                continue
            return cls.from_text(text, source=str(file_path))
        raise ValueError("cannot decode file as UTF-8 or GB18030")


@dataclass(frozen=True)
class KnowledgePassage:
    chunk_id: str
    document_id: str
    content: str
    source: str
    chunk_index: int
    content_hash: str
    line_start: int
    line_end: int
    heading_path: tuple[str, ...] = ()
    embedding_content: str | None = None
    table_name: str = ""
    table_headers: tuple[str, ...] = ()

    @property
    def embedding_text(self) -> str:
        body = self.content if self.embedding_content is None else self.embedding_content
        contexts = []
        # 优先保留叶子章节，标题再长也不能无限扩大模型输入。
        if self.heading_path:
            contexts.append(" > ".join(self.heading_path)[-MAX_HEADING_CONTEXT:])
        if self.table_name or self.table_headers:
            contexts.append(
                f"表名：{self.table_name}\n表头：{'；'.join(self.table_headers)}"[
                    :MAX_HEADING_CONTEXT
                ]
            )
        return "\n\n".join([*contexts, body])


@dataclass(frozen=True)
class _TextSection:
    start: int
    end: int
    heading_path: tuple[str, ...]


def _markdown_layout(text: str) -> tuple[list[_TextSection], list[tuple[int, int]]]:
    """识别章节和可整体保留的块；所有位置都指向原始文本，不生成改写正文。"""
    sections: list[_TextSection] = []
    protected: list[tuple[int, int]] = []
    headings: list[tuple[int, str]] = []
    section_start = 0
    section_has_body = False
    offset = 0
    fence: tuple[str, int, int] | None = None
    block_start = 0
    block_kind = ""
    for line in text.splitlines(keepends=True):
        end = offset + len(line)
        stripped = line.rstrip("\n")
        marker = _FENCE.match(stripped)
        if fence is not None:
            if (
                marker
                and marker[1][0] == fence[0]
                and len(marker[1]) >= fence[1]
                and not marker[2].strip()
            ):
                protected.append((fence[2], end))
                fence = None
            offset = end
            continue

        heading = _HEADING.match(stripped)
        kind = ""
        if not marker and not heading:
            if stripped.lstrip().startswith("|"):
                kind = "table"
            elif _LIST_ITEM.match(stripped) or (
                block_kind == "list" and stripped.startswith(("  ", "\t")) and stripped.strip()
            ):
                kind = "list"
        if kind != block_kind:
            if block_kind:
                protected.append((block_start, offset))
            block_start, block_kind = offset, kind

        if marker:
            fence = (marker[1][0], len(marker[1]), offset)
            section_has_body = True
        elif heading:
            if section_has_body:
                sections.append(
                    _TextSection(section_start, offset, tuple(title for _, title in headings))
                )
                section_start = offset
                section_has_body = False
            level = len(heading[1])
            title = re.sub(r"[ \t]+#+[ \t]*$", "", heading[2] or "").strip()
            headings = [(depth, name) for depth, name in headings if depth < level]
            if title:
                headings.append((level, title))
        elif stripped.strip():
            section_has_body = True
        offset = end

    if fence is not None:
        protected.append((fence[2], len(text)))
    if block_kind:
        protected.append((block_start, len(text)))
    sections.append(_TextSection(section_start, len(text), tuple(title for _, title in headings)))
    return sections, sorted(protected)


def _text_ranges(
    text: str,
    config: ChunkingConfig,
    section: _TextSection,
    protected: list[tuple[int, int]],
) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    start = section.start
    while start < section.end:
        end = min(start + config.chunk_size, section.end)
        if end < section.end:
            lower = start + max(config.chunk_overlap + 1, config.chunk_size // 2)
            for separators in (("\n\n",), ("\n",), ("。", "！", "？", ". ", "! ", "? "), (" ",)):
                boundaries = [text.rfind(separator, lower, end) for separator in separators]
                boundary = max(boundaries)
                if boundary >= lower:
                    end = boundary + len(separators[boundaries.index(boundary)])
                    break
        skip_overlap = False
        for block_start, block_end in protected:
            if block_start < end < block_end and block_end - block_start <= config.chunk_size:
                end = block_start if start < block_start else block_end
                skip_overlap = start > section.start and end - start <= config.chunk_overlap
                break
        if skip_overlap:
            # 上一块已覆盖此重叠尾巴，跳过它才能将下个完整结构放进一块。
            start = end
            continue
        if text[start:end].strip():
            ranges.append((start, end))
        if end == section.end:
            break
        start = end - config.chunk_overlap if end - start > config.chunk_overlap else end
    return ranges


def split_document(document: KnowledgeDocument, config: ChunkingConfig) -> list[KnowledgePassage]:
    """返回带章节元数据的原文切片；text 策略保留旧分块行为。"""
    text = document.content
    line_starts = [0, *(index + 1 for index, char in enumerate(text) if char == "\n")]
    passages: list[KnowledgePassage] = []
    structured = analyze_structured(text) if config.strategy == "structured" else None
    if structured is not None:
        blocks = list(structured.blocks)
        _, protected = _markdown_layout(text)
    else:
        sections, protected = (
            _markdown_layout(text)
            if config.strategy == "markdown"
            else ([_TextSection(0, len(text), ())], [])
        )
        blocks = [StructuredBlock(s.start, s.end, s.heading_path) for s in sections]
    for block in blocks:
        section = _TextSection(block.start, block.end, block.heading_path)
        local_protected = [
            (start, end) for start, end in protected if block.start <= start and end <= block.end
        ]
        for start, end in _text_ranges(text, config, section, local_protected):
            embedding_content = (
                structured.embedding_content(block, start, end) if structured is not None else None
            )
            if embedding_content is not None and not embedding_content.strip():
                continue
            index = len(passages)
            chunk_id = _digest(f"{document.document_id}:{config.processing_hash}:{index}")
            passages.append(
                KnowledgePassage(
                    chunk_id=chunk_id,
                    document_id=document.document_id,
                    content=text[start:end],
                    source=document.source,
                    chunk_index=index,
                    content_hash=document.content_hash,
                    line_start=bisect_right(line_starts, start),
                    line_end=bisect_right(line_starts, end - 1),
                    heading_path=block.heading_path,
                    embedding_content=embedding_content,
                    table_name=block.table_name,
                    table_headers=block.table_headers,
                )
            )
    return passages
