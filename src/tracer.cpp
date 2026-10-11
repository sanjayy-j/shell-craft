#include "tracer.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace shellcraft {

namespace {

std::uint64_t now_steady_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string redir_op_string(RedirectType t) {
    switch (t) {
        case RedirectType::INPUT:
            return "<";
        case RedirectType::OUTPUT_TRUNC:
            return ">";
        case RedirectType::OUTPUT_APPEND:
            return ">>";
    }
    return "?";
}

std::string json_escape(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b";  break;
            case '\f': oss << "\\f";  break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    oss << static_cast<char>(c);
                }
                break;
        }
    }
    return oss.str();
}

} // namespace

std::string trace_event_type_name(TraceEventType type) {
    switch (type) {
        case TraceEventType::COMMAND_PARSED:        return "COMMAND_PARSED";
        case TraceEventType::PLAN_CREATED:          return "PLAN_CREATED";
        case TraceEventType::PIPE_CREATED:          return "PIPE_CREATED";
        case TraceEventType::PIPE_FAILED:           return "PIPE_FAILED";
        case TraceEventType::FORK_SUCCEEDED:        return "FORK_SUCCEEDED";
        case TraceEventType::FORK_FAILED:           return "FORK_FAILED";
        case TraceEventType::PGID_ASSIGNED:         return "PGID_ASSIGNED";
        case TraceEventType::REDIRECTION_SUCCEEDED: return "REDIRECTION_SUCCEEDED";
        case TraceEventType::REDIRECTION_FAILED:    return "REDIRECTION_FAILED";
        case TraceEventType::BUILTIN_EXECUTED:      return "BUILTIN_EXECUTED";
        case TraceEventType::EXECVP_ATTEMPTED:      return "EXECVP_ATTEMPTED";
        case TraceEventType::EXECVP_SUCCEEDED:      return "EXECVP_SUCCEEDED";
        case TraceEventType::EXECVP_FAILED:         return "EXECVP_FAILED";
        case TraceEventType::TERMINAL_TRANSFERRED:  return "TERMINAL_TRANSFERRED";
        case TraceEventType::TERMINAL_RECLAIMED:    return "TERMINAL_RECLAIMED";
        case TraceEventType::WAITPID_COMPLETED:     return "WAITPID_COMPLETED";
        case TraceEventType::WAITPID_FAILED:        return "WAITPID_FAILED";
        case TraceEventType::PROCESS_EXITED:        return "PROCESS_EXITED";
        case TraceEventType::PROCESS_SIGNALED:      return "PROCESS_SIGNALED";
        case TraceEventType::PROCESS_STOPPED:       return "PROCESS_STOPPED";
        case TraceEventType::PROCESS_CONTINUED:     return "PROCESS_CONTINUED";
    }
    return "UNKNOWN_EVENT";
}

std::string errno_symbolic_name(int err) {
    switch (err) {
        case 0:      return "OK";
        case ENOENT: return "ENOENT";
        case EACCES: return "EACCES";
        case EPERM:  return "EPERM";
        case EEXIST: return "EEXIST";
        case EISDIR: return "EISDIR";
        case ENOTDIR:return "ENOTDIR";
        case EMFILE: return "EMFILE";
        case ENFILE: return "ENFILE";
        case EAGAIN: return "EAGAIN";
        case ENOMEM: return "ENOMEM";
        case EINTR:  return "EINTR";
        case ECHILD: return "ECHILD";
        case ESRCH:  return "ESRCH";
        case EBADF:  return "EBADF";
        case ENOTTY: return "ENOTTY";
        case EPIPE:  return "EPIPE";
        case ENOEXEC:return "ENOEXEC";
        default:     return "ERRNO_" + std::to_string(err);
    }
}

std::string signal_symbolic_name(int sig) {
    switch (sig) {
        case SIGINT:  return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGSEGV: return "SIGSEGV";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
        case SIGCHLD: return "SIGCHLD";
        case SIGCONT: return "SIGCONT";
        case SIGSTOP: return "SIGSTOP";
        case SIGTSTP: return "SIGTSTP";
        case SIGTTIN: return "SIGTTIN";
        case SIGTTOU: return "SIGTTOU";
        default:      return "SIG_" + std::to_string(sig);
    }
}

std::uint64_t TraceRecorder::begin_trace(const ExecutionPlan& plan, bool explicit_trace_mode) {
    if (history_.size() >= MAX_TRACE_HISTORY) {
        history_.pop_front();
    }

    ExecutionTrace trace;
    trace.trace_id = next_trace_id_++;
    trace.raw_command = plan.raw_command;
    trace.explicit_trace_mode = explicit_trace_mode;
    trace.plan = plan;
    trace.parent_pid = getpid();
    trace.background = plan.background;
    trace.start_steady_us = now_steady_us();

    for (const auto& pstage : plan.stages) {
        StageOutcome so;
        so.stage_index = pstage.stage_index;
        so.args = pstage.args;
        so.formatted_command = pstage.formatted_command;
        so.is_builtin = pstage.is_builtin;
        so.forks_child = pstage.forks_child;
        so.redir_expected = pstage.redirections.size();
        trace.stages.push_back(so);
    }

    history_.push_back(trace);
    std::uint64_t id = history_.back().trace_id;

    add_event(id,
              TraceEventType::COMMAND_PARSED,
              -1,
              trace.parent_pid,
              0,
              0,
              true,
              0,
              static_cast<int>(plan.num_stages),
              "Parsed command \"" + plan.raw_command + "\" into " +
                  std::to_string(plan.num_stages) + " stage(s)",
              "Tokenizer and syntax validator produced structured Pipeline AST.");

    add_event(id,
              TraceEventType::PLAN_CREATED,
              -1,
              trace.parent_pid,
              0,
              0,
              true,
              0,
              static_cast<int>(plan.expected_child_launches),
              "Created execution plan: expected_forks=" +
                  std::to_string(plan.expected_child_launches) +
                  ", expected_execvp=" + std::to_string(plan.expected_external_launches) +
                  ", pipes=" + std::to_string(plan.pipes_required) +
                  ", redirections=" + std::to_string(plan.total_redirections),
              "Pre-execution planner derived expected OS operations prior to running.");

    return id;
}

void TraceRecorder::add_event(std::uint64_t trace_id,
                              TraceEventType type,
                              int stage_index,
                              pid_t pid,
                              pid_t pgid,
                              int job_id,
                              bool success,
                              int err_no,
                              int code,
                              const std::string& fact,
                              const std::string& annotation) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace) {
        return;
    }

    TraceEvent ev;
    ev.seq = trace->events.size() + 1;
    std::uint64_t now = now_steady_us();
    ev.elapsed_us = (now >= trace->start_steady_us) ? (now - trace->start_steady_us) : 0;
    ev.type = type;
    ev.stage_index = stage_index;
    ev.pid = pid;
    ev.pgid = pgid;
    ev.job_id = job_id;
    ev.success = success;
    ev.err_no = err_no;
    ev.code = code;
    ev.fact = fact;
    ev.annotation = annotation;

    trace->events.push_back(ev);
}

void TraceRecorder::record_pipe(std::uint64_t trace_id,
                                int pipe_index,
                                int read_fd,
                                int write_fd,
                                bool ok,
                                int err_no) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace) {
        return;
    }

    if (ok) {
        ++trace->pipes_created;
        add_event(trace_id,
                  TraceEventType::PIPE_CREATED,
                  pipe_index - 1,
                  trace->parent_pid,
                  trace->pgid,
                  trace->job_id,
                  true,
                  0,
                  pipe_index,
                  "pipe() #" + std::to_string(pipe_index) + " created [read_fd=" +
                      std::to_string(read_fd) + ", write_fd=" + std::to_string(write_fd) + "]",
                  "Connects Stage " + std::to_string(pipe_index) + " stdout to Stage " +
                      std::to_string(pipe_index + 1) + " stdin.");
    } else {
        ++trace->pipes_failed;
        trace->last_pipe_errno = err_no;
        add_event(trace_id,
                  TraceEventType::PIPE_FAILED,
                  pipe_index - 1,
                  trace->parent_pid,
                  trace->pgid,
                  trace->job_id,
                  false,
                  err_no,
                  pipe_index,
                  "pipe() #" + std::to_string(pipe_index) + " failed: " +
                      errno_symbolic_name(err_no) + " (" + std::strerror(err_no) + ")",
                  "Pipeline setup aborted because kernel pipe allocation failed.");
    }
}

void TraceRecorder::record_fork(std::uint64_t trace_id,
                                std::size_t stage_idx,
                                pid_t child_pid,
                                pid_t pgid,
                                bool ok,
                                int err_no) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }

    StageOutcome& stage = trace->stages[stage_idx];
    stage.fork_attempted = true;

    if (ok) {
        stage.fork_succeeded = true;
        stage.pid = child_pid;
        add_event(trace_id,
                  TraceEventType::FORK_SUCCEEDED,
                  static_cast<int>(stage_idx),
                  child_pid,
                  pgid,
                  trace->job_id,
                  true,
                  0,
                  static_cast<int>(child_pid),
                  "fork() succeeded for Stage " + std::to_string(stage_idx + 1) +
                      " (" + stage.formatted_command + "): parent_pid=" +
                      std::to_string(trace->parent_pid) + ", child_pid=" + std::to_string(child_pid),
                  "Parent cloned address space for pipeline stage execution.");
    } else {
        stage.fork_succeeded = false;
        stage.fork_errno = err_no;
        add_event(trace_id,
                  TraceEventType::FORK_FAILED,
                  static_cast<int>(stage_idx),
                  trace->parent_pid,
                  pgid,
                  trace->job_id,
                  false,
                  err_no,
                  -1,
                  "fork() failed for Stage " + std::to_string(stage_idx + 1) + ": " +
                      errno_symbolic_name(err_no) + " (" + std::strerror(err_no) + ")",
                  "Child process creation failed.");
    }
}

void TraceRecorder::record_pgid(std::uint64_t trace_id,
                                std::size_t stage_idx,
                                pid_t pid,
                                pid_t pgid,
                                bool ok,
                                int err_no) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace) {
        return;
    }
    trace->pgid = pgid;
    if (ok) {
        add_event(trace_id,
                  TraceEventType::PGID_ASSIGNED,
                  static_cast<int>(stage_idx),
                  pid,
                  pgid,
                  trace->job_id,
                  true,
                  0,
                  static_cast<int>(pgid),
                  "setpgid(pid=" + std::to_string(pid) + ", pgid=" + std::to_string(pgid) + ") succeeded",
                  "Stage placed into job process group for terminal and signal control.");
    } else {
        add_event(trace_id,
                  TraceEventType::PGID_ASSIGNED,
                  static_cast<int>(stage_idx),
                  pid,
                  pgid,
                  trace->job_id,
                  false,
                  err_no,
                  static_cast<int>(pgid),
                  "setpgid(pid=" + std::to_string(pid) + ", pgid=" + std::to_string(pgid) +
                      ") failed: " + errno_symbolic_name(err_no) + " (" + std::strerror(err_no) + ")",
                  "Process group assignment reported an error.");
    }
}

void TraceRecorder::record_redirection(std::uint64_t trace_id,
                                       std::size_t stage_idx,
                                       std::size_t redir_idx,
                                       pid_t pid,
                                       bool ok,
                                       int err_no) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }

    StageOutcome& stage = trace->stages[stage_idx];
    std::string op_str = "?";
    std::string file_str = "?";
    if (stage_idx < trace->plan.stages.size() &&
        redir_idx < trace->plan.stages[stage_idx].redirections.size()) {
        const auto& pr = trace->plan.stages[stage_idx].redirections[redir_idx];
        op_str = redir_op_string(pr.type);
        file_str = pr.filename;
    }

    if (ok) {
        ++stage.redir_succeeded;
        add_event(trace_id,
                  TraceEventType::REDIRECTION_SUCCEEDED,
                  static_cast<int>(stage_idx),
                  pid,
                  trace->pgid,
                  trace->job_id,
                  true,
                  0,
                  static_cast<int>(redir_idx),
                  "Redirection '" + op_str + " " + file_str + "' configured via open() + dup2()",
                  "Standard stream redirected for Stage " + std::to_string(stage_idx + 1) + ".");
    } else {
        stage.redir_failed = true;
        stage.redir_errno = err_no;
        stage.failed_redir_op = op_str;
        stage.failed_redir_file = file_str;
        add_event(trace_id,
                  TraceEventType::REDIRECTION_FAILED,
                  static_cast<int>(stage_idx),
                  pid,
                  trace->pgid,
                  trace->job_id,
                  false,
                  err_no,
                  static_cast<int>(redir_idx),
                  "Redirection '" + op_str + " " + file_str + "' failed: " +
                      errno_symbolic_name(err_no) + " (" + std::strerror(err_no) + ")",
                  "open() or dup2() failed before command execution could start.");
    }
}

void TraceRecorder::record_builtin(std::uint64_t trace_id,
                                   std::size_t stage_idx,
                                   pid_t pid,
                                   int exit_status) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }

    StageOutcome& stage = trace->stages[stage_idx];
    stage.builtin_executed = true;
    stage.builtin_exit_status = exit_status;
    if (!stage.forks_child) {
        stage.wait_collected = true;
        stage.exited_normally = true;
        stage.exit_status = exit_status;
    }

    add_event(trace_id,
              TraceEventType::BUILTIN_EXECUTED,
              static_cast<int>(stage_idx),
              pid,
              trace->pgid,
              trace->job_id,
              exit_status == 0,
              0,
              exit_status,
              "Executed built-in '" + stage.formatted_command + "' (status=" +
                  std::to_string(exit_status) + ")",
              stage.forks_child ? "Built-in executed inside pipeline child process."
                                : "Standalone built-in executed directly in parent shell process.");
}

void TraceRecorder::record_exec_attempt(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }
    StageOutcome& stage = trace->stages[stage_idx];
    stage.exec_attempted = true;
    std::string prog = stage.args.empty() ? "" : stage.args[0];

    add_event(trace_id,
              TraceEventType::EXECVP_ATTEMPTED,
              static_cast<int>(stage_idx),
              pid,
              trace->pgid,
              trace->job_id,
              true,
              0,
              0,
              "Child pid=" + std::to_string(pid) + " invoking execvp(\"" + prog + "\")",
              "Child process attempting to replace its process image via PATH lookup.");
}

void TraceRecorder::record_exec_Debounce_success(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }
    StageOutcome& stage = trace->stages[stage_idx];
    stage.exec_succeeded = true;
    std::string prog = stage.args.empty() ? "" : stage.args[0];

    add_event(trace_id,
              TraceEventType::EXECVP_SUCCEEDED,
              static_cast<int>(stage_idx),
              pid,
              trace->pgid,
              trace->job_id,
              true,
              0,
              0,
              "execvp(\"" + prog + "\") succeeded in pid=" + std::to_string(pid) +
                  " (FD_CLOEXEC status pipe closed on exec)",
              "Kernel replaced child process image with target executable.");
}

void TraceRecorder::record_exec_failure(std::uint64_t trace_id,
                                        std::size_t stage_idx,
                                        pid_t pid,
                                        int err_no,
                                        int exit_code) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace || stage_idx >= trace->stages.size()) {
        return;
    }
    StageOutcome& stage = trace->stages[stage_idx];
    stage.exec_failed = true;
    stage.exec_errno = err_no;
    std::string prog = stage.args.empty() ? "" : stage.args[0];

    add_event(trace_id,
              TraceEventType::EXECVP_FAILED,
              static_cast<int>(stage_idx),
              pid,
              trace->pgid,
              trace->job_id,
              false,
              err_no,
              exit_code,
              "execvp(\"" + prog + "\") failed in pid=" + std::to_string(pid) + ": " +
                  errno_symbolic_name(err_no) + " (" + std::strerror(err_no) + ")",
              "Child could not load program image and exited with status " +
                  std::to_string(exit_code) + ".");
}

void TraceRecorder::set_job_context(std::uint64_t trace_id, int job_id, pid_t pgid) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace) {
        return;
    }
    trace->job_id = job_id;
    trace->pgid = pgid;
}

void TraceRecorder::record_terminal_transfer(pid_t pgid, bool ok, int err_no) {
    ExecutionTrace* trace = find_trace_by_pgid(pgid);
    if (!trace) {
        return;
    }
    trace->terminal_transferred = ok;
    add_event(trace->trace_id,
              TraceEventType::TERMINAL_TRANSFERRED,
              -1,
              trace->parent_pid,
              pgid,
              trace->job_id,
              ok,
              err_no,
              static_cast<int>(pgid),
              ok ? ("tcsetpgrp(STDIN_FILENO, pgid=" + std::to_string(pgid) + ") transferred terminal to foreground job")
                 : ("tcsetpgrp(STDIN_FILENO, pgid=" + std::to_string(pgid) + ") failed: " + std::strerror(err_no)),
              "Controlling terminal foreground group updated for job execution.");
}

void TraceRecorder::record_terminal_reclaim(pid_t shell_pgid, bool ok, int err_no) {
    if (history_.empty()) {
        return;
    }
    ExecutionTrace& trace = history_.back();
    trace.terminal_reclaimed = ok;
    add_event(trace.trace_id,
              TraceEventType::TERMINAL_RECLAIMED,
              -1,
              trace.parent_pid,
              shell_pgid,
              trace.job_id,
              ok,
              err_no,
              static_cast<int>(shell_pgid),
              ok ? ("tcsetpgrp(STDIN_FILENO, shell_pgid=" + std::to_string(shell_pgid) + ") reclaimed terminal for shell")
                 : ("tcsetpgrp(STDIN_FILENO, shell_pgid=" + std::to_string(shell_pgid) + ") failed: " + std::strerror(err_no)),
              "Shell restored itself as foreground process group of controlling terminal.");
}

void TraceRecorder::record_waitpid_status(pid_t pid, int raw_status) {
    ExecutionTrace* trace = find_trace_by_pid(pid);
    if (!trace) {
        return;
    }

    int stage_idx = -1;
    StageOutcome* stage_ptr = nullptr;
    for (auto& st : trace->stages) {
        if (st.pid == pid) {
            stage_idx = static_cast<int>(st.stage_index);
            stage_ptr = &st;
            break;
        }
    }
    if (!stage_ptr) {
        return;
    }

    add_event(trace->trace_id,
              TraceEventType::WAITPID_COMPLETED,
              stage_idx,
              pid,
              trace->pgid,
              trace->job_id,
              true,
              0,
              raw_status,
              "waitpid() returned pid=" + std::to_string(pid) + " for Stage " +
                  std::to_string(stage_idx + 1),
              "Parent shell observed child process state transition.");

    if (WIFEXITED(raw_status)) {
        int code = WEXITSTATUS(raw_status);
        stage_ptr->wait_collected = true;
        stage_ptr->exited_normally = true;
        stage_ptr->exit_status = code;
        stage_ptr->stopped = false;

        add_event(trace->trace_id,
                  TraceEventType::PROCESS_EXITED,
                  stage_idx,
                  pid,
                  trace->pgid,
                  trace->job_id,
                  code == 0,
                  0,
                  code,
                  "Stage " + std::to_string(stage_idx + 1) + " (pid=" + std::to_string(pid) +
                      ") exited normally with status " + std::to_string(code),
                  code == 0 ? "Child process completed with success exit code 0."
                            : "Child process terminated normally with non-zero exit status.");
    } else if (WIFSIGNALED(raw_status)) {
        int sig = WTERMSIG(raw_status);
        stage_ptr->wait_collected = true;
        stage_ptr->signaled = true;
        stage_ptr->term_signal = sig;
        stage_ptr->exit_status = 128 + sig;
        stage_ptr->stopped = false;

        add_event(trace->trace_id,
                  TraceEventType::PROCESS_SIGNALED,
                  stage_idx,
                  pid,
                  trace->pgid,
                  trace->job_id,
                  false,
                  0,
                  sig,
                  "Stage " + std::to_string(stage_idx + 1) + " (pid=" + std::to_string(pid) +
                      ") terminated by signal " + std::to_string(sig) + " (" +
                      signal_symbolic_name(sig) + ")",
                  "Child process was killed by an unhandled signal.");
    } else if (WIFSTOPPED(raw_status)) {
        int sig = WSTOPSIG(raw_status);
        stage_ptr->stopped = true;
        stage_ptr->stop_signal = sig;

        add_event(trace->trace_id,
                  TraceEventType::PROCESS_STOPPED,
                  stage_idx,
                  pid,
                  trace->pgid,
                  trace->job_id,
                  true,
                  0,
                  sig,
                  "Stage " + std::to_string(stage_idx + 1) + " (pid=" + std::to_string(pid) +
                      ") stopped by signal " + std::to_string(sig) + " (" +
                      signal_symbolic_name(sig) + ")",
                  "Process suspended (e.g., via Ctrl+Z); can be resumed with fg or bg.");
    } else if (WIFCONTINUED(raw_status)) {
        stage_ptr->stopped = false;
        stage_ptr->continued = true;

        add_event(trace->trace_id,
                  TraceEventType::PROCESS_CONTINUED,
                  stage_idx,
                  pid,
                  trace->pgid,
                  trace->job_id,
                  true,
                  0,
                  SIGCONT,
                  "Stage " + std::to_string(stage_idx + 1) + " (pid=" + std::to_string(pid) +
                      ") resumed via SIGCONT",
                  "Stopped process resumed execution.");
    }
}

void TraceRecorder::finalize_trace(std::uint64_t trace_id) {
    ExecutionTrace* trace = get_trace_by_id(trace_id);
    if (!trace) {
        return;
    }
    std::uint64_t now = now_steady_us();
    trace->duration_us = (now >= trace->start_steady_us) ? (now - trace->start_steady_us) : 0;
}

const ExecutionTrace* TraceRecorder::get_last_trace() const {
    if (history_.empty()) {
        return nullptr;
    }
    return &history_.back();
}

ExecutionTrace* TraceRecorder::get_trace_by_id(std::uint64_t trace_id) {
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->trace_id == trace_id) {
            return &(*it);
        }
    }
    return nullptr;
}

ExecutionTrace* TraceRecorder::find_trace_by_pid(pid_t pid) {
    if (pid <= 0) {
        return nullptr;
    }
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        for (const auto& st : it->stages) {
            if (st.pid == pid) {
                return &(*it);
            }
        }
    }
    return nullptr;
}

ExecutionTrace* TraceRecorder::find_trace_by_pgid(pid_t pgid) {
    if (pgid <= 0) {
        return nullptr;
    }
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->pgid == pgid) {
            return &(*it);
        }
    }
    return nullptr;
}

bool TraceRecorder::print_last_trace() const {
    const ExecutionTrace* trace = get_last_trace();
    if (!trace) {
        std::cerr << "shellcraft: trace: no recorded execution trace available\n";
        return false;
    }
    return print_trace(*trace, true);
}

bool TraceRecorder::print_trace(const ExecutionTrace& trace, bool is_replay) const {
    std::cout << "ShellCraft Runtime Execution Trace"
              << (is_replay ? " (Replay of Recorded History)" : "") << "\n\n";
    std::cout << "Trace ID:      " << trace.trace_id << "\n";
    std::cout << "Command:       " << trace.raw_command << "\n";
    std::cout << "Mode:          " << (trace.background ? "background (&)" : "foreground") << "\n";
    std::cout << "Parent PID:    " << trace.parent_pid << "\n";
    if (trace.pgid > 0) {
        std::cout << "Process Group: " << trace.pgid << " (job #" << trace.job_id << ")\n";
    }
    std::cout << "Elapsed Time:  " << trace.duration_us << " us\n";
    std::cout << "Total Events:  " << trace.events.size() << "\n\n";

    std::cout << "Chronological Event Timeline:\n";
    for (const auto& ev : trace.events) {
        std::cout << "  [" << std::setw(2) << ev.seq << "] +"
                  << std::setw(6) << ev.elapsed_us << " us | "
                  << std::left << std::setw(22) << trace_event_type_name(ev.type) << std::right
                  << " | " << (ev.success ? "OK  " : "FAIL");
        if (ev.stage_index >= 0) {
            std::cout << " | stage=" << (ev.stage_index + 1);
        }
        if (ev.pid > 0) {
            std::cout << " | pid=" << ev.pid;
        }
        std::cout << "\n       Fact:       " << ev.fact << "\n";
        if (!ev.annotation.empty()) {
            std::cout << "       Annotation: " << ev.annotation << "\n";
        }
    }

    std::cout << "\nFinal Observed Stage Statuses:\n";
    for (const auto& st : trace.stages) {
        std::cout << "  Stage " << (st.stage_index + 1) << " (" << st.formatted_command << "): ";
        if (!st.forks_child && st.is_builtin) {
            std::cout << "parent built-in exited with status " << st.exit_status << "\n";
        } else if (!st.fork_succeeded) {
            std::cout << "fork() failed (" << errno_symbolic_name(st.fork_errno) << ")\n";
        } else if (st.redir_failed) {
            std::cout << "pid=" << st.pid << " redirection failed (" << errno_symbolic_name(st.redir_errno)
                      << "), exited with status " << st.exit_status << "\n";
        } else if (st.exec_failed) {
            std::cout << "pid=" << st.pid << " execvp() failed (" << errno_symbolic_name(st.exec_errno)
                      << "), exited with status " << st.exit_status << "\n";
        } else if (st.exited_normally) {
            std::cout << "pid=" << st.pid << " exited normally with status " << st.exit_status << "\n";
        } else if (st.signaled) {
            std::cout << "pid=" << st.pid << " terminated by signal " << st.term_signal
                      << " (" << signal_symbolic_name(st.term_signal) << ")\n";
        } else if (st.stopped) {
            std::cout << "pid=" << st.pid << " stopped by signal " << st.stop_signal
                      << " (" << signal_symbolic_name(st.stop_signal) << ")\n";
        } else {
            std::cout << "pid=" << st.pid << " running / completion not yet collected\n";
        }
    }

    if (is_replay) {
        std::cout << "\nNote: Trace replay displays previously recorded shell events and does not re-execute external commands.\n";
    }
    return true;
}

bool TraceRecorder::export_last_trace_json(const std::string& filepath) const {
    const ExecutionTrace* trace = get_last_trace();
    if (!trace) {
        std::cerr << "shellcraft: trace export: no recorded execution trace to export\n";
        return false;
    }

    std::ofstream out(filepath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "shellcraft: trace export: cannot open '" << filepath << "': "
                  << std::strerror(errno) << "\n";
        return false;
    }

    out << "{\n";
    out << "  \"trace_id\": " << trace->trace_id << ",\n";
    out << "  \"command\": \"" << json_escape(trace->raw_command) << "\",\n";
    out << "  \"background\": " << (trace->background ? "true" : "false") << ",\n";
    out << "  \"parent_pid\": " << trace->parent_pid << ",\n";
    out << "  \"pgid\": " << trace->pgid << ",\n";
    out << "  \"job_id\": " << trace->job_id << ",\n";
    out << "  \"duration_us\": " << trace->duration_us << ",\n";

    out << "  \"plan\": {\n";
    out << "    \"num_stages\": " << trace->plan.num_stages << ",\n";
    out << "    \"expected_child_launches\": " << trace->plan.expected_child_launches << ",\n";
    out << "    \"expected_external_launches\": " << trace->plan.expected_external_launches << ",\n";
    out << "    \"pipes_required\": " << trace->plan.pipes_required << ",\n";
    out << "    \"total_redirections\": " << trace->plan.total_redirections << "\n";
    out << "  },\n";

    out << "  \"stages\": [\n";
    for (std::size_t i = 0; i < trace->stages.size(); ++i) {
        const auto& st = trace->stages[i];
        out << "    {\n";
        out << "      \"stage\": " << (st.stage_index + 1) << ",\n";
        out << "      \"command\": \"" << json_escape(st.formatted_command) << "\",\n";
        out << "      \"is_builtin\": " << (st.is_builtin ? "true" : "false") << ",\n";
        out << "      \"pid\": " << st.pid << ",\n";
        out << "      \"fork_succeeded\": " << (st.fork_succeeded ? "true" : "false") << ",\n";
        out << "      \"redir_expected\": " << st.redir_expected << ",\n";
        out << "      \"redir_succeeded\": " << st.redir_succeeded << ",\n";
        out << "      \"redir_failed\": " << (st.redir_failed ? "true" : "false") << ",\n";
        out << "      \"exec_attempted\": " << (st.exec_attempted ? "true" : "false") << ",\n";
        out << "      \"exec_succeeded\": " << (st.exec_succeeded ? "true" : "false") << ",\n";
        out << "      \"exec_failed\": " << (st.exec_failed ? "true" : "false") << ",\n";
        out << "      \"exec_errno\": \"" << errno_symbolic_name(st.exec_errno) << "\",\n";
        out << "      \"wait_collected\": " << (st.wait_collected ? "true" : "false") << ",\n";
        out << "      \"exited_normally\": " << (st.exited_normally ? "true" : "false") << ",\n";
        out << "      \"exit_status\": " << st.exit_status << ",\n";
        out << "      \"signaled\": " << (st.signaled ? "true" : "false") << ",\n";
        out << "      \"term_signal\": " << st.term_signal << ",\n";
        out << "      \"stopped\": " << (st.stopped ? "true" : "false") << "\n";
        out << "    }" << (i + 1 < trace->stages.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"events\": [\n";
    for (std::size_t i = 0; i < trace->events.size(); ++i) {
        const auto& ev = trace->events[i];
        out << "    {\n";
        out << "      \"seq\": " << ev.seq << ",\n";
        out << "      \"elapsed_us\": " << ev.elapsed_us << ",\n";
        out << "      \"type\": \"" << trace_event_type_name(ev.type) << "\",\n";
        out << "      \"stage\": " << (ev.stage_index >= 0 ? (ev.stage_index + 1) : 0) << ",\n";
        out << "      \"pid\": " << ev.pid << ",\n";
        out << "      \"pgid\": " << ev.pgid << ",\n";
        out << "      \"job_id\": " << ev.job_id << ",\n";
        out << "      \"success\": " << (ev.success ? "true" : "false") << ",\n";
        out << "      \"errno\": " << ev.err_no << ",\n";
        out << "      \"errno_name\": \"" << errno_symbolic_name(ev.err_no) << "\",\n";
        out << "      \"code\": " << ev.code << ",\n";
        out << "      \"fact\": \"" << json_escape(ev.fact) << "\",\n";
        out << "      \"annotation\": \"" << json_escape(ev.annotation) << "\"\n";
        out << "    }" << (i + 1 < trace->events.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    out.close();
    std::cout << "Exported trace #" << trace->trace_id << " (" << trace->events.size()
              << " events) to " << filepath << "\n";
    return true;
}

TraceRecorder& get_trace_recorder() {
    static TraceRecorder instance;
    return instance;
}

} // namespace shellcraft
