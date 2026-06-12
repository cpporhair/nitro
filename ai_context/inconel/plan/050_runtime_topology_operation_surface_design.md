# 050 - M11 Runtime Topology + Operation Surface

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M11
> （旧 step 25A runtime owner registry/topology + 旧 step 26B runtime-level
> operation 入口 + 旧 step 26C/D/E 的 operation 收口意图）。
> 目标：把 M01-M10 用显式拓扑参数驱动的 coord/front/wal 三角色接进
> production runtime——**builder 构造 + registry 注册 + PUMP tuple 调度**，
> 并交付正式 operation surface：`rt::write_batch(input)` /
> `rt::point_get(key)` 从 registry 解析拓扑、委托给 M09/M10 已冻结的底层
> sender（签名不动，facade 只做包装，048 §15.2 / 049 §15.1 兑现）。同时
> 关闭 047 §17.4.1 watch-item（front queue_depth 与 WAL prepare FIFO
> 容量解耦）。

## 1. 范围

M11 覆盖：

- L0 `format/format_profile.hh`：新增 WAL 区三个 disk-format 字段
  （`wal_base_paddr` / `wal_segment_size` / `wal_segment_count`，与
  superblock 同名字段对齐）+ `profile_is_self_consistent` 与 builder
  校验扩展 + `kBootstrapFormatProfile` 取值（§5.1）。
- L2 `front/wal_append.hh` + `front/scheduler.hh`：
  `wal_append_config.pending_prepare_capacity`（0 = 沿既有"跟随
  queue_depth"耦合形态；非 0 = 独立容量）——047 §17.4.1 的解耦裁决
  （§5.2）。
- L0 `core/registry.hh` 增量：`front_scheds {list, by_core}`、
  `wal_space_sched` singleton、`nvme_by_front_owner` 映射 + typed
  accessors（§6）。
- L3 `runtime/builder.hh`：
  - `front_topology_options` + `build_front_topology(...)`
    （device 无关的 front 栈构造 seam：gens → 初始 CAT → coord →
    fronts → wal_space → registry 注册，§7）；
  - `inconel_runtime_t` PUMP tuple 扩到 7 类 scheduler，
    `build_runtime` 接入 front 栈（默认全核 front、coord/wal 落
    cores[0]）、`nvme_by_front_owner` 回填、`destroy_runtime` 扩展
    （§8）；
  - 顺带修正 builder 内"tree allocator grows downward"的错误注释
    （实现事实：head 从 `value_data_area_base` 向高地址 bump，对向
    碰撞 value_head，ODF §10.4 形态）。
- L3 `runtime/operations.hh`（新文件）：`rt::write_batch` /
  `rt::point_get` 正式 operation surface（§9）。
- 测试 `inconel_test_m11_runtime_topology_operations`（§13）。

M11 不覆盖：

- seal_once / flush 编排的 operation surface（M12）；MultiGet/Scan
  （INC-021 维持延期）。
- recovery 安装 CAT_clean / next_lsn 的 runtime 接线（recovery 未迁移；
  本步 boot-from-empty，§7.4 留 hook 说明）。
- mock NVMe device 分层与 `rt::write_batch` 的带 I/O 全链 e2e
  （M13；本步 in-gate 边界见 §13.4 显式声明）。
- 多核运行测试（M13）。
- `batch_lookup` / `scan_memtable` 的 borrowed-snapshot 形态
  （049 §17.4.2，维持延期）。
- pump 框架任何改动（`per_core::queue` 的预分配形态照用，容量影响
  进 §12 估算表）。
- INC-054（tree/value 边界协议）：WAL 区静态布局校验落在本步，但
  tree 分配器与 value 的对向越界协议仍属 INC-054，不动。

## 2. 已对照输入

正式设计：

- `design_doc/INDEX.md`（容量/性能硬约束；新增 runtime carrier 必须
  做容量估算）
- `design_doc/design_overview.md` §1.7（8 类 scheduler 实例数/路由）、
  §3.3（运行时部署参数：front_sched_count 不入盘）、§4.1（格式化
  参数：WAL 三字段属盘格式）、§11（WAL segment pool）
- `design_doc/on_disk_formats.md` §2.2（superblock WAL 字段）、§3.6
  （segment size 约束）、§6（区域关系）、§7（格式化流程：
  wal_base = 2 + reserved）
- `design_doc/runtime_state_machine.md` §1（scheduler 总览）、§2
  （coord ctor 语义）、§3（front）、§5（wal_space）
- `design_doc/code_modules.md`（runtime/ 职责：部署参数、初始化流程、
  注册表；L2 互不依赖、跨 scheduler 走 L3）
- `design_doc/cross_doc_contracts.md` §1/§3（handle 签名与 owner 归属
  不动）
- `design_doc/code_quality_standard.md`（§3.3 carrier、§3.6 owner
  可见性、§3.8 单核 bring-up 不得折叠语义边界）

当前分支代码：

- `apps/inconel/runtime/{builder,facade,run,start}.hh`（既有四角色
  builder、role-core map、rt:: facade、专用 advance loop）
- `apps/inconel/core/registry.hh`（nvme/tree_read_domain lists +
  value/tree/coord singletons + shard map；coord singleton 字段已预留）
- `apps/inconel/format/format_profile.hh`（INC-034 单一 disk-format
  来源；现缺 WAL 字段）
- `apps/inconel/coord/scheduler.hh`（ctor:
  `(initial_cat, front_count, next_lsn=1, ready_window=65536,
  queue_depth=1024, pending_assign_capacity=0→ready_window)`）
- `apps/inconel/front/scheduler.hh`（ctor 第 186 行
  `wal_pending_prepare_capacity_(queue_depth)` 耦合点；11 条
  per_core 队列）
- `apps/inconel/wal/scheduler.hh`（ctor `(geometry, stream_count, …)`）
- `apps/inconel/write_path/write_batch.hh`（M09 底层签名）、
  `apps/inconel/pipeline/point_get.hh`（M10 底层签名）
- `pump/src/env/runtime/runner.hh`（`global_runtime_t` 变参 tuple、
  `add_core_schedulers` 全包一次性设置 + all-null 断言）、
  `pump/src/pump/core/lock_free_queue.hh`（`per_core::queue` 预分配
  129 lane、容量必须 2 的幂否则 abort）
- `apps/inconel/test/test_flush_e2e.cc`（build_runtime/destroy_runtime
  既有调用点，新增 build_options 字段必须带默认值保持其零改动）

plan 文档：

- `041` §（coord ctor 校验/ready window）、`042`（wal_space 语义）、
  `043` §4.1/§4.2（front 构造前置与 gen_id stride）、`045` §5.D
  （prepare FIFO 软容量）、`047` §7（三层并发 budget）/§12.2.9
  （ring 容量-1 语义）/§17.4.1（queue_depth 双用途 watch-item，本步
  关闭）、`048` §15.2（facade 只包装）、`049` §15.1/§17.4（point_get
  包装、watch-item 清单）

旧分支证据（语义参考，不迁移代码；设计者角色已声明读测试）：

- `inconel:ai_context/inconel/design_doc/runtime_owner_topology.md`
  （semantic-first vs local-first、typed group、禁止 mixed
  all_owners、front binding 元数据含 `home_tree_lookup_id`）
- `inconel:ai_context/inconel/design_doc/runtime_operation_entry.md`
  （runtime-level free function、install-once global runtime、
  consuming entry、轻量 write ack）
- `inconel:apps/inconel/runtime/inconel_runtime.hh` /
  `runtime_operations.hh` / `operations/*`（单 runtime 对象 owns
  everything + umbrella header 形态）

登记问题：INC-054（urgent，§1 排除范围声明边界）、INC-056（normal，
正交不动）、INC-021/INC-055（维持 049 裁决，不动）。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 25A/26B 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 050 决议 |
|---|---|---|---|---|
| topology 容器形态 | 单一 `inconel_runtime` 对象 owns everything + typed accessor；明确否定 kv 式全局 inline list（旧 §2.1） | `core::registry` inline lists + singletons + by_core 已是 005-049 全部模块的既有地址簿；builder/facade/测试全部建立其上 | dev plan M11 必须重构 1（不迁移旧 runtime 对象形状，除非匹配当前 builder）；迁移规则 5（现状非自动权威，但此处现状是 16 个 step 的 load-bearing 结构） | **保留并扩展 `core::registry`**。旧分支对 inline 表的反对点（语义 owner 被"随机挑一个"抹平）由 typed accessor 纪律承接：registry 只暴露 typed list/singleton，**不提供** `all_owners()` / `random_front()` / local-first front 入口；语义路由全部在 ops/pipeline 层按 key hash 完成 |
| install-once 全局 runtime | `install_global_runtime` 一次性安装、二次 fail-fast、不提供 reset | registry 由 builder 填、`clear()` 复位；m01-m10 测试 fixture 全部依赖 per-test clear+rebuild | 迁移规则 5/6 | **不迁移 install-once 契约**。生产填表点唯一（build_runtime），测试 clear+rebuild 是既有 house 模式；引入 install-once 会推翻全部既有 fixture 且无对应正确性收益。registry accessors 维持 assert/panic on 未注册（fail-fast 保留） |
| front binding 元数据 | `{owner_id, home_core, owner*, home_tree_lookup_id}` | 读路径 miss 经 `current_shard_partitions()->route(key)`（INC-040），front 与 read_domain 无静态绑定 | dev plan M11 必须重构 3 | binding 收敛为 `{owner_id（即 list 下标）, home_core, front_sched*}`；**不迁移 `home_tree_lookup_id`**（已被 shard-partition 路由取代）。home_core 仅作 locality/nvme 映射元数据，不参与语义 owner 选择（必须重构 2） |
| operation 入口形态 | runtime-level free function：`write_batch(client_batch_buffer&&)` → 轻量 ack；`point_get_memtable_only(string&&)`；不传 runtime& | M09 `write_path::write_batch` 显式拓扑签名 + `write_batch_result` 轻 ack；M10 `pipeline::point_get` 同款 | 048 §15.2 / 049 §15.1：底层签名不动，facade 只包装 | `runtime/operations.hh` 落 `rt::write_batch(core::client_batch_buffer&&, NvmeProvider = {})` / `rt::point_get(std::string_view, NvmeProvider = {})`：compose 期从 registry 解析拓扑后**直接委托底层 sender**。write 入口 consuming（&&，与旧分支一致）；read 入口维持 049 的 borrowed view + caller-pin 契约（不迁移旧 owned-string——那是旧全局 runtime API 的形态，M13 网络层本就持有 request buffer） |
| umbrella header | `runtime_operations.hh` include 全部 operations/* | 无 | dev plan M11 必须重构 4（不重新引入 root alias / giant public header） | `runtime/operations.hh` 只含本步两个 op + 必要 include；seal/flush 的 op 等 M12 自然生长，不预建 umbrella |
| 旧 raw_ops 重载 | `write_batch(vector<raw_batch_op>&&)` 服务 white-box | M01 起 ingress 冻结 `client_batch_buffer`；测试用 `encode_client_batch` | 039/041/048 | 不迁移；单一 buffer 入口 |
| WAL 几何来源 | 旧分支 mock 时代由 harness 给 | 当前仅测试 fixture `make_geom()` 硬编码；format_profile 无 WAL 字段 | INC-034（disk-format 字段必须收口 profile，不进 runtime config）；ODF §2.2 superblock 含同名三字段 | **WAL 三字段进 `format_profile`**（§5.1），builder 从 profile 构造 `segment_geometry`；`build_options` 不出现 WAL 布局字段 |
| queue_depth 双用途 | 无对应（旧 front 无 prepare FIFO） | front ctor 把 `queue_depth` 同时用作请求 ring 容量与 `wal_pending_prepare_capacity_`（045 §5.D / 047 §17.4.1 watch-item） | 047 §7：ring full = 配置错误 fail-fast，prepare full = 软背压 client error——两个语义不同的闸门不应共享一个旋钮 | **解耦**（§5.2）：`wal_append_config.pending_prepare_capacity`，0 = 跟随 queue_depth（既有形态，m08 测试 9 等依赖耦合的测试不改而绿），非 0 = 独立容量；production builder 显式设置 |

## 4. 冲突与裁决

1. **front 栈是否可选**：flush_e2e 既有 build_runtime 调用不含 front。
   裁决：**front 栈总是构造**（INC-034 同源哲学："标准 runtime 总是
   构造 value scheduler"；不留"半个引擎"形态）。默认拓扑 = 每个
   `cores` 成员一个 front、coord 与 wal_space 落 `cores[0]`；
   `build_options` 新字段全带默认值 → flush_e2e 调用点零改动（fronts
   闲置：front WAL segment 为惰性分配，首次 append 才触发，idle 无
   I/O；coord/wal 空转 advance 仅位图探测）。
2. **registry vs 全局 runtime 对象**：见 §3 行 1/2，保留 registry。
3. **`nvme_by_front_owner` 的归属**：ops 每次 compose 重建映射 =
   每调用一次 vector 分配（违反热路径预算）。裁决：builder 构建一次
   存入 registry（write-once vector），ops 取 span（§6.3）。
4. **rt::write_batch 的 in-gate 测试边界**：registry 的 nvme 槽位
   类型是 `nvme::runtime_scheduler*`（真实 SPDK 类型，INC-002 后无
   mock 同型物），fake NVMe 无法注册 → `rt::write_batch` 的带 I/O
   全链在 fake 环境不可达。裁决：**不为测试改 production 类型、不
   引注入面**；in-gate 覆盖 = 解析层（compose 从 registry 取拓扑 +
   零副作用专测）+ 拓扑元组正确性直测，带 I/O 全链显式留 M13
   mock-device 分层（dev plan M13 26L/26O 本就是该层的落点）。底层
   write 链语义已被 m08/m09/m10 全覆盖，rt 层增量仅解析。
   `rt::point_get` 不经 nvme_by_front_owner（value 读经 NvmeProvider
   注入）→ **可以在 m11 内做带 I/O 全链 e2e**，做满。
5. **队列容量校验位置**：`per_core::queue` 对非 2 的幂容量直接
   abort（pump 既有行为）。builder/`build_front_topology` 先行校验
   `front_queue_depth` / `coord_queue_depth` 为 2 的幂且 ≥ 2（ring
   可用槽 = 容量-1，047 §12.2.9），违例 `std::invalid_argument`——
   把进程 abort 提前成可诊断的配置错误。
6. **builder 注释修正**：`build_options` 上方"tree allocator grows
   from value_data_area_base downward"与实现相反（`tree_allocator::
   allocate` 自 `value_data_area_base` 向高地址 bump，对向碰
   `value_head_lba`）。Phase C 改 builder 时顺带修正该注释（两行，
   不另立提交）。

## 5. Disk-Format 与 L2 增量

### 5.1 `format_profile` WAL 字段（Phase A）

```cpp
struct format_profile {
    // …既有字段…
    // ── WAL 区参数（ODF §2.2 superblock 同名字段；M11 起 builder
    //    从 profile 构造 segment_geometry，WAL 布局不进 runtime config）──
    paddr    wal_base_paddr;
    uint32_t wal_segment_size;
    uint32_t wal_segment_count;
};
```

`kBootstrapFormatProfile` 取值（dev 默认，同 tree 几何的"非天花板非
要求"注释口径）：

```cpp
.wal_base_paddr    = paddr{0, 8},        // 2 superblock + 6 reserved（ODF §7）
.wal_segment_size  = 256u * 1024u,       // 64 LBAs/segment（dev 形态；ODF §3.6 的 ≥4MiB 是生产建议非结构约束）
.wal_segment_count = 32,                 // WAL 区 [8, 2056) < value_data_area_base.lba 4000
```

`profile_is_self_consistent` + builder tier-2 校验新增（结构规则）：

1. `wal_segment_size != 0` 且 `wal_segment_size % lba_size == 0`；
2. `wal_segment_count >= 1`；
3. `wal_base_paddr.device_id == value_data_area_base.device_id`；
4. `wal_base_paddr.lba >= 2`（superblock 双槽之后）；
5. `wal_base_paddr.lba + wal_segment_count * (wal_segment_size /
   lba_size) <= value_data_area_base.lba`（WAL 区静态落在 Data Area
   之外；tree 分配器不会低于 `value_data_area_base`，故无运行期对向
   碰撞——value/tree 间的对向协议另属 INC-054）；
6. `wal_segment_size` 扣除 header 与 trailer 预留后大于 v1 最大
   entry 编码长度（ODF §3.6；用 `format/wal.hh` 既有常量表达）。

**聚合构造点全量更新**：`format_profile` 是 constexpr 聚合 +
`static_assert(profile_is_self_consistent(...))`；新增字段会让任何
未更新的聚合初始化点拿到零值并被校验拒绝。Phase A 必须 grep 全部
`format_profile` 构造点（production + `test_flush_e2e` 若有自构
profile）一并补齐字段；这是编译期/构造期 fail-fast，不存在静默路径。

### 5.2 WAL prepare FIFO 容量解耦（Phase B，关闭 047 §17.4.1）

```cpp
struct wal_append_config {
    uint32_t max_fua_inflight = 16;
    uint32_t max_pages_per_plan = 16;
    // 0 = 跟随 front ctor 的 queue_depth（既有耦合形态，所有既有
    //     调用点行为不变）；非 0 = WAL pending prepare FIFO 的独立
    //     软容量（045 §5.D 的 backpressure 闸门）。
    uint32_t pending_prepare_capacity = 0;
};
```

front ctor：`wal_pending_prepare_capacity_ =
cfg.pending_prepare_capacity != 0 ? cfg.pending_prepare_capacity
: queue_depth`。`validate_wal_append_config` 不增非零约束（0 是合法
"跟随"语义，不是 sentinel hack——写明注释）。

语义依据（047 §7 表）：请求 ring 满 = 部署容量错误 → fail-fast
abort；prepare FIFO 满 = 写过载 → `prepare_queue_full` → release →
客户端可重试。两个闸门语义不同，production 必须可独立配置：ring 深
（吸收读写混合消息洪峰）不应隐含"WAL 背压阈值同样深"（每个排队
prepare 挂着一个 in-flight batch 的 value+WAL 资源）。

production 默认（builder `build_options.front_wal_config` 默认值）：
`pending_prepare_capacity = 64`。数字依据：页级 FUA 并发上限
`max_fua_inflight = 16`，单 plan ≤ `max_pages_per_plan = 16` 页——
喂满 FUA 管道只需 1-2 个 plan 在飞 + 少量排队；64 提供 4× 余量吸收
batch 到达抖动，同时把单 front 最大排队 WAL 工作量约束在 64 个
batch 的 fragment 范围内（对照 ready_window 65536 的全局在飞上限，
单 front 背压远早于全局窗口耗尽，符合 047 §7 第 2 层先于第 1 层
触发的设计取向）。该值是部署旋钮，非格式参数。

既有测试兼容性证据：m08 测试 9（依赖耦合形态构造 prepare 溢出）与
m05/m06/m09/m10 全部不改而绿 = 0-默认等价性证据。

## 6. Registry 增量（Phase C）

```cpp
// core/registry.hh
namespace apps::inconel::front { class front_sched; }
namespace apps::inconel::wal   { class wal_space_sched; }   // 实际声明形态以代码为准

struct front_list {
    std::vector<front::front_sched*> list;      // 下标 == owner_id
    std::vector<front::front_sched*> by_core;   // core_id → 实例或 nullptr
};
inline front_list front_scheds;
inline wal::wal_space_sched* wal_space_sched_singleton_ptr = nullptr;

// owner_id → 该 front home core 的 nvme scheduler（write_batch 的
// nvme_by_owner 实参）。builder 在 nvme 构造完成后一次性回填；
// write-once，ops 取 span 借用。
inline std::vector<nvme::runtime_scheduler*> nvme_by_front_owner;
```

Typed accessors（house 同款 assert/panic）：

```cpp
front::front_sched* front_at(uint32_t owner);        // 越界/未注册 fail-fast
uint32_t            front_count();
std::span<front::front_sched* const>       fronts_span();
wal::wal_space_sched*                      wal_space_singleton();
std::span<nvme::runtime_scheduler* const>  nvme_by_front_owner_span();
```

纪律（旧分支 §2.2/§2.3 的承接）：

1. **不提供** `local_front()` / `front_for_core()` / 任何 mixed
   owner 入口——front 是 semantic-first owner，实例选择只能来自
   key hash（ops/pipeline 层完成），locality 元数据只服务 nvme 映射。
2. `clear()` 复位全部新字段（front list/by_core、wal singleton、
   nvme_by_front_owner）。
3. 头文件只 forward-declare front/wal 类型（registry 存指针不解引用），
   保持 registry 轻量。若 wal_space_sched 的声明形态（class/struct +
   模板性）使 forward declare 不可行，允许 include 对应 scheduler.hh
   并在实现报告中声明。

## 7. `build_front_topology`（Phase C，device 无关 seam）

### 7.1 Options

```cpp
struct front_topology_options {
    std::span<const uint32_t> cores;            // 宿主核全集（校验用）
    std::span<const uint32_t> front_cores;      // 每核一个 front；空 = cores 全集
    int32_t coord_core = -1;                    // -1 → cores[0]
    int32_t wal_space_core = -1;                // -1 → coord core
    wal::segment_geometry wal_geometry;         // builder 从 profile 构造
    const core::tree_geometry* tree_geometry;   // 初始空 manifest 用
    std::size_t front_queue_depth = 1024;       // 2 的幂且 ≥ 2
    std::size_t coord_queue_depth = 1024;       // 同上
    std::size_t coord_ready_window = 65536;     // coord ctor 既有约束透传
    wal::wal_append_config front_wal_config =
        { .pending_prepare_capacity = 64 };     // §5.2 production 默认
};
```

### 7.2 构造顺序（单一函数内，每步 fail-fast）

```text
1. 校验：front_cores ⊆ cores 且无重复（空则取 cores 全集）；
   coord_core / wal_space_core ∈ cores；front/coord queue_depth 为
   2 的幂且 ≥ 2；wal_geometry / tree_geometry 非空自洽。
   违例 std::invalid_argument。
2. N = front_cores.size()。逐 front 预创建 active gen：
   front::make_front_memtable_gen(i, N, /*epoch*/0, active)
   （gen 无核亲和，可在 builder 线程统一创建；gen_id stride 由
   helper 保证，043 §4.2）。
3. 构造初始 CAT（boot-from-empty）：
   guard = checkpoint_guard{ manifest =
       make_shared<const tree_manifest>(tree_manifest::empty(geom)) }
   PRS  = { tree_guard = guard,
            fronts = [ {active = gen_i, imms = {}} ]×N, epoch = 1 }
   CAT  = publish_catalog{ PRS, durable_lsn = 0, epoch = 1 }
   不变量：PRS.fronts[i].active 与 front i 的 ctor initial_active 是
   **同一** shared_ptr（047 §12.1.3 fixture 不变量升格为 production
   构造规则）。
4. 在 wal_space_core 的 this_core_id 上下文构造
   wal_space_sched(wal_geometry, N)；注册 singleton。
5. 逐 front：this_core_id = front_cores[i]，构造
   front_sched(i, N, gen_i, /*next_epoch*/1, wal_geometry,
   front_wal_config, front_queue_depth)；注册 list[i] +
   by_core[front_cores[i]]。
6. 在 coord_core 的 this_core_id 上下文构造
   coord_sched(CAT, N, /*next_lsn*/1, coord_ready_window,
   coord_queue_depth)；注册 singleton。
7. 返回 raw 指针集（house 风格：raw new + destroy 收尾；INC-033
   把 init 失败定义为 process-fatal）。
```

### 7.3 与 build_runtime 的关系

`build_runtime` 在主 per-core 循环**之前**调用
`build_front_topology`（front 栈不依赖 nvme），把返回的指针并入
主循环的 `add_core_schedulers`（pump tuple 一次性全包设置 + all-null
断言，不允许事后补槽）；循环结束后回填
`registry::nvme_by_front_owner[i] = nvme_scheds.by_core[front_cores[i]]`。
this_core_id 在主循环内仍逐核重设，front 栈构造期的切换无残留影响。

### 7.4 Recovery hook（声明，不实现）

recovery 迁移（M-line 之外）落地时，CAT_clean / next_lsn /
recovered gens 将替换本步第 2-3/6 步的 boot-from-empty 输入；
`build_front_topology` 的参数面已按"初始 CAT 可注入"形态预留
（coord ctor 本就消费外部 CAT），届时不需要改 registry/ops 层。

## 8. Builder / Runtime Tuple（Phase C）

```cpp
template <core::cache_concept TreeCache, core::cache_concept ValueCache>
using inconel_runtime_t = pump::env::runtime::global_runtime_t<
    nvme::runtime_scheduler,
    core::tree_read_domain<TreeCache>,
    value::value_alloc_sched<ValueCache>,
    tree::tree_sched,
    coord::coord_sched,
    front::front_sched,
    wal::wal_space_sched
>;
```

- `add_core_schedulers(core, nvme, rd, value, tsched, coord_or_null,
  front_or_null, wal_or_null)`；非宿主核给 nullptr，`rt::run()` 的
  thunk 捕获自动跳过空槽（既有机制，零热路径新增）。
- `build_options` 新增字段（全带默认，flush_e2e 调用点零改动）：
  `front_cores`（空 = 全核）、`coord_core = -1`、`wal_space_core = -1`、
  `front_queue_depth = 1024`、`coord_queue_depth = 1024`、
  `coord_ready_window = 65536`、`front_wal_config = {…capacity 64}`。
- `validate_build_inputs` tier-2 增 §5.1 的 WAL 校验，tier-4 增
  front/coord/wal core 成员检查（§7.2.1 在 helper 内亦检——builder
  与独立调用 helper 的测试共用同一套校验，不双轨）。
- `destroy_runtime`：依既有逆序补 coord → fronts → wal_space 的
  delete（在 tree_sched 之前或之后均无依赖，选固定顺序写明）；
  `registry::clear()` 已覆盖新字段复位。

## 9. Operation Surface（Phase D，`runtime/operations.hh`）

```cpp
// namespace apps::inconel::rt — 与 facade 同一对外命名空间。
// 自包含：include write_path/write_batch.hh + pipeline/point_get.hh
// + core/registry.hh（+facade.hh）。

template <typename NvmeProvider = value::local_nvme_provider>
[[nodiscard]] inline auto
write_batch(core::client_batch_buffer&& input, NvmeProvider value_nvme = {}) {
    return write_path::write_batch(
        *core::registry::coord_sched_singleton(),
        core::registry::fronts_span(),
        *core::registry::wal_space_singleton(),
        core::registry::nvme_by_front_owner_span(),
        std::move(input),
        value_nvme);
}

template <typename NvmeProvider = value::local_nvme_provider>
[[nodiscard]] inline auto
point_get(std::string_view key, NvmeProvider value_nvme = {}) {
    return pipeline::point_get(
        *core::registry::coord_sched_singleton(),
        core::registry::fronts_span(),
        key, value_nvme);
}
```

要点：

1. **解析在 compose 期**：几次指针/span 载入；未注册 → accessor
   fail-fast（比 submit 后挂起可诊断）。spans 借用 registry
   write-once 存储，生产生命周期 = 进程；测试须保证 registry 在
   sender 存活期内不 clear（§11 表）。
2. **零行为增量**：委托后与 M09/M10 显式签名逐字节同链；底层校验
   （拓扑尺寸、PRS 一致性）原样生效，ops 不重复校验。
3. **consuming write entry**（`&&`）与 **borrowed read key**（view +
   caller-pin）：§3 对照表行 4 的裁决。
4. NvmeProvider 注入维持（M07 先例）；tree 路径照旧不注入。
5. 不建 umbrella；M12 的 seal op 届时加进同一文件或并列文件，由
   M12 设计裁决。

## 10. 错误 / 失败语义总表

| 场景 | 抛出点 | 类型 | 备注 |
|---|---|---|---|
| 拓扑配置错（front_cores ∉ cores / 重复 / coord 核非法 / depth 非 2 的幂或 < 2） | `build_front_topology` 校验 | `std::invalid_argument` | run_with 路径下经 INC-033 → panic |
| profile WAL 字段非法 | `profile_is_self_consistent`（编译期 static_assert）/ `validate_build_inputs`（运行期） | 编译错 / `std::invalid_argument` | 聚合点漏更新 = 零值被拒，无静默 |
| ops 在未注册 registry 上 compose | accessor | assert / panic | fail-fast；不提供"半注册可用"形态 |
| `rt::write_batch` 运行期 | 委托 M09 | 与 048 §10 总表逐条一致 | 解析层零新增失败形态 |
| `rt::point_get` 运行期 | 委托 M10 | 与 049 §10 总表逐条一致 | 同上 |
| prepare FIFO 独立容量溢出 | front WAL prepare | `wal_append_error{prepare_queue_full}` | 语义同 045 §7.1，仅容量来源解耦 |

## 11. Lifetime 契约

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| coord/front/wal 实例 | builder raw new，registry 持指针 | destroy_runtime（生产）/ 测试 fixture 显式 delete + clear | house 既有模式（tree/value/nvme 同款） |
| 初始 CAT / PRS / guard / 空 manifest | shared_ptr 链（coord cats_ + 各 read_handle） | 引用归零 | M02/M03 既有语义 |
| front active gens | front_state + PRS 双持（同一 shared_ptr） | gen 生命周期规则（RSM §8.4） | §7.2.3 不变量 |
| registry 三个新容器 | registry（write-once until clear） | clear / 进程退出 | ops spans 借用其存储；测试在 sender 存活期内不得 clear |
| ops 返回的 sender | 调用方 | submit 后由 PUMP scope 管理 | 与 M09/M10 相同 |

## 12. 热路径预算与容量估算

热路径：

| 路径 | M11 新增成本 | 说明 |
|---|---|---|
| `rt::write_batch` / `rt::point_get` compose | ~5 次指针/span 载入（registry 读） | 对照显式签名形态**零运行期增量**；无新 alloc/copy/hop |
| 写/读链运行期 | 0 | 委托链与 M09/M10 完全相同 |
| 每核 advance 循环 | 宿主核各 +1 thunk（coord/front/wal 各自核）；空闲 advance = 位图探测级 | run.hh 空槽捕获期剔除，非宿主核零成本 |
| prepare FIFO 解耦 | front ctor 一次三目选择 | 热路径零变化 |

容量（10 亿 KV 校准，新增 runtime carrier）：

| Carrier | 量级 | 估算 |
|---|---|---|
| registry 新容器 | front list/by_core + nvme_by_front_owner | ~3 × N × 8B + by_core(max_cores × 8B)，KB 级 |
| 初始 CAT/PRS/guard/空 manifest | 单套 | < 1 KB |
| **front_sched × N 实例** | 既有 M05 形态的 N 倍化（非本步新结构，但本步首次按部署 N 实例化，必须给数） | `per_core::queue` 每条预分配 129 lane × depth；11 条队列 × depth 1024 × 8B 指针 ≈ **~11.6 MB/front**（多为惰性缺页的虚拟映射）。32 front 部署 ≈ 370 MB 虚拟预留。部署旋钮 = `front_queue_depth`；**watch-item（§16.4）**：若实测常驻成压力，方向是 pump 队列惰性 lane 或按角色裁剪队列容量，属框架层议题不在本步动 |
| coord / wal_space 单例 | 各 1 | 同型队列预分配，coord ~6 队列 ≈ 6 MB 级，单例不随 N 放大 |

## 13. 测试计划

Target：`inconel_test_m11_runtime_topology_operations`
（`apps/inconel/test/test_m11_runtime_topology_operations.cc`，CMake
照 m10 模式注册，link `inconel_real_nvme`）。

### 13.1 Fixture

写侧复用 m10 蓝本（fake NVMe + value sched 13 参 + L3 write_batch
显式 spans 驱动写入）；拓扑构造改走 **production
`build_front_topology`**（fixture 自己的 cores={0}、fixture 几何、
小 depth——全部经 §7.2 校验路径），fixture 持有返回指针负责 delete +
registry clear。`advance_all` 加 coord/fronts/wal（经 registry 取，
顺带验证 accessor）。

### 13.2 测试列表

1. `m11_front_topology_builds_and_registers`
   build_front_topology(2 fronts) → coord/wal singleton 非空；
   `front_count()==2`、`front_at(i)` 与 `fronts_span()[i]` 一致、
   `by_core[front_core_i] == list[i]`；初始 CAT：durable_lsn 0、
   epoch 1、PRS.fronts[i].active 与 front i 当前 active 是**同一**
   shared_ptr（经 use_count/指针相等断言）；coord 首个 assign 得
   lsn 1（boot-from-empty 自洽）。
2. `m11_front_list_by_core_stable`
   多次 accessor 调用返回同一存储（span data() 恒等）；list 顺序 ==
   owner_id 顺序；非宿主核 by_core 槽为 nullptr。
3. `m11_ops_point_get_end_to_end`
   production 拓扑（registry）+ fixture fake NVMe：L3 write_batch
   写入 PUT/DELETE → `rt::point_get(key, fake_provider)` found/正确
   body；DELETE key → not_found；missing → not_found。**rt 读面
   带 I/O 全链 in-gate 覆盖**（§4.4）。
4. `m11_ops_write_batch_composes_from_registry_without_side_effect`
   `rt::write_batch(input)` compose 后销毁（不 submit）→ 各 owner
   零活动（fake NVMe total_calls 增量 0、下一 batch 经 L3 全链后
   ack.lsn 仍按序、front memtable 无变化）。解析层 + 零副作用纪律
   （M08/M09/M10 同款测试 7 形态）。
5. `m11_wal_pending_prepare_capacity_decoupled`
   front 以 `queue_depth = 1024` + `pending_prepare_capacity = 2`
   构造（经 build_front_topology options）；held fake NVMe 下 4 个
   DELETE-only batch 走 M08 phase senders：第 1 个占 pending plan、
   第 2/3 进 FIFO、第 4 个 `prepare_queue_full` → release（深 ring
   下浅背压生效——解耦的行为面证据；m08 测试 9 不改而绿是 0-默认
   的等价性证据）。
6. `m11_topology_validation_failures`
   front core ∉ cores / front_cores 重复 / coord_core ∉ cores /
   queue_depth 非 2 的幂 / queue_depth < 2 → 各 `invalid_argument`；
   失败后 registry 无半注册残留（构造顺序内先校验后 new，或失败
   路径 clear——实现选一并在报告声明）。
7. `m11_operations_header_self_contained`
   测试 TU 以 `apps/inconel/runtime/operations.hh` 为**第一个**
   include（文件结构即断言：单独可编译）。
8. `m11_profile_wal_fields_validated`
   对 `validate_build_inputs` 不可达的 device 前置（tier 1 先抛），
   改为直测 `profile_is_self_consistent`（constexpr 函数运行期可
   调）：构造若干非法 WAL 取值的 profile（错对齐 / 区间越过
   data_area_base / count 0 / usable 过小）逐一为 false；
   `kBootstrapFormatProfile` 为 true。

### 13.3 回归门

每 Phase：`cmake --build build` 全 target +
`inconel_test_m01..m11` 十一个二进制全 PASS（m05/m06/m08/m09/m10
不改而绿 = §5.2 解耦 0-默认与 §8 build_options 默认值的等价性证据）；
收尾另跑 `build_asan` 同名十一个全 PASS。`inconel_test_flush_e2e`
两套构建维持编译通过（其 build_runtime 调用点零改动是 §4.1 裁决的
直接验收）。

### 13.4 声明的 in-gate 边界（不是静默缺口）

`rt::write_batch` 带 I/O 全链 e2e 在 fake 环境不可达（§4.4 类型
事实），本步以"解析层直测（测试 1/2/4）+ 底层链 m08/m09/m10 全
覆盖"为 in-gate 证据，带 I/O 全链留 M13 mock-device 分层与真盘
smoke。实现总报告必须原样声明此边界。

## 14. 实现顺序（每 Phase 一个提交）

```text
Phase A  format/format_profile.hh：WAL 三字段 + 校验 + bootstrap 取值 + 全部聚合点更新
Phase B  front/wal_append.hh + front/scheduler.hh：pending_prepare_capacity 解耦
Phase C  core/registry.hh 增量 + runtime/builder.hh（front_topology_options /
         build_front_topology / tuple 7 类 / build_options / destroy / 注释修正）
Phase D  runtime/operations.hh：rt::write_batch / rt::point_get
Phase E  CMake 注册 m11 target + fixture + 测试 1-8
Phase F  全量回归（Release + ASAN，m01-m11）+ 总报告（声明跳过项与 §13.4 边界）
```

依赖：C 依赖 A/B；D 依赖 C；E 依赖 D。**Phase A-D 是 production 实现
阶段，禁止打开任何测试文件**；Phase E 以 M11 测试作者身份工作，允许
读的既有测试白名单：`test/check.hh`、
`test_m10_point_get_live_read.cc`（fixture 蓝本 + fake NVMe +
rt 读面驱动模式）、`test_m09_production_write_batch.cc`（write
fixture / hold）、`test_m08_write_baseline_inflight.cc`（phase
senders 驱动，测试 5 用）、
`test_m03_coord_scheduler_assign_publish_release.cc`（CAT/expect_throws
模式）；不得修改任何既有测试文件。

## 15. 排除范围

1. seal/flush operation surface、publish gate 编排（M12）。
2. mock NVMe device 分层、`rt::write_batch` 带 I/O e2e、多核运行、
   真盘 smoke（M13）。
3. recovery 接线（CAT_clean 注入，§7.4 hook 已留）。
4. MultiGet/Scan、INC-021、batch_lookup/scan 的 borrowed 形态。
5. pump 框架改动（per_core::queue 预分配形态、tuple 机制）。
6. INC-054 对向边界协议；INC-056。
7. 网络/协议层 ingress。

实现若需要以上任何一项才能编译，必须停下报告，不得用通用名伪装。

## 16. 相邻事项

1. **M12**：seal_once operation 进 `runtime/operations.hh` 同面；
   coord gate handles 已在（M03），M12 编排消费本步 registry 拓扑。
2. **M13**：mock device 分层落地后补 `rt::write_batch` 带 I/O 全链
   + 多核矩阵（dev plan 测试 1-13）；§13.4 边界在彼处关闭。
3. **plan 回填**：`front_wal_development_plan.md` M11 节标注"M11 的
   详细设计文档是 050_runtime_topology_operation_surface_design.md"。
4. **watch-item（新）**：front_sched × N 的 per_core::queue 预分配
   虚拟内存（§12 表）；若部署实测成压力，方向是框架层惰性 lane /
   按角色容量裁剪，挂 pump 议题不进 known_issues。
5. **known_issues**：047 §17.4.1（queue_depth 双用途）由 §5.2 关闭
   （wrap-up 时在 047 §17.4 加一行指向本文）；无新增条目预期。

## 17. 需要人工判断的点

无阻塞项。topology 容器（registry 扩展 vs 全局对象）、install-once
（不迁移）、front 栈默认必建、WAL 字段入 profile、prepare 容量解耦
与默认值 64、`rt::write_batch` in-gate 边界（留 M13）均已在 §3/§4
记录依据。两条硬停线：（a）若 `add_core_schedulers` 全包语义或
all-null 断言使 front 栈预构造方案不可行（§7.3），停下报告；（b）若
wal_space_sched / front_sched 的声明形态使 registry forward-declare
与 tuple 实例化产生不可调和的 include 环，停下报告——不得为绕环把
L2 头并进 registry 之外的奇怪位置。

## 18. Review 对账（2026-06-12，M11 实现 land 记录）

实现提交：`ffd95ae`(A profile WAL 字段) → `e1fc660`(B prepare 容量
解耦) → `dce72ef`(C registry + builder 拓扑接线) → `85ec970`(D
operations surface) → `9f9552c`(E CMake + 测试 1-8) → `df06bfe`(F 空
回归 marker，一次同义重写替换 b141cba)。production 变更 6 文件
（净增 ~600 行）+ CMake + 新测试；`pump/`、`ai_context/`、既有测试、
`test_flush_e2e.cc` 零触碰（§4.1/§13.3 验收项直接成立）。§17 两条
硬停线均未触发（7 类 tuple 与 forward-declare 均成立）。

### 18.1 语义对照结论

§13.2 的 8 个测试全部落地，**无跳过无降级**（实现方总报告声明，
review 独立核对成立）。要点与接受的实现形态：

1. Phase A 校验比设计稿强一档：WAL 区间计算带 uint64 溢出防御；
   `wal_put_entry_size(1024)` 的 1024 是 ODF §3.6 v1 MAX_KEY_LEN
   字面量（`WAL_PUT_MAX_KEY_LEN` 是 uint32 溢出界、语义不同不可
   代用）——具名 policy 常量留待格式层下次触碰顺带（cosmetic）。
2. Phase B 解析点收在 `configure_wal`（config 安装与容量解析同点）；
   `validate_wal_append_config` 显式注释 0 为合法"跟随"语义。
3. Phase C 含设计稿之外的**双重构建守卫**（registry 已有 front/coord/
   wal 拓扑时 `logic_error`）与 nvme_by_front_owner 回填的 null
   守卫；`build_front_topology` 全部校验先于任何 new/注册 →
   失败路径无半注册残留（测试 6 直接断言）。
4. Phase E 测试 1 用非对称拓扑（fronts {1,3} / coord 0 / wal 2）
   验证 role 放置，比设计稿对称形态强；§7.2.3 不变量（PRS active
   与 front 当前 active 同一 shared_ptr）直接断言。
5. **测试 4 的 nvme_by_front_owner 形态**：fixture 填 N 个 nullptr
   占位（span 非空过 accessor、compose 不解引用、测试不 submit）——
   "present but unusable" 的 test-local 表达；production 填表路径由
   builder 的 logic_error 守卫覆盖。接受。
6. `wal_geometry_from_profile` 省略 `expected_format_version` 安全：
   `segment_geometry` 默认成员初始化即 `SUPERBLOCK_FORMAT_VERSION_V1`
   （review 核对 core/wal_stream.hh）。
7. **设计定位错误修正**：§1 把"grows downward"陈旧注释定位在
   builder.hh，实际在 format_profile.hh（codex 按字面执行未触达，
   总报告如实声明"builder 无此注释"）；该注释已在 wrap-up 提交中
   修正为 ODF §10.4 的双向生长事实。

### 18.2 运行效率审计（独立小节）

- **operation surface 零运行期增量**：`rt::write_batch` /
  `rt::point_get` 的 registry 解析发生在 compose 期（~5 次指针/span
  载入），委托链与 M09/M10 显式签名形态逐字节同链；无新增
  alloc/copy/queue hop/atomic。`nvme_by_front_owner` 由 builder 一次
  构建，ops 借 span——无每请求映射重建（§4.3 预算兑现）。
- **每核 advance 循环增量**：仅宿主核各 +1 thunk（coord/front/wal），
  非宿主核 tuple 空槽在 `rt::run()` 捕获期剔除为零成本；空转
  advance = per_core 位图探测级。
- **prepare 容量解耦**：front ctor 一次三目 + 8B `queue_depth_`
  成员；热路径零变化。production 默认 64 的数字依据（§5.2：FUA
  管道 16×16 页 1-2 plan 喂满，4× 抖动余量）成立。
- **容量核销**：registry 新容器 KB 级；初始 CAT 套件 < 1KB；
  front × N 的 per_core::queue 预分配（~11.6MB/front 虚拟）按 §12
  表登记为部署 watch-item，本步未对 pump 做任何改动。
- **隐藏成本点名**：build_front_topology 的校验是 O(F²) 重复检查
  （front_cores 去重）——构造期一次性冷路径，F ≤ 核数，可忽略；
  无新增 push_context 节点。

### 18.3 独立验门记录（不采信实现方自报）

`cmake --build build` 全 target 0 错；Release `inconel_test_m01..m11`
11/11 PASS；`cmake --build build_asan` 全 target 0 错；ASAN 同名
11/11 PASS、无 AddressSanitizer/LeakSanitizer 输出；
`inconel_test_flush_e2e` 两套构建产物在且源文件 diff 为零（§4.1
验收项）；变更范围核查（8 个预期文件）与残留扫描（production 无
TODO/stub/step-phase 标识符、无残留进程）通过。

### 18.4 遗留 watch-item

1. front × N 的 per_core::queue 预分配虚拟内存（§12/§16.4）：部署
   实测后若成压力走框架层议题（惰性 lane / 按角色容量裁剪）。
2. `rt::write_batch` 带 I/O 全链 e2e：§13.4 声明边界，M13
   mock-device 分层关闭。
   （2026-06-12 已关闭：052 phase C/D，m13 矩阵 1-12 经
   `rt::write_batch` 带 I/O 全链 + 多核并发。）
3. ODF §3.6 v1 MAX_KEY_LEN 的具名 policy 常量（§18.1.1 cosmetic）。
4. 047 §17.4.1（queue_depth 双用途）由本文 §5.2 关闭；049 §17.4
   其余项维持。不新增 known_issues 条目。
