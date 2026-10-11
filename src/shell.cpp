#include "shell.hpp"
#include "executor.hpp"
#include "jobs.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "tracer.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>

namespace shellcraft {

namespace {

// Extracts the first whitespace-delimited keyword and the remainder of the line.
void split_first_keyword(const std::string& line, std::string& keyword, std::string& remainder) {
    keyword.clear();
    remainder.clear();

    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= n || line[i] == '#') {
        return;
    }

    std::size_t start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(line[i])) &&
           line[i] != '|' && line[i] != '<' && line[i] != '>' && line[i] != '&') {
        ++i;
    }
    keyword = line.substr(start, i - start);

    while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i < n) {
        remainder = line.substr(i);
    }
}

bool is_trace_subcommand_invocation(const std::string& remainder) {
    ParseResult sub = parse_command_line(remainder);
    if (!sub.ok || sub.empty || sub.pipeline.commands.size() != 1 || sub.pipeline.background) {
        return false;
    }
    const auto& args = sub.pipeline.commands[0].args;
    if (args.empty()) {
        return false;
    }
    if (args[0] == "last" && args.size() == 1) {
        return true;
    }
    if (args[0] == "export" && (args.size() == 1 || args.size() == 2)) {
        return true;
    }
    return false;
}

} // namespace

int run_shell() {
    init_shell_Subsystems();

    bool should_exit = false;
    int last_exit_status = 0;
    std::string line;

    while (!should_exit) {
        if (is_shell_interactive()) {
            get_job_manager().notify_and_cleanup(true);
            std::cout << "shellcraft> " << std::flush;
        } else {
            get_job_manager().reap_children();
        }

        g_sigint_flag = 0;
        errno = 0;

        if (!std::getline(std::cin, line)) {
            if (g_sigint_flag) {
                g_sigint_flag = 0;
                std::cin.clear();
                clearerr(stdin);
                std::cout << "\n";
                continue;
            }
            if (std::cin.eof()) {
                if (is_shell_interactive()) {
                    std::cout << "\n";
                }
                break;
            }
            if (errno == EINTR) {
                std::cin.clear();
                clearerr(stdin);
                continue;
            }
            break;
        }

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string keyword;
        std::string remainder;
        split_first_keyword(line, keyword, remainder);

        if (keyword.empty()) {
            continue;
        }

        // Feature 1: 'plan <command>' — Pre-Execution Impact Planner
        if (keyword == "plan") {
            if (remainder.empty()) {
                std::cerr << "shellcraft: plan: usage: plan <command>\n";
                last_exit_status = 2;
                continue;
            }
            ParseResult target_parsed = parse_command_line(remainder);
            ExecutionPlan plan = ExecutionPlanner::create_plan(target_parsed);
            ExecutionPlanner::print_plan(plan);
            last_exit_status = (plan.valid && !plan.empty) ? 0 : 2;
            continue;
        }

        // Feature 2: 'trace <command>' vs 'trace last' / 'trace export <file>'
        if (keyword == "trace") {
            if (remainder.empty()) {
                std::cerr << "shellcraft: trace: usage: trace <command> | trace last | trace export <file.json>\n";
                last_exit_status = 2;
                continue;
            }
            if (!is_trace_subcommand_invocation(remainder)) {
                ParseResult target_parsed = parse_command_line(remainder);
                if (!target_parsed.ok) {
                    std::cerr << "shellcraft: " << target_parsed.error_message << "\n";
                    last_exit_status = 2;
                    continue;
                }
                if (target_parsed.empty) {
                    continue;
                }
                last_exit_status = execute_pipeline(
                    target_parsed.pipeline, should_exit, last_exit_status, /*explicit_trace_mode=*/true);
                if (!is_shell_interactive()) {
                    get_job_manager().reap_children();
                }
                continue;
            }
        }

        ParseResult parsed = parse_command_line(line);
        if (!parsed.ok) {
            std::cerr << "shellcraft: " << parsed.error_message << "\n";
            last_exit_status = 2;
            continue;
        }

        if (parsed.empty) {
            continue;
        }

        last_exit_status = execute_pipeline(
            parsed.pipeline, should_exit, last_exit_status, /*explicit_trace_mode=*/false);

        if (!is_shell_interactive()) {
            // In non-interactive mode, reap any children that finished during command execution
            get_job_manager().reap_children();
        }
    }

    // Reap any remaining children before exiting
    get_job_manager().reap_children();
    return last_exit_status;
}

} // namespace shellcraft
