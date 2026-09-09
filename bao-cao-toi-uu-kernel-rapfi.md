# Báo cáo: Kiến trúc lõi (kernel) và dư địa tối ưu tốc độ engine Rapfi

*Ngày: 2026-09-09 · Phạm vi: `Rapfi/` (search, eval, game, pattern) · **Bản 3** — đã đối chiếu lại từng đề xuất với source*

---

## ⚠️ Bản 3 — đã double-check với source Rapfi

**Chưa sửa mã.** So với bản 1–2, bản này:

1. **Ràng buộc người dùng:** `eval/mix10nnue.*` **ngoài phạm vi** → hủy đề xuất memcpy accumulator.
   Evaluator NNUE trong phạm vi = **`eval/mix9svqnnue.cpp`** + tầng SIMD dùng chung `eval/simd/`.
   (Loại evaluator do config quyết định: `evalconfig.cpp:116` `"mix9svq"` / `:133` `"mix10"`; thư mục
   `Networks/` hiện **rỗng** nên không có mạng đóng gói mặc định.)
2. **Ràng buộc người dùng:** trọng tâm là **AVX512-VNNI**.
3. **3 đề xuất trong bản trước SAI, đã loại** (chi tiết ở §2):

| Đề xuất cũ | Vì sao sai (dẫn chứng source) |
|---|---|
| ❌ "`vpdpwssd` cho đường tuyến tính int16" | Không có `simd::linear<int16_t>` sống trên đường eval. `Affine<int16_t>` / `VecOp<int16_t>::dot2` (`_mm512_madd_epi16`, `vec.h:1284`) chỉ được `Affine<int16_t>::forward` gọi và **không call site nào** trong `mix9svqnnue.cpp` / `mix10nnue.cpp` truyền weight `int16_t`. Phần int16 trong accumulator mix9 là `mulhi`/`add`/`sub` element-wise (depthwise-conv) — VNNI không áp dụng. |
| ❌ "`pickNextMove<Best>` O(n²) selection sort" | `pickNextMove<Best>` **không được khởi tạo** — call duy nhất là `pickNextMove<Next>` (`movepick.cpp:556`). Nhánh `if constexpr (T == Best)` với `std::min_element` là **mã chết**. Move ordering thật dùng `fastPartialSort` (`movepick.cpp:72-97`) — đã thích ứng insertion / `std::sort` / `partial_sort` theo `nMoves`. |
| ❌ "`MovePicker::moves[MAX_MOVES]` = 226 phần tử (901 bàn 30)" | Sai số học: `BOARD_BOUNDARY = 5` → `MAX_BOARD_SIZE = 32 − 10 = 22` → `MAX_MOVES = 22·22 + 1 = 485` (cố định, **không** đổi theo kích thước bàn). `ScoredMove` = 8 byte → mảng = 3 880 byte. Vẫn là mục hợp lệ nhưng **nhỏ**. |
| ⚠️ "đổi `value_corner_down` sang `linear<...,false>` (u7i8)" *(có trong §3.3 bản 3a)* | **Sai** — `dotsum` (input tầng đó) **âm được**: `up2 = crelu<...,NoReLU=true>` (`mix9svqnnue.cpp:230`) không ReLU → có dấu; `dot2` (`activation.h:217`) giữ dấu qua `srai`+`packs`. u7i8 sẽ hiểu byte âm thành `+255`. → Thay bằng **mẹo lệch input `+128`** (bit-identical, §3.3). |

4. **AVX512-VNNI: đã bật và dùng đúng** cho tầng int8 chính. Khoảng trống duy nhất là đường **có dấu**
   `dot4_i8i8_accum` — sửa bằng mẹo lệch input `+128` (bit-identical), **không** phải đổi sang u7i8 (§3.3).

---

## 0. Tóm tắt điều hành (TL;DR)

Rapfi đã tối ưu **rất tốt ở tầng vi kiến trúc**: TT kiểu Stockfish lockless 12-byte/entry vừa khít cache line
(`hashtable.cpp:52-81`), prefetch TT trước `board.move()` (`search.cpp:1662-1664`), SIMD int8 với đường
`vpdpbusd` (`vec.h:1008`), cập nhật NNUE tăng dần có `multiPrefetch`, large-page cho TT, ràng buộc NUMA.
Dư địa **không nằm ở "viết lại cho nhanh"** mà ở vài điểm đòn bẩy:

| # | Cơ hội | Tầng | Ước lượng¹ | Rủi ro | Lớp CLAUDE.md |
|---|--------|------|-----------|--------|---------------|
| **1** | **PGO + LTO + `-march=native`/`-mtune=native`** cho binary (LTO đang tắt, PGO không có, `-march=native` chỉ dùng *dò* chứ không *biên dịch*) | Build | **+8–20% NPS** | Rất thấp | C |
| **2** | **AVX512-VNNI — bỏ "gấp đôi công" ở `dot4_i8i8_accum`** bằng **mẹo lệch input `+128`** (`in ^ 0x80`, gấp `−128·Σrow(w)` vào bias) → 1 `vpxor` + 1 `vpdpbusd` thay vì ~6 lệnh, **bit-identical**, mọi CPU VNNI. Live: `mix9svqnnue.cpp:237` `starBlock` `value_corner_down` (13×/value-eval). (`vpdpbssd` là bonus cho CPU mới.) | SIMD | +1–3% eval | Thấp | B |
| **3** | `checkForbiddenPoint` (đệ quy + `ScopedMove`) gọi **mỗi candidate** trong `pickNextMove` khi Renju-đen (`movepick.cpp:260-271`) | MovePick | +1–2% (chỉ Renju) | TB | B |
| **4** | `MovePicker::moves[485]` (~4KB) trên stack mỗi node dù chỉ chạm ~30–60 | MovePick | <1–2% NPS | Thấp | B |
| **5** | `positionComplexity()` (~12× đọc `p4Count` + 1 `logf`) tính ở **mọi** node kể cả node thoát sớm (`search.cpp:1371`) | Search | <0.5% | Thấp | B |
| **6** | Large-page cho trọng số `mix9svqnnue` + bảng lớn per-thread (hiện chỉ TT) | Eval/Mem | <1–2% (máy nhiều RAM) | Thấp | C |
| ~~—~~ | ~~accumulator `mix10nnue` memcpy toàn bàn~~ | — | — | **ngoài phạm vi** | — |

¹ % là **ngoại suy từ tài liệu** (Stockfish, GCC/Phoronix, WikiChip) — **chưa đo trên Rapfi**. §7 nêu cách đo.

**Việc số 0 (bắt buộc):** chạy `rapfi bench`, ghi `Nodes/s` + `Hash` vào ô baseline **còn trống** ở
`Rapfi/CLAUDE.md` §"Definition of done" mục 3.

---

## 1. Phương pháp

1. **Đọc source** (qua CodeGraph + đọc trực tiếp): `game/board.cpp` (`move`/`undo`), `search/ab/search.cpp`
   (alpha-beta 21 bước + `positionComplexity`), `search/movepick.{h,cpp}` (toàn bộ), `search/hashtable.cpp`,
   `eval/simd/{isa,linear,vec}.h`, `eval/mix9svqnnue.cpp`, `eval/mix10nnue.cpp` (đối chiếu — ngoài phạm vi),
   `eval/evalconfig.cpp`, `core/{pos.h,platform.h,types.h}`, `game/movegen.{h,cpp}`, `CMakeLists.txt`,
   `command/benchmark.cpp`, `Rapfi/CLAUDE.md`.
2. **Tra cứu Internet**: PGO/LTO/`-march`, AVX-512 VNNI `vpdpbusd`/`vpdpbssd`, Lazy SMP scaling, chi phí
   nhầm nhánh, nguyên tắc software prefetch. Trích dẫn ở §9.
3. **Đối chiếu** với Stockfish (engine mã nguồn mở tối ưu kỹ nhất, cùng họ kỹ thuật: alpha-beta + NNUE +
   Lazy SMP + TT lockless).

---

## 2. Bản đồ kiến trúc hiệu năng (đã xác minh)

### 2.1 Đo tốc độ — `command/benchmark.cpp`

```cpp
// benchmark.cpp:35  constexpr size_t TotalMoveTestNum = 2000000;
// benchmark.cpp:110  "Move Bench": chỉ Board::move()/undo(), 2M lần
// benchmark.cpp:139  "Search Bench": alpha-beta 1 luồng, TT 16MB, 10 vị trí, depth 19
```

Đầu ra: `Moves/s`, `Nodes/s`, và `Hash` (số node + eval gốc mỗi vị trí) — **mốc hồi quy** CLAUDE.md yêu cầu
giữ bit-identical trừ khi cố ý đổi hành vi. Ô baseline trong CLAUDE.md **đang trống**.

### 2.2 Nhân make/unmake — `game/board.cpp`

`Board::move<Rule,MoveType>` (`board.cpp:212`), đường nóng nhất ngoài eval:

- **Cửa sổ bitkey cuộn** (`board.cpp:262-348`): giữ `bitKey[4]` căn sẵn ở ô `i=-L`, dịch 2 bit/ô thay vì
  tính lại khóa dòng xoay từng ô.
- **LUT hợp nhất `PCodeP4`** (`game/pattern.h:159-169`): một lần nạp trả cả pattern code lẫn `Pattern4`.
- **`StateInfo` + `MoveRestore` LIFO** cho `undo()` rẻ; `undo()` phục hồi đúng ô đã đụng qua `touchedMask`
  (`board.cpp:439-455`), không quét lại `emptyBB`.
- Dispatch compile-time theo `Rule` × `MoveType` (`NORMAL`/`NO_EVAL`/`NO_EVAL_MULTI`) — hook evaluator biến
  mất khi không dùng (`board.cpp:387-392`).

### 2.3 Alpha-beta — `search/ab/search.cpp`

`search<Rule, NodeType>` — 21 bước đánh số. Heuristic ảnh hưởng tốc độ (ghi chú elo của tác giả trong mã):
razoring-VCF `~68` (bước 7), futility con `~121` (bước 8), null-move `~3` (bước 9), IIR `~3` (bước 10),
move-count pruning `~107` (bước 12), singular ext `~52`, LMR với LUT `reductions[RULE_NB]` (bước 15,
`parameter.h:227` `initReductionLUT`). `TT.prefetch(childKey)` đặt **trước** `board.move()`
(`search.cpp:1662-1664`) để chồng lấp truy cập bộ nhớ với pattern walk.

### 2.4 NNUE `mix9svqnnue` + SIMD (trong phạm vi)

- **Accumulator tăng dần**: `Evaluator::beforeMove`/`afterUndo` → `addCache`/`flushMoveCache`/`applyMoveCache`.
  Có `multiPrefetch` codebook (`mix9svqnnue.cpp:447-454`).
- **Kernel int8 linear** (`eval/simd/linear.h`): tích lũy int32, "weight scrambling" khi nạp
  (`preprocessLinear`, `linear.h:304`) để inference đọc tuần tự; xử lý **128 trọng số/vòng trong**
  (`linear.h:144`).
- **`starBlock`** (`mix9svqnnue.cpp:215-242`): 3 `simd::linear` — `value_corner_up1`/`up2` (`SignedInput=false`,
  u7×i8, **đã tối ưu VNNI**) + `value_corner_down` (**`SignedInput=true`**, i8×i8 → đường đắt, xem §3.3).
- **Dispatch ISA compile-time** (`eval/simd/isa.h:75-93`): AVX512 / AVX2 / SSE / NEON / WASM-SIMD.

### 2.5 AVX512-VNNI — hiện trạng (đã xác minh)

| Thành phần | Trạng thái |
|---|---|
| `vpdpbusd` (u8×i8) tầng int8 GEMM chính | ✅ **Dùng** — `vec.h:1005-1013` `_mm512_dpbusd_epi32` trong `dot4_u7i8_accum`, gate `USE_VNNI` |
| Dò + cờ | ✅ `CMakeLists.txt:49-66` `check_cxx_source_compiles(... _mm512_dpbusd_epi32 ...)` → `HAS_AVX512_VNNI`; `:576-579` phát `-mavx512vnni -mavx512vl` |
| Fallback CPU cũ | ✅ `maddubs`+`madd` (`vec.h:1010-1012`) |
| `dot4_i8i8_accum` (i8×i8, `vec.h:1017-1041`) | ⚠️ Tách `msb`/`low7`, chạy `vpdpbusd` **2 lần** + `and`/`andnot`/`sub`/`add` → khoảng trống, §3.3 |
| Biến thể 256-bit VL (`_mm256_dpbusd_epi32`) | ✅ Được `-mavx512vnni -mavx512vl` phủ (dùng khi kernel gọi `getInstTypeOfWidth(...,256)`) |

### 2.6 Transposition Table — `search/hashtable.cpp`

```cpp
// hashtable.cpp:52-81
struct TTEntry { uint32_t key32; union{ struct{ int16_t value16,eval16; uint16_t pvBoundBest16;
                                               uint8_t depth8,generation8; }; uint32_t data[2]; }; };  // 12 byte
struct TTBucket { TTEntry entry[5]; char _padding[4]; };   // 64 byte = 1 cache line
uint32_t key() const { return key32 ^ data[0] ^ data[1]; } // lockless XOR
```

Large-page alloc có suy giảm dần khi hết RAM (`:112-133`); xóa TT song song + bind NUMA (`:140-161`);
chỉ số bucket `mulhi64(key, numBuckets)` (`:171`).

### 2.7 Đa luồng — Lazy SMP

`search/searchthread.cpp`: pool worker, `pickNextDepth` (chọn depth ít luồng đang làm), `pickBestThread`.
History/killer/counter-move riêng theo luồng (`ABSearchData`, `searcher.h:44-45`); chỉ TT chia sẻ.
`Numa::bindThisThread`. Phù hợp lý thuyết Lazy SMP ([chessprogramming — Lazy SMP](https://chessprogramming.org/Lazy_SMP)).

### 2.8 Cấu hình biên dịch — `CMakeLists.txt`

```cmake
# :28-30  — -march=native CHỈ dùng cho check_cxx_source_compiles (DÒ tính năng), KHÔNG vào build thật
if(NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_REQUIRED_FLAGS "-march=native")
# :135  — LTO MẶC ĐỊNH TẮT
option(ENABLE_LTO "Enable link time optimization" OFF)
# :560-590 (nhánh GNU/Clang) — chỉ thêm cờ ISA cụ thể + tắt FMA để bench bit-identical:
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffp-contract=off")
if(USE_AVX512) ... -mavx512f -mavx512dq -mavx512bw (+ -mavx512vnni -mavx512vl nếu USE_VNNI)
elseif(USE_AVX2) ... -mavx2 -mfma (+ -mavxvnni nếu USE_VNNI)
```

**Không có**: PGO, `-mtune=native`, `-march=native` cho codegen, `-funroll-loops`. `CMAKE_BUILD_TYPE`
mặc định `Release` → GCC/Clang `-O3 -DNDEBUG`.

---

## 3. Dư địa cải thiện (chi tiết)

### 3.1 — Build: PGO + LTO + native tuning  ★ đòn bẩy lớn nhất, rủi ro thấp nhất

**Quan sát:** `-march=native` chỉ ở `CMAKE_REQUIRED_FLAGS` cho việc *dò* SSE/AVX (`CMakeLists.txt:29`);
binary thật chỉ nhận cờ ISA rời (`-mavx2 -mfma`…), **không** `-march=native`/`-mtune=native`. `ENABLE_LTO`
mặc định `OFF` (`:135`). Không preset/target PGO nào.

**Trích dẫn:**
- Wiki Stockfish: **`make profile-build`** là build chuẩn — *"standard build with profile-guided optimization"*
  (biên dịch 2 lần: instrument → chạy `bench` → biên dịch lại). Và *"To get the max speedup for your CPU
  (1.5% on Ivy Bridge) simply prepend `CXXFLAGS='-march=native'`"*
  ([Stockfish — Compiling from source](https://official-stockfish.github.io/docs/stockfish-wiki/Compiling-from-source.html)).
- PGO đặc biệt hợp engine cờ (mã dày rẽ nhánh khó đoán): nhầm nhánh **10–20 (tới 30) chu kỳ**; Skylake 16–17,
  Ice Lake 17–21, Zen1–2 ~19 ([Algorithmica — Cost of Branching](https://en.algorithmica.org/hpc/pipelining/branching/)).
  PGO cho compiler biết nhánh nóng → block layout, inline, đẩy đường lạnh ra xa.
- `-O3 + LTO + -march=native`: geomean **> 13%** trên EPYC vs `-O2`; riêng LTO ~+3–5%
  ([SUSE — GCC 14](https://documentation.suse.com/sbp/devel-tools/html/SBP-GCC-14/index.html);
  [Phoronix — GCC 11 opt levels/LTO](https://www.phoronix.com/review/gcc11-rocket-opts)).

**Đề xuất:**
1. Preset `*-Release-PGO` (2 bước): GCC `-fprofile-generate` → `pbrain-rapfi bench` → `-fprofile-use
   -fprofile-correction`; Clang `-fprofile-instr-generate` / `llvm-profdata merge` / `-fprofile-instr-use`.
2. `ENABLE_LTO` mặc định `ON` cho preset Release (giữ `OFF` cho Debug). CMake đã có `check_ipo_supported`
   (`CMakeLists.txt` nhánh `if(ENABLE_LTO)`).
3. Option `USE_NATIVE` → thêm `-march=native -mtune=native` (build máy-đích, không thay preset ISA phân phối).

**Rủi ro:** PGO/LTO **không** đổi số học nếu giữ `-ffp-contract=off` → bench hash phải giữ nguyên (kiểm).
`-march=native` có thể đổi thứ tự rút gọn float ở value head → **đo lại hash**, coi là build riêng cho máy.

**Cách đo:** `rapfi bench` Nodes/s trung vị 7 lần, cùng máy, cố định tần số CPU.

---

### 3.2 — ~~Accumulator NNUE memcpy toàn bàn~~ → HỦY (ngoài phạm vi)

Đề xuất cũ nhắm `Accumulator::move()` trong `mix10nnue.cpp` (`std::copy_n` toàn bảng chỉ số theo ô mỗi node,
~2KB, chỉ để ghi đè ≤44 phần tử). **`mix10nnue` ngoài phạm vi → hủy.** Nếu muốn cải thiện tương tự trong
phạm vi: cần kiểm `eval/mix9svqnnue.cpp` có cùng mẫu "version table copy-on-write" không (chưa xác minh; §6 chạm tới).

---

### 3.3 — AVX512-VNNI: bỏ "gấp đôi công" ở `dot4_i8i8_accum`  ★ trọng tâm

**Đường có dấu i8×i8** (`eval/simd/vec.h:1017-1041`):

```cpp
R msb  = _mm512_and_si512(a, _mm512_set1_epi8(0x80));       // and
R low7 = _mm512_andnot_si512(_mm512_set1_epi8(0x80), a);    // andnot
#if defined(USE_VNNI)
    msb  = _mm512_dpbusd_epi32(_mm512_setzero_si512(), msb, b);   // vpdpbusd #1
    low7 = _mm512_dpbusd_epi32(_mm512_setzero_si512(), low7, b);  // vpdpbusd #2
#endif
acc = _mm512_add_epi32(acc, _mm512_sub_epi32(low7, msb));   // sub + add
```

→ ~6 µop cho bước lẽ ra 1 lệnh, vì `vpdpbusd` chỉ nhận **u8**×i8 nên input có dấu phải tách bit 7.

**Call site sống (đã xác minh):** `starBlock` (`mix9svqnnue.cpp:237`):

```cpp
simd::linear<OutSize, OutSize, /*SignedInput=*/true>(outputi32, dotsum,
                                                     w.value_corner_down.weight,
                                                     w.value_corner_down.bias);
```

→ `Affine<int8_t>::forward<SignedInput=true>` (`linear.h:157-162`) → `dot4_i8i8_accum`. `starBlock` gọi
**13 lần / một lần eval value** (`mix9svqnnue.cpp:581-591` = 9, `:619-628` = 4). Các tầng lớn `FeatureDim`
dùng `SignedInput=false` (u7) → **đã** tối ưu VNNI.

**⚠️ `linear<...,false>` (u7i8) KHÔNG cho kết quả đúng ở đây — đã xác minh.** `dotsum` là **có dấu, âm được**:
- `up1 = crelu<...,128>` → có ReLU → `[0,127]` (u7). ✓
- `up2 = crelu<...,128, /*NoReLU=*/true>` (`mix9svqnnue.cpp:230`) → **tham số thứ 3 `true` tắt ReLU** →
  `up2 ∈ [-128,127]`, **có dấu** (đây chính là cổng nhân "star"/gating — cần dấu để đảo được).
- `dotsum = dot2(up1, up2)` (`activation.h:217-241`): `maddubs(u7, i8)` → int16 **âm được** → `srai` (dịch
  số học, giữ dấu) → `packs` (pack bão hòa về int8 **có dấu** `[-128,127]`).

→ Nếu đổi sang `dot4_u7i8_accum`, byte `dotsum = -1` (0xFF) bị hiểu là `+255` → sai lệch `256·w` mỗi lane âm.
**Eval hỏng, bench hash đổi.** Hướng "chuyển sang u7i8" trong bản trước **sai** — bỏ.

**Trích dẫn:**
- `VPDPBUSD` chạy port 0 & 5, 1 lệnh/chu kỳ; VNNI cải thiện INT8 inference **~2–3×** vs `maddubs+madd`
  ([WikiChip — AVX-512 VNNI](https://en.wikichip.org/wiki/x86/avx512_vnni);
  [OpenGenus — AVX512 VNNI 2X](https://iq.opengenus.org/avx512-vnni/)).
- Stockfish giữ **mọi activation không dấu** `[0,127]` (ClippedReLU) để `vpdpbusd` chạy trực tiếp, không cần
  input có dấu ([nnue-pytorch docs](https://github.com/official-stockfish/nnue-pytorch/blob/master/docs/nnue.md)).
  Rapfi cố tình dùng `up2` có dấu → không tránh được input có dấu ở tầng `value_corner_down`.
- `vpdpbssd` (i8×i8 trực tiếp) thuộc **AVX-VNNI-INT8** — tập lệnh *riêng*, có trên Sapphire Rapids/AVX10.2,
  Zen5 một phần; **không** đảm bảo trên mọi CPU có AVX512-VNNI
  ([Intel Community](https://community.intel.com/t5/Mobile-and-Desktop-Processors/Does-AVX-512-VNNI-has-vpdpbssd/td-p/1668218)).

**Hai hướng (độc lập, cả hai BIT-IDENTICAL):**

1. **★ Mẹo lệch input `+128` — mọi CPU có `vpdpbusd`, không cần tập lệnh mới, bit-identical.**
   Đồng nhất thức: `u_i = s_i XOR 0x80 = s_i + 128 ∈ [0,255]` (đúng cho mọi byte có dấu). Khi đó
   `vpdpbusd(acc, u, w) = Σ(s_i + 128)·w_i = Σ s_i·w_i + 128·Σ w_i`. Gấp `−128·Σ_row(w)` vào `bias[row]`
   lúc nạp trọng số → runtime chỉ còn **1 `vpxor` (khấu hao 1 lần/chunk input) + 1 `vpdpbusd`**, thay cho
   `vpand + vpandn + 2×vpdpbusd + vpsubd + vpaddd`.
   - Khả thi: `Mix9svqWeightLoader::preprocess(StarBlockWeight&)` (`mix9svqnnue.cpp:201-206`) đang có sẵn cả
     `weight` lẫn `bias` trong tay — tính tổng hàng của `value_corner_down.weight` **trước** khi
     `preprocessLinear` xáo trộn, rồi `bias[row] -= 128 * rowsum[row]`.
   - Tràn số: `Σ u·w`, `u∈[0,255]`, `w∈[-128,127]`, `InSize≈ValueDim` → ~2.1M, thừa headroom int32 (giống
     đúng như đường 2×`vpdpbusd` hiện tại). Số nguyên chính xác → **bench hash không đổi**.
   - Cần thêm cờ template/hàm: một biến thể `dot4_i8i8_accum` "input đã lệch" hoặc xử lý `SignedInput` ở
     `Affine::forward` bằng cách XOR `in0` một lần/chunk rồi gọi đúng đường `u7i8`.
2. **`vpdpbssd` — chỉ CPU rất mới.** Dò `HAS_AVX512_VNNI_INT8` (thử biên dịch `_mm512_dpbssd_epi32`) →
   `USE_VNNI_INT8` + `-mavx512vnniint8`/`-mavxvnniint8`. Trong `dot4_i8i8_accum`: `#if defined(USE_VNNI_INT8)`
   → `acc = _mm512_dpbssd_epi32(acc, a, b);`. Bit-identical. **Hướng 1 đã phủ hầu hết lợi ích trên nhiều CPU
   hơn → hướng 2 chỉ là bonus.**

**Phụ — kiểm chứng disasm:** nhánh `USE_AVX512` **không** thêm `-mavxvnni`, nhưng `vec.h` khi `USE_AVX512`
dùng biến thể VL `_mm256_dpbusd_epi32` (được `-mavx512vnni -mavx512vl` phủ). Vẫn nên chạy một lần
`objdump -d pbrain-rapfi | grep -c vpdpbusd` để chắc không kernel nào rơi về `vpmaddubsw`.

**Cách đo:** `rapfi bench` "Search Bench" Nodes/s + `Hash` (phải giữ nguyên); `perf stat instructions,uops`;
`perf annotate mix9svq...evaluateValue`; đếm `vpdpbusd` trong disasm trước/sau.

---

#### 3.3.1 — QUYẾT ĐỊNH: mẹo lệch `+128` — **AN TOÀN, rủi ro thấp** (nên làm)

**Ý tưởng: an toàn tuyệt đối** — toán số nguyên chính xác, không làm tròn:

| Kiểm | Kết quả (đã xác minh với source) |
|---|---|
| `s XOR 0x80 == s + 128`? | ✅ Đúng cho **cả hai** khoảng: `s∈[0,127]` (bit7=0, `\|0x80` = +128) và `s∈[-128,-1]` (bit7=1, `&0x7F` = −128 khỏi dạng bù-2 = +128 theo giá trị) |
| Đồng nhất thức `Σ(s+128)w = Σsw + 128Σw`? | ✅ Đại số thuần, số nguyên, không tràn trung gian |
| Tràn int32? | ✅ `value_corner_down` = `FCWeight<64,64>` (`ValueDim=64`). `max\|Σ u·w\| = 255·128·64 ≈ 2.09M` — biên **~1000×** so với `INT32_MAX`. Fold bias `128·Σrow(w) ≤ 128·8192 ≈ 1.05M`, `bias` là `int32_t` (`mixcommon.h:77`). |
| Bench `Hash`? | ✅ **Bit-identical** — cùng giá trị `outputi32`, cùng `crelu`. Đây là bài kiểm nghiệm bắt buộc. |

**Rủi ro còn lại — đều là rủi ro *triển khai*, khoanh vùng được:**

1. **Đụng `mix10` (ngoài phạm vi).** Nếu sửa thẳng `dot4_i8i8_accum` trong `vec.h` thì `mix10`
   (`linearBlock<true>` gọi nhiều lần) cũng đổi đường và **vỡ** nếu weight-loader của `mix10` không fold bias
   theo. → **Bắt buộc làm dạng opt-in:** thêm cờ template mới (vd `PreBiasedInput`) / hàm riêng, chỉ nối cho
   `mix9svqnnue`'s `value_corner_down`. **Không** đổi ngữ nghĩa `dot4_i8i8_accum` hiện có.
2. **Thứ tự trong `preprocess()`.** `rowsum` phải tính **trước** `simd::preprocessLinear` xáo trộn weight
   (`mix9svqnnue.cpp:205`). Sai thứ tự → hash đổi → bắt được ngay ở cổng bench.
3. **Fallback không-VNNI.** Input đã lệch `u∈[0,255]` sẽ **bão hòa `maddubs`** (u8·i8, cộng 2 → tràn int16).
   → nhánh lệch chỉ dùng khi `#if defined(USE_VNNI)`; `#else` giữ nguyên đường tách msb/low7 hiện tại.
4. Thêm `static_assert(255LL * 128 * InSize < INT32_MAX)` để chốt biên tràn cho tương lai (nếu `ValueDim` tăng).

**Phân loại CLAUDE.md: lớp B** (cách hiện thực eval; bias dẫn xuất lúc nạp, **không** phải encoding bền vững,
không bảng compile-time, không bản sắc vị trí). Cần: decision note + đo bench. **Không** phải lớp A.

**Cổng chấp nhận:** `rapfi bench` `Hash` **bit-identical**; nếu đổi → lỗi triển khai (thường là #2), revert.

**Lợi ích thực tế: khiêm tốn.** `value_corner_down` chỉ 64×64, 13×/lần eval value; eval value là một phần
nhỏ chi phí node (policy eval + movegen + make/unmake chiếm phần lớn). Ước lượng **~0.5–2% NPS**. Giá trị
chính là **không có rủi ro đúng-sai** — làm được mà không sợ hỏng eval.

**→ Kết luận: NÊN LÀM**, dạng opt-in mix9-only, gate bench-hash-identical. Rủi ro: **thấp**.

---

### 3.4 — `checkForbiddenPoint` gọi mỗi candidate trong `pickNextMove` (Renju)

**Quan sát mã:**

```cpp
// movepick.cpp:258-271  — pickNextMove<Next> (dạng DUY NHẤT được dùng, call ở :556)
while (curMove < endMove) {
    ...
    const bool isForbidden = forbidden && board.checkForbiddenPoint(curMove->pos);  // mỗi candidate
    const bool accepted    = !isTtMove && !isForbidden && filter();
    ...
}
```

`forbidden = (rule == RENJU && sideToMove() == BLACK)`. `checkForbiddenPoint` (`board.cpp:474`) có thể
**đệ quy** khi kiểm double-three (`board.cpp:522` `!checkForbiddenPoint(posi)`) + mở/đóng `ScopedMove` probe
(đặt/gỡ quân thật, cập nhật pattern). Với Renju-đen mọi candidate qua vòng này; cùng một cell còn bị kiểm lại
qua các lần re-search (LMR fail-high, PVS) trong cùng node.

**Đề xuất:**
- Cache tri-state theo node/ply: `Pos → {chưa biết, cấm, không cấm}` (mảng `MAX_BOARD_SIZE²` hoặc `flat_map`
  nhỏ trong `SearchStack`), xoá khi vào node — tránh kiểm lặp một cell trong các lần re-search.
- Hoặc cache LRU per-thread khoá `(zobristKey ^ LCHash(pos))` (giống `dbClient` cache).
- Tách phần **rẻ, không đệ quy** (overline / double-four xác nhận trực tiếp, `board.cpp:485-495`) đánh giá sớm;
  hoãn nhánh double-three (có `ScopedMove` + đệ quy) tới khi nước được chọn thật.

**Rủi ro:** trung bình. Cache sai = sinh nước bất hợp lệ (lớp B, chạm C nếu hỏng). Cần: test vị trí Renju
hand-crafted (CLAUDE.md §"Verification without a UI" mục 2) + bench hash bit-identical cho 3 vị trí RENJU +
self-play regression Renju.

**Cách đo:** `perf annotate` — tỉ lệ thời gian trong `checkForbiddenPoint` khi chạy 3 vị trí RENJU của bench;
nếu < 2% thì hạ ưu tiên.

---

### 3.5 — `MovePicker::moves[485]` trên stack mỗi node (mục nhỏ)

**Số liệu (đã sửa cho đúng):**

```
core/pos.h:36-38 : BOARD_BOUNDARY = 5 → MAX_BOARD_SIZE = 32 − 10 = 22 → MAX_MOVES = 22·22 + 1 = 485
game/movegen.h:57-64 : ScoredMove = {Pos(int16); Score(int16); union{Score; float}} = 8 byte
→ movepick.h:110  ScoredMove moves[MAX_MOVES]  = 485 × 8 = 3 880 byte, cố định (không đổi theo bàn)
```

`MovePicker` cấp phát ở **mọi** node AB (`search.cpp:1384`), nhưng `generate<ALL>` chỉ điền ~30–60 phần tử
(giới hạn `candArea`). `fastPartialSort` dùng `InsertionSortLimit = MAX_MOVES/4 = 121` (`movepick.cpp:75`) →
với ~40 nước luôn rơi insertion sort (tốt).

**Vì sao chỉ là mục nhỏ:** phần **chạm tới** mỗi node nhỏ; ~3.8KB stack chưa-ghi gần như miễn phí trên x86.
Chi phí thực: RSP tụt ~4–5KB/ply đệ quy → sau ~25 ply frame gốc cách ~120KB (ra khỏi L1, còn trong L2). Marginal.
Stockfish `moves[256]` (~1KB) vì cờ vua hiếm khi >40 nước
([ARM — Memory latency & cache](https://learn.arm.com/learning-paths/cross-platform/memory-latency/latency-and-cache-prefetching/)).

**Đề xuất:** với `CandidateRange != FULL_BOARD` dùng buffer per-thread trong `SearchThread` (con trỏ
`ScoredMove *moves`) kích thước theo `board.cellCount()`; hoặc giảm hằng số riêng cho `MovePicker` khi range hẹp.

**Rủi ro:** thấp, lớp B, bench bit-identical. Chú ý nhánh `FULL_BOARD` và bàn 20.

**Cách đo:** "Search Bench" Nodes/s; `perf stat -e page-faults,L1-dcache-load-misses,dTLB-load-misses`.

---

### 3.6 — `positionComplexity()` tính ở mọi node (mục nhỏ)

**Thân hàm (đã đọc):**

```cpp
// search.cpp:975-984
inline float positionComplexity(const Board &board, Color self, Color oppo) {
    uint16_t complexCount = 1;
    for (int pat4 = L_FLEX2; pat4 <= D_BLOCK4_PLUS; pat4++) {   // ~12 lần
        complexCount += board.p4Count(self, (Pattern4)pat4);    // đọc stateInfo().p4Count[] — nóng, L1
        complexCount += board.p4Count(oppo, (Pattern4)pat4);
    }
    return ::logf(complexCount);                                // 1 logf ~ 10-40 chu kỳ
}
```

Gọi ở `search.cpp:1371` (sau `moves_loop:`), dùng bởi LMR (`search.cpp:1684`). **Không** quét `candArea`
(bản trước viết sai) — chỉ ~24 lần đọc mảng `p4Count` (trong L1 vì `stateInfo` cực nóng) + **1 `logf`**.
Node thoát ngay ở bước 12 vẫn trả tiền cho `logf`.

**Đề xuất:** `std::optional<float>` / lambda lười — chỉ tính lần đầu LMR cần (`depth >= 2 && moveCount > 1 +
RootNode`, `search.cpp:1669`). Hoặc thay `logf` bằng LUT nhỏ (`complexCount` chỉ tới ~vài chục).

**Rủi ro:** rất thấp. Bản lười thuần: bit-identical. Bản LUT: đổi số → đo hash.

**Cách đo:** `perf annotate` — nếu `positionComplexity` (chủ yếu `logf`) < 0.5% thì bỏ qua.

---

### 3.7 — Large-page cho trọng số `mix9svqnnue` + bảng lớn per-thread

**Quan sát:** chỉ TT dùng `alignedLargePageAlloc` (`hashtable.cpp:115`). Trọng số evaluator + bảng lớn
per-thread trong accumulator cấp phát trang thường.

**Trích dẫn:** trọng số NNUE vài MB, truy cập ngẫu nhiên theo chỉ số mapping → nhạy TLB. Huge pages (2MB)
giảm TLB miss cho vùng nóng lớn (lý do Rapfi đã dùng large page cho TT)
([ARM — cache prefetching](https://learn.arm.com/learning-paths/cross-platform/memory-latency/latency-and-cache-prefetching/)).

**Đề xuất:** nạp trọng số `mix9svqnnue` qua `MemAlloc::make_unique_large_page` (helper sẵn có, `platform.h:283`).
Bảng per-thread bị ghi liên tục → lợi ích TLB rõ hơn ở bàn lớn / nhiều luồng.

**Rủi ro:** thấp; fallback trang thường khi OS từ chối (helper đã xử lý). Bench bit-identical.
**Lưu ý phạm vi:** nếu weight-loader / accumulator dùng chung giữa `mix9` và `mix10` thì cần khoanh vùng
phần dùng chung trước khi sửa.

---

### 3.8 — Các điểm đã tốt (ghi nhận, không đổi)

- **TT** — 1 cache line/probe, prefetch từ xa, lockless XOR, large-page, xóa song song. Chuẩn Stockfish.
- **Move ordering** — `fastPartialSort` (`movepick.cpp:72-97`) đã thích ứng (insertion / `std::sort` /
  `partial_sort` theo `nMoves`). Không cần đụng.
- **Lazy SMP** — TT chia sẻ, history riêng, `pickNextDepth`. Gợi ý *đo* scaling ≥16 luồng và **tăng TT mặc
  định** nếu nhiều luồng (*"when the TT is not large enough, multi-core searches can suffer more… due to hash
  conflicts"* — [chessprogramming — Lazy SMP](https://chessprogramming.org/Lazy_SMP)).
- **`-ffp-contract=off` toàn cục** — cố tình, để bench bit-identical GCC↔Clang. Chi phí: value/policy head
  (float) mất FMA. **Không khuyến nghị** đụng (phá nguyên tắc hồi quy).
- **`search<Rule,NT>`** instantiate 9 bản (3 luật × 3 node type) + vcf → I-cache lớn (đánh đổi có chủ ý: bỏ
  nhánh `rule` khỏi inner loop). PGO (§3.1) sẽ bố cục lại phần lạnh — thêm lý do cho §3.1.

---

## 4. Bảng tổng hợp & thứ tự thực hiện

> `mix10nnue` ngoài phạm vi. AVX512-VNNI đã bật; khoảng trống duy nhất = `dot4_i8i8_accum` (mục #3).

| Ưu tiên | Việc | Nỗ lực | Kỳ vọng | Lớp | Bench hash |
|---|---|---|---|---|---|
| **1** | Preset PGO + bật LTO cho Release | Nhỏ (CMake) | +8–20% NPS | C | giữ nguyên (kiểm) |
| **2** | Option `USE_NATIVE` (`-march=native -mtune=native`) build máy-đích | Nhỏ | +1–5% | C | có thể đổi → build riêng |
| **3** | **AVX512-VNNI**: `dot4_i8i8_accum` — **mẹo lệch input `+128`** + bias-fold ở `preprocess(StarBlockWeight&)` | TB | +1–3% eval | B | **bit-identical** |
| **4** | **AVX512-VNNI**: `dot4_i8i8_accum` — `vpdpbssd` khi CPU hỗ trợ (bonus, sau mục #3) | Nhỏ | phần dư | B | bit-identical |
| **5** | `objdump`/`perf annotate` xác nhận `vpdpbusd` trong vòng nóng eval | Nhỏ | dọn/xác nhận | — | không đổi |
| **6** | Cache `checkForbiddenPoint` / hoãn double-three (Renju) | TB | +1–2% (Renju) | B | kiểm 3 vị trí RENJU + self-play |
| **7** | `MovePicker::moves[]` buffer theo candidate thực | Nhỏ–TB | <1–2% NPS | B | bit-identical |
| **8** | Lazy `positionComplexity()` | Nhỏ | <0.5% | B | bit-identical |
| **9** | Large-page trọng số `mix9svqnnue` | Nhỏ | <1–2% | C | bit-identical |

**Quy trình mỗi mục** (CLAUDE.md §"Definition of done"): (1) liệt kê hàm→test case trong `docs/todo/<CODE>-*.md`
*trước*; (2) test đơn vị behavior-named, build `-UNDEBUG`; (3) `rapfi bench` trước/sau — hash bit-identical
hoặc lý do đã ghi; (4) lớp A/B đổi hành vi: thêm self-play regression + vị trí hand-crafted.

---

## 5. Điểm vào source (đã xác minh)

| Chủ đề | File:line |
|---|---|
| Make/unmake | `game/board.cpp:212` (`move`), `:394` (`undo`), `:262-348` (bitkey cuộn) |
| LUT hợp nhất PCodeP4 | `game/pattern.h:159-169` |
| Alpha-beta | `search/ab/search.cpp` — Step 1 ở `:1169`, Step 21 (TT store) ở `:1946` |
| `positionComplexity` | `search/ab/search.cpp:975-984` (thân), gọi ở `:1371` |
| LMR LUT | `search/ab/parameter.h:227` (`initReductionLUT`) |
| MovePicker — stage machine | `search/movepick.cpp:472-557` (`operator()`) |
| MovePicker — chọn nước | `search/movepick.cpp:258-305` (`pickNextMove<Next>`; `<Best>` là mã chết) |
| MovePicker — sort | `search/movepick.cpp:72-97` (`fastPartialSort`, thích ứng) |
| `checkForbiddenPoint` (đệ quy) | `game/board.cpp:474-554` |
| `MAX_MOVES` = 485 | `core/pos.h:36-38` (`BOARD_BOUNDARY=5`, `MAX_BOARD_SIZE=22`) |
| `ScoredMove` = 8 byte | `game/movegen.h:57-89` (`Pos` int16 `pos.h:83`, `Score` int16 `types.h:28`) |
| **VNNI int8 `vpdpbusd`** | `eval/simd/vec.h:882-928` (SSE), `:931-988` (AVX2), `:991-1041` (**AVX512**) |
| **`dot4_i8i8_accum` (đường đắt)** | `eval/simd/vec.h:1017-1041` |
| Call site i8i8 sống | `eval/mix9svqnnue.cpp:237` (`starBlock` → `value_corner_down`), gọi 13×/eval (`:581-591`, `:619-628`) |
| int8 Affine (`SignedInput`) | `eval/simd/linear.h:105-180` (`:157-162` nhánh signed) |
| int16 Affine — **mã chết trên eval** | `eval/simd/linear.h:236-286`, `eval/simd/vec.h:1284` (`_mm512_madd_epi16`) |
| CMake dò/bật VNNI | `CMakeLists.txt:49-66` (`HAS_AVX512_VNNI`), `:576-579` (`-mavx512vnni -mavx512vl`) |
| Chọn evaluator | `eval/evalconfig.cpp:116` (`"mix9svq"`), `:133` (`"mix10"`) |
| Transposition table | `search/hashtable.cpp:52-81` (layout), `:179` (`probe`), `:214` (`store`) |
| Prefetch TT | `search/ab/search.cpp:1313` (null move), `:1662-1664` (trước `move`) |
| Large-page alloc | `core/platform.h:253-291`, `search/hashtable.cpp:115` |
| Cờ biên dịch | `CMakeLists.txt:28-30` (dò), `:135` (LTO OFF), `:560-590` (GNU/Clang) |
| Benchmark | `command/benchmark.cpp:35,110,139` |

---

## 6. Cảnh báo phạm vi

- **Khảo sát + nghiên cứu, chưa sửa mã, chưa đo trên Rapfi.** Mọi % ngoại suy từ tài liệu Stockfish/GCC/CPU —
  engine Gomoku có phân bố node và cường độ eval khác cờ vua → **phải benchmark thực tế**.
- CLAUDE.md: heuristic search/eval = **lớp B** (decision note + đo); mã hóa pattern/accumulator = **lớp A**.
- **`eval/mix10nnue.*` ngoài phạm vi.**
- Repo **không phải git** (`Is a git repository: false`) — cân nhắc `git init` để so bench giữa các commit.

---

## 7. Kết luận

Rapfi không có "quả treo thấp" thuật toán — đường nóng viết ở trình độ engine hạng nhất.

**Đòn bẩy lớn nhất, an toàn nhất: tầng build** (PGO + LTO + native tuning) — hai chữ số phần trăm NPS mà không
đụng dòng mã engine, giữ hành vi (giữ `-ffp-contract=off`).

**AVX512-VNNI (trọng tâm người dùng):** đã bật và dùng đúng cho tầng int8 GEMM chính (`vpdpbusd`,
`vec.h:1008`). Khoảng trống **duy nhất** là `dot4_i8i8_accum` chạy `vpdpbusd` **2 lần** cho input **có dấu** —
live qua `mix9svqnnue.cpp:237` `starBlock` `value_corner_down` (13×/value-eval). Input `dotsum` **thật sự âm
được** (đã xác minh: `up2` không ReLU + `dot2` sign-preserving) → **không** thể đổi sang `u7i8` trực tiếp.
Lối đúng: **mẹo lệch input `+128`** (`in ^ 0x80` + gấp `−128·Σrow(w)` vào bias lúc nạp) → 1 `vpxor` + 1
`vpdpbusd`, **bit-identical**, chạy trên **mọi** CPU có `vpdpbusd`. `vpdpbssd` (VNNI-INT8) chỉ là bonus cho
CPU rất mới. Việc "hỗ trợ AVX512-VNNI" thực chất **đã xong ~90%**.

Điểm mã ngoài SIMD đáng theo đuổi tiếp: cache **`checkForbiddenPoint`** cho Renju (§3.4). Phần còn lại là tinh
chỉnh biên (< 1–2% mỗi mục).

Ba đề xuất trong bản trước (`vpdpwssd` int16, `pickNextMove<Best>` O(n²), số liệu `MAX_MOVES`) đã bị loại/sửa
sau khi đối chiếu source — xem hộp ⚠️ đầu báo cáo.

---

## 8. Giao thức benchmark đề xuất

```bash
# Build baseline
cmake --preset <preset-Release>   &&   cmake --build build/... -j

# Cố định môi trường
sudo cpupower frequency-set -g performance      # ổn định tần số
# đóng ứng dụng nền

# Đo: 7 lần, bỏ min/max, lấy trung vị
for i in $(seq 7); do ./build/.../pbrain-rapfi bench 2>&1 | grep -E "Nodes/s|Hash"; done

# Tìm hot function trước khi tối ưu điểm
perf record -g ./build/.../pbrain-rapfi bench   &&   perf report --stdio | head -50
perf stat -e cycles,instructions,branch-misses,L1-dcache-load-misses,dTLB-load-misses \
    ./build/.../pbrain-rapfi bench

# Riêng cho §3.3 (VNNI)
objdump -d ./build/.../pbrain-rapfi | grep -c 'vpdpbusd'
perf annotate -s '_ZN...mix9svq...evaluateValue...'   # xác nhận vpdpbusd trong vòng nóng
```

Tiêu chí chấp nhận: **Nodes/s trung vị tăng ngoài nhiễu ±1%** và **Hash giữ nguyên** (hoặc Hash đổi đã được
giải thích + self-play regression cho thay đổi lớp A/B đổi hành vi).

---

## 9. Tài liệu tham khảo

**Build / trình biên dịch:**
- [Stockfish — Compiling from source](https://official-stockfish.github.io/docs/stockfish-wiki/Compiling-from-source.html) — `make profile-build`; `-march=native`.
- [Stockfish issue #2917 — network for profile-build to optimize NNUE code](https://github.com/official-stockfish/Stockfish/issues/2917).
- [SUSE — Advanced Optimization and New Capabilities of GCC 14](https://documentation.suse.com/sbp/devel-tools/html/SBP-GCC-14/index.html) — `-O3 + LTO + -march=native` geomean > 13% (EPYC).
- [Phoronix — GCC 11 Compiler Performance: Optimization Levels, LTO](https://www.phoronix.com/review/gcc11-rocket-opts).
- [Phoronix — A Fresh Look At PGO Performance With GCC 8](https://www.phoronix.com/review/gcc-82-pgo/3) — Stockfish hưởng lợi PGO rõ hơn Crafty.
- [TechnoLynx — GCC Optimization Flags Explained: -O2, -O3, -march](https://www.technolynx.com/post/gcc-optimization-flags-explained-o2-o3-march-and-what-they-mean/) — `-march` (bật ISA) vs `-mtune` (lịch trình).
- [arXiv 2507.16649 — From Profiling to Optimization: Unveiling PGO](https://arxiv.org/pdf/2507.16649).

**SIMD / VNNI / INT8:**
- [WikiChip — AVX-512 VNNI](https://en.wikichip.org/wiki/x86/avx512_vnni) — `VPDPBUSD` u8×i8→i32, port 0 & 5.
- [OpenGenus — AVX512 VNNI boosts ML performance by 2X](https://iq.opengenus.org/avx512-vnni/) — INT8 inference ~3×.
- [Intel — Deep Learning with AVX-512 and Intel DL Boost](https://www.intel.com/content/www/us/en/developer/articles/guide/deep-learning-with-avx512-and-dl-boost.html).
- [Intel Community — Does AVX-512 VNNI have vpdpbssd?](https://community.intel.com/t5/Mobile-and-Desktop-Processors/Does-AVX-512-VNNI-has-vpdpbssd/td-p/1668218) — `vpdpbssd` (i8×i8) thuộc AVX-VNNI-INT8 / AVX10, không có trong AVX-512 VNNI đời đầu.
- [chessprogramming — AVX-512](https://chessprogramming.org/AVX-512).

**Cache / prefetch / branch:**
- [Algorithmica — Prefetching](https://en.algorithmica.org/hpc/cpu-cache/prefetching/) — độ trễ RAM ~100ns; prefetch quá sớm bị evict.
- [Algorithmica — The Cost of Branching](https://en.algorithmica.org/hpc/pipelining/branching/) — nhầm nhánh 10–30 chu kỳ.
- [johnnysswlab — How branches influence performance](https://johnnysswlab.com/how-branches-influence-the-performance-of-your-code-and-what-can-you-do-about-it/).
- [ARM Learn — Memory latency: latency & cache prefetching](https://learn.arm.com/learning-paths/cross-platform/memory-latency/latency-and-cache-prefetching/).
- [HPI — Fetch Me If You Can: Evaluating CPU Cache Prefetching (DaMoN'25)](https://hpi.de/oldsite/fileadmin/user_upload/fachgebiete/rabl/publications/2025/Mahling-DaMoN25-Prefetching.pdf).

**Đa luồng:**
- [chessprogramming — Lazy SMP](https://chessprogramming.org/Lazy_SMP) — scaling 8+ core; TT nhỏ → multi-core bị hại vì hash conflict.
- [TalkChess — Better NPS scaling for Stockfish](https://talkchess.com/viewtopic.php?t=55494); [TalkChess — Best Stockfish NPS scaling yet](https://talkchess.com/viewtopic.php?t=55536&start=10).
- [chessify — NPS: Nodes per Second in Chess Engine Analysis](https://chessify.me/blog/nps-what-are-the-nodes-per-second-in-chess-engine-analysis).

**Source Rapfi được trích (bản trên đĩa 2026-09-09):** `game/board.cpp`, `game/pattern.h`, `game/movegen.h`,
`search/ab/search.cpp`, `search/ab/parameter.h`, `search/movepick.{h,cpp}`, `search/hashtable.cpp`,
`eval/mix9svqnnue.cpp`, `eval/mix10nnue.cpp` (đối chiếu), `eval/evalconfig.cpp`,
`eval/simd/{isa,linear,vec}.h`, `core/{pos.h,platform.h,types.h}`, `command/benchmark.cpp`, `CMakeLists.txt`,
`Rapfi/CLAUDE.md`.
