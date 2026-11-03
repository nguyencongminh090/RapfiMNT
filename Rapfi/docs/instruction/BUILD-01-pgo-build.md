# BUILD-01 — execution guidance

Task: [`../todo/BUILD-01-pgo-build.md`](../todo/BUILD-01-pgo-build.md).

## Approach

1. Add `PGO_MODE` / `PGO_PROFILE` to `CMakeLists.txt` in the GNU/Clang branch only; default `OFF`.
2. Write `scripts/pgo_build.sh`: gen build -> train on `BENCH` -> merge -> use build.
3. Run the case list in the todo file; record Nodes/Hash and NPS in its §Verification.

## Pitfalls

- Apply profile flags with `target_compile_options` / `target_link_options` on `rapfi` only. Do not
  put them in `CMAKE_CXX_FLAGS`: that also reaches `rapfi_tests` and any external subproject.
- Clang needs `-fprofile-instr-generate` at link time too, or the runtime is not linked.
- Use `LLVM_PROFILE_FILE=<dir>/rapfi-%p.profraw` so threads/processes do not overwrite each other,
  then `llvm-profdata merge`. `llvm-profdata` must match the clang major version.
- Clang warns (`-Wprofile-instr-out-of-date`) and silently drops a stale profile after a source
  change. The script must fail on that warning's presence in the build log, not just on exit code.
- Never use `FORCE_ASSERTION` for the training or measured builds.
- Do not change engine sources. If bench Nodes/Hash move, the build is wrong; do not update the
  baseline.
