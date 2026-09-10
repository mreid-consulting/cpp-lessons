#!/usr/bin/env python3
"""Structural checks on content/*.md. Run before build.py."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONTENT = ROOT / "content"

PARTS = {
    "Part I - Foundations", "Part II - Modern C++23", "Part III - The Machine",
    "Part IV - Latency Engineering", "Part V - Concurrency",
    "Part VI - Trading Systems", "Appendix",
}
LEVELS = {"beginner", "intermediate", "advanced", "expert"}
REQUIRED = {"title", "part", "summary", "time", "level"}
ADM_KINDS = {"note", "key", "warn", "pitfall", "hft", "perf", "exercise", "asm"}


def check(path: Path) -> list[str]:
    problems: list[str] = []
    raw = path.read_text(encoding="utf-8")

    if not raw.startswith("---\n"):
        return ["missing front matter"]
    try:
        end = raw.index("\n---", 3)
    except ValueError:
        return ["unterminated front matter"]

    meta = {}
    for line in raw[4:end].split("\n"):
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    body = raw[end + 4:]

    for key in sorted(REQUIRED - set(meta)):
        problems.append(f"front matter missing '{key}'")
    if meta.get("part") and meta["part"] not in PARTS:
        problems.append(f"unknown part {meta['part']!r}")
    if meta.get("level") and meta["level"] not in LEVELS:
        problems.append(f"unknown level {meta['level']!r}")
    if len(meta.get("summary", "")) < 20:
        problems.append("summary too short")

    # Fences and admonitions must balance, counting only at line starts.
    lines = body.split("\n")
    in_fence = False
    open_adm: list[int] = []
    sections = 0
    for i, line in enumerate(lines, 1):
        s = line.strip()
        if s.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if s.startswith(":::"):
            spec = s[3:].strip()
            if not spec:
                if not open_adm:
                    problems.append(f"line {i}: ':::' close with nothing open")
                else:
                    open_adm.pop()
            else:
                kind = spec.split(" ")[0].lower()
                if kind not in ADM_KINDS:
                    problems.append(f"line {i}: unknown admonition kind {kind!r}")
                open_adm.append(i)
        elif s.startswith("## "):
            sections += 1
    if in_fence:
        problems.append("unclosed code fence")
    for i in open_adm:
        problems.append(f"line {i}: unclosed admonition")

    if sections < 4:
        problems.append(f"only {sections} top-level sections, want at least 4")
    if not re.search(r"^:::hft\b", body, re.M):
        problems.append("no :::hft block")
    if not re.search(r"^:::exercise\b", body, re.M):
        problems.append("no :::exercise block")
    if not re.search(r"^## Takeaways\s*$", body, re.M):
        problems.append("no '## Takeaways' section")

    # Appendix pages are reference cards and reading lists, so they carry tables
    # and prose rather than worked code.
    min_fences = 1 if meta.get("part") == "Appendix" else 3
    fences = len(re.findall(r"^```", body, re.M)) // 2
    if fences < min_fences:
        problems.append(f"only {fences} code blocks")

    words = len(re.sub(r"```.*?```", "", body, flags=re.S).split())
    if words < 600:
        problems.append(f"only ~{words} words of prose")

    for m in re.finditer(r"\blesson (\d+)\b", body, re.I):
        n = int(m.group(1))
        if not 1 <= n <= 44:
            problems.append(f"cross-reference to nonexistent lesson {n}")

    return problems


def main() -> None:
    files = sorted(CONTENT.glob("*.md"))
    total = 0
    for path in files:
        problems = check(path)
        if problems:
            total += len(problems)
            print(f"\n{path.name}")
            for p in problems:
                print(f"  - {p}")
    print(f"\n{len(files)} files checked, {total} problems")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
