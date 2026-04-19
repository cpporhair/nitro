# 034 — INC-046 Shadow CoW 写侧重写提案

> 目标：为 `INC-046` 提供一份独立提案文档，先把现状问题、写侧新算法、职责边界和最小验证范围讲清楚，再决定是否修改 production 代码。
>
> 状态：草案
>
> 本文不是正式 spec；正式语义来源仍然是 `design_doc/`。但本文的设计判断只以正式 spec 和当前代码事实为依据，不以 `INC-046` issue 文本里的“修法建议”为依据。

---

## 1. 范围与目标

本文只处理 `INC-046`：

1. 解释为什么当前 tree 写侧已经退化成传统 CoW path-copy，并带有跨 round correctness bug。
2. 提出一条新的写侧边界：
   - `read_domain` / `worker` 只处理 leaf
   - `owner/tree_sched` 独占 non-leaf 的 reverse / placement / write
3. 冻结“何时允许向上 cascade、何时必须停止”的唯一判据。
4. 定义 worker 返回 carrier 的最小形态，并要求 leaf page body 在 `worker -> owner -> nvme write` 链路上零额外拷贝。
5. 给出最小验证矩阵。

本文不做：

- 不修改 `design_doc/` 正式文档
- 不修改 production 代码
- 不讨论 `INC-044` / `INC-045`
- 不把 `INC-046` issue 文本本身当设计来源

---

## 2. 正式 spec 约束

本提案依赖的正式约束只有下面几条：

1. Shadow CoW 的核心语义是：
   - 普通更新优先写到同一 `shadow range` 的下一个 slot
   - 父节点保存 child `range_base`，不是 live slot 的精确 paddr
   - 只有 `split` 或 `consolidation` 改变 child `range_base` 时，父节点才需要更新  
     见 `design_overview.md` §1.3 / §1.9 / §10.1。
2. 运行时 slot 解析由 `tree_manifest` 负责：`range_base -> live slot`。  
   见 `design_overview.md` §4.4。
3. Flush Phase 2 的 owner seam 是：
   - `tree_read_domain.lookup` 做 key -> affected leaf mapping
   - `tree_read_domain.worker` 做 old leaf read + candidate build
   - `tree_sched` 做 tree delta / manifest delta / bounded writes  
     见 `flush_and_frontier_switch.md` §3.2 / §3.4。
4. Shadow slot 选择和 consolidation 的物理决策属于写侧规划：
   - `same-range next-slot`
   - `consolidation`
   - `retired.old_slots / old_ranges`
   - `new_manifest.slot_map` 更新  
     见 `flush_and_frontier_switch.md` §3.5 / §3.6 / §3.8。

---

## 3. 当前实现的两个根本问题

### 3.1 死链路：普通更新被错误地一路传播到 root

当前实现的核心死链路是：

1. `candidate_build.hh::process_flush_round()` 在 leaf merge 之后无条件进入 `initialize_cascade()`。
2. `initialize_cascade()` 对每个 affected leaf 都沿 `reverse_topology` 把整条 parent chain 注册成 `internal_work`，直到 root。
3. 后续 `build_one_internal()` 会把这些 `internal_work` 全部重建成新的 `mem_tree_node`。
4. owner 侧再给这些 node 统一分配 `same-range next-slot` 或 `fresh range`。

问题不在于某个 `if` 漏写了，而在于算法结构本身错位：

- worker 在还没有做最终 placement 之前，就已经把所有 ancestor 都当成“必须重写”处理了。
- owner 的 slot 选择发生在 ancestor 已经被 worker 复制之后。
- 因此系统中根本不存在一个阶段，能根据“child 对父可见的 `range_base` 是否变化”来决定“向上停还是继续”。

结果：

1. 普通 leaf rewrite 即使最后只是写到 child 自己的 next slot，ancestor 也已经被预先复制。
2. spec 中“普通更新不级联到 root”的约束失效。
3. Shadow CoW 的摊销写放大退化成普通 CoW path-copy。

这条死链路对应 `INC-046` 要求回答的第 1 点。

### 3.2 correctness bug：internal page 把 `slot paddr` 写进了 `child_base`

当前实现还有一个更直接的 correctness bug：

1. owner 侧 `child_to_paddr` / `reformat_internal_node()` 对 `unique_ptr<mem_tree_node>` child 使用 `new_paddr`。
2. 但在 same-range next-slot 路径下：
   - `new_range_base = carrier`
   - `new_paddr = slot_paddr(carrier, cur_slot + 1)`
   - `new_paddr != new_range_base`
3. lookup 侧读取 internal page 时，`find_child()` 读出来的 `child_base` 会直接送进 `manifest.resolve(child_base)`。
4. `tree_manifest::resolve()` 的 key 是 `slot_map[range_base]`，不是 `slot_map[slot_paddr]`。

因此一旦某个 child 走了第二轮 same-range next-slot：

- internal page 中保存的是 exact slot paddr
- `slot_map` 中登记的是 `range_base`
- `resolve()` 必 miss 并 panic

Round 1 没暴露只是因为 slot 0 的 paddr 恰好等于 range base。

这条错误对应 `INC-046` 要求回答的第 2 点。

---

## 4. 新边界：leaf-only `read_domain`，owner-only non-leaf

### 4.1 总体边界

本文提议把当前“worker 预建到 root，owner 只做 merge + placement”的模型，改成：

```text
read_domain.lookup
  = 只做 key -> leaf_range_base -> live leaf slot 的定位

read_domain.worker
  = 只读 old leaf、merge/compact、生成新 leaf page image

owner/tree_sched
  = 独占 non-leaf 的读取、缓存、reverse、placement、write、manifest 更新
```

这条边界的直接效果：

1. `worker` 不再读取或构造 internal/root。
2. `worker` 不再返回“局部新 root”。
3. `owner` 才是唯一会生成 non-leaf 新页、决定向上是否继续的人。

### 4.2 对读侧的连带变化

如果采用这条边界，则 point lookup / multiget 的 leaf 定位不再需要 non-leaf traversal，而改成：

```text
route(key)
-> manifest.leaf_order.find_leaf_for_key(key)
-> manifest.resolve(leaf_range_base)
-> read leaf
```

这意味着：

1. `read_domain` 只需要 leaf 相关信息。
2. `read_domain.node_cache` 只缓存 leaf page。
3. non-leaf page cache 从 `read_domain` 移出，转为 owner 独占。
4. 本轮 `INC-046` 不要求真的实现 scan 改造，但底层 carrier 不能把未来 scan 的 leaf-ordered 前进路径堵死。

换句话说：

1. 这次 proposal 真正要落定的是 point lookup / multiget 的 leaf-only 边界。
2. range scan 暂不作为实现范围。
3. 但新的 `leaf_order` / leaf chain / owner rebuild 后的 manifest 仍必须保持“按 leaf 有序推进”这条能力。

这不是 `INC-046` issue 文本给出的要求，而是为了让“worker 只做 leaf、owner 独占 non-leaf”成为一条干净的运行时边界。

---

## 5. worker 与 owner 的职责

### 5.1 worker 负责什么

worker 只负责 leaf-local 的事情：

1. 根据 `manifest.leaf_order` 把本 partition 的 key groups 映射到 affected leaves。
2. 读取 old leaf page。
3. 合并 old leaf records 与 memtable winners。
4. 做 page-local tombstone compact。
5. 对每个命中本轮 flush workset 的 leaf，生成 1 张或多张新 leaf page image。
6. 识别并返回被覆盖的 old tree `value_ref`。

worker 明确不负责：

1. 不读取 internal/root。
2. 不构造 parent/internal/root 新页。
3. 不分配 slot / range。
4. 不修改 manifest。
5. 不决定 non-leaf 的 `unchanged / same-range next-slot / consolidation / split`。

### 5.2 owner/tree_sched 负责什么

owner/tree_sched 负责所有 non-leaf 和所有最终 placement：

1. 汇总所有 worker 返回的 leaf chain。
2. 对 leaf 输出做最终 placement：
   - same-range next-slot
   - consolidation
   - split 时 continuation child / new sibling 的 range 分配
3. 基于 child interface delta 从 leaf 的 parent 开始向上 reverse。
4. 读取或命中 owner-only non-leaf cache，获得 old internal 结构。
5. 重建 parent/internal/root 新页。
6. 为 non-leaf 决定：
   - unchanged
   - same-range next-slot
   - consolidation
   - split
7. 维护 retired 集合。
8. 构造 new manifest：
   - `slot_map`
   - `leaf_order`
   - `reverse_topology`
9. 提交 bounded writes + device flush。

这对应 `INC-046` 要求回答的第 3 点。

---

## 6. 决策归属：谁决定 `unchanged / same-range next-slot / consolidation / split`

这一点必须明确拆成两层。

### 6.1 leaf 的逻辑形态由 worker 决定

worker 只看 leaf merge 结果，决定 leaf 的逻辑输出形态：

1. `rewrite`
   - 命中本轮 flush workset，且 merge 后需要 1 张新 leaf page
2. `split`
   - 命中本轮 flush workset，且 merge 后需要 2+ 张新 leaf page

这里特意不引入“touched leaf 但 page image 恰好与 old leaf 完全一样”的额外快路径：

1. 只要某个 leaf 命中了本轮 flush workset，就按 `changed` 处理。
2. worker 不再对 touched leaf 做一次整页 old/new equality 检查。
3. `unchanged` 只适用于根本没有命中本轮 flush workset 的 leaf；这类 leaf 不会进入 worker 返回 carrier。

worker 不决定这些新页最终落到 old range 还是 new range。

### 6.2 最终 placement 与所有 non-leaf 形态由 owner 决定

owner 对每个需要落盘的逻辑节点做最终 placement：

1. `same-range next-slot`
   - old range 还有空 slot
2. `consolidation`
   - old range slot 已满，需要 fresh range
3. `split`
   - child interface 的基数或 separator 变化，parent-visible
4. `unchanged`
   - child delta 应用后，本层结构与 old page 等价

因此本文的最终结论是：

1. leaf 的 `unchanged / rewrite / split` 由 worker 判。
2. leaf 的 `same-range next-slot / consolidation` 由 owner 判。
3. non-leaf 的 `unchanged / same-range next-slot / consolidation / split` 全部由 owner 判。

这对应 `INC-046` 要求回答的第 4 点。

---

## 7. 唯一判据：什么时候允许向上 cascade，什么时候必须停止

本文冻结一个唯一判据：

> 只有当某个 child 对父节点暴露的 `child interface` 发生变化时，才允许继续向上；否则必须停止。

这里的 `child interface` 指父节点可见的有序信息：

1. child 的 `range_base`
2. 若 child 替换成多个 sibling，则是有序 child `range_base[]`
3. 这些 child 之间的 separator

按这个判据：

1. `same-range next-slot`
   - child 只换 live slot
   - 对父可见的 `range_base` 不变
   - **必须停止**
2. `unchanged`
   - child interface 完全不变
   - **必须停止**
3. `consolidation`
   - child `range_base` 改变
   - **必须继续向上**
4. `split`
   - child 个数和/或 separator 改变
   - **必须继续向上**

这就是“什么时候允许向上 cascade、什么时候必须停止”的唯一判据，对应 `INC-046` 要求回答的第 5 点。

---

## 8. `child_base` 契约：internal page 只能写 child `range_base`

本文对 `child_base` 再明确一次：

1. internal page 中的 `child_base` 永远表示 child logical node 的 `range_base`。
2. internal page 绝不能写 child 当前 live slot 的 exact paddr。
3. `manifest.resolve(child_base)` 的输入语义固定是 `range_base`。

因此：

1. root 的 `manifest.root_slot` 可以是 exact slot paddr。
2. internal page 中的 child pointer 不可以是 exact slot paddr。
3. 即使 child 本轮写到了 next slot，parent page 仍必须继续写 old `range_base`。
4. 只有 consolidation 或 split 产生新的 logical child 时，parent 才写新的 `range_base`。

这保证下面这条契约始终成立：

```text
internal page child_base
  --(manifest.resolve)-->
current live slot paddr
```

这正是 `lookup_scheduler` 与 `tree_manifest::resolve()` 的契约关系，对应 `INC-046` 要求回答的第 2 点。

---

## 9. worker 返回什么：只返回新 leaf page + 最小元数据

为了做到零额外拷贝，worker 返回 carrier 必须收窄到“只有新页面”。

### 9.1 目标

worker 返回物满足：

1. old page 只是输入，不进入返回 carrier。
2. 未命中本轮 flush workset 的 leaf 不进入返回 carrier。
3. 命中本轮 flush workset 的 leaf 一律返回新 page image，不做额外 page-equality 快路径。
4. leaf page body 在 `worker -> owner -> nvme write` 链路上只 move ownership，不再做第二次 memcpy。

### 9.2 建议 carrier

建议把当前 `worker_tree_proposal` 改成下面这类 leaf-only carrier：

```cpp
struct owned_page_buf {
    char*     data;
    uint32_t  byte_len;
    // move-only owner; concrete backing can be unique_ptr<char[]>
    // or future writeback frame
};

struct leaf_page_image {
    owned_page_buf page;      // 已格式化完成的新 leaf page
    uint16_t       first_key_off;
    uint16_t       first_key_len;
};

struct leaf_chain_item {
    uint32_t                      old_leaf_idx;
    format::paddr                 old_range_base;
    core::internal_idx            parent_idx;
    enum class shape : uint8_t { rewrite, split } shape;
    absl::InlinedVector<leaf_page_image, 2> new_pages;
    absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
};

struct worker_leaf_chain {
    flush_round_id          round_id;
    uint32_t                read_domain_index;
    flush_stage_status      st;
    std::vector<leaf_chain_item> items; // old_leaf_idx 升序
};
```

这里 `first_key_off/len` 不是装饰字段，而是后续 owner 构造 `separator` / `leaf_order` 的零拷贝锚点：

1. worker 不另外分配 `std::string separator`
2. owner 直接从 `leaf_page_image.page` 中取 `string_view`
3. 只要该 output page 还活着，引用它的 separator view 就合法

### 9.3 为什么这满足零额外拷贝

1. worker 在最终落盘用的 page buffer 中直接格式化新 leaf page。
2. `worker -> owner` 只 transfer ownership。
3. owner 提交 write 时，`write_desc.data` 直接引用该 buffer。
4. write 完成前，该 buffer 不再被复制。

因此本文对“0 拷贝”的定义是：

> leaf page body 从 worker 构造完成到 NVMe write 提交之间，不再发生第二次 page-size memcpy。

这正是本 proposal 对 worker 返回物的要求。

---

## 10. owner 的 non-leaf cache

### 10.1 为什么要有 owner-only non-leaf cache

在新边界下：

1. `read_domain` 不再缓存 non-leaf。
2. worker 也不再碰 non-leaf。
3. 但 owner 在 reverse parent 时仍需要 old internal 结构。

由于正确实现的 Shadow CoW 下，上层 internal/root 变化频率远低于 leaf，owner 维护一份 non-leaf cache 是合理的。

### 10.2 cache 形态

owner cache 建议：

1. key 用 live `slot_paddr`
2. value 以 raw non-leaf page 为主形态，不预先物化成独立 `internal_view`
3. owner 在需要访问结构时，临时挂 `internal_page_reader` / 未来等价的 zero-copy reader

换句话说，owner cache 的主语义是“resident old non-leaf page image”，不是“常驻的解码后对象图”。

这样做的原因：

1. 满足零额外拷贝目标，不为 cache 再额外复制一份 `children[] / separators[]`
2. 当前 `internal_page_reader` 本身已经是轻量 parse：buffer 常驻，separator 用页内 `string_view`，只在取 child 时拷很小的 `paddr`
3. 若后续 profiling 证明 parse 真是热点，再讨论 lazy decoded sidecar；本文先不冻结这层优化

cache 语义：

1. 跨 round 持久，语义上与 `read_domain` cache 相同，但 owner-local、只服务 `tree_sched`
2. 只是优化，不是 correctness source
3. miss 时 owner 直接读 old internal page 并回填 raw page cache
4. evict / invalidate 必须先于 old slot / old range recycle

### 10.3 为什么不把 non-leaf 留在 read_domain

因为本文的目标是把边界彻底拉直：

1. read-side 只做 leaf
2. write-side reverse 只在 owner
3. non-leaf 的缓存、解析、重建都跟 owner 的写侧状态放在同一个 owner 上

这样 `INC-046` 的“停在哪里、谁负责决定”不会再被 shared read-domain cache 反向污染。

---

## 11. 新写侧算法

### 11.1 worker 阶段

对每个 affected leaf：

1. 读 old leaf
2. merge old leaf + winners
3. compact tombstone
4. 判定：
   - rewrite
   - split
5. 只要 leaf 命中了本轮 flush workset，就直接生成新 leaf page body；不再做 old/new page equality 检查
6. 返回 `leaf_chain_item`

输出是一条全局按 `old_leaf_idx` 可归并的 leaf chain。

### 11.2 owner 阶段：先做 leaf placement

owner 消费 leaf chain 时，先为 leaf 输出做最终 placement：

1. `rewrite`
   - 若 old range 有 next slot：写 same-range next-slot
   - 否则：leaf consolidation，分配 fresh range
2. `split`
   - 选一个 continuation child 代表 old leaf 的“左/保留”逻辑 child
   - continuation child 定义为：保留 old leaf lower fence 的那一片输出
   - continuation child 是 split outputs 中唯一允许复用 old `range_base` 的输出
   - continuation child 若 old range 有 next slot，可复用 old range
   - 额外 sibling 一律分配 fresh range
   - 若 old range 已满，则 continuation child 也分配 fresh range

例子：

```text
old leaf:
  range_base = R100
  key range   = [a, z)

split output:
  P0 = [a, m)
  P1 = [m, z)

if R100 still has next slot:
  P0 -> R100.next_slot   // continuation child, keeps old lower fence
  P1 -> R200.slot0       // fresh range

if R100 exhausted:
  P0 -> R300.slot0
  P1 -> R200.slot0
```

这一步输出的不是“新 root”，而是 leaf 对 parent 的 `child interface delta`：

```text
old child: old_leaf_range_base
new interface:
  children = [range_base1, range_base2, ...]
  separators = [sep1, sep2, ...]
```

为避免这一步再次退化成“文字描述”，本文建议 owner 的中间 carrier 至少长成下面这个形状：

```cpp
struct separator_ref {
    uint16_t page_index;   // 指向 outputs[page_index]
    uint16_t key_off;
    uint16_t key_len;
};

struct child_interface {
    absl::InlinedVector<format::paddr, 3> child_ranges;
    absl::InlinedVector<separator_ref, 2> separators;
};

struct child_interface_delta {
    format::paddr      old_child_range_base;
    core::internal_idx parent_idx;
    child_interface    new_iface;
};
```

约束：

1. `child_ranges.size() >= 1`
2. `separators.size() == child_ranges.size() - 1`
3. `separator_ref` 的字节来源是对应 output page 的首 key，不额外复制 separator bytes

因此 leaf placement 的直接产物不是“新 root”，而是：

```text
leaf outputs + child_interface_delta
```

### 11.3 owner 阶段：向上 reverse

owner 从叶子往上做 reverse：

1. 用 `parent_idx` 找到直接父节点。
2. 读取或命中 parent 的 old raw non-leaf page，并临时挂 zero-copy reader。
3. 把所有 child interface delta 应用到 parent 的 child 列表。
4. 生成 parent 的新逻辑内容。
5. 判断 parent 属于：
   - unchanged
   - same-range next-slot
   - consolidation
   - split
6. 若 parent interface 对祖父不可见，则停止。
7. 若 parent interface 对祖父可见，则把 parent 自己变成新的 child interface delta，继续向上。

这里有两个实现约束必须明确。

#### 11.3.1 同一 parent 的多个 child delta 必须一次性合并

owner 不能按“哪个 worker 先回来就先改一次 parent”的方式逐个重写同一 parent。

正确做法是：

1. 先把 leaf placement 产生的 `child interface delta` 按 `parent_idx` 分组。
2. 对同一个 `parent_idx`，owner 只读取 / 命中一次 old parent page。
3. 在 parent 的 old child order 上，一次性应用该 parent 下所有 child delta。
4. 只生成一份 parent 的新逻辑结果。

也就是说：

```text
many leaf deltas
  -> group by parent_idx
  -> one old parent page
  -> one merged parent rewrite decision
```

这样可以避免：

1. 多个 worker 命中同一 parent 时互相覆盖
2. 同一 parent 在一轮 flush 中被重复重写多次
3. owner 重新落回“局部 path-copy 再互相 merge”的旧模型

#### 11.3.2 合并顺序以 old child order 为准，不以 worker 返回顺序为准

worker 只保证 `leaf_chain_item` 按 `old_leaf_idx` 升序输出。

owner 在某一层 reverse 时，真正的合并顺序必须以当前 old parent page 里的 child order 为准：

1. 先用 zero-copy reader 读出 parent 的 old `children[]` 与 `separators[]`
2. 扫 old child order
3. 遇到未命中的 child，原样保留
4. 遇到命中的 child，把该 child 替换成对应的 new child interface
5. 若一个 old child 被 split 成多个 siblings，就在这个 old child 位置原地展开

因此 owner 的实际 splice 语义是：

```text
old parent children:
  [C0, C1, C2, C3]

if C1 got split into [N1, N2]
and C3 got consolidated into [N3]

new parent children:
  [C0, N1, N2, C2, N3]
```

这条规则很重要，因为：

1. 它保证 parent child order 与 key order 一致
2. 它不依赖 worker 返回顺序
3. 它天然支持多个 worker 同时命中同一 parent 的不同 children

这里还要补一条匹配规则：

1. owner 在 old parent page 上定位“哪个 child 被替换”时，以 `old_child_range_base` 做匹配
2. 不需要 worker 预先给出 `child_pos`
3. 因为同一 parent 下 child `range_base` 唯一，owner 扫 old child order 时能唯一定位替换点

这保证 `child_interface_delta` 只依赖：

```text
{ old_child_range_base, parent_idx, new_iface }
```

而不需要 worker 预先理解 parent 页布局

#### 11.3.3 向上推进的队列模型

owner 的 reverse 可以理解成一层层往上的 work queue：

1. 第 0 层输入：leaf placement 产出的 `child interface delta`
2. 按 `parent_idx` 分组，得到第 1 层待处理 parent 集合
3. 对每个 parent 做一次 merge + placement，产出：
   - stop
   - 或新的 parent-as-child delta
4. 新 delta 再按更高一层 `parent_idx` 分组
5. 重复直到 root

因此 owner 的 reverse 不是“从每个 leaf 各自一路 climb 到 root”，而是：

```text
level-by-level fan-in
```

这正是 owner-only non-leaf 模型与当前 worker-local climb 的本质差别。

为了让这条队列模型能直接写代码，本文建议 owner 至少维护这两个中间 carrier：

```cpp
struct parent_group {
    core::internal_idx                  parent_idx;
    std::vector<child_interface_delta*> deltas;
};

struct next_level_delta {
    format::paddr      old_child_range_base;
    core::internal_idx parent_idx;
    child_interface    new_iface;
};
```

处理流程：

1. 先收集一层 `child_interface_delta`
2. `group by parent_idx`
3. 对每个 `parent_group`：
   - 读 old parent page
   - splice child order
   - 生成新 parent output page(s)
   - 做 placement
   - 若 parent-visible change 需要继续传播，则产出 `next_level_delta`
4. 把 `next_level_delta` 作为下一层输入

这样 owner 真正实现时，不需要再把“reverse”拆回递归 path-copy。

### 11.4 root

root 层同样只按“child interface 是否变化”处理：

1. root unchanged：无 root change
2. root same-range next-slot：`root_slot` 变化，`root_range_base` 不变
3. root consolidation：`root_range_base` 变化，root-change
4. root split：新 root 产生，root-change

---

## 12. 为什么新算法不会再退化成 path-copy

新的停止点出现在 owner 做完每层 placement 之后：

1. child 只是 same-range next-slot  
   `child interface` 不变  
   owner 当场停止  
   ancestor 不会被复制
2. child consolidation  
   `child range_base` 改变  
   owner 继续向上
3. child split  
   child 列表 / separator 改变  
   owner 继续向上

因此，普通 leaf rewrite 只在 leaf 自己的 shadow range 内消化，不会像当前实现那样在 worker 阶段就把整条 parent chain 预先复制出来。

---

## 13. 最小验证矩阵

本文要求至少覆盖下面这些场景，对应 `INC-046` 要求回答的第 6 点。

### 13.1 必选场景

1. **同一 leaf 连续两轮 flush + 跨 round lookup**
   - Round 1：leaf rewrite
   - Round 2：同一 leaf 再次 rewrite，走 same-range next-slot
   - 验证：
     - parent `child_base` 仍是 leaf `range_base`
     - lookup 能通过 `manifest.resolve(range_base)` 命中第二轮 live slot
2. **leaf rewrite 但 parent 不应被重写**
   - child 走 same-range next-slot
   - 验证 owner 在 leaf 层停止，不生成 parent write
3. **leaf consolidation**
   - child old range 用尽 slots
   - 验证 parent 只因 child `range_base` 改变而重写
4. **leaf split**
   - 一个 leaf 变成 2+ siblings
   - 验证 parent child 列表 / separator 改变，继续向上
5. **parent same-range next-slot 后停止**
   - child split 导致 parent 重写，但 parent 自己仍在 old range next-slot
   - 验证 cascade 停在 parent，不再冒到 grandparent
6. **parent consolidation / split**
   - child delta 导致 parent 也满或也溢出
   - 验证只有这时才继续向上
7. **untouched leaf**
   - 某个 leaf 没有命中本轮 flush workset
   - 验证它不会进入 worker 返回 carrier，也不会被 owner 写入
8. **touched leaf 无 page-equality 快路径**
   - 某个 leaf 命中本轮 flush workset，但逻辑结果可能与旧值“看起来接近”
   - 验证 worker 仍返回新 page image，不额外做 old/new page equality 比较

### 13.2 重要但可在下一轮补强的场景

1. 两个 worker 命中同一 parent 下不同 leaves
2. 3-way split + continuation child 复用 old range
3. 3-way split + continuation child 因 slot exhausted 转 fresh range
4. root consolidation
5. root split

---

## 14. 本提案与当前代码的关系

如果接受本提案，则当前以下结构都不应继续沿用：

1. worker 返回“局部新 root”的 `worker_tree_proposal`
2. `initialize_cascade()` 对 affected leaf 无条件一路注册到 root
3. worker 读取和构造 internal/root
4. owner 把 `new_paddr` 写进 internal page `child_base`
5. read_domain 的 non-leaf traversal / non-leaf cache

对应地，新的实现方向是：

1. worker leaf-only carrier
2. owner-only non-leaf cache
3. owner reverse + placement
4. `child_base` 永远写 `range_base`
5. 以“child interface 是否变化”为唯一传播判据

---

## 15. 小结

`INC-046` 的本质不是某个局部 bug，而是写侧结构边界错了。

当前实现的问题是：

1. 在还没做最终 placement 之前，就把 ancestor 全部预先复制了
2. 在 internal page 中把 child logical id 和 live slot paddr 混为一谈

本文给出的修正方向是：

1. `read_domain` / worker 只处理 leaf
2. owner 独占 non-leaf 缓存、reverse、placement 和 write
3. worker 返回物只有新 leaf page 和最小元数据，leaf page body 零额外拷贝
4. 向上传播只由 owner 在每层 placement 之后决定
5. 唯一判据是“child interface 是否变化”

如果这条 proposal 通过，再进入 production 代码重写。

---

## 16. 编码就绪性

按本文当前版本，已经具备进入 production 重写的语义条件。

原因：

1. `INC-046` 要求的 6 个核心问题都已在文档中明确回答
2. `worker` / `owner` 的职责边界已经冻结
3. 向上传播的唯一判据已经冻结
4. `child_base` / `manifest.resolve()` 契约已经冻结
5. worker 返回 carrier、leaf placement、owner reverse fan-in 的最小结构已经给到可编码程度

仍然没有冻结、但不阻塞编码的事项：

1. `owned_page_buf` 的最终 backing 类型是 `unique_ptr<char[]>` 还是未来 writeback frame
2. owner non-leaf cache 的具体淘汰策略
3. parse 热点若成立时是否增加 lazy decoded sidecar

因此，除非后续 review 再发现语义冲突，否则本文状态可以视为：

> 可以开始编码。
