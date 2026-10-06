# VCN — Victory by Continuous N-level attack search mode (VC2..VC4; VC5 internal)

**Status:** done (2026-10-06). See §Verification results.

**Class:** B (search behaviour) + C (protocol surface). Decision note below stands in for a design
doc: no encoding, bitkey, zobrist-identity or compile-time table is changed. The one identity-ish
change (VCN-keyed TT hash) is confined to the TT probe/store key inside AB search while VCN is
active and is the identity function (`0`) otherwise.

Reference: <https://github.com/dhbloo/rapfi/tree/copilot/add-vcn-search-mode> (one commit,
"search: add and stabilize VCN search mode"). That commit is incomplete as published (it refers
to `vcnLevel`, `vcnIsAttacker` and `vcnAllowB4` in scopes where they are not defined, has no
protocol entry point, and indexes history/pattern tables with `Pos::PASS`), so it is ported by
hand to this tree rather than cherry-picked.

## Goal

A search mode in which one side (the *attacker*) must win by continuous attack while the other side
(the *defender*) may additionally **pass**. Level `N` in `VC2..VC5` bounds how many passes the
defender may use before the attacker has to win outright:

| Level | Meaning |
|---|---|
| `VC5` | Attacker must already have a five on the board at his turn (defender passed enough). No search. |
| `VC4` | Attacker may only play VCF (fours). A defender pass here means `VC5`. Equivalent to plain VCF. |
| `VC3` | Defender may pass; each pass moves the attacker to `VC4`. |
| `VC2` | Defender may pass; each pass moves the attacker to `VC3`. |

Level is carried per ply in `SearchStack::vcnLevel`; a node derives it from its parent and bumps it
by one when the parent (as defender) played `Pos::PASS`.

Two ways to switch it on (attacker is always the side to move when the search starts). `VC5` is
the internal end state of the level chain and is **not selectable** (hidden 2026-10-06: as a start
level it only proves open-four / double-four wins, a subset of VC4/VCF, and the GUI never sends it):

```
INFO VCTHREAD <0..3> Yixin-Board option ("Check VCT"/"Check VC2"): 0 off, 1 VCT=VC3, 2 VC2,
                     3 VC4 (extension, the GUI never sends it). Persistent. Prints
                     `VCN Mode enabled: ...` like the earlier engine_vcni build.
YXVCN <2..4>         one search at that level, then back to the INFO VCTHREAD level
```

(An earlier `INFO VC` option was removed 2026-10-06 in favour of `INFO VCTHREAD`.)

`think()` stamps `vcnMode.attacker = board.sideToMove()` before every search. `YXVCN` saves the
`INFO VCTHREAD` level, sets its own, calls `think()` and restores it (options are copied into the search
context when thinking starts, so restoring right after `think()` returns is safe).

## Decision note (what / expected / how measured)

- **What:** add `VCNLevel`/`VCNMode` to `search/searchcommon.h` and `SearchOptions::vcnMode`;
  `SearchStack::vcnLevel`; in `search/ab/search.cpp` derive/propagate the level, segregate TT keys by
  `(level, attacker)`, route `VC4` attacker nodes to `vcfsearch` (no stand-pat), add the `VC5`
  "no five ⇒ attacker loses" check, disable null-move pruning, and make `vcfdefend` return
  "defender wins by passing" at `VC4`; in `search/movepick.*` add an optional pass stage for the
  defender; guard every `Pos::PASS` use that indexes pattern/history tables; disable database
  read/write while VCN is active.
- **Expected:** with `vcnMode` disabled (every existing command) behaviour is unchanged: the new
  branches are all gated on `vcnMode.enabled()`; `SearchStack::vcnLevel` is zero (`VCNLevel::None`) and
  `Vcn::hashXor` returns 0. With `YXVCN`, a position that is a proven attacker win at that level
  returns a mate score for the attacker, and positions where the defender can refute with passes
  return a non-mate score.
- **Measured:** `rapfi bench` node count + hash bit-identical to the recorded baseline
  (`16733809` / `4621a98a`) — see §Verification. Targeted positions via unit tests.
- **Purpose and later decisions:** see VCN-02 §Settled decisions (VCN value = attack chance; ordinary pruning kept).
- **Deliberately not done:** VCN is not wired into MCTS, the PV output of passes is not formatted
  specially, and VCN is not exposed through `INFO`. Heuristic pruning (futility, razoring, LMR) is
  left active inside VCN search below the attacker's VC4/VC5 cut points; results are search
  results, not a proof certificate.

## Settled design points

| Point | Decision |
|---|---|
| Naming clash | `GenType::VC2` (global, movegen) already exists with a different meaning (two-threat move filter). `VCNLevel` is a scoped `enum class` (`VCNLevel::None/VC2/VC3/VC4/VC5`), so it cannot collide with it. Deviates from the reference, which uses an unscoped enum. |
| Activation | `YXVCN n` one-shot (user decision 2026-10-06) plus persistent `INFO VCTHREAD n` (user request 2026-10-06; `INFO VC` dropped the same day); attacker = side to move. |
| Scope | Full VC2–VC5 search levels (user decision 2026-10-06); VC5 hidden from users, internal only. |
| Pass stages | Defender pass is offered in `MAIN`, `DEFENDFOUR` and `DEFENDB4F3`; **never** in `DEFENDFIVE` (a five must be blocked). Never twice in a row. |
| Null move | Disabled while VCN is on (VCN has its own pass). |
| Database | Probe and write both disabled while VCN is on. Reference only restricted writes; reading would return non-VCN labels for VCN-keyed nodes. |
| `Pos::PASS` safety | `pattern4()`, `mainHistory`, `counterMoveHistory`, `statScore` and killers are indexed by cell; a pass uses a `NONE` sentinel / skips the update instead. |
| Root | Root level is initialised to `n` on every ply of the stack at the start of `ABSearcher::search`. Root move list never contains `PASS`. |

## Scope steps

1. `search/searchcommon.h` — `VCNLevel`, `VCNMode`, `SearchOptions::vcnMode`.
2. `search/ab/searchstack.h` — `vcnLevel` field (default `VCNLevel::None`).
3. `search/movepick.{h,cpp}` — `ExtraArgs<MAIN>::generatePassMove`, `ExtraArgs<QVCF>::forceAllowB4InVCF`,
   three `*_PASS` stages, `PASS`-safe `ttmValid`.
4. `search/ab/history.cpp` — `PASS`-safe `updateTTMoveStats` / `updateBestmoveStats` / `addSearchedMove`.
5. `search/ab/search.cpp` — `Vcn` helpers, level derivation, TT key, VC4/VC5 cut points, null-move
   gate, pass-safe heuristics, DB gating, `vcfsearch`/`vcfdefend` changes.
6. `command/gomocup.cpp` — `INFO VCTHREAD`, `YXVCN` handler + dispatch; `think()` sets the attacker.
7. Tests (see below) + `CMakeLists.txt` test target entry.

## Function → test-case list (write before code)

The `Vcn::` helpers (`levelIncrement`, `childLevel`, `hashXor`, `vc5AttackerLoses`) live in an
anonymous namespace of `search.cpp`, so they are covered through search results, not called directly.

`MovePicker` (MAIN / QVCF)
- `movePickerMain_generatePassOff_neverYieldsPass`
- `movePickerMain_generatePassOn_yieldsPassBeforeBoardMoves`
- `movePickerMain_generatePassOn_afterLastMovePass_noSecondPass`
- `movePickerMain_ttMoveIsPass_yieldedOnceOnly`
- `movePickerMain_ttMoveIsPassButPassDisabled_ttMoveDropped`
- `movePickerMain_opponentHasFive_neverYieldsPass`
- `movePickerQvcf_forceAllowB4_enumeratesPlainB4`

VCN search (hand-built positions, via `YXVCN`)
- `vcnSearch_vc2/vc3/vc4_openThreeAttacker_*` — attacker mates (open four, then five)
- `vcnSearch_vc4_attackerWithoutVcf_loses`,
  `vcnSearch_vc3_attackerWithoutThreat_defenderPassRefutes`
- `vcnSearch_levelIsNotLeftSetAfterYxvcn_nextSearchIsOrdinary`

Commands
- `infoVcthread_guiValues_mapToVctAndVc2`, `infoVcthread_extensionValue_mapsToVc4`,
  `infoVcthread_vc5Value_rejectedBecauseVc5IsInternal`,
  `infoVcthread_outOfRange_rejectedAndKeepsPreviousLevel`, `infoVcthread_lowercaseAsSentByGui_isAccepted`
- `yxvcn_levelOutOfRange_errorsAndDoesNotSearch`, `yxvcn_vc5Level_errorsAndDoesNotSearch`, `yxvcn_afterInfoVcthread_restoresPersistentLevel`

## Verification

- Tests built with asserts live (`-UNDEBUG` on the test target, existing `rapfi_tests`).
- `rapfi bench` bit-identical vs baseline (feature inactive path): Search Bench Nodes `16733809`,
  Hash `4621a98a`.
- Manual: hand-built renju/freestyle positions with `YXVCN 2|3|4|5`.

### Verification results (2026-10-06, gcc, Release, AVX512)

1. **Asserts live.** `./build/rapfi_tests --check-asserts-live` aborts (exit 134).
2. **bench bit-identical.** Baseline before any change: Nodes `16733809`, Hash `4621a98a`. After the
   MovePicker/history/search changes, and again after the final root-dispatch fix: identical.
3. **Unit tests.** `./build/rapfi_tests` → all pass (see final count in the task summary).
4. **Manual (freestyle, `MAX_DEPTH 8`).** Black open three on row 7, `INFO VCTHREAD`/`YXVCN` levels 2, 3, 4 → `+M3`, bestmove `6,7`. A lone black stone: `YXVCN 4` → `-M2`, `YXVCN 3` → `-M4`, `YXVCN 2` → eval `179` (depth-limited, no proof).

### Findings during implementation
- **Root VC4 dispatch.** The reference sends every VC4 attacker node to `vcfsearch`, including the
  root, which then never enumerates root moves (`Eval VAL_NONE`). Fixed by excluding `RootNode`:
  a non-forcing root move is refuted by the defender's pass one ply below.
- **Reference bugs not carried over:** undefined `vcnLevel` / `vcnIsAttacker` / `vcnAllowB4`,
  database probing left on in VCN mode, history/statScore indexed with `Pos::PASS`.
- VC2 is depth-limited: with no forced win in range it reports a normal eval, not a proof.
