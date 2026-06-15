# 053 — INC-050 CRC32C 硬件 inline（方向 B）

> 范围冻结：只做 INC-050 的**方向 B**（header-only inline 硬件 CRC32C，替换
> 9 处 absl 调用点）。方向 A（leaf CRC 下放 worker）**不在本步**，理由见 §11。
> 本步不改任何磁盘格式、不改任何 handle 签名、不改任何 struct 字段；唯一可观察
> 变化是 CRC 计算更快，产出的 CRC 值**逐位不变**。

---

## 0. 角色分工（沿用 052 §0 纪律）

| 范围 | 负责方 | 纪律 |
|---|---|---|
| `apps/inconel/format/crc32c.hh` 新建 + 9 个调用点替换 + 死 include 清理 | **codex** | 物理禁读任何测试文件；按本文 §4/§5 契约实现；产出 CRC 值必须与 absl 逐位一致（论证见 §4.4，验证由 review 侧微基准背书） |
| 字节兼容性 + 吞吐微基准（target `inconel_test_crc32c` / `inconel_bench_crc32c`）、独立验门 | **review 方** | 测试形态由 review 侧执笔，不进 codex 视野 |

codex 的总报告必须显式声明：跳过了哪些调用点（若有）、是否有任何调用点形状与
§5 映射不符而需要裁决、是否动了 CMake link 项（默认不动，见 §7）。

---

## 1. 背景与范围

INC-050（urgent）：tree page CRC 是 owner 合并阶段单一最大 CPU 项。Step 035
Phase 1 实测 flush#1 owner 线程 **70% 时间在算 CRC**，吞吐仅 **371 MiB/s**，距
x86 硬件 CRC32C 应有的 GB/s 量级差 ~25×。CRC 还在 value persist（写热路径）、
value read（读热路径）、WAL append（写热路径）三条 canonical 路径上，不只 flush。

本步只做方向 B：把 9 处 `absl::ComputeCrc32c` / `ExtendCrc32c` 调用换成一个
header-only、能被 `-O3 -flto` inline 到每个 call site 的硬件 CRC32C 实现。

**不做**（显式收窄，见 §11）：方向 A（worker 算 leaf CRC）；3-way 流水 /
PCLMULQDQ folding 满速精修；移除 CMake `absl::crc32c` link 项。

---

## 2. 根因复述（避免误判为"缺硬件"）

已核实：
- absl crc 是 **shared-only**（`/usr/lib/libabsl_crc32c.so`，无 `.a`）。每次
  `ComputeCrc32c`/`ExtendCrc32c` 是跨 DSO 的 PLT 调用，LTO 无法 inline。
- 目标机 i9-12900HX 有 `sse4_2` + `pclmulqdq`，`-march=native` 已开 →
  `_mm_crc32_u64` 直接可用。**硬件一直在，不缺。**
- absl 内部本来就走硬件 CRC32C；371 MiB/s 的病根是**调用粒度**：10377 页 ×
  2 次调用、平均每次仅 ~2KB，PCLMULQDQ folding 的 setup 摊不平，且每页一次
  `Crc32cCombine`（perf 里的 `CRC32::Extend`）做多项式约减。

方向 B 的价值：让同一条硬件指令在一个 inline 的紧循环里全速跑，**无跨 DSO 调用、
无两段拼接的 combine 数学、无 buffer 修改**。

---

## 3. 设计：`apps/inconel/format/crc32c.hh`

### 3.1 平台前提与 fail-fast（CLAUDE.md 约束 A）

本实现要求 SSE4.2。header 顶部显式 fail-fast，不做 silent 软件 fallback：

```cpp
#if !defined(__SSE4_2__)
#  error "format/crc32c.hh requires SSE4.2 (-march=native). Inconel targets x86_64 w/ hardware CRC32C."
#endif
#include <nmmintrin.h>   // _mm_crc32_u64 / _mm_crc32_u8
```

（项目已无条件 `-march=native`，等于已假定该指令存在；此处只是把隐式前提显式钉死。）

### 3.2 API 面（三个符号，命名不带 step/issue 编号）

命名空间沿用 `apps::inconel::format`。

```cpp
// 流式累加器：唯一的真正原语。一个 running 寄存器从头跑到尾。
// 不 init/xorout per-update —— 这正是省掉 combine 的关键。
struct crc32c_stream {
    uint32_t st = 0xFFFFFFFFu;                 // 标准 Castagnoli 初值（reflected 域）
    void update(const void* p, size_t n) noexcept;
    [[nodiscard]] uint32_t finish() const noexcept { return st ^ 0xFFFFFFFFu; }
};

// 一次性连续 buffer 便利函数 == ComputeCrc32c(p, n)
[[nodiscard]] inline uint32_t crc32c(const void* p, size_t n) noexcept {
    crc32c_stream s; s.update(p, n); return s.finish();
}

// 跳过中间 [hole_off, hole_off+hole_len) 的单趟 CRC。
// == ComputeCrc32c([0,hole_off)) 接 ExtendCrc32c([hole_off+hole_len, n))。
// 用于 page_crc / 各 header crc 字段就在被覆盖区中间的情况。
[[nodiscard]] inline uint32_t
crc32c_skip(const void* p, size_t n, size_t hole_off, size_t hole_len) noexcept {
    crc32c_stream s;
    s.update(p, hole_off);
    s.update(static_cast<const char*>(p) + hole_off + hole_len,
             n - hole_off - hole_len);
    return s.finish();
}
```

### 3.3 `update` 实现要点（codex 实现依据）

```cpp
inline void crc32c_stream::update(const void* p, size_t n) noexcept {
    const char* b = static_cast<const char*>(p);
    uint64_t crc = st;
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, b, 8);          // 不假定对齐；编译器降为一条 mov
        crc = _mm_crc32_u64(crc, w);
        b += 8; n -= 8;
    }
    // 尾部 <8 字节逐字节消费 —— 必须在本次 update 返回前消费干净，
    // 否则下一段 update 的首字节衔接错位（见 §4.4 不变量 I2）。
    while (n--) crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *b++);
    st = static_cast<uint32_t>(crc);
}
```

v1 用**单累加器**。可选的 u32/u16 尾部细分允许但非必须；3-way 流水 /
PCLMULQDQ folding **本步不做**（§11）。

### 3.4 正确性不变量（codex 必须保持，review 微基准逐条背书）

- **I1（字节流等价）**：`crc32c(p,n)` 必须逐位等于 `absl::ComputeCrc32c(p,n)`。
  依据：x86 `CRC32` 指令算的就是 reflected CRC-32C（Castagnoli 0x1EDC6F41，
  init 0xFFFFFFFF，input/output reflected，xorout 0xFFFFFFFF），与 absl 同一标准。
- **I2（chain 等价）**：对任意切分 `n = a + c`，
  `{stream.update(p,a); stream.update(p+a,c);}` 必须等于 `crc32c(p,n)`，
  **无论 a 是否 8 的倍数**。这要求 update 在返回前把不足 8 的尾部逐字节消费干净。
- **I3（skip 等价）**：`crc32c_skip(p,n,off,len)` 必须逐位等于现 `tree_page_compute_crc`
  的 absl 两段结果（即 `ComputeCrc32c([0,off)) + ExtendCrc32c([off+len,n))`）。

### 3.5 字节兼容性论证（为什么零磁盘格式风险）

absl 两次调用 `ComputeCrc32c(A)` 后 `ExtendCrc32c(crc, B)`，文档保证等价于
`ComputeCrc32c(A‖B)`。`crc32c_stream` 一个 running 寄存器连续喂 A 再喂 B，算的
也是 `CRC32C(A‖B)`（I2）。两者逐位相同 → **已落盘的 page/value/WAL/superblock
CRC 全部不变 → 无磁盘格式改动、recovery 完全兼容、纯 drop-in**。这是本步可以
低风险落地的根本原因，也是 review 微基准必须先验 I1/I2/I3 的原因。

---

## 4. 调用点替换映射（9 处，逐项镜像；codex 按此改，不得自创形态）

> 分类依据 = 现有代码形状（一次性连续 / 多段链式 / segmented 流式 / 跳字段）。

| # | 文件:行 | 现状 | 改为 | 类别 |
|---|---|---|---|---|
| 1 | `tree_page.hh:160-166` `tree_page_compute_crc` | `ComputeCrc32c([0,15)) + ExtendCrc32c([19,end))` | `return crc32c_skip(page, page_size, crc_field_offset, crc_field_size);` | 跳字段（I3） |
| 2 | `value_object.hh:57` `encode_value_object` | `ComputeCrc32c(body)` | `crc32c(body.data(), body.size())` | 一次性连续 |
| 3 | `value_object.hh:87` `decode_value_object` | `ComputeCrc32c(body_ptr, len)` | `crc32c(body_ptr, expected_body_len)` | 一次性连续 |
| 4 | `value_object.hh:293` `encode_value_object_slot_from` | `ComputeCrc32c(body)` | `crc32c(body.data(), body.size())` | 一次性连续 |
| 5 | `value_object.hh:332-339` `decode_value_object_slot_to` | `crc32c_t crc{0}; visit_segmented_const_bytes(... ExtendCrc32c(crc, chunk) ...)` | `crc32c_stream s; visit_segmented_const_bytes(... s.update(src, n) ...); computed = s.finish();` | segmented 流式 |
| 6 | `wal.hh:132-133` `wal_segment_header_crc` | `ComputeCrc32c(&h, covered)` | `crc32c(&h, covered)` | 一次性连续 |
| 7 | `wal.hh:139-140` `wal_sealed_trailer_crc` | `ComputeCrc32c(&t, covered)` | `crc32c(&t, covered)` | 一次性连续 |
| 8 | `wal.hh:358-370` `wal_entry_parts_crc` | `Compute(header) → Extend(value_ref) → Extend(key)` | `crc32c_stream s; s.update(&parts.header,sizeof); s.update(vr_bytes...); s.update(key_bytes...); return s.finish();` | 多段链式 |
| 9 | `wal.hh:576-577` decode entry crc | `ComputeCrc32c(src.data(), covered)` | `crc32c(src.data(), covered)` | 一次性连续 |
| 10 | `superblock.hh:113-114` `superblock_compute_crc` | `ComputeCrc32c(&s, covered)` | `crc32c(&s, covered)` | 一次性连续 |
| 11 | `tree/page_reader.hh:91-103` `detail::extend_crc` helper | 参数 `absl::crc32c_t& crc`，lambda 里 `crc = ExtendCrc32c(crc, {src,n})` | 参数改 `crc32c_stream& s`，lambda 里 `s.update(src, n)`；返回 bool 与 visit 短路语义不变 | segmented 流式 |
| 12 | `tree/page_reader.hh:170-185` page 校验调用方 | `absl::crc32c_t crc{0};` 两次 `extend_crc([0,15)) / ([19,end))` 进同一 `crc`，比 `hdr.page_crc != (uint32_t)crc` | `crc32c_stream s;` 两次 `extend_crc(...,s)`，比 `hdr.page_crc != s.finish()` | segmented + 跳字段（caller 两段 update） |

**关于 #11/#12（2026-06-15 设计侧裁决，回应实现侧硬停项 a）**：全树扫描确认
`tree/page_reader.hh` 是 format/ 之外**唯一**的 absl crc 调用点（95/99/172 三处，
属同一对 helper+caller），无第三处。它是 `tree_page_compute_crc`（#1）的**读侧
segmented-frame 孪生版**：同样跳过 `page_crc` 字段 `[15,19)`，只是页存在分段 frame
里、由 caller 两次 `extend_crc` 喂进一个累加器完成跳字段。裁决=**纳入 §4**（非放宽
§5 grep）——它是 tree page 读校验热路径，留 absl 既不一致也违背本步目的。字节兼容性：
`absl::crc32c_t{0}` 起两段 extend == `CRC32C([0,15) ‖ [19,end))`，与 #1 同值，由
I2/I3 覆盖。`crc_field_offset` / `crc_field_size` 常量原样保留。

注：
- #5 的 lambda 现在捕获 `absl::crc32c_t crc`；改为捕获 `crc32c_stream& s` 调
  `s.update(src, n)`，循环外 `computed = s.finish()`。segmented 分块边界任意，
  由 I2 保证正确。
- #8 三段 span 顺序必须保持 header → value_ref_bytes → key_bytes（与原
  Compute→Extend→Extend 顺序一致），否则 CRC 值变化、破坏兼容。
- 所有 `crc_field_offset` / `covered` / `crc_field_size` 这些 `offsetof` 常量
  **原样保留**，只换计算函数。

---

## 5. include / CMake / 命名处理

1. 四个 format 头（tree_page / value_object / wal / superblock）顶部 `#include
   <absl/crc/crc32c.h>` 改为 `#include "crc32c.hh"`（或正确相对路径）。
   `tree/page_reader.hh`（#11/#12）的 `absl::crc32c_t` 当前经 format 头**传递**
   include 进来（自身无直接 `#include <absl/crc/...>`）；改完后它需要能看到
   `format/crc32c.hh`——若已 include `format/tree_page.hh` 即自动可见，否则按实际
   相对路径显式加 `#include "../format/crc32c.hh"`。并确保 page_reader.hh 内无任何
   直接 absl/crc include 残留。
2. codex 改完后 `grep -rn "ComputeCrc32c\|ExtendCrc32c\|absl::crc32c_t\|absl/crc" apps/inconel`
   确认 0 残留；若某文件仍有 absl crc 用法（本文未枚举的调用点），**停下报告**，
   不自行扩范围。
3. **CMake `INCONEL_ABSL_LIBS` 里的 `absl::crc32c` 不动**（留作 review 决定是否清）。
   未使用的 link 项无害；移除是独立低风险 cleanup，不混进本步。
4. 命名禁带 step/issue 编号（memory：no-step-names-in-production-code）。文件
   `format/crc32c.hh`，符号 `crc32c` / `crc32c_stream` / `crc32c_skip`。注释里可
   引用 INC-050 作为 rationale，但无任何标识符携带编号。

---

## 6. 错误 / 失败语义（不变）

CRC 不匹配的行为**完全不变**：各 decode 路径仍返回既有 reason status
（`tree_page_status::bad_crc` / `value_decode_status::bad_crc` /
`wal_entry_decode_status::bad_crc` / `superblock_status` 对应项），上层 panic /
fallback 逻辑一字不动。本步只换"怎么算 CRC"，不换"算错了怎么办"。

---

## 7. 验收 / 兼容性微基准（review 方执笔，target `inconel_test_crc32c`）

兼容性是硬门，必须先于吞吐验。微基准对**同一段字节**断言新实现与 absl 逐位相等：

1. **I1 一次性**：长度 ∈ {0,1,2,3,4,5,6,7,8,9,15,16,17,4096,16384} 的随机
   buffer，`crc32c(p,n) == (uint32_t)absl::ComputeCrc32c({p,n})`。含非 8 对齐起始地址。
2. **I2 chain**：对 16KB buffer 在切分点 ∈ {1,7,8,9,4095,4096,4097} 处切两段，
   `{s.update(A); s.update(B);}.finish()` 等于 `crc32c(whole)` 等于 absl 单算。
   多段（3+ 段）切分也覆盖一组（镜像 #8 的 header/value_ref/key 三段）。
3. **I3 skip**：对 16KB page 用 `crc32c_skip(p,n,15,4)`，等于现
   `tree_page_compute_crc` 的 absl 两段结果。
4. **回归交叉**：构造一个 page/value/wal/superblock，分别用旧 absl 路径与新
   路径各算一次，断言相等（证明 §4 映射逐项不改变 on-disk 值）。

吞吐基准（`inconel_bench_crc32c`，可与上面同 target）：对 16KB×N inline 紧循环
测 GiB/s，与 absl 两段+combine 路径比值。**只看比值，记录环境**（CLAUDE.md 性能
方法论）。预期方向性大幅提升；具体倍数实测为准，不照搬 known_issues 的 25×/10-20× 口径。

独立验门：`cmake --build build` / `build_asan` 全 target 0 错；新 `inconel_test_crc32c`
Release + ASAN 全绿；现有 `inconel_test_*`（tree_page_format / wal_format /
superblock_format / value_space_manager / flush_e2e / m01..m13 等）**零回归**——
因为 CRC 值不变，所有 golden-layout / readback 断言必须照过。任何一个回归 = 兼容性
被破坏的一手信号，停下查 I1/I2/I3。

---

## 8. 实现顺序（codex 分 phase，逐 commit）

- **Phase A**：新建 `format/crc32c.hh`（§3 全部：fail-fast + stream + crc32c + crc32c_skip）。
- **Phase B**：替换 §4 的 #1（tree_page，flat skip 路径）+ #11/#12（tree/page_reader，
  segmented skip：helper + caller）+ #6/#7/#9/#10（wal/superblock 一次性）。
- **Phase C**：替换 #2/#3/#4（value_object 一次性）+ #5（value_object segmented 流式）+ #8（wal 三段链式）。
- **Phase D**：§5 的 include 切换 + grep 0 残留确认 + 全量 build。

每 phase 一个 `nitro:` 前缀 commit。review 侧在 Phase A 后即可先写并跑兼容性
微基准（Phase A 的 header 一存在就能对撞 absl）。

---

## 9. 需要人工判断的点 / 硬停线

无阻塞项。硬停线（命中即停下报告，不自行绕）：
- （a）§4 之外存在本文未枚举的 `ComputeCrc32c`/`ExtendCrc32c` 调用点 → 报告，
  不扩范围自行替换。
- （b）任一调用点的字节覆盖范围（`covered` / `offsetof` / body span 边界）与
  §4 描述不符 → 报告，**绝不为了"能编译过"调整覆盖范围**（那会改变 on-disk CRC）。
- （c）review 微基准发现任一 I1/I2/I3 不逐位相等 → 实现有 bug，停下，不得改测试
  迁就。

---

## 10. 文档对账（三阶段检查 after 段）

- on_disk_formats §1.3：CRC-32C 语义**不变**（仍 Castagnoli + init/xor conditioning）。
  实现从 absl 换成 inline 硬件指令属实现细节，可在 §1.3 加一句"runtime 实现见
  `format/crc32c.hh`，产出值与标准 CRC-32C 一致"，不改语义定义。
- cross_doc_contracts §1/§2/§5 / 三条红线：**零触及**（无 handle 签名、无 struct
  字段、无 pipeline 路径变化）。
- known_issues：本步落地后把 INC-050 的方向 B 标记完成，保留方向 A 为 open
  follow-up（明确 A 依赖 B 先把单次 CRC 做快 + 属 INC-022 flush handle 的
  worker/owner 职责划分范围）。

---

## 11. 为什么方向 A 不在本步（收窄声明）

方向 A（leaf page CRC 下放 worker）会改 INC-046 契约（"worker 返回 zero-extra-copy
leaf carrier" → 追加 CRC 是 worker 职责），且收益依赖 B 先把单次 CRC 做快（否则只是
把慢 CRC 挪个核）。A 本质属 tree 写侧 owner/worker 职责划分，与 INC-022 flush handle
同摊，应在进 flush 编排主线时一并裁，不在这个 header-only 低风险步里夹带。

同理，3-way 流水 / PCLMULQDQ folding 满速精修也延后：v1 单累加器已足够把 371 MiB/s
的瓶颈拆掉；进一步逼近峰值要等实测数据证明单累加器仍是瓶颈再做（实测先行纪律）。
