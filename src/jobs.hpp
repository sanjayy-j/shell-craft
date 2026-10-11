#ifndef SHELLCRAFT_JOBS_HPP
#define SHELLCRAFT_JOBS_HPP

#include <csignal>
#include <string>
#include <termios.h>
#include <sys/types.h>
#include <vector>

namespace shellcraft {

enum class JobState {
    RUNNING,
    STOPPED,
    DONE
};

struct ProcessInfo {
    pid_t pid = -1;
    bool running = true;
    bool stopped = false;
    bool completed = false;
    int status = 0;
};

struct Job {
    int job_id = 0;
    pid_t pgid = 0;
    std::string command;
    JobState state = JobState::RUNNING;
    std::vector<ProcessInfo> processes;
    bool background = false;
    bool notified = false;
    int last_exit_status = 0;
};

// Minimal async-signal-safe flags set by signal handlers
extern volatile sig_atomic_t g_sigchld_flag;
extern volatile sig_atomic_t g_sigint_flag;

// Initialize shell process group, terminal control, and signal handlers
void init_shell_Subsystems();

// Restore default signal dispositions in a forked child process before execvp()
void restore_child_signals();

// Check whether the shell is running interactively attached to a terminal
bool is_shell_interactive();

// Return the shell's own process group ID
pid_t get_shell_pgid();

// Give terminal foreground control to the specified process group (if interactive)
bool give_terminal_to(pid_t pgid);

// Reclaim terminal foreground control for the shell (if interactive)
void reclaim_terminal();

class JobManager {
public:
    // Add a newly launched job to the job table and return its assigned job_id
    int add_job(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, bool background);

    // Reap any state-changed child processes using nonblocking waitpid()
    void reap_children();

    // Print notifications for completed/stopped background jobs and remove DONE jobs
    void notify_and_cleanup(bool print_done_notifications);

    // Wait for a foreground job until it completes or stops, then reclaim terminal
    int wait_for_foreground_job(int job_id);

    // Print all jobs for the 'jobs' built-in command
    void print_jobs();

    // Bring job_id to the foreground ('fg %n')
    bool bring_job_to_foreground(int job_id, int& exit_status_out);

    // Resume a stopped job in the background ('bg %n')
    bool resume_job_in_background(int job_id);

    // Find a job by job_id (or return the most recent job if job_id == -1)
    Job* find_job(int job_id);

    // Parse a job specifier like "%1" or "1" (returns -1 on invalid syntax)
    static int parse_job_spec(const std::string& spec);

private:
    std::vector<Job> jobs_;

    int get_next_job_id() const;
    bool update_process_status(pid_t pid, int status);
    void refresh_job_state(Job& job);
    void remove_job(int job_id);
};

// Global JobManager instance for the shell session
JobManager& get_job_manager();

} // namespace shellcraft

#endif // SHELLCRAFT_JOBS_HPP
