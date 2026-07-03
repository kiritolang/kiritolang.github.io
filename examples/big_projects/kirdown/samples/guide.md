# kirdown user guide

## Single-file mode

Give an input file and (optionally) `-o OUT.html`:

```
ki --lib examples/big_projects/kirdown/lib \
   examples/big_projects/kirdown/main.ki \
   README.md -o README.html
```

Pass `-o -` to stream to stdout instead of a file.

## Directory mode

Use `-d` and set `-o` to a target directory. kirdown walks the input tree,
converts every `.md`, and writes the result under the mirror path:

```
ki --lib kirdown/lib kirdown/main.ki -d docs -o site
```

## Options

1. `-o, --output` — output file (single-file mode) or output directory (`-d` mode)
2. `-t, --title`  — value for `<title>` (default: first heading, else "Untitled")
3. `-d, --dir`    — treat the input as a directory
4. `--body`       — emit ONLY the `<body>` fragment, no `<html>/<head>` shell
5. `-h, --help`   — print usage

## What's *not* supported

kirdown is a **subset** of CommonMark. It intentionally skips setext headings
(`===` under a line), nested lists, HTML passthrough, and reference-style links.
For those, use a full CommonMark implementation — but the subset here already
covers the vast majority of README-style content.
