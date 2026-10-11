# POSIX System Calls Used in ShellCraft

This document explains the core POSIX system calls used in ShellCraft, where they appear in the codebase, and why each is required.

---

## 1. `fork()` — Process Creation (`src/executor.cpp`)
- **Purpose**: Creates a new child process by duplicating the calling parent process.
- **How ShellCraft uses it**: For every stage in a pipeline (or single external command), the shell calls `fork()`. It returns `0` in the child process and the child's PID (`> 0`) in the parent process.
- **Why it is needed**: Unix separates process creation (`fork()`) from program loading (`execvp()`), allowing the child process to configure its process group, signal dispositions, pipes, and file redirections before loading the target executable.

## 2. `execvp()` — Program Replacement (`src/executor.cpp`)
- **Purpose**: Replaces the current process image with a new program image, searching directories listed in the `PATH` environment variable when the command name does not contain a slash (`/`).
- **How ShellCraft uses it**: In the forked child, `Command::args` is converted into a null-terminated `char*` array and passed to `execvp(argv[0], argv)`.
- **Error Handling**: `execvp()` only returns if an error occurs (e.g., `ENOENT` for unknown commands). On failure, the child prints a diagnostic message to `stderr` and terminates immediately via `_exit(127)` (or `_exit(126)`) so it never continues running the parent shell loop.

## 3. `waitpid()` — Process Synchronisation & Reaping (`src/jobs.cpp`)
- **Purpose**: Waits for process state changes (termination, stop by signal, or continuation) in child processes and reaps terminated children to prevent zombie (`<defunct>`) processes.
- **How ShellCraft uses it**:
  - **Foreground execution (`wait_for_foreground_job`)**: Calls `waitpid(-1, &status, WUNTRACED)` in a loop until all processes in the foreground job have either exited (`WIFEXITED` / `WIFSIGNALED`) or stopped (`WIFSTOPPED`, e.g., via `Ctrl+Z`).
  - **Background reaping (`reap_children`)**: Calls `waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)` in a nonblocking loop during normal shell execution to collect finished or state-changed background children without blocking the prompt.

## 4. `pipe()` — Inter-Process Communication (`src/executor.cpp`)
- **Purpose**: Creates a unidirectional kernel data channel with two file descriptors: `pipe_fds[0]` (read end) and `pipe_fds[1]` (write end).
- **How ShellCraft uses it**: For an $N$-stage pipeline `cmd1 | cmd2 | ... | cmdN`, the shell creates a pipe before forking each stage $i < N - 1$. Stage $i$ redirects its standard output to `pipe_fds[1]`, and stage $i + 1$ redirects its standard input from `pipe_fds[0]`.
- **Avoiding Deadlocks**: Both parent and child close all unused pipe file descriptors immediately. If the write end remained open in the parent or later children, readers would never observe `EOF`.

## 5. `open()`, `dup2()`, and `close()` — File Descriptor Manipulation (`src/executor.cpp`)
- **Purpose**:
  - `open()` opens or creates a file and returns a file descriptor (`O_RDONLY` for `<`, `O_WRONLY | O_CREAT | O_TRUNC` for `>`, `O_WRONLY | O_CREAT | O_APPEND` for `>>`, mode `0644`).
  - `dup2(oldfd, newfd)` atomically closes `newfd` (if open) and duplicates `oldfd` onto `newfd` (`STDIN_FILENO` = 0 or `STDOUT_FILENO` = 1).
  - `close(fd)` releases the temporary descriptor so file descriptors are not leaked into the executed program.
- **Standalone Built-ins**: When a standalone built-in uses redirection in the parent process, `dup()` saves the original `STDIN_FILENO` and `STDOUT_FILENO` descriptors beforehand and `dup2()` restores them after the built-in finishes.

## 6. `setpgid()` — Process Group Management (`src/jobs.cpp`, `src/executor.cpp`)
- **Purpose**: Assigns a process to a process group (`pgid`). Signals sent to a terminal's foreground group (such as `SIGINT` from `Ctrl+C` or `SIGTSTP` from `Ctrl+Z`) are delivered to every process in that process group simultaneously.
- **How ShellCraft uses it**:
  - At startup, the interactive shell places itself in its own process group (`setpgid(shell_pid, shell_pid)`).
  - When launching a job or pipeline, the first child's PID becomes the job's `pgid`, and every process in the pipeline is placed into `pgid`. Both parent and child call `setpgid()` to eliminate scheduling race conditions.

## 7. `tcsetpgrp()` and `tcgetpgrp()` — Controlling Terminal Ownership (`src/jobs.cpp`, `src/executor.cpp`)
- **Purpose**: Sets or queries which process group currently owns the foreground of the controlling terminal (`STDIN_FILENO`).
- **How ShellCraft uses it**:
  - When launching or resuming (`fg %n`) a foreground job in interactive mode, the shell calls `tcsetpgrp(STDIN_FILENO, job.pgid)` so the job can read terminal input and receive terminal-generated signals (`Ctrl+C`, `Ctrl+Z`).
  - When the foreground job terminates or stops, the shell calls `tcsetpgrp(STDIN_FILENO, shell_pgid)` to reclaim foreground terminal control before printing the next `shellcraft> ` prompt.

## 8. `sigaction()` and `kill()` — Signal Handling and Job Control (`src/jobs.cpp`)
- **Purpose**:
  - `sigaction()` reliably configures signal dispositions (`SIGCHLD`, `SIGINT`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`, `SIGQUIT`).
  - `kill(-pgid, SIGCONT)` sends `SIGCONT` to every process in process group `pgid` when resuming a stopped job via `fg %n` or `bg %n`.
