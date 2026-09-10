#!/usr/bin/env python3
"""Static site generator for the C++23 for Low-Latency Trading lecture series.

Reads content/*.md (front matter + restricted markdown) and writes index.html
plus lessons/*.html. Standard library only; output is fully offline-capable.
"""
from __future__ import annotations

import html
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONTENT = ROOT / "content"
OUT_LESSONS = ROOT / "lessons"

PART_ORDER = [
    "Part I - Foundations",
    "Part II - Modern C++23",
    "Part III - The Machine",
    "Part IV - Latency Engineering",
    "Part V - Concurrency",
    "Part VI - Trading Systems",
    "Appendix",
]

PART_BLURB = {
    "Part I - Foundations": "The language from zero: translation units, classes, enums, pointers, value categories, ownership.",
    "Part II - Modern C++23": "Compile-time computation, concepts, views and vocabulary types that cost nothing at run time.",
    "Part III - The Machine": "Caches, alignment, branch prediction, inlining, allocation-free data structures.",
    "Part IV - Latency Engineering": "Measuring nanoseconds honestly, then removing them: layout, prefetch, SIMD, PGO.",
    "Part V - Concurrency": "Atomics, memory ordering, wait-free queues, core pinning, seqlocks for market data.",
    "Part VI - Trading Systems": "Feed handlers, order books, tick-to-trade paths and a capstone you can compile.",
    "Appendix": "Reference cards, compiler flags, further reading.",
}

# ---------------------------------------------------------------- highlighting

KEYWORDS = {
    "alignas", "alignof", "asm", "auto", "break", "case", "catch", "class",
    "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default",
    "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "final", "for", "friend", "goto", "if", "import", "inline",
    "module", "mutable", "namespace", "new", "noexcept", "operator",
    "override", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this",
    "thread_local", "throw", "try", "typedef", "typeid", "typename", "union",
    "using", "virtual", "volatile", "while",
}

TYPES = {
    "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
    "int", "int8_t", "int16_t", "int32_t", "int64_t", "long", "short",
    "signed", "size_t", "ptrdiff_t", "uint8_t", "uint16_t", "uint32_t",
    "uint64_t", "unsigned", "void", "wchar_t", "intptr_t", "uintptr_t",
}

LITERALS = {"true", "false", "nullptr", "NULL"}

TOKEN_RE = re.compile(
    r"""
    (?P<comment>//[^\n]*|/\*.*?\*/)
  | (?P<pp>^[ \t]*\#[^\n]*)
  | (?P<rawstr>R"([^(]*)\(.*?\)\2")
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<char>'(?:[^'\\\n]|\\.)*')
  | (?P<attr>\[\[[^\]]*\]\])
  | (?P<number>\b(?:0[xXbB][0-9a-fA-F']+|\d[\d'.]*(?:[eE][+-]?\d+)?)(?:[uUlLfFzZ]*)\b)
  | (?P<ident>[A-Za-z_]\w*)
    """,
    re.VERBOSE | re.DOTALL | re.MULTILINE,
)


def highlight_cpp(code: str) -> str:
    out: list[str] = []
    pos = 0
    for m in TOKEN_RE.finditer(code):
        out.append(html.escape(code[pos:m.start()]))
        pos = m.end()
        kind = m.lastgroup
        text = html.escape(m.group())
        if kind == "ident":
            word = m.group()
            after = code[m.end():m.end() + 1]
            if word in KEYWORDS:
                cls = "k"
            elif word in TYPES:
                cls = "t"
            elif word in LITERALS:
                cls = "l"
            elif word == "std":
                cls = "ns"
            elif after == "(":
                cls = "fn"
            elif word.isupper() and len(word) > 1:
                cls = "cst"
            else:
                out.append(text)
                continue
        elif kind == "rawstr":
            cls = "s"
        else:
            cls = {"comment": "c", "pp": "pp", "string": "s", "char": "s",
                   "attr": "at", "number": "n"}[kind]
        out.append(f'<span class="{cls}">{text}</span>')
    out.append(html.escape(code[pos:]))
    return "".join(out)


def highlight_plain(code: str) -> str:
    return html.escape(code)


def highlight_shell(code: str) -> str:
    lines = []
    for line in code.split("\n"):
        esc = html.escape(line)
        if line.startswith("$"):
            esc = '<span class="pp">$</span>' + esc[1:]
        elif line.startswith("#"):
            esc = f'<span class="c">{esc}</span>'
        lines.append(esc)
    return "\n".join(lines)


HIGHLIGHTERS = {
    "cpp": highlight_cpp, "c++": highlight_cpp, "c": highlight_cpp,
    "sh": highlight_shell, "bash": highlight_shell, "asm": highlight_plain,
}

# ------------------------------------------------------------------- markdown

CODE_SPAN = re.compile(r"`([^`]+)`")
BOLD = re.compile(r"\*\*([^*]+)\*\*")
ITALIC = re.compile(r"(?<![\w*])\*([^*\n]+)\*(?![\w*])")
LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def inline(text: str) -> str:
    spans: list[str] = []

    def stash(m: re.Match[str]) -> str:
        spans.append(f'<code>{highlight_cpp(m.group(1))}</code>')
        return f"\x00{len(spans) - 1}\x00"

    text = CODE_SPAN.sub(stash, text)
    text = html.escape(text)
    text = LINK.sub(r'<a href="\2">\1</a>', text)
    text = BOLD.sub(r"<strong>\1</strong>", text)
    text = ITALIC.sub(r"<em>\1</em>", text)
    text = re.sub(r"\x00(\d+)\x00", lambda m: spans[int(m.group(1))], text)
    return text


def slugify(text: str) -> str:
    s = re.sub(r"[^\w\s-]", "", text.lower()).strip()
    return re.sub(r"[\s_-]+", "-", s)


ADMONITION_LABEL = {
    "note": "Note",
    "key": "Key idea",
    "warn": "Careful",
    "pitfall": "Common trap",
    "hft": "On the trading floor",
    "perf": "Performance",
    "exercise": "Exercise",
    "asm": "What the compiler emits",
}


@dataclass
class Lesson:
    number: int
    suffix: str
    slug: str
    title: str
    part: str
    summary: str = ""
    time: str = ""
    level: str = ""
    tags: list[str] = field(default_factory=list)
    body: str = ""
    headings: list[tuple[str, str]] = field(default_factory=list)

    @property
    def label(self) -> str:
        """Display number. A suffix slots a lesson in without renumbering the rest."""
        return f"{self.number:02d}{self.suffix}"

    @property
    def href(self) -> str:
        return f"{self.label}-{self.slug}.html"


def parse_front_matter(raw: str) -> tuple[dict[str, str], str]:
    if not raw.startswith("---"):
        return {}, raw
    end = raw.index("\n---", 3)
    meta: dict[str, str] = {}
    for line in raw[3:end].strip().split("\n"):
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    return meta, raw[end + 4:].lstrip("\n")


class Renderer:
    def __init__(self) -> None:
        self.headings: list[tuple[str, str]] = []

    def render(self, text: str) -> str:
        lines = text.split("\n")
        out: list[str] = []
        i = 0
        n = len(lines)
        while i < n:
            line = lines[i]
            stripped = line.strip()

            if not stripped:
                i += 1
                continue

            if stripped.startswith("```"):
                lang = stripped[3:].strip() or "text"
                caption = ""
                if " " in lang:
                    lang, caption = lang.split(" ", 1)
                buf: list[str] = []
                i += 1
                while i < n and not lines[i].strip().startswith("```"):
                    buf.append(lines[i])
                    i += 1
                i += 1
                code = "\n".join(buf)
                hl = HIGHLIGHTERS.get(lang.lower(), highlight_plain)(code)
                cap = f'<div class="code-cap">{inline(caption)}</div>' if caption else ""
                out.append(
                    f'<figure class="code">{cap}'
                    f'<div class="code-head"><span class="lang">{html.escape(lang)}</span>'
                    f'<button class="copy" type="button">copy</button></div>'
                    f'<pre><code>{hl}</code></pre></figure>'
                )
                continue

            if stripped.startswith(":::"):
                spec = stripped[3:].strip()
                kind, _, label = spec.partition(" ")
                kind = kind.lower() or "note"
                buf = []
                i += 1
                while i < n and lines[i].strip() != ":::":
                    buf.append(lines[i])
                    i += 1
                i += 1
                inner = Renderer().render("\n".join(buf))
                title = label.strip() or ADMONITION_LABEL.get(kind, "Note")
                out.append(
                    f'<aside class="adm adm-{html.escape(kind)}">'
                    f'<div class="adm-t">{inline(title)}</div>{inner}</aside>'
                )
                continue

            if stripped.startswith("#"):
                level = len(stripped) - len(stripped.lstrip("#"))
                title = stripped[level:].strip()
                sid = slugify(title)
                if level == 2:
                    self.headings.append((sid, title))
                out.append(
                    f'<h{level} id="{sid}">{inline(title)}'
                    f'<a class="anchor" href="#{sid}">#</a></h{level}>'
                )
                i += 1
                continue

            if stripped.startswith("|") and i + 1 < n and set(lines[i + 1].strip()) <= set("|-: "):
                header = [c.strip() for c in stripped.strip("|").split("|")]
                i += 2
                rows = []
                while i < n and lines[i].strip().startswith("|"):
                    rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                    i += 1
                th = "".join(f"<th>{inline(c)}</th>" for c in header)
                tb = "".join(
                    "<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
                    for r in rows
                )
                out.append(
                    f'<div class="tw"><table><thead><tr>{th}</tr></thead>'
                    f"<tbody>{tb}</tbody></table></div>"
                )
                continue

            if stripped.startswith("> "):
                buf = []
                while i < n and lines[i].strip().startswith(">"):
                    buf.append(lines[i].strip().lstrip(">").strip())
                    i += 1
                out.append(f"<blockquote>{Renderer().render(chr(10).join(buf))}</blockquote>")
                continue

            m_ol = re.match(r"^(\d+)\.\s+(.*)", stripped)
            if stripped.startswith(("- ", "* ")) or m_ol:
                ordered = bool(m_ol)
                items: list[str] = []
                while i < n:
                    s = lines[i].strip()
                    m2 = re.match(r"^(\d+)\.\s+(.*)", s)
                    if s.startswith(("- ", "* ")):
                        items.append(s[2:])
                    elif m2:
                        items.append(m2.group(2))
                    elif s and (lines[i].startswith("  ") or lines[i].startswith("\t")) and items:
                        items[-1] += " " + s
                    else:
                        break
                    i += 1
                tag = "ol" if ordered else "ul"
                li = "".join(f"<li>{inline(x)}</li>" for x in items)
                out.append(f"<{tag}>{li}</{tag}>")
                continue

            buf = []
            while i < n and lines[i].strip() and not lines[i].strip().startswith(
                ("```", ":::", "#", "- ", "* ", "> ", "|")
            ) and not re.match(r"^\d+\.\s", lines[i].strip()):
                buf.append(lines[i].strip())
                i += 1
            if buf:
                out.append(f"<p>{inline(' '.join(buf))}</p>")
            else:
                i += 1
        return "\n".join(out)

# ------------------------------------------------------------------- template

SITE_TITLE = "C++23 for Low-Latency Trading"


def shell(title: str, body: str, depth: int, extra_class: str = "") -> str:
    base = "../" if depth else ""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<link rel="stylesheet" href="{base}assets/style.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='13' font-size='13'>&#9889;</text></svg>">
</head>
<body class="{extra_class}">
{body}
<script src="{base}assets/site.js"></script>
</body>
</html>
"""


def nav_html(lessons: list[Lesson], current: Lesson | None, depth: int) -> str:
    base = "../" if depth else ""
    parts: list[str] = []
    for part in PART_ORDER:
        group = [l for l in lessons if l.part == part]
        if not group:
            continue
        open_attr = " open" if current is None or current.part == part else ""
        items = "".join(
            f'<li class="{"here" if current and l.href == current.href else ""}">'
            f'<a href="{base}lessons/{l.href}"><span class="num">{l.label}</span>'
            f"{html.escape(l.title)}</a></li>"
            for l in group
        )
        parts.append(
            f"<details{open_attr}><summary>{html.escape(part)}"
            f'<span class="cnt">{len(group)}</span></summary><ul>{items}</ul></details>'
        )
    return (
        f'<nav class="sidebar"><a class="brand" href="{base}index.html">'
        f'<span class="mark">&#9889;</span><span>{SITE_TITLE}</span></a>'
        f'<div class="navsearch"><input id="q" type="search" placeholder="Filter lessons" '
        f'autocomplete="off"></div>'
        f'<div class="navtree">{"".join(parts)}</div></nav>'
    )


def build_index(lessons: list[Lesson]) -> str:
    cards = []
    for part in PART_ORDER:
        group = [l for l in lessons if l.part == part]
        if not group:
            continue
        rows = "".join(
            f'<a class="row" href="lessons/{l.href}">'
            f'<span class="num">{l.label}</span>'
            f'<span class="rt"><b>{html.escape(l.title)}</b>'
            f'<span class="rs">{html.escape(l.summary)}</span></span>'
            f'<span class="rm">{html.escape(l.time)}</span></a>'
            for l in group
        )
        cards.append(
            f'<section class="part"><header><h2>{html.escape(part)}</h2>'
            f"<p>{html.escape(PART_BLURB.get(part, ''))}</p></header>"
            f'<div class="rows">{rows}</div></section>'
        )

    n_parts = len({l.part for l in lessons})
    hero = f"""
<header class="hero">
  <div class="kicker">Lecture series &middot; {len(lessons)} lesson{'' if len(lessons) == 1 else 's'} &middot; {n_parts} part{'' if n_parts == 1 else 's'} &middot; C++23</div>
  <h1>{SITE_TITLE}</h1>
  <p class="lede">From <em>what is a class</em> to cache-line padding, branchless
  decoding and a wait-free order book. Every chapter earns its place on a hot path:
  if it does not show up in a tick-to-trade profile, it is not here.</p>
  <div class="cta">
    <a class="btn primary" href="lessons/{lessons[0].href}">Start at lesson {lessons[0].label}</a>
    <a class="btn" href="#curriculum">Jump to the curriculum</a>
  </div>
  <div class="statbar">
    <div><b>0</b><span>allocations on the hot path</span></div>
    <div><b>64 B</b><span>the unit that actually matters</span></div>
    <div><b>p99.9</b><span>the number you are graded on</span></div>
  </div>
</header>
<section class="prereq">
  <h3>How to use this series</h3>
  <ol>
    <li>Read a lesson, then compile its snippets. Nothing here is theoretical.</li>
    <li>Paste the hot ones into a disassembler and read the output. The assembly is the ground truth.</li>
    <li>Measure before you believe. Part IV teaches you how to measure without lying to yourself.</li>
  </ol>
  <p class="fine">Compiles with GCC 14+ or Clang 18+ using <code>-std=c++23 -O2</code>.
  Linux-specific material is flagged where it appears.</p>
  <h3>Where to start</h3>
  <ul class="tracks">
    <li><b>New to C++.</b> Lessons 01 to 18 in order. Do not skip ahead to Part III:
    the optimisation material assumes you can read the code it optimises.</li>
    <li><b>Fluent in C++, new to latency.</b> Skim Part II for the C++23 additions, then
    start at lesson 19. Part III is the core of the series.</li>
    <li><b>Already writing hot paths.</b> Lessons 24, 26, 32, 33 and 39 are the ones that
    change how a system is built. Lesson 43 is a review checklist you can use tomorrow.</li>
  </ul>
</section>
<h2 class="curriculum-h" id="curriculum">Curriculum</h2>
"""
    body = (
        nav_html(lessons, None, 0)
        + f'<main class="content home">{hero}{"".join(cards)}'
        + '<footer class="foot">Serve locally: <code>python3 -m http.server 8000</code></footer>'
        + "</main>"
    )
    return shell(SITE_TITLE, body, 0, "has-nav")


def build_lesson(lesson: Lesson, lessons: list[Lesson], renderer: Renderer) -> str:
    idx = lessons.index(lesson)
    prev = lessons[idx - 1] if idx else None
    nxt = lessons[idx + 1] if idx + 1 < len(lessons) else None

    toc = ""
    if len(lesson.headings) > 2:
        items = "".join(
            f'<li><a href="#{sid}">{html.escape(t)}</a></li>' for sid, t in lesson.headings
        )
        toc = f'<aside class="toc"><div class="toc-t">On this page</div><ul>{items}</ul></aside>'

    tags = "".join(f'<span class="tag">{html.escape(t)}</span>' for t in lesson.tags)
    meta = (
        f'<div class="lmeta"><span class="pill">{html.escape(lesson.part)}</span>'
        f'<span class="pill lvl-{html.escape(lesson.level)}">{html.escape(lesson.level)}</span>'
        f'<span class="pill">{html.escape(lesson.time)}</span></div>'
    )

    pager = '<nav class="pager">'
    pager += (
        f'<a class="pg prev" href="{prev.href}"><span>Previous</span><b>{html.escape(prev.title)}</b></a>'
        if prev else '<a class="pg prev" href="../index.html"><span>Back</span><b>Curriculum</b></a>'
    )
    if nxt:
        pager += f'<a class="pg next" href="{nxt.href}"><span>Next</span><b>{html.escape(nxt.title)}</b></a>'
    pager += "</nav>"

    body = (
        nav_html(lessons, lesson, 1)
        + '<main class="content lesson">'
        + f'<div class="crumb"><a href="../index.html">Curriculum</a> / {html.escape(lesson.part)}</div>'
        + f'<h1><span class="lnum">{lesson.label}</span>{html.escape(lesson.title)}</h1>'
        + f'<p class="lede">{inline(lesson.summary)}</p>'
        + meta + (f'<div class="tags">{tags}</div>' if tags else "")
        + toc
        + f'<article class="prose">{lesson.body}</article>'
        + pager
        + "</main>"
    )
    return shell(f"{lesson.label} - {lesson.title}", body, 1, "has-nav")


# ----------------------------------------------------------------------- main

def main() -> None:
    files = sorted(CONTENT.glob("*.md"))
    if not files:
        raise SystemExit("no content found in content/")

    lessons: list[Lesson] = []
    for path in files:
        m = re.match(r"^(\d+)([a-z]?)-(.+)\.md$", path.name)
        if not m:
            continue
        raw = path.read_text(encoding="utf-8")
        meta, body_md = parse_front_matter(raw)
        renderer = Renderer()
        body = renderer.render(body_md)
        lessons.append(
            Lesson(
                number=int(m.group(1)),
                suffix=m.group(2),
                slug=m.group(3),
                title=meta.get("title", m.group(2)),
                part=meta.get("part", "Appendix"),
                summary=meta.get("summary", ""),
                time=meta.get("time", ""),
                level=meta.get("level", ""),
                tags=[t.strip() for t in meta.get("tags", "").split(",") if t.strip()],
                body=body,
                headings=renderer.headings,
            )
        )

    lessons.sort(key=lambda l: (l.number, l.suffix))
    order = {p: i for i, p in enumerate(PART_ORDER)}
    unknown = {l.part for l in lessons} - set(PART_ORDER)
    if unknown:
        raise SystemExit(f"unknown part(s): {sorted(unknown)}")

    if OUT_LESSONS.exists():
        shutil.rmtree(OUT_LESSONS)
    OUT_LESSONS.mkdir()

    for lesson in lessons:
        (OUT_LESSONS / lesson.href).write_text(
            build_lesson(lesson, lessons, Renderer()), encoding="utf-8"
        )
    (ROOT / "index.html").write_text(build_index(lessons), encoding="utf-8")
    print(f"built {len(lessons)} lessons across {len({l.part for l in lessons})} parts")


if __name__ == "__main__":
    main()
