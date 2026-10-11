#ifndef SHELLCRAFT_VERIFIER_HPP
#define SHELLCRAFT_VERIFIER_HPP

#include "tracer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace shellcraft {

struct VerificationSummary {
    std::size_t expected_stages = 0;
    std::size_t observed_stages_launched = 0;

    std::size_t expected_pipes = 0;
    std::size_t succeeded_pipes = 0;
    std::size_t failed_pipes = 0;

    std::size_t expected_forks = 0;
    std::size_t succeeded_forks = 0;
    std::size_t failed_forks = 0;

    std::size_t expected_redirs = 0;
    std::size_t succeeded_redirs = 0;
    std::size_t failed_redirs = 0;
    std::size_t unknown_redirs = 0;

    std::size_t expected_execs = 0;
    std::size_t attempted_execs = 0;
    std::size_t succeeded_execs = 0;
    std::size_t failed_execs = 0;
    std::size_t unreached_execs = 0;

    std::size_t recorded_outcomes = 0;
    std::size_t unknown_outcomes = 0;
    std::size_t zero_exit_stages = 0;
    std::size_t nonzero_exit_stages = 0;
    std::size_t signaled_stages = 0;
    std::size_t stopped_stages = 0;

    bool setup_discrepancy = false;
    bool has_unknown_evidence = false;
    bool has_runtime_failures = false;

    std::vector<std::string> discrepancies;
    std::vector<std::string> runtime_notes;
};

struct StageDiagnosis {
    std::size_t stage_number = 0;
    std::string formatted_command;
    std::vector<std::string> args;
    pid_t pid = 0;
    bool is_builtin = false;
    bool forks_child = false;

    std::string process_creation_status;
    std::string redirection_status;
    std::string execution_status;
    std::string outcome_status;
    std::string confirmed_cause;
    std::string likely_explanation;
};

class PlanVerifier {
public:
    // Compare predicted ExecutionPlan against recorded ExecutionTrace outcomes
    static VerificationSummary verify(const ExecutionTrace& trace);

    // Print the Verification Report for the given ExecutionTrace
    static void print_verification_report(const ExecutionTrace& trace);
};

class PipelineAnalyzer {
public:
    // Analyze all stages in an ExecutionTrace and produce evidence-based diagnostics
    static std::vector<StageDiagnosis> analyze(const ExecutionTrace& trace);

    // Print the Pipeline Diagnostic Report for the given ExecutionTrace
    static void print_diagnostic_report(const ExecutionTrace& trace);
};

// Convenience helpers used by 'inspect last' and 'diagnose last'
bool inspect_last_execution();
bool diagnose_last_execution();

} // namespace shellcraft

#endif // SHELLCRAFT_VERIFIER_HPP
