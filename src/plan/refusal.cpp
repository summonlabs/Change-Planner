#include "change_planner/plan/refusal.hpp"

#include <algorithm>

namespace cplan {

void BlockingConstraint::encode(CanonicalEncoder& out) const {
  out.put_tag("blocking-constraint");
  out.put_u8(static_cast<std::uint8_t>(kind));
  encode_value(out, id);
  out.put_u8(static_cast<std::uint8_t>(invariant));
  encode_value(out, message);
  encode_value(out, justification_code);
  out.put_u32(static_cast<std::uint32_t>(evidence.size()));
  for (const EntityRef& reference : evidence) {
    encode_entity_ref(out, reference);
  }
}

BlockingConstraint BlockingConstraint::decode(CanonicalDecoder& in) {
  BlockingConstraint value;
  in.get_tag("blocking-constraint");
  const std::uint8_t raw_kind = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_kind > static_cast<std::uint8_t>(ConstraintKind::generation_binding)) {
    in.fail(ErrorCode::malformed_input, "constraint kind out of range during decode");
    return value;
  }
  value.kind = static_cast<ConstraintKind>(raw_kind);
  value.id = decode_value<ConstraintId>(in);
  const std::uint8_t raw_invariant = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_invariant > static_cast<std::uint8_t>(InvariantId::service_continuity)) {
    in.fail(ErrorCode::malformed_input, "invariant id out of range during decode");
    return value;
  }
  value.invariant = static_cast<InvariantId>(raw_invariant);
  value.message = in.get_text();
  value.justification_code = in.get_text();
  const std::uint32_t count = in.get_count(6);
  if (!in.ok()) {
    return value;
  }
  value.evidence.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    value.evidence.push_back(decode_entity_ref(in));
    if (!in.ok()) {
      return {};
    }
  }
  return value;
}

void BlockedStep::encode(CanonicalEncoder& out) const {
  out.put_tag("blocked-step");
  encode_value(out, id);
  encode_operation(out, operation);
  out.put_u32(static_cast<std::uint32_t>(violations.size()));
  for (const Violation& violation : violations) {
    out.put_u8(static_cast<std::uint8_t>(violation.invariant));
    out.put_u8(static_cast<std::uint8_t>(violation.severity));
    out.put_text(violation.detail_code);
    out.put_text(violation.message);
    out.put_u32(static_cast<std::uint32_t>(violation.evidence.size()));
    for (const EntityRef& reference : violation.evidence) {
      encode_entity_ref(out, reference);
    }
  }
  encode_list(out, unsatisfied_preconditions, encode_condition);
  encode_value(out, blocking_code);
}

namespace {

std::vector<Violation> decode_violations(CanonicalDecoder& in) {
  std::vector<Violation> violations;
  const std::uint32_t count = in.get_count(6);
  if (!in.ok()) {
    return violations;
  }
  violations.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Violation violation;
    const std::uint8_t raw_invariant = in.get_u8();
    const std::uint8_t raw_severity = in.get_u8();
    if (!in.ok()) {
      return {};
    }
    if (raw_invariant > static_cast<std::uint8_t>(InvariantId::service_continuity) ||
        raw_severity > static_cast<std::uint8_t>(Severity::note)) {
      in.fail(ErrorCode::malformed_input, "violation enum out of range during decode");
      return {};
    }
    violation.invariant = static_cast<InvariantId>(raw_invariant);
    violation.severity = static_cast<Severity>(raw_severity);
    violation.detail_code = in.get_text();
    violation.message = in.get_text();
    const std::uint32_t evidence_count = in.get_count(6);
    if (!in.ok()) {
      return {};
    }
    violation.evidence.reserve(evidence_count);
    for (std::uint32_t k = 0; k < evidence_count; ++k) {
      violation.evidence.push_back(decode_entity_ref(in));
      if (!in.ok()) {
        return {};
      }
    }
    violations.push_back(std::move(violation));
  }
  return violations;
}

}  // namespace

BlockedStep BlockedStep::decode(CanonicalDecoder& in) {
  BlockedStep value;
  in.get_tag("blocked-step");
  value.id = decode_value<StepId>(in);
  value.operation = decode_operation(in);
  value.violations = decode_violations(in);
  value.unsatisfied_preconditions = decode_list<Condition>(in, 8, decode_condition);
  value.blocking_code = in.get_text();
  return value;
}

std::string Refusal::explain() const {
  std::string out = "refused (";
  out += cplan::to_string(code);
  out += "): ";
  out += summary_code.empty() ? "no safe plan exists" : summary_code;
  if (!blocking.empty()) {
    out += "\nblocking obligations:";
    for (const BlockingConstraint& entry : blocking) {
      out += "\n  - ";
      out += std::string(cplan::to_string(entry.kind));
      out += " [";
      out += entry.id.str();
      out += "]: ";
      out += entry.message;
      if (!entry.evidence.empty()) {
        out += " (evidence:";
        for (const EntityRef& reference : entry.evidence) {
          out += " ";
          out += reference.to_string();
        }
        out += ")";
      }
    }
  }
  if (!residual_violations.empty()) {
    out += "\nresidual violations:";
    for (const Violation& violation : residual_violations) {
      out += "\n  - ";
      out += violation.to_string();
    }
  }
  if (!blocked_steps.empty()) {
    out += "\nblocked steps:";
    for (const BlockedStep& step : blocked_steps) {
      out += "\n  - ";
      out += step.id.str();
      out += " (";
      out += step.operation.to_string();
      out += ")";
      if (!step.blocking_code.empty()) {
        out += " blocked by ";
        out += step.blocking_code;
      }
      for (const Condition& condition : step.unsatisfied_preconditions) {
        out += "\n      unsatisfied precondition: ";
        out += condition.to_string();
      }
      for (const Violation& violation : step.violations) {
        out += "\n      ";
        out += violation.to_string();
      }
    }
  }
  if (!rejected.empty()) {
    out += "\nrejected alternatives:";
    for (const RejectedAlternative& alternative : rejected) {
      out += "\n  - ";
      out += alternative.code;
      out += ": ";
      out += alternative.detail;
    }
  }
  if (!justification.notes.empty()) {
    out += "\nnotes:";
    for (const std::string& note : justification.notes) {
      out += "\n  - ";
      out += note;
    }
  }
  return out;
}

void Refusal::encode(CanonicalEncoder& out) const {
  out.put_tag("cplan.refusal.v1");
  out.put_u8(static_cast<std::uint8_t>(code));
  encode_value(out, summary_code);
  encode_value(out, request_digest);
  binding.encode(out);
  out.put_u32(static_cast<std::uint32_t>(blocking.size()));
  for (const BlockingConstraint& entry : blocking) {
    entry.encode(out);
  }
  out.put_u32(static_cast<std::uint32_t>(residual_violations.size()));
  for (const Violation& violation : residual_violations) {
    out.put_u8(static_cast<std::uint8_t>(violation.invariant));
    out.put_u8(static_cast<std::uint8_t>(violation.severity));
    out.put_text(violation.detail_code);
    out.put_text(violation.message);
    out.put_u32(static_cast<std::uint32_t>(violation.evidence.size()));
    for (const EntityRef& reference : violation.evidence) {
      encode_entity_ref(out, reference);
    }
  }
  out.put_u32(static_cast<std::uint32_t>(blocked_steps.size()));
  for (const BlockedStep& step : blocked_steps) {
    step.encode(out);
  }
  out.put_u32(static_cast<std::uint32_t>(rejected.size()));
  for (const RejectedAlternative& alternative : rejected) {
    alternative.encode(out);
  }
  justification.encode(out);
}

Refusal Refusal::decode(CanonicalDecoder& in) {
  Refusal value;
  in.get_tag("cplan.refusal.v1");
  const std::uint8_t raw_code = in.get_u8();
  if (!in.ok()) {
    return value;
  }
  if (raw_code > static_cast<std::uint8_t>(RefusalCode::internal_verification_failed)) {
    in.fail(ErrorCode::malformed_input, "refusal code out of range during decode");
    return value;
  }
  value.code = static_cast<RefusalCode>(raw_code);
  value.summary_code = in.get_text();
  value.request_digest = decode_value<Digest>(in);
  value.binding = ValidityBinding::decode(in);
  value.blocking = decode_list<BlockingConstraint>(in, 8, [](CanonicalDecoder& decoder) {
    return BlockingConstraint::decode(decoder);
  });
  value.residual_violations = decode_violations(in);
  value.blocked_steps = decode_list<BlockedStep>(in, 8, [](CanonicalDecoder& decoder) {
    return BlockedStep::decode(decoder);
  });
  value.rejected = decode_list<RejectedAlternative>(in, 8, [](CanonicalDecoder& decoder) {
    return RejectedAlternative::decode(decoder);
  });
  value.justification = Justification::decode(in);
  return value;
}

PlanResult PlanResult::from_plan(Plan plan, PlanDiagnostics diagnostics) {
  PlanResult result;
  result.outcome = std::move(plan);
  result.diagnostics = diagnostics;
  return result;
}

PlanResult PlanResult::from_refusal(Refusal refusal, PlanDiagnostics diagnostics) {
  PlanResult result;
  result.outcome = std::move(refusal);
  result.diagnostics = diagnostics;
  return result;
}

}  // namespace cplan
