#!/usr/bin/env bash
# Automated test suite for ShellCraft:
# Covers Parser, Core Shell Execution, Built-ins, Redirections, Pipelines, Background Jobs,
# Feature 1 (Planner), Feature 2 (Trace Recorder & JSON Export), Feature 3 (Plan Verifier),
# Feature 4 (Pipeline Failure Analyzer), and Interactive PTY Job Control.

set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHELL_BIN="$ROOT_DIR/shellcraft"
PARSER_TEST_BIN="$ROOT_DIR/build/test_parser"

PASSED=0
FAILED=0

pass() {
    PASSED=$((PASSED + 1))
    printf "  [PASS] %s\n" "$1"
}

fail() {
    FAILED=$((FAILED + 1))
    printf "  [FAIL] %s\n" "$1"
    if [ $# -ge 2 ]; then
        printf "         Details: %s\n" "$2"
    fi
}

echo "=========================================="
echo "1. Running Parser Unit Tests"
echo "=========================================="
if "$PARSER_TEST_BIN"; then
    pass "C++ parser unit test suite"
else
    fail "C++ parser unit test suite"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo ""
echo "=========================================="
echo "2. Basic Command Execution Tests"
echo "=========================================="

OUT="$("$SHELL_BIN" << 'EOF'
echo Hello
EOF
)"
if [ "$OUT" = "Hello" ]; then
    pass "echo Hello"
else
    fail "echo Hello" "Expected 'Hello', got '$OUT'"
fi

OUT="$("$SHELL_BIN" << 'EOF'
echo "Hello from ShellCraft" 'single quotes'
EOF
)"
if [ "$OUT" = "Hello from ShellCraft single quotes" ]; then
    pass "quoted arguments"
else
    fail "quoted arguments" "Got '$OUT'"
fi

OUT="$("$SHELL_BIN" << 'EOF'
printf "%s:%s\n" alpha beta
EOF
)"
if [ "$OUT" = "alpha:beta" ]; then
    pass "external command with arguments (printf)"
else
    fail "external command with arguments (printf)" "Got '$OUT'"
fi

ERR_FILE="$TMP_DIR/unknown_err.txt"
OUT="$("$SHELL_BIN" 2>"$ERR_FILE" << 'EOF'
definitely_not_a_real_command_12345
echo "still alive"
EOF
)"
if [ "$OUT" = "still alive" ] && grep -qi "command not found" "$ERR_FILE"; then
    pass "unknown command reports error and shell remains usable"
else
    fail "unknown command handling" "OUT='$OUT', ERR='$(cat "$ERR_FILE")'"
fi

echo ""
echo "=========================================="
echo "3. Built-in Commands Tests"
echo "=========================================="

OUT="$("$SHELL_BIN" << EOF
cd "$TMP_DIR"
pwd
EOF
)"
REAL_TMP="$(cd "$TMP_DIR" && pwd)"
if [ "$OUT" = "$REAL_TMP" ]; then
    pass "cd changes directory and pwd reports updated directory"
else
    fail "cd and pwd" "Expected '$REAL_TMP', got '$OUT'"
fi

OUT="$("$SHELL_BIN" << 'EOF'
cd /
cd
pwd
EOF
)"
REAL_HOME="$(cd "$HOME" && pwd)"
if [ "$OUT" = "$REAL_HOME" ]; then
    pass "cd with no arguments changes to HOME"
else
    fail "cd with no arguments" "Expected '$REAL_HOME', got '$OUT'"
fi

OUT="$("$SHELL_BIN" << 'EOF'
help
EOF
)"
if echo "$OUT" | grep -q "plan <command>" && echo "$OUT" | grep -q "trace last" && echo "$OUT" | grep -q "inspect last"; then
    pass "help built-in displays core and differentiating commands"
else
    fail "help built-in" "Got '$OUT'"
fi

OUT="$("$SHELL_BIN" << 'EOF'
echo before_exit
exit
echo after_exit
EOF
)"
if [ "$OUT" = "before_exit" ]; then
    pass "exit terminates shell cleanly before subsequent lines"
else
    fail "exit built-in" "Got '$OUT'"
fi

echo ""
echo "=========================================="
echo "4. I/O Redirection Tests"
echo "=========================================="

REDIR_OUT="$TMP_DIR/output.txt"
OUT="$("$SHELL_BIN" << EOF
echo Hello > "$REDIR_OUT"
echo Again >> "$REDIR_OUT"
cat < "$REDIR_OUT"
EOF
)"
EXPECTED=$'Hello\nAgain'
if [ "$OUT" = "$EXPECTED" ]; then
    pass "output truncate (>), append (>>), and input (<) redirection"
else
    fail "redirection >, >>, <" "Expected '$EXPECTED', got '$OUT'"
fi

SORTED_OUT="$TMP_DIR/sorted_single.txt"
"$SHELL_BIN" << EOF
cat < "$REDIR_OUT" > "$SORTED_OUT"
EOF
if cmp -s "$REDIR_OUT" "$SORTED_OUT"; then
    pass "combined input (<) and output (>) redirection on one command"
else
    fail "combined < and > redirection"
fi

BUILTIN_REDIR="$TMP_DIR/pwd_redir.txt"
OUT="$("$SHELL_BIN" << EOF
cd "$TMP_DIR"
pwd > "$BUILTIN_REDIR"
echo "stdout_restored"
EOF
)"
if [ "$OUT" = "stdout_restored" ] && [ "$(cat "$BUILTIN_REDIR")" = "$REAL_TMP" ]; then
    pass "standalone built-in redirection and fd restoration"
else
    fail "standalone built-in redirection" "OUT='$OUT', FILE='$(cat "$BUILTIN_REDIR" 2>/dev/null)'"
fi

echo ""
echo "=========================================="
echo "5. Pipelines Tests"
echo "=========================================="

OUT="$("$SHELL_BIN" << 'EOF'
echo Hello | wc -c
EOF
)"
OUT_TRIMMED="$(echo "$OUT" | tr -d '[:space:]')"
if [ "$OUT_TRIMMED" = "6" ]; then
    pass "two-stage pipeline (echo Hello | wc -c)"
else
    fail "two-stage pipeline" "Expected '6', got '$OUT_TRIMMED'"
fi

PIPE_IN="$TMP_DIR/pipe_input.txt"
cat > "$PIPE_IN" << 'EOF'
info: system boot
error: disk full
warning: high load
error: network timeout
error: permission denied
EOF

OUT="$("$SHELL_BIN" << EOF
cat "$PIPE_IN" | grep error | wc -l
EOF
)"
OUT_TRIMMED="$(echo "$OUT" | tr -d '[:space:]')"
if [ "$OUT_TRIMMED" = "3" ]; then
    pass "three-stage pipeline (cat | grep | wc -l)"
else
    fail "three-stage pipeline" "Expected '3', got '$OUT_TRIMMED'"
fi

PIPE_SORTED="$TMP_DIR/sorted_pipe.txt"
"$SHELL_BIN" << EOF
cat < "$PIPE_IN" | grep error | sort > "$PIPE_SORTED"
EOF
EXPECTED_SORTED=$'error: disk full\nerror: network timeout\nerror: permission denied'
ACTUAL_SORTED="$(cat "$PIPE_SORTED" 2>/dev/null)"
if [ "$ACTUAL_SORTED" = "$EXPECTED_SORTED" ]; then
    pass "multi-stage pipeline with input and output redirection"
else
    fail "pipeline with redirection" "Got '$ACTUAL_SORTED'"
fi

echo ""
echo "=========================================="
echo "6. Background Jobs & Reaping Tests"
echo "=========================================="

BG_OUT="$("$SHELL_BIN" << 'EOF'
sleep 1 &
jobs
sleep 2
jobs
echo "done_bg_test"
EOF
)"

if echo "$BG_OUT" | grep -Eq '^\[1\] [0-9]+' &&
   echo "$BG_OUT" | grep -Eq '^\[1\] Running sleep 1 &' &&
   echo "$BG_OUT" | grep -q 'done_bg_test'; then
    pass "background job launch, jobs listing while active, and child reaping"
else
    fail "background jobs & reaping" "Output was:\n$BG_OUT"
fi

echo ""
echo "=========================================="
echo "7. Feature 1: Pre-Execution Impact Planner Tests"
echo "=========================================="

EXISTING_TARGET="$TMP_DIR/existing_errors.txt"
echo "keep_this_content" > "$EXISTING_TARGET"
UNTOUCHED_MARKER="$TMP_DIR/should_never_be_created.txt"

PLAN_OUT="$("$SHELL_BIN" << EOF
plan cat "$PIPE_IN" | grep ERROR | wc -l > "$EXISTING_TARGET"
plan touch "$UNTOUCHED_MARKER"
EOF
)"

if echo "$PLAN_OUT" | grep -q "Pipeline stages: 3" &&
   echo "$PLAN_OUT" | grep -q "Expected external process launches: 3" &&
   echo "$PLAN_OUT" | grep -q "Pipes required: 2" &&
   echo "$PLAN_OUT" | grep -q "stdout: pipe 1" &&
   echo "$PLAN_OUT" | grep -q "stdin: pipe 2" &&
   echo "$PLAN_OUT" | grep -q "Warning: output redirection may truncate" &&
   echo "$PLAN_OUT" | grep -q "Execution has not started." &&
   [ "$(cat "$EXISTING_TARGET")" = "keep_this_content" ] &&
   [ ! -e "$UNTOUCHED_MARKER" ]; then
    pass "planner predicts stages/pipes/redirections/truncation warnings without executing or modifying files"
else
    fail "planner output & non-execution guarantee" "Output was:\n$PLAN_OUT"
fi

PLAN_ERR="$TMP_DIR/plan_syntax_err.txt"
"$SHELL_BIN" 2>"$PLAN_ERR" << 'EOF'
plan cat <
EOF
if grep -qi "syntax error" "$PLAN_ERR" && grep -q "Execution has not started." "$PLAN_ERR"; then
    pass "planner reports syntax error on invalid command without crashing"
else
    fail "planner syntax error handling" "ERR='$(cat "$PLAN_ERR")'"
fi

echo ""
echo "=========================================="
echo "8. Feature 2: Runtime Execution Trace Recorder, Replay & JSON Export"
echo "=========================================="

REPLAY_COUNTER_FILE="$TMP_DIR/replay_count.txt"
TRACE_JSON="$TMP_DIR/trace_export.json"

TRACE_OUT="$("$SHELL_BIN" << EOF
trace cat "$PIPE_IN" | grep error | wc -l >> "$REPLAY_COUNTER_FILE"
trace last
trace export "$TRACE_JSON"
EOF
)"

LINE_COUNT="$(wc -l < "$REPLAY_COUNTER_FILE" | tr -d '[:space:]')"
if echo "$TRACE_OUT" | grep -q "COMMAND_PARSED" &&
   echo "$TRACE_OUT" | grep -q "PLAN_CREATED" &&
   echo "$TRACE_OUT" | grep -q "PIPE_CREATED" &&
   echo "$TRACE_OUT" | grep -q "FORK_SUCCEEDED" &&
   echo "$TRACE_OUT" | grep -q "EXECVP_ATTEMPTED" &&
   echo "$TRACE_OUT" | grep -q "EXECVP_SUCCEEDED" &&
   echo "$TRACE_OUT" | grep -q "PROCESS_EXITED" &&
   [ "$LINE_COUNT" = "1" ]; then
    pass "trace <cmd> records lifecycle events and 'trace last' replays history without re-running command"
else
    fail "trace recording and replay" "LINE_COUNT=$LINE_COUNT, OUT=\n$TRACE_OUT"
fi

if [ -s "$TRACE_JSON" ] && python3 -c "
import json, sys
with open(sys.argv[1]) as f:
    data = json.load(f)
assert data['plan']['num_stages'] == 3
assert data['plan']['pipes_required'] == 2
assert len(data['stages']) == 3
assert len(data['events']) >= 10
assert all(s['fork_succeeded'] and s['exec_succeeded'] and s['exit_status'] == 0 for s in data['stages'])
" "$TRACE_JSON"; then
    pass "trace export produces valid JSON containing real stage outcomes and events"
else
    fail "trace JSON export validation" "JSON content:\n$(cat "$TRACE_JSON" 2>/dev/null)"
fi

echo ""
echo "=========================================="
echo "9. Feature 3: Execution Plan vs. Actual Behaviour Verifier"
echo "=========================================="

# 9a. Successful pipeline verification
VERIFY_OK_OUT="$("$SHELL_BIN" << EOF
trace cat "$PIPE_IN" | grep error | wc -l
inspect last
EOF
)"
if echo "$VERIFY_OK_OUT" | grep -q "Pipeline stages:       expected 3 / observed 3" &&
   echo "$VERIFY_OK_OUT" | grep -q "Pipe setup:            expected 2 / succeeded 2" &&
   echo "$VERIFY_OK_OUT" | grep -q "Process launches:      expected 3 / succeeded 3" &&
   echo "$VERIFY_OK_OUT" | grep -q "Result: No discrepancies detected in the tracked operations."; then
    pass "verifier confirms matching plan vs. actual execution on successful pipeline"
else
    fail "verifier success report" "OUT=\n$VERIFY_OK_OUT"
fi

# 9b. Redirection failure discrepancy
VERIFY_REDIR_OUT="$("$SHELL_BIN" 2>/dev/null << EOF
trace cat < "$TMP_DIR/missing_redir_input.txt"
inspect last
EOF
)"
if echo "$VERIFY_REDIR_OUT" | grep -q "Redirection setup:     expected 1 / succeeded 0 / failed 1" &&
   echo "$VERIFY_REDIR_OUT" | grep -q "execvp() was not reached because redirection setup failed beforehand" &&
   echo "$VERIFY_REDIR_OUT" | grep -q "Result: Discrepancies detected between planned shell setup and observed execution."; then
    pass "verifier detects redirection failure and unreached execvp()"
else
    fail "verifier redirection discrepancy" "OUT=\n$VERIFY_REDIR_OUT"
fi

# 9c. Missing completion evidence (background job still running) reported as unknown
VERIFY_BG_OUT="$("$SHELL_BIN" << 'EOF'
trace sleep 2 &
inspect last
sleep 2
EOF
)"
if echo "$VERIFY_BG_OUT" | grep -q "unknown (incomplete)" &&
   echo "$VERIFY_BG_OUT" | grep -q "completion evidence is incomplete/unknown"; then
    pass "verifier reports uncollected background process outcome as unknown rather than succeeded"
else
    fail "verifier missing evidence handling" "OUT=\n$VERIFY_BG_OUT"
fi

echo ""
echo "=========================================="
echo "10. Feature 4: Pipeline Failure Analyzer"
echo "=========================================="

# 10a. Multi-stage pipeline where stage 1 fails to open input file (cat missing.txt | grep ERROR | wc -l)
DIAG_PIPE_OUT="$("$SHELL_BIN" 2>/dev/null << EOF
cat "$TMP_DIR/missing_file_operand.txt" | grep ERROR | wc -l
diagnose last
EOF
)"
if echo "$DIAG_PIPE_OUT" | grep -q "Stage 1: cat" &&
   echo "$DIAG_PIPE_OUT" | grep -q "ENOENT: No such file or directory" &&
   echo "$DIAG_PIPE_OUT" | grep -q "Stage 2: grep ERROR" &&
   echo "$DIAG_PIPE_OUT" | grep -q "no input lines matched" &&
   echo "$DIAG_PIPE_OUT" | grep -q "Stage 3: wc -l" &&
   echo "$DIAG_PIPE_OUT" | grep -q "Stage completed successfully (exit status 0)"; then
    pass "pipeline analyzer diagnoses 'cat missing.txt | grep ERROR | wc -l' across all 3 stages"
else
    fail "pipeline analyzer multi-stage diagnosis" "OUT=\n$DIAG_PIPE_OUT"
fi

# 10b. Unknown executable (ENOENT / exit status 127)
DIAG_EXEC_OUT="$("$SHELL_BIN" 2>/dev/null << 'EOF'
nonexistent_binary_xyz_999 --flag
diagnose last
EOF
)"
if echo "$DIAG_EXEC_OUT" | grep -q "execvp() FAILED (ENOENT" &&
   echo "$DIAG_EXEC_OUT" | grep -q "was not found in PATH"; then
    pass "pipeline analyzer differentiates execvp() ENOENT failure from normal non-zero exit"
else
    fail "pipeline analyzer execvp ENOENT" "OUT=\n$DIAG_EXEC_OUT"
fi

echo ""
echo "=========================================="
echo "11. Malformed Input & Error Handling Tests"
echo "=========================================="

ERR_SYNTAX="$TMP_DIR/syntax_err.txt"
OUT="$("$SHELL_BIN" 2>"$ERR_SYNTAX" << EOF
echo "unclosed quote
| wc -l
ls >
cat < "$TMP_DIR/does_not_exist_999.txt"
echo "recovered_after_errors"
EOF
)"
if [ "$OUT" = "recovered_after_errors" ] && [ -s "$ERR_SYNTAX" ]; then
    pass "malformed syntax and missing redirect files handled without crashing"
else
    fail "malformed input error recovery" "OUT='$OUT', ERR='$(cat "$ERR_SYNTAX")'"
fi

echo ""
echo "=========================================="
echo "12. Interactive PTY Job Control & Signal Trace Tests"
echo "=========================================="
if command -v python3 >/dev/null 2>&1; then
    if python3 "$ROOT_DIR/tests/interactive_pty_test.py" "$SHELL_BIN"; then
        pass "interactive PTY job control (Ctrl+Z, jobs, bg %1, fg %1, Ctrl+C, trace & diagnose)"
    else
        fail "interactive PTY job control"
    fi
else
    echo "  [SKIP] python3 not available for automated PTY test"
fi

echo ""
echo "=========================================="
printf "Test Summary: %d passed, %d failed\n" "$PASSED" "$FAILED"
echo "=========================================="

if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
exit 0
