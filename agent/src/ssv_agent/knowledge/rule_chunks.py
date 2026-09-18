"""规则文档的分块与稳定 chunk ID。"""

from __future__ import annotations

from dataclasses import dataclass
import re

from ssv_agent.knowledge.catalog import RuleDocument


MAX_SECTION_LENGTH = 1200
CLAUSE_HEADING = re.compile(
    r"^(?:(?:#{1,6}\s*)?(?P<section>\d+(?:\.\d+){0,5})(?=\s|$)"
    r"|(?P<heading>#{1,6})\s+(?P<title>\S.*))"
)
_LAYOUT_NOISE = re.compile(
    r"^(?:#{1,6}\s+\?\s*\d+\s*\?|\d+|GB26860[—-]2011)$"
)


@dataclass(frozen=True)
class RulePassage:
    """一条规则正文分块及其检索元数据。"""

    source: str
    section: str
    content: str
    rule_id: str = ""
    rule_version: str = ""
    event_types: tuple[str, ...] = ()
    content_hash: str = ""


def _split_large_section(section: str) -> list[str]:
    if len(section) <= MAX_SECTION_LENGTH:
        return [section]

    chunks: list[str] = []
    buffer = ""
    for sentence in re.split(r"(?<=[\u3002\uff01\uff1f\uff1b!?;])", section):
        sentence = sentence.strip()
        if not sentence:
            continue
        if buffer and len(buffer) + len(sentence) + 1 > MAX_SECTION_LENGTH:
            chunks.append(buffer.strip())
            buffer = ""
        buffer += sentence + "\n"
    if buffer.strip():
        chunks.append(buffer.strip())
    return chunks


def split_rule_clauses(
    source: str,
    text: str,
    *,
    rule: RuleDocument | None = None,
) -> list[RulePassage]:
    """按条款标题切分规则正文，并附加规则身份元数据。"""
    passages: list[RulePassage] = []

    def make_passage(section: str, content: str) -> RulePassage:
        return RulePassage(
            source=source,
            section=section,
            content=content,
            rule_id=rule.rule_id if rule else "",
            rule_version=rule.version if rule else "",
            event_types=rule.event_types if rule else (),
            content_hash=rule.content_hash if rule else "",
        )

    section = "未编号"
    section_lines: list[str] = []
    for raw_line in text.replace("\r\n", "\n").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if _LAYOUT_NOISE.fullmatch(line):
            continue
        match = CLAUSE_HEADING.match(line)
        if match and section_lines:
            passages.extend(
                make_passage(section, content)
                for content in _split_large_section("\n".join(section_lines))
            )
            section_lines = []
        if match and match.group("section"):
            section = match.group("section")
        elif match and match.group("title"):
            section = match.group("title")
        section_lines.append(line)
    if section_lines:
        passages.extend(
            make_passage(section, content)
            for content in _split_large_section("\n".join(section_lines))
        )
    return passages


def rule_chunk_id(passage: RulePassage) -> str:
    """根据来源、规则身份、条款和内容哈希生成稳定 chunk ID。"""
    digest = passage.content_hash.removeprefix("sha256:")[:12]
    return (
        f"{passage.source}:{passage.rule_id}:{passage.rule_version}:"
        f"{passage.section}:{digest}"
    )
