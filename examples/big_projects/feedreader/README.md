# feedreader — RSS 2.0 + Atom feed reader in pure Kirito

Subscribe to feeds by URL (http/https) or local `.xml` path, sync them into a
persistent store, list unread items, mark read/unread, and search — the way a
Newsboat/Feedly-style tool feels from the CLI. Parses both RSS 2.0 and Atom
1.0 (auto-detected from the root tag), normalizes them into a single
`{id, title, link, summary, date}` shape, and hashes each `(feed_url, item_id)`
pair into a stable 16-char key so read markers survive across syncs.

## Standard-library surface exercised

| module        | how feedreader uses it |
|---|---|
| `xml`         | the real workhorse: `xml.fromstring` for both RSS and Atom trees, walked with the ElementTree-style `element.children` / `element.get(attr)` API |
| `net`         | `net.get(url)` + `Response.raiseforstatus()` when the source is `http://` / `https://` |
| `regex`       | `stripHtml` (drops inline tags) + whitespace collapse; search subcommand accepts an arbitrary regex |
| `hash`        | `sha256(feedUrl + item_id)[0:16]` — a stable, opaque, fixed-length key for the read set |
| `json`        | on-disk state format (`store.load` / `store.save`); pretty-printed with `indent = 2` |
| `arg`         | subcommand-style CLI + `--store`, `--tag`, `--width` options |
| `textwrap`    | wraps the summary body in `show` |
| `path` + `io` | reading local feeds, tempdir for the default store file |

## Layout

```
feedreader/
  main.ki                  # CLI (subcommands: add/remove/list/sync/items/read/unread/search/show)
  feed.ki                  # RSS+Atom XML parser (namespace-stripped)
  store.ki                 # persistent state (feeds + read markers + tags)
  ui.ki                    # listItems / listFeeds / detail + stripHtml
  test_feedreader.ki       # 50 checks; matched against test_feedreader.expected
  samples/
    sample_rss.xml
    sample_atom.xml
```

## Usage

```
# subscribe (a URL http:// or a local .xml file are both fine)
ki examples/big_projects/feedreader/main.ki \
   add examples/big_projects/feedreader/samples/sample_rss.xml \
   -s /tmp/reader.json --tag kirito

# ...another one
ki examples/big_projects/feedreader/main.ki \
   add examples/big_projects/feedreader/samples/sample_atom.xml \
   -s /tmp/reader.json --tag cpp,programming

# list every subscribed feed with per-feed unread counts
ki examples/big_projects/feedreader/main.ki list "" -s /tmp/reader.json

# list every item across every feed (R = read, blank = unread)
ki examples/big_projects/feedreader/main.ki items "" -s /tmp/reader.json

# filter items by tag
ki examples/big_projects/feedreader/main.ki items "" -s /tmp/reader.json --tag cpp

# search titles + summaries (any Kirito `regex` pattern; case-insensitive)
ki examples/big_projects/feedreader/main.ki search coroutines -s /tmp/reader.json

# read an item (KEY is the 16-hex-char id shown by `items`)
ki examples/big_projects/feedreader/main.ki read 8c45d1ca56bceee7 -s /tmp/reader.json

# full detail view (with a wrapped summary)
ki examples/big_projects/feedreader/main.ki show 8c45d1ca56bceee7 -s /tmp/reader.json --width 78

# re-fetch every feed and merge new items
ki examples/big_projects/feedreader/main.ki sync "" -s /tmp/reader.json
```

`sync` is safe against transient network failures — a failed fetch is reported
and the run continues with the other feeds; the store file is only rewritten
after all fetches complete.

## Self-test

```
ki examples/big_projects/feedreader/test_feedreader.ki
# → feedreader: 50 checks passed
```

Wired as `script_feedreader`. Uses only the local sample XML files, so it works
offline. Passes clean under ASAN.
