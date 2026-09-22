# Change Planner CLI

`cplan` is the operator and developer surface of Change Planner: it reads a
generation-bound planning request document, derives the typed transition to the
requested target, verifies every interleaving of every stage, explains the
result step by step, and persists it as an integrity-checked artifact that other
runtimes may choose to apply. It never executes a change, never distributes
configuration, never drains a device or enters maintenance, and never contacts a
switch.

## Synopsis

```text
Change Planner 1.0.0 - Summon Software Labs
Vendor-neutral change planning runtime. Plans are computed, verified and reasoned about;
this tool never distributes configuration or executes rollout stages.

usage: cplan <command> [options]

commands:
  plan           generate a plan (or a refusal) from a request document
  why-rejected   plan and report the alternatives the search rejected
  explain        explain one step: preconditions, dependencies, compensation
  deps           list dependency edges and stages of a plan
  compare        compare two plan artifacts step by step
  validate       re-verify a stored plan and check it against current inputs
  replan         replan from an observed partial execution state
  rollback       build a compensation plan back to the pre-plan state
  inspect        inspect a stored artifact (content, binding, integrity)
  scan           scan a directory of artifacts and report unreadable ones
  version        print version information
  help           print this text

common options:
  --json                 machine-readable output
  --quiet                suppress diagnostics on stderr
  --input PATH           planning request document (required by plan/why-rejected/validate/replan)
  --plan PATH            plan artifact (explain/deps/inspect/validate/replan/rollback)
  --observed PATH        observed execution document (replan/rollback)
  --out PATH             write the generated plan as an artifact
  --step STEP_ID         restrict explain/deps to one step
  --left PATH --right PATH   artifacts for compare
  --verify               verify the generated plan before reporting success
  --window-shift         allow the planner to shift the maintenance instant
  --mode rollback        replan in rollback mode
  --exhaustive           verify every interleaving (validate)
  --directory PATH       directory for scan

exit codes: 0 success, 1 refused (no safe plan exists), 2 usage, 3 invalid document, 4 IO/integrity
```

| Command | Required arguments | Notes |
| --- | --- | --- |
| `plan` | `--input PATH` | Optional `--out PATH`, `--verify`, `--label TEXT`, `--window-shift` |
| `why-rejected` | `--input PATH` | Reports the candidate orderings the search rejected |
| `explain` | `--plan PATH --step STEP_ID` | One step: preconditions, dependencies, compensation |
| `deps` | `--plan PATH` | With `--step STEP_ID` it prints the same document as `explain` |
| `compare` | `--left PATH --right PATH` | Step-level difference between two artifacts |
| `validate` | `--plan PATH --input PATH` | Invalidation decision plus re-verification; `--exhaustive` replays every interleaving |
| `replan` | `--plan PATH --observed PATH --input PATH` | Optional `--out PATH` and `--mode rollback` |
| `rollback` | `--plan PATH --observed PATH --input PATH` | Routed to the same code path as `replan`; pass `--mode rollback` for the rollback lineage (see below) |
| `inspect` | `--plan PATH` | Content, binding, integrity and an explicit freshness warning |
| `scan` | `--directory PATH` | Reports readable and unreadable artifacts, including orphaned partial writes |
| `version` | none | Product, version, artifact/schema identities |
| `help` | none | Alias: `--help`, `-h`; `version` also answers to `--version` |

Argument handling:

* `--flag value` and `--flag=value` are both accepted. A flag with no
  following value, or followed by another `--` token, is stored as the literal
  value `true`.
* `--json` and `--quiet` are global and are stripped before command dispatch,
  so they are recognised only as exact standalone tokens. `--json=true` is not
  a JSON switch: `cplan version --json=true` still prints the text form.
* Positional arguments after the command are collected but no command reads
  them.
* Relative paths are resolved against the process working directory; the
  examples below assume the repository root.

## Exit codes

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | success | The command completed: a plan was produced, or the artifact/scan was clean |
| 1 | refused | No safe plan exists, verification failed, or `scan` found unreadable artifacts |
| 2 | usage | The command line itself is unusable (missing `--step`, `--observed`, `--directory`, `--plan`, unknown command, no arguments) |
| 3 | invalid document | The request/observed document was rejected: malformed JSON, unknown enum token, invalid identity, contradictory counters |
| 4 | IO or integrity | File missing, oversized, truncated, wrong magic, unsupported envelope version, digest mismatch |

How the code is chosen:

| Situation | Exit |
| --- | --- |
| `Status` is `not_found`, `truncated`, `integrity_failure`, `unsupported_version` or `size_limit` | 4 |
| `Status` is `invalid_argument`, `malformed_input` or `duplicate_identity` while parsing a request document | 3 |
| A plan artifact cannot be loaded | 4, except a missing `--plan` flag, which is a usage error -> 2 |
| Missing `--input` flag | 3 (`invalid_argument: --input PATH is required`), unlike the other missing flags |
| `plan`/`replan` produced a refusal | 1, except `RefusalCode::cancelled`, which is reported as 4 |
| `plan --verify` produced a report with `ok: false` | 1 |
| `validate` where invalidation or verification failed | 1 |
| `scan` where at least one entry is unreadable | 1 |

Observed probes (build of this tree):

```text
$ cplan
<usage text>
[exit 2]

$ cplan plan
invalid_argument: --input PATH is required
[exit 3]

$ cplan plan --input nope.json
not_found: cannot open 'nope.json' for reading
[exit 4]

$ cplan inspect
invalid_argument: --plan PATH is required
[exit 2]

$ cplan frobnicate
unknown command 'frobnicate'
<usage text>
[exit 2]
```

## Input documents

`cplan plan` (and `why-rejected`, `validate`, `replan`, `rollback`) read one
JSON request document. Documents are parsed strictly: a missing required member,
a wrong JSON kind, an out-of-range number, an unknown enum token or a duplicate
object key is rejected with a precise message instead of being defaulted.

### Request document

| Member | JSON type | Required | Default | Notes |
| --- | --- | --- | --- | --- |
| `planId` | string | yes | - | Identity, ASCII rules below |
| `planGeneration` | integer | no | `1` | Plan generation; replanning increments it |
| `planningInstant` | integer | no | `state.observedAt` | Nanoseconds; the planner never reads the wall clock |
| `label` | string | no | `""` | Free text; echoed in the plan |
| `objective` | string or array of strings | no | `"safety-first"` | Objective code or metric priority list |
| `limits` | object | no | built-in defaults | See "Objectives and limits" |
| `state` | object | yes | - | Authoritative snapshot |
| `target` | object | yes | - | Declarative intent |
| `constraints` | object | yes | - | Declared safety obligations |
| `evidence` | object | yes | - | Observed capabilities |

Unrecognised members are ignored, not rejected. The shipped example carries
`"schema": "cplan.input.v1"` and `"limits": { "maxSearchNodes": 0 }`; neither
is read by the parser and `cplan plan` succeeds with both present.

### state

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `topologyGeneration` | integer | yes | - |
| `authority` | string (identity) | yes | - |
| `authorityGeneration` | integer | no | `0` |
| `bootIncarnation` | integer | no | `0` |
| `epoch` | integer | no | `0` |
| `observedAt` | integer (ns) | no | `0` |
| `devices` | array of device objects | no | `[]` |
| `links` | array of link objects | no | `[]` |
| `routes` | array of route objects | no | `[]` |
| `workloads` | array of workload objects | no | `[]` |
| `policies` | array of policy objects | no | `[]` |
| `configs` | array of config-entry objects | no | `[]` |

Element objects (required members marked *):

| Object | Members (type, default) |
| --- | --- |
| device | `id`* (identity), `domain`* (identity), `state` (entity-state token, `present`), `terminationCapacity` (integer, 0), `terminationReserved` (integer, 0), `capabilities` (array of capability tokens, `[]`), `maxConcurrentChanges` (integer, 1), `profileCode` (string, `""`), `exclusions` (array of exclusion windows, `[]`), `maintenanceWindows` (array of maintenance windows, `[]`) |
| exclusion window | `id`* (identity), `begin`* (integer, ns), `end`* (integer, ns), `reason` (string, `""`) |
| maintenance window | `id`* (identity), `begin`* (integer, ns), `end`* (integer, ns), `ticket` (string, `""`) |
| link | `id`* (identity), `endpointA`* (device identity), `endpointB`* (device identity), `capacity` (integer, 0), `reserved` (integer, 0), `latencyClass` (integer, 0), `transitDomains` (array of failure-domain identities, `[]`), `state` (entity-state token, `present`) |
| route | `id`* (identity), `source`* (device identity), `sink`* (device identity), `path` (array of link identities, `[]`), `policy` (policy identity, empty), `state` (route-state token, `inactive`) |
| workload | `id`* (identity), `bindings` (array, `[]`), `requirements` (object, absent), `contractCode` (string, `""`) |
| workload binding | `route`* (route identity), `active` (boolean, `true`), `demand` (integer, 0) |
| workload requirements | `requireConnectivity` (boolean, `true`), `minDisjointDomains` (integer, 0), `minLinkDisjointPaths` (integer, 0), `minGuaranteedCapacity` (integer, 0), `forbiddenDomains` (array of failure-domain identities, `[]`) |
| policy | `id`* (identity), `scope` (policy-scope token, `global`), `target` (string, `""`), `value` (string, `""`), `enforced` (boolean, `true`) |
| config entry | `device`* (device identity), `key`* (config-key identity), `value` (string, `""`), `revision` (integer, 0) |

The `requirements` defaults apply only when the object is present. When
`requirements` is absent from a workload the workload records no connectivity
requirement at all, because the underlying aggregate is default-constructed.

### target

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `revision` | integer | yes | - |
| `basedOnGeneration` | integer | yes | - |
| `authority` | string (identity) | yes | - |
| `authorityGeneration` | integer | no | `0` |
| `basedOnEpoch` | integer | no | `0` |
| `authoredAt` | integer (ns) | no | `0` |
| `intentCode` | string | no | `""` |
| `devices`, `links`, `routes`, `workloads`, `policies`, `configs` | arrays of desired entries | no | `[]` |

Every desired entry carries `action` (desired-action token, default `ensure`)
and, except configs, `id`* (identity). When `action` is `remove` only the
identity is read; the configuration members are ignored. When it is `ensure`
the entry carries the same members as its state counterpart with these
differences:

| Desired entry | Spec members and defaults |
| --- | --- |
| device | `domain`* (identity), `state`, `terminationCapacity`, `terminationReserved`, `capabilities`, `maxConcurrentChanges`, `profileCode`, `exclusions`, `maintenanceWindows` - same defaults as state |
| link | `endpointA`*, `endpointB`*, `capacity`, `reserved`, `latencyClass`, `transitDomains` - same defaults as state |
| route | `source`*, `sink`*, `path`, `policy`, `state` (route-state token, default `active`, not `inactive`) |
| workload | `bindings`, `requirements`, `contractCode` |
| policy | `scope`, `target`, `value`, `enforced` (default `true`) |
| config | `device`* (device identity), `key`* (config-key identity), `value` |

### constraints

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `id` | string (identity) | yes | - |
| `authorityGeneration` | integer | no | `0` |
| `governingPolicy` | string (policy identity) | yes | - |
| `governingPolicyDigest` | string (hex digest) | no | empty; must be valid hex when non-empty |
| `items` | array of constraint items | yes | - (an empty array is accepted) |
| `contracts` | array of contracts | no | `[]` |
| `tolerance` | object | no | all defaults below |

Constraint item:

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `id` | string (identity) | yes | - |
| `kind` | string (constraint-kind token) | yes | - |
| `enabled` | boolean | no | `true` |
| `headroom` | integer | no | `0`; must be within 0..1000 permille |
| `maxConcurrentChanges` | integer | no | `1` |
| `maxDomainsInMaintenance` | integer | no | `1` |
| `parameter` | string | no | `""` |
| `justification` | string | no | `""` |

Contract:

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `id` | string (identity) | yes | - |
| `workloads` | array of workload identities | no | `[]` |
| `requiredUnits` | integer | no | `0` |
| `minDisjointDomains` | integer | no | `0` |
| `maxSimultaneousDomainMaintenance` | integer | no | `1` |
| `forbidConcurrentChangesInSameDomain` | boolean | no | `true` |
| `sla` | string | no | `""` |

Tolerance:

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `topologyDrift`, `capacityDrift`, `demandDrift` | integer (permille) | no | `0` |
| `maxAddedEntities`, `maxRemovedEntities` | integer | no | `0` |
| `allowCapabilityGrowth`, `allowCapabilityShrink` | boolean | no | `false` |
| `exemptIdentities` | array of strings | no | `[]` |

### evidence

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `topologyGeneration` | integer | yes | - |
| `bootIncarnation` | integer | no | `0` |
| `capabilityVersion` | integer | no | `0` |
| `observedAt` | integer (ns) | no | `0` |
| `devices` | array of capability evidence | no | `[]` |

| Evidence entry | JSON type | Required | Default |
| --- | --- | --- | --- |
| `device` | string (device identity) | yes | - |
| `capabilities` | array of capability tokens | no | `[]` |
| `capabilityVersion` | integer | no | the document-level `capabilityVersion` |

### observed execution

| Member | JSON type | Required | Default |
| --- | --- | --- | --- |
| `planId` | string (plan identity) | yes | - |
| `planGeneration` | integer | yes | - |
| `attempt` | integer | no | `1`; must not be zero |
| `authorityGeneration` | integer | no | `0`; must equal `state.authorityGeneration` |
| `observedGeneration` | integer | no | `0`; must equal `state.topologyGeneration` |
| `bootIncarnation` | integer | no | `0` |
| `observedEpoch` | integer | no | `0` |
| `observedAt` | integer (ns) | no | `0` |
| `state` | object | yes | - (parsed exactly as the state document above) |
| `observations` | array of step observations | no | `[]` |

| Step observation | JSON type | Required | Default |
| --- | --- | --- | --- |
| `step` | string (step identity) | yes | - |
| `attempt` | integer | no | `1`; must not be zero |
| `outcome` | string (step-outcome token) | no | `unknown` |
| `detail` | string | no | `""` |

Two observations for the same step are rejected as a duplicate identity.

### Enum tokens

| Enum | Tokens |
| --- | --- |
| Entity state | `absent`, `present`, `draining`, `in-maintenance`, `removed` |
| Route state | `inactive`, `active` |
| Policy scope | `global`, `device`, `link`, `route`, `workload` |
| Capability | `none`, `device-add`, `device-remove`, `device-capacity`, `device-state`, `device-drain`, `device-maintenance`, `link-add`, `link-remove`, `link-capacity`, `route-add`, `route-remove`, `route-state`, `policy-bind`, `config-set`, `workload-binding`, `workload-requirements` |
| Constraint kind | `connectivity`, `failure-domain-redundancy`, `path-diversity`, `capacity-headroom`, `maintenance-exclusion`, `maintenance-window`, `drain-before-maintenance`, `change-serialization`, `workload-contract`, `capability-evidence`, `rollback-metadata`, `generation-binding` |
| Desired action | `ensure`, `remove` |
| Step outcome | `succeeded`, `failed`, `skipped`, `unknown`, `partially-applied` |
| Objective metric | `minimum-stages` or `min-stages`, `risk-exposure` or `min-risk`, `churn` or `min-churn`, `maintenance-duration` or `min-maintenance` |

An unknown token is rejected: a device with `"state": "presentt"` fails with
`malformed_input: unknown entity state token: presentt` and exit code 3.

### Identities and strict parsing

Identities (device, link, route, workload, failure domain, policy, contract,
config key, window, plan, step, constraint, authority) are strings that must be
non-empty, at most 128 bytes long, and made only of the characters
`A-Z a-z 0-9` and the six punctuation characters `- . / : @ _`. Every other
byte is rejected: control characters, space and all non-ASCII bytes because the
accepted range is `0x21`..`0x7E`, and the following 26 printable characters
because they are explicitly refused:

```text
"  \  '  `  <  >  |  ?  *  &  ^  %  #  !  (  )  [  ]  {  }  ,  ;  $  ~  =  +
```

```text
$ cplan plan --input bad-identity.json      # "planId": "plan ladder 42"
invalid_argument: identity text rejected for plan: 'plan ladder 42'
[exit 3]
```

JSON-level strictness (all observed against a copy of the shipped example):

| Rule | Result |
| --- | --- |
| Duplicate object keys | `malformed_input: '...': duplicate object key: planId (offset 17970)`, exit 3 |
| Trailing content or truncated document | `malformed_input`, exit 3 |
| Invalid UTF-8 | `malformed_input`, exit 3 |
| Unknown enum token | `malformed_input: unknown entity state token: ...`, exit 3 |
| Non-negative integers only | A negative generation/capacity is rejected |
| Document size | 8 MiB maximum; larger input fails with `size_limit: '...' exceeds the maximum document size`, exit 4 |
| Nesting depth | 64 levels maximum; deeper input fails with `malformed_input: '...': maximum nesting depth exceeded (offset 65)`, exit 3 |
| String size | 1 MiB per string |
| Array/object size | 1048576 elements per container |
| Unrecognised members | Ignored (not an error) |

## Worked sessions

Every command and output below was produced by running the CLI built from this
tree, from the repository root. `<out>` stands for a scratch output directory
(the verification run used `%TEMP%\cplan-scratch\doc-work2`); no other
character of the output was altered, and long documents are cut with an explicit
`... (truncated)` marker.

`cplan version` - identity of the binary and of the artifact/plan/input schemas.

```text
$ cplan version
Change Planner 1.0.0 (Summon Software Labs)
artifact format 1, plan schema 1, input schema cplan.request
```

`cplan plan --input examples/data/ladder-request.json` - the text form: two
stages, four parallel capacity changes plus one ordered change, and the three
candidate orderings the objective rejected.

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
    - objective-loss: unlock-first candidate rejected: candidate lost the deterministic tie-break on content digest
    - objective-loss: maintenance-first candidate rejected: candidate risk exposure 120 exceeds 100 (objective prioritises risk-exposure)
    - objective-loss: reverse-canonical candidate rejected: candidate lost the deterministic tie-break on content digest
```

`cplan plan --input ... --out <out>\p.cplan --verify --json` - machine-readable
form. With `--json` an invocation emits exactly one JSON object; `--verify`
adds the verification report and `--out` records the artifact path it wrote.

```text
{
  "artifactPath": "<out>\\p.cplan",
  "plan": {
    "binding": { ... (truncated) },
    "notes": [
      "candidate ordering produced by strategy safety-first",
      "verified 2 stage(s) over 5 step(s)"
    ],
    "stageList": [
      {
        "index": 0,
        "rationale": "stage-width-bound",
        "steps": [ ... (truncated) ],
        "verifiedApplications": 80,
        "verifiedPermutations": 20
      },
      ... (truncated)
    ]
  },
  "verification": {
    "explanation": "verified 97 step application(s) across 25 interleaving(s) and 98 invariant evaluation(s)",
    "failingStage": 0,
    "failingStep": "",
    "invariantEvaluations": 98,
    "message": "",
    "ok": true,
    "permutationsVerified": 25,
    "stepsApplied": 97,
    "traceDigest": "6c46c395fcb7b3275c5ca20e9aa38cef5a3dbef327f1b770dacbef229375a65f",
    "violations": []
  }
}
```

What to look for: every step is listed with its operation, operation class, risk,
duration, preconditions, postconditions, the compensation that undoes it, and
the invariants checked for it; the stage list records how many interleavings
were replayed.

`cplan inspect --plan <out>\p.cplan` - the stored plan plus three lines that
matter: integrity, payload digest, and the explicit statement that freshness is
not assumed.

```text
$ cplan inspect --plan <out>\p.cplan
plan plan-ladder-42 generation 1 (initial lineage)
  steps: 5 in 2 stage(s)
  planning instant: 1000000000
  topology generation: 11, authority 'fabric-authority' generation 3
  objective: stages=2 risk=100 churn=5 maintenance=0ns
  verification digest: 6cbdb80a2a54729db4db7aeb607401d1fce65746cf3885183b13f023f6b350d6
  content digest:      24bed3ee4377bf6bdeb516eeb4838ded4116007c4c9d22bdc8f04da13f51e6e3
  ... (truncated)
  integrity: verified (yes), payload 6904 bytes
  artifact payload digest: 1caa561c54f658523e80654fbcba4a6993fc9cb27b71f2a301adb5dc5ed69e16
  freshness: NOT assumed - run 'validate' against current state
```

`cplan deps --plan <out>\p.cplan` - the dependency graph and stage membership.
This plan has no explicit edges: its ordering comes from the stage boundaries,
and stage 1 exists because the last change could not be verified in parallel.

```text
$ cplan deps --plan <out>\p.cplan
dependency edges of plan plan-ladder-42:
  (none)
stages:
  stage 0: step-1733db7eea8b875d8ab43c8c step-1be6c442fac955ff7b983386 step-4f88082fd1d7e6a92d2b69d2 step-57d04565b2a866c307619e55
  stage 1: step-6908bf2ea88ca0859bd29f35
```

`cplan explain --plan <out>\p.cplan --step step-4f88082fd1d7e6a92d2b69d2` - why
one step is safe: preconditions checked against the entry state, postconditions
verified after it applies, what it waits for, what it may run beside, and how it
is undone. Use a real step id from `deps` or `inspect`.

```text
$ cplan explain --plan <out>\p.cplan --step step-4f88082fd1d7e6a92d2b69d2
step step-4f88082fd1d7e6a92d2b69d2 (stage 0): link-set-capacity l-1b mid-1--ep-b capacity=15000 reason=target-capacity
  class: configuration
  risk: 30, estimated duration 30000000000ns
  reason: statically-verified
    note: operation class configuration
  invariants checked: connectivity failure-domain-redundancy path-diversity capacity-headroom maintenance-exclusion maintenance-window change-serialization workload-contract capability-evidence generation-binding service-continuity
  preconditions:
    - entity-present(device:ep-b)
    - entity-present(device:mid-1)
    - entity-present(link:l-1b)
    - exclusion-inactive(device:ep-b)
    - exclusion-inactive(device:mid-1)
  postconditions:
    - capacity-at-least(link:l-1b, 15000)
  depends on: nothing
  unlocks: nothing
  concurrent with: step-1733db7eea8b875d8ab43c8c step-1be6c442fac955ff7b983386 step-57d04565b2a866c307619e55
  compensation: inverse (link-set-capacity l-1b mid-1--ep-b capacity=10000)
```

`cplan validate --plan <out>\p.cplan --input examples/data/ladder-request.json --exhaustive`
- the two independent checks: the invalidation decision against the supplied
inputs, and re-verification of every interleaving.

```text
$ cplan validate --plan <out>\p.cplan --input examples/data/ladder-request.json --exhaustive
plan remains valid (none)
verified 97 step application(s) across 25 interleaving(s) and 98 invariant evaluation(s)
```

When the inputs have moved the command refuses and names the dimension:

```text
$ cplan validate --plan <out>\p.cplan --input authority-change.json
plan invalidated (authority) [authority-changed]: plan was computed under authority 'fabric-authority' but the current authority is 'other-authority'
verification failed: plan validity binding does not describe the supplied request: the plan was computed against different inputs
[exit 1]
```

`cplan compare --left <out>\p.cplan --right <out>\p.cplan` - two artifacts
compared step by step. Comparing an artifact with itself reports equality and
both content digests; comparing a plan with its replan shows the added
compensation step and the dropped already-applied step.

```text
$ cplan compare --left <out>\p.cplan --right <out>\p.cplan
plans are identical (left: 5 steps in 2 stages; right: 5 steps in 2 stages)
left content:  24bed3ee4377bf6bdeb516eeb4838ded4116007c4c9d22bdc8f04da13f51e6e3
right content: 24bed3ee4377bf6bdeb516eeb4838ded4116007c4c9d22bdc8f04da13f51e6e3
no step-level differences
```

`cplan replan --plan <out>\p.cplan --observed examples/data/observed-partial.json --input examples/data/ladder-request.json --out <out>\p2.cplan`
- generation 2, replan lineage: the already-applied step is absent, the
interrupted step is compensated first, and the failed and unacknowledged steps
are re-issued.

```text
$ cplan replan --plan <out>\p.cplan --observed examples/data/observed-partial.json --input examples/data/ladder-request.json --out <out>\p2.cplan
plan plan-ladder-42 generation 2 (replan lineage)
  steps: 5 in 2 stage(s)
  planning instant: 1000000000
  topology generation: 12, authority 'fabric-authority' generation 3
  objective: stages=2 risk=100 churn=4 maintenance=0ns
  verification digest: f7b0c968579b1ee8be700bc9746e044082b60e8c14cd1a4300a89704a8cbe413
  content digest:      75ebf3e1d87594c2d6461366b8c893fcd40c8f83a97b3830cddaa91da06b0ecf
  stage 0 (compensate-partially-applied-steps, 1 verified interleaving(s)):
    - step-c86032a257f05c6165b6dbeb link-set-capacity l-1b mid-1--ep-b capacity=10000 reason=compensate:step-4f88082fd1d7e6a92d2b69d2
  stage 1 (independent-parallel-group, 24 verified interleaving(s)):
    - step-1733db7eea8b875d8ab43c8c policy-bind p-change-window scope=global value=maintenance-window-mandatory reason=target-ensure
    - step-4f88082fd1d7e6a92d2b69d2 link-set-capacity l-1b mid-1--ep-b capacity=15000 reason=target-capacity
    - step-57d04565b2a866c307619e55 link-set-capacity l-2b mid-2--ep-b capacity=15000 reason=target-capacity
    - step-6908bf2ea88ca0859bd29f35 link-set-capacity l-a2 ep-a--mid-2 capacity=15000 reason=target-capacity
  rejected alternatives:
    - objective-loss: unlock-first candidate rejected: candidate lost the deterministic tie-break on content digest
    - objective-loss: maintenance-first candidate rejected: candidate lost the deterministic tie-break on content digest
    - objective-loss: reverse-canonical candidate rejected: candidate lost the deterministic tie-break on content digest
wrote artifact <out>\p2.cplan
```

The same run with `--json` records the rebase in the plan notes:

```text
"notes": [
  "candidate ordering produced by strategy safety-first",
  "verified 1 stage(s) over 4 step(s)",
  "replanned from observed attempt 2: 1 step(s) reported succeeded, 1 failed, 1 unknown, 1 partially applied",
  "no prior step effect is assumed: the continuation was derived from the observed authoritative state",
  "target intent and capability evidence rebased onto observed topology generation 12 and epoch 2 (revision 7 unchanged)",
  "compensated 1 partially applied step(s) before continuing"
]
```

`cplan scan --directory <out>` - explains the trust level of every file in a
directory. Artifacts that parse report their payload digest; unreadable entries
report the reason, and an orphaned partial write is reported as such rather than
hidden or trusted. With at least one unreadable entry the command exits 1.

```text
$ cplan scan --directory <out>
<out>\c.cplan  OK  b56f022a53d6fea8
<out>\junk.cplan  UNREADABLE  truncated: artifact is shorter than the minimum envelope size
<out>\p.cplan  OK  1caa561c54f65852
<out>\stale.cplan.partial-42-0  UNREADABLE  incomplete publication: orphaned partial file (a writer did not finish)
2 readable, 2 unreadable
[exit 1]
```

`cplan help` - prints the synopsis in the first section of this document and
exits 0. Running `cplan` with no arguments prints the same text and exits 2.

## Objectives and limits

### Objective codes

`objective` is either a code string or an explicit metric priority list. The
planner evaluates candidate orderings under the list (the first metric
dominates every later metric) and breaks a complete tie on the candidate content
digest, so identical inputs and policy always select the same plan.

| Code | Priority | What it does |
| --- | --- | --- |
| `safety-first` | stages > risk > churn > maintenance | Default. Fewest stages first, then lowest peak per-stage risk, then fewest touched entities, then shortest maintenance |
| `min-stages` | identical to `safety-first` | Alias; the code string is preserved in the plan |
| `min-risk` | risk > stages > churn > maintenance | Prefers the smallest peak risk exposure even at the cost of an extra stage |
| `min-churn` | churn > stages > risk > maintenance | Prefers touching the fewest entities |
| `min-maintenance` | maintenance > stages > risk > churn | Prefers the shortest total maintenance duration |
| array of metrics | the order given | Code becomes `custom`; empty lists and repeated metrics are rejected |

```text
$ cplan plan --input examples/data/ladder-request.json      # objective: "safety-first"
  objective: stages=2 risk=100 churn=5 maintenance=0ns
  ... (truncated)
  rejected alternatives:
    - objective-loss: maintenance-first candidate rejected: candidate risk exposure 120 exceeds 100 (objective prioritises risk-exposure)
```

The shipped example shows the priority working: the maintenance-first ordering
would have had the same stage count but a peak risk of 120 against the selected
candidate's 100, so it lost on the second metric.

### Planning limits

| Member | Default | Bounds and effect | When exhausted |
| --- | --- | --- | --- |
| `maxSteps` | 4096 | Maximum number of derived operations a plan may contain | Refusal `search-limit-exceeded` / `step-budget-exceeded`, exit 1 |
| `maxCandidates` | 4 | Number of candidate orderings evaluated; must be within 1..16, and only the four built-in strategies exist (`safety-first`, `unlock-first`, `maintenance-first`, `reverse-canonical`), so at most four are ever run | Fewer alternatives reported; never a silent unsafe plan |
| `maxStageWidth` | 4 | Maximum steps grouped into one stage; must be within 1..5 so that exhaustive interleaving replay stays bounded | The scheduler closes the stage and starts another; a value outside 1..5 is rejected with exit 3 |
| `maxSchedulingAttempts` | 64 | Accepted and required to be positive; not consulted by the current scheduler | No effect in this build |
| `maxCoreMinimisationAttempts` | 64 | Bounds the search that minimises the blocking constraint core reported in a refusal | The reported core stops shrinking; the refusal is still emitted |
| `maxPermutationChecks` | 2000000 | Total interleavings replayed while proving a stage safe | Candidate aborts with `permutation verification budget exhausted` |
| `maxStatesVisited` | 2000000 | Total state applications visited during scheduling and interleaving checks | Candidate aborts with `state budget exhausted during interleaving verification` |
| `allowWindowShift` | `false` | Lets the planner move the planning instant to the earliest permitted maintenance instant | Without it, an instant outside every window is refused |

A budget is never "spent" by publishing a partially verified plan: exceeding a
candidate's budget rejects that candidate, and if no candidate completes the run
ends in a refusal document and exit code 1.

```text
$ cplan plan --input small-budget.json          # "maxSteps": 1
refused (search-limit-exceeded): step-budget-exceeded
notes:
  - target requires 5 operations which exceeds the configured step budget of 1
[exit 1]

$ cplan plan --input wide-stage.json            # "maxStageWidth": 9
invalid_argument: max_stage_width must be within 1..5 so that exhaustively verified interleavings stay bounded
[exit 3]
```

## Artifacts

A `.cplan` file is a plan artifact: a fixed little-endian envelope around the
canonical binary encoding of the plan document.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 16 | Magic `CPLAN-ARTIFACT`, zero padded |
| 16 | 4 | Format version (`1`); a different value is refused as unsupported |
| 20 | 4 | Plan schema version (`1`); a different value is refused as unsupported |
| 24 | 8 | Payload length in bytes |
| 32 | 32 | SHA-256 payload digest |
| 64 | payload length | Canonical plan payload |

The digest covers the magic, both versions, the payload length and the payload,
so header tampering is detected as well as payload tampering, and the total
envelope header is 64 bytes. Writing is atomic: the artifact is written to a
sibling file named `<path>.partial-<pid>-<sequence>`, flushed, and then renamed
over the destination, so a partially written artifact is never observable at the
destination path. A failed write removes the partial file.

Integrity is verified on read. Reading refuses, rather than repairs:

* a file shorter than the envelope, or whose declared payload length does not
  match the file size, is `truncated` (exit 4);
* a wrong magic or non-canonical payload is `malformed_input`;
* a payload that decodes but does not re-encode byte for byte is an
  `integrity_failure`;
* a wrong format or schema version is `unsupported_version`;
* artifacts larger than 32 MiB are refused as `size_limit`.

Reading a plan never implies accepting it. `inspect` prints the integrity
result, the payload digest and the line `freshness: NOT assumed - run 'validate'
against current state`; the plan carries the topology generation, authority,
authority generation, boot incarnation, epoch, capability version, target
revision and the state/target/constraint/evidence digests it was computed from
in its validity binding, and only `validate` compares that binding with current
inputs.

`scan` enumerates a directory and reports every entry in sorted path order:

| Entry state | Report |
| --- | --- |
| Readable artifact | `OK` plus the short payload digest |
| Orphaned partial file (name contains `.partial-`) | `UNREADABLE  incomplete publication: orphaned partial file (a writer did not finish)` |
| Corrupt or truncated file | `UNREADABLE` plus the exact parser error |
| Any unreadable entry | The command exits 1; the summary line counts readable and unreadable entries |

See the `scan` example in "Worked sessions" for the exact output.

## Replanning and rollback

`replan` starts from what the runtime actually observed, not from what the
prior plan expected. The observed execution document is fenced before anything
else happens:

| Fence | Rule | Violation |
| --- | --- | --- |
| Plan identity | `planId` must equal the artifact's plan id | Refusal `stale_input`, exit 1 |
| Plan generation | `planGeneration` must equal the artifact's generation | Refusal `stale_input` |
| Authority generation | `authorityGeneration` must be at least the plan's authority generation | Refusal `stale_input` |
| Topology generation | `observedGeneration` must be at least the plan's topology generation | Refusal `stale_input` |
| Boot incarnation | `bootIncarnation` must equal the plan's boot incarnation | Refusal `stale_input` |
| Self-consistency | `authorityGeneration` must equal `state.authorityGeneration`, and `observedGeneration` must equal `state.topologyGeneration` | Exit 3 |
| Attempts | Plan generation and every attempt number must be non-zero; one observation per step | Exit 3 |

No prior step effect is assumed. The continuation is derived from the observed
authoritative snapshot, so a step reported `succeeded` that is absent from the
observed state is planned again, and a step reported `failed` or `unknown` is
re-issued rather than trusted. Steps that are still satisfied by the observed
state produce no operation at all.

The target intent and the capability evidence are rebased onto the observed
generation and epoch: `target.basedOnGeneration`, `target.basedOnEpoch`,
`target.authority`, `target.authorityGeneration` and the evidence generation
and incarnation are taken from the observed state, while the target revision is
unchanged. The rebase is recorded, never silent - the resulting plan notes name
the observed attempt, the outcome census, the "no prior step effect is assumed"
statement, the rebased generation and epoch, and how many partially applied
steps were compensated. The four note strings are quoted verbatim in the
`replan` session above.

Partially applied steps are compensated before the continuation is attempted.
A step qualifies only when its compensation is recorded as available and is an
inverse; the compensation is emitted as a new step in a prepended stage whose
operation is the inverse, whose reason code is `compensate:<prior step id>`,
whose identity is derived from that operation, and whose preconditions and
postconditions are derived from the observed state. The prepended stage is
re-verified, and if the compensation cannot be applied safely the run refuses
with `no_safe_ordering` / `compensation-unsafe` instead of continuing.
`--no-compensate` disables this and leaves partial effects in place.

Rollback builds the plan that returns the fabric to the state the prior plan
started from: the observed state is the start and the pre-plan state is the
target, with lineage kind `rollback`. It is selected with
`cplan replan --mode rollback ...`; the `rollback` subcommand currently enters
the same code path as `replan` without setting that mode, so
`cplan rollback ...` produces replan lineage unless `--mode rollback` is given
explicitly. Both forms refuse if the supplied request does not describe the
state the prior plan started from (`rollback-origin-mismatch`).