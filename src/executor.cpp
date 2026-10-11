#include "executor.hpp"
#include "builtins.hpp"
#include "jobs.hpp"
#include "planner.hpp"
#include "tracer.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace shellcraft {

namespace {

bool is_untraced_meta_command(const Pipeline& pipeline) {
    if (pipeline.commands.size() != 1 || pipeline.background) {
        return false;
    }
    const auto& args = pipeline.commands[0].args;
    if (args.empty()) {
        return true;
    }
    const std::string& cmd = args[0];
    return cmd == "plan" ||
           cmd == "trace" ||
           cmd == "inspect" ||
           cmd == "verify" ||
           cmd == "diagnose" ||
           cmd == "help" ||
           cmd == "exit";
}

void send_child_report(int report_fd,
                       ChildReportKind kind,
                       int stage_index,
                       int redir_index,
                       int err_no,
                       int extra) {
    if (report_fd < 0) {
        return;
    }
    ChildReportMsg msg{kind, stage_index, redir_index, err_no, extra};
    ssize_t unused = write(report_fd, &msg, sizeof(msg));
    (void)unused;
}

bool apply_redirections(const Command& cmd,
                        int stage_index,
                        int child_report_fd,
                        std::uint64_t parent_trace_id) {
    for (std::size_t r_idx = 0; r_idx < cmd.redirections.size(); ++r_idx) {
        const auto& redir = cmd.redirections[r_idx];
        int fd = -1;
        int target_fd = -1;

        if (redir.type == RedirectType::INPUT) {
            fd = open(redir.filename.c_str(), O_RDONLY);
            target_fd = STDIN_FILENO;
        } else if (redir.type == RedirectType::OUTPUT_TRUNC) {
            fd = open(redir.filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            target_fd = STDOUT_FILENO;
        } else if (redir.type == RedirectType::OUTPUT_APPEND) {
            fd = open(redir.filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            target_fd = STDOUT_FILENO;
        }

        if (fd < 0) {
            int err = errno;
            std::cerr << "shellcraft: " << redir.filename << ": " << std::strerror(err) << "\n";
            if (child_report_fd >= 0) {
                send_child_report(child_report_fd,
                                  ChildReportKind::REDIR_FAILED,
                                  stage_index,
                                  static_cast<int>(r_idx),
                                  err,
                                  target_fd);
            } else if (parent_trace_id != 0) {
                get_trace_recorder().record_redirection(
                    parent_trace_id, static_cast<std::size_t>(stage_index), r_idx, getpid(), false, err);
            }
            return false;
        }

        if (fd != target_fd) {
            if (dup2(fd, target_fd) < 0) {
                int err = errno;
                std::cerr << "shellcraft: dup2 failed: " << std::strerror(err) << "\n";
                close(fd);
                if (child_report_fd >= 0) {
                    send_child_report(child_report_fd,
                                      ChildReportKind::REDIR_FAILED,
                                      stage_index,
                                      static_cast<int>(r_idx),
                                      err,
                                      target_fd);
                } else if (parent_trace_id != 0) {
                    get_trace_recorder().record_redirection(
                        parent_trace_id, static_cast<std::size_t>(stage_index), r_idx, getpid(), false, err);
                }
                return false;
            }
            if (close(fd) < 0) {
                std::cerr << "shellcraft: close failed: " << std::strerror(errno) << "\n";
                return false;
            }
        }

        if (child_report_fd >= 0) {
            send_child_report(child_report_fd,
                              ChildReportKind::REDIR_OK,
                              stage_index,
                              static_cast<int>(r_idx),
                              0,
                              target_fd);
        } else if (parent_trace_id != 0) {
            get_trace_recorder().record_redirection(
                parent_trace_id, static_cast<std::size_t>(stage_index), r_idx, getpid(), true, 0);
        }
    }
    return true;
}

int execute_standalone_builtin(const Command& cmd,
                               bool& should_exit,
                               int last_exit_status,
                               std::uint64_t trace_id) {
    if (cmd.redirections.empty()) {
        int status = execute_builtin(cmd.args, should_exit, last_exit_status);
        if (trace_id != 0) {
            get_trace_recorder().record_builtin(trace_id, 0, getpid(), status);
            get_trace_recorder().finalize_trace(trace_id);
        }
        return status;
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        std::cerr << "shellcraft: dup failed: " << std::strerror(errno) << "\n";
        if (saved_stdin >= 0) close(saved_stdin);
        if (saved_stdout >= 0) close(saved_stdout);
        if (trace_id != 0) {
            get_trace_recorder().finalize_trace(trace_id);
        }
        return 1;
    }

    int status = 1;
    if (apply_redirections(cmd, 0, -1, trace_id)) {
        status = execute_builtin(cmd.args, should_exit, last_exit_status);
        if (trace_id != 0) {
            get_trace_recorder().record_builtin(trace_id, 0, getpid(), status);
        }
        std::cout.flush();
        std::cerr.flush();
    }

    if (dup2(saved_stdin, STDIN_FILENO) < 0) {
        std::cerr << "shellcraft: failed to restore stdin: " << std::strerror(errno) << "\n";
    }
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) {
        std::cerr << "shellcraft: failed to restore stdout: " << std::strerror(errno) << "\n";
    }
    close(saved_stdin);
    close(saved_stdout);

    if (trace_id != 0) {
        get_trace_recorder().finalize_trace(trace_id);
    }
    return status;
}

void collect_child_reports(std::uint64_t trace_id,
                           int report_read_fd,
                           std::size_t stage_idx,
                           pid_t child_pid) {
    if (report_read_fd < 0) {
        return;
    }

    bool saw_exec_attempt = false;
    bool saw_exec_failed = false;

    ChildReportMsg msg{};
    while (true) {
        ssize_t n = read(report_read_fd, &msg, sizeof(msg));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0 || static_cast<std::size_t>(n) < sizeof(msg)) {
            break;
        }

        if (trace_id == 0) {
            continue;
        }

        switch (msg.kind) {
            case ChildReportKind::REDIR_OK:
                get_trace_recorder().record_redirection(
                    trace_id,
                    stage_idx,
                    static_cast<std::size_t>(msg.redir_index),
                    child_pid,
                    true,
                    0);
                break;
            case ChildReportKind::REDIR_FAILED:
                get_trace_recorder().record_redirection(
                    trace_id,
                    stage_idx,
                    static_cast<std::size_t>(msg.redir_index),
                    child_pid,
                    false,
                    msg.err_no);
                break;
            case ChildReportKind::BUILTIN_RUN:
                get_trace_recorder().record_builtin(trace_id, stage_idx, child_pid, msg.extra);
                break;
            case ChildReportKind::EXEC_ATTEMPT:
                saw_exec_attempt = true;
                get_trace_recorder().record_exec_attempt(trace_id, stage_idx, child_pid);
                break;
            case ChildReportKind::EXEC_FAILED:
                saw_exec_failed = true;
                get_trace_recorder().record_exec_failure(
                    trace_id, stage_idx, child_pid, msg.err_no, msg.extra);
                break;
        }
    }

    // If EXEC_ATTEMPT was sent and the FD_CLOEXEC pipe closed with EOF without EXEC_FAILED,
    // the kernel successfully replaced the child image via execvp().
    if (trace_id != 0 && saw_exec_attempt && !saw_exec_failed) {
        get_trace_recorder().record_exec_Debounce_success(trace_id, stage_idx, child_pid);
    }

    close(report_read_fd);
}

} // namespace

int execute_pipeline(const Pipeline& pipeline,
                     bool& should_exit,
                     int last_exit_status,
                     bool explicit_trace_mode) {
    if (pipeline.commands.empty()) {
        return 0;
    }

    std::uint64_t trace_id = 0;
    if (!is_untraced_meta_command(pipeline) || explicit_trace_mode) {
        ExecutionPlan plan = ExecutionPlanner::create_plan_from_pipeline(pipeline);
        trace_id = get_trace_recorder().begin_trace(plan, explicit_trace_mode);
    }

    // Standalone built-ins execute directly in the parent process
    if (pipeline.commands.size() == 1 &&
        !pipeline.background &&
        !pipeline.commands[0].args.empty() &&
        is_builtin_command(pipeline.commands[0].args[0])) {
        int status = execute_standalone_builtin(
            pipeline.commands[0], should_exit, last_exit_status, trace_id);
        return status;
    }

    const std::size_t num_cmds = pipeline.commands.size();
    pid_t pgid = 0;
    std::vector<pid_t> pids;
    pids.reserve(num_cmds);

    struct StageReportPipe {
        std::size_t stage_idx;
        pid_t child_pid;
        int read_fd;
    };
    std::vector<StageReportPipe> report_pipes;
    report_pipes.reserve(num_cmds);

    int prev_read_fd = -1;
    bool launch_failed = false;

    for (std::size_t i = 0; i < num_cmds; ++i) {
        const Command& cmd = pipeline.commands[i];
        int pipe_fds[2] = {-1, -1};

        if (i + 1 < num_cmds) {
            if (pipe(pipe_fds) < 0) {
                int err = errno;
                std::cerr << "shellcraft: pipe failed: " << std::strerror(err) << "\n";
                if (trace_id != 0) {
                    get_trace_recorder().record_pipe(
                        trace_id, static_cast<int>(i + 1), -1, -1, false, err);
                }
                if (prev_read_fd != -1) {
                    close(prev_read_fd);
                    prev_read_fd = -1;
                }
                launch_failed = true;
                break;
            }
            if (trace_id != 0) {
                get_trace_recorder().record_pipe(
                    trace_id, static_cast<int>(i + 1), pipe_fds[0], pipe_fds[1], true, 0);
            }
        }

        // Create close-on-exec reporting pipe so child can report redirection and execvp outcomes safely
        int report_fds[2] = {-1, -1};
        if (pipe(report_fds) == 0) {
            fcntl(report_fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(report_fds[1], F_SETFD, FD_CLOEXEC);
        }

        // Flush parent streams before fork to prevent duplicate buffered output
        std::cout.flush();
        std::cerr.flush();

        pid_t pid = fork();
        if (pid < 0) {
            int err = errno;
            std::cerr << "shellcraft: fork failed: " << std::strerror(err) << "\n";
            if (trace_id != 0) {
                get_trace_recorder().record_fork(trace_id, i, -1, pgid, false, err);
            }
            if (prev_read_fd != -1) {
                close(prev_read_fd);
                prev_read_fd = -1;
            }
            if (pipe_fds[0] != -1) close(pipe_fds[0]);
            if (pipe_fds[1] != -1) close(pipe_fds[1]);
            if (report_fds[0] != -1) close(report_fds[0]);
            if (report_fds[1] != -1) close(report_fds[1]);
            launch_failed = true;
            break;
        }

        if (pid == 0) {
            // Child process
            if (report_fds[0] != -1) {
                close(report_fds[0]);
            }
            int child_report_fd = report_fds[1];

            // Close any report read fds inherited from earlier stages of this pipeline
            for (const auto& rp : report_pipes) {
                if (rp.read_fd >= 0) {
                    close(rp.read_fd);
                }
            }

            pid_t child_pid = getpid();
            pid_t target_pgid = (pgid == 0) ? child_pid : pgid;
            setpgid(0, target_pgid);

            if (!pipeline.background && is_shell_interactive() && i == 0) {
                tcsetpgrp(STDIN_FILENO, target_pgid);
            }

            restore_child_signals();

            // Connect input from previous pipe stage if present
            if (prev_read_fd != -1) {
                if (dup2(prev_read_fd, STDIN_FILENO) < 0) {
                    std::cerr << "shellcraft: dup2(stdin) failed: " << std::strerror(errno) << "\n";
                    if (child_report_fd != -1) close(child_report_fd);
                    _exit(1);
                }
                close(prev_read_fd);
            }

            // Connect output to next pipe stage if present
            if (i + 1 < num_cmds) {
                if (pipe_fds[0] != -1) {
                    close(pipe_fds[0]);
                }
                if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
                    std::cerr << "shellcraft: dup2(stdout) failed: " << std::strerror(errno) << "\n";
                    if (child_report_fd != -1) close(child_report_fd);
                    _exit(1);
                }
                close(pipe_fds[1]);
            }

            // Apply explicit file redirections (<, >, >>) for this stage
            if (!apply_redirections(cmd, static_cast<int>(i), child_report_fd, 0)) {
                if (child_report_fd != -1) close(child_report_fd);
                _exit(1);
            }

            if (cmd.args.empty()) {
                if (child_report_fd != -1) close(child_report_fd);
                _exit(0);
            }

            // If a built-in appears inside a pipeline or background job, run it in the child
            if (is_builtin_command(cmd.args[0])) {
                send_child_report(child_report_fd,
                                  ChildReportKind::BUILTIN_RUN,
                                  static_cast<int>(i),
                                  -1,
                                  0,
                                  0);
                if (child_report_fd != -1) {
                    close(child_report_fd);
                    child_report_fd = -1;
                }
                bool child_exit = false;
                int status = execute_builtin(cmd.args, child_exit, last_exit_status);
                std::cout.flush();
                std::cerr.flush();
                _exit(status);
            }

            // Prepare null-terminated argv array for execvp
            std::vector<char*> argv_ptrs;
            argv_ptrs.reserve(cmd.args.size() + 1);
            for (const auto& arg : cmd.args) {
                argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
            }
            argv_ptrs.push_back(nullptr);

            send_child_report(child_report_fd,
                              ChildReportKind::EXEC_ATTEMPT,
                              static_cast<int>(i),
                              -1,
                              0,
                              0);

            execvp(argv_ptrs[0], argv_ptrs.data());

            // If execvp returns, execution failed
            int err = errno;
            int exit_code = (err == ENOENT) ? 127 : 126;
            send_child_report(child_report_fd,
                              ChildReportKind::EXEC_FAILED,
                              static_cast<int>(i),
                              -1,
                              err,
                              exit_code);
            if (child_report_fd != -1) {
                close(child_report_fd);
            }

            if (err == ENOENT) {
                std::cerr << "shellcraft: " << cmd.args[0] << ": command not found\n";
            } else {
                std::cerr << "shellcraft: " << cmd.args[0] << ": " << std::strerror(err) << "\n";
            }
            _exit(exit_code);
        }

        // Parent process
        if (report_fds[1] != -1) {
            close(report_fds[1]);
        }

        if (pgid == 0) {
            pgid = pid;
        }

        if (trace_id != 0) {
            get_trace_recorder().record_fork(trace_id, i, pid, pgid, true, 0);
        }

        bool pgid_ok = true;
        int pgid_err = 0;
        if (setpgid(pid, pgid) < 0) {
            if (errno != EACCES && errno != ESRCH && errno != EPERM) {
                pgid_ok = false;
                pgid_err = errno;
                std::cerr << "shellcraft: setpgid failed: " << std::strerror(pgid_err) << "\n";
            }
        }
        if (trace_id != 0) {
            get_trace_recorder().record_pgid(trace_id, i, pid, pgid, pgid_ok, pgid_err);
        }

        pids.push_back(pid);
        report_pipes.push_back({i, pid, report_fds[0]});

        if (prev_read_fd != -1) {
            close(prev_read_fd);
            prev_read_fd = -1;
        }
        if (i + 1 < num_cmds) {
            close(pipe_fds[1]);
            prev_read_fd = pipe_fds[0];
        }
    }

    if (prev_read_fd != -1) {
        close(prev_read_fd);
    }

    // Collect pre-exec child setup and execvp() reports from each forked stage
    for (const auto& rp : report_pipes) {
        collect_child_reports(trace_id, rp.read_fd, rp.stage_idx, rp.child_pid);
    }

    if (pids.empty()) {
        if (trace_id != 0) {
            get_trace_recorder().finalize_trace(trace_id);
        }
        return 1;
    }

    int job_id = get_job_manager().add_job(pgid, pids, pipeline.raw_command, pipeline.background);
    if (trace_id != 0) {
        get_trace_recorder().set_job_context(trace_id, job_id, pgid);
    }

    if (pipeline.background && !launch_failed) {
        std::cout << "[" << job_id << "] " << pids.back() << "\n";
        if (trace_id != 0) {
            get_trace_recorder().finalize_trace(trace_id);
        }
        return 0;
    }

    give_terminal_to(pgid);
    int status = get_job_manager().wait_for_foreground_job(job_id);

    if (trace_id != 0) {
        get_trace_recorder().finalize_trace(trace_id);
    }

    return launch_failed ? 1 : status;
}

} // namespace shellcraft
