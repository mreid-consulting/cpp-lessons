# Authoring guide

Lessons live in `content/NN-slug.md` and are compiled to `lessons/NN-slug.html` by
`build.py`. Only the constructs below are supported by the renderer.

## Front matter

Required, exactly this shape, at the very top:

```
---
title: What a Class Really Is
part: Part I - Foundations
summary: One sentence, no trailing period issues, appears under the H1 and on the index.
time: 25 min
level: beginner
tags: classes, raii, invariants
---
```

`part` must be one of, spelled exactly:

- `Part I - Foundations`
- `Part II - Modern C++23`
- `Part III - The Machine`
- `Part IV - Latency Engineering`
- `Part V - Concurrency`
- `Part VI - Trading Systems`
- `Appendix`

`level` is one of `beginner`, `intermediate`, `advanced`, `expert`.

## Markup

- `## Heading` becomes a section and appears in the on-page table of contents. Use 4 to 7
  per lesson. `###` and `####` nest under it and do not appear in the TOC.
- Paragraphs are blank-line separated. Lists use `- ` or `1. `.
- Pipe tables need a `|---|---|` separator row.
- Inline: `` `code` ``, `**bold**`, `*italic*`, `[text](url)`.
- Fenced code: ```` ```cpp ```` , ```` ```sh ```` , ```` ```text ```` , ```` ```asm ```` .
  Anything after the language on the fence line is rendered as a caption above the block:
  ```` ```cpp book.hpp ```` or ```` ```cpp The version that does not allocate ````.
- Admonitions:

```
:::key
Body markdown, including code fences.
:::
```

  Kinds: `key`, `note`, `warn`, `pitfall`, `hft`, `perf`, `exercise`, `asm`.
  An optional custom label follows the kind: `:::hft Why the desk cares`.

## House style

- Open with two or three sentences of motivation. No "In this lesson we will".
- Write for someone who has never written C++ but is technically strong. Define every
  term the first time it appears, then use it freely.
- Every claim about performance is either measured, quoted with a number, or marked as
  something to verify. Never say "faster" without saying than what and roughly how much.
- Code must compile as written under `-std=c++23 -O2` given the includes shown. Prefer
  complete snippets over fragments with `...`.
- Use trading examples throughout: order books, ticks, prices in integer ticks, feed
  handlers, ring buffers. Never `Animal`/`Dog`/`Shape`.
- Prices are `std::int64_t` ticks, never `double`. Quantities are `std::uint32_t`.
- Include at least one `:::hft` block per lesson connecting the topic to a real desk
  concern, and at least one `:::exercise`.
- Close with a `## Takeaways` section of four to six bullets.
- Comments in code state constraints and intent. No narration of the lesson's own history.
- Cross-reference by number in prose: "covered in lesson 19", never by file path.
- Target 1400 to 2200 words of prose plus 5 to 12 code blocks. Density over length:
  cut a sentence that restates the code, never a topic the brief asked for. A lesson that
  runs long because it covers what it promised is correct; one that runs long because it
  explains the same idea twice is not. Appendix reference pages have no code minimum.

## Build

```sh
$ python3 build.py && python3 -m http.server 8000
```
