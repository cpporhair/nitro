# 052 - M13 E2E Harness / Mock Device / Multicore

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M13
> （旧 step 26L mock NVMe owner 边界 + 26O mock device 分层与设备验证 +
> 26Q 多核测试基础设施与热路径观察）。
> 目标：交付 **mock NVMe 后端**（device + scheduler，类型级替换
> `nvme::runtime_device/runtime_scheduler`），使完整 production 栈
> （build_runtime → registry → rt:: ops → tree/value/WAL 全 I/O 路径）
> 在无 SPDK 硬件下可 in-gate 运行；在其上落 dev plan 的 13 条 e2e
> 测试矩阵（含多核并发写与 seal→collect→**真实 tree-local flush**
> 全桥）与 standalone profile binary，关闭 M11 §13.4 / M12 §13.4 声明
> 的全部 in-gate 边界，完成 dev plan §6 迁移完成定义。

## 0. 角色分工（本步与 M08-M12 不同，先冻结）

| 工作 | 执笔 | 纪律 |
|---|---|---|
| 本设计文档、测试程序（`test/` 下全部：13 条矩阵、多核 harness、profile binary）、测试运行与问题诊断 | **review 方（本文作者）** | 测试作者角色，可读全部测试与 production 代码 |
| mock 后端（`mock_nvme/` 模块）、切换面（`nvme/runtime_scheduler.hh` + builder allocator 选择）、后续实测暴露的 production 修复 | **codex** | 照旧**物理禁读任何测试文件**；mock 按本文 §5 契约表实现（契约来源 = `real_scheduler`/`real_device` 公有面 + 调用点 grep），不按测试形态实现 |
| 实测问题的修复设计 | review 方 | 以 052 增补节或独立修复任务书形式下发 codex |

理由：M13 主交付物是测试基础设施本身，由 review 方直接执笔并运行；
production 代码（mock 后端是被 production 头 alias 的契约实现）仍由
不可见测试的一方实现——"实现禁读测试"的失败模式防线不变。

## 1. 范围

M13 覆盖：

- L1 `apps/inconel/mock_nvme/`（新模块，现存空目录的归宿）：
  - `device.hh`：`mock_device`——flat byte store 的单 namespace 内存
    盘，**mutex 线程安全**（多核 scheduler 共享同一 device），同步
    read/write/trim/flush + 操作计数 + 测试预置/回读接口；满足
    builder/validate 触达的 `real_device` 子面（§5.2）。
  - `scheduler.hh`：`mock_scheduler`——逐项镜像 `real_scheduler`
    消费面（§5.1）的 per-core PUMP scheduler：owner 队列 +
    `advance()` 内对共享 `mock_device` 执行 memcpy I/O，完成后 cb。
- L1 `nvme/runtime_scheduler.hh`：编译期后端切换（§4.1）：
  `INCONEL_NVME_MOCK_BACKEND` define 下
  `runtime_device/runtime_scheduler` alias 指向 mock。
- L3 `runtime/builder.hh`：mock define 下 DMA allocator 选择
  `make_heap_dma_page_allocator()`（§4.2；同一 define，集中在一个
  helper，不散落 if-def）。
- 测试（review 方执笔）：
  - `inconel_test_m13_e2e_matrix`（13 条矩阵，§7）；
  - 多核 harness（production `build_runtime` + `rt::run` 真线程）；
  - `inconel_profile_m13`（standalone profile binary，非 CTest 门，
    §8）。

M13 不覆盖：

- 真盘 smoke 的自动化（flush_e2e 维持手动真盘流程；mock 矩阵不替代
  真盘验证，两者互补——mock 验语义，真盘验设备）。
- recovery（迁移计划明确不在本线）。
- seal 自动触发、INC-021/INC-055/INC-054/INC-056（维持既有裁决）。
- pump 框架改动。
- 网络/协议层。

## 2. 已对照输入

- dev plan M13 节（必须重构 1-3 + 13 条矩阵）+ §6 迁移完成定义。
- `design_doc/code_modules.md`（nvme/ 模块职责；L1 共享基础设施）、
  `code_quality_standard.md` §3.9（设备访问边界：mock 不得诱发同步
  直调——mock scheduler 仍是 owner 队列 + advance 异步形态）、
  `runtime_state_machine.md` §7（nvme 操作面）、ODF §4.5（TRIM 后
  读回全零——mock trim 语义依据）。
- 当前代码：`nvme/real_scheduler.hh`（§5.1 契约源：双 ctor、
  advance、read/write contiguous 兼容路径、read_frame/write_frame/
  flush/trim/trim_ns_lba、lba_size、get_ssd）、`nvme/real_device.hh`
  （§5.2 契约源：qpair_for_core/sector_size/total_logical_lbas/
  device_id/namespace_bytes）、`nvme/frame_io.hh`（free 函数以
  `runtime_scheduler*` 与模板双形态消费）、`runtime/builder.hh`
  （ctor 实参与 validate tier-3 触达点）、m06-m12 测试 target 均
  link `inconel_real_nvme` 而不初始化 SPDK（§4.1 安全性先例）。
- 旧分支证据（设计者角色已声明读测试）：
  `inconel:apps/inconel/runtime/mock_nvme/{device,owner,access,io_plan}.hh`
  （flat store + mutex + 计数 + sender owner 形态——语义参考不迁移
  代码）、`inconel:apps/inconel/test/write_batch_device_verification_test.cc`
  / `write_batch_multicore_test.cc`（矩阵与多核驱动意图）、
  step 26L/26O/26Q 设计。
- M11 §13.4 / M12 §13.4（本步要关闭的声明边界）、
  feedback：模块完成阶段 e2e 必须附 standalone profile binary。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` 26L/26O/26Q 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 052 决议 |
|---|---|---|---|---|
| mock 落点 | `runtime/mock_nvme/*` 模块 | `mock_nvme/` 空目录已在；INC-002 删了旧内存路径 | dev plan 落点 2"mock/real test backend"；必须重构 1（mock 不成为 production boundary） | mock 落 `apps/inconel/mock_nvme/`，**只经 `INCONEL_NVME_MOCK_BACKEND` define 进入 alias**；未定义时 production 头零引用 mock（boundary 隔离的机制化） |
| mock device 形态 | flat byte array + mutex + 计数 + 同步 I/O；测试直接预置/回读 | 无 | ODF §4.5（TRIM 读回零）；多核共享单盘 | 保留旧形态语义：mutex 保护的 flat store（不迁移代码，按 §5.2 契约重写）；TRIM = memset 0（与 DLFEAT 验证一致）；read/write/trim/flush/FUA 计数为矩阵断言面 |
| mock scheduler 形态 | sender owner + request pool + advance 执行 | fake NVMe（m07-m12 test-local）已镜像最小面但非 runtime 类型 | CQS §3.9（owner 队列 + advance 异步边界不得折叠） | mock_scheduler = per-core owner 队列 + `advance()` 内同步 memcpy 完成 + cb；**不**做同步直调捷径。镜像 §5.1 全契约（含 contiguous read/write 兼容路径） |
| 切换机制 | 旧分支全局 mock runtime 对象 | `runtime_scheduler = real_scheduler` 硬连 | dev plan 落点 2 | **per-target 编译 define 切 alias**（§4.1）。同 target 内类型一致（单二进制无 ODR 面）；real targets 零变化 |
| SPDK 依赖处理 | 旧分支 mock 无 SPDK | builder 无条件用 `make_spdk_dma_page_allocator` + SPDK 常量 | m06-m12 先例：link SPDK 不初始化即安全 | **不剥离 SPDK link**（m13 target 照 link `inconel_real_nvme`）；仅 builder 在 mock define 下选 heap allocator（§4.2）——SPDK 头/常量全保留，改动面最小 |
| 多核驱动 | 26Q：真线程推进 owner advance | run.hh/start.hh 已有 per-core loop；m01-m12 全部单线程 advance 轮询 | 必须重构 3（推进 advance()/runtime scheduler，不直调 state） | harness 用 production `build_runtime`（mock 后端）+ 每核真线程跑 `rt::run`，主线程作客户端 submit rt:: ops，stop flag 收敛；细节由测试作者落 |
| 矩阵 | 26O/26Q 测试集 | dev plan 13 条 | dev plan M13 完成测试矩阵 | §7 全收：1-11 单核 mock e2e（含设备字节验证）、12 多核并发写、13 seal→collect→**真实 tree_local_flush**（mock 上全链可跑——本步最大红利） |
| profile | 26Q"热路径优化" | 无 | feedback：模块完成阶段附 standalone profile binary | `inconel_profile_m13`：mock 后端 N 写 M 读吞吐/延迟分布，非 CTest 门（flush_e2e 模式手动跑）；发现的热路径问题按 §0 迭代流程处理 |

## 4. 关键裁决

### 4.1 后端切换：per-target 编译 define 的 alias

```cpp
// nvme/runtime_scheduler.hh
#ifdef INCONEL_NVME_MOCK_BACKEND
    #include "../mock_nvme/device.hh"
    #include "../mock_nvme/scheduler.hh"
    namespace apps::inconel::nvme {
        using runtime_device    = mock_nvme::mock_device;
        using runtime_scheduler = mock_nvme::mock_scheduler;
    }
#else
    // 既有 real alias 原样
#endif
```

依据：(a) registry/builder/全部消费点以 `nvme::runtime_scheduler*`
具型存储，类型级替换是唯一不改契约面的形态；(b) per-target define
（m13 矩阵 + profile 两个 target `target_compile_definitions(...
PRIVATE INCONEL_NVME_MOCK_BACKEND)`）使 real targets（m01-m12、
flush_e2e）零重编差异；(c) 单 target 单类型，无跨二进制 ODR 面；
(d) 未定义 define 时 production 头不 include mock —— dev plan 必须
重构 1 的机制化。m13 targets 照常 link `inconel_real_nvme`（SPDK
库 link 不初始化即惰性，m06-m12 已证），不做任何 SPDK 剥离。

### 4.2 mock build 下的 DMA allocator

builder 当前无条件 `make_spdk_dma_page_allocator()`（需 SPDK env
初始化）。裁决：builder 引一个 backend-allocator helper（同一
define 下返回 `make_heap_dma_page_allocator()`，否则 SPDK 版），
替换 builder 内全部分配器构造点；不在调用点散落 ifdef。语义零
变化（allocator 本就是注入参数）。

### 4.3 mock 的 I/O 语义

1. 写完成即 durable（FUA 仅计数，不改语义）——mock 不模拟易失
   缓存；崩溃语义不在 mock 验证范围（那是真盘/未来 fault-injection
   的事，矩阵只验逻辑语义 + 字节）。
2. TRIM = 区间 memset 0（ODF §4.5 读回全零契约）。
3. flush = 计数 + 成功。
4. 越界 I/O → cb(false)（与 real 完成失败同形态），不 panic——
   上层 fail-fast 路径自己裁决。
5. device 级 mutex 串行全部 I/O：多核下每核 mock_scheduler 共享
   单 device；mutex 在 mock 粒度上模拟设备串行点，吞吐失真可接受
   （profile 解读时注明）。

### 4.4 矩阵 13（真实 flush 桥）的范围

mock 后端使 `tree_local_flush` 的写盘段首次 in-gate 可跑：测试 13
做 seal → collect → `tree_local_flush`（经 `rt::owner()` 全链，含
NVMe FLUSH 到 mock）→ manifest 产出 → **`rt::point_get` 经 tree
路径读回 flush 后数据**（release_gens 后 memtable miss → tree hit
——M10 接的 tree path 第一次吃到真实 flush 产物）。这同时关闭
M11 §13.4（rt::write_batch 带 I/O）与 M12 §13.4（真实 flush 桥）。
若 flush 链在 mock 上暴露 production 问题，走 §0 迭代流程（预期内
——这正是本步存在的意义）。

## 5. Mock 契约表（codex 实现依据，逐项镜像）

### 5.1 `mock_scheduler`（镜像 `real_scheduler` 消费面）

| 成员 | 形态 | mock 语义 |
|---|---|---|
| `qpair_t` | `using qpair_t = mock_device::core_handle`（§5.2） | builder 的 `device->qpair_for_core(core)` 直通 |
| ctor ×2 | 与 real 双 ctor 同参数表（qpair_t*, lba_size, [allocator \| pool_pages], queue_depth, local_depth, alignment, numa_id, device_id） | 存 device 句柄 + lba_size + 自建 `lba_dma_page_pool`（heap allocator）；null qpair → `invalid_argument`；pool_pages 形态忽略 SPDK mempool 仅按 heap 处理 |
| `advance()` / `advance(Runtime&)` | bool | drain owner 队列：对 device 执行 memcpy I/O，cb(ok)；bounded drain |
| `read_frame(frame, flags=0)` / `write_frame(frame, flags=0)` | → sender\<bool\> | 逐 LBA 对 frame 的 segmented pages memcpy（read: device→pages；write: pages→device）；FUA flag 计数 |
| `flush()` / `trim(lba,n)` / `trim_ns_lba(lba,n)` | → sender\<bool\> | §4.3 语义 |
| `read(lba,buf,n)` / `write(lba,buf,n,flags=0)` | → sender\<bool\>（contiguous 兼容路径） | 经临时 frame 或直接 memcpy；与 real 完成值语义一致 |
| `lba_size()` | uint32_t | 直返 |
| `get_ssd()` | 仅当现有调用点编译需要时提供（codex grep `get_ssd` 全部调用点后裁决：无 mock-target 触达则可不提供，报告声明） | nullptr 等价物 |
| PUMP 件 | op/sender + `op_pusher`/`compute_sender_type` 特化（六组件模式） | 与 m07-m12 fake 同形态但归 mock_nvme 模块所有（CQS §3.7 owner 自包含） |

约束：**不实现矩阵/测试专属接口**（hold/fail 注入留给测试侧的
fake——mock 是 runtime 后端不是故障注入器；故障注入需求出现时另立
设计）。计数器（read/write/trim/flush/fua）放 device（§5.2），
scheduler 不留观察口。

### 5.2 `mock_device`（镜像 `real_device` 被触达面 + mock 专属）

| 成员 | 形态 | 语义 |
|---|---|---|
| `core_handle` | per-core 句柄类型（含 device 反指 + core id） | `qpair_for_core(core)` 返回其指针；进程生命周期由 device 持有 |
| ctor | `(uint32_t lba_size, uint64_t namespace_lbas, uint16_t device_id = 0)` | flat store 分配清零 |
| `qpair_for_core(core)` | `core_handle*` | 非空（懒建或预建均可） |
| `sector_size()` | uint32_t | = lba_size |
| `total_logical_lbas(logical_lba_size)` | uint64_t | namespace 字节 / logical_lba_size |
| `namespace_bytes()` | uint64_t | 仅当现有调用点触达（codex grep 后裁决） |
| `device_id()` | uint16_t | 直返 |
| 同步 I/O | `read/write/trim/flush`（mutex 串行）+ 计数 | §4.3；**测试预置/回读接口**：`read_bytes/write_bytes(lba, span)`（即同步 I/O 本身，测试直接用） |
| 计数 | reads/writes/trims/flushes/fua_writes | 矩阵断言面（如 DELETE-only 零 value 写、WAL 字节验证配合回读） |

### 5.3 codex 实现纪律

照旧：**物理禁读任何测试文件**；契约只来自本表 + real_* 头 + 调用
点 grep；`get_ssd`/`namespace_bytes` 这类"按需提供"项必须在总报告
声明裁决依据（调用点清单）。Phase 划分：A = mock_nvme 模块；
B = alias 切换 + builder allocator helper + 全仓编译门（real
targets 不重编差异、m01-m12 全绿）。**不注册任何 m13 测试 target
——CMake 的 m13/profile target 由测试作者随测试提交。**

## 6. 错误 / 失败语义

| 场景 | 行为 |
|---|---|
| mock I/O 越界 | cb(false)（与 real 完成失败同形态） |
| mock ctor 非法（lba_size 0 / namespace 0 / null qpair） | `invalid_argument` |
| define 切换下 real targets | 零行为变化（编译期隔离） |
| 矩阵/多核/flush 桥暴露的 production 失败 | §0 迭代流程：诊断 → 修复设计 → codex → review → 复测 |

## 7. 测试矩阵（review 方执笔；target `inconel_test_m13_e2e_matrix`）

环境：production `build_runtime`（mock 后端，单核拓扑为 1-11、多核
拓扑为 12-13）→ registry → `rt::write_batch` / `rt::point_get` /
`rt::seal_once` 全 rt 面驱动；设备级断言经 mock_device 回读/计数。
dev plan 13 条逐一落（编号即 dev plan 序）：

1. single PUT value on device（vr 处字节含 header+body，device 回读验证）
2. multi-key fan-out 到双 front（WAL 区字节按 stream 分布）
3. same-key canonicalization（单 batch 同 key 折叠后 device 仅一份）
4. PUT then DELETE（tombstone 遮蔽 + value 字节仍在（未回收）的现状声明）
5. DELETE-only 零 value 写（device value 区写计数为 0）
6. WAL entry decode 与 value_ref 对账（device WAL 区字节解码 == memtable vr）
7. sequential LSN advance（连续 batch ack 序）
8. memtable visible after write（rt::point_get hit）
9. point_get after write（= M10 语义在 rt+mock 全链）
10. overwrite read latest
11. delete read not_found
12. **multicore concurrent write batches**（真线程 rt::run，N 客户端并发 rt::write_batch，全部 ack 后全 key 可读、LSN 无洞）
13. **seal → collect → 真实 tree_local_flush → release_gens → rt::point_get 经 tree 路径读回**（§4.4）

矩阵之外按需补强（测试作者裁量）：多核下 seal 与写并发的不跨代
断言（M12 机制的真并发行使）。

## 8. Profile Binary（review 方执笔；target `inconel_profile_m13`）

standalone、非 CTest 门：mock 后端上 N 写（可调 batch 大小/值尺寸/
front 数）+ M 读，输出吞吐与延迟分位；多核模式复用 harness。用途：
迁移线模块完成的热路径基线 + 后续真盘对照的形状参考（绝对值不跨
后端对比，比值可以）。mock mutex 串行的失真在输出头注明。

## 9. 实现顺序

```text
Phase A（codex）  mock_nvme/{device,scheduler}.hh（§5 契约表）
Phase B（codex）  nvme/runtime_scheduler.hh 切换 + builder allocator helper；
                  全仓编译 + m01-m12 全绿（mock 模块此时无调用者，以一个
                  最小 compile-check TU 或 profile 占位确保 mock 路径编译——
                  形式由 codex 选并声明）
——以下 review 方执笔——
Phase C  m13 矩阵 1-11（单核 mock e2e）+ CMake 注册
Phase D  多核 harness + 矩阵 12 + 补强
Phase E  矩阵 13（真实 flush 桥）
Phase F  profile binary + 全量回归（Release/ASAN m01-m13）
迭代     C-F 中实测问题按 §0 流程随发随修
```

## 10. 验收 / 迁移完成定义对账

m01-m13 Release + ASAN 全绿后，对照 dev plan §6 逐条勾验并在
wrap-up 记录（含哪条由哪个测试/步骤背书）；M11 §13.4 / M12 §13.4
边界关闭记录写进各自设计文档 watch-item 更新。

## 11. 需要人工判断的点

无阻塞项。切换机制（§4.1）、SPDK 不剥离（§4.1d）、allocator
（§4.2）、mock 语义（§4.3）、桥范围（§4.4）均有依据。硬停线：
（a）若 real_scheduler 契约面存在本表未枚举且 mock 无法等价提供的
被触达成员，codex 停下报告；（b）若 mock 后端下 build_runtime 的
任何 production 校验路径需要"放宽"才能通过（即 mock 撒谎才能过
validate），停下报告——那是 validate 或 mock 设计错误，不得绕。

## 12. Review 对账与结果（2026-06-12，M13 land 记录）

实现提交：`3e90cd8`(A mock 后端，codex) → `b0a7f6c`(B 切换面，codex) →
`bbbf674`(C 矩阵 1-11，review 方) → `fff68a6`(D 多核 harness + 矩阵 12 +
seal 交错探针，review 方) → `de2eac1`(E 矩阵 13 全桥，review 方) →
`c80edd1`(F profile binary，review 方) → wrap-up。§0 角色分工全程成立。

### 12.1 codex A/B 对账

1. §5 契约表逐行落地；`get_ssd()` / `namespace_bytes()` 按"无 production
   调用点"省略（报告附 grep 依据，review 核实）。mock_scheduler 走
   owner 队列 + bounded advance（CQS §3.9 未折叠）；`pump::core::
   ring_queue` 为既有 pump 头（review 曾误判不存在——grep 范围不全的
   自纠记录）。
2. compile-check 以临时 TU 等价形式交付（实例化 `build_runtime` +
   `destroy_runtime` 全链）；in-tree 永久门由 Phase C 起的 m13 target
   承担（首个 mock 类型下 rt 全链实例化点）。
3. **流程瑕疵（review 修正）**：codex amend 丢失 commit hook 的
   `nitro:` 前缀；review 以树不变的 commit-tree reword 修复
   （662cccc/6a87802 → 3e90cd8/b0a7f6c）。
4. 独立门：reword 后树 Release m01-m12 12/12。

### 12.2 矩阵与多核结果（review 方执笔/运行）

- **矩阵 1-13 全部首跑即过，全程零 production 修复需求**——M01-M12
  逐步纪律 + mock 契约实现首次全栈对撞无一处需要返工（§0 迭代流程
  空转关闭）。
- 矩阵 1-11：production `build_runtime`（mock 后端）+ rt:: 全链默认
  provider 零注入；设备级字节验证（value object decode / WAL 区
  decode / DELETE-only 顶区不变断言）。
- 矩阵 12：4 runtime 核真线程（pin + `rt::run`）+ 双 client 线程
  （独立 producer lane）各 100 批窗口化并发——200 个 ack 恰为
  1..200 无洞无重，全 key 在线可读。
- **seal 交错探针**：120 个跨 front batch 与 3 轮 `rt::seal_once`
  真并发竞争，逐 batch 断言两 key 的 gen 纪元一致——**M12
  `enter_memtable_phase` 机制在真并发下的首次直接行使成立**（052
  补强项）。
- 矩阵 13：bootstrap 格式化 mock 盘（production `build_superblock`，
  layout 按 profile 逐字段）→ 写入 → seal → collect → **真实
  `tree_local_flush`**（bootstrap root-change：tree 页落盘、NVMe
  FLUSH、superblock 读改写 generation 1→2 + root 非零）→ production
  `install_cat`/`release_gens` 组合 frontier switch → **`rt::point_get`
  经 tree 路径读回全部 key**（memtable miss 断言先行）+ 负例。
  专用 `coord::frontier_switch` handle 留 flush 编排步（测试以
  production 件组合，声明非缺口）。
- 抖动：m13 Release 复跑 8+5 次稳定；ASAN 全绿无 leak/UAF。

### 12.3 边界关闭与新证据

1. **M11 §13.4 关闭**：`rt::write_batch` 带 I/O 全链（矩阵 1-12）。
2. **M12 §13.4 关闭**：seal 与写真并发（交错探针）+ 真实 flush 全桥
   （矩阵 13）。050 §18.4.2 / 051 §17.4.1 对应项随本文勾销。
3. INC-055 现状驻留的规模证据：profile 5000 写后 5000 读仅 1425 次
   设备读（读多数命中 value 驻留/缓存）。
4. profile 基线（mock，4 核，256B，窗口 16）：写 ~187k batch/s
   （p50 85μs 含窗口排队）、读 ~224k get/s（p50 4μs）；FUA = 全部
   写（v1 每页 FUA 政策直观可见）；mock mutex 串行失真已在输出与
   §8 注明，仅作比值基线。

### 12.4 独立验门记录

`cmake --build build`/`build_asan` 全 target 0 错；Release
`inconel_test_m01..m13` **13/13**；ASAN 同名 **13/13**、无
AddressSanitizer/LeakSanitizer 输出；m13 复跑 5 次稳定；无残留进程；
变更范围 = mock_nvme 两头 + runtime_scheduler.hh + builder.hh（codex）
与 test/ 两文件 + CMake（review 方），`pump/`、`ai_context/`、既有
m01-m12 测试零触碰。

### 12.5 遗留 watch-item

1. 真盘 smoke 的 front/WAL 链路（flush_e2e 目前只覆盖 tree/value）：
   下次真盘窗口把 m13 矩阵关键路径在 real 后端重放（052 §1 声明的
   mock/真盘互补边界）。
2. mock 故障注入（失败矩阵的 e2e 化）：需求出现时按 052 §5.1 约束
   另立设计。
3. profile 的真盘对照与热路径深挖：迁移线已完成基线；后续优化按
   "实测数据先行"纪律单独立项。
