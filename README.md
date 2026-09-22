# Change Planner

Vendor-neutral change planning for Summon Software Labs Fabric OS.

Change Planner answers one question: **given the current authoritative state of a
fabric, an operator's target intent, topology and capability evidence, and a set
of declared safety obligations, is there an ordered sequence of typed changes
that reaches the target without breaking any obligation - and if not, exactly
which obligation makes that impossible?**

It computes, verifies, persists and explains plans. It does not execute them.

## Boundary

Change Planner is a pure planning and control component.

It **does**:

* consume a generation-bound current-state snapshot, a declarative target intent,
  capability evidence and explicit safety constraints;
* derive the delta between them as typed operations;
* derive ordering dependencies so that unsafe parallel changes become ordered;
* group independent changes into stages and prove every interleaving of every
  stage safe;
* emit required drain, maintenance and configuration actions as typed plan steps
  for other runtimes to execute;
* refuse, with a minimal explanation, when no safe plan exists;
* replan from an observed partial execution without assuming any prior step
  succeeded;
* invalidate a plan when topology, authority, capability or target intent moves
  beyond declared tolerances;
* persist plans as immutable, integrity-checked artifacts and explain them.

It **does not**: distribute configuration, execute rollout stages, drain
resources, enter maintenance, talk to switches, or claim any vendor-specific
capability. It never assumes a device is healthy because a plan said so.

## Guarantees

Every plan Change Planner emits satisfies these invariants, each of which is
enforced mechanically rather than by convention:

1. **Every step is precondition-satisfied and postcondition-verified** under the
   planner model before the plan is published.
2. **Every interleaving of every stage is verified.** A stage is built only when
   exhaustive permutation replay proves that all orders keep every declared
   obligation satisfied and converge to the same state.
3. **No ordering silently violates redundancy, diversity, capacity, contract,
   exclusion or serialization obligations.** If no ordering exists, the planner
   refuses instead of fabricating one.
4. **Inputs are generation-bound.** Snapshot generation, authority identity,
   authority generation, boot incarnation, epoch and target revision travel with
   the plan. Stale inputs are refused, not reinterpreted.
5. **A plan is never valid against materially different state.**
   `evaluate_validity` compares the plan binding with current inputs and names the
   dimension that invalidated it.
6. **Identical inputs and policy produce semantically identical plans.** No wall
   clock, no hash-order iteration and no unseeded randomness reaches a decision;
   the final tie-break is a content digest.
7. **A cancelled run never publishes a plan.**
8. **Persisted plans are immutable and integrity-checked**: SHA-256 envelope,
   canonical-form verification, atomic publication, and conservative recovery
   that reports an orphaned partial write instead of trusting it.

## Architecture

```text
include/change_planner/
  core/      strong identities, SHA-256, canonical binary codec, strict JSON, cancellation
  model/     current state, target intent, constraints, capability evidence, operations
  safety/    invariants and the safety checker
  plan/      plan document, delta derivation, dependency graph, scheduling, verification,
             refusal minimisation, replanning, invalidation, comparison
  persist/   plan artifacts (integrity-checked, atomic) and a bounded plan repository
  cli/       document parsing and the command layer
src/         implementations of the above
tests/       unit, property, adversarial, concurrency, independent-process, downstream suites
benchmarks/  completed-work throughput measurements
examples/    end-to-end example, example input documents, downstream find_package consumer
```

### Domain model

State is a snapshot of devices, links, routes, workloads, policies and config
entries. Devices belong to failure domains; links carry capacity and transit
domains; routes are ordered link paths; workloads bind routes with demand and
requirements (connectivity, failure-domain redundancy, link-disjoint diversity,
guaranteed capacity, forbidden domains). Contracts add cross-workload
obligations.

Transitions are explicit typed operations: device add/remove/capacity/state/
windows, link add/remove/capacity, route add/remove/path/state, workload
add/remove/bindings/requirements, policy bind/unbind, config set/clear, and the
maintenance-class operations drain, enter-maintenance and exit-maintenance.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full pipeline and
[docs/SAFETY_MODEL.md](docs/SAFETY_MODEL.md) for the exact invariant definitions.

## Building

Requirements: CMake 3.24+ and a C++20 compiler. Validated with MSVC 19.44
(Visual Studio 2022 Build Tools) and Ninja on Windows.

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Options: `CPLAN_BUILD_TESTS`, `CPLAN_BUILD_BENCHMARKS`, `CPLAN_BUILD_EXAMPLES`,
`CPLAN_WARNINGS_AS_ERRORS` (default ON) and `CPLAN_ENABLE_ASAN` (AddressSanitizer).

Install and consume:

```sh
cmake --install build-release --prefix /path/to/prefix
# in an independent project:
#   find_package(ChangePlanner CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE ChangePlanner::change_planner)
```

`examples/downstream_consumer` is exactly such a project; the test suite installs
the package into a scratch prefix and then builds and runs it.

## CLI

```sh
cplan plan     --input request.json [--out plan.cplan] [--verify] [--json]
cplan explain  --plan plan.cplan --step step-... [--json]
cplan deps     --plan plan.cplan [--step step-...] [--json]
cplan compare  --left a.cplan --right b.cplan [--json]
cplan why-rejected --input request.json [--json]
cplan validate --plan plan.cplan --input request.json [--exhaustive] [--json]
cplan replan   --plan plan.cplan --observed observed.json --input request.json [--out c.cplan]
cplan rollback --plan plan.cplan --observed observed.json --input request.json [--out r.cplan]
cplan inspect  --plan plan.cplan [--json]
cplan scan     --directory DIR [--json]
cplan version
```

Exit codes: `0` success, `1` refused (no safe plan exists), `2` usage,
`3` invalid document, `4` IO or integrity failure.

A real run of the shipped example request:

```text
$ cplan plan --input examples/data/ladder-request.json
plan plan-ladder-42 generation 1 (initial lineage)
  steps: 5 in 2 stage(s)
  planning instant: 1000000000
  topology generation: 11, authority 'fabric-authority' generation 3
  objective: stages=2 risk=100 churn=5 maintenance=0ns
  verification digest: 6cbdb80a2a54729db4db7aeb607401d1fce65746cf3885183b13f023f6b350d6
  content digest:      24bed3ee4377bf6bdeb516eeb4838ded4116007c4c9d22bdc8f04da13f51e6e3
  stage 0 (stage-width-bound, 20 verified interleaving(s)):
    - step-1733db7eea8b875d8ab43c8c policy-bind p-change-window scope=global value=maintenance-window-mandatory reason=target-ensure
    - step-1be6c442fac955ff7b983386 link-set-capacity l-a1 ep-a--mid-1 capacity=15000 reason=target-capacity
    - step-4f88082fd1d7e6a92d2b69d2 link-set-capacity l-1b mid-1--ep-b capacity=15000 reason=target-capacity
    - step-57d04565b2a866c307619e55 link-set-capacity l-2b mid-2--ep-b capacity=15000 reason=target-capacity
  stage 1 (single-step-change, 1 verified interleaving(s)):
    - step-6908bf2ea88ca0859bd29f35 link-set-capacity l-a2 ep-a--mid-2 capacity=15000 reason=target-capacity
  rejected alternatives:
    - objective-loss: maintenance-first candidate rejected: candidate risk exposure 120 exceeds 100 (objective prioritises risk-exposure)
```

The same plan, after one capacity change landed and one was interrupted, replanned
from the observed state (`cplan replan --plan plan.cplan --observed
examples/data/observed-partial.json --input examples/data/ladder-request.json`):

```text
plan plan-ladder-42 generation 2 (replan lineage)
  steps: 5 in 2 stage(s)
  topology generation: 12, authority 'fabric-authority' generation 3
  objective: stages=2 risk=100 churn=4 maintenance=0ns
  stage 0 (compensate-partially-applied-steps, 1 verified interleaving(s)):
    - step-c86032a257f05c6165b6dbeb link-set-capacity l-1b mid-1--ep-b capacity=10000 reason=compensate:step-4f88082fd1d7e6a92d2b69d2
  stage 1 (independent-parallel-group, 24 verified interleaving(s)):
    - step-1733db7eea8b875d8ab43c8c policy-bind p-change-window scope=global value=maintenance-window-mandatory reason=target-ensure
    - step-4f88082fd1d7e6a92d2b69d2 link-set-capacity l-1b mid-1--ep-b capacity=15000 reason=target-capacity
    - step-57d04565b2a866c307619e55 link-set-capacity l-2b mid-2--ep-b capacity=15000 reason=target-capacity
    - step-6908bf2ea88ca0859bd29f35 link-set-capacity l-a2 ep-a--mid-2 capacity=15000 reason=target-capacity
```

The already-applied change (l-a1) is not re-issued, the interrupted change is
first compensated back to its previous value, and the failed and unacknowledged
changes are re-issued. Nothing is assumed about the prior attempt.

See [docs/CLI.md](docs/CLI.md) for the input document schema and worked sessions.

## Testing and proof surfaces

The suite covers unit, property (seeded randomized topologies), adversarial
(corrupt, truncated, contradictory, oversized), concurrency (threads, races,
cancellation), independent-process (real CLI processes, concurrent atomic
publication, a real process kill during publication) and downstream packaging
validation. See [docs/TESTING.md](docs/TESTING.md) for the full matrix, the
measured numbers and the explicit REAL / SYNTHETIC / UNSUPPORTED boundary.

## Performance

`cplan_bench` measures completed work - plans generated *and* exhaustively
verified - not submission latency:

```text
$ cplan_bench 3 20240917
cplan_bench: dual-path chain, link capacity bump + per-device config set; measured work = completed generate_plan + exhaustive verify_plan; iterations=3 seed=20240917
devices      steps   stages      plans/sec        steps/sec   verified perms/sec
8               16        4           2.41               39                  232
16              32        8           0.67               21                  128
32              64       16           0.13                8                   48
64             128       32           0.03                4                   24
```

## Limitations

* Planning is a bounded search over orderings. Exhausting a budget produces a
  refusal with code `search_limit_exceeded`, never an unverified plan.
* The planner refuses to plan from a state that already violates a declared
  obligation. It is a planner, not a repair engine.
* Stage width is bounded (`max_stage_width`, default 4) precisely because every
  interleaving of every stage is enumerated.
* Failure-domain redundancy is single-domain survivability: the number of
  eligible paths that remain after the loss of any one failure domain.
* Path-diversity search is exact up to an explicit budget; exceeding it produces a
  refusal rather than a guess.
* The evidence in this repository is its test suite: no live switch, fabric,
  firmware, RDMA, NVLink or multi-node deployment has been exercised.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
