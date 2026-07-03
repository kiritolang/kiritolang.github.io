# kirdown — Markdown → HTML in pure Kirito

A self-contained Markdown-to-HTML converter written entirely in Kirito. Covers a
solid CommonMark subset: ATX headings, paragraphs, bold/italic/inline-code, fenced
code blocks with a language class, unordered and ordered lists, blockquotes,
horizontal rules, links, images, and auto-links. Every HTML-special character is
escaped, so a Markdown document is safe to serve as-is (`<script>` in a paragraph
is rendered as literal text, not executed).

## Standard-library surface exercised

| module | how kirdown uses it |
|---|---|
| `regex`   | every block-level pattern (ATX heading, fence, bullet, ordered-list, HR, blockquote) and the auto-link inside inline formatting — all as pre-compiled patterns |
| `arg`     | full CLI: positional `input`, options `--output` / `--title`, flags `--dir` / `--body`, `-h`/`--help` |
| `path`    | `walk` / `isfile` / `splitext` / `join` / `dirname` / `mkdir` / `gettempdir` for both directory mode and the self-test fixtures |
| `io`      | `open("r")` / `open("w")` / `write` / `read` / `eprint` — real file I/O for round-trip conversion |
| `hash`    | `sha256` prefix — the CLI prints an 8-character content fingerprint after writing, so a downstream tool can log a stable digest |
| `time`    | `perfcounterns` for directory-mode wall-clock reporting |

## Layout

```
kirdown/
  main.ki                # CLI entry (uses argmain — no side effects on import)
  inline.ki              # inline formatter: bold/italic/code/links/images/escaping
  block.ki               # block parser: lines → block-node AST
  render.ki              # AST → HTML string; also a full <html> page shell
  kirdown.ki             # public facade: toBody / toPage
  test_kirdown.ki        # self-test (56 checks; matched against test_kirdown.expected)
  samples/
    index.md
    guide.md
```

All `.ki` files sit at the package root, so `ki` resolves the imports through the
script's own directory — no `--lib` flag needed.

## Usage

```
# single-file mode
ki examples/big_projects/kirdown/main.ki \
   examples/big_projects/kirdown/samples/index.md \
   -o /tmp/index.html

# directory mode: mirror a docs tree
ki examples/big_projects/kirdown/main.ki \
   -d examples/big_projects/kirdown/samples \
   -o /tmp/kirdown_out
```

The single-file mode also accepts `--body` (no `<html>/<head>` wrapper) and
`--title TITLE` (used inside `<title>` when the page shell is emitted); passing
`-o -` streams to stdout.

## Running the self-test

```
ki examples/big_projects/kirdown/test_kirdown.ki
# → kirdown: 56 checks passed
```

CTest wires this as `script_kirdown` — a fresh interpreter run whose stdout must
match `test_kirdown.expected` byte-for-byte.
