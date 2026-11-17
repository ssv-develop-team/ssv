"""以原文偏移为基础的条款、转换噪声和表格结构分析。"""

from __future__ import annotations

import html
import re
from collections import Counter
from dataclasses import dataclass

_ATX = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*))?$")
_FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
_PAGE = re.compile(r"<!--\s*PDF\s*第\s*\d+\s*页\s*-->")
_IMAGE = re.compile(r"^!\[([^\]]*)\]\([^)]+\)\s*$")
_CAPTION = re.compile(r"^[图表]\s*(?:[A-Z]\s*[.．]\s*)?\d")
_CLAUSE = re.compile(r"^([1-9]\d?(?:\.\d{1,2}){0,5}|[A-Z](?:\.\d{1,2})+)(.*)$")
_ANNEX = re.compile(r"^附\s*录\s*([A-Z])(?:\s|[（(]|$)")
_SEPARATOR = re.compile(r"^:?-{3,}:?$")


def _visible(text: str) -> str:
    return html.unescape(re.sub(r"<br\s*/?>", " ", text, flags=re.IGNORECASE))


def _cells(text: str) -> tuple[str, ...]:
    # 转义管道和代码中的管道不能可靠地当作列边界，留给原文回退。
    if "\\|" in text or "`" in text or not text.strip().startswith("|"):
        return ()
    stripped = text.strip()
    body = stripped[1:-1] if stripped.endswith("|") else stripped[1:]
    return tuple(_visible(cell).strip() for cell in body.split("|"))


@dataclass(frozen=True)
class _SourceLine:
    start: int
    end: int
    text: str
    ignored: bool
    code: bool


@dataclass(frozen=True)
class StructuredBlock:
    start: int
    end: int
    heading_path: tuple[str, ...]
    table_name: str = ""
    table_headers: tuple[str, ...] = ()


@dataclass(frozen=True)
class StructuredDocument:
    blocks: tuple[StructuredBlock, ...]
    lines: tuple[_SourceLine, ...]

    def embedding_content(self, block: StructuredBlock, start: int, end: int) -> str:
        pieces = []
        for line in self.lines:
            if line.start >= end:
                break
            if line.end <= start or line.ignored:
                continue
            fragment = line.text[max(0, start - line.start) : min(len(line.text), end - line.start)]
            pieces.append(fragment if line.code else _visible(fragment))
        content = "".join(pieces)
        if start == block.start and end == block.end and block.table_headers:
            cells = _cells(content)
            if len(cells) == len(block.table_headers):
                return "\n".join(
                    f"{label}：{value}" if label else value
                    for label, value in zip(block.table_headers, cells, strict=True)
                    if value
                )
        return content


def _source_lines(text: str) -> tuple[list[_SourceLine], bool]:
    raw = text.splitlines(keepends=True)
    code = []
    fence: tuple[str, int] | None = None
    for line in raw:
        marker = _FENCE.match(line.rstrip())
        code.append(fence is not None or marker is not None)
        if marker:
            if fence is None:
                fence = (marker[1][0], len(marker[1]))
            elif marker[1][0] == fence[0] and len(marker[1]) >= fence[1] and not marker[2].strip():
                fence = None

    pdf = any(_PAGE.search(line) and not code[index] for index, line in enumerate(raw))

    page_headers = []
    if pdf:
        for index, line in enumerate(raw):
            if _PAGE.search(line) and not code[index]:
                following = next((item.strip() for item in raw[index + 1 :] if item.strip()), "")
                if following and not following.startswith(("#", "|", "!", "<")):
                    page_headers.append(following)
    repeated = {
        line for line, count in Counter(page_headers).items() if count >= 3 and len(line) <= 100
    }
    lines = []
    offset = 0
    navigation: int | None = None
    for index, line in enumerate(raw):
        stripped = line.strip()
        heading = _ATX.match(line.rstrip()) if not code[index] else None
        title = (heading[2] or "").strip() if heading else ""
        compact = re.sub(r"\s", "", title)
        if pdf and heading:
            if compact in ("目次", "目录", "索引"):
                navigation = len(heading[1])
            elif (
                navigation is not None
                and len(heading[1]) <= navigation
                and not re.fullmatch(r"[A-Z]", compact)
            ):
                navigation = None
        image = _IMAGE.fullmatch(stripped) if not code[index] else None
        generic_image = image is not None and (
            not image[1] or re.fullmatch(r"PDF\s*第\s*\d+\s*页原图", image[1]) is not None
        )
        page_number = False
        if pdf and re.fullmatch(r"[0-9ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩ]+", stripped):
            following = next((item.strip() for item in raw[index + 1 :] if item.strip()), "")
            page_number = bool(_PAGE.search(following) or _IMAGE.fullmatch(following))
        ignored = not code[index] and (
            bool(_PAGE.search(line))
            or stripped in repeated
            or generic_image
            or page_number
            or navigation is not None
        )
        lines.append(_SourceLine(offset, offset + len(line), line, ignored, code[index]))
        offset += len(line)
    return lines, pdf


def _clause(title: str, pdf: bool) -> tuple[tuple[str, ...], str, bool] | None:
    candidate = re.sub(r"^-\s+", "", title.strip())
    annex = _ANNEX.match(candidate)
    if annex:
        return (annex[1],), candidate, False
    match = _CLAUSE.match(candidate)
    if not match:
        return None
    number, remainder = match[1], match[2]
    if not remainder:
        return (tuple(number.split(".")), number, False) if pdf and "." in number else None
    if not remainder[0].isspace() and not re.match(r"[\u4e00-\u9fff]", remainder):
        return None
    remainder = remainder.strip()
    is_body = len(remainder) > 60 or bool(re.search(r"[。；;，,：:]", remainder))
    return tuple(number.split(".")), number if is_body else f"{number} {remainder}", is_body


def _reliable_headers(cells: tuple[str, ...]) -> tuple[str, ...]:
    labels = [cell for cell in cells if cell]
    if (
        not labels
        or len(set(labels)) != len(labels)
        or sum(map(len, labels)) > 256
        or any(len(label) > 64 or re.search(r"\d[-.]\d", label) for label in labels)
    ):
        return ()
    return cells


def analyze_structured(text: str) -> StructuredDocument:
    lines, pdf = _source_lines(text)
    blocks: list[StructuredBlock] = []
    markdown: list[tuple[int, str]] = []
    numbered: list[tuple[tuple[str, ...], str]] = []
    heading_path: tuple[str, ...] = ()
    document_title = ""
    table_name = ""
    pending = 0
    has_body = False

    def flush(end: int, *, allow_heading_only: bool = False) -> None:
        if pending < end and (has_body or allow_heading_only):
            blocks.append(StructuredBlock(pending, end, heading_path))

    index = 0
    while index < len(lines):
        line = lines[index]
        if line.ignored:
            index += 1
            continue
        stripped = line.text.strip()
        heading = _ATX.match(line.text.rstrip()) if not line.code else None
        title = re.sub(r"\s+#+$", "", (heading[2] or "").strip()) if heading else stripped
        caption = _CAPTION.match(title) if not line.code else None
        if caption:
            if title.startswith("表"):
                flush(line.start)
                pending, has_body = line.start, False
                table_name = title
            index += 1
            continue

        cells = _cells(line.text) if not line.code else ()
        next_cells = _cells(lines[index + 1].text) if index + 1 < len(lines) else ()
        if cells and next_cells and all(_SEPARATOR.fullmatch(cell) for cell in next_cells):
            flush(line.start)
            labels = _reliable_headers(cells) if len(cells) == len(next_cells) else ()
            if not labels:
                blocks.append(StructuredBlock(line.start, line.end, heading_path, table_name))
            index += 2
            while index < len(lines) and lines[index].text.strip().startswith("|"):
                row = lines[index]
                if not row.ignored:
                    blocks.append(
                        StructuredBlock(row.start, row.end, heading_path, table_name, labels)
                    )
                index += 1
            pending = lines[index].start if index < len(lines) else len(text)
            has_body = False
            continue

        clause = (
            _clause(title, pdf) if not line.code and not stripped.startswith(("|", ">")) else None
        )
        if clause or heading:
            if has_body:
                flush(line.start)
                pending, has_body = line.start, False
            table_name = ""
            if clause:
                key, name, inline_body = clause
                numbered = [
                    (prior, label)
                    for prior, label in numbered
                    if len(prior) < len(key) and key[: len(prior)] == prior
                ]
                numbered.append((key, name))
                base = (
                    [document_title] if pdf and document_title else [label for _, label in markdown]
                )
                heading_path = tuple(dict.fromkeys([*base, *(label for _, label in numbered)]))
                has_body = inline_body
            else:
                assert heading is not None
                if not document_title and title:
                    document_title = title
                markdown = [(level, label) for level, label in markdown if level < len(heading[1])]
                if title:
                    markdown.append((len(heading[1]), title))
                if pdf and numbered and numbered[0][0][0].isalpha():
                    heading_path = tuple(
                        dict.fromkeys([document_title, *(label for _, label in numbered), title])
                    )
                else:
                    numbered = []
                    heading_path = tuple(dict.fromkeys(label for _, label in markdown))
        elif stripped:
            has_body = True
        index += 1
    flush(len(text), allow_heading_only=not blocks)
    return StructuredDocument(tuple(blocks), tuple(lines))
