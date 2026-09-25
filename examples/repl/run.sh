#!/usr/bin/env bash
# Replays every session from lesson 01a and prints what each one produces.
#
#   ./run.sh                  run all sessions
#   CHECK=1 ./run.sh          also fail if any expected result is missing (used in CI)
#   CLANG_REPL=... ./run.sh   use a specific clang-repl binary
set -uo pipefail
cd "$(dirname "$0")"

find_repl() {
    if [[ -n "${CLANG_REPL:-}" ]]; then echo "$CLANG_REPL"; return; fi
    for c in clang-repl clang-repl-21 clang-repl-20 clang-repl-19 clang-repl-18 \
             /opt/homebrew/opt/llvm/bin/clang-repl /usr/local/opt/llvm/bin/clang-repl; do
        if command -v "$c" >/dev/null 2>&1; then command -v "$c"; return; fi
    done
}

REPL="$(find_repl)"
if [[ -z "$REPL" ]]; then
    echo "clang-repl not found. Install LLVM (brew install llvm, or apt install clang-tools-18)." >&2
    exit 1
fi

# The matching clang++ builds the shared library, so both agree on the ABI.
CXX="$(dirname "$REPL")/clang++"
[[ -x "$CXX" ]] || CXX="$(command -v "$(basename "$REPL" | sed 's/clang-repl/clang++/')" || command -v clang++)"

FLAGS=(--Xcc=-std=c++23)
if [[ "$(uname -s)" == "Darwin" ]]; then
    # LLVM's own libc++ plus the platform SDK for the C headers. Adding the SDK's
    # libc++ as well puts two standard libraries on the include path.
    FLAGS+=(--Xcc=-isysroot "--Xcc=$(xcrun --show-sdk-path)")
fi

# Replays a session with a time limit. A multi-line construct without backslash
# continuations can leave the REPL spinning, so no session is allowed to hang.
replay() {
    local label="$1" file="$2"; shift 2
    echo "=== $label ==="
    "$REPL" "${FLAGS[@]}" "$@" < "$file" > ".out.$$" 2>&1 &
    local pid=$!
    for _ in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill "$pid" 2>/dev/null; then echo "TIMED OUT after 60 s"; fi
    wait "$pid" 2>/dev/null
    grep -v -e '^<<< inputs' -e 'note:' -e '^In file included' ".out.$$" | sed '/^$/d' \
        | tee -a ".all.$$"
    rm -f ".out.$$"
    echo
}

FAILED=0
expect() {
    if ! grep -qF -- "$1" ".all.$$"; then
        echo "MISSING EXPECTED OUTPUT: $1" >&2
        FAILED=1
    fi
}

echo "using $REPL"
echo

replay "layout, persistence, redefinition, versioning, undo" layout_and_state.repl
echo "(the 'redefinition of position' error above is intentional: that input is rejected"
echo " and the session carries on)"
echo

replay "driving the capstone order book" book_session.repl --Xcc=-I../capstone

"$CXX" -std=c++23 -O2 -shared -fPIC risk.cpp -o librisk.so
replay "loading a compiled library with %lib" library.repl
rm -f librisk.so

replay "inlining: helper and loop in one included header, -O2" inline_same_tu.repl --Xcc=-O2 --Xcc=-I.
replay "inlining: helper typed as a separate input, -O2" inline_split_inputs.repl --Xcc=-O2
echo "Same code, same -O2. The second is slower because each input is its own"
echo "translation unit and the helper cannot be inlined across that boundary."

if [[ "${CHECK:-0}" == "1" ]]; then
    # Exact results only. Timings vary by machine, so only their presence is checked.
    expect "Quote: size 16, align 8, qty@8, side@12"
    expect "position 60"
    expect "position 5"
    expect "round_to_tick(-7, 5):  v1 -5   v2 -10"
    expect "levels 32"
    expect "bid 99999 x 100   ask 100002 x 250   spread 3"
    expect "after cancel: bid 99998 x 300   spread 4"
    expect "within_band: 1 0"
    expect "checksum 524539656875000"
    if grep -qF "TIMED OUT" ".all.$$"; then echo "A session timed out" >&2; FAILED=1; fi
    [[ "$FAILED" == "0" ]] && echo "all expected results present"
fi
rm -f ".all.$$"
exit "$FAILED"
