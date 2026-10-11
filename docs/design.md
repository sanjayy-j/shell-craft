# ShellCraft Architecture and Design

## 1. Shared Internal Architecture

ShellCraft implements its core shell execution and its four differentiating features (`plan`, `trace`, `inspect`, `diagnose`) on top of one shared internal pipeline and event architecture:

```
User Input (TTY / stdin)
        │
        ▼
┌──────────────────────┐
│    CommandParser     │  src/parser.hpp, src/parser.cpp
│ (parse_command_line) │  Produces structured Pipeline, Command, Redirection
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│   ExecutionPlanner   │  src/planner.hpp, src/planner.cpp
│    (create_plan)     │  Derives ExecutionPlan, PlannedStage, file warnings
└────┬────────────┬────┘  (Used both by 'plan <cmd>' and before execution)
     │            │
     │            ▼
     │   ┌──────────────────────┐
     │   │   ExecutionEngine    │  src/executor.hpp, src/executor.cpp
     │   │  (execute_pipeline)  │  pipe(), fork(), setpgid(), dup2(), execvp()
     │   │  + JobManager        │  + FD_CLOEXEC child-to-parent reporting pipe
     │   └──────────┬───────────┘
     │              │
     │              ▼
     │   ┌──────────────────────┐
     └──►│    TraceRecorder     │  src/tracer.hpp, src/tracer.cpp
         │   (ExecutionTrace)   │  Bounded history of chronological TraceEvents
         └──────────┬───────────┘  & StageOutcomes; JSON export & replay
                    │
                    ▼
         ┌──────────────────────┐
         │   PlanVerifier &     │  src/verifier.hpp, src/verifier.cpp
         │  PipelineAnalyzer    │  Compares ExecutionPlan vs ExecutionTrace
         └──────────────────────┘  and diagnoses per-stage failures
```

---

## 2. Module Responsibilities

| File | Responsibility |
|---|---|
| `src/main.cpp` | Entry point; invokes `shellcraft::run_shell()`. |
| `src/shell.hpp/.cpp` | REPL loop, prompt rendering, `plan <cmd>` and `trace <cmd>` prefix dispatch, `EINTR` recovery on `SIGINT`, and background child reaping. |
| `src/parser.hpp/.cpp` | Lexical scanner and syntax validator producing `Pipeline`, `Command`, and `Redirection` structures. |
| `src/planner.hpp/.cpp` | **Feature 1 (`ExecutionPlanner`)**: Computes expected child launches, external `execvp()` calls, pipe wiring, and non-destructive `stat()`/`access()` file impact warnings. |
| `src/tracer.hpp/.cpp` | **Feature 2 (`TraceRecorder`)**: Records chronological `TraceEvent`s and per-stage `StageOutcome`s in a bounded ring buffer (32 traces), supporting `trace last` replay and `trace export <file.json>`. |
| `src/verifier.hpp/.cpp` | **Features 3 & 4 (`PlanVerifier` & `PipelineAnalyzer`)**: Verifies planned vs. observed operations (`inspect last`) and performs evidence-based stage failure diagnosis (`diagnose last`). |
| `src/builtins.hpp/.cpp` | Built-in commands (`cd`, `pwd`, `help`, `exit`, `jobs`, `fg`, `bg`, `trace`, `inspect`, `verify`, `diagnose`). |
| `src/executor.hpp/.cpp` | Executes standalone built-ins and multi-stage pipelines (`pipe`, `fork`, `dup2`, `execvp`) with a `FD_CLOEXEC` status pipe from each child to the parent. |
| `src/jobs.hpp/.cpp` | Manages the job table (`JobManager`), process groups (`setpgid`), terminal handoff (`tcsetpgrp`), nonblocking child reaping (`waitpid`), and `sigaction` signal handlers. |

---

## 3. Fork-Safe Child Event Reporting (`FD_CLOEXEC` Pipe)

Because `fork()` duplicates the parent's memory space into a separate child process, any mutations to C++ containers inside the child process would be lost upon `execvp()` or `_exit()`.

To capture genuine child-side redirection and `execvp()` outcomes in the parent's `TraceRecorder`:
1. Before forking stage $i$, the parent creates a dedicated reporting pipe `report_fds[2]` and sets `FD_CLOEXEC` on both ends via `fcntl()`.
2. The child writes fixed-size POD `ChildReportMsg` packets (`REDIR_OK`, `REDIR_FAILED`, `BUILTIN_RUN`, `EXEC_ATTEMPT`, `EXEC_FAILED`) to `report_fds[1]` including the exact `errno` when `open()`, `dup2()`, or `execvp()` fails.
3. When `execvp()` succeeds, the kernel automatically closes `report_fds[1]` due to `FD_CLOEXEC`. When the parent sees `EXEC_ATTEMPT` followed by `EOF` (with no `EXEC_FAILED`), it records `EXECVP_SUCCEEDED`.
