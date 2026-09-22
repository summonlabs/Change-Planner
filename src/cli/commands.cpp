#include "change_planner/cli/commands.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "change_planner/cli/io.hpp"
#include "change_planner/persist/artifact.hpp"
#include "change_planner/plan/graph.hpp"
#include "change_planner/version.hpp"

namespace cplan::cli {
namespace {

ExitCode exit_code_for(const Status& status) {
  switch (status.code()) {
    case ErrorCode::not_found:
    case ErrorCode::truncated:
    case ErrorCode::integrity_failure:
    case ErrorCode::unsupported_version:
    case ErrorCode::size_limit:
      return ExitCode::io_error;
    case ErrorCode::invalid_argument:
    case ErrorCode::malformed_input:
    case ErrorCode::duplicate_identity:
      return ExitCode::invalid_document;
    default:
      return ExitCode::invalid_document;
  }
}

struct Arguments {
  std::map<std::string, std::string> options;
  std::vector<std::string> positional;

  [[nodiscard]] bool has(const std::string& key) const { return options.count(key) != 0; }
  [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = "") const {
    const auto entry = options.find(key);
    return entry == options.end() ? fallback : entry->second;
  }
};

Result<Arguments> parse_arguments(const std::vector<std::string>& arguments) {
  Arguments parsed;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& token = arguments[i];
    if (token.rfind("--", 0) != 0) {
      parsed.positional.push_back(token);
      continue;
    }
    const std::size_t separator = token.find('=');
    if (separator != std::string::npos) {
      parsed.options[token.substr(2, separator - 2)] = token.substr(separator + 1);
      continue;
    }
    const std::string key = token.substr(2);
    if (i + 1 < arguments.size() && arguments[i + 1].rfind("--", 0) != 0) {
      parsed.options[key] = arguments[i + 1];
      ++i;
    } else {
      parsed.options[key] = "true";
    }
  }
  return parsed;
}

Result<PlanArtifact> load_artifact(const Arguments& arguments, const char* key = "plan") {
  if (!arguments.has(key)) {
    return Status::error(ErrorCode::invalid_argument, std::string("--") + key + " PATH is required");
  }
  return read_plan_artifact(arguments.get(key));
}

// A failure to read an artifact is an IO/integrity failure, not an invalid user
// document: the operator handed us a plan, not a request.
ExitCode exit_code_for_artifact(const Status& status) {
  if (status.code() == ErrorCode::invalid_argument) {
    return ExitCode::usage;  // a missing flag, not a bad file
  }
  return ExitCode::io_error;
}

Result<PlanRequest> load_request(const Arguments& arguments) {
  if (!arguments.has("input")) {
    return Status::error(ErrorCode::invalid_argument, "--input PATH is required");
  }
  auto document = read_json_file(arguments.get("input"));
  if (!document.ok()) {
    return document.status();
  }
  return parse_plan_request(document.value());
}

std::string require_step_argument(const Arguments& arguments, std::string* error) {
  if (!arguments.has("step")) {
    *error = "--step STEP_ID is required";
    return {};
  }
  return arguments.get("step");
}

CommandResult finish(const CommandResult& result, const GlobalOptions& options) {
  if (!result.output.empty()) {
    std::cout << result.output;
    if (result.output.back() != '\n') {
      std::cout << '\n';
    }
  }
  if (!result.error.empty() && !options.quiet) {
    std::cerr << result.error;
    if (result.error.back() != '\n') {
      std::cerr << '\n';
    }
  }
  return result;
}

CommandResult command_plan(const Arguments& arguments, const GlobalOptions& options) {
  auto request = load_request(arguments);
  if (!request.ok()) {
    return CommandResult{exit_code_for(request.status()), "", request.status().to_string()};
  }
  PlanRequest plan_request = request.value();
  if (arguments.has("label")) {
    plan_request.label = arguments.get("label");
  }
  if (arguments.has("window-shift")) {
    plan_request.limits.allow_window_shift = true;
  }
  const PlanResult result = generate_plan(plan_request);

  VerificationReport report;
  const bool verify_requested = arguments.has("verify");
  if (verify_requested) {
    report = verify_plan(result.plan(), plan_request, true);
  }

  CommandResult outcome;
  if (result.ok()) {
    outcome.code = verify_requested && !report.ok ? ExitCode::refused : ExitCode::success;
    if (options.json) {
      // Machine-readable output is exactly one JSON document per invocation.
      JsonValue::object_type members;
      members.emplace_back("plan", plan_to_json(result.plan()));
      if (verify_requested) {
        members.emplace_back("verification", verification_to_json(report));
      }
      if (arguments.has("out")) {
        members.emplace_back("artifactPath", JsonValue::make_string(arguments.get("out")));
      }
      outcome.output = write_json(JsonValue::make_object(std::move(members)), JsonStyle::pretty);
    } else {
      outcome.output = describe_plan(result.plan());
      if (arguments.has("out")) {
        outcome.output += "\nwrote artifact " + arguments.get("out");
      }
      if (verify_requested) {
        outcome.output += "\n" + describe_verification(report);
      }
    }
    if (arguments.has("out")) {
      const Status written = write_plan_artifact(arguments.get("out"), result.plan());
      if (!written.ok()) {
        return CommandResult{ExitCode::io_error, outcome.output, written.to_string()};
      }
    }
    return outcome;
  }

  const Refusal& refusal = result.refusal();
  outcome.code = refusal.code == RefusalCode::cancelled ? ExitCode::io_error : ExitCode::refused;
  outcome.output = options.json ? write_json(refusal_to_json(refusal), JsonStyle::pretty)
                                : refusal.explain();
  return outcome;
}

CommandResult command_why_rejected(const Arguments& arguments, const GlobalOptions& options) {
  auto request = load_request(arguments);
  if (!request.ok()) {
    return CommandResult{exit_code_for(request.status()), "", request.status().to_string()};
  }
  const PlanResult result = generate_plan(request.value());
  CommandResult outcome;
  if (result.ok()) {
    JsonValue::object_type members;
    members.emplace_back("outcome", JsonValue::make_string("planned"));
    members.emplace_back("planId", JsonValue::make_string(result.plan().id.str()));
    JsonValue::array_type rejected;
    for (const RejectedAlternative& alternative : result.plan().rejected) {
      JsonValue::object_type entry;
      entry.emplace_back("code", JsonValue::make_string(alternative.code));
      entry.emplace_back("detail", JsonValue::make_string(alternative.detail));
      entry.emplace_back("invariant", JsonValue::make_string(to_string(alternative.violated)));
      rejected.push_back(JsonValue::make_object(std::move(entry)));
    }
    members.emplace_back("rejectedAlternatives", JsonValue::make_array(std::move(rejected)));
    const JsonValue document = JsonValue::make_object(std::move(members));
    if (options.json) {
      outcome.output = write_json(document, JsonStyle::pretty);
    } else {
      outcome.output = "a safe plan exists; the search rejected these alternatives:";
      for (const RejectedAlternative& alternative : result.plan().rejected) {
        outcome.output += "\n  - " + alternative.code + ": " + alternative.detail;
      }
      if (result.plan().rejected.empty()) {
        outcome.output += "\n  (none: every candidate ordering was accepted)";
      }
    }
    outcome.code = ExitCode::success;
    return outcome;
  }
  outcome.code = ExitCode::refused;
  outcome.output = options.json ? write_json(refusal_to_json(result.refusal()), JsonStyle::pretty)
                                : result.refusal().explain();
  return outcome;
}

CommandResult command_explain(const Arguments& arguments, const GlobalOptions& options) {
  auto artifact = load_artifact(arguments);
  if (!artifact.ok()) {
    return CommandResult{exit_code_for_artifact(artifact.status()), "", artifact.status().to_string()};
  }
  std::string error;
  const std::string step_text = require_step_argument(arguments, &error);
  if (!error.empty()) {
    return CommandResult{ExitCode::usage, "", error};
  }
  const auto step_id = StepId::parse(step_text);
  if (!step_id.ok()) {
    return CommandResult{ExitCode::invalid_document, "", step_id.status().to_string()};
  }
  auto explanation = explain_step(artifact.value().plan, step_id.value());
  if (!explanation.ok()) {
    return CommandResult{exit_code_for(explanation.status()), "", explanation.status().to_string()};
  }
  CommandResult outcome;
  outcome.code = ExitCode::success;
  outcome.output = options.json
                       ? write_json(step_explanation_to_json(explanation.value()), JsonStyle::pretty)
                       : explanation.value().explain();
  return outcome;
}

CommandResult command_deps(const Arguments& arguments, const GlobalOptions& options) {
  auto artifact = load_artifact(arguments);
  if (!artifact.ok()) {
    return CommandResult{exit_code_for_artifact(artifact.status()), "", artifact.status().to_string()};
  }
  const Plan& plan = artifact.value().plan;
  CommandResult outcome;
  outcome.code = ExitCode::success;
  if (arguments.has("step")) {
    auto explanation = explain_step(plan, StepId::parse(arguments.get("step")).value_or(StepId{}));
    if (!explanation.ok()) {
      return CommandResult{exit_code_for(explanation.status()), "", explanation.status().to_string()};
    }
    outcome.output = options.json
                         ? write_json(step_explanation_to_json(explanation.value()), JsonStyle::pretty)
                         : explanation.value().explain();
    return outcome;
  }
  JsonValue::array_type edges;
  for (const DependencyEdge& edge : plan.edges) {
    JsonValue::object_type entry;
    entry.emplace_back("from", JsonValue::make_string(edge.from.str()));
    entry.emplace_back("to", JsonValue::make_string(edge.to.str()));
    entry.emplace_back("reason", JsonValue::make_string(to_string(edge.reason)));
    entry.emplace_back("detail", JsonValue::make_string(edge.detail_code));
    edges.push_back(JsonValue::make_object(std::move(entry)));
  }
  if (options.json) {
    JsonValue::object_type members;
    members.emplace_back("planId", JsonValue::make_string(plan.id.str()));
    members.emplace_back("dependencies", JsonValue::make_array(std::move(edges)));
    JsonValue::array_type stages;
    for (const Stage& stage : plan.stages) {
      JsonValue::object_type entry;
      entry.emplace_back("index", JsonValue::make_uint(stage.index.value()));
      JsonValue::array_type steps;
      for (const StepId& id : stage.steps) {
        steps.push_back(JsonValue::make_string(id.str()));
      }
      entry.emplace_back("steps", JsonValue::make_array(std::move(steps)));
      stages.push_back(JsonValue::make_object(std::move(entry)));
    }
    members.emplace_back("stages", JsonValue::make_array(std::move(stages)));
    outcome.output = write_json(JsonValue::make_object(std::move(members)), JsonStyle::pretty);
    return outcome;
  }
  outcome.output = "dependency edges of plan " + plan.id.str() + ":";
  if (plan.edges.empty()) {
    outcome.output += "\n  (none)";
  }
  for (const DependencyEdge& edge : plan.edges) {
    outcome.output += "\n  " + edge.from.str() + " -> " + edge.to.str() + "  [" +
                      std::string(to_string(edge.reason)) + ": " + edge.detail_code + "]";
  }
  outcome.output += "\nstages:";
  for (const Stage& stage : plan.stages) {
    outcome.output += "\n  stage " + std::to_string(stage.index.value()) + ":";
    for (const StepId& id : stage.steps) {
      outcome.output += " " + id.str();
    }
  }
  return outcome;
}

CommandResult command_compare(const Arguments& arguments, const GlobalOptions& options) {
  auto left = load_artifact(arguments, "left");
  if (!left.ok()) {
    return CommandResult{exit_code_for_artifact(left.status()), "", left.status().to_string()};
  }
  auto right = load_artifact(arguments, "right");
  if (!right.ok()) {
    return CommandResult{exit_code_for_artifact(right.status()), "", right.status().to_string()};
  }
  const PlanComparison comparison = compare_plans(left.value().plan, right.value().plan);
  CommandResult outcome;
  outcome.code = ExitCode::success;
  outcome.output = options.json ? write_json(comparison_to_json(comparison), JsonStyle::pretty)
                                : comparison.explain();
  return outcome;
}

CommandResult command_validate(const Arguments& arguments, const GlobalOptions& options) {
  auto artifact = load_artifact(arguments);
  if (!artifact.ok()) {
    return CommandResult{exit_code_for_artifact(artifact.status()), "", artifact.status().to_string()};
  }
  auto request = load_request(arguments);
  if (!request.ok()) {
    return CommandResult{exit_code_for(request.status()), "", request.status().to_string()};
  }
  const InvalidationContext context{request.value().state, request.value().evidence,
                                    request.value().target, request.value().constraints,
                                    request.value().planning_instant};
  const InvalidationDecision decision = evaluate_validity(artifact.value().plan, context);
  const VerificationReport report =
      verify_plan(artifact.value().plan, request.value(), arguments.has("exhaustive"));

  JsonValue::object_type members;
  members.emplace_back("valid", JsonValue::make_bool(decision.valid));
  members.emplace_back("invalidation", invalidation_to_json(decision));
  members.emplace_back("verification", verification_to_json(report));
  CommandResult outcome;
  outcome.code = (decision.valid && report.ok) ? ExitCode::success : ExitCode::refused;
  outcome.output = options.json
                       ? write_json(JsonValue::make_object(std::move(members)), JsonStyle::pretty)
                       : describe_invalidation(decision) + "\n" + describe_verification(report);
  return outcome;
}

CommandResult command_replan(const Arguments& arguments, const GlobalOptions& options) {
  auto artifact = load_artifact(arguments);
  if (!artifact.ok()) {
    return CommandResult{exit_code_for_artifact(artifact.status()), "", artifact.status().to_string()};
  }
  if (!arguments.has("observed")) {
    return CommandResult{ExitCode::usage, "", "--observed PATH is required"};
  }
  auto document = read_json_file(arguments.get("observed"));
  if (!document.ok()) {
    return CommandResult{exit_code_for(document.status()), "", document.status().to_string()};
  }
  auto observed = parse_observed_execution(document.value());
  if (!observed.ok()) {
    return CommandResult{exit_code_for(observed.status()), "", observed.status().to_string()};
  }
  auto request = load_request(arguments);
  if (!request.ok()) {
    return CommandResult{exit_code_for(request.status()), "", request.status().to_string()};
  }
  ReplanningOptions replan_options;
  replan_options.limits = request.value().limits;
  replan_options.compensate_partial_steps = !arguments.has("no-compensate");
  const bool rollback = arguments.get("mode", "replan") == "rollback";
  const PlanResult result =
      rollback ? build_rollback_plan(artifact.value().plan, observed.value(), request.value(),
                                     replan_options)
               : replan(artifact.value().plan, observed.value(), request.value(), replan_options);

  CommandResult outcome;
  if (!result.ok()) {
    outcome.code = ExitCode::refused;
    outcome.output = options.json ? write_json(refusal_to_json(result.refusal()), JsonStyle::pretty)
                                  : result.refusal().explain();
    return outcome;
  }
  outcome.code = ExitCode::success;
  outcome.output = options.json ? write_json(plan_to_json(result.plan()), JsonStyle::pretty)
                                : describe_plan(result.plan());
  if (arguments.has("out")) {
    const Status written = write_plan_artifact(arguments.get("out"), result.plan());
    if (!written.ok()) {
      return CommandResult{ExitCode::io_error, outcome.output, written.to_string()};
    }
    if (!options.json) {
      outcome.output += "\nwrote artifact " + arguments.get("out");
    }
  }
  return outcome;
}

CommandResult command_inspect(const Arguments& arguments, const GlobalOptions& options) {
  auto artifact = load_artifact(arguments);
  if (!artifact.ok()) {
    return CommandResult{exit_code_for_artifact(artifact.status()), "", artifact.status().to_string()};
  }
  CommandResult outcome;
  outcome.code = ExitCode::success;
  outcome.output = options.json ? write_json(plan_to_json(artifact.value().plan), JsonStyle::pretty)
                                : describe_plan(artifact.value().plan);
  if (!options.json) {
    outcome.output += "\n  integrity: verified (" +
                      std::string(artifact.value().integrity_verified ? "yes" : "no") +
                      "), payload " + std::to_string(artifact.value().payload_bytes) + " bytes";
    outcome.output += "\n  artifact payload digest: " + artifact.value().payload_digest.hex();
    outcome.output += "\n  freshness: NOT assumed - run 'validate' against current state";
  }
  return outcome;
}

CommandResult command_scan(const Arguments& arguments, const GlobalOptions& options) {
  if (!arguments.has("directory")) {
    return CommandResult{ExitCode::usage, "", "--directory PATH is required"};
  }
  auto scan = scan_plan_directory(arguments.get("directory"));
  if (!scan.ok()) {
    return CommandResult{exit_code_for(scan.status()), "", scan.status().to_string()};
  }
  JsonValue::array_type entries;
  std::string text;
  for (const ArtifactScanEntry& entry : scan.value().entries) {
    JsonValue::object_type item;
    item.emplace_back("path", JsonValue::make_string(entry.path));
    item.emplace_back("readable", JsonValue::make_bool(entry.readable));
    item.emplace_back("planId", JsonValue::make_string(entry.plan_id.str()));
    item.emplace_back("generation", JsonValue::make_uint(entry.generation.value()));
    item.emplace_back("payloadDigest", JsonValue::make_string(entry.payload_digest.hex()));
    item.emplace_back("error", JsonValue::make_string(entry.error));
    entries.push_back(JsonValue::make_object(std::move(item)));
    text += entry.path + (entry.readable ? "  OK  " + entry.payload_digest.short_hex()
                                         : "  UNREADABLE  " + entry.error) +
            "\n";
  }
  CommandResult outcome;
  outcome.code = scan.value().unreadable_count == 0 ? ExitCode::success : ExitCode::refused;
  if (options.json) {
    JsonValue::object_type members;
    members.emplace_back("readable", JsonValue::make_uint(scan.value().readable_count));
    members.emplace_back("unreadable", JsonValue::make_uint(scan.value().unreadable_count));
    members.emplace_back("entries", JsonValue::make_array(std::move(entries)));
    outcome.output = write_json(JsonValue::make_object(std::move(members)), JsonStyle::pretty);
  } else {
    outcome.output = text + std::to_string(scan.value().readable_count) + " readable, " +
                     std::to_string(scan.value().unreadable_count) + " unreadable";
  }
  return outcome;
}

}  // namespace

std::string usage_text() {
  return std::string(kProductName) + " " + kVersionString + " - " + kVendorName + R"(
Vendor-neutral change planning runtime. Plans are computed, verified and reasoned about;
this tool never distributes configuration or executes rollout stages.

usage: cplan <command> [options]

commands:
  plan           generate a plan (or a refusal) from a request document
  why-rejected   plan and report the alternatives the search rejected
  explain        explain one step: preconditions, dependencies, compensation
  deps           list dependency edges and stages of a plan
  compare        compare two plan artifacts step by step
  validate       re-verify a stored plan and check it against current inputs
  replan         replan from an observed partial execution state
  rollback       build a compensation plan back to the pre-plan state
  inspect        inspect a stored artifact (content, binding, integrity)
  scan           scan a directory of artifacts and report unreadable ones
  version        print version information
  help           print this text

common options:
  --json                 machine-readable output
  --quiet                suppress diagnostics on stderr
  --input PATH           planning request document (required by plan/why-rejected/validate/replan)
  --plan PATH            plan artifact (explain/deps/inspect/validate/replan/rollback)
  --observed PATH        observed execution document (replan/rollback)
  --out PATH             write the generated plan as an artifact
  --step STEP_ID         restrict explain/deps to one step
  --left PATH --right PATH   artifacts for compare
  --verify               verify the generated plan before reporting success
  --window-shift         allow the planner to shift the maintenance instant
  --mode rollback        replan in rollback mode
  --exhaustive           verify every interleaving (validate)
  --directory PATH       directory for scan

exit codes: 0 success, 1 refused (no safe plan exists), 2 usage, 3 invalid document, 4 IO/integrity
)";
}

CommandResult run_command(const std::vector<std::string>& arguments, const GlobalOptions& options) {
  if (arguments.empty()) {
    return CommandResult{ExitCode::usage, usage_text(), ""};
  }
  const std::string& command = arguments.front();
  std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
  auto parsed = parse_arguments(rest);
  if (!parsed.ok()) {
    return CommandResult{ExitCode::usage, "", parsed.status().to_string()};
  }
  const Arguments& args = parsed.value();

  if (command == "help" || command == "--help" || command == "-h") {
    return CommandResult{ExitCode::success, usage_text(), ""};
  }
  if (command == "version" || command == "--version") {
    JsonValue::object_type members;
    members.emplace_back("product", JsonValue::make_string(kProductName));
    members.emplace_back("version", JsonValue::make_string(kVersionString));
    members.emplace_back("vendor", JsonValue::make_string(kVendorName));
    members.emplace_back("artifactFormatVersion", JsonValue::make_uint(kArtifactFormatVersion));
    members.emplace_back("planSchemaVersion", JsonValue::make_uint(kPlanSchemaVersion));
    members.emplace_back("inputSchemaVersion", JsonValue::make_string(kRequestSchemaName));
    CommandResult outcome;
    outcome.code = ExitCode::success;
    outcome.output = options.json
                         ? write_json(JsonValue::make_object(std::move(members)), JsonStyle::pretty)
                         : std::string(kProductName) + " " + kVersionString + " (" + kVendorName +
                               ")\nartifact format " + std::to_string(kArtifactFormatVersion) +
                               ", plan schema " + std::to_string(kPlanSchemaVersion) +
                               ", input schema " + kRequestSchemaName;
    return outcome;
  }
  if (command == "plan") return command_plan(args, options);
  if (command == "why-rejected") return command_why_rejected(args, options);
  if (command == "explain") return command_explain(args, options);
  if (command == "deps") return command_deps(args, options);
  if (command == "compare") return command_compare(args, options);
  if (command == "validate") return command_validate(args, options);
  if (command == "replan" || command == "rollback") {
    Arguments effective = args;
    if (command == "rollback" && !effective.has("mode")) {
      // The subcommand selects the mode; the flag only overrides it.
      effective.options["mode"] = "rollback";
    }
    return command_replan(effective, options);
  }
  if (command == "inspect") return command_inspect(args, options);
  if (command == "scan") return command_scan(args, options);

  return CommandResult{ExitCode::usage, "",
                       "unknown command '" + command + "'\n" + usage_text()};
}

int main_entry(int argc, char** argv) {
  GlobalOptions options;
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--json") {
      options.json = true;
      continue;
    }
    if (token == "--quiet") {
      options.quiet = true;
      continue;
    }
    arguments.push_back(token);
  }
  const CommandResult result = finish(run_command(arguments, options), options);
  return static_cast<int>(result.code);
}

}  // namespace cplan::cli
