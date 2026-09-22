#pragma once

#include <string>
#include <vector>

#include "change_planner/core/status.hpp"

namespace cplan::cli {

enum class ExitCode : int {
  success = 0,
  refused = 1,
  usage = 2,
  invalid_document = 3,
  io_error = 4,
};

struct CommandResult {
  ExitCode code{ExitCode::success};
  std::string output;
  std::string error;
};

struct GlobalOptions {
  bool json{false};
  bool quiet{false};
};

[[nodiscard]] std::string usage_text();

// Executes one command. Never throws for expected failures: every failure is a
// typed exit code plus a diagnostic.
[[nodiscard]] CommandResult run_command(const std::vector<std::string>& arguments,
                                        const GlobalOptions& options);

[[nodiscard]] int main_entry(int argc, char** argv);

}  // namespace cplan::cli
