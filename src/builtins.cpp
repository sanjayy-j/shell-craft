#include "builtins.hpp"
#include "jobs.hpp"
#include "tracer.hpp"
#include "verifier.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace shellcraft {

namespace {

int builtin_cd(const std::vector<std::string>& args) {
    if (args.size() > 2) {
        std::cerr << "shellcraft: cd: too many arguments\n";
        return 1;
    }

    std::string target_dir;
    if (args.size() == 1 || args[1] == "~") {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') {
            std::cerr << "shellcraft: cd: HOME not set\n";
            return 1;
        }
        target_dir = home;
    } else if (args[1].size() >= 2 && args[1][0] == '~' && args[1][1] == '/') {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') {
            std::cerr << "shellcraft: cd: HOME not set\n";
            return 1;
        }
        target_dir = std::string(home) + args[1].substr(1);
    } else {
        target_dir = args[1];
    }

    if (chdir(target_dir.c_str()) != 0) {
        std::cerr << "shellcraft: cd: " << target_dir << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    return 0;
}

int builtin_pwd() {
    char buffer[4096];
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        std::cerr << "shellcraft: pwd: " << std::strerror(errno) << "\n";
        return 1;
    }
    std::cout << buffer << "\n";
    return 0;
}

int builtin_help() {
    std::cout << "ShellCraft — Custom POSIX Unix Shell (C++17)\n"
              << "\n"
              << "Core built-in commands:\n"
              << "  cd [dir]                Change current working directory (defaults to $HOME)\n"
              << "  pwd                     Print current working directory\n"
              << "  jobs                    List active background and stopped jobs\n"
              << "  fg [%n]                 Bring job %n (or current job) to the foreground\n"
              << "  bg [%n]                 Resume stopped job %n (or current job) in the background\n"
              << "  help                    Display this help message\n"
              << "  exit [code]             Exit ShellCraft cleanly\n"
              << "\n"
              << "Execution Planning, Tracing & Verification commands:\n"
              << "  plan <command>          Predict pipeline stages, pipes, redirections, and file impacts without running\n"
              << "  trace <command>         Execute <command> while recording shell lifecycle events\n"
              << "  trace last              Replay the chronological event trace of the last executed command\n"
              << "  trace export <file>     Export the last recorded trace to a JSON file\n"
              << "  inspect last            Verify predicted plan vs. observed execution & diagnose stages\n"
              << "  diagnose last           Run the Pipeline Failure Analyzer on the last executed command\n"
              << "\n"
              << "Supported shell features:\n"
              << "  External commands       Executed via fork(), execvp(), and waitpid()\n"
              << "  Quoting & escapes       Single quotes ('...'), double quotes (\"...\"), and '\\'\n"
              << "  I/O Redirection         cmd < in.txt, cmd > out.txt, cmd >> out.txt\n"
              << "  Pipelines               cmd1 | cmd2 | cmd3 ...\n"
              << "  Background jobs         cmd &\n"
              << "  Job control             Ctrl+C (SIGINT), Ctrl+Z (SIGTSTP), fg, bg, jobs\n";
    return 0;
}

int builtin_jobs(const std::vector<std::string>& args) {
    if (args.size() > 1) {
        std::cerr << "shellcraft: jobs: unexpected arguments\n";
        return 1;
    }
    get_job_manager().print_jobs();
    return 0;
}

int builtin_fg(const std::vector<std::string>& args) {
    if (args.size() > 2) {
        std::cerr << "shellcraft: fg: usage: fg [%job_id]\n";
        return 1;
    }
    int job_id = -1;
    if (args.size() == 2) {
        job_id = JobManager::parse_job_spec(args[1]);
        if (job_id <= 0) {
            std::cerr << "shellcraft: fg: " << args[1] << ": invalid job specification\n";
            return 1;
        }
    }
    int exit_status = 0;
    if (!get_job_manager().bring_job_to_foreground(job_id, exit_status)) {
        return 1;
    }
    return exit_status;
}

int builtin_bg(const std::vector<std::string>& args) {
    if (args.size() > 2) {
        std::cerr << "shellcraft: bg: usage: bg [%job_id]\n";
        return 1;
    }
    int job_id = -1;
    if (args.size() == 2) {
        job_id = JobManager::parse_job_spec(args[1]);
        if (job_id <= 0) {
            std::cerr << "shellcraft: bg: " << args[1] << ": invalid job specification\n";
            return 1;
        }
    }
    if (!get_job_manager().resume_job_in_background(job_id)) {
        return 1;
    }
    return 0;
}

int builtin_trace_control(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "shellcraft: trace: usage: trace <command> | trace last | trace export <file.json>\n";
        return 2;
    }
    if (args[1] == "last") {
        if (args.size() != 2) {
            std::cerr << "shellcraft: trace last: unexpected arguments\n";
            return 2;
        }
        get_job_manager().reap_children();
        return get_trace_recorder().print_last_trace() ? 0 : 1;
    }
    if (args[1] == "export") {
        if (args.size() != 3) {
            std::cerr << "shellcraft: trace export: usage: trace export <filename.json>\n";
            return 2;
        }
        get_job_manager().reap_children();
        return get_trace_recorder().export_last_trace_json(args[2]) ? 0 : 1;
    }
    std::cerr << "shellcraft: trace: invalid subcommand '" << args[1] << "'\n";
    return 2;
}

int builtin_inspect(const std::vector<std::string>& args) {
    if (args.size() > 2 || (args.size() == 2 && args[1] != "last")) {
        std::cerr << "shellcraft: " << args[0] << ": usage: " << args[0] << " last\n";
        return 2;
    }
    return inspect_last_execution() ? 0 : 1;
}

int builtin_diagnose(const std::vector<std::string>& args) {
    if (args.size() > 2 || (args.size() == 2 && args[1] != "last")) {
        std::cerr << "shellcraft: diagnose: usage: diagnose last\n";
        return 2;
    }
    return diagnose_last_execution() ? 0 : 1;
}

int builtin_exit(const std::vector<std::string>& args, bool& should_exit, int last_exit_status) {
    if (args.size() > 2) {
        std::cerr << "shellcraft: exit: too many arguments\n";
        return 1;
    }
    int code = last_exit_status;
    if (args.size() == 2) {
        char* endptr = nullptr;
        errno = 0;
        long parsed = std::strtol(args[1].c_str(), &endptr, 10);
        if (errno != 0 || endptr == args[1].c_str() || *endptr != '\0') {
            std::cerr << "shellcraft: exit: " << args[1] << ": numeric argument required\n";
            should_exit = true;
            return 2;
        }
        code = static_cast<int>(parsed & 0xFF);
    }
    should_exit = true;
    return code;
}

} // namespace

bool is_builtin_command(const std::string& cmd_name) {
    return cmd_name == "cd" ||
           cmd_name == "pwd" ||
           cmd_name == "exit" ||
           cmd_name == "help" ||
           cmd_name == "jobs" ||
           cmd_name == "fg" ||
           cmd_name == "bg" ||
           cmd_name == "trace" ||
           cmd_name == "inspect" ||
           cmd_name == "verify" ||
           cmd_name == "diagnose";
}

int execute_builtin(const std::vector<std::string>& args, bool& should_exit, int last_exit_status) {
    if (args.empty()) {
        return 0;
    }
    const std::string& cmd = args[0];
    if (cmd == "cd") {
        return builtin_cd(args);
    }
    if (cmd == "pwd") {
        return builtin_pwd();
    }
    if (cmd == "help") {
        return builtin_help();
    }
    if (cmd == "jobs") {
        return builtin_jobs(args);
    }
    if (cmd == "fg") {
        return builtin_fg(args);
    }
    if (cmd == "bg") {
        return builtin_bg(args);
    }
    if (cmd == "trace") {
        return builtin_trace_control(args);
    }
    if (cmd == "inspect" || cmd == "verify") {
        return builtin_inspect(args);
    }
    if (cmd == "diagnose") {
        return builtin_diagnose(args);
    }
    if (cmd == "exit") {
        return builtin_exit(args, should_exit, last_exit_status);
    }
    return 1;
}

} // namespace shellcraft
