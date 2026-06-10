# 039 — Front/WAL Phase A：Carrier + INC-055 Memtable Shape

> 本文是 `front_wal_development_plan.md` 里 M01 的详细设计文档。
> M01 只冻结 shared batch carrier 和 INC-055 之后的 memtable shape。
> 本文不设计 WAL append、coord ready bitmap / publish gate，也不设计
> `write_batch` sender pipeline。

## 1. 范围

M01 的产物是可被后续 coord/front/wal/pipeline 共同引用的 L2/L3
边界数据结构。它必须能表达旧 `inconel` 分支 Step 7 / Step 10 /
Step 26I / Step 26J/K 已经验证过的 batch、fragment、memtable
语义，但必须按当前正式设计文档和 INC-055 改掉旧 `hot_blob` 路线。

M01 落点：

1. `apps/inconel/core/batch_carrier.hh`：
   batch input buffer、batch view、canonical entries、front fragments、
   `batch_ctx`。
2. `apps/inconel/core/memtable.hh`：
   只含 `value_ref` 的 `value_handle`、`memtable_entry`、只保存 key 的
   `gen_arena`、`memtable_gen`、`front_read_set`、memtable lookup result
   carrier。
3. M01 级合约测试：
   canonicalization/routing/lifetime、INC-055 memtable shape、arena key
   ownership。

M01 明确不做：

1. WAL byte layout、WAL segment allocation、FUA issue、WAL decode。
2. Coord LSN assignment、ready bitmap、publish/release、read_handle。
3. `write_batch` pipeline state、sender chain、bounded fan-out、failure
   recovery。
4. front scheduler owner methods。M01 只提供后续 owner 要消费的类型。

## 2. 已检查输入

旧分支证据：

1. `inconel:apps/inconel/runtime/front/memtable.hh`
2. `inconel:apps/inconel/runtime/batch_ctx.hh`
3. `inconel:apps/inconel/test/step_07_memtable_contract_test.cc`
4. `inconel:apps/inconel/test/step_10_batch_ctx_contract_test.cc`
5. `inconel:apps/inconel/test/step_26i_memtable_arena_contract_test.cc`
6. `inconel:apps/inconel/test/step_26j_network_ingress_contract_test.cc`
7. `inconel:apps/inconel/test/step_26k_phase4_write_baseline_contract_test.cc`

当前分支证据：

1. `apps/inconel/core/memtable.hh`
2. `apps/inconel/core/value_ref.hh`
3. 当前分支尚无正式 `batch_ctx` / front / coord / wal carrier 实现。

正式设计依据：

1. `ai_context/inconel/design_doc/code_modules.md`
2. `ai_context/inconel/design_doc/code_quality_standard.md`
3. `ai_context/inconel/design_doc/cross_doc_contracts.md`
4. `ai_context/inconel/design_doc/write_path_and_pipeline.md`
5. `ai_context/inconel/design_doc/read_api_and_pipeline.md`
6. `ai_context/inconel/design_doc/runtime_state_machine.md`
7. `ai_context/inconel/design_doc/runtime_memory_and_cache.md`
8. `ai_context/inconel/design_doc/flush_and_frontier_switch.md`
9. `ai_context/inconel/design_doc/on_disk_formats.md`

## 3. 语义来源对照表

### 3.1 Batch Carrier

| 项目 | 旧 `inconel` 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 039 决议 |
|---|---|---|---|---|
| op kind | Step 10 使用 `raw_op_type::{put,del}`。Step 26J/K 继续把它用于 network view。 | 无当前 carrier。 | 写路径 canonical entry 区分 PUT 和 DELETE。 | 在 `core` 定义 `write_op_type::{put,del}`。这是语义 op kind，不绑定 raw ingress。 |
| `raw_batch_op` | Step 10 接受 owning `std::string key/value` op 并构建 `batch_ctx`。 | 无当前 carrier。 | 正式设计不要求 raw owning op。 | 只保留为 whitebox/test/legacy adapter。生产 ingress 先规范化成 `client_batch_buffer`，保证只有一种 lifetime 模型。 |
| `client_batch_buffer` | Step 26J 引入 owning contiguous buffer，用于 network-style ingress。 | 无当前 carrier。 | 代码质量要求 owner 明确，不能隐式依赖 caller stack。 | `client_batch_buffer` 拥有 `std::vector<std::byte> bytes`；它是 move-only、非持久化、非 WAL 格式。 |
| `client_batch_view` | Step 26J 从 buffer 解析 borrowed op views。 | 无当前 carrier。 | 后续 pipeline 需要 view-based zero-copy carrier。 | `client_batch_view` 是覆盖 `client_batch_buffer` 的非 owning validated view；格式错误在 parse/build 边界抛 `std::invalid_argument`。 |
| `canonical_entry` | Step 10 使用 owning strings。Step 26K 改成带 `std::string_view key/value` 和 `value_ref allocated_vr` 的 `canonical_view_entry`。 | 无当前 carrier。 | `canonical_entry` 字段包含 op、key、value、allocated `value_ref`；代码质量拒绝按 phase 重复复制 value。 | 生产 `canonical_entry` 采用 Step 26K 的 view 形态，但使用 canonical 名字：`write_op_type op`、`std::string_view key`、`std::string_view value`、`format::value_ref allocated_vr`。view 指向 `batch_ctx.input.bytes`。 |
| canonicalization | Step 10 先按 key 排序，保留每个 key 的最后 raw position，再按保留下来的 position 升序恢复 canonical order。Step 26K 对 view 保持相同规则。 | 无当前 carrier。 | 正式写路径要求每个 key 一个 canonical record，并定义 canonical record count。 | 精确保留 Step 10/26K 规则。同 batch 内 same-key last writer wins；输出顺序是 winning op 的原始位置升序。 |
| routing | Step 10/26K 使用 `key_hash(key) % front_count` 路由，并按 owner 排序 fragments。 | 无当前 carrier。 | 迁移计划要求 PUT/DEL 按 `key_hash % front_count` 路由。 | `build_batch_ctx` 接受 `front_count`；`front_count == 0` 抛 `std::invalid_argument`。fragments 按 owner 升序；同一 fragment 内 entry 保持全局 canonical order。 |
| fragment entries | Step 10/26K 的 fragment 保存指向 `canonical_entries` 的指针；测试只验证默认 move 后不悬挂。 | 无当前 carrier。 | 正式示例里仍有 pointer-style fragment，但代码质量要求 lifetime 证明明确；迁移计划优先 stable index 或 context-owned vector。 | 使用稳定的 `uint32_t entry_indices`，不用 `canonical_entry*`。index 指向 `batch_ctx.canonical_entries`，可承受 vector relocation 和 `batch_ctx` move，不需要 pointer repair。 |
| fragment metadata | Step 10/26K 的 fragment 携带 `owner`、`batch_lsn`、`entry_count`。 | 无当前 carrier。 | 正式 `fragment` 携带 owner、batch_lsn、global entry_count。 | 定义 `front_fragment { uint32_t owner; uint64_t batch_lsn; uint32_t entry_count; InlinedVector<uint32_t, 32> entry_indices; }`。`entry_count` 是全局 canonical count，不是本 fragment size。 |
| `put_entries` | Step 26K pipeline state 构建 `std::vector<canonical_view_entry*> put_entries`。 | 无当前 carrier。 | value persist 只消费 PUT entries。 | M01 在 `batch_ctx` 中暴露稳定 index 形式的 `put_entry_indices`。它不定义 value persist fan-out 或 pipeline state。 |
| `batch_ctx` ownership | Step 10 的 `batch_ctx` 拥有 canonical strings 和 pointer fragments。Step 26K 的 `batch_plan` 借用 `client_batch_buffer`；pipeline state 拥有该 buffer。 | 无当前 carrier。 | `batch_ctx` 是 shared core carrier；其 lifetime 必须覆盖 value/WAL/memtable phases。 | `batch_ctx` 先拥有 input buffer，再拥有 canonical entries、fragments、put indices。删除 copy。move 为 `noexcept`；view 仍有效，因为 `std::vector<std::byte>` 的 heap allocation 随对象 move 转移。 |
| `batch_pipeline_state` | Step 26K 有 pipeline-specific state，包含 input variant、plan、put_entries、wal_error。 | 无当前 carrier。 | pipeline state 属于后续写路径设计。 | 排除出 M01。后续 M08/M09 可以包装 `batch_ctx`，但 039 不定义 sender/pipeline state。 |

### 3.2 Memtable 字段与 Lookup Result

| 项目 | 旧 `inconel` 证据 | 当前 `inconel.new` 状态 | 正式设计依据 | 039 决议 |
|---|---|---|---|---|
| `value_handle::durable` | 旧 Step 7 在 `hot_blob` 旁边携带 `value_ref durable`。 | 当前 header 有 `format::value_ref durable`。 | cross-doc contract 规定 `value_handle` durable 是 `value_ref`。 | 保留 `durable`，并使其成为 `value_handle` 中唯一的 value payload。 |
| `value_handle::hot` / `value_view` | 旧 Step 7 使用 refcounted `hot_blob`；Step 26J/K 仍测试 hot value preservation。 | 当前 header 有兼容性的 `value_view hot`。注释说明它是 INC-055 之前的 legacy。 | INC-055 和 read docs 要求 memtable 不保存 value body；value residency 属于 `value_alloc_sched`。 | 移除 production `value_view` / `hot_blob`。测试必须断言 hot field 不属于正式 contract。 |
| `memtable_entry::data_ver` | Step 7 lookup 选择最大 visible version。 | 当前 header 有 `uint64_t data_ver`。 | 正式文档要求 `data_ver`，lookup 按 `data_ver <= read_lsn`。 | 保留 `uint64_t data_ver`；它是 canonical record 的 batch LSN。 |
| `memtable_entry::kind` | Step 7 有 value/tombstone 语义。 | 当前 header 有 `enum class kind { value, tombstone }`。 | cross-doc contract 要求 value/tombstone。 | 保留显式 kind。tombstone 不携带有意义的 `value_ref`。 |
| `memtable_entry::vh` | 旧 value entry 持有 durable + hot。 | 当前 `vh` 包含 durable + hot 兼容形态。 | 正式文档说 entry 保存 `value_handle vh`，且 `value_handle` 只有 durable。 | `vh` 只对 value entry 有意义，且只包含 durable `value_ref`。 |
| `gen_arena` chunks | Step 26I 引入 arena-backed key ownership；旧 Step 7 仍在 arena 外保留 value hot_blob。 | 当前 header 说目标是 key-only，但 legacy tests 可能分配 hot values。 | runtime state machine 和 memory/cache docs 规定 `gen_arena` 只拥有 key bytes。 | `gen_arena` 只拥有 key bytes：chunks、bump_next、bump_end。不保存 value bytes、WAL bytes 或 transient scratch。 |
| `memtable_gen::gen_id` | 旧 front gen 有 generation identity。 | 当前 header 有 `gen_id`。 | runtime state machine 要求 gen identity。 | 保留 `uint64_t gen_id`。 |
| `memtable_gen::state` | 旧分支有 active/sealing/sealed 风格状态。 | 当前 header 有 `memtable_gen_state`。 | runtime state machine 要求 active/sealing/sealed/flush states。 | 保留 state enum；M01 只定义 storage，不定义 transitions。 |
| `memtable_gen::front_owner_index` | 旧 front state 每个 front 拥有自己的 gens。 | 当前 header 有 owner index。 | runtime state machine 包含 front owner index。 | 保留 mandatory owner index。全 1 sentinel 只能用于构造完成前；live gen 必须有真实 owner。 |
| `memtable_gen::min_lsn/max_lsn` | 旧测试按 LSN 观察 visibility。 | 当前 header 有两者。 | runtime state machine 要求 min/max LSN。 | 保留并在 insert 时更新。empty gen 使用 `min_lsn = UINT64_MAX`、`max_lsn = 0`。 |
| `memtable_gen::kv_arena` | 旧 Step 26I 要求 key 不悬挂 caller stack，重复 key 不重复 materialize key storage。 | 当前 header 有 `kv_arena`。 | 正式文档说 `gen_arena` 保存 key bytes。 | 保留 key-only arena。insert 必须先 probe 现有 key，再按需分配新的 key copy。 |
| `memtable_gen::table` | 旧 Step 7 使用 key -> versions。Step 26I 要求 stable arena keys。 | 当前 header 使用 `absl::btree_map<std::string_view, InlinedVector<memtable_entry, 1>>`。 | runtime state machine 使用相同逻辑形态。 | 保留 arena-owned key view 到 per-key version vector 的 map。vector contract 是“该 gen 中此 key 的所有 versions”；lookup 必须选择最大 visible `data_ver`。 |
| `memtable_gen::loser_durable_refs` | 旧 flush/reclaim 路径跟踪 obsolete refs。 | 当前 header 有 `retire_list<retired_value_ref> loser_durable_refs`。 | flush docs 要求 memtable-only losers 记录在 owning gen 上。 | 保留该字段。M01 只定义 carrier；fold/reclaim population 后续再做。 |
| `front_read_set::active/imms` | 旧 read set pin active 和 immutable gens。 | 当前 header 使用 `shared_ptr<memtable_gen>`。 | memory/cache docs 定义 shared_ptr chain 是 correctness lifetime gate。 | 保留 `std::shared_ptr` pins；不使用 intrusive refcount。 |
| lookup result | 旧 Step 7 point-get 可以返回 hot value body。 | 当前分支没有最终 lookup result carrier；memtable header 仍暴露 hot-compatible value shape。 | cross-doc contract 规定 `variant<value_ref, tombstone, miss>`；read path 规定 memtable hit 后由 value_alloc read。 | 定义 typed result carrier：`memtable_value_hit{value_ref}`、`memtable_tombstone`、`memtable_miss`，以及 `using memtable_lookup_result = std::variant<...>`。不返回 value body。 |

## 4. Batch Carrier 设计

### 4.1 Input Buffer 与 View

`client_batch_buffer` 是生产 ingress bytes 的唯一 owner：

```cpp
struct client_batch_buffer {
    std::vector<std::byte> bytes;
};
```

这个 byte format 是 runtime 内部 ingress format。它不是 WAL 格式，不是
on-disk format，也不是 recovery contract。M01 使用 Step 26J 的测试 helper
形态来做 deterministic tests：record count 后跟重复的 op、key length、value
length、key bytes、value bytes。parser 在暴露 view 前必须验证所有边界。

`client_batch_view` 是 non-owning view，只在被引用的 `client_batch_buffer`
存活期间有效。它暴露一组：

```cpp
struct client_batch_op_view {
    write_op_type op;
    std::string_view key;
    std::string_view value;
    uint32_t original_position;
};
```

验证规则：

1. 未知 op code 非法。
2. truncated header 或 payload 非法。
3. DELETE 必须携带 0 字节 value。
4. batch parser 不解释 key bytes，除非检查边界。如果 M01 实现时已有独立
   key-format validator，builder 必须调用该 validator，不能在这里发明第二套规则。
5. PUT 允许 0 字节 value；value length 是语义 payload 长度，不是 presence flag。

外部输入错误从 parse/build helpers 抛 `std::invalid_argument`。内部 invariant
破坏，例如 fragment index 超出 `canonical_entries`，使用
`core::panic_inconsistency`。

### 4.2 Canonical Entries

生产 canonicalization 输出：

```cpp
struct canonical_entry {
    write_op_type op;
    std::string_view key;
    std::string_view value;
    format::value_ref allocated_vr;
};
```

`allocated_vr` 在 M01 中 default-initialized。后续 value persist 会在 WAL/memtable
phases 消费 fragment 前，为 PUT entries 填入它。DELETE entries 不使用
`allocated_vr`。

canonicalization 算法沿用 Step 10 / Step 26K：

1. 把 request 解析成带 original positions 的 op views。
2. 按 key 分组。
3. 每个 key 保留 original position 最大的 op。
4. 将保留下来的 ops 按 original position 升序排序。
5. 按该顺序 materialize `canonical_entries`。

这保持 batch 内 last-writer-wins，同时让 batch 的 observable order 稳定。

### 4.3 Fragments 与 Indices

Fragments 把 canonical entries 路由给 front owners：

```cpp
struct front_fragment {
    uint32_t owner;
    uint64_t batch_lsn;
    uint32_t entry_count;
    absl::InlinedVector<uint32_t, 32> entry_indices;
};
```

规则：

1. `owner = key_hash(entry.key) % front_count`。
2. 每个 fragment 的 `entry_count = batch_ctx.canonical_entries.size()`。
3. Fragments 按 `owner` 升序。
4. 同一 fragment 内 entries 保持全局 canonical order。
5. `entry_indices` 是指向 `batch_ctx.canonical_entries` 的 stable indices，
   绝不保存 raw pointers。

旧 Step 10/26K 的 pointer carrier 在旧代码里能工作，是因为测试只覆盖默认 move。
对当前 shared carrier 来说，这不够强：后续 vector growth、rebuild 或自定义 move
都可能静默制造 dangling pointers。stable indices 让 lifetime 规则变成机械约束。

### 4.4 `batch_ctx`

`batch_ctx` 是单个 canonicalized batch 的 M01 carrier：

```cpp
struct batch_ctx {
    client_batch_buffer input;
    uint64_t batch_lsn;
    uint32_t entry_count;
    std::vector<canonical_entry> canonical_entries;
    std::vector<front_fragment> fragments;
    std::vector<uint32_t> put_entry_indices;
};
```

对象属性要求：

1. 删除 copy construction 和 copy assignment。
2. move construction 和 move assignment 必须是 `noexcept`。
3. `input` 必须声明在 `canonical_entries` 前面，使 context move 时先转移 byte
   storage，再转移携带 `string_view` 指针的 entries。
4. `canonical_entry::key/value` 必须指向 `input.bytes`，不能指向 caller
   stack、temporary strings、fragment storage 或 WAL buffers。
5. `put_entry_indices` 按 canonical order 保存 PUT entries 的 canonical index。
   它是后续 value persist 的 helper，不是 pipeline state。

`build_batch_ctx(client_batch_buffer&& input, uint64_t batch_lsn,
uint32_t front_count)` 负责验证、canonicalize、route，并返回 move-only
`batch_ctx`。raw-op test adapter 必须先 encode/materialize 一个
`client_batch_buffer`，再调用同一个 builder。

## 5. INC-055 Memtable Shape

### 5.1 Value Handle

生产 `value_handle` 是：

```cpp
struct value_handle {
    format::value_ref durable;
};
```

没有 `value_view`、`hot_blob`、refcounted value body、inline value bytes，
也没有指向 client input 的指针。memtable hit 证明的是 durable value location，
不是 value-body residency。read path 必须使用 `value_alloc_sched.read_value()`
或后续等价接口 materialize value body。

这是有意偏离旧 Step 7 和当前 `apps/inconel/core/memtable.hh` 兼容代码的设计。

### 5.2 Memtable Entry

```cpp
struct memtable_entry {
    uint64_t data_ver;
    enum class kind : uint8_t { value, tombstone } type;
    value_handle vh;
};
```

规则：

1. `data_ver` 是 canonical record 被分配到的 batch LSN。
2. `type == value` 要求有有效 durable `value_ref`。
3. `type == tombstone` 忽略 `vh`；没有 value body，也没有 value allocation。
4. entry 必须能在 `absl::InlinedVector<memtable_entry, 1>` 中低成本 copy/move。

### 5.3 Key-only Arena

`gen_arena` 只拥有 key bytes：

```cpp
struct gen_arena {
    std::vector<std::unique_ptr<char[]>> chunks;
    char* bump_next;
    char* bump_end;
};
```

Insert 必须先用 incoming key view probe `memtable_gen::table`。如果该 key
已经存在于当前 generation，只向现有 version vector append 新的
`memtable_entry`，并复用已有 arena-owned key view。这保持 Step 26I 的
duplicate-key storage invariant。

Value bytes 绝不能分配到 `gen_arena`。

### 5.4 Generation

`memtable_gen` 保持正式 runtime state-machine 形态：

```cpp
struct memtable_gen {
    uint64_t gen_id;
    memtable_gen_state state;
    uint32_t front_owner_index;
    uint64_t min_lsn;
    uint64_t max_lsn;
    gen_arena kv_arena;
    absl::btree_map<
        std::string_view,
        absl::InlinedVector<memtable_entry, 1>,
        std::less<>>
        table;
    retire_list<retired_value_ref> loser_durable_refs;
};
```

M01 只定义 storage 和 field semantics。state transitions、seal、flush、
reclaim 属于后续步骤。

Version-vector ordering 不是 M01 的 correctness contract。lookup 和 fold
代码必须选择可见的最大 `data_ver`，不能假设 `versions.back()` 一定是 winner，
除非后续 write-pipeline design 能证明同一个 gen 内 per-key inserts 严格递增。
这是对正式 read-path requirement `max(data_ver <= read_lsn)` 的保守解读，也符合
write-path docs 允许独立 batches 以非拓扑顺序完成 phases 的事实。

### 5.5 Read Set 与 Lookup Result Carrier

`front_read_set` 用 `std::shared_ptr` pin memtable generations：

```cpp
struct front_read_set {
    std::shared_ptr<memtable_gen> active;
    std::vector<std::shared_ptr<memtable_gen>> imms;
};
```

lookup result carrier 是：

```cpp
struct memtable_value_hit {
    format::value_ref durable;
};

struct memtable_tombstone {};
struct memtable_miss {};

using memtable_lookup_result =
    std::variant<memtable_value_hit, memtable_tombstone, memtable_miss>;
```

M01 只冻结该 result type。M02 的 lookup 实现必须搜索传入的 read-set
snapshot，不能读取当前 front owner active/imms，并且必须返回
`read_lsn` 以下可见的最大 `data_ver`。

## 6. Hot-path Cost 与 Lifetime Budget

| 路径 | 允许成本 | 禁止成本 |
|---|---|---|
| client ingress | 每个 request 一个 owned byte buffer；基于 buffer 的 validated views。 | 生产路径把每个 key/value 复制进 canonical storage。 |
| canonicalization | 单个 request 内 O(n log n) key grouping；一组 canonical entries；每个 fragment 一组 stable indices。 | 需要在 `batch_ctx` 外解释 lifetime 的 raw entry pointers。 |
| route fragments | 对 canonical entries 一次遍历，加上 per-owner index vectors。 | fragment ownership 已知后，每个后续 phase 再重复 hash。 |
| memtable insert | 同一 generation 中 key 第一次出现时复制一次 key bytes；POD entry append。 | 把 value bytes 复制进 memtable、arena、hot cache 或 lookup result。 |
| memtable lookup carrier | 不分配 heap，返回 `value_ref`、tombstone 或 miss。 | 返回 value body、client-buffer view 或 refcounted hot blob。 |

`batch_ctx` 拥有 canonical entries 引用的所有 bytes，直到后续 phases 完成。
`memtable_gen` 拥有 key bytes，直到 gen retired。Value bytes 由 value pages
拥有，并通过 `value_ref` 访问。

## 7. M01 必须覆盖的测试

测试应保留旧 step 的意图，同时把 value contract 改成 INC-055：

1. `canonicalization_same_key_last_writer_wins`：
   同一 key 的重复 PUT/DEL 只保留最后一个 op，输出按 winning original
   positions 升序。
2. `canonicalization_routes_put_and_delete_by_front_owner`：
   `owner == key_hash(key) % front_count`，fragments 按 owner 排序，fragment
   entry order 保持 canonical order。
3. `batch_ctx_views_are_context_owned`：
   通过 adapter 从 stack strings 或 temporary raw ops 构建；adapter 输入销毁后，
   canonical key/value views 仍指向 `ctx.input.bytes`。
4. `batch_ctx_move_preserves_fragment_indices`：
   move context 后，每个 fragment index 都能解析到预期 canonical entry；不允许写
   pointer identity test，因为 pointer 已不再是 carrier。
5. `client_batch_view_rejects_malformed_input`：
   truncated bytes、bad op、DELETE with value bytes、`front_count == 0` 都用
   `std::invalid_argument` 失败。
6. `memtable_value_handle_is_durable_only`：
   用 compile-time 或 type-level check 证明 `value_handle` 没有 `hot`、
   `value_view` 或 value-body pointer field；memtable hit carrier 只包含
   `value_ref`。
7. `memtable_duplicate_key_reuses_key_storage`：
   同一个 gen 中两次插入同一 key，只保留一份 arena key copy，并有两个 entries。
8. `memtable_key_bytes_do_not_dangle_caller_stack`：
   从 temporary key buffer insert，销毁或修改 caller storage 后，gen key 仍有效。
9. `memtable_lookup_result_shape`：
   value hit 返回 `memtable_value_hit{value_ref}`，tombstone 返回
   `memtable_tombstone`，miss 返回 `memtable_miss`；result 中看不到 value bytes。

旧 Step 7 中断言 `hot_blob` refcount 或 hot value body 的测试必须重写，不能直接迁移。
旧 Step 26J/K 中断言 `write_batch(client_batch_buffer)` sender behavior 的测试属于
M08/M09，不属于 M01。

## 8. 冲突与决议

1. **旧 `hot_blob` vs INC-055**  
   旧 Step 7/26J/K 在 memtable 中保留 value body。正式文档现在要求只保存 durable
   `value_ref`。039 选择正式 INC-055 contract，并把当前 `value_handle::hot`
   标记为需要移除的 compatibility debt。

2. **Pointer fragments vs stable indices**  
   旧 Step 10/26K 使用 `canonical_entry*`。正式示例中有些地方仍展示 pointer
   fragments，但迁移计划明确优先 stable index 或 context-owned vector。039 选择
   stable indices，因为它让 move 和 vector-reallocation safety 变成显式性质。

3. **Owning canonical strings vs view-based carrier**  
   Step 10 和部分正式示例使用 owning strings；Step 26K 使用 borrowed views。
   039 选择 view-based production entries，因为 `batch_ctx` 拥有 ingress buffer，
   且代码质量要求禁止可避免的 per-phase value copies。raw-op adapter 可以复制进
   buffer，但 production phases 消费 views。

4. **Version-vector ordering**  
   正式 flush notes 有些地方像是默认最新 entry 在 `versions.back()`；read-path docs
   要求选择最大 visible `data_ver`；write-path docs 又允许独立 batches 非拓扑顺序
   完成 phases。039 不把 vector order 作为 correctness contract。后续
   flush/pipeline work 必须维护 sorted per-key versions，或 scan/select max
   `data_ver`；不能静默假设 append-order visibility。

## 9. 需要人工确认的点

M01 中唯一需要 reviewer 在实现前确认的设计点是 version-vector ordering。039 建议
不把 ordering 作为 correctness contract：lookup/fold 必须选择最大 visible
`data_ver`，除非后续 write-pipeline design 证明同一个 gen 内 per-key insertion
严格递增。如果正式意图是“`versions.back()` 永远是最新 entry”，那么 M01
实现必须维护 sorted per-key version vectors，或者后续 pipeline step 必须序列化
memtable inserts 以证明该 invariant。

## 10. 相邻事项

M02 应直接消费 `front_read_set` 和 `memtable_lookup_result`：snapshot lookup
必须从传入的 PRS 返回 `value_ref`、tombstone 或 miss，不能读取 front current
state。

M06/M08/M09 才是设计 WAL append、coord ready bitmap 和 `write_batch`
sender pipeline 的位置。它们不能把隐藏行为回填进 M01 types。等这些步骤加入
value/WAL fan-out 时，`known_issues.md` 的 INC-048 要求显式 bounded concurrency，
不能使用 unbounded `concurrent()`。

M12/flush work 在使用 `versions.back()` 作为 winner optimization 前，必须重新处理
上面的 version-vector ordering 问题。
