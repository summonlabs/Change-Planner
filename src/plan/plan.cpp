#include "change_planner/plan/plan.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "change_planner/core/checked.hpp"
#include "change_planner/plan/graph.hpp"

namespace cplan {

const char* to_string(CompensationKind kind) noexcept {
  switch (kind) {
    case CompensationKind::inverse:
      return "inverse";
    case CompensationKind::reapply:
      return "reapply";
    case CompensationKind::manual:
      return "manual";
  }
  return "unknown";
}

Result<CompensationKind> parse_compensation_kind(std::string_view token) {
  if (token == "inverse") return CompensationKind::inverse;
  if (token == "reapply") return CompensationKind::reapply;
  if (token == "manual") return CompensationKind::manual;
  return Status::error(ErrorCode::malformed_input,
                       "unknown compensation kind token: " + std::string(token));
}

const char* to_string(DependencyReason reason) noexcept {
  switch (reason) {
    case DependencyReason::structural_prerequisite:
      return "structural-prerequisite";
    case DependencyReason::write_write_conflict:
      return "write-write-conflict";
    case DependencyReason::write_read_conflict:
      return "write-read-conflict";
    case DependencyReason::domain_serialization:
      return "domain-serialization";
    case DependencyReason::contract_serialization:
      return "contract-serialization";
    case DependencyReason::maintenance_ordering:
      return "maintenance-ordering";
    case DependencyReason::invariant_ordering:
      return "invariant-ordering";
    case DependencyReason::deterministic_tie_break:
      return "deterministic-tie-break";
  }
  return "unknown";
}

Result<DependencyReason> parse_dependency_reason(std::string_view token) {
  if (token == "structural-prerequisite") return DependencyReason::structural_prerequisite;
  if (token == "write-write-conflict") return DependencyReason::write_write_conflict;
  if (token == "write-read-conflict") return DependencyReason::write_read_conflict;
  if (token == "domain-serialization") return DependencyReason::domain_serialization;
  if (token == "contract-serialization") return DependencyReason::contract_serialization;
  if (token == "maintenance-ordering") return DependencyReason::maintenance_ordering;
  if (token == "invariant-ordering") return DependencyReason::invariant_ordering;
  if (token == "deterministic-tie-break") return DependencyReason::deterministic_tie_break;
  return Status::error(ErrorCode::malformed_input,
                       "unknown dependency reason token: " + std::string(token));
}

const char* to_string(LineageKind kind) noexcept {
  switch (kind) {
    case LineageKind::initial:
      return "initial";
    case LineageKind::replan:
      return "replan";
    case LineageKind::rollback:
      return "rollback";
  }
  return "unknown";
}

Result<LineageKind> parse_lineage_kind(std::string_view token) {
  if (token == "initial") return LineageKind::initial;
  if (token == "replan") return LineageKind::replan;
  if (token == "rollback") return LineageKind::rollback;
  return Status::error(ErrorCode::malformed_input,
                       "unknown lineage kind token: " + std::string(token));
}

const char* to_string(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::accepted:
      return "accepted";
    case RefusalCode::input_invalid:
      return "input-invalid";
    case RefusalCode::stale_input:
      return "stale-input";
    case RefusalCode::unsatisfiable_target:
      return "unsatisfiable-target";
    case RefusalCode::impossible_transition:
      return "impossible-transition";
    case RefusalCode::capability_missing:
      return "capability-missing";
    case RefusalCode::unsafe_initial_state:
      return "unsafe-initial-state";
    case RefusalCode::no_safe_ordering:
      return "no-safe-ordering";
    case RefusalCode::contradictory_constraints:
      return "contradictory-constraints";
    case RefusalCode::search_limit_exceeded:
      return "search-limit-exceeded";
    case RefusalCode::cancelled:
      return "cancelled";
    case RefusalCode::internal_verification_failed:
      return "internal-verification-failed";
  }
  return "unknown";
}

Result<RefusalCode> parse_refusal_code(std::string_view token) {
  if (token == "accepted") return RefusalCode::accepted;
  if (token == "input-invalid") return RefusalCode::input_invalid;
  if (token == "stale-input") return RefusalCode::stale_input;
  if (token == "unsatisfiable-target") return RefusalCode::unsatisfiable_target;
  if (token == "impossible-transition") return RefusalCode::impossible_transition;
  if (token == "capability-missing") return RefusalCode::capability_missing;
  if (token == "unsafe-initial-state") return RefusalCode::unsafe_initial_state;
  if (token == "no-safe-ordering") return RefusalCode::no_safe_ordering;
  if (token == "contradictory-constraints") return RefusalCode::contradictory_constraints;
  if (token == "search-limit-exceeded") return RefusalCode::search_limit_exceeded;
  if (token == "cancelled") return RefusalCode::cancelled;
  if (token == "internal-verification-failed") return RefusalCode::internal_verification_failed;
  return Status::error(ErrorCode::malformed_input,
                       "unknown refusal code token: " + std::string(token));
}

// ---------------------------------------------------------------------------
// Canonical codecs for plan documents
// ---------------------------------------------------------------------------

namespace {

void encode_invariant_list(CanonicalEncoder& out, const std::vector<InvariantId>& values) {
  out.put_u32(static_cast<std::uint32_t>(values.size()));
  for (const InvariantId value : values) {
    out.put_u8(static_cast<std::uint8_t>(value));
  }
}

std::vector<InvariantId> decode_invariant_list(CanonicalDecoder& in) {
  std::vector<InvariantId> values;
  const std::uint32_t count = in.get_count(1);
  if (!in.ok()) {
    return values;
  }
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint8_t raw = in.get_u8();
    if (!in.ok()) {
      return {};
    }
    if (raw > static_cast<std::uint8_t>(InvariantId::service_continuity)) {
      in.fail(ErrorCode::malformed_input, "invariant id out of range during decode");
      return {};
    }
    values.push_back(static_cast<InvariantId>(raw));
  }
  return values;
}

void encode_entity_ref_list(CanonicalEncoder& out, const std::vector<EntityRef>& values) {
  out.put_u32(static_cast<std::uint32_t>(values.size()));
  for (const EntityRef& value : values) {
    encode_entity_ref(out, value);
  }
}

std::vector<EntityRef> decode_entity_ref_list(CanonicalDecoder& in) {
  std::vector<EntityRef> values;
  const std::uint32_t count = in.get_count(6);
  if (!in.ok()) {
    return values;
  }
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values.push_back(decode_entity_ref(in));
    if (!in.ok()) {
      return {};
    }
  }
  return values;
}

void encode_string_list(CanonicalEncoder& out, const std::vector<std::string>& values) {
  out.put_u32(static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) {
    out.put_text(value);
  }
}

std::vector<std::string> decode_string_list(CanonicalDecoder& in) {
  std::vector<std::string> values;
  const std::uint32_t count = in.get_count(4);
  if (!in.ok()) {
    return values;
  }
  values.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    values.push_back(in.get_text());
    if (!in.ok()) {
      return {};
    }
  }
  return values;
}

std::uint8_t decode_byte_enum(CanonicalDecoder& in, std::uint8_t max_value, const char* name) {
  const std::uint8_t raw = in.get_u8();
  if (!in.ok()) {
    return 0;
  }
  if (raw > max_value) {
    in.fail(ErrorCode::malformed_input, std::string("enum out of range during decode: ") + name);
    return 0;
  }
  return raw;
}

}  // namespace

void Justification::encode(CanonicalEncoder& out) const {
  out.put_tag("justification");
  encode_value(out, governing_policy);
  encode_value(out, governing_policy_digest);
  encode_value(out, authority_generation);
  encode_invariant_list(out, checked_invariants);
  encode_entity_ref_list(out, evidence);
  encode_value(out, rationale_code);
  encode_string_list(out, notes);
}

Justification Justification::decode(CanonicalDecoder& in) {
  Justification value;
  in.get_tag("justification");
  value.governing_policy = decode_value<PolicyId>(in);
  value.governing_policy_digest = decode_value<Digest>(in);
  value.authority_generation = decode_value<AuthorityGeneration>(in);
  value.checked_invariants = decode_invariant_list(in);
  value.evidence = decode_entity_ref_list(in);
  value.rationale_code = in.get_text();
  value.notes = decode_string_list(in);
  return value;
}

void Compensation::encode(CanonicalEncoder& out) const {
  out.put_tag("compensation");
  out.put_u8(static_cast<std::uint8_t>(kind));
  encode_value(out, available);
  encode_operation(out, inverse);
  encode_value(out, note_code);
}

Compensation Compensation::decode(CanonicalDecoder& in) {
  Compensation value;
  in.get_tag("compensation");
  value.kind = static_cast<CompensationKind>(
      decode_byte_enum(in, static_cast<std::uint8_t>(CompensationKind::manual), "compensation-kind"));
  value.available = in.get_bool();
  value.inverse = decode_operation(in);
  value.note_code = in.get_text();
  return value;
}

void DependencyEdge::encode(CanonicalEncoder& out) const {
  out.put_tag("dependency-edge");
  encode_value(out, from);
  encode_value(out, to);
  out.put_u8(static_cast<std::uint8_t>(reason));
  encode_value(out, detail_code);
}

DependencyEdge DependencyEdge::decode(CanonicalDecoder& in) {
  DependencyEdge value;
  in.get_tag("dependency-edge");
  value.from = decode_value<StepId>(in);
  value.to = decode_value<StepId>(in);
  value.reason = static_cast<DependencyReason>(decode_byte_enum(
      in, static_cast<std::uint8_t>(DependencyReason::deterministic_tie_break),
      "dependency-reason"));
  value.detail_code = in.get_text();
  return value;
}

void Step::encode(CanonicalEncoder& out) const {
  out.put_tag("step");
  encode_value(out, id);
  encode_value(out, stage);
  encode_operation(out, operation);
  encode_list(out, preconditions, encode_condition);
  encode_list(out, postconditions, encode_condition);
  compensation.encode(out);
  encode_value(out, risk);
  encode_value(out, estimated_duration);
  encode_sequence(out, windows);
  justification.encode(out);
}

Step Step::decode(CanonicalDecoder& in) {
  Step value;
  in.get_tag("step");
  value.id = decode_value<StepId>(in);
  value.stage = decode_value<StageIndex>(in);
  value.operation = decode_operation(in);
  value.preconditions = decode_list<Condition>(in, 8, decode_condition);
  value.postconditions = decode_list<Condition>(in, 8, decode_condition);
  value.compensation = Compensation::decode(in);
  value.risk = decode_value<RiskScore>(in);
  value.estimated_duration = decode_value<DurationNs>(in);
  value.windows = decode_sequence<WindowId>(in, 4);
  value.justification = Justification::decode(in);
  return value;
}

void Stage::encode(CanonicalEncoder& out) const {
  out.put_tag("stage");
  encode_value(out, index);
  encode_sequence(out, steps);
  encode_value(out, rationale_code);
  encode_value(out, verified_permutations);
  encode_value(out, verified_applications);
}

Stage Stage::decode(CanonicalDecoder& in) {
  Stage value;
  in.get_tag("stage");
  value.index = decode_value<StageIndex>(in);
  value.steps = decode_sequence<StepId>(in, 4);
  value.rationale_code = in.get_text();
  value.verified_permutations = in.get_u64();
  value.verified_applications = in.get_u64();
  return value;
}

void EntityFingerprint::encode(CanonicalEncoder& out) const {
  out.put_tag("entity-fingerprint");
  encode_entity_ref(out, reference);
  encode_value(out, content);
}

EntityFingerprint EntityFingerprint::decode(CanonicalDecoder& in) {
  EntityFingerprint value;
  in.get_tag("entity-fingerprint");
  value.reference = decode_entity_ref(in);
  value.content = decode_value<Digest>(in);
  return value;
}

const EntityFingerprint* TopologySummary::find(const EntityRef& reference) const {
  for (const EntityFingerprint& entry : entities) {
    if (entry.reference == reference) {
      return &entry;
    }
  }
  return nullptr;
}

void TopologySummary::encode(CanonicalEncoder& out) const {
  out.put_tag("topology-summary");
  encode_list(out, entities, [](CanonicalEncoder& encoder, const EntityFingerprint& value) {
    value.encode(encoder);
  });
  encode_value(out, total_device_capacity);
  encode_value(out, total_link_capacity);
  encode_value(out, total_active_demand);
}

TopologySummary TopologySummary::decode(CanonicalDecoder& in) {
  TopologySummary value;
  in.get_tag("topology-summary");
  value.entities = decode_list<EntityFingerprint>(in, 40, [](CanonicalDecoder& decoder) {
    return EntityFingerprint::decode(decoder);
  });
  value.total_device_capacity = in.get_u64();
  value.total_link_capacity = in.get_u64();
  value.total_active_demand = in.get_u64();
  return value;
}

void ValidityBinding::encode(CanonicalEncoder& out) const {
  out.put_tag("validity-binding");
  encode_value(out, topology_generation);
  encode_value(out, authority);
  encode_value(out, authority_generation);
  encode_value(out, boot_incarnation);
  encode_value(out, epoch);
  encode_value(out, capability_version);
  encode_value(out, target_revision);
  encode_value(out, topology_digest);
  encode_value(out, target_digest);
  encode_value(out, constraint_digest);
  encode_value(out, evidence_digest);
  topology.encode(out);
}

ValidityBinding ValidityBinding::decode(CanonicalDecoder& in) {
  ValidityBinding value;
  in.get_tag("validity-binding");
  value.topology_generation = decode_value<TopologyGeneration>(in);
  value.authority = decode_value<AuthorityId>(in);
  value.authority_generation = decode_value<AuthorityGeneration>(in);
  value.boot_incarnation = decode_value<BootIncarnation>(in);
  value.epoch = decode_value<Epoch>(in);
  value.capability_version = decode_value<CapabilityVersion>(in);
  value.target_revision = decode_value<TargetRevision>(in);
  value.topology_digest = decode_value<Digest>(in);
  value.target_digest = decode_value<Digest>(in);
  value.constraint_digest = decode_value<Digest>(in);
  value.evidence_digest = decode_value<Digest>(in);
  value.topology = TopologySummary::decode(in);
  return value;
}

bool operator<(const ObjectiveVector& lhs, const ObjectiveVector& rhs) {
  if (!(lhs.stages == rhs.stages)) return lhs.stages < rhs.stages;
  if (!(lhs.risk_exposure == rhs.risk_exposure)) return lhs.risk_exposure < rhs.risk_exposure;
  if (lhs.churn != rhs.churn) return lhs.churn < rhs.churn;
  if (!(lhs.maintenance_duration == rhs.maintenance_duration)) {
    return lhs.maintenance_duration < rhs.maintenance_duration;
  }
  return lhs.tie_break < rhs.tie_break;
}

void ObjectiveVector::encode(CanonicalEncoder& out) const {
  out.put_tag("objective-vector");
  encode_value(out, stages);
  encode_value(out, risk_exposure);
  encode_value(out, churn);
  encode_value(out, maintenance_duration);
  encode_value(out, tie_break);
}

ObjectiveVector ObjectiveVector::decode(CanonicalDecoder& in) {
  ObjectiveVector value;
  in.get_tag("objective-vector");
  value.stages = decode_value<StageIndex>(in);
  value.risk_exposure = decode_value<RiskScore>(in);
  value.churn = in.get_u64();
  value.maintenance_duration = decode_value<DurationNs>(in);
  value.tie_break = decode_value<Digest>(in);
  return value;
}

void PlanLineage::encode(CanonicalEncoder& out) const {
  out.put_tag("plan-lineage");
  out.put_u8(static_cast<std::uint8_t>(kind));
  encode_value(out, parent_plan);
  encode_value(out, parent_generation);
  encode_value(out, observed_attempt);
  encode_value(out, observed_execution_digest);
}

PlanLineage PlanLineage::decode(CanonicalDecoder& in) {
  PlanLineage value;
  in.get_tag("plan-lineage");
  value.kind = static_cast<LineageKind>(
      decode_byte_enum(in, static_cast<std::uint8_t>(LineageKind::rollback), "lineage-kind"));
  value.parent_plan = decode_value<PlanId>(in);
  value.parent_generation = decode_value<PlanGeneration>(in);
  value.observed_attempt = decode_value<AttemptNumber>(in);
  value.observed_execution_digest = decode_value<Digest>(in);
  return value;
}

void RejectedAlternative::encode(CanonicalEncoder& out) const {
  out.put_tag("rejected-alternative");
  encode_value(out, code);
  encode_value(out, detail);
  out.put_u8(static_cast<std::uint8_t>(violated));
  encode_entity_ref_list(out, evidence);
}

RejectedAlternative RejectedAlternative::decode(CanonicalDecoder& in) {
  RejectedAlternative value;
  in.get_tag("rejected-alternative");
  value.code = in.get_text();
  value.detail = in.get_text();
  value.violated = static_cast<InvariantId>(decode_byte_enum(
      in, static_cast<std::uint8_t>(InvariantId::service_continuity), "invariant-id"));
  value.evidence = decode_entity_ref_list(in);
  return value;
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

const Step* Plan::find_step(const StepId& step_id) const {
  for (const Step& step : steps) {
    if (step.id == step_id) {
      return &step;
    }
  }
  return nullptr;
}

const Stage* Plan::find_stage(StageIndex index) const {
  for (const Stage& stage : stages) {
    if (stage.index == index) {
      return &stage;
    }
  }
  return nullptr;
}

std::vector<StepId> Plan::steps_of_stage(StageIndex index) const {
  const Stage* stage = find_stage(index);
  return stage == nullptr ? std::vector<StepId>{} : stage->steps;
}

std::vector<DependencyEdge> Plan::incoming_edges(const StepId& step_id) const {
  std::vector<DependencyEdge> result;
  for (const DependencyEdge& edge : edges) {
    if (edge.to == step_id) {
      result.push_back(edge);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<DependencyEdge> Plan::outgoing_edges(const StepId& step_id) const {
  std::vector<DependencyEdge> result;
  for (const DependencyEdge& edge : edges) {
    if (edge.from == step_id) {
      result.push_back(edge);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

Digest Plan::content_digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

bool Plan::semantically_identical(const Plan& other) const {
  return content_digest() == other.content_digest();
}

Result<void> Plan::verify_structure() const {
  if (id.empty()) {
    return Status::error(ErrorCode::invalid_argument, "plan identity is empty");
  }
  if (binding.authority.empty()) {
    return Status::error(ErrorCode::invalid_argument, "plan validity binding has no authority");
  }
  if (binding.topology_digest.is_zero() || binding.target_digest.is_zero() ||
      binding.constraint_digest.is_zero() || binding.evidence_digest.is_zero()) {
    return Status::error(ErrorCode::invalid_argument,
                         "plan validity binding contains a zero digest");
  }
  if (steps.empty() != stages.empty()) {
    return Status::error(ErrorCode::invalid_argument,
                         "plan must either contain steps and stages or be an empty converged "
                         "plan");
  }
  if (steps.empty()) {
    // An already-converged plan carries no work; it still binds its inputs.
    return Status::success();
  }

  std::set<std::string> step_ids;
  for (const Step& step : steps) {
    if (step.id.empty()) {
      return Status::error(ErrorCode::invalid_argument, "plan contains a step with an empty id");
    }
    if (!step_ids.insert(step.id.str()).second) {
      return Status::error(ErrorCode::duplicate_identity,
                           "plan contains duplicate step id '" + step.id.str() + "'");
    }
    if (step.id != step_id_for(step.operation)) {
      return Status::error(ErrorCode::integrity_failure,
                           "step id '" + step.id.str() +
                               "' does not match the identity digest of its operation");
    }
    if (step.preconditions.empty()) {
      return Status::error(ErrorCode::invalid_argument, "step '" + step.id.str() +
                                                            "' declares no preconditions");
    }
    if (step.postconditions.empty()) {
      return Status::error(ErrorCode::invalid_argument, "step '" + step.id.str() +
                                                            "' declares no postconditions");
    }
    if (step.justification.checked_invariants.empty()) {
      return Status::error(ErrorCode::invalid_argument,
                           "step '" + step.id.str() + "' carries no verified invariants");
    }
  }

  std::set<std::string> staged;
  for (std::size_t position = 0; position < stages.size(); ++position) {
    const Stage& stage = stages[position];
    if (stage.index.value() != position) {
      return Status::error(ErrorCode::invalid_argument,
                           "stage indices must be contiguous starting at zero");
    }
    if (stage.steps.empty()) {
      return Status::error(ErrorCode::invalid_argument, "stage " + std::to_string(position) +
                                                            " contains no steps");
    }
    for (const StepId& step_id : stage.steps) {
      if (!staged.insert(step_id.str()).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "step '" + step_id.str() + "' appears in more than one stage");
      }
      const Step* step = find_step(step_id);
      if (step == nullptr) {
        return Status::error(ErrorCode::not_found,
                             "stage references unknown step '" + step_id.str() + "'");
      }
      if (!(step->stage == stage.index)) {
        return Status::error(ErrorCode::integrity_failure,
                             "step '" + step_id.str() +
                                 "' carries a stage index that disagrees with its stage");
      }
    }
  }
  if (staged.size() != step_ids.size()) {
    return Status::error(ErrorCode::integrity_failure,
                         "the union of plan stages does not cover every step exactly once");
  }

  for (const DependencyEdge& edge : edges) {
    if (edge.from == edge.to) {
      return Status::error(ErrorCode::invalid_argument,
                           "dependency edge references step '" + edge.from.str() +
                               "' as its own predecessor");
    }
    const Step* from = find_step(edge.from);
    const Step* to = find_step(edge.to);
    if (from == nullptr || to == nullptr) {
      return Status::error(ErrorCode::not_found, "dependency edge references an unknown step");
    }
    if (!(from->stage < to->stage)) {
      return Status::error(ErrorCode::integrity_failure,
                           "dependency edge from '" + edge.from.str() + "' to '" + edge.to.str() +
                               "' does not respect stage ordering");
    }
  }

  // Dependency graph must be acyclic (stage ordering above already forbids
  // cycles, but the explicit check keeps the invariant independent of stage
  // assignment).
  std::map<std::string, std::vector<std::string>> successors;
  std::map<std::string, std::size_t> indegree;
  for (const Step& step : steps) {
    indegree[step.id.str()] = 0;
  }
  for (const DependencyEdge& edge : edges) {
    successors[edge.from.str()].push_back(edge.to.str());
    ++indegree[edge.to.str()];
  }
  std::vector<std::string> ready;
  for (const auto& entry : indegree) {
    if (entry.second == 0) {
      ready.push_back(entry.first);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const std::string current = ready.back();
    ready.pop_back();
    ++visited;
    for (const std::string& next : successors[current]) {
      if (--indegree[next] == 0) {
        ready.push_back(next);
      }
    }
  }
  if (visited != steps.size()) {
    return Status::error(ErrorCode::integrity_failure, "plan dependency graph contains a cycle");
  }

  return Status::success();
}

void Plan::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.plan.v1");
  encode_value(out, id);
  encode_value(out, generation);
  lineage.encode(out);
  binding.encode(out);
  encode_value(out, planning_instant);
  encode_value(out, request_digest);
  encode_list(out, stages, [](CanonicalEncoder& encoder, const Stage& value) {
    value.encode(encoder);
  });
  encode_list(out, steps, [](CanonicalEncoder& encoder, const Step& value) {
    value.encode(encoder);
  });
  encode_list(out, edges, [](CanonicalEncoder& encoder, const DependencyEdge& value) {
    value.encode(encoder);
  });
  objective.encode(out);
  encode_invariant_list(out, verified_invariants);
  encode_value(out, verification_digest);
  encode_value(out, verified_permutations);
  encode_list(out, rejected, [](CanonicalEncoder& encoder, const RejectedAlternative& value) {
    value.encode(encoder);
  });
  justification.encode(out);
  encode_value(out, label);
}

Plan Plan::decode(CanonicalDecoder& in) {
  Plan value;
  in.get_tag("cplan.plan.v1");
  value.id = decode_value<PlanId>(in);
  value.generation = decode_value<PlanGeneration>(in);
  value.lineage = PlanLineage::decode(in);
  value.binding = ValidityBinding::decode(in);
  value.planning_instant = decode_value<TimestampNs>(in);
  value.request_digest = decode_value<Digest>(in);
  value.stages = decode_list<Stage>(in, 16, [](CanonicalDecoder& decoder) {
    return Stage::decode(decoder);
  });
  value.steps = decode_list<Step>(in, 16, [](CanonicalDecoder& decoder) {
    return Step::decode(decoder);
  });
  value.edges = decode_list<DependencyEdge>(in, 8, [](CanonicalDecoder& decoder) {
    return DependencyEdge::decode(decoder);
  });
  value.objective = ObjectiveVector::decode(in);
  value.verified_invariants = decode_invariant_list(in);
  value.verification_digest = decode_value<Digest>(in);
  value.verified_permutations = in.get_u64();
  value.rejected = decode_list<RejectedAlternative>(in, 8, [](CanonicalDecoder& decoder) {
    return RejectedAlternative::decode(decoder);
  });
  value.justification = Justification::decode(in);
  value.label = in.get_text();
  return value;
}

}  // namespace cplan
