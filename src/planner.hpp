#ifndef SHELLCRAFT_PLANNER_HPP
#define SHELLCRAFT_PLANNER_HPP

#include "parser.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace shellcraft {

struct PlannedRedirection {
    RedirectType type;
    std::string filename;
    bool file_exists = false;
    bool is_regular_file = false;
    bool is_directory = false;
    bool readable = false;
    bool writable = false;
};

struct PlannedStage {
    std::size_t stage_index = 0; // 0-based index
    std::vector<std::string> args;
    std::string formatted_command;
    bool is_builtin = false;
    bool forks_child = false;
    bool attempts_execvp = false;
    std::string stdin_source;
    std::string stdout_target;
    std::vector<PlannedRedirection> redirections;
};

struct ExecutionPlan {
    bool valid = true;
    bool empty = false;
    std::string syntax_error;
    std::string raw_command;
    bool background = false;

    std::size_t num_stages = 0;
    std::size_t expected_child_launches = 0;
    std::size_t expected_external_launches = 0;
    std::size_t pipes_required = 0;
    std::size_t total_redirections = 0;

    std::vector<PlannedStage> stages;
    std::vector<std::string> files_to_create;
    std::vector<std::string> files_to_truncate;
    std::vector<std::string> files_to_append;
    std::vector<std::string> input_files;
    std::vector<std::string> warnings;
};

class ExecutionPlanner {
public:
    // Build an ExecutionPlan from a ParseResult without executing commands or modifying files
    static ExecutionPlan create_plan(const ParseResult& parsed);

    // Build an ExecutionPlan directly from a validated Pipeline
    static ExecutionPlan create_plan_from_pipeline(const Pipeline& pipeline);

    // Format and print the ExecutionPlan to std::cout (or std::cerr on syntax error)
    static void print_plan(const ExecutionPlan& plan);
};

} // namespace shellcraft

#endif // SHELLCRAFT_PLANNER_HPP
