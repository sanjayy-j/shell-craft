#ifndef SHELLCRAFT_BUILTINS_HPP
#define SHELLCRAFT_BUILTINS_HPP

#include <string>
#include <vector>

namespace shellcraft {

// Returns true if cmd_name is a recognized ShellCraft built-in command
bool is_builtin_command(const std::string& cmd_name);

// Executes a built-in command. Sets should_exit = true if 'exit' was invoked.
// Returns the command exit status (0 for success, non-zero for error).
int execute_builtin(const std::vector<std::string>& args, bool& should_exit, int last_exit_status);

} // namespace shellcraft

#endif // SHELLCRAFT_BUILTINS_HPP
