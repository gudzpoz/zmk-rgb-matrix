# RGB matrix architecture

Configuration belongs in the [README](../README.md); extension contracts belong in
[the extension guide](extending.md) and public headers. This document describes
system-wide ownership, ordering and consistency guarantees.

## Ownership and identity

- The reusable engine owns geometry, the declaration-ordered overlay scene,
  registered-overlay lifecycle/input, activity brightness, conditions and output.
  Product power/session/connection policy stays outside; generic inhibition supplies
  its output-permission boundary.
- One controller owns global defaults and the GLOBAL endpoint, including on a
  stripless central. Controlled compositions own selectable children and GLOBAL
  endpoints; private compositions own their child state.
- Enabled overlays and members have dense declaration-ordered identities. Both
  halves must agree on ordering and endpoint names. Rebuild and flash them together.
- Owners manage nested children. Retained selection and sampled live-child identity
  are distinct; switching back to an initialized child preserves its state unless
  a reset is required.

## Startup order

1. Resolve geometry and targets before overlay initialization. Authors guarantee
   target uniqueness.
2. Initialize required effects, overlays, conditions and provider state. Providers
   must be ready before RGB startup; late-provider recovery is unsupported.
3. Restore staged settings under the matrix mutex, then publish started state and
   start conditions once. Selected-effect queries return NULL before this boundary.

The RGB settings commit runs at `INT_MAX` so provider restoration finishes before
conditions can sample or trigger actions. Provider commits must run strictly earlier.
Without settings, startup runs during application initialization after providers.
Repeated commits preserve live parameters; changed restored selections invalidate
control freshness.

## Concurrency and scene admission

| Context | Contract |
| --- | --- |
| Thread-only controls and output | Serialize through the blocking matrix mutex. Binding callers may run on different threads. |
| Condition sampling and action dispatch | Run on the coordinator without the matrix mutex; gate publication takes it. |
| Renderer callbacks | Must not mutate scene controls, including through behavior invocation. Documented queries and flush/invalidation are allowed. |
| Any-context requests | Flush and condition invalidation are non-blocking; inhibition queries are also any-context. |
| Storage and split transport | Capture state under the matrix mutex; perform I/O unlocked. |

One delayable coordinator admits scene work in this order:

1. Sample each eligible live condition once, in dependency order.
2. Publish gates, then dispatch gate behaviors and trigger actions using that pass.
3. Acquire the matrix mutex and recheck control freshness.
4. Reconcile lifecycle, deliver input, then paint as needed. Deactivations precede
   required resets and activations, including on input-only passes.

Requests arriving during work survive completion. Pending control blocks new scene
admission; continuous invalidation can starve scenes. Already-admitted scenes may
finish, and published gates/actions are not rolled back. Black delivery remains
serviceable independently of control freshness.

Intermediate visibility transitions may coalesce. For conditions and triggers:

- Conditions refresh on invalidation; timed providers arrange their own wakeups.
- Samples must be side-effect-free and must not synchronously invalidate conditions.
- Rules must be non-looping. Initially true triggers execute `on-enter`; actions
  execute in declaration order.

## Activity and output

Retained user intent is separate from logical ON and runtime inhibition.

| Activity | Output policy when user intent is ON |
| --- | --- |
| ACTIVE | Use the configured maximum brightness. |
| IDLE | Zero idle brightness means logical OFF; otherwise cap idle brightness at the maximum, preserving effect phase. |
| SLEEP | Logical OFF. |

Each paint starts black, renders every active unoccluded contributor, applies master
brightness once and submits the disposable scene. Fully occluded participants are
inactive: they receive no input and cannot sustain animation. All-static scenes sleep.

Output blocking requests one black attempt; resume cancels an unsent request. With
initialized LEDs, newly observed SLEEP attempts black synchronously under the matrix
mutex before the activity listener returns, even when already OFF or inhibited.
A failed static frame or blackout can remain wrong indefinitely: output failures
are not autonomously retried, and driver latency is unbounded by the engine.

## Persistence

- A whole-state snapshot stores intent, master brightness and positional selections.
  Color and period are runtime-only; reload preserves them. Parameter-default
  restoration is separate from lifecycle reset.
- Missing or invalid selections use configured defaults. Count changes preserve
  valid overlapping entries, but reordering, insertion or disabling preceding
  entries can restore different effects and trigger different boot actions.
- Save-requesting behaviors debounce whole-state saves; global reset requests an
  immediate save of defaults. A snapshot can include another owner's non-saving
  mutation.
- Pending requests are consumed before unlocked storage I/O. Completion must preserve
  newer requests. Failure requires a later explicit save request; durability depends
  on the settings backend.

## Split consistency

GLOBAL conversion turns relative commands into addressed absolute commands. Use
`zmk_behavior_invoke_binding()` for forwarding; direct callbacks apply locally and
retain their save policy.

### Invocation pairing

Replay completion is guarded across conversion, forwarding and local application.
The bit-15 marker pairs conversion with exactly one pressed **or** released callback,
not key-down with key-up.

This depends on ZMK `6e2ef41e022d555b10f116e395832913f71717b3`: dispatch converts a
copied binding, forwards GLOBAL commands, then invokes exactly one local callback.
Endpoints and callbacks must remain available throughout. Converted bindings must
not be discarded, asynchronously retained, copied/reused or forged. Abandonment can
block replay completion indefinitely. Revalidate this invariant whenever upstream
dispatch or endpoint lifetime changes.

### Repair and convergence

- Each observed peripheral has a current-state cursor: brightness, intent, central
  gates, then every enabled member's color/period and each selection. Every attempt,
  including retries, samples current state.
- Completion requires unchanged generation, no outstanding converted invocations
  and no unconsumed repair request.
- Reconnect or transport change starts settled repair. Overlapping invocations or a
  negative converted callback request repair after the invocation group drains.
  Isolated successful calls and local-only changes do not start idle repair;
  replicated-value changes still invalidate an active pass.
- State sends share pacing; failed peers retry independently. Clock correction has
  its own cadence. Requests preserve settling/retry deadlines. Synchronization stays
  live while output is OFF, inhibited, hidden or stripless.
- Received central gates preserve independent local decisions. Application across
  gate words is not atomic.

Transport acceptance is not remote acknowledgment. Live enqueue errors or short
disconnects may go unobserved; continuous mutation can delay convergence. Repair
restores current values, not guaranteed reset actions.
