# BUILD-01 — Profile-guided optimization (PGO) build

**Status:** done (2026-11-03). Measured gain is modest (+3.6% median NPS), see §Verification.

**Class:** C (build tooling, no engine source touched). Decision note below stands in for a design doc.

## Goal

Raise NPS of the release binary without changing engine behaviour: add a CMake option for
profile-guided optimization (Clang and GCC) and a script that runs the full
instrument -> profile -> optimize cycle for any configure preset, with LTO on.

## Decision note (what / expected / how measured)

- **What:** CMake cache vars `PGO_MODE` (`OFF` default / `GENERATE` / `USE`) and `PGO_PROFILE`
  (path to the merged `.profdata` for Clang, or the `.gcda` directory for GCC). Flags are applied to
  the `rapfi` target only, never to `rapfi_tests`. `scripts/pgo_build.sh <preset>` drives three
  builds in `build/<preset>-pgo-{gen,use}`, training on `BENCH`.
- **Expected:** `PGO_MODE=OFF` leaves every compile/link command line unchanged. A PGO+LTO build
  searches the same tree as before (`-ffp-contract=off` is already forced) and is faster.
- **Measured:** `echo BENCH | ./pbrain-rapfi` Nodes `17621937` / Hash `8c1f72e5` identical to the
  baseline in `docs/process/change-process.md`, and median `Nodes/s` over 5 runs vs the plain
  build on the same preset (`x64-clang-AVX512VNNI`). Reference plain-build figure measured
  2026-11-03: `1018020` Nodes/s (single run, clang 21.1.8, no LTO).
- **Known limitation:** training workload is `BENCH` only; the profile is tied to the exact source
  and compiler. Re-run the script after any source change.

## Function -> test-case list (written before code)

There are no C++ functions in this slice; the units are the CMake option handling and the script.
Each case below is a manual/scripted check recorded in §Verification.

| Unit | Case name | Expected |
|---|---|---|
| CMake `PGO_MODE` | `pgoMode_off_leavesBuildCommandsUnchanged` | `compile_commands`/`build.ninja` flags equal those of the plain preset; bench identical |
| CMake `PGO_MODE` | `pgoMode_generateClang_addsInstrGenerateToCompileAndLink` | `-fprofile-instr-generate` present in compile and link lines of `rapfi` |
| CMake `PGO_MODE` | `pgoMode_generateGcc_addsProfileGenerate` | `-fprofile-generate` present (checked with gcc if installed; else recorded as untested) |
| CMake `PGO_MODE` | `pgoMode_useWithoutProfile_failsConfigure` | configure aborts with a clear `FATAL_ERROR` |
| CMake `PGO_MODE` | `pgoMode_useWithMissingFile_failsConfigure` | configure aborts when `PGO_PROFILE` does not exist |
| CMake `PGO_MODE` | `pgoMode_invalidValue_failsConfigure` | configure aborts naming the accepted values |
| CMake `PGO_MODE` | `pgoMode_msvcOrEmscripten_failsConfigure` | unsupported toolchain aborts (not silently ignored) |
| CMake `PGO_MODE` | `pgoMode_notAppliedToTestTarget` | `rapfi_tests` command line has no profile flag |
| `pgo_build.sh` | `pgoBuild_missingProfdata_exitsNonzeroWithMessage` | script exits != 0 when `llvm-profdata` (Clang) is absent |
| `pgo_build.sh` | `pgoBuild_emptyProfile_exitsNonzero` | script exits != 0 when no `.profraw` was produced |
| `pgo_build.sh` | `pgoBuild_endToEnd_benchNodesAndHashUnchanged` | final binary prints Nodes `17621937`, Hash `8c1f72e5` |
| `pgo_build.sh` | `pgoBuild_endToEnd_npsHigherThanPlainBuild` | median Nodes/s of PGO build > plain build (recorded, not asserted) |

## Verification

Environment: clang 21.1.8, preset `x64-clang-AVX512VNNI`, 8 threads, 2026-11-03.

| Case | Result |
|---|---|
| `pgoMode_off_leavesBuildCommandsUnchanged` | pass — sorted `FLAGS`/`DEFINES`/`LINK_FLAGS` lines of `build.ninja` identical (md5) between the old and new `CMakeLists.txt` |
| `pgoMode_generateClang_addsInstrGenerateToCompileAndLink` | pass — all 65 `rapfi` build blocks carry `-fprofile-instr-generate` |
| `pgoMode_generateGcc_addsProfileGenerate` | pass for configure only (gcc present, `-fprofile-generate` in `build.ninja`); GCC `USE` build and `.gcda` flow **not run** |
| `pgoMode_useWithoutProfile_failsConfigure` | pass — `FATAL_ERROR`, rc=1 |
| `pgoMode_useWithMissingFile_failsConfigure` | pass — `FATAL_ERROR`, rc=1 |
| `pgoMode_invalidValue_failsConfigure` | pass — `FATAL_ERROR` names accepted values |
| `pgoMode_msvcOrEmscripten_failsConfigure` | **not run** (no MSVC/Emscripten here); guarded by code only |
| `pgoMode_notAppliedToTestTarget` | pass after fix — first version leaked the flag into `rapfi_tests` (46/48 blocks) because the test target copies `rapfi` `COMPILE_OPTIONS`; now filtered, 0/48 |
| `pgoBuild_missingProfdata_exitsNonzeroWithMessage` | pass — rc=1, "llvm-profdata not found" |
| `pgoBuild_emptyProfile_exitsNonzero` | **not run**; guard is `ls *.profraw` / `find *.gcda` |
| `pgoBuild_endToEnd_benchNodesAndHashUnchanged` | pass — Nodes `17621937`, Hash `8c1f72e5` on all 5 PGO runs (= baseline) |
| `pgoBuild_endToEnd_npsHigherThanPlainBuild` | recorded — see below |

NPS (`Nodes/s`, 5 interleaved runs each): plain (no LTO) 962947 / 967228 / 975150 / 990218 / 1010779,
median **975150**; PGO+LTO 995252 / 1006737 / 1009968 / 1047989 / 1056977, median **1009968**
(**+3.6%**). Run-to-run spread is about 5%, so the ranges overlap; LTO-only was not measured
separately, so PGO and LTO contributions are not split. Far below the +8–20% estimate in
`bao-cao-toi-uu-kernel-rapfi.md`; the NNUE kernel is intrinsics-bound.

Not done: `rapfi_tests` was not rebuilt/run (no engine source changed; only its flag list is
filtered). Unit tests for the engine are unaffected by this slice.
