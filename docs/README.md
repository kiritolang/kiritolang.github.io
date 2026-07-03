# Kirito documentation

Hand-authored Markdown in `pages/`, rendered by `build_docs.py` (stock Python 3, no dependencies)
into a single-page app in `site/`: one `index.html` shell plus a `data.js` carrying every rendered
page and a symbol index, loaded with `<script src>` so it runs straight from disk (`file://`, no
server). Documentation lives in these Markdown files — it is **not** scraped from source-code
comments, so the codebase stays uncluttered.

The site (dark theme) gives you:

- **Symbol cross-links** — every documented method/attribute/function/class/module is auto-anchored,
  and every inline `code` mention of one becomes a link to its definition, resolved by context
  (`xs.append` → `List.append`); a genuinely ambiguous bare name links into the search box instead.
- **A search box** (press <kbd>/</kbd>) that matches symbols first — by name, qualified name, or
  signature — and falls back to a full-text scan of the pages when a query has no symbol hits.
- **A persistent left nav + an on-this-page outline**; navigating swaps only the content area, so the
  nav keeps its scroll position. Old per-page URLs (`stdlib.html`) still resolve via redirect stubs.

## Build

```
python3 docs/build_docs.py     # writes docs/site/{index.html, data.js, *.html stubs}
```

Open `docs/site/index.html` in a browser (works straight from disk — no server needed).

## Extending the docs

Drop a new `NN-title.md` file in `pages/`. It appears in the sidebar automatically, ordered by its
numeric prefix; the first line (`# Title`) becomes its sidebar label and page title. The renderer
supports headings, lists, tables, blockquotes, links, inline `code`/**bold**/*italic*, and fenced
code blocks (use ` ```kirito ` for Kirito syntax highlighting).

## Pages

- **Introduction / Getting Started / Embedding / Extending** — overview, build, C++ integration.
- **Language Guide / Recipes** — syntax-and-semantics reference and task-oriented snippets.
- **Builtins / Standard Library** — every built-in function, module, and method.
- **Packages & kpm** — installing, versioning, and publishing packages.
- **Course** — a hands-on lesson path: 16 core lessons (editor setup → capstone) plus 6 bonus lessons
  for specialized libraries (regex, CLI programs, tabular data, linear algebra, tensors/autograd).
