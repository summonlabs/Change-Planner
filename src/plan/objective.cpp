#include "change_planner/plan/objective.hpp"

#include <algorithm>
#include <set>

namespace cplan {

const char* to_string(ObjectiveMetric metric) noexcept {
  switch (metric) {
    case ObjectiveMetric::minimum_stages:
      return "minimum-stages";
    case ObjectiveMetric::risk_exposure:
      return "risk-exposure";
    case ObjectiveMetric::churn:
      return "churn";
    case ObjectiveMetric::maintenance_duration:
      return "maintenance-duration";
  }
  return "unknown";
}

Result<ObjectiveMetric> parse_objective_metric(std::string_view token) {
  if (token == "minimum-stages" || token == "min-stages") return ObjectiveMetric::minimum_stages;
  if (token == "risk-exposure" || token == "min-risk") return ObjectiveMetric::risk_exposure;
  if (token == "churn" || token == "min-churn") return ObjectiveMetric::churn;
  if (token == "maintenance-duration" || token == "min-maintenance") {
    return ObjectiveMetric::maintenance_duration;
  }
  return Status::error(ErrorCode::malformed_input,
                       "unknown objective metric token: " + std::string(token));
}

Objective Objective::default_objective() {
  Objective objective;
  objective.code = "safety-first";
  objective.priority = {ObjectiveMetric::minimum_stages, ObjectiveMetric::risk_exposure,
                        ObjectiveMetric::churn, ObjectiveMetric::maintenance_duration};
  return objective;
}

Result<Objective> Objective::parse_code(std::string_view code) {
  Objective objective;
  if (code == "safety-first" || code == "min-stages") {
    objective.code = std::string(code);
    objective.priority = {ObjectiveMetric::minimum_stages, ObjectiveMetric::risk_exposure,
                          ObjectiveMetric::churn, ObjectiveMetric::maintenance_duration};
    return objective;
  }
  if (code == "min-risk") {
    objective.code = std::string(code);
    objective.priority = {ObjectiveMetric::risk_exposure, ObjectiveMetric::minimum_stages,
                          ObjectiveMetric::churn, ObjectiveMetric::maintenance_duration};
    return objective;
  }
  if (code == "min-churn") {
    objective.code = std::string(code);
    objective.priority = {ObjectiveMetric::churn, ObjectiveMetric::minimum_stages,
                          ObjectiveMetric::risk_exposure, ObjectiveMetric::maintenance_duration};
    return objective;
  }
  if (code == "min-maintenance") {
    objective.code = std::string(code);
    objective.priority = {ObjectiveMetric::maintenance_duration, ObjectiveMetric::minimum_stages,
                          ObjectiveMetric::risk_exposure, ObjectiveMetric::churn};
    return objective;
  }
  return Status::error(ErrorCode::invalid_argument,
                       "unknown objective code: " + std::string(code));
}

Result<Objective> Objective::from_metrics(std::vector<ObjectiveMetric> metrics) {
  if (metrics.empty()) {
    return Status::error(ErrorCode::invalid_argument, "objective priority list is empty");
  }
  std::set<std::uint8_t> seen;
  for (const ObjectiveMetric metric : metrics) {
    if (!seen.insert(static_cast<std::uint8_t>(metric)).second) {
      return Status::error(ErrorCode::invalid_argument,
                           "objective priority list repeats metric '" +
                               std::string(cplan::to_string(metric)) + "'");
    }
  }
  Objective objective;
  objective.code = "custom";
  objective.priority = std::move(metrics);
  return objective;
}

bool Objective::valid() const {
  if (priority.empty()) {
    return false;
  }
  std::set<std::uint8_t> seen;
  for (const ObjectiveMetric metric : priority) {
    if (!seen.insert(static_cast<std::uint8_t>(metric)).second) {
      return false;
    }
  }
  return true;
}

std::string Objective::to_string() const {
  std::string out = code;
  out += " [";
  for (std::size_t i = 0; i < priority.size(); ++i) {
    if (i != 0) {
      out += " > ";
    }
    out += cplan::to_string(priority[i]);
  }
  out += "]";
  return out;
}

Digest Objective::digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

void Objective::encode(CanonicalEncoder& out) const {
  out.put_tag("objective");
  encode_value(out, code);
  out.put_u32(static_cast<std::uint32_t>(priority.size()));
  for (const ObjectiveMetric metric : priority) {
    out.put_u8(static_cast<std::uint8_t>(metric));
  }
}

Objective Objective::decode(CanonicalDecoder& in) {
  Objective value;
  in.get_tag("objective");
  value.code = in.get_text();
  const std::uint32_t count = in.get_count(1);
  if (!in.ok()) {
    return value;
  }
  value.priority.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t raw = in.get_u8();
    if (!in.ok()) {
      return {};
    }
    if (raw > static_cast<std::uint8_t>(ObjectiveMetric::maintenance_duration)) {
      in.fail(ErrorCode::malformed_input, "objective metric out of range during decode");
      return {};
    }
    value.priority.push_back(static_cast<ObjectiveMetric>(raw));
  }
  return value;
}

static Digest candidate_tie_break(const std::vector<Stage>& stages,
                                   const std::vector<Step>& steps,
                                   const std::vector<DependencyEdge>& edges) {
  CanonicalEncoder encoder;
  encoder.put_tag("candidate-order");
  for (const Stage& stage : stages) {
    encode_value(encoder, stage.index);
    encode_sequence(encoder, stage.steps);
  }
  for (const Step& step : steps) {
    encode_value(encoder, step.id);
    encode_value(encoder, step.stage);
  }
  for (const DependencyEdge& edge : edges) {
    edge.encode(encoder);
  }
  return encoder.digest();
}

ObjectiveVector evaluate_objective_vector(const std::vector<Stage>& stages,
                                          const std::vector<Step>& steps,
                                          const std::vector<DependencyEdge>& edges) {
  ObjectiveVector vector;
  vector.stages = StageIndex(static_cast<std::uint32_t>(stages.size()));
  std::uint32_t maximum_stage_risk = 0;
  std::uint64_t maintenance_duration = 0;
  std::set<std::string> touched;
  for (const Step& step : steps) {
    for (const EntityRef& reference : step.operation.write_set()) {
      touched.insert(reference.to_string());
    }
    if (step.operation.requires_maintenance_window()) {
      maintenance_duration += step.estimated_duration.value();
    }
  }
  for (const Stage& stage : stages) {
    std::uint32_t stage_risk = 0;
    for (const StepId& step_id : stage.steps) {
      for (const Step& step : steps) {
        if (step.id == step_id) {
          stage_risk += step.risk.value();
          break;
        }
      }
    }
    maximum_stage_risk = std::max(maximum_stage_risk, stage_risk);
  }
  vector.risk_exposure = RiskScore(maximum_stage_risk);
  vector.churn = touched.size();
  vector.maintenance_duration = DurationNs(maintenance_duration);
  vector.tie_break = candidate_tie_break(stages, steps, edges);
  return vector;
}

bool objective_prefers(const Objective& objective, const ObjectiveVector& lhs,
                       const ObjectiveVector& rhs) {
  for (const ObjectiveMetric metric : objective.priority) {
    switch (metric) {
      case ObjectiveMetric::minimum_stages:
        if (!(lhs.stages == rhs.stages)) return lhs.stages < rhs.stages;
        break;
      case ObjectiveMetric::risk_exposure:
        if (!(lhs.risk_exposure == rhs.risk_exposure)) {
          return lhs.risk_exposure < rhs.risk_exposure;
        }
        break;
      case ObjectiveMetric::churn:
        if (lhs.churn != rhs.churn) return lhs.churn < rhs.churn;
        break;
      case ObjectiveMetric::maintenance_duration:
        if (!(lhs.maintenance_duration == rhs.maintenance_duration)) {
          return lhs.maintenance_duration < rhs.maintenance_duration;
        }
        break;
    }
  }
  // Deterministic final tie-break: the smaller content digest wins.
  return lhs.tie_break < rhs.tie_break;
}

std::string explain_objective_loss(const Objective& objective, const ObjectiveVector& candidate,
                                   const ObjectiveVector& winner) {
  for (const ObjectiveMetric metric : objective.priority) {
    switch (metric) {
      case ObjectiveMetric::minimum_stages:
        if (!(candidate.stages == winner.stages)) {
          return "candidate needs " + std::to_string(candidate.stages.value()) +
                 " stage(s) against " + std::to_string(winner.stages.value()) +
                 " (objective prioritises " + std::string(to_string(metric)) + ")";
        }
        break;
      case ObjectiveMetric::risk_exposure:
        if (!(candidate.risk_exposure == winner.risk_exposure)) {
          return "candidate risk exposure " + std::to_string(candidate.risk_exposure.value()) +
                 " exceeds " + std::to_string(winner.risk_exposure.value()) +
                 " (objective prioritises " + std::string(to_string(metric)) + ")";
        }
        break;
      case ObjectiveMetric::churn:
        if (candidate.churn != winner.churn) {
          return "candidate touches " + std::to_string(candidate.churn) +
                 " entities against " + std::to_string(winner.churn) +
                 " (objective prioritises " + std::string(to_string(metric)) + ")";
        }
        break;
      case ObjectiveMetric::maintenance_duration:
        if (!(candidate.maintenance_duration == winner.maintenance_duration)) {
          return "candidate maintenance duration " +
                 std::to_string(candidate.maintenance_duration.value()) + "ns exceeds " +
                 std::to_string(winner.maintenance_duration.value()) + "ns (objective prioritises " +
                 std::string(to_string(metric)) + ")";
        }
        break;
    }
  }
  return "candidate lost the deterministic tie-break on content digest";
}

}  // namespace cplan
