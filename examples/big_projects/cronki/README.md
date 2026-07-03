# cronki — cron-like scheduler + one-shot batch runner in pure Kirito

A small scheduler that reads a crontab file, tells you when each job would fire,
which are due at a given wall time, and runs them via `sys.shell` while appending
every result (start/end, exit code, stdout/stderr, elapsed ms, sha256 digest) to
a JSON-lines history log. Handles the full crontab micro-language: literals,
ranges, `A-B/K` steps, `*`, `*/K`, comma lists, and `@yearly`/`@monthly`/
`@weekly`/`@daily`/`@midnight`/`@hourly` macros.

## Standard-library surface exercised

| module        | how cronki uses it |
|---|---|
| `regex`       | field tokenizer, line pattern, macro pattern (all pre-compiled) |
| `time`        | `DateTime` arithmetic — `time.make`, `.add(seconds)`, `.year/.month/.day/.hour/.minute/.weekday`, `.iso()`, `strptime` for `--at` |
| `arg`         | subcommand CLI with `--at`, `--log`, `--timeout` options |
| `sys`         | `sys.shell(cmd, timeout=...)` executes the job and returns `{code, stdout, stderr}` |
| `hash`        | `sha256` prefix as a stable digest of the combined stdout+stderr |
| `json`        | one JSON-encoded dict per line in the history log (append-only, easy to `tail`) |
| `path` + `io` | reading the crontab, writing the log, tempdir for the self-test |

## Layout

```
cronki/
  main.ki                # CLI (subcommands: list / next / due / run / history)
  parse.ki               # crontab parser (ranges, steps, lists, macros)
  schedule.ki            # matches(dt, job) + nextRun(dt, job) + nextRuns(dt, job, count)
  runner.ki              # runJob(job, timeout) via sys.shell, returns the result record
  history.ki             # append + read + tail a JSON-lines log
  test_cronki.ki         # 49 checks; matched against test_cronki.expected
  samples/
    demo.crontab
```

## Subcommands

```
# list all jobs (with source line numbers)
ki examples/big_projects/cronki/main.ki list "" \
   -f examples/big_projects/cronki/samples/demo.crontab

# next 3 fire times per job, relative to a chosen wall time
ki examples/big_projects/cronki/main.ki next 3 \
   -f examples/big_projects/cronki/samples/demo.crontab \
   --at 2024-01-15T09:00

# which jobs are due AT that moment?
ki examples/big_projects/cronki/main.ki due "" \
   -f examples/big_projects/cronki/samples/demo.crontab \
   --at 2024-01-15T09:00

# actually run the due jobs (append to --log)
ki examples/big_projects/cronki/main.ki run "" \
   -f examples/big_projects/cronki/samples/demo.crontab \
   --at 2024-01-15T09:00 --log /tmp/cronki.log

# show the last N history entries
ki examples/big_projects/cronki/main.ki history 20 \
   --log /tmp/cronki.log
```

`--at` accepts `YYYY-MM-DDTHH:MM[:SS]` or `YYYY-MM-DD`; if omitted, cronki uses
`time.now()`. `--timeout` is per-job in seconds (default 60).

## Cron semantics

- Field ranges: minute 0..59, hour 0..23, day-of-month 1..31, month 1..12,
  day-of-week 0..6 with 0 = Sunday.
- POSIX day rule: when both day-of-month and day-of-week are restricted
  (neither is `*`), a job fires when EITHER matches (a union, not intersection).
- `nextRun` skips whole months / days / hours whose field doesn't match, so even
  restrictive schedules (`0 0 1 * *`) return in constant time; an impossible
  schedule (`0 0 30 2 *`) throws after one year rather than spinning forever.

## Self-test

```
ki examples/big_projects/cronki/test_cronki.ki
# → cronki: 49 checks passed
```

Wired as `script_cronki`. Passes clean under ASAN.
