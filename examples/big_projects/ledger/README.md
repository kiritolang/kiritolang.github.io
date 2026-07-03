# ledger — plain-text double-entry accounting in pure Kirito

A small but real accounting tool inspired by `hledger` and `beancount`. Reads a
plain-text ledger file (double-entry: each transaction sums to zero across its
postings), then prints balance / income / register / top / stats / csv reports.
Rejects invalid dates strictly (a lenient `time.make` silently rewrites Feb 30 to
Mar 1 — the parser catches that before it corrupts the book).

## Standard-library surface exercised

| module        | how ledger uses it |
|---|---|
| `regex`       | transaction-header, posting-line, and comment patterns — every one pre-compiled |
| `arg`         | subcommand-style CLI: `command` + `target` positionals, `--file/--from/--to` options |
| `enum`        | five-way chart of accounts (Assets / Liabilities / Income / Expenses / Equity) |
| `heapq`       | `nlargest` for the top-expenses report (decorated so the sort is stable) |
| `statistics`  | mean / median for the expenses summary |
| `csv`         | export postings as RFC-4180 CSV (`csv.format`) |
| `hash`        | `sha256` prefix — the `stats` command prints a stable digest so a downstream tool can watch for book drift |
| `path` + `io` | reading the ledger file + writing the CSV; `gettempdir` for the self-test fixture |
| `time`        | domain-checks a date via `time.make` (the parser then STRICTLY validates day-in-month up front) |

## Layout

```
ledger/
  main.ki                # CLI (subcommands, uses argmain)
  parse.ki               # ledger-file parser (regex + strict date check + balance check)
  book.ki                # in-memory model: balances, filters, category enum
  report.ki              # pretty-printers for each subcommand
  fmt.ki                 # money + iso-date formatting
  test_ledger.ki         # 43 checks; matched against test_ledger.expected
  samples/
    demo.ledger
```

## Usage

```
# balances (all non-zero accounts, sorted)
ki examples/big_projects/ledger/main.ki balances "" \
   -f examples/big_projects/ledger/samples/demo.ledger

# income for a date range
ki examples/big_projects/ledger/main.ki income "" \
   -f examples/big_projects/ledger/samples/demo.ledger \
   --from 2024-02-01 --to 2024-02-28

# register on any account (matches by exact name OR prefix at a :-boundary)
ki examples/big_projects/ledger/main.ki register Expenses:Food \
   -f examples/big_projects/ledger/samples/demo.ledger

# top 5 expenses
ki examples/big_projects/ledger/main.ki top 5 \
   -f examples/big_projects/ledger/samples/demo.ledger

# summary + digest
ki examples/big_projects/ledger/main.ki stats "" \
   -f examples/big_projects/ledger/samples/demo.ledger

# dump every posting as CSV
ki examples/big_projects/ledger/main.ki csv /tmp/postings.csv \
   -f examples/big_projects/ledger/samples/demo.ledger
```

The `command` positional is required; the `target` positional is the second
argument for `register`/`top`/`csv` (or `""` when unused). Pass `-h`/`--help`
for the full usage.

## Ledger file grammar

```
; comment (# also works)
YYYY-MM-DD FLAG description
    Account:Sub    AMOUNT [USD]
    Account:Sub   -AMOUNT [USD]
```

Rules:

- The date must be a real calendar date (Feb 30, month 13, day 0 are all rejected).
- A transaction must have at least one posting.
- The postings of a transaction MUST sum to zero (allowing a tiny 0.001 slack for
  Float representation error), otherwise the parser throws with the file, line,
  and imbalance amount.
- Account names use `Top:Sub:SubSub` colon-separated segments; top-level
  segments outside the five known categories still parse (they're just not
  classified by `book.categoryOf`).

## Self-test

```
ki examples/big_projects/ledger/test_ledger.ki
# → ledger: 43 checks passed
```

CTest wires this as `script_ledger`. Passes clean under ASAN.
