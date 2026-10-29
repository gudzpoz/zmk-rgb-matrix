# RGB test coverage inventory

This maps behavioral contracts to test families, not recorded test results.
See the [runner guide](sim/README.md) for commands, fixture-authoring rules and
artifacts. Runner scenario/configuration tables are authoritative; CI uses the same
native inventory.

## Entry points

Family names are accepted by `sim/run-native.sh`.

| Family | Coverage |
| --- | --- |
| `contracts` | Engine, output, composition and lifecycle |
| `control_contracts` | Controls and owner helpers |
| `selector`, `selector_defaults` | Selection, owner-local defaults/reset, settings-on/off startup and configuration rejection |
| `condition_contracts` | Conditions, triggers and gates in all three roles |
| `effect_contracts` | Built-in effects, owner helpers and battery overlay |
| `scoped_settings` | Multi-owner persistence and storage races |
| `scoped_split` | Selector-bearing split repair |
| `singleton_settings_split` | Selector-free settings and final-gate repair completion |
| `authoring` | Documented snippets and fixed scenes |
| `configuration` | DTS constraints and configuration branches |
| `static_metadata`, `geometry` | Registries, targets and layouts |
| `composite` | Engine recomposition with destructive/failing composite children |
| `overlay_pair` | Custom owner, private children and opacity |
| `led_strip_composite` | Standalone driver topology, mutation and errors in both pixel layouts |
| `ztest` | Math/color helpers using Zephyr RGB types |
| `smoke` | Capture frames/format and I/O failure diagnostics |

`unit_run.sh` discovers host suites:

- `unit/test_scoped_persist.py`: snapshot encoding/validation and bounded decoding.
- `unit/test_native_process.py`: subprocess bounds, descendant cleanup and partial logs.
- `unit/test_preview_timing.py`: resampling and encoded duration; encoding checks
  explicitly skip when ffmpeg/ffprobe are unavailable.

## Engine

`sim/test_contracts.py` observes frames, provider callbacks and transfer timing.

| Contract | Evidence |
| --- | --- |
| Disposable output | Fresh composition after driver mutation or failure; static failures settle, animation follows cadence, explicit requests recompose. |
| Composition | Declaration order, sparse/reordered targets, opacity, filters and master brightness applied once. |
| Occlusion | Hidden participants receive no input or render demand; all-static scenes settle. |
| Lifecycle/input | Deactivation before reset/activation, then input/paint; deferred inactive resets, preserved timestamps and bounded input draining. |
| Clocks | Resume, rollover, slow rendering/transfers, coalescing and explicit recovery after scheduling errors. |
| Freshness | Interrupted samples and scene-entry races block stale work; already-admitted transfers finish. |
| Black service | One-shot black between interrupted passes; synchronous SLEEP black even when already OFF/inhibited/idle-blank. |
| Serialization | Mutex contention preserves work; setters, rendering and transfers serialize; supported IRQ requests succeed. |
| Power boundaries | Boot activity states, idle caps and phase preservation; zero LEDs, unready output, empty registries and inactive scenes. |

## Controls

`sim/test_control_contracts.py` and selector/default fixtures check:

- Owner/member isolation, endpoint locality, private/foreign/disabled membership and
  rejection of commands sent to the wrong endpoint.
- Read-only relative conversion and absolute application against diverged state.
  Boundary values exercise selection wrapping, hue wrapping and saturation/duration
  clipping before narrowing.
- Changed, identical and rejected requests through callbacks and rendering.
- Scoped/global/default-only resets, inactive-child reset on use, deferred callbacks,
  preserved timestamps and independent owner clocks.
- Black-initialized child targets, opacity, and private no-save/no-send setup paired
  with positive behavior-save/send controls.
- Configured/default brightness, duration bounds and early parameter retention with
  settings enabled or disabled, across standalone, central and peripheral roles.

`control-isr` tests supported any-context operations. Malformed-command cleanup is
checked in thread context; these tests do not establish thread-only API safety in ISRs.

## Conditions

`sim/test_condition_contracts.py --role all` checks:

| Area | Evidence |
| --- | --- |
| Dependency graph | Once-per-pass dependency ordering, shared diamond/chain/AND/OR providers, short-circuiting and unused providers. |
| Invalidation | Coalesced IRQ/external-work interruptions and action-owned requests survive into later passes. |
| Role authority | Transitive CENTRAL_ONLY faults, received versus local gates and peripheral trigger suppression. |
| Triggers | Initial transitions, cached values, shared timestamps, ordered pressed-only actions and continuation after action errors. |
| Freshness | Selection bursts, resume, source changes and settings reload block stale input/render/transfer without assisting invalidation. |
| Gates | Coverage/opacity/replacement boundaries, zero LEDs and live gates while OFF/inhibited. |

Finite cascades yield without losing rules. Arbitrary feedback-loop termination and
side-effecting samples are outside the contract. Dependency, scope and root failures
require specific diagnostics. Split fixtures own transport assertions; engine
fixtures own the comprehensive input/lifecycle assertions.

## Effects

`sim/test_effect_contracts.py` exercises every built-in mode in full-strip and
zero-LED images with UBSan. The external example runs when available; an explicitly
required missing checkout fails. Coverage includes:

- Full, reordered, sparse and empty targets; sentinels, render demand and RNG use.
- Matched entropy/time for brightness scaling, with integer-aware HSV/ripple checks.
  Static DTS pixels are exempt from effect-brightness scaling.
- Parameter-preserving reset/activation, zero-delta idempotence, extreme elapsed time,
  phase/wrap handling, cooling, terminal demand and reactive-feedback clearing.
- Stochastic bulk versus partitioned time, interval boundaries, bounded catch-up and fade.
- Instance lifecycle, independent clocks and raw dispatch preserving supplied buffers
  and timing.
- Owner forwarding, opacity and terminal black; battery fill ordering, sparse targets,
  clamping and zero LEDs. Battery listener-to-engine flush is outside this owner-local test.

## Authoring and configuration

`sim/test_authoring.py` uses separate images:

| Case | Evidence |
| --- | --- |
| `documented-readme`, `documented-guide` | Compile actual Markdown DTS/keymap snippets; build checks only. |
| `status` | Standalone selector-free scene: captured pixels, brightness and live OFF/inhibited gates. |
| `api-only-zero` | Trigger-enabled zero-LED image: gates and empty capture. The `&none` action does not demonstrate action execution. |
| `api-only-split-local` | Peripheral local conditions, trigger/split-sync suppression and captured scene transitions. |

Configuration tests check controller count, placement, child cardinality, targets
and supported schema constraints. Native role fixtures check service enablement.

## Metadata and geometry

- Static metadata: dense indices/gates, local/received authority, key/raw target
  ordering, last-mapping-wins resolution, absent/invalid targets and shared all-LED views.
- Cross-translation-unit providers: early invalidation, forward DTS dependencies and
  numeric registration order. Empty registries must link and start.
- Geometry: declared versus chosen layouts, transforms, signed coordinates,
  normalization, reverse mappings/extents and zero local LEDs.
- Missing keys, incompatible layouts and invalid mappings require diagnostics.
  Target uniqueness is a fixture precondition.

## Startup boundary

Controls/defaults, conditions and settings fixtures check provider initialization and
restoration before RGB publication or sampling. Public selections remain NULL before
startup; repeated unstaged commits preserve parameters and stay silent.
Deferred split endpoints are tested separately from RGB provider startup.

## Settings mapping

**SS** = `sim/test_scoped_settings.py`; **GS** = the `settings` and
`settings-disabled` configurations of `sim/test_singleton_settings_split.py`.
SS general scenarios use `auto`; `original` and `reordered` share saved records
across layouts to demonstrate positional reinterpretation, including changed boot actions.

Host tests own codec byte-level correctness. Native tests observe backend capture,
staging, restoration and concurrent I/O.

| Contract | Scenarios and evidence |
| --- | --- |
| Validation | SS/GS `validation`: malformed records and failed reads preserve valid staging; missing/excess entries and invalid slots exercise fallback. |
| Whole-state save | SS `existing`: a selection behavior saves both owners, including another owner's private mutation. Parameter commands alone do not save. |
| Debounce | SS `debounce`: latest deadline, latest complete state, then silence. |
| Reset | SS `staged-reset`: scoped reset preserves other staged values; global reset restores defaults. GS covers selector-free reset. |
| Failed saves | SS `global-failure`, `*-second-failure`: durable bytes and runtime state survive; recovery requires an explicit request. |
| Concurrent requests | SS `selector-{dirty,reset,failed-dirty,failed-reset}`, `global-{reset,failed-reset}`: immutable first snapshot followed by newly requested state. |
| Expired deadlines | SS `*-expired`: blocked I/O preserves newer requests whether the earlier write succeeds or fails. |
| Reset timing | Concurrent reset variants distinguish debounced scoped reset from immediate global reset. |
| Reload | SS `existing`: full/subtree commits restore saved selections/global state while preserving runtime parameters; unstaged commits stay silent. |
| Publication | Provider restoration replaces staged state before selected-effect queries become available. |
| Selector-free/disabled | GS checks capture/reload and startup/reset with settings enabled or disabled. |

A second Zephyr thread acquires the matrix mutex during SS storage callbacks.
Assertions also check that concurrent mutations leave the in-flight snapshot intact.

## Split repair and clock mapping

**SP** = `sim/test_scoped_split.py`; **GP** = split configurations of
`sim/test_singleton_settings_split.py`.

### Selector-bearing configurations

| SP configuration | Purpose / focused scenarios |
| --- | --- |
| `transport` | General transport and ordering, mutation/reset sweeps, local/central gates and dim/black defaults. |
| `transport-zero` | Zero-LED reconnect, retry resampling, interrupted selection/reset replay and gate publication. |
| `transport-three` | Additional member traversal after a disabled slot; sweep phases 8/9. |
| `transport-multiword` | `multiword-later`, `multiword-final`, `multiword-control`: word boundaries, resampling and control dispatch. |
| `ordering-settings` | `ordering-settings`, `ordering-settings-active`: real commits while idle/in flight. |
| `transport-settling` | `ordering-request-settling`: requests during settling. |
| `transport-absent` | `no-transport`: all transports unavailable. |
| `late-global` | Deferred global endpoint readiness. |
| `receiving` | Peripheral command validation and local authority. |
| `receiving-multiword-zero` | Indexed peripheral reception with zero LEDs. |

`multiword-control` runs a complete condition pass on the RGB queue, outside the
matrix mutex, then requires a silent second pass.

### Worker contracts

Transport doubles record accepted presses into a peripheral FIFO model. Final
assertions compare every member's color/period, selections, intent, brightness and
gates. A second thread acquires the matrix mutex during enqueues; a queue sentinel
checks one attempt per handler.

| Contract | SP scenarios / evidence |
| --- | --- |
| Replay order | `existing` and convergence cases: globals, indexed gates, member parameters, then selection. |
| Retry and fairness | `retry-*`, `pacing-*`: resampled values, completion-relative deadlines, shared pacing and healthy-peer progress. |
| Clocks | `clock`, `clock-failure`: independent deadlines, current timestamps and continued state progress. Peripheral cases check signed offsets/wrap. |
| Live/replay races | `ordering-before`, `ordering-during`: stale replay follows a live packet, then converges after callback completion. |
| Overlap | `ordering-overlap-*`: both completion orders, idle and active repair; one coalesced repair after the group drains. |
| Isolated calls | `ordering-isolated`, `ordering-relative`, `ordering-noop`, `ordering-release`, `ordering-enqueue-error`: forwarding/cleanup without idle repair. |
| Failure/cleanup | `ordering-failure`, `ordering-global-failure`, `ordering-cleanup`: marked callback failures drain reservations; direct calls cannot consume another invocation. |
| Request races | `ordering-request-*`: requests at enqueue, completion, exit, settling and disconnect survive worker progress. |
| Mutation/reset | `ordering-sweep-*`: changes at each replay field/completion boundary converge after successful or failed enqueue. |
| Local changes | `local-silence`, `ordering-silence`: publication invalidates active replay without starting idle repair. |
| Gates/defaults | `ordering-defaults`, `ordering-gates-repair`: changed/no-op publication, inactive defaults, local-gate exclusion and latest central-gate repair. |
| Settings | `ordering-settings*`: parameters survive reload; idle commits stay quiet and active repair converges. |
| Connections | `transport-change`, `negative-query`, `no-transport`: replacement, query errors, unavailable transport and recovery. |
| Reception | `receiving`: real peripheral validation, member isolation and no echo. |

### Selector-free global phases

GP isolates completion at the last gate word, rather than at a selector's selection.
Its transport/source callbacks also allow another thread to acquire the matrix mutex.

| GP configuration | Purpose |
| --- | --- |
| `split` | Global fields, final-gate completion, retries, pacing, fairness and silence. |
| `split-zero` | Reconnect, interrupted final-gate completion and retry resampling without LEDs. |
| `split-multiword` | Intermediate/final-word boundaries and mutations to already-sent words. |
| `split-unready` | Deferred global endpoint readiness. |

### Evidence limits

- FIFO acceptance does not prove radio delivery or remote acknowledgment.
- Zero-LED cases prove protocol completion, not physical strip writes.
- LED-count and topology variants cover selected boundaries, not every cross-product.
- Tests preserve initialized device readiness and immutable metadata.
- Absent-transport cases verify unavailable transport and silent traffic; they do not
  require ZMK to clear its internal transport pointer.
