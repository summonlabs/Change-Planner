# Safety model

This document defines exactly what Change Planner proves before it emits a plan,
and what it refuses to do. It is the reference for `check_invariants`,
`derive_preconditions`, `derive_postconditions` and `verify_plan`.

## Model of a decision

Every decision is made under an evaluation context:

```text
EvaluationContext {
  instant      logical planning instant (never the wall clock)
  constraints  the governing SafetyConstraints (identity, digest, authority generation)
  evidence     capability evidence observed for the snapshot
  baseline     the snapshot the plan started from (null for standalone checks)
}
```

An obligation is enforced only when a constraint item of the corresponding kind is
enabled. Disabling an item removes that obligation from the proof, and is visible
in the plan binding: the constraint digest changes, so previously generated plans
are invalidated rather than silently reinterpreted.

## Invariants

| Invariant | Constraint kind | Definition, checked after every step |
|---|---|---|
| `connectivity` | `connectivity` | Every workload requiring connectivity has at least one active binding whose route is active, walkable, whose links and endpoint devices are present, and whose failure domains avoid the workload's forbidden domains. |
| `failure-domain-redundancy` | `failure-domain-redundancy` | For every failure domain, count the eligible paths that do not traverse it; the minimum over all domains must be at least the workload's `min_disjoint_domains`. This is single-domain survivability, not a domain census. |
| `path-diversity` | `path-diversity` | The maximum number of pairwise link-disjoint eligible paths must be at least `min_link_disjoint_paths`. The maximum is computed exactly under a bounded search; exhausting the budget produces a violation (`diversity-proof-budget-exhausted`) rather than an assumption. |
| `capacity-headroom` | `capacity-headroom` | For every link and device, active demand must not exceed `(capacity - reserved) * (1000 - headroom) / 1000`. A workload's `min_guaranteed_capacity` additionally requires that much headroom on each of its active paths. |
| `maintenance-exclusion` | `maintenance-exclusion` | No device in draining or maintenance state may sit inside one of its exclusion windows at the planning instant. |
| `maintenance-window` | `maintenance-window`, `drain-before-maintenance` | A device in maintenance must be inside one of its declared maintenance windows; maintenance-class operations require an open window and, under the drain policy, a prior drain. |
| `change-serialization` | `change-serialization` | Per failure domain, devices out of service must not exceed the declared concurrency budget, which is the minimum of the constraint item and the devices' own budgets. |
| `service-continuity` | `change-serialization` | The number of failure domains with any device out of service must not exceed `max_domains_in_maintenance`. |
| `workload-contract` | `workload-contract` | For every supplied contract: its workloads exist, their aggregate active demand meets `required_units`, their paths survive the loss of a single domain at least `min_disjoint_domains` times, at most `max_simultaneous_domain_maintenance` of their domains are in change, and - when declared - never two devices of one domain at once. |
| `capability-evidence` | `capability-evidence` | Every device of the plan's baseline snapshot has recorded capability evidence. Devices created by the plan are exempt (no evidence can exist yet) and are instead gated by their declared target capabilities. |
| `generation-binding` | `generation-binding` | The snapshot authority generation equals the governing constraint generation, and the evidence generation and boot incarnation equal the snapshot's. |

## Structural rules

Structural validity is checked by `CurrentStateSnapshot::validate` and by
`apply_operation` independently of any obligation: unique identities, existing
references, walkable route paths, positive window durations, capacity above the
reserved floor, and value ranges. A structurally invalid input is rejected with a
typed error (for example `duplicate_identity` or `invalid_argument`); it is never
coerced into something plausible.

Impossible transitions are distinguishable from unsafe ones:

* `not_found` - the operation targets something that is not there;
* `conflict` - the operation contradicts the current state (already present, still
  bound, already drained);
* `capacity_exceeded` - the change would violate a structural floor;
* `unsafe_transition` - the transition is structurally meaningless or forbidden by
  an ordering rule, for example removing a device before draining it under the
  drain-before-maintenance policy;
* `unsupported_capability` - the target asks for something the operation
  vocabulary cannot express.

## Preconditions and postconditions

Every step carries typed conditions derived from the state it will be applied to:

* device and link operations require the entity to be present (or absent, when
  creating), the expected state, and no active exclusion window;
* maintenance-class operations additionally require an open maintenance window,
  and drain-dependent operations require a drained device;
* route operations require their endpoints and path links, and creating a route or
  changing its path requires a walkable path;
* policy and config operations require their targets;
* postconditions assert the effect: presence or absence, state, capacity,
  walkability, bound policy, configuration value.

`verify_plan` requires every precondition to hold before a step is applied and
every postcondition to hold afterwards - in *every* interleaving - and fails the
verification otherwise. A plan that cannot be verified is never returned.

## Stage semantics

A stage is a set of steps the plan claims may run concurrently. The claim is
proved, not asserted:

1. every permutation of the stage is replayed from the stage entry state;
2. preconditions and postconditions are checked per step in that order;
3. the full invariant set is re-evaluated after every individual application;
4. every permutation must converge to the same final state, so the steps commute;
5. steps connected by a dependency edge are never placed in the same stage.

Exhaustive permutation replay is why stage width is bounded (default 4, maximum
5). A run that cannot afford a proof refuses; it never emits a stage it has not
proved.

## Fencing

Stale state is rejected wherever it could influence a decision:

| Signal | Where it is fenced |
|---|---|
| topology generation | request validation, generation binding, plan binding, invalidation, observation fencing |
| authority identity and generation | the same paths, plus constraint issuance |
| boot incarnation | evidence freshness, observation fencing, invalidation |
| epoch | request validation, observation fencing, invalidation |
| target revision | plan binding, invalidation |
| plan generation and attempt | observation fencing, repository publication, compensation lineage |
| artifact integrity | envelope digest, canonical-form check, structural validation on read |

A persisted plan is never treated as fresh because it deserialized: freshness is a
separate decision (`evaluate_validity`) against current inputs and the declared
tolerances.

## Conservatism

Wherever the checker cannot complete a proof it refuses rather than assumes:

* path-diversity search budget exhausted -> violation, and the plan is refused;
* scheduling or permutation budget exhausted -> `search_limit_exceeded`;
* an unknown condition parameter -> evaluation error, so verification fails;
* an ambiguous or malformed document -> `malformed_input`, never a default;
* an orphaned partial artifact -> reported by `scan`, never read as a plan.
