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
