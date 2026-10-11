# ShellCraft Implementation Milestones

This document summarizes what was implemented across each milestone and differentiating feature of the ShellCraft project.

---

## Milestone 1 — Interactive Shell and Command Parser (`src/parser.hpp`, `src/parser.cpp`, `src/shell.cpp`)
- Implemented the interactive read–parse–execute loop (`run_shell()`) displaying `shellcraft> ` when attached to a terminal and exiting cleanly on `exit` or `EOF` (`Ctrl+D`).
- Built a custom lexical tokenizer and parser producing structured `Pipeline`, `Command`, and `Redirection` objects.
- Added full support for single-quoted strings (`'...'`), double-quoted strings (`"..."`), and backslash escapes (`\`).
- Recognizes operators `|`, `<`, `>`, `>>`, and `&` even when adjacent to command words (e.g., `cat<in.txt|wc -l>out.txt`).
- Detects and reports syntax errors (unclosed quotes, trailing backslashes, leading/trailing/empty pipes, missing redirection targets, misplaced `&`) without crashing.

## Milestone 2 — External Commands and Built-ins (`src/executor.cpp`, `src/builtins.cpp`)
- Implemented external program execution using `fork()`, `execvp()` (searching `PATH`), and `waitpid()`.
- Handled unknown commands (`ENOENT`, exit status `127`) and execution errors (`126`) cleanly via `_exit()` in the child process.
- Implemented all required built-in commands: `cd [dir]`, `pwd`, `help`, `exit [code]`, `jobs`, `fg [%n]`, `bg [%n]`, plus `plan`, `trace`, `inspect`, and `diagnose`.
- Standalone foreground built-ins execute in the parent process, saving and restoring `STDIN_FILENO` / `STDOUT_FILENO` via `dup()` and `dup2()` when redirected.

## Milestone 3 — I/O Redirection (`src/executor.cpp`)
- Implemented input redirection (`<`, `O_RDONLY`), output truncation (`>`, `O_WRONLY | O_CREAT | O_TRUNC`, mode `0644`), and output appending (`>>`, `O_WRONLY | O_CREAT | O_APPEND`, mode `0644`).
- Combined input and output redirections on single commands and pipeline stages work seamlessly while closing temporary descriptors to prevent leaks.

## Milestone 4 — Pipelines (`src/executor.cpp`)
- Implemented two-stage and multi-stage pipelines (`A | B | C | ...`) using `pipe()`, `fork()`, and `dup2()`.
- Closed all unused pipe read/write ends in both parent and child processes to prevent pipe deadlocks.
- Parent waits for all processes in a foreground pipeline before returning the prompt.

## Milestone 5 — Background Execution and Job Table (`src/jobs.hpp`, `src/jobs.cpp`)
- Commands and pipelines ending in `&` run asynchronously in the background and print `[<job_id>] <pid>`.
- `JobManager` tracks job IDs, process group IDs (`pgid`), command strings, job states (`Running`, `Stopped`, `Done`), and every constituent process in a pipeline (`std::vector<ProcessInfo>`).
- Background children are reaped non-blockingly using `waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)` to prevent zombie processes.

## Milestone 6 — Foreground/Background Job Control and Signals (`src/jobs.cpp`, `src/executor.cpp`)
- Placed each job/pipeline into its own process group (`setpgid()`) in both parent and child to eliminate race conditions.
- Handed controlling terminal ownership to foreground jobs with `tcsetpgrp(STDIN_FILENO, pgid)` and reclaimed terminal ownership for the shell when the foreground job exited or stopped.
- Configured signal dispositions via `sigaction()` using minimal async-signal-safe handlers (`volatile sig_atomic_t` flags only) in the parent shell and restored `SIG_DFL` in child processes before `execvp()`.
- Verified `Ctrl+C` (`SIGINT`), `Ctrl+Z` (`SIGTSTP`), `jobs`, `bg %n`, and `fg %n`.

## Differentiating Feature 1 — Pre-Execution Impact Planner (`src/planner.hpp`, `src/planner.cpp`)
- Implemented `plan <command>` using `ExecutionPlanner::create_plan()`.
- Inspects target files non-destructively via `stat()` and `access()` without executing external commands or truncating files.
- Reports stage count, expected child and external `execvp()` launches, pipe topology, per-stage `stdin`/`stdout` sources, redirections, and truncation/missing-input warnings.

## Differentiating Feature 2 — Runtime Execution Trace Recorder & Replay (`src/tracer.hpp`, `src/tracer.cpp`)
- Implemented `trace <command>`, `trace last`, and `trace export <file.json>`.
- Uses a fork-safe close-on-exec (`FD_CLOEXEC`) reporting pipe from each forked child to the parent to capture child redirection and `execvp()` outcomes accurately.
- Records timestamped, sequenced `TraceEvent`s and per-stage `StageOutcome`s in a bounded ring buffer (`MAX_TRACE_HISTORY = 32`).

## Differentiating Feature 3 — Execution Plan vs. Actual Behaviour Verifier (`src/verifier.hpp`, `src/verifier.cpp`)
- Implemented `inspect last` (`PlanVerifier::print_verification_report()`).
- Compares planned stages, pipes, process launches, redirections, `execvp()` calls, and `waitpid()` outcomes against actual trace evidence, reporting missing evidence as `unknown` rather than `succeeded`.

## Differentiating Feature 4 — Pipeline Failure Analyzer (`src/verifier.hpp`, `src/verifier.cpp`)
- Implemented `diagnose last` (also included in `inspect last`) via `PipelineAnalyzer::print_diagnostic_report()`.
- Distinguishes confirmed causes (`ENOENT`, `EACCES`, `SIGINT`, `SIGPIPE`, `SIGTSTP`, redirection failures) from evidence-based explanations (e.g., missing file operands for `cat` or `grep` status 1 on zero matches).
