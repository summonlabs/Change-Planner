# Architecture

How Change Planner turns a generation-bound snapshot and a declarative target
into a verified, ordered transition plan - or into a refusal that names the
obligation which makes a plan impossible.

## Layering

```text
core      identities, digests, canonical codec, strict JSON, cancellation   (no domain knowledge)
  |
model     state, target, constraints, evidence, operations, evaluation      (domain data + structural rules)
  |
safety    invariants and the safety checker                                 (declarative obligations)
  |
plan      delta, dependencies, scheduling, verification, refusal, replan     (decisions)
  |
persist   artifacts and repository                                          (durable form)
cli       documents and commands                                            (operator surface)
```

Dependencies point downward only. `core` and `model` never depend on `plan`,
`persist` or `cli`; the planner never depends on the CLI.

## Planning pipeline

1. **Request validation.** `PlanRequest::validate` runs the structural validators
   of the snapshot, target, constraints, evidence, objective and limits. Failure
   yields a refusal with code `input_invalid`.
2. **Generation binding.** Snapshot generation, authority identity, authority
   generation, boot incarnation, epoch, evidence generation/incarnation and (when
   enforced) the constraint authority generation must agree. Any mismatch yields
   a refusal with code `stale_input` and a summary naming the dimension.
3. **Maintenance-instant selection.** When `allow_window_shift` is set, the
   planner searches the earliest instant at or after the requested one that
   satisfies every touched device's maintenance and exclusion windows; if none
   exists it refuses with `no-permitted-maintenance-instant`.
4. **Delta derivation.** `derive_operations` compares the snapshot with the
   declarative target entry by entry and emits typed operations in canonical
   order. Entity changes that the vendor-neutral operation set cannot express
   (for example changing a device's failure domain in place) are reported as
   `unsatisfiable_target` rather than approximated.
5. **Capability gating.** Every operation's capability must be supported by the
   devices that would execute it (declared capability intersected with observed
   evidence). Devices created by the plan are gated by their declared target
   capabilities. A missing capability is `capability_missing`.
6. **Initial-state safety.** The snapshot itself must satisfy every enforced
   obligation. The planner refuses to plan from a state that is already unsafe
   (`unsafe_initial_state` plus the residual violations).
7. **Dependency derivation.** `derive_dependencies` builds the static conflict
   graph: structural prerequisites (drain before removal or maintenance, device
   before link, link before route, route before binding, unbinding before route
   removal), write/write and write/read conflicts, per-domain serialization of
   out-of-service transitions, and contract-forbidden concurrency.
8. **Scheduling search.** A bounded, deterministic depth-first search over step
   orderings applies one step at a time, checking preconditions, applying the
   operation and re-checking the whole invariant set. Ready steps are ordered by
   the candidate strategy; symmetry reduction skips a ready step that is provably
   independent of an already-tried candidate (they commute and neither reads what
   the other writes, so a solution starting with one can be rotated to start with
   the other).
9. **Stage compression.** The verified linear order is compressed into maximal
   stages. A step joins the current stage only if exhaustive permutation replay
   proves that every interleaving of the enlarged stage keeps all invariants,
   satisfies all preconditions and postconditions, and converges to the same
   final state. Steps joined by a dependency edge never share a stage; the width
   bound (`max_stage_width`) keeps the permutation proof finite.
10. **Candidate selection.** Several deterministic strategies (safety-first,
    unlock-first, maintenance-first, reverse-canonical) produce different
    orderings. Each candidate is measured - stages, peak per-stage risk, touched
    entities, maintenance duration - and compared under the request's objective
    priority list, with the plan content digest as the final tie-break.
11. **Independent verification.** The selected plan is replayed from the
    request's start state by `verify_plan`, which re-checks structure, binding,
    preconditions, postconditions and invariants under every interleaving of
    every stage and records a trace digest. A plan that fails verification is
    never returned: the planner refuses with `internal_verification_failed`.
12. **Plan assembly.** Steps carry preconditions, postconditions, compensation
    metadata, risk, duration, bound windows and a justification that names the
    governing policy, the authority generation and the invariant set that was
    checked. Stages record how many interleavings were verified.

## Determinism

* The planner never reads the wall clock: the planning instant is an input.
* Every collection is canonicalized (sorted, de-duplicated) before planning.
* Operations have a total order derived from kind, target identity and a content
  digest.
* Candidate strategies are enumerated in a fixed order; ties are broken by the
  plan's canonical content digest.
* Digests are SHA-256 over a canonical binary encoding with explicit widths,
  length prefixes and domain-separation tags.

## Refusals

A refusal is a first-class result, not an error. It carries the code, the binding
it was decided under, residual violations, the blocked steps with the
preconditions or invariants that stopped them, the alternatives the search
rejected, and - for ordering failures - a *minimal* blocking set of declared
obligations.

The blocking set is computed by iteratively disabling enabled constraint items
and re-running a single-strategy planning attempt until planning becomes
possible, then re-enabling every item that is not needed. The result is
irreducible: removing any single reported obligation no longer unblocks the plan.
If no single obligation unblocks it, the refusal says so explicitly and reports
the structural cause instead.

## Replanning

`replan` fences the observation against the plan it claims to describe (plan
identity, plan generation, attempt, authority generation, topology generation,
boot incarnation), then builds a request whose start state is the *observed*
state with the target intent and evidence rebased onto the observed generation
(the rebase is recorded in the plan notes). The continuation is derived from that
state, so a step whose effect is already visible is not re-issued, and a step
that was never observed is. Steps observed as partially applied are compensated
by a pre-stage built from the prior plan's compensation metadata before their
effect is re-attempted. `rollback_request_for` and `replan_request_for` expose
the exact requests, so a persisted continuation can be verified later with the
inputs it was bound to.

## Invalidation

`evaluate_validity` compares a plan's binding with the supplied current inputs
and returns the first dimension that fails: authority, incarnation, epoch,
capability (growth or shrink), governing policy, topology drift, target intent,
or planning instant. Topology and target drift are measured from a compact
fingerprint set stored in the plan (per-entity content digests plus capacity and
active-demand totals), so drift is quantifiable within the tolerances declared in
the governing constraints. Policy changes carry no tolerance by design.

## Persistence

A plan artifact is `magic[16] | format u32 | schema u32 | payload length u64 |
payload digest[32] | payload`. The digest covers the header fields and the
payload. Readers verify the magic, both versions, the declared length against the
file size, the digest, the structural validity of the decoded plan, and that the
payload is the *canonical* encoding (a payload that decodes but re-encodes
differently is rejected). Publication writes a sibling temporary file and renames
it, so the destination is never a partial document; a killed writer leaves only
an orphaned partial file, which `scan` reports rather than silently ignoring.
Reading an artifact never makes it fresh: freshness is established by
`evaluate_validity` against current state.

## Concurrency and ownership

Planning is single-threaded per request and holds no locks: a `PlanRequest` is
copied into the run and the result is a value. The only shared mutable state in
the library is `PlanRepository`, audited as follows:

* exactly one mutex guards the slot map;
* serialization and digest computation happen before the lock is taken;
* no callback, event or user code runs while the lock is held;
* no nested lock acquisition exists on any path (there is only one lock);
* every accessor returns copies, so callers never hold a reference into locked
  state;
* eviction is deterministic (lowest plan id and generation retire first) and
  capacity is caller-bounded.

Cancellation is cooperative: `StopToken` is polled at bounded intervals inside the
scheduling search and the stage verification loop. A cancelled run returns a
refusal with code `cancelled` and never publishes a plan.

## Extension points

* New entity kinds follow the pattern: strongly typed identity, canonical codec,
  validation in the snapshot validator, typed operations, preconditions,
  postconditions and compensation metadata.
* New obligations become `ConstraintKind` items plus an invariant in the checker,
  so refusals can name them.
* New objectives become metrics with a deterministic computation and a documented
  position in the lexicographic priority list.
