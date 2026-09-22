#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "change_planner/model/operation.hpp"
#include "change_planner/safety/invariant.hpp"

namespace cplan {

// ---------------------------------------------------------------------------
// Why a decision was made. Every step and every refusal carries the policy,
// authority generation, evidence and invariants it was decided under.
// ---------------------------------------------------------------------------
struct Justification {
  PolicyId governing_policy;
  Digest governing_policy_digest;
  AuthorityGeneration authority_generation;
  std::vector<InvariantId> checked_invariants;
  std::vector<EntityRef> evidence;
  std::string rationale_code;
  std::vector<std::string> notes;

  friend bool operator==(const Justification&, const Justification&) = default;

  void encode(CanonicalEncoder& out) const;
  static Justification decode(CanonicalDecoder& in);
};

// Compensation metadata lets the runtime undo or re-apply a step. A step whose
// compensation is unavailable says so explicitly instead of pretending.
enum class CompensationKind : std::uint8_t { inverse = 0, reapply, manual };

const char* to_string(CompensationKind kind) noexcept;
Result<CompensationKind> parse_compensation_kind(std::string_view token);

struct Compensation {
  CompensationKind kind{CompensationKind::manual};
  bool available{false};
  Operation inverse;
  std::string note_code;

  friend bool operator==(const Compensation&, const Compensation&) = default;

  void encode(CanonicalEncoder& out) const;
  static Compensation decode(CanonicalDecoder& in);
};

// Why step B must follow step A.
enum class DependencyReason : std::uint8_t {
  structural_prerequisite = 0,
  write_write_conflict,
  write_read_conflict,
  domain_serialization,
  contract_serialization,
  maintenance_ordering,
  invariant_ordering,
  deterministic_tie_break,
};

const char* to_string(DependencyReason reason) noexcept;
Result<DependencyReason> parse_dependency_reason(std::string_view token);

struct DependencyEdge {
  StepId from;
  StepId to;
  DependencyReason reason{DependencyReason::deterministic_tie_break};
  std::string detail_code;

  friend bool operator==(const DependencyEdge&, const DependencyEdge&) = default;
  friend bool operator<(const DependencyEdge& lhs, const DependencyEdge& rhs) {
    if (lhs.from != rhs.from) return lhs.from < rhs.from;
    if (lhs.to != rhs.to) return lhs.to < rhs.to;
    return static_cast<std::uint8_t>(lhs.reason) < static_cast<std::uint8_t>(rhs.reason);
  }

  void encode(CanonicalEncoder& out) const;
  static DependencyEdge decode(CanonicalDecoder& in);
};

struct Step {
  StepId id;
  StageIndex stage;
  Operation operation;
  std::vector<Condition> preconditions;
  std::vector<Condition> postconditions;
  Compensation compensation;
  RiskScore risk;
  DurationNs estimated_duration;
  std::vector<WindowId> windows;
  Justification justification;

  friend bool operator==(const Step&, const Step&) = default;

  void encode(CanonicalEncoder& out) const;
  static Step decode(CanonicalDecoder& in);
};

struct Stage {
  StageIndex index;
  std::vector<StepId> steps;
  std::string rationale_code;
  std::uint64_t verified_permutations{0};
  std::uint64_t verified_applications{0};

  friend bool operator==(const Stage&, const Stage&) = default;

  void encode(CanonicalEncoder& out) const;
  static Stage decode(CanonicalDecoder& in);
};

// A single entity with a content fingerprint, used to measure how far a plan's
// input state has drifted.
struct EntityFingerprint {
  EntityRef reference;
  Digest content;

  friend bool operator==(const EntityFingerprint&, const EntityFingerprint&) = default;
  friend bool operator<(const EntityFingerprint& lhs, const EntityFingerprint& rhs) {
    return lhs.reference < rhs.reference;
  }

  void encode(CanonicalEncoder& out) const;
  static EntityFingerprint decode(CanonicalDecoder& in);
};

// Compact, deterministic summary of the state a plan was computed from. It is
// what makes invalidation measurable without re-deriving the whole snapshot.
struct TopologySummary {
  std::vector<EntityFingerprint> entities;
  std::uint64_t total_device_capacity{0};
  std::uint64_t total_link_capacity{0};
  std::uint64_t total_active_demand{0};

  friend bool operator==(const TopologySummary&, const TopologySummary&) = default;

  [[nodiscard]] const EntityFingerprint* find(const EntityRef& reference) const;

  void encode(CanonicalEncoder& out) const;
  static TopologySummary decode(CanonicalDecoder& in);
};

[[nodiscard]] TopologySummary summarize(const CurrentStateSnapshot& state);

// Everything a plan is bound to. A plan is never valid against materially
// different state: these fields are compared by plan invalidation.
struct ValidityBinding {
  TopologyGeneration topology_generation;
  AuthorityId authority;
  AuthorityGeneration authority_generation;
  BootIncarnation boot_incarnation;
  Epoch epoch;
  CapabilityVersion capability_version;
  TargetRevision target_revision;
  Digest topology_digest;
  Digest target_digest;
  Digest constraint_digest;
  Digest evidence_digest;
  TopologySummary topology;

  friend bool operator==(const ValidityBinding&, const ValidityBinding&) = default;

  void encode(CanonicalEncoder& out) const;
  static ValidityBinding decode(CanonicalDecoder& in);
};

// Objective metrics in the order they are compared.
struct ObjectiveVector {
  StageIndex stages;
  RiskScore risk_exposure;
  std::uint64_t churn{0};
  DurationNs maintenance_duration;
  Digest tie_break;

  friend bool operator==(const ObjectiveVector&, const ObjectiveVector&) = default;
  friend bool operator<(const ObjectiveVector& lhs, const ObjectiveVector& rhs);

  void encode(CanonicalEncoder& out) const;
  static ObjectiveVector decode(CanonicalDecoder& in);
};

enum class LineageKind : std::uint8_t { initial = 0, replan, rollback };

const char* to_string(LineageKind kind) noexcept;
Result<LineageKind> parse_lineage_kind(std::string_view token);

struct PlanLineage {
  LineageKind kind{LineageKind::initial};
  PlanId parent_plan;
  PlanGeneration parent_generation;
  AttemptNumber observed_attempt;
  Digest observed_execution_digest;

  friend bool operator==(const PlanLineage&, const PlanLineage&) = default;

  void encode(CanonicalEncoder& out) const;
  static PlanLineage decode(CanonicalDecoder& in);
};

// A path that the search considered and rejected, with the reason.
struct RejectedAlternative {
  std::string code;
  std::string detail;
  InvariantId violated{InvariantId::connectivity};
  std::vector<EntityRef> evidence;

  friend bool operator==(const RejectedAlternative&, const RejectedAlternative&) = default;
  friend bool operator<(const RejectedAlternative& lhs, const RejectedAlternative& rhs) {
    if (lhs.code != rhs.code) return lhs.code < rhs.code;
    return lhs.detail < rhs.detail;
  }

  void encode(CanonicalEncoder& out) const;
  static RejectedAlternative decode(CanonicalDecoder& in);
};

struct Plan {
  PlanId id;
  PlanGeneration generation;
  PlanLineage lineage;
  ValidityBinding binding;
  TimestampNs planning_instant;
  Digest request_digest;
  std::vector<Stage> stages;
  std::vector<Step> steps;
  std::vector<DependencyEdge> edges;
  ObjectiveVector objective;
  std::vector<InvariantId> verified_invariants;
  Digest verification_digest;
  std::uint64_t verified_permutations{0};
  std::vector<RejectedAlternative> rejected;
  Justification justification;
  std::string label;

  friend bool operator==(const Plan&, const Plan&) = default;

  [[nodiscard]] const Step* find_step(const StepId& id) const;
  [[nodiscard]] const Stage* find_stage(StageIndex index) const;
  [[nodiscard]] std::vector<StepId> steps_of_stage(StageIndex index) const;
  // Steps that must run before the given step.
  [[nodiscard]] std::vector<DependencyEdge> incoming_edges(const StepId& id) const;
  [[nodiscard]] std::vector<DependencyEdge> outgoing_edges(const StepId& id) const;

  // Immutable content identity: canonical encoding of every plan field.
  [[nodiscard]] Digest content_digest() const;
  // Two plans are semantically identical when their content digests match.
  [[nodiscard]] bool semantically_identical(const Plan& other) const;

  // Structural self-consistency: unique step ids, stages covering every step
  // exactly once, dependency edges referencing known steps and forming a DAG.
  [[nodiscard]] Result<void> verify_structure() const;

  void encode(CanonicalEncoder& out) const;
  static Plan decode(CanonicalDecoder& in);
};

}  // namespace cplan
