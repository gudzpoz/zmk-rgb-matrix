# Native RGB tests

Run from the `zmk-rgb-matrix` module directory with the repository's Python environment
active or `west` on PATH. `ZMK_WS` must point to an **existing** ZMK west workspace:
`zmk/` in this monorepo. Never initialize a workspace at the repository root.

```sh
bash tests/unit_run.sh                 # SDK-free host tests
bash tests/sim/run-native.sh           # complete native/configuration matrix
bash tests/sim/run-native.sh contracts # one family
bash tests/sim/run-native.sh contracts -- --variant normal --scenario scene-entry-freshness --repeat 10
```

The [coverage inventory](../fixture-coverage.md) lists families and behavioral scope.
Runner tables are authoritative for scenarios. Unknown suites or scenarios fail.
Arguments after `--` pass to the selected runner; `selector_defaults`, `authoring`,
`configuration`, `composite` and `overlay_pair` accept none.

## Focused runs

Entry points below are in `tests/sim/`. Consult their `--help` for complete options.

| Entry point | Main filters/options |
| --- | --- |
| `test_contracts.py` | `--variant`, `--scenario`, `--repeat`, `--no-build`, `--build-dir` |
| `test_geometry.py`, `test_static_metadata.py` | repeated `--case`, `--build-dir` |
| `test_control_contracts.py` | `--scenario`, `--repeat`, `--build-dir` |
| `test_condition_contracts.py` | `--role`, repeated `--scenario`, `--repeat`, `--build-dir` |
| `test_effect_contracts.py` | `--scenario`, `--repeat`, `--build-dir`, `--example-module`, `--no-example` |
| `test_scoped_settings.py`, `test_scoped_split.py`, `test_singleton_settings_split.py` | repeated `--config`/`--scenario`, `--repeat`, `--no-build`, `--artifacts` |
| `test_smoke.py` | `--build-dir`, `--artifacts`, `--duration` |
| `test_ztest.py` | `--build-dir`, `--artifact-dir` |

- The full condition runner selects all three roles.
- Scoped split `--config transport` covers general transport/ordering;
  `transport-multiword` covers indexed gates. See the [configuration map](../fixture-coverage.md#split-repair-and-clock-mapping).
- Effects run full-strip and zero-LED images with UBSan. The external example runs
  when available; `--example-module` requires a specific checkout, while
  `--no-example` selects built-ins only.
- `--no-build` must reuse the original explicit artifact/build directory.

## Fixture authoring

- Compile production translation units with actual headers and DTS. Keep kernel
  threads, IRQs, initialization and queues real; do not copy functions/types or
  replace kernel/DT APIs.
- Restrict doubles to external strip, storage, transport, RNG and battery services.
- Start each runtime case in a fresh process. Share compiled images, not singleton
  state; settings-reordering cases share only saved records.
- Observe public boundaries, callbacks, packets or pixels. Use small shared setup
  helpers. `module/include/kp_rgb_test_mutations.h` is private test-only setup,
  not a public mutation API.
- Compare cadence/retries using simulated kernel time and completion timestamps.
  Native runs use `-no-rt`.

## Native artifact roots

Native Python harnesses that retain builds, logs, or captures accept the optional
`RGB_NATIVE_BUILD_ROOT` environment variable. With no explicit CLI root, each
suite writes beneath `<RGB_NATIVE_BUILD_ROOT>/<suite>`. Suite paths distinguish
different test families, but concurrent invocations need separate root values.
An explicit `--build-dir`, `--artifacts`, or
other root option takes precedence. Without either setting, each harness keeps
its existing default (including unique temporary directories and their existing
prefix/parent). These roots are retained and are never cleaned up by the root
selector. The setting does not change `--no-build` requirements or runtime
process isolation.

Example:

```sh
RGB_NATIVE_BUILD_ROOT=/tmp/rgb-native python3 tests/sim/test_contracts.py --variant normal
# Reuse a particular build/artifact root explicitly:
python3 tests/sim/test_contracts.py --build-dir /tmp/rgb-native/contracts
```

## Results and artifacts

`native_process.py` bounds subprocesses. Timeouts terminate the process group and
preserve partial output; they never satisfy expected-failure tests.

| Test kind | Required evidence |
| --- | --- |
| Runtime contract | Successful exit and exact scenario completion marker |
| Configuration rejection | Expected diagnostic |
| Capture | Validated frames |
| Smoke I/O failure | Exit 1 and the expected open/write diagnostic |

The top-level runner retains logs and temporary builds/captures under
`build/native-artifacts`. Explicit build directories remain on failure; repeated
scenarios have separate logs. CI retains generated configurations, logs and captures
on failure.

## CI cost reporting

The native job remains serialized. Its 2 GiB ccache is separate from the west
cache, and pristine builds remain enabled. Only superseded pull-request runs are
cancelled. The `native-cost-reports` artifact and job summary contain per-suite
TSV timings and ccache statistics. A valid
cost benchmark must use the same module and ZMK revisions on comparable runners
and compare disabled-cache, cold-cache, and warm-cache full-inventory runs,
including cache transfer and job overhead.
