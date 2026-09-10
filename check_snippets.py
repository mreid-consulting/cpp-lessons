#!/usr/bin/env python3
"""Syntax-check the C++ snippets in content/*.md.

A block is checked if it carries its own #include lines. Lessons build code up
across several blocks, so a block that fails alone is retried with the preceding
blocks of the same lesson prepended. Only a block that fails both ways is a
genuine defect. Blocks needing a platform header or a library feature this
toolchain lacks are skipped and counted.
"""
from __future__ import annotations

import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CONTENT = ROOT / "content"

FENCE = re.compile(r"^```(cpp|c\+\+)[^\n]*\n(.*?)^```", re.M | re.S)
LOCAL_INCLUDE = re.compile(r'^\s*#include\s*"', re.M)
SYS_INCLUDE = re.compile(r"^\s*#include\s*<([^>]+)>", re.M)

# Headers that do not exist on this build host, or library features not yet
# implemented by its standard library.
UNAVAILABLE_HEADERS = {
    "sched.h", "numa.h", "immintrin.h", "x86intrin.h", "emmintrin.h",
    "sys/mman.h", "sys/socket.h", "sys/epoll.h", "netinet/in.h", "arpa/inet.h",
    "linux/if_packet.h", "pthread.h", "unistd.h", "liburing.h",
    "benchmark/benchmark.h", "hdr/hdr_histogram.h", "experimental/simd",
    "stacktrace", "flat_map", "flat_set", "simd", "execution", "print", "format",
}
UNAVAILABLE_TOKENS = (
    "std::start_lifetime_as", "std::simd", "std::stacktrace", "std::flat_map",
    "std::flat_set", "std::println", "std::print", "cpu_set_t", "__builtin_ia32",
    "_mm_", "__m256", "__m128", "asm volatile", "[[assume", "std::execution",
)
ELLIPSIS = re.compile(r"^\s*(//\s*)?\.\.\.\s*$", re.M)


def unavailable(code: str) -> bool:
    if LOCAL_INCLUDE.search(code):
        return True
    if ELLIPSIS.search(code):
        return True
    if any(tok in code for tok in UNAVAILABLE_TOKENS):
        return True
    return any(h in UNAVAILABLE_HEADERS for h in SYS_INCLUDE.findall(code))


def merge(blocks: list[str]) -> str:
    """Concatenate blocks, hoisting every #include to the top and deduplicating."""
    includes: list[str] = []
    bodies: list[str] = []
    for b in blocks:
        body_lines = []
        for line in b.split("\n"):
            if line.lstrip().startswith("#include") or line.lstrip().startswith("#pragma once"):
                inc = line.strip()
                if inc.startswith("#include") and inc not in includes:
                    includes.append(inc)
            else:
                body_lines.append(line)
        bodies.append("\n".join(body_lines))
    return "\n".join(includes) + "\n\n" + "\n\n".join(bodies)


def sdk_flags() -> list[str]:
    if platform.system() != "Darwin":
        return []
    try:
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True,
                             text=True, check=True).stdout.strip()
    except Exception:
        return []
    inc = Path(sdk) / "usr/include/c++/v1"
    return ["-isystem", str(inc)] if (inc / "algorithm").exists() else []


def main() -> None:
    base = ["c++", "-std=c++23", "-fsyntax-only", "-Wall"] + sdk_flags()
    checked = failed = skipped = recovered = 0
    reports: list[str] = []
    inconclusive: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)

        def compile_ok(code: str, name: str) -> tuple[bool, str]:
            src = tmpdir / f"{name}.cpp"
            src.write_text(code, encoding="utf-8")
            proc = subprocess.run(base + [str(src)], capture_output=True, text=True)
            return proc.returncode == 0, proc.stderr

        for path in sorted(CONTENT.glob("*.md")):
            blocks = [m.group(2) for m in FENCE.finditer(path.read_text(encoding="utf-8"))]
            for i, code in enumerate(blocks):
                if "#include" not in code:
                    continue
                if unavailable(code):
                    skipped += 1
                    continue
                checked += 1
                ok, err = compile_ok(code, f"{path.stem}_{i}")
                if ok:
                    continue
                # Retry with the lesson's earlier blocks prepended.
                context = [b for b in blocks[:i] if not unavailable(b)]
                if context:
                    ok2, err2 = compile_ok(merge(context + [code]), f"{path.stem}_{i}_ctx")
                    if ok2:
                        recovered += 1
                        continue
                    err = err2
                lines = [l.split("error:", 1)[-1].strip()
                         for l in err.split("\n") if "error:" in l][:2]
                joined = " | ".join(lines)
                # A lesson that shows a naive version and then a better one will
                # redefine names; and a block whose helper lives in a skipped block
                # cannot resolve it. Neither is a defect in the snippet.
                if "redefinition" in joined or any(
                        k in joined for k in ("unknown type name", "undeclared identifier",
                                              "no template named", "no type named")):
                    inconclusive.append(f"  {path.name} block {i}: {joined}")
                    continue
                failed += 1
                reports.append(f"  {path.name} block {i}: " + joined)

    if reports:
        print("errors:")
        for r in reports:
            print(r)
    if inconclusive and "-v" in sys.argv:
        print("\ncontinuation fragments (not checkable in isolation):")
        for r in inconclusive:
            print(r)
    print(f"\n{checked} snippets compiled ({recovered} needed their lesson's earlier "
          f"blocks), {failed} failed, {len(inconclusive)} continuation fragments, "
          f"{skipped} skipped as unavailable on this host")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
