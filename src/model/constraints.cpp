#include "change_planner/model/constraints.hpp"

#include <algorithm>
#include <set>

namespace cplan {

const char* to_string(ConstraintKind kind) noexcept {
  switch (kind) {
    case ConstraintKind::connectivity:
      return "connectivity";
    case ConstraintKind::failure_domain_redundancy:
      return "failure-domain-redundancy";
    case ConstraintKind::path_diversity:
      return "path-diversity";
    case ConstraintKind::capacity_headroom:
      return "capacity-headroom";
    case ConstraintKind::maintenance_exclusion:
      return "maintenance-exclusion";
    case ConstraintKind::maintenance_window:
      return "maintenance-window";
    case ConstraintKind::drain_before_maintenance:
      return "drain-before-maintenance";
    case ConstraintKind::change_serialization:
      return "change-serialization";
    case ConstraintKind::workload_contract:
      return "workload-contract";
    case ConstraintKind::capability_evidence:
      return "capability-evidence";
    case ConstraintKind::rollback_metadata:
      return "rollback-metadata";
    case ConstraintKind::generation_binding:
      return "generation-binding";
  }
  return "unknown";
}

Result<ConstraintKind> parse_constraint_kind(std::string_view token) {
  if (token == "connectivity") return ConstraintKind::connectivity;
  if (token == "failure-domain-redundancy") return ConstraintKind::failure_domain_redundancy;
  if (token == "path-diversity") return ConstraintKind::path_diversity;
  if (token == "capacity-headroom") return ConstraintKind::capacity_headroom;
  if (token == "maintenance-exclusion") return ConstraintKind::maintenance_exclusion;
  if (token == "maintenance-window") return ConstraintKind::maintenance_window;
  if (token == "drain-before-maintenance") return ConstraintKind::drain_before_maintenance;
  if (token == "change-serialization") return ConstraintKind::change_serialization;
  if (token == "workload-contract") return ConstraintKind::workload_contract;
  if (token == "capability-evidence") return ConstraintKind::capability_evidence;
  if (token == "rollback-metadata") return ConstraintKind::rollback_metadata;
  if (token == "generation-binding") return ConstraintKind::generation_binding;
  return Status::error(ErrorCode::malformed_input,
                       "unknown constraint kind token: " + std::string(token));
}

Result<void> SafetyConstraints::validate() const {
  if (id.empty()) {
    return Status::error(ErrorCode::invalid_argument, "constraint set identity is empty");
  }
  if (governing_policy.empty()) {
    return Status::error(ErrorCode::invalid_argument,
                         "constraint set must name its governing policy");
  }
  std::set<std::string> seen;
  for (const ConstraintItem& item : items) {
    if (item.id.empty()) {
      return Status::error(ErrorCode::invalid_argument, "constraint item has an empty identity");
    }
    if (!seen.insert(item.id.str()).second) {
      return Status::error(ErrorCode::duplicate_identity,
                           "duplicate constraint item identity: " + item.id.str());
    }
    if (item.headroom.value() > 1000) {
      return Status::error(ErrorCode::invalid_argument,
                           "constraint item '" + item.id.str() +
                               "' declares headroom above 1000 permille");
    }
  }
  std::set<std::string> contract_ids;
  for (const NetworkContract& contract : contracts) {
    if (contract.id.empty()) {
      return Status::error(ErrorCode::invalid_argument, "contract has an empty identity");
    }
    if (!contract_ids.insert(contract.id.str()).second) {
      return Status::error(ErrorCode::duplicate_identity,
                           "duplicate contract identity: " + contract.id.str());
    }
    std::set<std::string> workloads;
    for (const WorkloadId& workload : contract.workloads) {
      if (!workloads.insert(workload.str()).second) {
        return Status::error(ErrorCode::duplicate_identity,
                             "contract '" + contract.id.str() + "' repeats workload '" +
                                 workload.str() + "'");
      }
    }
  }
  return Status::success();
}

Digest SafetyConstraints::digest() const {
  CanonicalEncoder encoder;
  encode(encoder);
  return encoder.digest();
}

bool SafetyConstraints::enforced(ConstraintKind kind) const {
  for (const ConstraintItem& item : items) {
    if (item.kind == kind && item.enabled) {
      return true;
    }
  }
  return false;
}

const ConstraintItem* SafetyConstraints::find(ConstraintKind kind) const {
  for (const ConstraintItem& item : items) {
    if (item.kind == kind && item.enabled) {
      return &item;
    }
  }
  return nullptr;
}

std::vector<const ConstraintItem*> SafetyConstraints::all(ConstraintKind kind) const {
  std::vector<const ConstraintItem*> result;
  for (const ConstraintItem& item : items) {
    if (item.kind == kind && item.enabled) {
      result.push_back(&item);
    }
  }
  return result;
}

void SafetyConstraints::canonicalize() {
  std::sort(items.begin(), items.end());
  std::sort(contracts.begin(), contracts.end(),
            [](const NetworkContract& lhs, const NetworkContract& rhs) { return lhs.id < rhs.id; });
  for (NetworkContract& contract : contracts) {
    std::sort(contract.workloads.begin(), contract.workloads.end());
    contract.workloads.erase(std::unique(contract.workloads.begin(), contract.workloads.end()),
                             contract.workloads.end());
  }
  std::sort(tolerance.exempt_identities.begin(), tolerance.exempt_identities.end());
  tolerance.exempt_identities.erase(
      std::unique(tolerance.exempt_identities.begin(), tolerance.exempt_identities.end()),
      tolerance.exempt_identities.end());
}

namespace {

void encode_constraint_item(CanonicalEncoder& out, const ConstraintItem& value) {
  out.put_tag("constraint-item");
  encode_value(out, value.id);
  encode_value(out, static_cast<std::uint8_t>(value.kind));
  encode_value(out, value.enabled);
  encode_value(out, value.headroom);
  encode_value(out, value.max_concurrent_changes);
  encode_value(out, value.max_domains_in_maintenance);
  encode_value(out, value.parameter_code);
  encode_value(out, value.justification_code);
}

ConstraintItem decode_constraint_item(CanonicalDecoder& in) {
  ConstraintItem value;
  in.get_tag("constraint-item");
  value.id = decode_value<ConstraintId>(in);
  const std::uint8_t raw_kind = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_kind > static_cast<std::uint8_t>(ConstraintKind::generation_binding)) {
    in.fail(ErrorCode::malformed_input, "constraint kind out of range during decode");
    return value;
  }
  value.kind = static_cast<ConstraintKind>(raw_kind);
  value.enabled = in.get_bool();
  value.headroom = decode_value<Permille>(in);
  value.max_concurrent_changes = in.get_u32();
  value.max_domains_in_maintenance = in.get_u32();
  value.parameter_code = in.get_text();
  value.justification_code = in.get_text();
  return value;
}

void encode_contract(CanonicalEncoder& out, const NetworkContract& value) {
  out.put_tag("network-contract");
  encode_value(out, value.id);
  encode_sequence(out, value.workloads);
  encode_value(out, value.required_units);
  encode_value(out, value.min_disjoint_domains);
  encode_value(out, value.max_simultaneous_domain_maintenance);
  encode_value(out, value.forbid_concurrent_changes_in_same_domain);
  encode_value(out, value.sla_code);
}

NetworkContract decode_contract(CanonicalDecoder& in) {
  NetworkContract value;
  in.get_tag("network-contract");
  value.id = decode_value<ContractId>(in);
  value.workloads = decode_sequence<WorkloadId>(in, 4);
  value.required_units = decode_value<CapacityUnits>(in);
  value.min_disjoint_domains = in.get_u32();
  value.max_simultaneous_domain_maintenance = in.get_u32();
  value.forbid_concurrent_changes_in_same_domain = in.get_bool();
  value.sla_code = in.get_text();
  return value;
}

}  // namespace

void SafetyConstraints::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.constraints.v1");
  encode_value(out, id);
  encode_value(out, authority_generation);
  encode_value(out, governing_policy);
  encode_value(out, governing_policy_digest);
  encode_list(out, items, encode_constraint_item);
  encode_list(out, contracts, encode_contract);
  encode_value(out, tolerance.topology_drift);
  encode_value(out, tolerance.capacity_drift);
  encode_value(out, tolerance.demand_drift);
  encode_value(out, tolerance.max_added_entities);
  encode_value(out, tolerance.max_removed_entities);
  encode_value(out, tolerance.allow_capability_growth);
  encode_value(out, tolerance.allow_capability_shrink);
  out.put_u32(static_cast<std::uint32_t>(tolerance.exempt_identities.size()));
  for (const std::string& identity : tolerance.exempt_identities) {
    encode_value(out, identity);
  }
}

SafetyConstraints SafetyConstraints::decode(CanonicalDecoder& in) {
  SafetyConstraints constraints;
  in.get_tag("cplan.constraints.v1");
  constraints.id = decode_value<ConstraintId>(in);
  constraints.authority_generation = decode_value<AuthorityGeneration>(in);
  constraints.governing_policy = decode_value<PolicyId>(in);
  constraints.governing_policy_digest = decode_value<Digest>(in);
  constraints.items = decode_list<ConstraintItem>(in, 8, decode_constraint_item);
  constraints.contracts = decode_list<NetworkContract>(in, 8, decode_contract);
  constraints.tolerance.topology_drift = decode_value<Permille>(in);
  constraints.tolerance.capacity_drift = decode_value<Permille>(in);
  constraints.tolerance.demand_drift = decode_value<Permille>(in);
  constraints.tolerance.max_added_entities = in.get_u32();
  constraints.tolerance.max_removed_entities = in.get_u32();
  constraints.tolerance.allow_capability_growth = in.get_bool();
  constraints.tolerance.allow_capability_shrink = in.get_bool();
  const std::uint32_t count = in.get_count(4);
  if (!in.ok()) {
    return constraints;
  }
  constraints.tolerance.exempt_identities.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    constraints.tolerance.exempt_identities.push_back(in.get_text());
    if (!in.ok()) {
      return {};
    }
  }
  return constraints;
}

SafetyConstraints default_constraints(Permille headroom, std::uint32_t max_concurrent_changes,
                                      std::uint32_t max_domains_in_maintenance) {
  SafetyConstraints constraints;
  constraints.id = ConstraintId::from_validated("default-constraints");
  constraints.governing_policy = PolicyId::from_validated("default-policy");
  constraints.governing_policy_digest =
      Sha256::hash("cplan.default-policy.v1:no-vendor-specific-assumptions");

  static constexpr ConstraintKind kAllKinds[] = {
      ConstraintKind::connectivity,
      ConstraintKind::failure_domain_redundancy,
      ConstraintKind::path_diversity,
      ConstraintKind::capacity_headroom,
      ConstraintKind::maintenance_exclusion,
      ConstraintKind::maintenance_window,
      ConstraintKind::drain_before_maintenance,
      ConstraintKind::change_serialization,
      ConstraintKind::workload_contract,
      ConstraintKind::capability_evidence,
      ConstraintKind::rollback_metadata,
      ConstraintKind::generation_binding,
  };

  for (const ConstraintKind kind : kAllKinds) {
    ConstraintItem item;
    item.id = ConstraintId::from_validated(std::string("default-") + to_string(kind));
    item.kind = kind;
    item.enabled = true;
    item.headroom = headroom;
    item.max_concurrent_changes = max_concurrent_changes;
    item.max_domains_in_maintenance = max_domains_in_maintenance;
    item.justification_code = "default-policy";
    constraints.items.push_back(std::move(item));
  }
  return constraints;
}

}  // namespace cplan
