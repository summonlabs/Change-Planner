#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "change_planner/plan/plan.hpp"

namespace cplan {

enum class ObjectiveMetric : std::uint8_t {
  minimum_stages = 0,
  risk_exposure,
  churn,
  maintenance_duration,
};

const char* to_string(ObjectiveMetric metric) noexcept;
Result<ObjectiveMetric> parse_objective_metric(std::string_view token);

// A deterministic, lexicographic optimisation objective. The priority list is
// ordered: the first metric dominates every later metric. The final tie-break is
// always the content digest of the candidate, so identical inputs and policy
// always select the same plan.
struct Objective {
  std::string code{"safety-first"};
  std::vector<ObjectiveMetric> priority;

  friend bool operator==(const Objective&, const Objective&) = default;

  [[nodiscard]] static Objective default_objective();
  [[nodiscard]] static Result<Objective> parse_code(std::string_view code);
  [[nodiscard]] static Result<Objective> from_metrics(std::vector<ObjectiveMetric> metrics);
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] Digest digest() const;

  void encode(CanonicalEncoder& out) const;
  static Objective decode(CanonicalDecoder& in);
};

// Computes the objective metrics of a candidate ordering: number of stages, peak
// per-stage risk, touched entities and maintenance duration, plus the
// deterministic content tie-break.
[[nodiscard]] ObjectiveVector evaluate_objective_vector(const std::vector<Stage>& stages,
                                                        const std::vector<Step>& steps,
                                                        const std::vector<DependencyEdge>& edges);

// Lexicographic comparison under the objective. Returns true when lhs is the
// better (preferred) plan.
[[nodiscard]] bool objective_prefers(const Objective& objective, const ObjectiveVector& lhs,
                                     const ObjectiveVector& rhs);

// Human-readable account of why one vector lost to another.
[[nodiscard]] std::string explain_objective_loss(const Objective& objective,
                                                 const ObjectiveVector& candidate,
                                                 const ObjectiveVector& winner);

}  // namespace cplan
