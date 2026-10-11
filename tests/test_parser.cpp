#include "parser.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using shellcraft::parse_command_line;
using shellcraft::ParseResult;
using shellcraft::RedirectType;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const std::string& test_name) {
    if (condition) {
        ++g_passed;
    } else {
        ++g_failed;
        std::cerr << "[FAIL] " << test_name << "\n";
    }
}

void test_empty_and_whitespace() {
    auto r1 = parse_command_line("");
    check(r1.ok && r1.empty, "empty string is valid and marked empty");

    auto r2 = parse_command_line("   \t  \r ");
    check(r2.ok && r2.empty, "whitespace-only string is valid and marked empty");

    auto r3 = parse_command_line("   # full line comment");
    check(r3.ok && r3.empty, "comment line is valid and marked empty");
}

void test_basic_command_and_quotes() {
    auto r1 = parse_command_line("echo Hello World");
    check(r1.ok && !r1.empty && r1.pipeline.commands.size() == 1, "basic echo command count");
    check(r1.pipeline.commands[0].args == std::vector<std::string>{"echo", "Hello", "World"},
          "basic echo tokens");

    auto r2 = parse_command_line("echo \"Hello from ShellCraft\" 'single quoted | > < &'");
    check(r2.ok && r2.pipeline.commands.size() == 1, "quoted command parses");
    check(r2.pipeline.commands[0].args ==
              std::vector<std::string>{"echo", "Hello from ShellCraft", "single quoted | > < &"},
          "single and double quotes preserve spaces and operators");

    auto r3 = parse_command_line("echo \"\" '' foo\\ bar \"escaped \\\"quote\\\"\"");
    check(r3.ok && r3.pipeline.commands.size() == 1, "empty quotes and escapes parse");
    check(r3.pipeline.commands[0].args ==
              std::vector<std::string>{"echo", "", "", "foo bar", "escaped \"quote\""},
          "empty quoted args and backslash escapes");
}

void test_adjacent_operators_and_redirection() {
    auto r1 = parse_command_line("cat<input.txt>output.txt");
    check(r1.ok && r1.pipeline.commands.size() == 1, "adjacent < and > parse");
    check(r1.pipeline.commands[0].args == std::vector<std::string>{"cat"}, "cat arg extracted");
    check(r1.pipeline.commands[0].input_redirect == "input.txt", "input_redirect set");
    check(r1.pipeline.commands[0].output_redirect == "output.txt" && !r1.pipeline.commands[0].append_output,
          "output_redirect truncate set");

    auto r2 = parse_command_line("echo Again>>output.txt");
    check(r2.ok && r2.pipeline.commands.size() == 1, "adjacent >> parses");
    check(r2.pipeline.commands[0].output_redirect == "output.txt" && r2.pipeline.commands[0].append_output,
          "output_redirect append set");

    auto r3 = parse_command_line("cat input.txt|grep error|wc -l&");
    check(r3.ok && r3.pipeline.background && r3.pipeline.commands.size() == 3,
          "adjacent pipes and trailing & parse into 3-stage background pipeline");
    check(r3.pipeline.commands[0].args == std::vector<std::string>{"cat", "input.txt"}, "stage 1 args");
    check(r3.pipeline.commands[1].args == std::vector<std::string>{"grep", "error"}, "stage 2 args");
    check(r3.pipeline.commands[2].args == std::vector<std::string>{"wc", "-l"}, "stage 3 args");
}

void test_malformed_syntax() {
    check(!parse_command_line("echo \"unclosed double").ok, "unclosed double quote rejected");
    check(!parse_command_line("echo 'unclosed single").ok, "unclosed single quote rejected");
    check(!parse_command_line("echo trailing\\").ok, "trailing backslash rejected");
    check(!parse_command_line("| ls").ok, "leading pipe rejected");
    check(!parse_command_line("ls |").ok, "trailing pipe rejected");
    check(!parse_command_line("ls | | wc").ok, "empty pipe stage rejected");
    check(!parse_command_line("ls >").ok, "missing output redirect target rejected");
    check(!parse_command_line("cat <").ok, "missing input redirect target rejected");
    check(!parse_command_line("ls >> | wc").ok, "redirect target followed by pipe rejected");
    check(!parse_command_line("> out.txt").ok, "redirection without command rejected");
    check(!parse_command_line("&").ok, "standalone & rejected");
    check(!parse_command_line("sleep 1 & echo hi").ok, "non-trailing & rejected");
}

} // namespace

int main() {
    test_empty_and_whitespace();
    test_basic_command_and_quotes();
    test_adjacent_operators_and_redirection();
    test_malformed_syntax();

    std::cout << "Parser unit tests: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
