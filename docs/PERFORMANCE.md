# Performance baseline

Measured with `tools/benchmark.py`, which generates a synthetic
monster repository (hostile on both axes: history depth for log
paging/lane computation, tree width for the watcher and status walk)
and drives the real binary through the test bridge.

What the columns mean:

- **bridge** — process start until the test bridge answers (app init)
- **open** — until `dump-state` reports the repo open (libgit2 open +
  .git-internals watch; the window is already up with the loading
  overlay before this)
- **first commits** — until the first log page is in the model: the
  moment a user sees history
- **RSS** — resident memory after refreshes settle

## Results

| date | commits | dirs | bridge | open | first commits | RSS |
|------|---------|------|--------|------|---------------|-----|
| 2026-06-12 | 50000 | 20000 | 0.68s | 0.68s | 1.3s | 186 MB |

Machine: macOS 15.6, Apple Silicon (10 cores), Qt 6.11, release-ish
Debug-default build. Best of 3, warm filesystem cache (the generator
had just written the repo); a cold-cache first open will be slower —
dominated by libgit2 faulting in the pack, not by GitBolt code.

## Known, deliberate limits

- The file watcher enumerates at most 4096 working-tree directories
  (`FileWatcher`), applied in chunks of ~512 per event-loop tick.
  In a tree wider than that (like this benchmark's `wide/`), edits in
  unwatched directories don't auto-refresh status — .git-internals
  watching (commits, ref updates, index changes) is unaffected.
- The commit log loads in pages (256 rows first); `logRows` at settle
  reflects the first page, not the full 50k — deeper history streams
  in as the view scrolls, with lane state folded incrementally.

## Reproducing

```bash
cmake --build build --parallel
python3 tools/benchmark.py build/src/app/GitBolt.app/Contents/MacOS/GitBolt
```

The repo is cached at /tmp/gitbolt-monster and reused; delete it to
regenerate (~40s). Add a row above when re-measuring after perf work.
