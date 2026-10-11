#include "parser.hpp"

#include <cctype>

namespace shellcraft {

namespace {

enum class TokenType {
    WORD,
    PIPE,
    REDIRECT_IN,
    REDIRECT_OUT,
    REDIRECT_APPEND,
    BACKGROUND
};

struct Token {
    TokenType type;
    std::string text;
};

std::string trim_line(const std::string& str) {
    std::size_t first = 0;
    while (first < str.size() && std::isspace(static_cast<unsigned char>(str[first]))) {
        ++first;
    }
    if (first == str.size()) {
        return "";
    }
    std::size_t last = str.size() - 1;
    while (last > first && std::isspace(static_cast<unsigned char>(str[last]))) {
        --last;
    }
    return str.substr(first, last - first + 1);
}

bool is_operator_char(char c) {
    return c == '|' || c == '<' || c == '>' || c == '&';
}

bool tokenize(const std::string& line, std::vector<Token>& tokens, std::string& error_msg) {
    std::size_t i = 0;
    const std::size_t n = line.size();

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i >= n) {
            break;
        }

        // Comments starting with '#' at token boundary
        if (line[i] == '#') {
            break;
        }

        // Operators
        if (line[i] == '|') {
            tokens.push_back({TokenType::PIPE, "|"});
            ++i;
            continue;
        }
        if (line[i] == '<') {
            tokens.push_back({TokenType::REDIRECT_IN, "<"});
            ++i;
            continue;
        }
        if (line[i] == '>') {
            if (i + 1 < n && line[i + 1] == '>') {
                tokens.push_back({TokenType::REDIRECT_APPEND, ">>"});
                i += 2;
            } else {
                tokens.push_back({TokenType::REDIRECT_OUT, ">"});
                ++i;
            }
            continue;
        }
        if (line[i] == '&') {
            tokens.push_back({TokenType::BACKGROUND, "&"});
            ++i;
            continue;
        }

        // Word token (may include single-quoted, double-quoted, and escaped segments)
        std::string word;
        while (i < n && !std::isspace(static_cast<unsigned char>(line[i])) && !is_operator_char(line[i])) {
            char c = line[i];
            if (c == '\'') {
                ++i; // skip opening single quote
                while (i < n && line[i] != '\'') {
                    word.push_back(line[i]);
                    ++i;
                }
                if (i >= n) {
                    error_msg = "syntax error: unclosed single quote";
                    return false;
                }
                ++i; // skip closing single quote
            } else if (c == '"') {
                ++i; // skip opening double quote
                while (i < n && line[i] != '"') {
                    if (line[i] == '\\') {
                        if (i + 1 >= n) {
                            error_msg = "syntax error: unclosed double quote";
                            return false;
                        }
                        char next = line[i + 1];
                        if (next == '"' || next == '\\' || next == '$' || next == '`') {
                            word.push_back(next);
                            i += 2;
                        } else {
                            word.push_back(line[i]);
                            ++i;
                        }
                    } else {
                        word.push_back(line[i]);
                        ++i;
                    }
                }
                if (i >= n) {
                    error_msg = "syntax error: unclosed double quote";
                    return false;
                }
                ++i; // skip closing double quote
            } else if (c == '\\') {
                if (i + 1 >= n) {
                    error_msg = "syntax error: trailing backslash escape";
                    return false;
                }
                word.push_back(line[i + 1]);
                i += 2;
            } else {
                word.push_back(c);
                ++i;
            }
        }

        tokens.push_back({TokenType::WORD, word});
    }

    return true;
}

} // namespace

ParseResult parse_command_line(const std::string& line) {
    ParseResult result;
    std::vector<Token> tokens;
    std::string error_msg;

    if (!tokenize(line, tokens, error_msg)) {
        result.ok = false;
        result.error_message = error_msg;
        return result;
    }

    if (tokens.empty()) {
        result.ok = true;
        result.empty = true;
        return result;
    }

    result.pipeline.raw_command = trim_line(line);

    // Check '&' background operator placement
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::BACKGROUND) {
            if (i != tokens.size() - 1 || i == 0) {
                result.ok = false;
                result.error_message = "syntax error: unexpected token '&'";
                return result;
            }
            result.pipeline.background = true;
        }
    }

    if (result.pipeline.background) {
        tokens.pop_back();
        if (tokens.empty()) {
            result.ok = false;
            result.error_message = "syntax error: unexpected token '&'";
            return result;
        }
    }

    if (tokens.front().type == TokenType::PIPE || tokens.back().type == TokenType::PIPE) {
        result.ok = false;
        result.error_message = "syntax error: unexpected token '|'";
        return result;
    }

    Command current_cmd;
    bool has_tokens_in_stage = false;

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& tok = tokens[i];

        if (tok.type == TokenType::PIPE) {
            if (!has_tokens_in_stage || current_cmd.args.empty()) {
                result.ok = false;
                result.error_message = "syntax error: unexpected token '|'";
                return result;
            }
            result.pipeline.commands.push_back(current_cmd);
            current_cmd = Command{};
            has_tokens_in_stage = false;
            continue;
        }

        has_tokens_in_stage = true;

        if (tok.type == TokenType::REDIRECT_IN ||
            tok.type == TokenType::REDIRECT_OUT ||
            tok.type == TokenType::REDIRECT_APPEND) {
            if (i + 1 >= tokens.size() || tokens[i + 1].type != TokenType::WORD) {
                result.ok = false;
                result.error_message = "syntax error: expected filename after '" + tok.text + "'";
                return result;
            }
            const std::string& filename = tokens[i + 1].text;
            if (filename.empty()) {
                result.ok = false;
                result.error_message = "syntax error: empty redirection filename";
                return result;
            }

            if (tok.type == TokenType::REDIRECT_IN) {
                current_cmd.input_redirect = filename;
                current_cmd.redirections.push_back({RedirectType::INPUT, filename});
            } else if (tok.type == TokenType::REDIRECT_OUT) {
                current_cmd.output_redirect = filename;
                current_cmd.append_output = false;
                current_cmd.redirections.push_back({RedirectType::OUTPUT_TRUNC, filename});
            } else {
                current_cmd.output_redirect = filename;
                current_cmd.append_output = true;
                current_cmd.redirections.push_back({RedirectType::OUTPUT_APPEND, filename});
            }
            ++i; // consume filename token
            continue;
        }

        if (tok.type == TokenType::WORD) {
            current_cmd.args.push_back(tok.text);
        }
    }

    if (!has_tokens_in_stage || current_cmd.args.empty()) {
        result.ok = false;
        result.error_message = "syntax error: missing command";
        return result;
    }

    result.pipeline.commands.push_back(current_cmd);
    return result;
}

} // namespace shellcraft
