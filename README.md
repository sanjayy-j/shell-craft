# ShellCraft

A custom Unix shell built from scratch in C++ as an Operating Systems Lab project.

ShellCraft is a command-line interpreter that reads user input, parses it into
commands, and executes them using the POSIX process-management API. The goal is
to understand how a real shell works internally — process creation, program
replacement, process synchronisation, file-descriptor manipulation, and signal
delivery — by implementing these mechanisms directly instead of relying on a
library.

---

## Objectives

- Understand how a shell creates and manages processes using `fork()` and the `exec()` family.
- Learn how a parent process synchronises with its children using `wait()` and `waitpid()`.
- Explore file-descriptor manipulation to implement input/output redirection and pipes.
- Understand process groups, sessions, and foreground/background terminal control.
- Learn how signals such as `SIGINT` and `SIGTSTP` are generated, delivered, and handled.
- Gain practical experience writing a non-trivial systems program in C++.

---

## Planned Features

> None of the features below are implemented yet. They describe the intended
> final shell and will be added incrementally, one milestone at a time.

- [ ] Interactive prompt and read–parse–execute loop
- [ ] Basic command execution with arguments
- [ ] Process creation using `fork()` and `exec()`
- [ ] Process synchronisation using `wait()` / `waitpid()`
- [ ] Input redirection (`<`) and output redirection (`>`, `>>`)
- [ ] Pipes (`|`), including multi-stage pipelines
- [ ] Background execution (`&`)
- [ ] Job control built-ins: `jobs`, `fg`, `bg`
- [ ] Signal handling for `Ctrl+C` (`SIGINT`) and `Ctrl+Z` (`SIGTSTP`)
- [ ] Process groups and foreground terminal management
- [ ] Built-in commands such as `cd` and `exit`

---

## Technologies Used

| Component | Details |
|---|---|
| Language | C++17 |
| Compiler | `g++` |
| Build system | GNU Make |
| System API | POSIX (`unistd.h`, `sys/wait.h`, `signal.h`, `fcntl.h`) |
| Platform | Linux / WSL |
| Version control | Git |

> **Note:** ShellCraft relies on POSIX system calls and therefore must be built
> and run on Linux (or WSL on Windows). It will not compile on native Windows.

---

## Project Structure

```
shell-craft/
├── src/
│   └── main.cpp        # Entry point
├── tests/              # Test scripts and sample inputs
├── docs/               # Design notes and lab documentation
├── .gitignore
├── Makefile
└── README.md
```

---

## Build Instructions

Requires `g++` (with C++17 support) and GNU `make`.

```bash
make
```

This compiles the sources in `src/` with `-std=c++17 -Wall -Wextra -g` and
produces an executable named `shellcraft` in the project root. Object files are
placed in `build/`.

## Run Instructions

```bash
./shellcraft
```

Expected output at the current stage:

```
ShellCraft starting...
```

A `make run` target is also provided, which builds the project and runs it in
one step.

## Clean Instructions

```bash
make clean
```

Removes the `shellcraft` executable and the `build/` directory.

---

## Project Status

**Under development.**

The repository currently contains only the project skeleton and a minimal
`main.cpp` that verifies the build toolchain. Shell functionality — parsing,
command execution, redirection, pipes, job control, and signal handling — has
**not** been implemented yet and will be added in subsequent milestones.
