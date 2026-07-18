#!/usr/bin/env python3
"""Generate the Kirito HTML documentation site from the Markdown pages in docs/pages/.

Architecture (single-page app, works straight from disk — file://, no server):
  - One shell `index.html` (sidebar + search + content + on-this-page TOC) plus a `data.js` that
    carries every rendered page and the symbol index, loaded via <script src> (NOT fetch, so it
    runs from a file:// URL). The client router swaps the content area on navigation, so the left
    nav is never re-rendered and its scroll position is preserved.
  - Symbol cross-linking: every documented method/attribute/function/class/module is indexed with a
    QUALIFIED anchor (`sym-List.append`, `sym-io.print`), so same-named members on different types
    no longer collide. Every inline `code` mention of a symbol becomes a link to its definition,
    resolved by context (a `recv.method` / `Owner.method` / the current section's owner); a name that
    is genuinely ambiguous links into the search panel pre-filled with that name.
  - Search: a single-word query fuzzy-matches symbols (name / qualified / signature, with a
    subsequence fallback for typos) AND, alongside, a section-level full-text scan; a multi-word query
    is treated as prose (AND across terms) and searches the text. Text hits deep-link to the nearest
    section heading and are ranked by term frequency + heading matches.
  - Zero dependencies: a small self-contained Markdown renderer; stock Python 3. Dark theme only.

Documentation content is authored in the .md files, NOT scraped from source-code comments.

Usage:  python3 docs/build_docs.py   ->   writes docs/site/{index.html,data.js,*.html stubs}
"""
import html
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PAGES_DIR = os.path.join(HERE, "pages")
OUT_DIR = os.path.join(HERE, "site")

# Built-in value types — a fixed set, used to tell a type "owner" heading (## List) apart from a
# category heading that merely happens to be one word (## Numbers / ## Notes on the builtins page).
TYPES = {"None", "Bool", "Integer", "Float", "String", "Bytes", "List", "Set", "Dict", "Array", "Number"}
# A dotted token whose suffix is one of these is a FILENAME (analyzer.hpp, kirito.hpp), not an
# `owner.member` symbol — never index or link it as such (its "member" would collide, e.g. every
# `*.hpp` becoming a bogus `hpp` member that mis-resolves).
_FILE_EXTS = {"hpp", "cpp", "h", "ki", "py", "sh", "ps1", "md", "json", "js", "html", "xml", "yml",
              "yaml", "cmake", "txt", "gch", "gz", "so", "a", "o", "in", "expected", "experr", "args"}

# Kirito keywords / builtins for lightweight highlighting of ```kirito fences.
KW = set("var Function if elif else while for in break continue return and or not "
         "class try catch finally throw with as pass todo assert discard switch case default "
         "True False None".split())
BUILTINS = set("print input len range sum min max abs round sorted enumerate zip map filter type "
               "inspect all any reversed divmod isinstance ord chr bin oct hex pow import Integer Float "
               "bitand bitor bitxor bitnot shl shr String Bool List Set Dict".split())


def highlight_kirito(code):
    out = []
    for line in code.split("\n"):
        m = re.match(r"^(.*?)(#.*)$", line)
        comment = ""
        if m and "\"" not in m.group(2):
            line, comment = m.group(1), m.group(2)
        parts = re.split(r'("(?:[^"\\]|\\.)*")', line)
        rendered = ""
        for i, part in enumerate(parts):
            if i % 2 == 1:
                rendered += f'<span class="str">{html.escape(part)}</span>'
            else:
                def tok(mt):
                    w = mt.group(0)
                    if w in KW:
                        return f'<span class="kw">{w}</span>'
                    if w in BUILTINS:
                        return f'<span class="builtin">{w}</span>'
                    return w
                esc = html.escape(part)
                esc = re.sub(r"[A-Za-z_][A-Za-z0-9_]*", tok, esc)
                esc = re.sub(r"\b\d+\.?\d*\b", lambda m: f'<span class="num">{m.group(0)}</span>', esc)
                rendered += esc
        if comment:
            rendered += f'<span class="com">{html.escape(comment)}</span>'
        out.append(rendered)
    return "\n".join(out)


# --- symbol model -----------------------------------------------------------------------------
QUALIFIED = {}     # "List.append" -> {"slug","anchor","sig","owner","name","kind"}
BY_NAME = {}       # "append" -> [qualified keys]
HEADING_SYMS = {}  # "List"/"io" -> {"slug","anchor","kind"} — the CANONICAL (shallowest) owner section
_HEADING_LEVEL = {}  # "String" -> the heading level its HEADING_SYMS entry came from (shallowest wins)
HEADING_SYMS_ALL = {}  # "List" -> [ {"slug","anchor","kind"}, ... ] — EVERY page that heads this name, so
                       # a bare `List` on the C++ API page links to that page's own `List` section rather
                       # than the canonical (Kirito-type) one. Same-page candidate wins in _resolve_link.
OWNERS = set()     # names that own members (modules + types + native objects)
_EMITTED = set()   # (slug, anchor) ids already emitted this render, to avoid dup ids
_CUR_SLUG = ""     # page slug currently rendering (for owner reset + ids)
_CUR_OWNER = None  # effective owner (type/module) of the section currently rendering
_CUR_MODULE = None # module scope (## lowercase heading)
_CUR_OBJ = None    # object scope (### X object / ### X methods) within the module


def _slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def _lead_ident(code_text):
    m = re.match(r"\s*`?([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)", code_text)
    return m.group(1) if m else None


# Owner tracking is two-level: a `## module` (lowercase) sets the MODULE scope; a `### X object` /
# `### X methods` sets the OBJECT scope within it; any other `###` subsection (e.g. "### URL helpers",
# "### Linear algebra") reverts to module scope, so a module function listed after an object section
# is attributed to the module, not the object. The effective owner is the object scope if set, else
# the module. A level-2 type heading (## List) is itself the object scope (no module).
def _owner_step(module, objowner, level, htext):
    t = htext.strip().strip("`")
    mo = re.match(r"^([A-Za-z_]\w*)\s+(?:object|methods)$", t)
    if mo and mo.group(1) in OWNERS:
        return (module, mo.group(1))
    bare = bool(re.fullmatch(r"[A-Za-z_]\w*", t))
    if level <= 2:
        if bare and t in OWNERS:
            return (t, None) if t[:1].islower() else (None, t)
        return (None, None)
    if bare and t in OWNERS:
        return (module, t)
    return (module, None)


def _eff_owner(module, objowner):
    return objowner or module


def _collect_owners(pages):
    # Pass 1: discover the owner names (modules / types / native-object sections) from headings.
    for slug, _, text in pages:
        for line in text.split("\n"):
            h = re.match(r"^(#{1,4})\s+(.*)$", line)
            if not h:
                continue
            level, t = len(h.group(1)), h.group(2).strip().strip("`")
            mo = re.match(r"^([A-Za-z_]\w*)\s+(?:object|methods)$", t)
            if mo:
                OWNERS.add(mo.group(1))
                HEADING_SYMS.setdefault(mo.group(1), {"slug": slug, "anchor": _slug(t), "kind": "type"})
                HEADING_SYMS_ALL.setdefault(mo.group(1), []).append({"slug": slug, "anchor": _slug(t), "kind": "type"})
            elif re.fullmatch(r"[A-Za-z_]\w*", t):
                # a lowercase single-word level-2 heading is a module; a known builtin type is a type.
                # A name can head a section on MORE THAN ONE page — e.g. `## String` in the types
                # reference AND `#### String` in the C++ API (the C++ wrapper). A bare mention must link
                # to the CANONICAL entry (SHALLOWEST heading level, first page breaking a tie — so the
                # type reference's `## String` at level 2 wins over a `#### String` deep in another page)
                # UNLESS the mention is ON a page that has its own section for that name, in which case the
                # same-page section wins (HEADING_SYMS_ALL + _resolve_link) — so `List` in the C++ API
                # reference links to the C++ wrapper, not the Kirito type.
                kind = "module" if (level == 2 and t[:1].islower()) else ("type" if t in TYPES else None)
                if kind:
                    HEADING_SYMS_ALL.setdefault(t, []).append({"slug": slug, "anchor": _slug(t), "kind": kind})
                if kind and level < _HEADING_LEVEL.get(t, 99):
                    OWNERS.add(t)
                    HEADING_SYMS[t] = {"slug": slug, "anchor": _slug(t), "kind": kind}
                    _HEADING_LEVEL[t] = level
                elif kind:
                    OWNERS.add(t)   # still an owner (for member attribution), just not the link target


def _classify_def(first_cell, owner):
    # A table-row / list-item first cell `code` that DEFINES a symbol -> (qualified, anchor, name, kind, sig).
    if not first_cell.lstrip().startswith("`"):
        return None
    lead = _lead_ident(first_cell)
    if not lead:
        return None
    ms = re.match(r"\s*`([^`]+)`", first_cell)   # the signature is the LEADING code span only, not the
    sig = ms.group(1).strip() if ms else lead    # whole list-item line (which carries a description)
    has_call = "(" in sig
    if "." in lead:
        head, member = lead.split(".", 1)
        member = member.split(".")[0]
        if member in _FILE_EXTS:                 # `analyzer.hpp` etc. is a filename, not a symbol
            return None
        if head in OWNERS:                       # io.print / math.gcd / tensor.dot
            q = head + "." + member
            kind = "function" if HEADING_SYMS.get(head, {}).get("kind") == "module" else "method"
            return (q, "sym-" + q, member, kind, sig)
        if owner:                                 # xs.append -> the section's type
            q = owner + "." + member
            return (q, "sym-" + q, member, "method", sig)
        return None
    if owner:                                     # a bare member under a module/type section
        q = owner + "." + lead
        kind = "method" if owner in TYPES or HEADING_SYMS.get(owner, {}).get("kind") == "type" else "function"
        return (q, "sym-" + q, lead, kind, sig)
    return (lead, "sym-" + lead, lead, "function" if has_call else "symbol", sig)  # flat builtin


def _iter_def_cells(text):
    # Yield (owner, first_cell) for every table row / list item that could define a symbol, tracking
    # the section owner exactly as the renderer does.
    module = objowner = None
    in_fence = False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        h = re.match(r"^(#{1,4})\s+(.*)$", line)
        if h:
            module, objowner = _owner_step(module, objowner, len(h.group(1)), h.group(2))
            continue
        owner = _eff_owner(module, objowner)
        cell = None
        if line.lstrip().startswith("|") and "|" in line.strip().strip("|"):
            cell = line.strip().strip("|").split("|")[0].strip()
        elif re.match(r"^\s*[-*]\s+", line):
            cell = re.sub(r"^\s*[-*]\s+", "", line).lstrip("*").strip()
        if cell:
            yield owner, cell


def collect_symbols(pages):
    _collect_owners(pages)
    # Pass 2: index every defined member with its qualified anchor. A qualified name defined twice is
    # kept once (first wins) — qualification makes real collisions rare; genuine bare-name ambiguity
    # is handled at link time (multiple owners -> link into search).
    for slug, _, text in pages:
        for owner, cell in _iter_def_cells(text):
            d = _classify_def(cell, owner)
            if not d:
                continue
            q, anchor, name, kind, sig = d
            if q in QUALIFIED:
                continue
            QUALIFIED[q] = {"slug": slug, "anchor": anchor, "sig": sig, "owner": owner or "", "name": name, "kind": kind}
            BY_NAME.setdefault(name, [])
            if q not in BY_NAME[name]:
                BY_NAME[name].append(q)


def _row_anchor(first_cell, owner):
    # The anchor id for a row/list item that DEFINES a symbol on the current page (first time only).
    d = _classify_def(first_cell, owner)
    if not d:
        return ""
    q, anchor = d[0], d[1]
    entry = QUALIFIED.get(q)
    if not entry or entry["slug"] != _CUR_SLUG or entry["anchor"] != anchor or (_CUR_SLUG, anchor) in _EMITTED:
        return ""
    _EMITTED.add((_CUR_SLUG, anchor))
    return anchor


def _anchor_first_code(cell_html, anchor):
    return cell_html.replace("<code>", f'<code id="{anchor}">', 1) if anchor else cell_html


def _resolve_link(code_text):
    # Resolve an inline `code` mention to (slug, anchor) or ("__search__", name), or None.
    m = re.match(r"\s*([A-Za-z_]\w*)(?:\.([A-Za-z_]\w*))?", code_text)
    if not m:
        return None
    head, member = m.group(1), m.group(2)
    if member is not None:
        if member in _FILE_EXTS:                                          # `analyzer.hpp` — a filename, not a link
            return None
        if head in OWNERS and (head + "." + member) in QUALIFIED:        # Owner.member (io.print, List.append)
            e = QUALIFIED[head + "." + member]
            return (e["slug"], e["anchor"])
        cands = BY_NAME.get(member)                                       # recv.member (xs.append) -> by member
        if cands:
            ctx = [q for q in cands if QUALIFIED[q]["owner"] == _CUR_OWNER]
            if len(cands) == 1 or len(ctx) == 1:
                e = QUALIFIED[(ctx or cands)[0]]
                return (e["slug"], e["anchor"])
            return ("__search__", member)
        return None
    # bare name -> a type / module / native-object section. When the name heads a section on MORE THAN
    # one page (e.g. `List` in the Kirito type reference AND in the C++ API), a mention ON one of those
    # pages links to THAT page's section (so `List` in the C++ reference points at the C++ wrapper);
    # anywhere else it falls back to the canonical (shallowest) section.
    if head in HEADING_SYMS:
        for c in HEADING_SYMS_ALL.get(head, ()):
            if c["slug"] == _CUR_SLUG:
                return (c["slug"], c["anchor"])
        e = HEADING_SYMS[head]
        return (e["slug"], e["anchor"])
    cands = BY_NAME.get(head)
    if not cands:
        return None
    ctx = [q for q in cands if QUALIFIED[q]["owner"] == _CUR_OWNER]
    if len(cands) == 1 or len(ctx) == 1:
        e = QUALIFIED[(ctx or cands)[0]]
        return (e["slug"], e["anchor"])
    return ("__search__", head)


def rewrite_internal_links(body, slugs):
    # Explicit `[text](page.html#anchor)` links authored in the Markdown must route inside the SPA
    # (the per-page .html files are now redirect stubs that would drop the #anchor). Rewrite them to
    # the hash form the client router understands: `#slug::anchor`.
    def r(m):
        slug, anchor = m.group(1), m.group(2)
        if slug == "index":
            return 'href="#"'
        if slug in slugs:
            return 'href="#' + slug + ("::" + anchor[1:] if anchor else "") + '"'
        return m.group(0)
    body = re.sub(r'href="([a-z0-9-]+)\.html(#[^"]*)?"', r, body)
    # A bare same-page fragment link authored as [text](#anchor) becomes href="#anchor", but the SPA
    # router reads a bare `#anchor` as a PAGE slug (falling back to the intro). Qualify it with the
    # current page: `#anchor` -> `#<slug>::anchor`. Auto-links already carry `slug::` (the `:` keeps
    # them from matching), and `#` / `#search=…` are left alone. A fragment whose text IS a page slug
    # (produced by the `page.html` rewrite above for a link with NO explicit anchor) is a cross-page
    # navigation, not a same-page fragment — skip it so it stays a valid page-select link.
    def qualify(m):
        anchor = m.group(1)
        if anchor in slugs:
            return m.group(0)
        return 'href="#' + _CUR_SLUG + "::" + anchor + '"'
    body = re.sub(r'href="#([a-z0-9][a-z0-9-]*)"', qualify, body)
    return body


def linkify(body):
    def repl(m):
        target = _resolve_link(m.group(1))
        if not target:
            return m.group(0)
        slug, anchor = target
        if slug == "__search__":
            return f'<a class="xref ambig" href="#search={anchor}"><code>{m.group(1)}</code></a>'
        return f'<a class="xref" href="#{slug}::{anchor}"><code>{m.group(1)}</code></a>'
    # Skip fenced blocks (<pre>) and text already inside an <a>, so definitions (which carry a
    # <code id=…>) and existing links are never rewritten.
    parts = re.split(r"(<pre>.*?</pre>|<a\b[^>]*>.*?</a>)", body, flags=re.S)
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"<code>([^<]+)</code>", repl, parts[i])
    return "".join(parts)


def render_inline(text):
    text = html.escape(text)
    spans = []

    def stash(m):
        spans.append(m.group(1))
        return f"\x00{len(spans) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)
    text = re.sub(r"\x00(\d+)\x00", lambda m: f"<code>{spans[int(m.group(1))]}</code>", text)
    return text


def _is_block_start(s):
    return bool(re.match(r"^\s*[-*]\s+", s) or re.match(r"^\s*\d+\.\s+", s)
                or s.startswith("#") or s.startswith(">")
                or s.lstrip().startswith("```") or s.strip() == "")


def render_markdown(md):
    global _CUR_OWNER, _CUR_MODULE, _CUR_OBJ
    _CUR_OWNER = _CUR_MODULE = _CUR_OBJ = None
    lines = md.split("\n")
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.lstrip()
        if stripped.startswith("```"):
            indent = len(line) - len(stripped)
            lang = stripped[3:].strip()
            i += 1
            buf = []
            while i < n and not lines[i].lstrip().startswith("```"):
                row = lines[i]
                k = 0
                while k < indent and k < len(row) and row[k] == " ":
                    k += 1
                buf.append(row[k:])
                i += 1
            i += 1
            code = "\n".join(buf)
            inner = highlight_kirito(code) if lang in ("kirito", "ki") else html.escape(code)
            # Wrap every code block so the client can drop in a "Copy code" button (see the CSS/JS
            # in the page shell). The <pre>…</pre> stays intact so the xref rewriter still skips it.
            out.append(
                '<div class="codeblock">'
                '<button class="copy-btn" type="button" aria-label="Copy code">Copy</button>'
                f"<pre><code>{inner}</code></pre></div>"
            )
            continue
        if line.lstrip().startswith("<!--"):
            while i < n and "-->" not in lines[i]:
                i += 1
            i += 1
            continue
        m = re.match(r"^(#{1,4})\s+(.*)$", line)
        if m:
            level = len(m.group(1))
            text = render_inline(m.group(2))
            anchor = re.sub(r"[^a-z0-9]+", "-", m.group(2).lower()).strip("-")
            _CUR_MODULE, _CUR_OBJ = _owner_step(_CUR_MODULE, _CUR_OBJ, level, m.group(2))
            _CUR_OWNER = _eff_owner(_CUR_MODULE, _CUR_OBJ)
            out.append(f'<h{level} id="{anchor}">{text}</h{level}>')
            i += 1
            continue
        if "|" in line and i + 1 < n and re.match(r"^\s*\|?[\s:|-]+\|?\s*$", lines[i + 1]) and "-" in lines[i + 1]:
            header = [c.strip() for c in line.strip().strip("|").split("|")]
            i += 2
            rows = []
            while i < n and "|" in lines[i]:
                rows.append([c.strip() for c in lines[i].strip().strip("|").split("|")])
                i += 1
            t = "<table><thead><tr>" + "".join(f"<th>{render_inline(h)}</th>" for h in header) + "</tr></thead><tbody>"
            for r in rows:
                anchor = _row_anchor(r[0], _CUR_OWNER) if r else ""
                cells = [render_inline(c) for c in r]
                if anchor and cells:
                    cells[0] = _anchor_first_code(cells[0], anchor)
                t += "<tr>" + "".join(f"<td>{c}</td>" for c in cells) + "</tr>"
            t += "</tbody></table>"
            out.append(t)
            continue
        if line.startswith(">"):
            buf = []
            while i < n and lines[i].startswith(">"):
                buf.append(lines[i][1:].strip())
                i += 1
            out.append(f"<blockquote>{render_inline(' '.join(buf))}</blockquote>")
            continue
        if re.match(r"^\s*[-*]\s+", line):
            buf = []
            while i < n and (re.match(r"^\s*[-*]\s+", lines[i])
                             or (buf and not _is_block_start(lines[i]))):
                if re.match(r"^\s*[-*]\s+", lines[i]):
                    buf.append(re.sub(r"^\s*[-*]\s+", "", lines[i]))
                else:
                    buf[-1] = buf[-1] + " " + lines[i].strip()
                i += 1
            items = []
            for x in buf:
                anchor = _row_anchor(x.lstrip("*"), _CUR_OWNER) if x.lstrip("*").lstrip().startswith("`") else ""
                items.append(f"<li>{_anchor_first_code(render_inline(x), anchor)}</li>")
            out.append("<ul>" + "".join(items) + "</ul>")
            continue
        if re.match(r"^\s*\d+\.\s+", line):
            buf = []
            while i < n and (re.match(r"^\s*\d+\.\s+", lines[i])
                             or (buf and not _is_block_start(lines[i]))):
                if re.match(r"^\s*\d+\.\s+", lines[i]):
                    buf.append(re.sub(r"^\s*\d+\.\s+", "", lines[i]))
                else:
                    buf[-1] = buf[-1] + " " + lines[i].strip()
                i += 1
            out.append("<ol>" + "".join(f"<li>{render_inline(x)}</li>" for x in buf) + "</ol>")
            continue
        if line.strip() == "":
            i += 1
            continue
        if line.strip() == "---":
            out.append("<hr>")
            i += 1
            continue
        buf = [line]
        i += 1
        while i < n and lines[i].strip() and not re.match(r"^(#{1,4}\s|```|>|\s*[-*]\s|\s*\d+\.\s)", lines[i]) \
                and not (lines[i].strip() == "---"):
            buf.append(lines[i])
            i += 1
        out.append(f"<p>{render_inline(' '.join(buf))}</p>")
    return "\n".join(out)


# --- page grouping for the sidebar ------------------------------------------------------------
def _group_of(slug):
    if slug.startswith("course-"):
        return "Course"
    if slug.startswith("bonus-"):
        return "Bonus lessons"
    if slug in ("builtins", "types", "stdlib", "cpp-api", "exceptions"):
        return "Reference"
    return "Guide"


GROUP_ORDER = ["Guide", "Reference", "Course", "Bonus lessons"]


def _toc(body_html):
    # On-this-page entries from the h2/h3 ids already emitted into the page body.
    toc = []
    for level, anchor, inner in re.findall(r'<h([23]) id="([^"]+)">(.*?)</h\1>', body_html, flags=re.S):
        label = re.sub(r"<[^>]+>", "", inner)
        toc.append({"level": int(level), "anchor": anchor, "label": html.unescape(label)})
    return toc


def main():
    files = sorted(f for f in os.listdir(PAGES_DIR) if f.endswith(".md"))
    if not files:
        sys.exit("no pages found in " + PAGES_DIR)
    os.makedirs(OUT_DIR, exist_ok=True)

    raw = []
    for f in files:
        with open(os.path.join(PAGES_DIR, f), encoding="utf-8") as fh:
            text = fh.read()
        title = text.split("\n", 1)[0].lstrip("# ").strip()
        slug = re.sub(r"^\d+-", "", f[:-3])
        raw.append((slug, title, text))

    collect_symbols(raw)

    global _CUR_SLUG, _EMITTED
    slugs = {slug for slug, _, _ in raw}
    pages_data = []
    for slug, title, text in raw:
        _CUR_SLUG = slug
        _EMITTED = set()
        body = rewrite_internal_links(linkify(render_markdown(text)), slugs)
        pages_data.append({"slug": slug, "title": title, "group": _group_of(slug),
                           "html": body, "toc": _toc(body)})

    # symbol index for search (sorted: shorter/lower names first within a name)
    symbols = []
    for q, e in sorted(QUALIFIED.items()):
        symbols.append({"q": q, "name": e["name"], "owner": e["owner"], "kind": e["kind"],
                        "sig": e["sig"], "slug": e["slug"], "anchor": e["anchor"]})
    for name, e in sorted(HEADING_SYMS.items()):
        symbols.append({"q": name, "name": name, "owner": "", "kind": e["kind"],
                        "sig": name, "slug": e["slug"], "anchor": e["anchor"]})

    nav = [{"slug": p["slug"], "title": p["title"], "group": p["group"]} for p in pages_data]
    data = {"pages": pages_data, "symbols": symbols, "nav": nav, "groups": GROUP_ORDER}
    with open(os.path.join(OUT_DIR, "data.js"), "w", encoding="utf-8") as fh:
        fh.write("window.KIRITO=" + json.dumps(data, ensure_ascii=False) + ";\n")

    with open(os.path.join(OUT_DIR, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(SHELL)

    # Per-page stubs so old deep links (stdlib.html, …) still resolve into the SPA.
    for p in pages_data:
        stub = ('<!DOCTYPE html><meta charset="utf-8">'
                f'<meta http-equiv="refresh" content="0; url=index.html#{p["slug"]}">'
                f'<a href="index.html#{p["slug"]}">Kirito documentation — {html.escape(p["title"])}</a>')
        with open(os.path.join(OUT_DIR, p["slug"] + ".html"), "w", encoding="utf-8") as fh:
            fh.write(stub)

    print(f"wrote index.html + data.js + {len(pages_data)} stubs to {OUT_DIR} "
          f"({len(symbols)} symbols indexed)")


# --- the SPA shell (CSS + markup + app JS), dark theme only -----------------------------------
CSS = r"""
:root{
  --bg:#0f1014;--panel:#16171d;--panel2:#1b1d24;--fg:#e6e6ee;--muted:#9aa0ad;--faint:#6b7280;
  --accent:#a78bfa;--accent2:#7c5cff;--border:#262833;--code-bg:#1b1d24;--sel:#2a2438;
  --kw:#c89bff;--str:#9bdc8a;--com:#6b7280;--num:#e0a06a;--builtin:#6cb6ff;
}
*{box-sizing:border-box}
/* Theme the scrollbars: a transparent track/corner so the default light scrollbar gutter can't
   show through the rounded corners of scrollable code blocks/tables as stray white dots. */
*{scrollbar-width:thin;scrollbar-color:var(--border) transparent}
::-webkit-scrollbar{width:10px;height:10px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--border);border-radius:8px}
::-webkit-scrollbar-thumb:hover{background:var(--muted)}
::-webkit-scrollbar-corner{background:transparent}
html,body{margin:0;height:100%}
body{font:15.5px/1.65 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  color:var(--fg);background:var(--bg);-webkit-font-smoothing:antialiased}
a{color:var(--accent);text-decoration:none}
a:hover{text-decoration:underline}
::selection{background:var(--sel)}

#app{display:grid;grid-template-columns:288px minmax(0,1fr) 232px;min-height:100vh}
@media(max-width:1100px){#app{grid-template-columns:288px minmax(0,1fr)} #toc{display:none}}
@media(max-width:760px){#app{grid-template-columns:1fr} #sidebar{position:fixed;z-index:30;transform:translateX(-100%);transition:transform .2s} #sidebar.open{transform:none} #menu-btn{display:flex}}

/* sidebar */
#sidebar{background:var(--panel);border-right:1px solid var(--border);position:sticky;top:0;
  height:100vh;overflow:hidden;display:flex;flex-direction:column}
.brand{padding:20px 20px 14px;border-bottom:1px solid var(--border)}
.brand .name{font-size:19px;font-weight:700;letter-spacing:.2px}
.brand .name b{color:var(--accent)}
.brand .tag{color:var(--muted);font-size:12.5px;margin-top:2px}
.search-wrap{padding:12px 14px 8px}
#search{width:100%;background:var(--panel2);border:1px solid var(--border);color:var(--fg);
  border-radius:9px;padding:9px 11px;font-size:14px;outline:none}
#search:focus{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(124,92,255,.15)}
.search-hint{color:var(--faint);font-size:11px;padding:3px 4px 0}
.search-hint kbd{background:var(--panel2);border:1px solid var(--border);border-radius:4px;padding:0 5px;font-size:10.5px}
#nav{flex:1;overflow-y:auto;padding:8px 10px 24px}
.nav-group{color:var(--faint);text-transform:uppercase;letter-spacing:.08em;font-size:11px;
  font-weight:700;margin:16px 10px 5px}
#nav a{display:block;color:var(--fg);padding:6px 11px;border-radius:8px;font-size:14px;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#nav a:hover{background:var(--panel2);text-decoration:none}
#nav a.active{background:linear-gradient(90deg,var(--accent2),#9b7bff);color:#fff;font-weight:600}

/* content */
#main{min-width:0;padding:46px 56px 120px;max-width:900px;margin:0 auto;width:100%}
#content h1,#content h2,#content h3,#content h4{line-height:1.25;font-weight:680;scroll-margin-top:20px}
#content h1{font-size:32px;margin:.1em 0 .7em;padding-bottom:.32em;border-bottom:1px solid var(--border)}
#content h2{font-size:23px;margin:1.8em 0 .55em;padding-top:.2em}
#content h3{font-size:18px;margin:1.5em 0 .45em}
#content h4{font-size:15.5px;margin:1.3em 0 .4em;color:var(--muted)}
#content p{margin:.7em 0}
#content code{background:var(--code-bg);padding:2px 6px;border-radius:5px;font-size:.85em;
  font-family:"SF Mono",SFMono-Regular,Consolas,"Liberation Mono",Menlo,monospace}
#content pre{background:var(--code-bg);padding:15px 17px;border-radius:11px;overflow-x:auto;
  border:1px solid var(--border);margin:0}
#content pre code{background:none;padding:0;font-size:13px;line-height:1.55}
/* code block + copy button */
#content .codeblock{position:relative;margin:1em 0}
#content .codeblock .copy-btn{position:absolute;top:8px;right:8px;z-index:2;padding:4px 10px;
  font:600 12px/1 inherit;color:var(--fg);background:var(--panel2);border:1px solid var(--border);
  border-radius:7px;cursor:pointer;opacity:0;transition:opacity .12s,background .12s}
#content .codeblock:hover .copy-btn,#content .copy-btn:focus{opacity:1}
#content .copy-btn:hover{background:var(--border)}
#content .copy-btn.copied{color:#fff;background:#2ea043;border-color:#2ea043;opacity:1}
#content table{border-collapse:collapse;width:100%;margin:1.1em 0;font-size:14px;display:block;overflow-x:auto}
#content th,#content td{border:1px solid var(--border);padding:8px 12px;text-align:left;vertical-align:top}
#content th{background:var(--panel2);font-weight:650}
#content tr:nth-child(even) td{background:rgba(255,255,255,.015)}
#content blockquote{margin:1.1em 0;padding:.5em 1.1em;border-left:3px solid var(--accent);
  background:var(--panel2);border-radius:0 9px 9px 0;color:var(--fg)}
#content hr{border:none;border-top:1px solid var(--border);margin:2em 0}
.kw{color:var(--kw)}.str{color:var(--str)}.com{color:var(--com);font-style:italic}
.num{color:var(--num)}.builtin{color:var(--builtin)}
a.xref{text-decoration:none}
a.xref:hover{text-decoration:none}
a.xref code{color:var(--accent);background:rgba(124,92,255,.10)}
a.xref.ambig code{color:var(--muted)}
:target{animation:flash 1.4s ease}
@keyframes flash{0%,40%{background:rgba(124,92,255,.22)}100%{background:transparent}}
.note{font-size:12.5px;color:var(--faint);margin-top:54px;border-top:1px solid var(--border);padding-top:14px}

/* on-this-page TOC */
#toc{position:sticky;top:0;height:100vh;overflow-y:auto;padding:46px 18px;font-size:13px;border-left:1px solid var(--border)}
#toc .toc-title{color:var(--faint);text-transform:uppercase;letter-spacing:.08em;font-size:10.5px;font-weight:700;margin-bottom:8px}
#toc a{display:block;color:var(--muted);padding:3px 0 3px 10px;border-left:2px solid transparent;line-height:1.4}
#toc a.lvl3{padding-left:22px;font-size:12.5px}
#toc a:hover{color:var(--fg);text-decoration:none}
#toc a.active{color:var(--accent);border-left-color:var(--accent)}

/* search results overlay */
#results{display:none;position:absolute;left:14px;right:14px;top:108px;z-index:40;background:var(--panel2);
  border:1px solid var(--border);border-radius:11px;box-shadow:0 18px 50px rgba(0,0,0,.5);
  max-height:70vh;overflow-y:auto;padding:6px}
#results.show{display:block}
.res{display:block;padding:8px 11px;border-radius:8px;color:var(--fg)}
.res:hover,.res.sel{background:var(--sel);text-decoration:none}
.res .sig{font-family:"SF Mono",SFMono-Regular,Consolas,Menlo,monospace;font-size:13px;color:var(--fg)}
.res .sig b{color:var(--accent2);font-weight:700}
.res .meta{font-size:11.5px;color:var(--muted);margin-top:1px}
.res .meta .crumb{color:var(--faint)}
.res .kindtag{font-size:10px;text-transform:uppercase;letter-spacing:.05em;color:var(--accent);
  border:1px solid var(--border);border-radius:4px;padding:0 5px;margin-left:6px}
.res .snip{font-size:12.5px;color:var(--muted);margin-top:2px}
.res .snip b{color:var(--fg);font-weight:600;background:rgba(124,92,255,.16);border-radius:3px}
.res-group{color:var(--faint);text-transform:uppercase;letter-spacing:.07em;font-size:10.5px;
  font-weight:700;padding:8px 11px 4px}
.res-empty{padding:14px 12px;color:var(--muted);font-size:13.5px}

#menu-btn{display:none;position:fixed;top:12px;left:12px;z-index:50;width:40px;height:40px;
  align-items:center;justify-content:center;background:var(--panel);border:1px solid var(--border);
  border-radius:9px;color:var(--fg);cursor:pointer;font-size:18px}
#scrim{display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:20}
#scrim.show{display:block}
"""

APP_JS = r"""
(function(){
  var D = window.KIRITO || {pages:[],symbols:[],nav:[],groups:[]};
  var pageBySlug = {}; D.pages.forEach(function(p){ pageBySlug[p.slug]=p; });
  var $ = function(s,r){ return (r||document).querySelector(s); };
  var content = $("#content"), nav = $("#nav"), toc = $("#toc"), results = $("#results"),
      search = $("#search"), sidebar = $("#sidebar"), scrim = $("#scrim");

  // ---- sidebar nav (built ONCE; never re-rendered, so its scroll is preserved) ----
  (function buildNav(){
    var byGroup = {}; D.nav.forEach(function(n){ (byGroup[n.group]=byGroup[n.group]||[]).push(n); });
    var hgroups = D.groups.filter(function(g){ return byGroup[g]; });
    Object.keys(byGroup).forEach(function(g){ if(hgroups.indexOf(g)<0) hgroups.push(g); });
    var h="";
    hgroups.forEach(function(g){
      h+='<div class="nav-group">'+esc(g)+'</div>';
      byGroup[g].forEach(function(n){ h+='<a data-slug="'+n.slug+'" href="#'+n.slug+'">'+esc(n.title)+'</a>'; });
    });
    nav.innerHTML=h;
  })();
  var navLinks = {};
  Array.prototype.forEach.call(nav.querySelectorAll("a"), function(a){ navLinks[a.getAttribute("data-slug")]=a; });

  function esc(s){ return s.replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];}); }

  // ---- routing ----
  var curSlug=null, tocLinks=[], headings=[];
  function render(slug, anchor){
    var p = pageBySlug[slug] || D.pages[0];
    if(p.slug!==curSlug){
      content.innerHTML = p.html + '<p class="note">Kirito documentation — generated from docs/pages/.</p>';
      curSlug = p.slug;
      document.title = p.title + " — Kirito";
      Object.keys(navLinks).forEach(function(s){ navLinks[s].classList.toggle("active", s===p.slug); });
      buildToc(p);
      content.scrollTop = 0;
      window.scrollTo(0,0);
    }
    if(anchor){
      var el = document.getElementById(anchor);
      if(el){ el.scrollIntoView(); el.id && history.replaceState(null,"",location.pathname+location.search+"#"+p.slug+"::"+anchor); }
    } else {
      window.scrollTo(0,0);
    }
    closeSidebar();
  }
  function buildToc(p){
    if(!p.toc || !p.toc.length){ toc.innerHTML=""; tocLinks=[]; headings=[]; return; }
    var h='<div class="toc-title">On this page</div>';
    p.toc.forEach(function(t){ h+='<a class="'+(t.level===3?'lvl3':'')+'" href="#'+p.slug+'::'+t.anchor+'" data-anchor="'+t.anchor+'">'+esc(t.label)+'</a>'; });
    toc.innerHTML=h;
    tocLinks = Array.prototype.slice.call(toc.querySelectorAll("a"));
    headings = p.toc.map(function(t){ return t.anchor; });
  }
  function go(hash){
    hash = (hash||"").replace(/^#/,"");
    if(hash.indexOf("search=")===0){ openSearch(decodeURIComponent(hash.slice(7))); return; }
    var parts = hash.split("::"); var slug = parts[0]||D.pages[0].slug; var anchor = parts[1]||"";
    if(!pageBySlug[slug] && D.pages[0]){ slug=D.pages[0].slug; }
    render(slug, anchor);
  }
  window.addEventListener("hashchange", function(){ go(location.hash); });

  // intercept in-page anchor clicks so cross-page xrefs route without a reload
  content.addEventListener("click", function(e){
    // "Copy code" button: copy the sibling <pre>'s text, flash a confirmation.
    var btn = e.target.closest && e.target.closest(".copy-btn");
    if(btn){
      var pre = btn.parentElement.querySelector("pre");
      var text = pre ? pre.innerText : "";
      var done = function(){ btn.classList.add("copied"); var t=btn.textContent; btn.textContent="Copied!";
        setTimeout(function(){ btn.classList.remove("copied"); btn.textContent="Copy"; }, 1400); };
      if(navigator.clipboard && navigator.clipboard.writeText){ navigator.clipboard.writeText(text).then(done, done); }
      else { try{ var ta=document.createElement("textarea"); ta.value=text; document.body.appendChild(ta);
        ta.select(); document.execCommand("copy"); document.body.removeChild(ta); done(); }catch(_){} }
      return;
    }
    var a = e.target.closest && e.target.closest("a"); if(!a) return;
    var href = a.getAttribute("href")||"";
    if(href.charAt(0)==="#"){ e.preventDefault(); if(("#"+location.hash.replace(/^#/,""))===href){ go(href);} else { location.hash=href; } }
  });

  // ---- scroll-spy for the TOC ----
  window.addEventListener("scroll", function(){
    if(!headings.length) return;
    var best=null, bestTop=-1e9;
    for(var i=0;i<headings.length;i++){
      var el=document.getElementById(headings[i]); if(!el) continue;
      var top=el.getBoundingClientRect().top-90;
      if(top<=0 && top>bestTop){ bestTop=top; best=headings[i]; }
    }
    if(best==null) best=headings[0];
    tocLinks.forEach(function(a){ a.classList.toggle("active", a.getAttribute("data-anchor")===best); });
  }, {passive:true});

  // ---- search: fuzzy symbol match + section-level full-text, shown together ----
  var KIND_RANK={module:0,type:0,"function":1,method:1,attribute:1,symbol:2};
  // Subsequence (fuzzy) score: every char of q appears in order in name -> tolerant of typos and gaps
  // ("gtitem"->"getitem", "wrdcount"->"wordcount"). Tighter + earlier spans score higher. -1 if no.
  function subseq(q,name){
    var i=0,j=0,first=-1,last=-1;
    while(i<q.length&&j<name.length){ if(q.charCodeAt(i)===name.charCodeAt(j)){ if(first<0)first=j; last=j; i++; } j++; }
    if(i<q.length) return -1;
    return 24-(last-first+1-q.length)-first*0.2;
  }
  function scoreSym(s,q){
    var name=s.name.toLowerCase(), ql=s.q.toLowerCase();
    if(name===q) return 100;
    if(ql===q) return 95;
    if(name.indexOf(q)===0) return 82-name.length*0.1;
    if(ql.indexOf(q)===0) return 72;
    if(name.indexOf(q)>=0) return 56;
    if(ql.indexOf(q)>=0) return 46;
    if((s.sig||"").toLowerCase().indexOf(q)>=0) return 30;
    if(q.length>=3){ var ss=subseq(q,name); if(ss>0) return ss; }   // fuzzy fallback
    return -1;
  }
  function esctags(h){ var d=document.createElement("div"); d.innerHTML=h; return (d.textContent||"").replace(/\s+/g," "); }
  function hl(escaped,terms){                                        // bold each (regex-escaped) term
    if(!terms.length) return escaped;
    return escaped.replace(new RegExp("("+terms.join("|")+")","ig"),"<b>$1</b>");
  }
  // Split each page into SECTIONS at headings that carry an id, so a text hit deep-links to the
  // nearest section (not just the page top) and ranks at section granularity. Built once, cached.
  var sectCache=null;
  function buildSections(){
    sectCache=[];
    D.pages.forEach(function(p){
      var re=/<h[1-4][^>]*\bid="([^"]+)"[^>]*>([\s\S]*?)<\/h[1-4]>/g, m, marks=[];
      while((m=re.exec(p.html))){ marks.push({s:m.index, e:re.lastIndex, anchor:m[1], title:esctags(m[2])}); }
      function push(anchor,title,chunk){ var t=esctags(chunk); if(t) sectCache.push({p:p,anchor:anchor,title:title,t:t,tl:t.toLowerCase(),titl:(title||"").toLowerCase()}); }
      if(!marks.length){ push(null,p.title,p.html); return; }
      push(null,p.title,p.html.slice(0,marks[0].s));
      for(var k=0;k<marks.length;k++){ push(marks[k].anchor,marks[k].title,p.html.slice(marks[k].e, k+1<marks.length?marks[k+1].s:p.html.length)); }
    });
  }
  function fulltext(terms){
    if(!sectCache) buildSections();
    var hits=[];
    sectCache.forEach(function(sec){
      var ok=true,score=0,firstIdx=1e9;
      for(var i=0;i<terms.length;i++){ var t=terms[i], idx=sec.tl.indexOf(t); if(idx<0){ok=false;break;}
        firstIdx=Math.min(firstIdx,idx);
        var c=0,p=idx; while(p>=0&&c<8){ c++; p=sec.tl.indexOf(t,p+t.length); } score+=c;   // frequency
        if(sec.titl.indexOf(t)>=0) score+=12;                                                // heading hit
      }
      if(!ok) return;
      var s0=Math.max(0,firstIdx-45);
      hits.push({sec:sec, score:score, snip:(s0>0?"…":"")+sec.t.slice(s0,firstIdx+90)+"…"});
    });
    hits.sort(function(a,b){ return b.score-a.score; });
    return hits.slice(0,8);
  }
  function runSearch(q){
    q=(q||"").trim().toLowerCase();
    if(!q){ results.classList.remove("show"); results.innerHTML=""; return; }
    var terms=q.split(/\s+/).filter(Boolean);
    var reterms=terms.map(function(t){return t.replace(/[.*+?^${}()|[\]\\]/g,"\\$&");});
    var syms=[];
    // match symbols on the FULL query (names have no spaces); skip if the query is multi-word prose.
    if(terms.length===1){
      D.symbols.forEach(function(s){ var sc=scoreSym(s,q); if(sc>=0) syms.push({s:s,sc:sc}); });
      syms.sort(function(a,b){ return b.sc-a.sc || (KIND_RANK[a.s.kind]-KIND_RANK[b.s.kind]) || a.s.q.length-b.s.q.length; });
      syms=syms.slice(0,14);
    }
    var h="";
    if(syms.length){
      h+='<div class="res-group">Symbols</div>';
      syms.forEach(function(o){ var s=o.s;
        h+='<a class="res" href="#'+s.slug+'::'+s.anchor+'"><span class="sig">'+hl(esc(s.sig),reterms)+'</span>'+
           '<span class="kindtag">'+esc(s.kind)+'</span><div class="meta">'+
           (s.owner?esc(s.owner)+" · ":"")+esc(pageBySlug[s.slug]?pageBySlug[s.slug].title:s.slug)+'</div></a>';
      });
    }
    var ft=fulltext(terms);                              // ALWAYS show relevant prose, alongside symbols
    if(ft.length){
      h+='<div class="res-group">In the documentation</div>';
      ft.forEach(function(o){ var sec=o.sec;
        var href='#'+sec.p.slug+(sec.anchor?'::'+sec.anchor:'');
        var label=esc(sec.p.title)+(sec.title&&sec.title!==sec.p.title?' <span class="crumb">›</span> '+esc(sec.title):'');
        h+='<a class="res" href="'+href+'"><div class="meta">'+label+'</div><div class="snip">'+hl(esc(o.snip),reterms)+'</div></a>';
      });
    }
    if(!h) h='<div class="res-empty">No matches for “'+esc(q)+'”.</div>';
    results.innerHTML=h; results.classList.add("show"); selIdx=-1;
  }
  var selIdx=-1;
  function resItems(){ return Array.prototype.slice.call(results.querySelectorAll(".res")); }
  function moveSel(d){ var it=resItems(); if(!it.length) return; selIdx=(selIdx+d+it.length)%it.length;
    it.forEach(function(a,i){ a.classList.toggle("sel",i===selIdx); }); it[selIdx].scrollIntoView({block:"nearest"}); }
  function openSearch(q){ search.value=q; search.focus(); runSearch(q); }

  search.addEventListener("input", function(){ runSearch(search.value); });
  search.addEventListener("keydown", function(e){
    if(e.key==="ArrowDown"){ e.preventDefault(); moveSel(1); }
    else if(e.key==="ArrowUp"){ e.preventDefault(); moveSel(-1); }
    else if(e.key==="Enter"){ var it=resItems(); var a=it[selIdx<0?0:selIdx]; if(a){ e.preventDefault(); location.hash=a.getAttribute("href"); closeResults(); } }
    else if(e.key==="Escape"){ closeResults(); search.blur(); }
  });
  results.addEventListener("click", function(){ setTimeout(closeResults,0); });
  function closeResults(){ results.classList.remove("show"); }
  document.addEventListener("click", function(e){ if(!results.contains(e.target) && e.target!==search) closeResults(); });
  document.addEventListener("keydown", function(e){
    if(e.key==="/" && document.activeElement!==search){ e.preventDefault(); search.focus(); search.select(); }
  });

  // ---- mobile sidebar ----
  function closeSidebar(){ sidebar.classList.remove("open"); scrim.classList.remove("show"); }
  $("#menu-btn").addEventListener("click", function(){ sidebar.classList.toggle("open"); scrim.classList.toggle("show"); });
  scrim.addEventListener("click", closeSidebar);

  go(location.hash || "#" + (D.pages[0] ? D.pages[0].slug : ""));
})();
"""

SHELL = ('<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">'
         '<meta name="viewport" content="width=device-width,initial-scale=1">'
         '<title>Kirito documentation</title><style>' + CSS + '</style></head><body>'
         '<button id="menu-btn" aria-label="Menu">☰</button><div id="scrim"></div>'
         '<div id="app">'
         '<aside id="sidebar">'
         '<div class="brand"><div class="name"><b>Kirito</b> docs</div>'
         '<div class="tag">a dynamically-typed scripting language</div></div>'
         '<div class="search-wrap"><input id="search" type="text" autocomplete="off" spellcheck="false" '
         'placeholder="Search methods, types, text…"><div class="search-hint">Press '
         '<kbd>/</kbd> to search · fuzzy, jumps into the text</div></div>'
         '<div id="results"></div>'
         '<nav id="nav"></nav></aside>'
         '<main id="main"><div id="content"></div></main>'
         '<aside id="toc"></aside>'
         '</div>'
         '<script src="data.js"></script><script>' + APP_JS + '</script>'
         '</body></html>')


if __name__ == "__main__":
    main()
