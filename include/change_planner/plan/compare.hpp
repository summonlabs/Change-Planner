#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "change_planner/plan/plan.hpp"

namespace cplan {

enum class StepChangeKind : std::uint8_t { added = 0, removed, moved, redefined };

const char* to_string(StepChangeKind kind) noexcept;

struct StepDifference {
  StepId id;
  StepChangeKind kind{StepChangeKind::added};
  bool present_left{false};
  bool present_right{false};
  StageIndex left_stage;
  StageIndex right_stage;
  std::string detail;

  friend bool operator==(const StepDifference&, const StepDifference&) = default;
  friend bool operator<(const StepDifference& lhs, const StepDifference& rhs) {
    if (lhs.id != rhs.id) return lhs.id < rhs.id;
    return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
  }
};

struct PlanComparison {
  bool identical{false};
  bool semantically_identical{false};
  Digest left_content;
  Digest right_content;
  std::size_t left_stages{0};
  std::size_t right_stages{0};
  std::size_t left_steps{0};
  std::size_t right_steps{0};
  std::vector<StepDifference> differences;
  std::vector<StepId> common_steps;
  std::string summary;

  [[nodiscard]] std::string explain() const;
};

[[nodiscard]] PlanComparison compare_plans(const Plan& left, const Plan& right);

// Full, deterministic explanation of one step: what it does, why it is ordered
// where it is, which invariants were checked, and how it can be compensated.
struct StepExplanation {
  StepId id;
  StageIndex stage;
  std::string operation;
  OperationKind operation_kind{OperationKind::device_add};
  OperationClass operation_class{OperationClass::configuration};
  std::vector<Condition> preconditions;
  std::vector<Condition> postconditions;
  std::vector<DependencyEdge> incoming;
  std::vector<DependencyEdge> outgoing;
  std::vector<StepId> concurrent_steps;
  Compensation compensation;
  RiskScore risk;
  DurationNs estimated_duration;
  std::vector<WindowId> windows;
  Justification justification;

  [[nodiscard]] std::string explain() const;
};

[[nodiscard]] Result<StepExplanation> explain_step(const Plan& plan, const StepId& id);
[[nodiscard]] std::vector<StepExplanation> explain_steps(const Plan& plan);

}  // namespace cplan
