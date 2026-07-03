# snip — code snippet manager in pure Kirito

A CLI-first snippet manager: add snippets from stdin or a file, tag them, list
them (filtered by tag, language, or arbitrary regex), view them with line
numbers, and export/import a portable JSON snapshot. Uses the `dump` binary
codec for its primary on-disk format (compact + reference-preserving) and
`json` for the portable dump so a checked-in snippet library is diff-able.

Each snippet's id is derived from the sha256 of its body, so the same body
uploaded twice gets one row (metadata is refreshed, the id doesn't drift), and
short-prefix lookups (`snip show c8`) work as long as the prefix is unique.

## Standard-library surface exercised

| module       | how snip uses it |
|---|---|
| `dump`       | primary on-disk format — `dump.dumps` / `loads` a nested dict, reference-preserving |
| `json`       | portable `export` / `import` (schema-versioned, diff-able) |
| `hash`       | `sha256(body)[0:12]` derives the snippet id |
| `regex`      | `search` filter matches title OR body (case-insensitive) |
| `arg`        | subcommand-style CLI + `--db`, `--title`, `--lang`, `--tags`, `--regex`, `--top` |
| `time`       | `time.now().timestamp` for created/modified stamps; `humanAge` renders them |
| `path` + `io`| reading source files for `addfile`, writing the dump/JSON |

## Layout

```
snip/
  main.ki           # CLI (add / addfile / list / show / remove / stats / export / import)
  db.ki             # in-memory DB + dump persistence + JSON round-trip
  search.ki         # composable filters: byTags / byLang / byRegex + topTags / languages
  fmt.ki            # humanAge + listLine + show (with line numbers) + stats
  test_snip.ki      # 65 checks; matched against test_snip.expected
  samples/
    hello.py
    factorial.ki
```

## Usage

```
# add a snippet inline
ki examples/big_projects/snip/main.ki \
   add "print('hello')" -d /tmp/snip.dump \
   --title "hello world" --lang python --tags demo,cli

# add a snippet from a file (language auto-guessed from the extension)
ki examples/big_projects/snip/main.ki \
   addfile examples/big_projects/snip/samples/factorial.ki \
   -d /tmp/snip.dump --tags math,recursion

# list every snippet, most recent first
ki examples/big_projects/snip/main.ki list "" -d /tmp/snip.dump

# filter by tag AND language AND regex (they combine)
ki examples/big_projects/snip/main.ki list "" -d /tmp/snip.dump \
   --tags demo --lang python --regex hello

# show a snippet with line numbers (an ID prefix is enough if it's unique)
ki examples/big_projects/snip/main.ki show c8 -d /tmp/snip.dump

# stats: total, per-language, top tags
ki examples/big_projects/snip/main.ki stats "" -d /tmp/snip.dump --top 5

# export to a JSON snapshot (diff-able, portable)
ki examples/big_projects/snip/main.ki export /tmp/snips.json -d /tmp/snip.dump

# ...and import it back into a fresh store
ki examples/big_projects/snip/main.ki import /tmp/snips.json -d /tmp/snip2.dump
```

`import` is idempotent — re-importing the same JSON adds nothing (ids collide).

## Self-test

```
ki examples/big_projects/snip/test_snip.ki
# → snip: 65 checks passed
```

Wired as `script_snip` in CTest. Passes clean under ASAN.
