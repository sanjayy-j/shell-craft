# ShellCraft Test Suite

This directory contains automated and interactive tests covering all core ShellCraft milestones and the four differentiating features (`plan`, `trace`, `inspect`, `diagnose`).

## 1. Automated Test Suite (`make test`)

Run from the repository root:

```bash
make test
```

`make test` compiles `shellcraft` and `build/test_parser` and runs `tests/run_tests.sh`, which executes:

1. **`tests/test_parser.cpp` (Parser Unit Tests — 31 assertions)**:
   - Empty lines, whitespace, and `#` comments.
   - Single-quoted (`'...'`), double-quoted (`"..."`), and backslash-escaped (`\`) arguments.
   - Operators (`|`, `<`, `>`, `>>`, `&`) both space-separated and directly adjacent to words.
   - Malformed syntax rejection (unclosed quotes, trailing backslashes, leading/trailing/empty pipes, missing redirection targets, misplaced `&`).
2. **Core Shell Integration Tests (`tests/run_tests.sh`)**:
   - Basic external execution (`echo`, `printf`, argument handling, unknown command recovery).
   - Built-in commands (`cd`, `pwd`, `help`, `exit`).
   - File-descriptor redirection (`>`, `>>`, `<`, combined `<` and `>`, standalone built-in redirection with descriptor restoration).
   - Two-stage and three-stage pipelines (`|`) with file redirections.
   - Background job launch (`&`), `jobs` listing while active, and nonblocking `waitpid()` child reaping.
3. **Differentiating Feature Tests (`tests/run_tests.sh`)**:
   - **Feature 1 (`plan <command>`)**: Verifies stage count, pipe count, redirection targets, existing-file truncation warnings, syntax error reporting, and non-execution/non-modification guarantees.
   - **Feature 2 (`trace <command>`, `trace last`, `trace export`)**: Verifies recorded lifecycle events, replay without re-executing commands, and valid JSON export verified via Python's `json` parser.
   - **Feature 3 (`inspect last`)**: Verifies matching plan-vs-actual reports on successful pipelines, discrepancy detection on redirection/exec failures, and `unknown (incomplete)` reporting when inspecting an uncollected background job.
   - **Feature 4 (`diagnose last`)**: Verifies multi-stage failure diagnosis on `cat missing.txt | grep ERROR | wc -l` and `execvp()` `ENOENT` failures.
4. **Interactive PTY Job Control & Signal Trace Tests (`tests/interactive_pty_test.py`)**:
   - Allocates a Linux pseudo-terminal (PTY) with a controlling terminal (`TIOCSCTTY`).
   - Verifies foreground process-group handoff (`tcsetpgrp`), stopping a foreground job with `Ctrl+Z` (`SIGTSTP`), terminal reclamation by the shell, inspecting stopped status with `jobs`, resuming in background with `bg %1`, bringing back to foreground with `fg %1`, interrupting with `Ctrl+C` (`SIGINT`), and inspecting recorded terminal/signal events via `trace last` and `diagnose last`.
