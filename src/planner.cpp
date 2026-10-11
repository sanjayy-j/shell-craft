#include "planner.hpp"
#include "builtins.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace shellcraft {

namespace {

std::string format_args(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        const std::string& arg = args[i];
        bool needs_quotes = arg.empty();
        for (char c : arg) {
            if (c == ' ' || c == '\t' || c == '|' || c == '<' || c == '>' || c == '&' || c == '\'' || c == '"') {
                needs_quotes = true;
                break;
            }
        }
        if (needs_quotes) {
            out += '"';
            for (char c : arg) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c;
            }
            out += '"';
        } else {
            out += arg;
        }
    }
    return out;
}

std::string redir_operator_str(RedirectType type) {
    switch (type) {
        case RedirectType::INPUT:
            return "<";
        case RedirectType::OUTPUT_TRUNC:
            return ">";
        case RedirectType::OUTPUT_APPEND:
            return ">>";
    }
    return "?";
}

void add_unique(std::vector<std::string>& vec, const std::string& item) {
    for (const auto& existing : vec) {
        if (existing == item) {
            return;
        }
    }
    vec.push_back(item);
}

} // namespace

ExecutionPlan ExecutionPlanner::create_plan(const ParseResult& parsed) {
    if (!parsed.ok) {
        ExecutionPlan plan;
        plan.valid = false;
        plan.syntax_error = parsed.error_message;
        return plan;
    }
    if (parsed.empty) {
        ExecutionPlan plan;
        plan.valid = true;
        plan.empty = true;
        return plan;
    }
    return create_plan_from_pipeline(parsed.pipeline);
}

ExecutionPlan ExecutionPlanner::create_plan_from_pipeline(const Pipeline& pipeline) {
    ExecutionPlan plan;
    plan.valid = true;
    plan.empty = pipeline.commands.empty();
    plan.raw_command = pipeline.raw_command;
    plan.background = pipeline.background;
    plan.num_stages = pipeline.commands.size();
    plan.pipes_required = (plan.num_stages > 1) ? (plan.num_stages - 1) : 0;

    const bool single_fg_command = (plan.num_stages == 1 && !pipeline.background);

    for (std::size_t i = 0; i < plan.num_stages; ++i) {
        const Command& cmd = pipeline.commands[i];
        PlannedStage stage;
        stage.stage_index = i;
        stage.args = cmd.args;
        stage.formatted_command = format_args(cmd.args);
        stage.is_builtin = (!cmd.args.empty() && is_builtin_command(cmd.args[0]));

        if (single_fg_command && stage.is_builtin) {
            stage.forks_child = false;
            stage.attempts_execvp = false;
        } else {
            stage.forks_child = true;
            stage.attempts_execvp = !stage.is_builtin;
            ++plan.expected_child_launches;
            if (stage.attempts_execvp) {
                ++plan.expected_external_launches;
            }
        }

        // Default pipe/inherited wiring
        if (i == 0) {
            stage.stdin_source = "inherited";
        } else {
            stage.stdin_source = "pipe " + std::to_string(i);
        }

        if (i + 1 < plan.num_stages) {
            stage.stdout_target = "pipe " + std::to_string(i + 1);
        } else {
            stage.stdout_target = "inherited";
        }

        // Inspect redirections using non-mutating stat() and access()
        for (const auto& redir : cmd.redirections) {
            ++plan.total_redirections;

            PlannedRedirection pr;
            pr.type = redir.type;
            pr.filename = redir.filename;

            struct stat st {};
            if (stat(redir.filename.c_str(), &st) == 0) {
                pr.file_exists = true;
                pr.is_regular_file = S_ISREG(st.st_mode);
                pr.is_directory = S_ISDIR(st.st_mode);
                pr.readable = (access(redir.filename.c_str(), R_OK) == 0);
                pr.writable = (access(redir.filename.c_str(), W_OK) == 0);
            }

            if (redir.type == RedirectType::INPUT) {
                if (i > 0 && stage.stdin_source.rfind("pipe ", 0) == 0) {
                    plan.warnings.push_back(
                        "Stage " + std::to_string(i + 1) + ": input redirection '< " + redir.filename +
                        "' overrides " + stage.stdin_source + ".");
                }
                stage.stdin_source = redir.filename;
                add_unique(plan.input_files, redir.filename);

                if (!pr.file_exists) {
                    plan.warnings.push_back(
                        "input file '" + redir.filename + "' does not currently exist (" +
                        std::strerror(ENOENT) + ").");
                } else if (pr.is_directory) {
                    plan.warnings.push_back(
                        "input target '" + redir.filename + "' is a directory.");
                } else if (!pr.readable) {
                    plan.warnings.push_back(
                        "input file '" + redir.filename + "' lacks read permission.");
                }
            } else if (redir.type == RedirectType::OUTPUT_TRUNC) {
                if (i + 1 < plan.num_stages && stage.stdout_target.rfind("pipe ", 0) == 0) {
                    plan.warnings.push_back(
                        "Stage " + std::to_string(i + 1) + ": output redirection '> " + redir.filename +
                        "' overrides " + stage.stdout_target + ".");
                }
                stage.stdout_target = redir.filename;

                if (pr.file_exists) {
                    add_unique(plan.files_to_truncate, redir.filename);
                    plan.warnings.push_back(
                        "output redirection may truncate " + redir.filename + " (file already exists).");
                    if (pr.is_directory) {
                        plan.warnings.push_back(
                            "output target '" + redir.filename + "' is a directory and cannot be opened for writing.");
                    } else if (!pr.writable) {
                        plan.warnings.push_back(
                            "output file '" + redir.filename + "' exists but is not writable.");
                    }
                } else {
                    add_unique(plan.files_to_create, redir.filename);
                    plan.warnings.push_back(
                        "output redirection may create or truncate " + redir.filename + ".");
                }
            } else if (redir.type == RedirectType::OUTPUT_APPEND) {
                if (i + 1 < plan.num_stages && stage.stdout_target.rfind("pipe ", 0) == 0) {
                    plan.warnings.push_back(
                        "Stage " + std::to_string(i + 1) + ": append redirection '>> " + redir.filename +
                        "' overrides " + stage.stdout_target + ".");
                }
                stage.stdout_target = redir.filename + " (append)";
                add_unique(plan.files_to_append, redir.filename);

                if (!pr.file_exists) {
                    add_unique(plan.files_to_create, redir.filename);
                } else if (pr.is_directory) {
                    plan.warnings.push_back(
                        "append target '" + redir.filename + "' is a directory.");
                } else if (!pr.writable) {
                    plan.warnings.push_back(
                        "append target '" + redir.filename + "' exists but is not writable.");
                }
            }

            stage.redirections.push_back(pr);
        }

        if (stage.is_builtin && plan.num_stages > 1 && !stage.args.empty() && stage.args[0] == "cd") {
            plan.warnings.push_back(
                "Stage " + std::to_string(i + 1) +
                ": 'cd' inside a pipeline runs in a child process and will not change the parent shell directory.");
        }

        plan.stages.push_back(stage);
    }

    return plan;
}

void ExecutionPlanner::print_plan(const ExecutionPlan& plan) {
    if (!plan.valid) {
        std::cerr << "ShellCraft Execution Plan — Planning Error\n\n"
                  << "Error: " << plan.syntax_error << "\n"
                  << "Execution has not started.\n";
        return;
    }

    if (plan.empty) {
        std::cout << "ShellCraft Execution Plan\n\n"
                  << "Empty command (no stages to execute).\n"
                  << "Execution has not started.\n";
        return;
    }

    std::cout << "ShellCraft Execution Plan\n\n";
    std::cout << "Command: " << plan.raw_command << "\n";
    std::cout << "Execution mode: " << (plan.background ? "background (&)" : "foreground") << "\n";
    std::cout << "Pipeline stages: " << plan.num_stages << "\n";
    std::cout << "Expected child process launches: " << plan.expected_child_launches << "\n";
    std::cout << "Expected external process launches: " << plan.expected_external_launches << "\n";
    std::cout << "Pipes required: " << plan.pipes_required << "\n";

    for (const auto& stage : plan.stages) {
        std::cout << "\nStage " << (stage.stage_index + 1) << ": " << stage.formatted_command << "\n";
        std::cout << "  command: " << (stage.args.empty() ? "<none>" : stage.args[0]) << "\n";
        std::cout << "  arguments: [";
        for (std::size_t a = 0; a < stage.args.size(); ++a) {
            if (a > 0) std::cout << ", ";
            std::cout << "\"" << stage.args[a] << "\"";
        }
        std::cout << "]\n";
        if (stage.is_builtin) {
            std::cout << "  execution mechanism: shell built-in ("
                      << (stage.forks_child ? "child process" : "parent process") << ")\n";
        } else {
            std::cout << "  execution mechanism: external program (fork + execvp)\n";
        }
        std::cout << "  stdin: " << stage.stdin_source << "\n";
        std::cout << "  stdout: " << stage.stdout_target << "\n";

        if (!stage.redirections.empty()) {
            std::cout << "  redirections: ";
            for (std::size_t r = 0; r < stage.redirections.size(); ++r) {
                if (r > 0) std::cout << ", ";
                const auto& redir = stage.redirections[r];
                std::cout << redir_operator_str(redir.type) << " " << redir.filename;
            }
            std::cout << "\n";
        }
    }

    if (!plan.files_to_truncate.empty() || !plan.files_to_create.empty() || !plan.files_to_append.empty()) {
        std::cout << "\nPredicted file impacts:\n";
        for (const auto& f : plan.files_to_truncate) {
            std::cout << "  - truncate existing file: " << f << "\n";
        }
        for (const auto& f : plan.files_to_create) {
            std::cout << "  - create new file: " << f << "\n";
        }
        for (const auto& f : plan.files_to_append) {
            std::cout << "  - append to file: " << f << "\n";
        }
    }

    std::cout << "\n";
    for (const auto& warning : plan.warnings) {
        std::cout << "Warning: " << warning << "\n";
    }

    std::cout << "Note: This plan describes known static structure and predicted file effects only. "
                 "Runtime system-call errors, OS scheduling, signal delivery, and external process "
                 "behaviour cannot be known before execution.\n";
    std::cout << "Execution has not started.\n";
}

} // namespace shellcraft
