# Testing and proof surfaces

This document states what is actually verified by this repository, how to
reproduce it, and where the boundary between real, synthetic and unsupported
evidence lies.

## Running the suite

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

AddressSanitizer configuration:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCPLAN_ENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Tests are plain executables that run to completion. There are no watchdogs, no
sleep-based synchronisation and no timeouts of any kind: a hanging test is a
defect to diagnose, not something to hide behind a timer.

## Suites

| Suite | Source | Cases | What it proves |
|---|---|---|---|
| core | tests/unit/core_test.cpp | 18 | Strong identities, checked arithmetic, SHA-256 against NIST vectors, canonical codec round trips and rejection of truncation/limits, strict JSON parsing, UTF-8 validation. |
| model | tests/unit/model_test.cpp | 9 | Snapshot validation (duplicates, dangling references, malformed windows), order-independent canonicalization, typed operation application, condition evaluation, precondition/postcondition derivation, dependency prerequisites, content-derived step identity, compensation metadata, deterministic operation ordering. |
| safety | tests/unit/safety_test.cpp | 11 | Every invariant: connectivity, path diversity, failure-domain survivability, capacity headroom, maintenance windows and exclusions, change serialization, service continuity, contracts, capability evidence, generation binding, deterministic violation ordering, and that disabled obligations are not enforced. |
| plan | tests/unit/plan_test.cpp | 13 | Plan generation for capacity/policy/config changes, determinism of identical inputs, dependency-respecting schedules (drain before removal, unbind before route removal), refusal when no ordering exists, converged targets, stale-input fencing, capability refusal, maintenance-window gating, window shifting, stage-width bounds, objective priority effects, rejected-alternative reporting. |
| persist | tests/unit/persist_test.cpp | 11 | Artifact round trip and integrity, corruption/truncation/version/canonical-form rejection, scan reporting of unreadable files and orphaned partials, bounded repository semantics (idempotent republication, conflict rejection, deterministic eviction, retirement), invalidation dimensions, declared-tolerance drift, observation fencing, continuation planning, rollback to the pre-plan state. |
| property | tests/property/planner_property_test.cpp | 6 | Seeded randomized topologies: plan-or-refuse (never an unverified plan), prefix simulation of every plan, artifact round trip of every generated plan, determinism across runs and input orderings, stages never containing dependent steps, replanning from every partial execution, fencing of every stale generation, cancellation. |
| adversarial | tests/adversarial/adversarial_test.cpp | 6 | Malformed document shapes and enum tokens, duplicate identities, dangling references, contradictory constraints, impossible transitions with typed errors, resource limits that refuse instead of truncating, systematic byte mutation and truncation of artifacts, repository accounting returning to baseline. |
| concurrency | tests/concurrency/concurrency_test.cpp | 4 | Concurrent publish/find/list on the repository, deterministic planning from many threads, cancellation races that never publish a partial plan, repeated create/write/read/verify lifecycles. |
| process | tests/process/process_test.cpp | 5 | Real CLI child processes: plan/publish/inspect/validate round trip, refusal exit codes, six concurrent publishers of one artifact, a real process kill during publication, and torn-write rejection by an independent reader. |
| downstream | tests/downstream/run_downstream.cmake | 1 | Installs the package into a scratch prefix, then configures, builds and runs an independent CMake project through `find_package(ChangePlanner CONFIG REQUIRED)`. |

83 test cases plus the packaging validation.

## Measured results

All numbers below were produced on the development machine (Windows, MSVC 19.44,
Ninja, x64) with this repository at its release state.

| Configuration | Build | Warnings | ctest |
|---|---|---|---|
| Release | clean | 0 first-party warnings (`/W4 /WX`) | 10/10 suites pass |
| Debug | clean | 0 first-party warnings | 10/10 suites pass |
| Debug + AddressSanitizer | clean | 0 first-party warnings | 10/10 suites pass |

Completed-work throughput (Release), measured by `cplan_bench`; each sample
generates a plan *and* exhaustively verifies it:

```text
$ cplan_bench 3 20240917
devices      steps   stages      plans/sec        steps/sec   verified perms/sec
8               16        4          15.70              251                 1507
16              32        8           4.12              132                  792
32              64       16           0.99               64                  381
64             128       32           0.07                9                   55
```

Cost grows with the exhaustive proof (all permutations of every stage) rather
than with submission.

## Property and randomized testing

Randomized scenarios are generated by a seeded splitmix64 generator, so every
run is reproducible from its seed; the property suite uses fixed seed bases and
covers device counts, failure-domain counts, extra links and target mutations
(capacity uplift, configuration, policy binding, path retirement).

The central property is: **for every seeded scenario, the planner either returns
a plan that passes exhaustive interleaving verification and whose every prefix
keeps every declared obligation, or returns a refusal that explains itself**. No
third outcome exists, and no plan is returned unverified.

## Independent-process tests

`cplan_process_tests` drives the real `cplan` executable and a small helper
process through the operating system:

* one process plans and publishes an artifact, another inspects and validates it;
* six processes publish the same artifact concurrently; after all of them exit,
  the destination is a complete, integrity-checked artifact and no partial file
  is left behind;
* a helper process is killed (`TerminateProcess`) while it is mid-publication: the
  destination never exists, the orphaned partial file is reported by `scan` as an
  incomplete publication, and a later healthy publication succeeds in the same
  directory;
* a helper that publishes a truncated document non-atomically is rejected by
  every reader, and `inspect` exits with the IO/integrity code.

## Defects found and fixed during hardening

Every item below was reproduced by a test before being fixed, and the fix is
covered by the suite that found it.

| Defect | Found by | Fix |
|---|---|---|
| Empty optional identities (for example the parent plan of an initial plan) failed to decode, making every persisted plan unreadable | artifact round trip in the persist/process suites | The identity codec treats an empty field as the canonical absent reference. |
| Steps joined by a dependency edge could be grouped into the same executable stage, producing a plan whose own structure check rejected it | plan suite (capacity scenario) | Stage compression refuses to place linked steps together. |
| Incremental capacity changes were ordered by failure-domain serialization, collapsing unrelated parallel work into extra stages | plan suite (parallel stage expectations) | Domain serialization now considers only changes that take a device out of service. |
| Workload invariants were evaluated even when their governing obligation was disabled, so constraint relaxation could not explain a refusal | property suite (blocking-constraint check) | Connectivity, redundancy, diversity and contract checks are gated by their constraint kinds. |
| A plan's stored preconditions were derived from the state *after* its stage was applied, so verification failed for correct plans | plan suite (`scheduled_order_respects_dependencies`) | Stage entry state is captured before application. |
| Maintenance-window shifting silently fell back to the requested instant when no window was open | plan suite (window shift) | An impossible shift is now an explicit `no-permitted-maintenance-instant` refusal. |
| Concurrent publishers of the same artifact collided on one temporary file name and raced each other's rename | process suite (six concurrent publishers) | Temporary publication names include the process id. |
| A rename over a destination that another process was replacing could fail transiently on Windows and surface as an IO error | process suite under AddressSanitizer | Publication retries the rename a bounded number of times before failing. |
| The `rollback` subcommand did not select rollback mode and behaved exactly like `replan` | independent documentation review of the CLI | The subcommand injects its mode; the flag now only overrides it. |
| `PlanningLimits::maxSchedulingAttempts` was parsed, validated and digested but never consulted | independent documentation review of the CLI | The budget is enforced before each candidate strategy. |
| Devices created by a plan could never satisfy the capability-evidence obligation, making device addition impossible | adversarial suite (intent-created device) | Plan-created devices are exempt from pre-existing evidence and are gated by their declared target capabilities instead. |
| A data race in the concurrency test itself (loop index captured by reference) reported spurious nondeterminism | 30-fold repetition of the concurrency suite | The index is captured by value; determinism holds under 30 consecutive runs. |

## Real, synthetic and unsupported evidence

**REAL** (executed in this repository, reproducible with the commands above):

* the planner, safety checker, verifier, persistence layer and CLI as compiled
  artefacts;
* unit, property, adversarial and concurrency suites running as native processes;
* independent OS processes: the CLI, the crash helper, concurrent publishers and a
  real process kill;
* the CMake install/export and an independent downstream `find_package` consumer
  that builds and runs against the installed artifact;
* AddressSanitizer-instrumented builds of the entire suite;
* the benchmark numbers above, measured as completed work.

**SYNTHETIC** (modelled inside the process, not observed from hardware):

* every fabric state, topology, device, link, route, workload, failure domain and
  capability evidence set in the suites and examples is generated by fixtures;
* simulated execution: applying a plan to a snapshot model is not the same as
  executing it on a fabric, and the tests say so;
* the crash and torn-write scenarios simulate a failing writer, using real
  processes and real files, but not real power loss or filesystem failure;
* timing measurements are wall-clock observations of completed work on one
  machine, not throughput guarantees.

**UNSUPPORTED** (not claimed anywhere in this repository):

* live switches, fabrics, controllers, firmware or vendor APIs;
* RDMA, NVLink, InfiniBand, multi-GPU or multi-node transports;
* a distributed planning service, consensus, or leader election;
* telemetry, licensing servers or any network egress;
* execution of plans: Change Planner plans, and other runtimes execute.
