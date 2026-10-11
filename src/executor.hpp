#ifndef SHELLCRAFT_EXECUTOR_HPP
#define SHELLCRAFT_EXECUTOR_HPP

#include "parser.hpp"

namespace shellcraft {

// Executes a parsed Pipeline (built-ins, external commands, redirections, pipes, and background jobs).
// Records execution plan and runtime trace events in TraceRecorder.
// Sets should_exit = true if the 'exit' built-in is executed in the parent shell.
// Returns the exit status of the foreground command/pipeline (or 0 for background job launch).
int execute_pipeline(const Pipeline& pipeline,
                     bool& should_exit,
                     int last_exit_status,
                     bool explicit_trace_mode = false);

} // namespace shellcraft

#endif // SHELLCRAFT_EXECUTOR_HPP
