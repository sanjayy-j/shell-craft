#include "verifier.hpp"
#include "jobs.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace shellcraft {

namespace {

bool looks_like_file_command(const std::string& cmd) {
    return cmd == "cat" || cmd == "grep" || cmd == "sort" || cmd == "wc" ||
           cmd == "head" || cmd == "tail" || cmd == "ls" || cmd == "cp" ||
           cmd == "mv" || cmd == "rm";
}

void inspect_command_file_operands(const StageOutcome& st,
                                   std::size_t min_arg_index,
                                   std::string& out_explanation) {
    for (std::size_t i = min_arg_index; i < st.args.size(); ++i) {
        const std::string& arg = st.args[i];
        if (arg.empty() || arg[0] == '-') {
            continue;
        }
        struct stat sb {};
        if (stat(arg.c_str(), &sb) != 0) {
            int err = errno;
            out_explanation = "Operand '" + arg + "' could not be accessed (" +
                              errno_symbolic_name(err) + ": " + std::strerror(err) +
                              "); '" + st.args[0] + "' likely failed while opening this input path.";
            return;
        }
        if (access(arg.c_str(), R_OK) != 0) {
            int err = errno;
            out_explanation = "Operand '" + arg + "' lacks read permission (" +
                              errno_symbolic_name(err) + ": " + std::strerror(err) + ").";
            return;
        }
    }
}

} // namespace

VerificationSummary PlanVerifier::verify(const ExecutionTrace& trace) {
    VerificationSummary sum;
    sum.expected_stages = trace.plan.num_stages;
    sum.expected_pipes = trace.plan.pipes_required;
    sum.succeeded_pipes = trace.pipes_created;
    sum.failed_pipes = trace.pipes_failed;

    sum.expected_forks = trace.plan.expected_child_launches;
    sum.expected_redirs = trace.plan.total_redirections;
    sum.expected_execs = trace.plan.expected_external_launches;

    if (sum.succeeded_pipes != sum.expected_pipes || sum.failed_pipes > 0) {
        sum.setup_discrepancy = true;
        sum.discrepancies.push_back(
            "Pipe setup mismatch: expected " + std::to_string(sum.expected_pipes) +
            " pipe(s), succeeded " + std::to_string(sum.succeeded_pipes) +
            ", failed " + std::to_string(sum.failed_pipes) +
            (trace.last_pipe_errno != 0
                 ? (" (" + errno_symbolic_name(trace.last_pipe_errno) + ": " +
                    std::strerror(trace.last_pipe_errno) + ")")
                 : "") + ".");
    }

    for (std::size_t i = 0; i < trace.stages.size(); ++i) {
        const auto& st = trace.stages[i];
        const std::string stage_label = "Stage " + std::to_string(i + 1) + " (" + st.formatted_command + ")";

        // 1. Process creation / stage launch
        if (st.forks_child) {
            if (st.fork_succeeded) {
                ++sum.succeeded_forks;
                ++sum.observed_stages_launched;
            } else if (st.fork_attempted) {
                ++sum.failed_forks;
                sum.setup_discrepancy = true;
                sum.discrepancies.push_back(
                    stage_label + ": fork() failed with " + errno_symbolic_name(st.fork_errno) +
                    " (" + std::strerror(st.fork_errno) + ").");
            } else {
                sum.setup_discrepancy = true;
                sum.has_unknown_evidence = true;
                sum.discrepancies.push_back(
                    stage_label + ": process launch was never attempted due to an earlier setup error.");
            }
        } else {
            // Standalone parent built-in
            if (st.builtin_executed) {
                ++sum.observed_stages_launched;
            }
        }

        // 2. Redirections
        sum.succeeded_redirs += st.redir_succeeded;
        if (st.redir_failed) {
            ++sum.failed_redirs;
            sum.setup_discrepancy = true;
            sum.discrepancies.push_back(
                stage_label + ": redirection '" + st.failed_redir_op + " " + st.failed_redir_file +
                "' failed with " + errno_symbolic_name(st.redir_errno) +
                " (" + std::strerror(st.redir_errno) + ").");
            if (st.redir_expected > st.redir_succeeded + 1) {
                sum.unknown_redirs += (st.redir_expected - st.redir_succeeded - 1);
            }
        } else if (st.redir_succeeded < st.redir_expected) {
            std::size_t missing = st.redir_expected - st.redir_succeeded;
            sum.unknown_redirs += missing;
            sum.has_unknown_evidence = true;
            sum.discrepancies.push_back(
                stage_label + ": " + std::to_string(missing) +
                " expected redirection(s) have no recorded completion evidence.");
        }

        // 3. External execvp()
        if (!st.is_builtin) {
            if (st.exec_attempted) {
                ++sum.attempted_execs;
            }
            if (st.exec_succeeded) {
                ++sum.succeeded_execs;
            } else if (st.exec_failed) {
                ++sum.failed_execs;
                sum.setup_discrepancy = true;
                sum.discrepancies.push_back(
                    stage_label + ": execvp() failed with " + errno_symbolic_name(st.exec_errno) +
                    " (" + std::strerror(st.exec_errno) + ").");
            } else {
                ++sum.unreached_execs;
                sum.setup_discrepancy = true;
                if (st.redir_failed) {
                    sum.discrepancies.push_back(
                        stage_label + ": execvp() was not reached because redirection setup failed beforehand.");
                } else {
                    sum.has_unknown_evidence = true;
                    sum.discrepancies.push_back(
                        stage_label + ": missing execvp() evidence (status unknown).");
                }
            }
        }

        // 4. Process completion outcome
        if (st.wait_collected) {
            ++sum.recorded_outcomes;
            if (st.exited_normally) {
                if (st.exit_status == 0) {
                    ++sum.zero_exit_stages;
                } else {
                    ++sum.nonzero_exit_stages;
                    sum.has_runtime_failures = true;
                    sum.runtime_notes.push_back(
                        stage_label + ": exited with non-zero status " + std::to_string(st.exit_status) + ".");
                }
            } else if (st.signaled) {
                ++sum.signaled_stages;
                sum.has_runtime_failures = true;
                sum.runtime_notes.push_back(
                    stage_label + ": terminated by signal " + std::to_string(st.term_signal) +
                    " (" + signal_symbolic_name(st.term_signal) + ").");
            }
        } else if (st.stopped) {
            ++sum.stopped_stages;
            sum.has_unknown_evidence = true;
            sum.runtime_notes.push_back(
                stage_label + ": currently stopped by signal " + std::to_string(st.stop_signal) +
                " (" + signal_symbolic_name(st.stop_signal) + "); final exit status not yet known.");
        } else {
            ++sum.unknown_outcomes;
            sum.has_unknown_evidence = true;
            sum.runtime_notes.push_back(
                stage_label + ": final process completion has not been recorded yet (unknown / still running).");
        }
    }

    return sum;
}

void PlanVerifier::print_verification_report(const ExecutionTrace& trace) {
    VerificationSummary sum = verify(trace);

    std::cout << "ShellCraft Verification Report\n\n";
    std::cout << "Command:               " << trace.raw_command << "\n";
    std::cout << "Pipeline stages:       expected " << sum.expected_stages
              << " / observed " << sum.observed_stages_launched << "\n";

    std::cout << "Pipe setup:            expected " << sum.expected_pipes
              << " / succeeded " << sum.succeeded_pipes;
    if (sum.failed_pipes > 0) {
        std::cout << " (failed " << sum.failed_pipes << ")";
    }
    std::cout << "\n";

    std::cout << "Process launches:      expected " << sum.expected_forks
              << " / succeeded " << sum.succeeded_forks;
    if (sum.failed_forks > 0) {
        std::cout << " (failed " << sum.failed_forks << ")";
    }
    std::cout << "\n";

    std::cout << "Redirection setup:     ";
    if (sum.expected_redirs == 0) {
        std::cout << "none planned (0)\n";
    } else if (sum.failed_redirs == 0 && sum.unknown_redirs == 0 &&
               sum.succeeded_redirs == sum.expected_redirs) {
        std::cout << "succeeded (" << sum.succeeded_redirs << "/" << sum.expected_redirs << ")\n";
    } else {
        std::cout << "expected " << sum.expected_redirs
                  << " / succeeded " << sum.succeeded_redirs
                  << " / failed " << sum.failed_redirs
                  << " / unknown " << sum.unknown_redirs << "\n";
    }

    std::cout << "External execvp():     expected " << sum.expected_execs
              << " / succeeded " << sum.succeeded_execs;
    if (sum.failed_execs > 0) {
        std::cout << " / failed " << sum.failed_execs;
    }
    if (sum.unreached_execs > 0) {
        std::cout << " / not reached " << sum.unreached_execs;
    }
    std::cout << "\n";

    std::cout << "Process outcomes:      ";
    if (sum.unknown_outcomes == 0 && sum.stopped_stages == 0) {
        std::cout << "recorded (" << sum.recorded_outcomes << "/" << sum.expected_stages
                  << " completed: " << sum.zero_exit_stages << " status=0";
        if (sum.nonzero_exit_stages > 0) {
            std::cout << ", " << sum.nonzero_exit_stages << " non-zero";
        }
        if (sum.signaled_stages > 0) {
            std::cout << ", " << sum.signaled_stages << " signaled";
        }
        std::cout << ")\n";
    } else {
        std::cout << sum.recorded_outcomes << "/" << sum.expected_stages << " completed";
        if (sum.stopped_stages > 0) {
            std::cout << ", " << sum.stopped_stages << " stopped";
        }
        if (sum.unknown_outcomes > 0) {
            std::cout << ", " << sum.unknown_outcomes << " unknown (incomplete)";
        }
        std::cout << "\n";
    }

    if (!sum.discrepancies.empty()) {
        std::cout << "\nDetected Plan vs. Execution Discrepancies:\n";
        for (const auto& d : sum.discrepancies) {
            std::cout << "  - " << d << "\n";
        }
    }

    if (!sum.runtime_notes.empty()) {
        std::cout << "\nStage Runtime & Lifecycle Observations:\n";
        for (const auto& n : sum.runtime_notes) {
            std::cout << "  - " << n << "\n";
        }
    }

    std::cout << "\nResult: ";
    if (sum.setup_discrepancy) {
        std::cout << "Discrepancies detected between planned shell setup and observed execution.\n";
    } else if (sum.has_unknown_evidence) {
        std::cout << "Setup matched plan, but final process completion evidence is incomplete/unknown.\n";
    } else if (sum.has_runtime_failures) {
        std::cout << "No discrepancies in shell setup (pipes, forks, redirections, execvp all succeeded as planned), "
                     "but one or more stages exited non-zero or were terminated by a signal.\n";
    } else {
        std::cout << "No discrepancies detected in the tracked operations.\n";
    }
    std::cout << "Scope Note: Verification compares ShellCraft's execution plan against recorded shell-level events; "
                 "it does not prove absence of internal application bugs or OS kernel faults.\n";
}

std::vector<StageDiagnosis> PipelineAnalyzer::analyze(const ExecutionTrace& trace) {
    std::vector<StageDiagnosis> list;
    list.reserve(trace.stages.size());

    for (std::size_t i = 0; i < trace.stages.size(); ++i) {
        const auto& st = trace.stages[i];
        StageDiagnosis diag;
        diag.stage_number = i + 1;
        diag.formatted_command = st.formatted_command;
        diag.args = st.args;
        diag.pid = st.pid;
        diag.is_builtin = st.is_builtin;
        diag.forks_child = st.forks_child;

        // 1. Process creation status
        if (!st.forks_child && st.is_builtin) {
            diag.process_creation_status = "not required (executed in parent shell process)";
        } else if (st.fork_succeeded) {
            diag.process_creation_status = "succeeded (pid=" + std::to_string(st.pid) + ")";
        } else if (st.fork_attempted) {
            diag.process_creation_status = "failed (" + errno_symbolic_name(st.fork_errno) +
                                           ": " + std::strerror(st.fork_errno) + ")";
        } else {
            diag.process_creation_status = "not attempted (aborted by prior pipeline setup failure)";
        }

        // 2. Redirection status
        if (st.redir_expected == 0) {
            diag.redirection_status = "none configured";
        } else if (st.redir_failed) {
            diag.redirection_status = "FAILED on '" + st.failed_redir_op + " " + st.failed_redir_file +
                                      "' (" + errno_symbolic_name(st.redir_errno) + ": " +
                                      std::strerror(st.redir_errno) + ")";
        } else if (st.redir_succeeded == st.redir_expected) {
            diag.redirection_status = "succeeded (" + std::to_string(st.redir_succeeded) + "/" +
                                      std::to_string(st.redir_expected) + ")";
        } else {
            diag.redirection_status = "unknown / incomplete (" + std::to_string(st.redir_succeeded) +
                                      "/" + std::to_string(st.redir_expected) + " recorded)";
        }

        // 3. Execution status
        if (st.is_builtin) {
            diag.execution_status = st.builtin_executed
                                        ? "built-in command executed"
                                        : "built-in not reached (redirection failed)";
        } else if (st.exec_succeeded) {
            diag.execution_status = "execvp() succeeded";
        } else if (st.exec_failed) {
            diag.execution_status = "execvp() FAILED (" + errno_symbolic_name(st.exec_errno) +
                                    ": " + std::strerror(st.exec_errno) + ")";
        } else if (st.redir_failed) {
            diag.execution_status = "not attempted (aborted after redirection failure)";
        } else {
            diag.execution_status = "unknown / not reached";
        }

        // 4. Outcome & Root-Cause Analysis
        if (!st.fork_succeeded && st.forks_child) {
            diag.outcome_status = "stage did not run";
            if (trace.pipes_failed > 0) {
                diag.confirmed_cause = "Pipe creation failed with " +
                                       errno_symbolic_name(trace.last_pipe_errno) + " (" +
                                       std::strerror(trace.last_pipe_errno) + ").";
            } else if (st.fork_attempted) {
                diag.confirmed_cause = "Child process creation (fork) failed with " +
                                       errno_symbolic_name(st.fork_errno) + " (" +
                                       std::strerror(st.fork_errno) + ").";
            } else {
                diag.confirmed_cause = "Pipeline setup aborted before this stage was forked.";
            }
        } else if (st.redir_failed) {
            diag.outcome_status = "exited with status " + std::to_string(st.exit_status) + " before command execution";
            if (st.failed_redir_op == "<") {
                diag.confirmed_cause = "Input redirection file '" + st.failed_redir_file +
                                       "' could not be opened: " + errno_symbolic_name(st.redir_errno) +
                                       " (" + std::strerror(st.redir_errno) + ").";
            } else {
                diag.confirmed_cause = "Output redirection target '" + st.failed_redir_file +
                                       "' (" + st.failed_redir_op + ") could not be opened: " +
                                       errno_symbolic_name(st.redir_errno) + " (" +
                                       std::strerror(st.redir_errno) + ").";
            }
        } else if (st.exec_failed) {
            diag.outcome_status = "exited with status " + std::to_string(st.exit_status) + " after execvp() failure";
            std::string prog = st.args.empty() ? "" : st.args[0];
            if (st.exec_errno == ENOENT) {
                diag.confirmed_cause = "Executable '" + prog + "' was not found in PATH (" +
                                       errno_symbolic_name(st.exec_errno) + ": " +
                                       std::strerror(st.exec_errno) + ").";
            } else {
                diag.confirmed_cause = "Executable '" + prog + "' failed to launch (" +
                                       errno_symbolic_name(st.exec_errno) + ": " +
                                       std::strerror(st.exec_errno) + ").";
            }
        } else if (st.signaled) {
            diag.outcome_status = "terminated by signal " + std::to_string(st.term_signal) +
                                  " (" + signal_symbolic_name(st.term_signal) + ")";
            if (st.term_signal == SIGINT) {
                diag.confirmed_cause = "Process was interrupted and terminated by SIGINT (Ctrl+C).";
            } else if (st.term_signal == SIGPIPE) {
                diag.confirmed_cause = "Process received SIGPIPE (Broken pipe) when writing to a pipe "
                                       "whose downstream reader stage had already closed its input.";
            } else {
                diag.confirmed_cause = "Process was terminated by unhandled signal " +
                                       signal_symbolic_name(st.term_signal) + ".";
            }
        } else if (st.stopped) {
            diag.outcome_status = "stopped by signal " + std::to_string(st.stop_signal) +
                                  " (" + signal_symbolic_name(st.stop_signal) + ")";
            diag.confirmed_cause = "Process is currently suspended by " +
                                   signal_symbolic_name(st.stop_signal) +
                                   " (e.g., Ctrl+Z) and has not yet terminated.";
        } else if (!st.wait_collected) {
            diag.outcome_status = "unknown / incomplete (still running or not yet reaped)";
            diag.confirmed_cause = "No waitpid() termination event has been collected for this stage yet.";
        } else if (st.exited_normally) {
            diag.outcome_status = "exited normally with status " + std::to_string(st.exit_status);
            if (st.exit_status == 0) {
                diag.confirmed_cause = "Stage completed successfully (exit status 0).";
            } else {
                std::string prog = st.args.empty() ? "" : st.args[0];
                diag.confirmed_cause = "Program '" + prog +
                                       "' launched successfully and returned non-zero exit status " +
                                       std::to_string(st.exit_status) + ".";

                // Evidence-based inspection for common utilities
                if (prog == "grep" && st.exit_status == 1) {
                    // For grep, pattern is first non-option arg; file operands follow it
                    std::size_t first_non_opt = 1;
                    while (first_non_opt < st.args.size() && !st.args[first_non_opt].empty() &&
                           st.args[first_non_opt][0] == '-') {
                        ++first_non_opt;
                    }
                    std::string file_issue;
                    if (first_non_opt + 1 < st.args.size()) {
                        inspect_command_file_operands(st, first_non_opt + 1, file_issue);
                    }
                    if (!file_issue.empty()) {
                        diag.likely_explanation = file_issue;
                    } else {
                        diag.likely_explanation =
                            "For 'grep', exit status 1 normally means no input lines matched the search pattern "
                            "(not an OS system-call failure).";
                        if (i > 0 && trace.stages[i - 1].exit_status != 0) {
                            diag.likely_explanation +=
                                " Note that upstream Stage " + std::to_string(i) + " ('" +
                                trace.stages[i - 1].formatted_command +
                                "') also failed, which may have produced empty pipe input.";
                        }
                    }
                } else if (looks_like_file_command(prog)) {
                    inspect_command_file_operands(st, 1, diag.likely_explanation);
                    if (diag.likely_explanation.empty()) {
                        diag.likely_explanation =
                            "External program reported an application-level error or non-zero condition.";
                    }
                } else {
                    diag.likely_explanation =
                        "Non-zero exit status returned by the program itself (all shell setup calls succeeded).";
                }
            }
        }

        list.push_back(diag);
    }

    return list;
}

void PipelineAnalyzer::print_diagnostic_report(const ExecutionTrace& trace) {
    auto diagnoses = analyze(trace);

    std::cout << "ShellCraft Pipeline Failure & Stage Diagnostic Report\n\n";
    std::cout << "Command: " << trace.raw_command << "\n";
    std::cout << "Stages:  " << diagnoses.size() << "\n";

    for (const auto& d : diagnoses) {
        std::cout << "\nStage " << d.stage_number << ": " << d.formatted_command << "\n";
        std::cout << "  PID:                "
                  << (d.pid > 0 ? std::to_string(d.pid)
                                : (d.is_builtin && !d.forks_child ? "parent shell" : "none"))
                  << "\n";
        std::cout << "  Process creation:   " << d.process_creation_status << "\n";
        std::cout << "  Redirection setup:  " << d.redirection_status << "\n";
        std::cout << "  Program execution:  " << d.execution_status << "\n";
        std::cout << "  Observed outcome:   " << d.outcome_status << "\n";
        if (!d.confirmed_cause.empty()) {
            std::cout << "  Confirmed fact:     " << d.confirmed_cause << "\n";
        }
        if (!d.likely_explanation.empty()) {
            std::cout << "  Likely explanation: " << d.likely_explanation << "\n";
        }
    }
}

bool inspect_last_execution() {
    get_job_manager().reap_children();
    const ExecutionTrace* trace = get_trace_recorder().get_last_trace();
    if (!trace) {
        std::cerr << "shellcraft: inspect: no recorded execution available to inspect\n";
        return false;
    }
    PlanVerifier::print_verification_report(*trace);
    std::cout << "\n------------------------------------------------------------\n\n";
    PipelineAnalyzer::print_diagnostic_report(*trace);
    return true;
}

bool diagnose_last_execution() {
    get_job_manager().reap_children();
    const ExecutionTrace* trace = get_trace_recorder().get_last_trace();
    if (!trace) {
        std::cerr << "shellcraft: diagnose: no recorded execution available to diagnose\n";
        return false;
    }
    PipelineAnalyzer::print_diagnostic_report(*trace);
    return true;
}

} // namespace shellcraft
