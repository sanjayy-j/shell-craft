#ifndef SHELLCRAFT_SHELL_HPP
#define SHELLCRAFT_SHELL_HPP

namespace shellcraft {

// Runs the interactive or non-interactive read-parse-execute loop of ShellCraft.
// Returns the final exit status of the shell.
int run_shell();

} // namespace shellcraft

#endif // SHELLCRAFT_SHELL_HPP
