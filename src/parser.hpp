#ifndef SHELLCRAFT_PARSER_HPP
#define SHELLCRAFT_PARSER_HPP

#include <string>
#include <vector>

namespace shellcraft {

enum class RedirectType {
    INPUT,          // <
    OUTPUT_TRUNC,   // >
    OUTPUT_APPEND   // >>
};

struct Redirection {
    RedirectType type;
    std::string filename;
};

struct Command {
    std::vector<std::string> args;
    std::vector<Redirection> redirections;
    std::string input_redirect;
    std::string output_redirect;
    bool append_output = false;
};

struct Pipeline {
    std::vector<Command> commands;
    bool background = false;
    std::string raw_command;
};

struct ParseResult {
    bool ok = true;
    bool empty = false;
    std::string error_message;
    Pipeline pipeline;
};

// Parses a raw command line into a structured Pipeline representation.
ParseResult parse_command_line(const std::string& line);

} // namespace shellcraft

#endif // SHELLCRAFT_PARSER_HPP
