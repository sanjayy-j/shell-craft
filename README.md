# ShellCraft

A custom Unix shell built from scratch in C++17 as an Operating Systems Lab project, featuring **Pre-Execution Impact Planning**, **Runtime Execution Tracing & Replay**, **Plan-vs-Execution Verification**, and **Evidence-Based Pipeline Failure Diagnosis**.

ShellCraft is an interactive and scriptable command-line interpreter that reads user input, parses it into structured commands and pipelines, and executes them directly using POSIX process-management, file-descriptor, and terminal job-control system calls.

---

## Objectives

- Understand how a shell creates and manages processes using `fork()` and `execvp()`.
- Synchronise foreground pipelines and reap background children using `waitpid()`.
- Manipulate file descriptors using `open()`, `dup2()`, `pipe()`, and `close()` to implement I/O redirection and multi-stage pipelines.
- Manage process groups (`setpgid()`) and controlling terminal foreground ownership (`tcsetpgrp()`).
- Handle signals (`SIGINT`, `SIGTSTP`, `SIGCHLD`, `SIGTTIN`, `SIGTTOU`) safely using `sigaction()` and async-signal-safe handlers.
- Provide deep OS observability through pre-execution planning (`plan`), runtime tracing (`trace`), plan verification (`inspect last`), and failure analysis (`diagnose last`).

---

## Core & Differentiating Features

### Core Shell Capabilities
- [x] Interactive prompt (`shellcraft> `) and read–parse–execute loop (plus non-interactive piped input support)
- [x] Structured command & pipeline parser supporting single quotes (`'...'`), double quotes (`"..."`), and backslash escapes (`\`)
- [x] Operator recognition (`|`, `<`, `>`, `>>`, `&`), including operators adjacent to command words
- [x] External command execution via `fork()`, `execvp()` (`PATH` lookup), and `waitpid()`
- [x] Built-in commands: `cd [dir]`, `pwd`, `help`, `exit [code]`, `jobs`, `fg [%n]`, `bg [%n]`
- [x] Input redirection (`<`) and output redirection (`>` truncate, `>>` append), including on standalone built-ins and individual pipeline stages
- [x] Two-stage and multi-stage pipelines (`A | B | C | ...`) with full pipe descriptor cleanup
- [x] Background job execution (`&`), in-memory job table (`JobManager`), and nonblocking zombie reaping
- [x] Process groups (`setpgid()`) and terminal foreground handoff (`tcsetpgrp()`)
- [x] Signal handling (`sigaction()`) for `Ctrl+C` (`SIGINT`), `Ctrl+Z` (`SIGTSTP`), and `SIGCHLD`

### Four Integrated Differentiating Features
1. **Pre-Execution Impact Planner (`plan <command>`)**:
   - Inspects a parsed command or multi-stage pipeline without executing external programs or opening files with truncating flags.
   - Displays pipeline stages, argument lists, expected child/external process launches, pipe topology (`pipe 1`, `pipe 2`, ...), `stdin`/`stdout` sources, redirections, non-destructive `stat()`/`access()` file truncation/creation warnings, and syntax errors.
2. **Runtime Execution Trace Recorder & Replay (`trace <command>`, `trace last`, `trace export <file.json>`)**:
   - Records actual shell lifecycle events (`COMMAND_PARSED`, `PLAN_CREATED`, `PIPE_CREATED`/`PIPE_FAILED`, `FORK_SUCCEEDED`/`FORK_FAILED`, `PGID_ASSIGNED`, `REDIRECTION_SUCCEEDED`/`REDIRECTION_FAILED`, `EXECVP_ATTEMPTED`, `EXECVP_SUCCEEDED`/`EXECVP_FAILED`, `TERMINAL_TRANSFERRED`/`TERMINAL_RECLAIMED`, `WAITPID_COMPLETED`, `PROCESS_EXITED`/`PROCESS_SIGNALED`/`PROCESS_STOPPED`/`PROCESS_CONTINUED`) with sequence numbers and microsecond timestamps.
   - Uses a fork-safe close-on-exec (`FD_CLOEXEC`) reporting pipe between each child and the parent so child redirection and `execvp()` outcomes are captured accurately without corrupting parent C++ containers across `fork()`.
   - Supports replaying the last trace (`trace last`) without re-executing commands and exporting valid JSON (`trace export <file.json>`).
3. **Execution Plan vs. Actual Behaviour Verifier (`inspect last`)**:
   - Compares the shared `ExecutionPlan` against the recorded `ExecutionTrace` across stage counts, pipe setup, child process launches, redirections, `execvp()` execution, and `waitpid()` completion outcomes.
   - Reports missing completion evidence (such as a still-running background job) as `unknown` rather than `succeeded`, and distinguishes shell setup failures from application non-zero exit codes.
4. **Pipeline Failure Analyzer (`diagnose last` / `inspect last`)**:
   - Produces a stage-by-stage diagnostic report covering PIDs, process creation, redirection setup, `execvp()` status, exit codes, and signals (`SIGINT`, `SIGPIPE`, `SIGTSTP`).
   - Clearly distinguishes **Confirmed Facts** (proven by recorded shell `errno` or kernel wait status) from **Likely Explanations** (e.g., missing file operands on `cat` or `grep` returning status 1 when 0 lines match).

---

## Architecture & Module Responsibilities

```text
shell-craft/
├── src/
│   ├── main.cpp          # Entry point
│   ├── shell.hpp/.cpp    # REPL loop, prompt, plan/trace dispatch, background reaping
│   ├── parser.hpp/.cpp   # Tokenizer & parser producing Pipeline and Command structs
│   ├── planner.hpp/.cpp  # Feature 1: ExecutionPlanner & non-destructive file inspection
│   ├── tracer.hpp/.cpp   # Feature 2: TraceRecorder, event timeline, replay, and JSON export
│   ├── verifier.hpp/.cpp # Features 3 & 4: PlanVerifier and PipelineAnalyzer
│   ├── builtins.hpp/.cpp # Built-in commands (cd, pwd, help, exit, jobs, fg, bg, trace, inspect, diagnose)
│   ├── executor.hpp/.cpp # fork(), execvp(), pipe(), open(), dup2(), FD_CLOEXEC child reporting
│   └── jobs.hpp/.cpp     # JobManager table, setpgid(), tcsetpgrp(), sigaction(), waitpid()
├── tests/
│   ├── test_parser.cpp         # C++ unit tests for the command-line parser
│   ├── run_tests.sh            # Automated integration test suite (core + 4 novel features)
│   ├── interactive_pty_test.py # Automated PTY test for Ctrl+C, Ctrl+Z, fg, bg, and signal tracing
│   └── README.md               # Test documentation
├── docs/
│   ├── design.md         # Shared internal architecture and module design notes
│   ├── syscalls.md       # Explanation of POSIX system calls used
│   ├── milestones.md     # Milestone and feature implementation summary
│   └── README.md         # Documentation index
├── Makefile
└── README.md
```

---

## Role of Core POSIX System Calls

| System Call | Role in ShellCraft |
|---|---|
| `fork()` | Duplicates the shell process to create a child process for each command or pipeline stage (`src/executor.cpp`). |
| `execvp()` | Replaces the child process image with the target program searched via `PATH` (`src/executor.cpp`). |
| `waitpid()` | Waits for foreground pipeline processes (`WUNTRACED`) and reaps background children non-blockingly (`WNOHANG \| WUNTRACED \| WCONTINUED`) to prevent zombies (`src/jobs.cpp`). |
| `pipe()` & `fcntl(FD_CLOEXEC)` | Creates inter-stage data pipes and a close-on-exec child-to-parent telemetry pipe to report child redirection and `execvp()` outcomes safely (`src/executor.cpp`). |
| `open()`, `dup2()`, `close()` | Opens files for `<`, `>`, `>>` and redirects `STDIN_FILENO` (0) or `STDOUT_FILENO` (1), closing unused descriptors so none leak (`src/executor.cpp`). |
| `stat()`, `access()` | Inspects file existence, types, and permissions non-destructively during `plan` and post-failure diagnosis (`src/planner.cpp`, `src/verifier.cpp`). |
| `setpgid()` | Places all processes of a job/pipeline into a dedicated process group (`pgid`) in both parent and child to avoid scheduling races (`src/executor.cpp`, `src/jobs.cpp`). |
| `tcsetpgrp()` | Transfers controlling terminal foreground ownership to the active foreground job's process group and reclaims it for the shell when the job finishes or stops (`src/jobs.cpp`). |
| `sigaction()` | Installs async-signal-safe handlers (`SIGCHLD`, `SIGINT`) and ignores job-control stop signals (`SIGTSTP`, `SIGTTIN`, `SIGTTOU`) in the parent shell while resetting `SIG_DFL` in children (`src/jobs.cpp`). |

---

## Required Environment & Setup

ShellCraft uses POSIX APIs and requires **Linux** or **Windows Subsystem for Linux (WSL)** with:
- `g++` (with C++17 support)
- GNU `make`
- `bash` and `python3` (for running the automated test suite)

On Ubuntu / WSL:
```bash
sudo apt-get update && sudo apt-get install -y g++ make python3
```

---

## Build, Run, and Test Instructions

```bash
# Clean previous build artifacts
make clean

# Compile the shellcraft executable
make

# Run the automated unit, integration, novel-feature, and PTY job-control test suite
make test

# Launch ShellCraft interactively
./shellcraft
```

---

## Sample Usage

### 1. Pre-Execution Impact Planner (`plan`)
```text
shellcraft> plan cat server.log | grep ERROR | wc -l > errors.txt
ShellCraft Execution Plan

Command: cat server.log | grep ERROR | wc -l > errors.txt
Execution mode: foreground
Pipeline stages: 3
Expected child process launches: 3
Expected external process launches: 3
Pipes required: 2

Stage 1: cat server.log
  command: cat
  arguments: ["cat", "server.log"]
  execution mechanism: external program (fork + execvp)
  stdin: inherited
  stdout: pipe 1
...
Execution has not started.
```

### 2. Runtime Execution Tracing, Verification, and Failure Diagnosis (`trace`, `inspect`, `diagnose`)
```text
shellcraft> trace cat missing.txt | grep ERROR | wc -l
cat: missing.txt: No such file or directory
0
shellcraft> trace last
shellcraft> trace export trace.json
shellcraft> inspect last
shellcraft> diagnose last
```

---

## Known Limitations

ShellCraft implements approximately **85–90%** of a student-level Unix shell scope, plus all four planning, tracing, verification, and diagnostic features. The following advanced Bash features are out of scope:
- Command substitution (`$(cmd)`, `` `cmd` ``) and general `$VAR` expansion (except `~` in `cd`)
- Wildcard / glob expansion (`*`, `?`)
- Logical chaining (`&&`, `||`, `;`), subshells, and scripting loops/conditionals
- Here-documents (`<<`), arbitrary descriptor redirection (`2>&1`), and readline history navigation
