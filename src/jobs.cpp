#include "jobs.hpp"
#include "tracer.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace shellcraft {

volatile sig_atomic_t g_sigchld_flag = 0;
volatile sig_atomic_t g_sigint_flag = 0;

namespace {

bool g_is_interactive = false;
pid_t g_shell_pgid = 0;
struct termios g_shell_tmodes;
bool g_has_tmodes = false;

// Minimal async-signal-safe SIGCHLD handler
void sigchld_handler(int /*signum*/) {
    g_sigchld_flag = 1;
}

// Minimal async-signal-safe SIGINT handler for parent shell
void sigint_handler(int /*signum*/) {
    g_sigint_flag = 1;
}

std::string state_to_string(JobState state) {
    switch (state) {
        case JobState::RUNNING:
            return "Running";
        case JobState::STOPPED:
            return "Stopped";
        case JobState::DONE:
            return "Done";
    }
    return "Unknown";
}

} // namespace

void init_shell_Subsystems() {
    g_is_interactive = (isatty(STDIN_FILENO) != 0);

    // Install minimal SIGCHLD handler via sigaction
    struct sigaction sa_chld {};
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, nullptr);

    // Install minimal SIGINT handler (without SA_RESTART so getline wakes on Ctrl+C)
    struct sigaction sa_int {};
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, nullptr);

    // Ignore job-control stop/tty signals in the shell process itself
    struct sigaction sa_ign {};
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGQUIT, &sa_ign, nullptr);
    sigaction(SIGTSTP, &sa_ign, nullptr);
    sigaction(SIGTTIN, &sa_ign, nullptr);
    sigaction(SIGTTOU, &sa_ign, nullptr);

    if (g_is_interactive) {
        g_shell_pgid = getpid();
        if (setpgid(g_shell_pgid, g_shell_pgid) < 0) {
            if (errno != EPERM) {
                std::cerr << "shellcraft: setpgid failed: " << std::strerror(errno) << "\n";
            }
            g_shell_pgid = getpgrp();
        }

        if (tcsetpgrp(STDIN_FILENO, g_shell_pgid) < 0) {
            if (errno == ENOTTY) {
                g_is_interactive = false;
            } else {
                std::cerr << "shellcraft: tcsetpgrp failed: " << std::strerror(errno) << "\n";
            }
        }

        if (g_is_interactive && tcgetattr(STDIN_FILENO, &g_shell_tmodes) == 0) {
            g_has_tmodes = true;
        }
    } else {
        g_shell_pgid = getpgrp();
    }
}

void restore_child_signals() {
    struct sigaction sa_dfl {};
    sa_dfl.sa_handler = SIG_DFL;
    sigemptyset(&sa_dfl.sa_mask);
    sa_dfl.sa_flags = 0;

    sigaction(SIGINT, &sa_dfl, nullptr);
    sigaction(SIGQUIT, &sa_dfl, nullptr);
    sigaction(SIGTSTP, &sa_dfl, nullptr);
    sigaction(SIGTTIN, &sa_dfl, nullptr);
    sigaction(SIGTTOU, &sa_dfl, nullptr);
    sigaction(SIGCHLD, &sa_dfl, nullptr);
}

bool is_shell_interactive() {
    return g_is_interactive;
}

pid_t get_shell_pgid() {
    return g_shell_pgid;
}

bool give_terminal_to(pid_t pgid) {
    if (!g_is_interactive || pgid <= 0) {
        return true;
    }
    if (tcsetpgrp(STDIN_FILENO, pgid) < 0) {
        get_trace_recorder().record_terminal_transfer(pgid, false, errno);
        return false;
    }
    get_trace_recorder().record_terminal_transfer(pgid, true, 0);
    return true;
}

void reclaim_terminal() {
    if (!g_is_interactive) {
        return;
    }
    if (tcsetpgrp(STDIN_FILENO, g_shell_pgid) < 0) {
        int err = errno;
        get_trace_recorder().record_terminal_reclaim(g_shell_pgid, false, err);
        std::cerr << "shellcraft: failed to reclaim terminal: " << std::strerror(err) << "\n";
    } else {
        get_trace_recorder().record_terminal_reclaim(g_shell_pgid, true, 0);
    }
    if (g_has_tmodes) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_shell_tmodes);
    }
}

int JobManager::get_next_job_id() const {
    int max_id = 0;
    for (const auto& job : jobs_) {
        if (job.job_id > max_id) {
            max_id = job.job_id;
        }
    }
    return max_id + 1;
}

int JobManager::add_job(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, bool background) {
    Job job;
    job.job_id = get_next_job_id();
    job.pgid = pgid;
    job.command = command;
    job.state = JobState::RUNNING;
    job.background = background;
    job.notified = false;

    for (pid_t pid : pids) {
        ProcessInfo pinfo;
        pinfo.pid = pid;
        pinfo.running = true;
        pinfo.stopped = false;
        pinfo.completed = false;
        pinfo.status = 0;
        job.processes.push_back(pinfo);
    }

    jobs_.push_back(job);
    return job.job_id;
}

void JobManager::refresh_job_state(Job& job) {
    bool all_completed = true;
    bool any_stopped = false;

    for (const auto& proc : job.processes) {
        if (!proc.completed) {
            all_completed = false;
            if (proc.stopped) {
                any_stopped = true;
            }
        }
    }

    if (all_completed) {
        job.state = JobState::DONE;
    } else if (any_stopped) {
        job.state = JobState::STOPPED;
    } else {
        job.state = JobState::RUNNING;
    }
}

bool JobManager::update_process_status(pid_t pid, int status) {
    if (pid <= 0) {
        return false;
    }

    for (auto& job : jobs_) {
        for (std::size_t i = 0; i < job.processes.size(); ++i) {
            auto& proc = job.processes[i];
            if (proc.pid == pid) {
                proc.status = status;
                if (WIFSTOPPED(status)) {
                    proc.stopped = true;
                    proc.running = false;
                    job.notified = false;
                } else if (WIFCONTINUED(status)) {
                    proc.stopped = false;
                    proc.running = true;
                    job.notified = false;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    proc.completed = true;
                    proc.running = false;
                    proc.stopped = false;
                    if (i + 1 == job.processes.size()) {
                        if (WIFEXITED(status)) {
                            job.last_exit_status = WEXITSTATUS(status);
                        } else if (WIFSIGNALED(status)) {
                            job.last_exit_status = 128 + WTERMSIG(status);
                        }
                    }
                }
                refresh_job_state(job);
                get_trace_recorder().record_waitpid_status(pid, status);
                return true;
            }
        }
    }
    return false;
}

void JobManager::reap_children() {
    g_sigchld_flag = 0;
    int status = 0;
    pid_t pid = 0;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        update_process_status(pid, status);
    }
}

void JobManager::notify_and_cleanup(bool print_done_notifications) {
    reap_children();

    auto it = jobs_.begin();
    while (it != jobs_.end()) {
        if (it->state == JobState::DONE) {
            if (print_done_notifications) {
                std::cout << "[" << it->job_id << "] Done                    " << it->command << "\n";
            }
            it = jobs_.erase(it);
        } else if (it->state == JobState::STOPPED && !it->notified) {
            std::cout << "[" << it->job_id << "] Stopped                 " << it->command << "\n";
            it->notified = true;
            ++it;
        } else {
            ++it;
        }
    }
}

int JobManager::wait_for_foreground_job(int job_id) {
    Job* job = find_job(job_id);
    if (!job) {
        return 0;
    }

    bool interrupted_by_sigint = false;

    while (true) {
        job = find_job(job_id);
        if (!job || job->state == JobState::DONE || job->state == JobState::STOPPED) {
            break;
        }

        int status = 0;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                job = find_job(job_id);
                if (job) {
                    for (auto& proc : job->processes) {
                        proc.completed = true;
                        proc.running = false;
                    }
                    job->state = JobState::DONE;
                }
                break;
            }
            break;
        }

        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) {
            interrupted_by_sigint = true;
        }

        update_process_status(pid, status);
    }

    reclaim_terminal();

    job = find_job(job_id);
    if (!job) {
        return 0;
    }

    int exit_status = job->last_exit_status;

    if (interrupted_by_sigint) {
        std::cout << "\n";
    }

    if (job->state == JobState::STOPPED) {
        std::cout << "\n[" << job->job_id << "] Stopped                 " << job->command << "\n";
        job->notified = true;
        exit_status = 148; // 128 + SIGTSTP (20)
    } else if (job->state == JobState::DONE) {
        remove_job(job_id);
    }

    return exit_status;
}

void JobManager::print_jobs() {
    reap_children();

    auto it = jobs_.begin();
    while (it != jobs_.end()) {
        std::cout << "[" << it->job_id << "] " << state_to_string(it->state) << " " << it->command << "\n";
        if (it->state == JobState::DONE) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

Job* JobManager::find_job(int job_id) {
    if (jobs_.empty()) {
        return nullptr;
    }
    if (job_id == -1) {
        return &jobs_.back();
    }
    for (auto& job : jobs_) {
        if (job.job_id == job_id) {
            return &job;
        }
    }
    return nullptr;
}

void JobManager::remove_job(int job_id) {
    jobs_.erase(
        std::remove_if(jobs_.begin(), jobs_.end(),
                       [job_id](const Job& j) { return j.job_id == job_id; }),
        jobs_.end());
}

int JobManager::parse_job_spec(const std::string& spec) {
    if (spec.empty()) {
        return -1;
    }
    std::size_t idx = 0;
    if (spec[0] == '%') {
        idx = 1;
    }
    if (idx >= spec.size()) {
        return -1;
    }
    int val = 0;
    for (; idx < spec.size(); ++idx) {
        if (!std::isdigit(static_cast<unsigned char>(spec[idx]))) {
            return -1;
        }
        val = val * 10 + (spec[idx] - '0');
    }
    return val > 0 ? val : -1;
}

bool JobManager::bring_job_to_foreground(int job_id, int& exit_status_out) {
    reap_children();

    Job* job = find_job(job_id);
    if (!job || job->state == JobState::DONE) {
        if (job_id == -1) {
            std::cerr << "shellcraft: fg: current: no such job\n";
        } else {
            std::cerr << "shellcraft: fg: %" << job_id << ": no such job\n";
        }
        return false;
    }

    int target_id = job->job_id;
    pid_t target_pgid = job->pgid;

    std::cout << job->command << "\n";

    // Hand terminal control to the job's process group before continuing it
    give_terminal_to(target_pgid);

    for (auto& proc : job->processes) {
        if (!proc.completed) {
            proc.stopped = false;
            proc.running = true;
        }
    }
    job->state = JobState::RUNNING;
    job->background = false;
    job->notified = false;

    if (kill(-target_pgid, SIGCONT) < 0) {
        std::cerr << "shellcraft: fg: kill(SIGCONT) failed: " << std::strerror(errno) << "\n";
        reclaim_terminal();
        return false;
    }

    exit_status_out = wait_for_foreground_job(target_id);
    return true;
}

bool JobManager::resume_job_in_background(int job_id) {
    reap_children();

    Job* job = find_job(job_id);
    if (!job || job->state == JobState::DONE) {
        if (job_id == -1) {
            std::cerr << "shellcraft: bg: current: no such job\n";
        } else {
            std::cerr << "shellcraft: bg: %" << job_id << ": no such job\n";
        }
        return false;
    }

    if (job->state == JobState::RUNNING) {
        std::cerr << "shellcraft: bg: job %" << job->job_id << " already in background\n";
        return true;
    }

    for (auto& proc : job->processes) {
        if (!proc.completed) {
            proc.stopped = false;
            proc.running = true;
        }
    }
    job->state = JobState::RUNNING;
    job->background = true;
    job->notified = false;

    if (kill(-job->pgid, SIGCONT) < 0) {
        std::cerr << "shellcraft: bg: kill(SIGCONT) failed: " << std::strerror(errno) << "\n";
        return false;
    }

    std::cout << "[" << job->job_id << "] " << job->command << " &\n";
    return true;
}

JobManager& get_job_manager() {
    static JobManager instance;
    return instance;
}

} // namespace shellcraft
