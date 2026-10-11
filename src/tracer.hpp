#ifndef SHELLCRAFT_TRACER_HPP
#define SHELLCRAFT_TRACER_HPP

#include "planner.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <sys/types.h>
#include <vector>

namespace shellcraft {

// POD message sent from a forked child to the parent shell over a close-on-exec pipe
enum class ChildReportKind : int {
    REDIR_OK = 1,
    REDIR_FAILED = 2,
    EXEC_ATTEMPT = 3,
    EXEC_FAILED = 4,
    BUILTIN_RUN = 5
};

struct ChildReportMsg {
    ChildReportKind kind;
    int stage_index;
    int redir_index;
    int err_no;
    int extra;
};

enum class TraceEventType {
    COMMAND_PARSED,
    PLAN_CREATED,
    PIPE_CREATED,
    PIPE_FAILED,
    FORK_SUCCEEDED,
    FORK_FAILED,
    PGID_ASSIGNED,
    REDIRECTION_SUCCEEDED,
    REDIRECTION_FAILED,
    BUILTIN_EXECUTED,
    EXECVP_ATTEMPTED,
    EXECVP_SUCCEEDED,
    EXECVP_FAILED,
    TERMINAL_TRANSFERRED,
    TERMINAL_RECLAIMED,
    WAITPID_COMPLETED,
    WAITPID_FAILED,
    PROCESS_EXITED,
    PROCESS_SIGNALED,
    PROCESS_STOPPED,
    PROCESS_CONTINUED
};

struct TraceEvent {
    std::size_t seq = 0;
    std::uint64_t elapsed_us = 0;
    TraceEventType type = TraceEventType::COMMAND_PARSED;
    int stage_index = -1; // -1 if pipeline-wide, 0..N-1 for stage 1..N
    pid_t pid = 0;
    pid_t pgid = 0;
    int job_id = 0;
    bool success = true;
    int err_no = 0;
    int code = 0;
    std::string fact;
    std::string annotation;
};

struct StageOutcome {
    std::size_t stage_index = 0;
    std::vector<std::string> args;
    std::string formatted_command;
    bool is_builtin = false;
    bool forks_child = false;

    bool fork_attempted = false;
    bool fork_succeeded = false;
    int fork_errno = 0;
    pid_t pid = 0;

    std::size_t redir_expected = 0;
    std::size_t redir_succeeded = 0;
    bool redir_failed = false;
    int redir_errno = 0;
    std::string failed_redir_op;
    std::string failed_redir_file;

    bool builtin_executed = false;
    int builtin_exit_status = 0;

    bool exec_attempted = false;
    bool exec_succeeded = false;
    bool exec_failed = false;
    int exec_errno = 0;

    bool wait_collected = false;
    bool exited_normally = false;
    int exit_status = -1;
    bool signaled = false;
    int term_signal = 0;
    bool stopped = false;
    int stop_signal = 0;
    bool continued = false;
};

struct ExecutionTrace {
    std::uint64_t trace_id = 0;
    std::string raw_command;
    bool explicit_trace_mode = false;
    ExecutionPlan plan;

    pid_t parent_pid = 0;
    pid_t pgid = 0;
    int job_id = 0;
    bool background = false;

    std::uint64_t start_steady_us = 0;
    std::uint64_t duration_us = 0;

    std::size_t pipes_created = 0;
    std::size_t pipes_failed = 0;
    int last_pipe_errno = 0;

    bool terminal_transferred = false;
    bool terminal_reclaimed = false;

    std::vector<StageOutcome> stages;
    std::vector<TraceEvent> events;
};

std::string trace_event_type_name(TraceEventType type);
std::string errno_symbolic_name(int err);
std::string signal_symbolic_name(int sig);

class TraceRecorder {
public:
    static constexpr std::size_t MAX_TRACE_HISTORY = 32;

    // Begin recording a new command execution trace from a validated ExecutionPlan
    std::uint64_t begin_trace(const ExecutionPlan& plan, bool explicit_trace_mode);

    // Append an event to a specific trace by trace_id
    void add_event(std::uint64_t trace_id,
                   TraceEventType type,
                   int stage_index,
                   pid_t pid,
                   pid_t pgid,
                   int job_id,
                   bool success,
                   int err_no,
                   int code,
                   const std::string& fact,
                   const std::string& annotation);

    // Record pipe creation outcome
    void record_pipe(std::uint64_t trace_id, int pipe_index, int read_fd, int write_fd, bool ok, int err_no);

    // Record fork outcome for a stage
    void record_fork(std::uint64_t trace_id, std::size_t stage_idx, pid_t child_pid, pid_t pgid, bool ok, int err_no);

    // Record process group assignment
    void record_pgid(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid, pid_t pgid, bool ok, int err_no);

    // Record redirection outcome (in parent or reported from child via close-on-exec pipe)
    void record_redirection(std::uint64_t trace_id,
                            std::size_t stage_idx,
                            std::size_t redir_idx,
                            pid_t pid,
                            bool ok,
                            int err_no);

    // Record built-in execution outcome
    void record_builtin(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid, int exit_status);

    // Record execvp attempt and outcome reported via close-on-exec status pipe
    void record_exec_attempt(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid);
    void record_exec_Debounce_success(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid);
    void record_exec_failure(std::uint64_t trace_id, std::size_t stage_idx, pid_t pid, int err_no, int exit_code);

    // Associate job_id and pgid with the trace
    void set_job_context(std::uint64_t trace_id, int job_id, pid_t pgid);

    // Record terminal foreground transfer / reclamation
    void record_terminal_transfer(pid_t pgid, bool ok, int err_no);
    void record_terminal_reclaim(pid_t shell_pgid, bool ok, int err_no);

    // Record waitpid result for a child pid (automatically matches the owning trace)
    void record_waitpid_status(pid_t pid, int raw_status);

    // Mark trace finished (computes foreground duration)
    void finalize_trace(std::uint64_t trace_id);

    // Retrieve the most recent ExecutionTrace (or nullptr if none)
    const ExecutionTrace* get_last_trace() const;
    ExecutionTrace* get_trace_by_id(std::uint64_t trace_id);

    // Display the last recorded trace chronologically ('trace last')
    bool print_last_trace() const;

    // Print a specific trace by trace_id
    bool print_trace(const ExecutionTrace& trace, bool is_replay) const;

    // Export the last recorded trace to a JSON file ('trace export <path>')
    bool export_last_trace_json(const std::string& filepath) const;

private:
    std::deque<ExecutionTrace> history_;
    std::uint64_t next_trace_id_ = 1;

    ExecutionTrace* find_trace_by_pid(pid_t pid);
    ExecutionTrace* find_trace_by_pgid(pid_t pgid);
};

TraceRecorder& get_trace_recorder();

} // namespace shellcraft

#endif // SHELLCRAFT_TRACER_HPP
