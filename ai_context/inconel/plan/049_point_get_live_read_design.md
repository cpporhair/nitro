# 049 - M10 Point GET Live Read

> 本文对应 `ai_context/inconel/plan/front_wal_development_plan.md` 的 M10
> （旧 step 25 Point GET memtable-only 路径 + 旧 step 33 目标读形态中
> memtable hit / value read 相关部分）。
> 目标：交付**单链 production `point_get` sender**——一次 submit 驱动
> `coord(acquire_read_handle) → front(lookup_memtable, PRS snapshot) →
> [hit value] value(read_value) / [hit tombstone] not_found / [miss]
> tree::lookup(manifest) → [leaf value] value(read_value) / [leaf
> tombstone | absent] not_found` 全程（OV §8.1 规范顺序），完成
> write-after-read 的 live 读回闭环。M09 交付的 `write_path::write_batch`
> 是本步测试的写入口；M11 的 runtime topology / operation surface 将包装
> 本步的显式拓扑签名。

## 1. 范围

M10 覆盖：

- L3 `pipeline/point_get.hh`（新文件，`pipeline/` 目录第一个文件）：
  - `point_get_result { found, value }` 完成值类型。
  - 顶层 `point_get(...)` sender（§6）。
- L2 `front/scheduler.hh` + `front/sender.hh` 增量：
  - lookup 的 **borrowed-snapshot 入口**（§7）：
    `lookup_memtable(sched, key, read_lsn, const core::front_read_set*)`，
    production 读路径以 0 owning copy 携带 PRS snapshot。既有 by-value
    入口语义不变、继续保留。
- 测试 `inconel_test_m10_point_get_live_read`（§12），含 tree-hit 的
  node-cache 预热 fixture（全部走 production sender，无 NVMe 注入面
  改动）。

M10 不覆盖：

- MultiGet / batch_lookup 编排、Range Scan、`read_page_values`
  （INC-021，见 §4.4 裁决：维持延期）。
- INC-055 的 dedicated recently-written residency tier（§4.3 裁决：
  显式延期，本步以测试钉住现状行为）。
- runtime/registry topology、public operation surface、facade 包装
  （M11；本步拓扑仍是显式参数，与 M09 同款）。
- seal / flush / frontier switch 与读路径的运行期交互测试（M12+；
  本步用 `install_cat_for_testing` 构造多 CAT snapshot 验证 PRS pin
  语义，不驱动真实 seal round）。
- 多核读测试（M13；本步单核 fixture + advance 轮询）。
- tree / value / coord / wal 任何 L2 行为变更（front 的 borrowed 入口
  是唯一 L2 增量，且不改变 lookup 语义）。
- 旧分支 `point_get_memtable_only` 的 miss-外露形态（§3 对照表：不迁移）。

## 2. 已对照输入

正式设计：

- `design_doc/INDEX.md`（容量/性能硬约束）
- `design_doc/design_overview.md` §8.0（读路径最高准则四条红线）、
  §8.1（Point GET 规范顺序 + 数据源断言表）、§14.2（Point GET pipeline
  形状）、§5.5（read_handle）、§7.3（memtable 允许未发布 entry）
- `design_doc/read_api_and_pipeline.md` §2（read_handle 生命周期 +
  context 传递）、§4（Point GET 全流程 / §4.2 pipeline / §4.3 lookup /
  §4.4 value_ref 契约 / §4.5 tree-path value read）、§9（frame/cache
  模型）、§10（读路径异常）、§11（可见性判定）
- `design_doc/cross_doc_contracts.md` §1（`acquire_read_handle` /
  `lookup_memtable` / `tree_lookup` / `read_value` 签名）、§4（数据源
  断言）、§5（读路径跳转）、§6.1（三条读路径红线）
- `design_doc/runtime_state_machine.md` §2.3（acquire）、§3.7
  （handle_lookup_memtable）、§4.7（tree_lookup 路由与执行域）、
  §6.5（read_value）
- `design_doc/code_modules.md`（pipeline/ = 其它顶层编排；Point GET
  行已在 pipeline/ 职责表中）
- `design_doc/code_quality_standard.md`（§3.1 热路径预算、§3.3 carrier
  策略、§3.5 flat_map、§3.6 owner 可见性）

当前分支代码（全部按已冻结语义消费）：

- `apps/inconel/coord/sender.hh` + `coord/scheduler.hh`
  （`acquire_read_handle()` → `core::read_handle`；
  `install_cat_for_testing`；M03 产物）
- `apps/inconel/core/read_catalog.hh`（read_handle / publish_catalog /
  catalog_store；M02 产物）
- `apps/inconel/core/memtable_lookup.hh` + `core/memtable.hh`
  （`memtable_lookup_result = variant<memtable_value_hit{durable},
  memtable_tombstone, memtable_miss>`；`front_read_set`；M01/M02 产物）
- `apps/inconel/front/scheduler.hh` + `front/sender.hh`
  （`lookup_memtable` owner handle，搜传入 PRS snapshot；M05 产物）
- `apps/inconel/value/sender.hh`（`read_value(vr, NvmeProvider)` →
  owning `std::string` body；M07 产物）
- `apps/inconel/tree/sender.hh` + `tree/lookup.hh` +
  `tree/lookup_scheduler.hh` + `core/tree_read_domain.hh` +
  `core/shard_partition*.hh`
  （`tree::lookup(keys, manifest)` 无 sched 指针、shard-partition
  路由、空树短路、`lookup_result = variant<lookup_value{data_ver, vr},
  lookup_tombstone{data_ver}, lookup_absent>`；INC-003 / INC-040 /
  step 030 收敛形态）
- `apps/inconel/write_path/write_batch.hh`（M09 单链形态先例：外层
  `get_context >> flat_map(&state)`、内层子链借用引用）
- `apps/inconel/core/batch_carrier.hh`（`core::key_hash`）

plan 文档：

- `040` §4-6（lookup/scan 语义、pin 链、§4.4 scan view 契约）、
  `041` §（acquire 的 catalog_store 委托）、`043` §5.1（borrowed
  inputs：read pipeline 必须持有 API key bytes 到 front sender 完成）、
  `046` §3（read_value 现状保留 + NvmeProvider）、`047` §12.1
  （fixture 蓝本）、`048` §6（单链形态）/§15.1（M10 对接预告）/§17.4
  （watch-item 留 M11+，本步不动）

旧分支证据（语义参考，不迁移代码；设计者角色已声明读测试）：

- `inconel:apps/inconel/runtime/operations/point_get_memtable_only.hh`
  （owned-key context state + miss 外露的 memtable-only 形态）
- `inconel:apps/inconel/runtime/front/owner_impl.hh`
  （`point_get_memtable_result = variant<value, not_found, miss>`；
  front op 每请求 own `std::string key` + `read_handle`）
- `inconel:apps/inconel/test/step_25_*`（PUT 读回 / overwrite /
  DELETE not_found / missing key / cross-front 测试意图）

登记问题：INC-054（urgent，正交不动）、INC-056（normal，正交不动）、
INC-055（§4.3 本步裁决记录）、INC-021（§4.4 本步裁决记录）。

## 3. 语义来源对照表

| 项目 | 旧 `inconel` Step 25 证据 | 当前 `inconel.new` 现状 | 正式设计依据 | 049 决议 |
|---|---|---|---|---|
| 第一跳 | `point_get_memtable_only` 先 `coord().acquire_read_handle()` | `coord::acquire_read_handle(sched)` sender 已交付（M03） | OV §8.1 step 1 / RAP §2.1 / cross_doc §5 读路径 | 保留：point_get 第一跳必须是 coord `acquire_read_handle`，read_handle 经 `push_result_to_context` 进 context、整次调用共享（RAP §2.4） |
| miss 对外语义 | 旧形态把 `point_get_miss` 直接暴露给调用方（memtable-only，名字带限制词） | tree 读路径（`tree::lookup` + shard_partition 路由 + read_domain node cache）已是现成 production 能力 | OV §8.1 step 4-5（规范顺序含 tree 段）；dev plan M10 必须重构 4（禁止"后续接入"掩盖语义缺口）；CLAUDE.md 约束 B（通用名必须配完整语义） | **接入完整 tree path**（§4.1 裁决）。`point_get` 对外只有 found / not_found；memtable miss 在链内继续走 `tree::lookup(单 key, manifest)`，leaf value → `read_value`，leaf tombstone / absent → not_found。不落任何 `*_memtable_only` 的对外形态 |
| memtable hit 载荷 | 旧 front 返回 hot value body（hot_blob 时代） | `lookup_memtable` 返回 `memtable_value_hit{durable: value_ref}`（INC-055 M01 已收口） | OV §8.1 规则 4-5 / RAP §4.4 / RSM §3.7 | 保留：hit value 后统一 `value::read_value(vr)` 取 body，绝不从 memtable / kv_arena 取 value bytes |
| key 携带 | 旧 API `std::string&& key` move 进 context state，front op 每请求 own 一份 string | M05 §5.1 冻结 borrowed view 契约（"read pipeline 必须用 request context 持有 API key bytes 到 front sender 完成"） | CQS §3.3（scalar/view 按值 + caller lifetime 契约）；M05 §5.1 | **不迁移 owned-string 形态**：`point_get(…, std::string_view key, …)` borrowed view，调用方保活到 sender 终结（§8）。每 GET 0 次 key copy。M11 facade 若需在边界 own，由 M11 决定 |
| read_handle 进 front | 旧 front op own 整个 `read_handle`，front 内部抽 PRS | cross_doc §1 冻结 `lookup_memtable(key, read_lsn, front_read_set)`——PRS 提取在 L3 | cross_doc §4 数据源断言 / RSM §3.7 | 保留当前形态：L3 从 `rh.cat->prs->fronts[owner]` 提取 snapshot 传给 front；read_handle 不进 front req |
| PRS snapshot 携带成本 | 旧 op own read_handle（shared_ptr 拷贝） | M05 入口按值收 `core::front_read_set`（imms 非空时每请求 1 次 vector heap alloc + N 次 refcount RMW） | CQS §3.3 默认否决"每请求按值复制 owning 容器"；read_handle 已 pin 整条链 | **新增 borrowed-snapshot 入口**（§7）：指针重载 + pin 契约，production point_get 0 owning copy；by-value 入口保留给无 pin 调用方与既有测试 |
| 结果类型 | `variant<point_get_value, point_get_not_found, point_get_miss>` | 无 | OV §8.2 规则 4（API 不输出 tombstone）；RAP §11.2 | `point_get_result { bool found; std::string value; }`。tombstone（memtable 或 tree）与 absent 一律 found=false，不对外区分（区分即泄漏内部表示） |
| 拓扑来源 | `global_runtime().key_to_front(key)` | M09 先例：显式拓扑参数（fronts span），registry 接入留 M11 | 048 §15.2 | 显式参数 `std::span<front::front_sched* const> fronts`；owner = `core::key_hash(key) % fronts.size()`，与写路由同一 hash（OV §1.6 same key → same front） |
| 测试梯度 | step 25 测试：PUT 读回 / overwrite / DELETE / missing / cross-front | M09 交付 write_batch ack（精确知道写入 lsn）+ write_fixture 蓝本 | dev plan M10 完成测试 5 条 | §12 全收，并加 unpublished-invisible、cold-cache NVMe 读回、tree-hit / tree-tombstone / memtable-shadows-tree、compose 无副作用、gate-closed 读不受阻 |

## 4. 冲突与裁决

### 4.1 memtable miss 的对外完成语义（dev plan M10 必须重构 4，本步最大裁决点）

三个候选：接入当前 tree path / 返回显式 miss / 扩大范围。裁决：
**接入当前 tree path，point_get 落完整 OV §8.1 语义**。依据：

1. tree 读路径在当前分支是**现成 production 能力**，不是要新建的范围：
   `tree::lookup(keys, manifest)`（INC-003/INC-040 收敛，无 sched 指针，
   内部 shard-partition 路由 + fan-out + scatter）、`tree_read_domain`
   node cache、`manifest->resolve` 精确 slot、空树 `has_root()==false`
   短路全部就位。M10 只做组合，无 tree 模块改动——"接入"的边际成本
   是一个 flat_map 分支。
2. 约束 B：`point_get` 这个通用名必须对应设计文档完整语义。落
   memtable-only 形态就必须改名 `*_memtable_only_*` 并对 miss
   fail-fast——那正是旧分支被 dev plan 必须重构 4 点名要裁决掉的形态，
   且 M13 e2e 还要再换一次对外语义，违反"完整语义切片"。
3. 显式 miss（把 miss 暴露给调用方）在语义上把"上层必须自己懂 tree"
   泄漏出 KV 边界，违反 OV §8.1（规范顺序是引擎内闭环）。
4. live e2e 验收范围不变：dev plan 写明本文范围的 e2e 只验收
   recently-written memtable-hit 读回。接入 tree path 后，**miss 分支
   的空树短路（absent → not_found）恰好被"missing key 不伪装 found"
   完成测试驱动**；非空树 hit/tombstone 分支由 §12.4 的 node-cache
   预热测试在 m10 target 内驱动（不留未测 production 分支）。

### 4.2 落点：`pipeline/point_get.hh`

dev plan M10 落点写 `apps/inconel/pipeline/point_get.hh`。与 047 §3 /
048 §4.1 的裁决链对照：`code_modules.md` 关键约束把 `write_path/` 定为
**写请求专用**组合层，`pipeline/` "保留为**其它**顶层 pipeline 编排
入口"，且 code_modules 的 pipeline/ 职责表本就列有 Point GET 行。读
路径落 `pipeline/` 与 M08/M09 写路径落 `write_path/` 是同一裁决的两半，
**dev plan 落点无需修正**（这是 M08/M09 之后第一次落点与 plan 原文
一致；§15.3 仍补设计文档引用行）。`point_get_result` 随 pipeline 文件
（house 先例：`write_batch_result` 在 `write_path/write_batch.hh`）。

### 4.3 INC-055 评估点：dedicated residency tier 本步不落（显式延期）

INC-055 的 (1)-(3)（memtable 只存 value_ref、lookup 返回 variant、读
路径走 read_value）已由 M01/M02/M05/M07 完成；(4) "dedicated
write-through residency tier（budget/epoch/pin）" 是纯性能层。裁决：
**M10 不新增 residency tier 机制**，理由与证据义务：

1. 现状已有 write-through 驻留：FUA 完成后 sub-LBA partial 页保留为
   `resident_partial`、写满的 1-LBA 页按 admission 进
   `readonly_frame_cache`（RSM §6.6），`read_value` 先查 dirty round /
   resident / cache 再 NVMe。M10 live 读回的 hit 路径预期 0 NVMe 读。
2. 该预期**必须可证伪**：§12 测试 1 断言"PUT 后立即 point_get，fake
   NVMe read calls == 0"。该断言把"现状驻留行为足够覆盖
   read-after-write"从口头直觉变成回归门事实；若未来 value 模块改动
   破坏驻留，该测试先红。
3. 已知边界显式声明：multi-LBA class（span_lbas > 1）按现行 admission
   策略不进 readonly cache（`read_miss.admit_to_cache` 注释），其
   read-after-write 会读 NVMe。这是带宽换容量的既有决策，不是 M10
   引入的缺口；§12 测试 1 用 sub-LBA 值，§12 测试 7 显式覆盖"读盘也
   必须正确"。
4. tier 的"做不做、怎么做"需要真盘读延迟数据支撑（046 §1 已同向
   预告"等 M10 读路径用实测数据再做"）；在 fake NVMe 上设计 tier 的
   budget/epoch 参数没有依据，违反"并发参数给数字依据"。延期到
   M13 真盘 e2e + 模块完成 profile 之后，INC-055 条目随 §15.4 更新
   裁决记录，不关闭。

### 4.4 INC-021 评估点：`read_page_values` 维持延期

Point GET 的规范路径只用单值 `read_value(vr)`（OV §8.1 规则 4、RAP
§4.5：按页分组是 MultiGet / Scan 的义务，"Point GET 直接调用
read_value"）。M10 范围内没有任何调用点需要 `read_page_values`，落它
就是 dead surface。裁决：**维持 INC-021 延期**，等 MultiGet/Scan 读
管线（M13 后）一起落，届时与请求内 page grouping 一起设计。

### 4.5 front lookup 的 PRS snapshot 携带形态

现状 by-value 入口每请求复制 `front_read_set`（`shared_ptr active` +
`vector<shared_ptr> imms`）：imms 非空时 1 次 heap alloc + (N+1) 次
atomic RMW，触发 CQS §3.3 默认否决项（每请求按值复制 owning 容器）。
而 point_get 链内 read_handle 已通过 context pin 住
`cat → prs → fronts vector → front_read_set` 整条链（RAP §2.2），
snapshot 数据在 sender 终结前地址稳定——borrow 是零成本且安全的。
裁决：**front 增加指针重载**（显式指针 = 显式 borrow 语义，杜绝
const& 绑临时量的悬垂坑），req/op/sender 内部统一为 "owned +
borrowed 指针二选一" 形态（§7）；by-value 入口保留（无 pin 的调用方
与既有 m05/m08/m09 测试不动）。`handle_lookup` 的查找逻辑零变更。

### 4.6 tree-hit 测试的驱动方式（不给 tree 模块加 NVMe 注入面）

tree 读路径的 cache-miss 读经 `rt::local_nvme()` 解析为真实
`nvme::runtime_scheduler` 类型，fake NVMe 不可注册。两个候选：给
tree::lookup 加 NvmeProvider 注入（侵入 tree 公共面 + flush 路径连带）
/ 测试侧用 production API 预热 node cache。裁决：**预热**。依据：

1. `tree_lookup_sched_base::process` 的几何校验按值比较（H-1 review
   修正就是为 test fixture 自持几何留的口子）。
2. 预热全程 production API：`make_lookup_state` → `process(state)` →
   `decision_need_read{frames}`（frames 由 scheduler 自己的 pool 分配）
   → 测试把手工构造的 leaf page image `copy_from_contiguous` 进 frames
   （替代 NVMe read 的唯一一步）→ `submit_cache(frames)` → 后续
   descent 全 cache hit。frame 所有权流转与 production
   `on_decision_need_read` 完全一致，无 teardown 形态分叉。
3. 不为测试改 production（M09 §12.2 排除项同一纪律）。若实现中发现
   预热路径在 frame pool 机制下不可达，必须停下报告，不得给 tree
   加注入面或砍掉 tree-hit 覆盖。

## 5. 新增类型

### 5.1 `point_get_result`（pipeline/point_get.hh）

```cpp
struct point_get_result {
    bool        found = false;
    std::string value;   // found 时为 value body bytes；否则 empty
};
```

成功完成值。`found == false` 统一表达 memtable tombstone / tree
tombstone / tree absent 三种内部形态（OV §8.2 规则 4：API 不输出
tombstone）。不携带 data_ver（GET 对上层只承诺 live value，RAP §11）。

## 6. 顶层 `point_get` Sender

### 6.1 签名

```cpp
template <typename NvmeProvider = value::local_nvme_provider>
[[nodiscard]] inline auto
point_get(coord::coord_sched& coord_sched,
          std::span<front::front_sched* const> fronts,
          std::string_view key,
          NvmeProvider value_nvme = {});
```

完成值 `point_get_result`。`key` 是 borrowed view，`fronts` span 与
schedulers 由调用方保活到 sender 终结（M09 同款契约；M11 起由 runtime
topology 承担）。`value_nvme` 只注入 value 读路径（与 M07/M09 一致）；
tree 读路径按 production 形态解析 `rt::local_nvme()`，不注入。

### 6.2 链结构（语义展开）

```text
point_get(...) =
  coord::acquire_read_handle(coord_sched)                  // 第一跳：coord owner
  >> push_result_to_context()                              // read_handle 入 context，
                                                           // pin CAT→PRS→gens/guard 整条链
  >> get_context<core::read_handle>()
  >> flat_map([key, fronts, value_nvme](core::read_handle& rh) {
         // ── 拓扑/快照校验（运行期，链内第一步）──
         //   fronts 非空、fronts.size() == rh.cat->prs->fronts->size()、
         //   fronts[owner] 非空；违例抛 std::invalid_argument（接线错）
         const uint32_t owner = core::key_hash(key) % fronts.size();
         const core::front_read_set* frs = &(*rh.cat->prs->fronts)[owner];
         return front::lookup_memtable(*fronts[owner], key, rh.read_lsn, frs)
             >> visit()                                    // variant<value_hit, tombstone, miss>
             >> flat_map([key, value_nvme, &rh]<typename R>(R&& r) {
                    if constexpr (R == memtable_value_hit) {
                        // OV §8.1 规则 4：hit value → value owner 读 body
                        return value::read_value(r.durable, value_nvme)
                            >> then(body -> point_get_result{true, move(body)});
                    } else if constexpr (R == memtable_tombstone) {
                        // OV §8.1 规则 3：命中即不回退 tree
                        return just(point_get_result{});
                    } else {  // memtable_miss → tree path（OV §8.1 step 4-5）
                        // tree::lookup 内部是 with_context(...)(...) bind_back，
                        // 作 flat_map 返回值必须 just() >> 前缀（实现期修正，
                        // 见 §17.1.2；树模块 perform_superblock_io 注释同款先例）
                        return just()
                            >> tree::lookup(single_key_span(key),
                                            rh.cat->prs->tree_guard->manifest.get())
                            >> then(results -> 取 results[0])   // 单 key，结果恰一条
                            >> visit()                          // variant<leaf_value, leaf_tombstone, absent>
                            >> flat_map([value_nvme]<typename T>(T&& t) {
                                   if constexpr (T == lookup_value)
                                       return value::read_value(t.vr, value_nvme)
                                           >> then(body -> point_get_result{true, move(body)});
                                   else
                                       return just(point_get_result{});
                               });
                    }
                });
     })
  >> pop_context();                                        // 释放 read_handle pin
```

### 6.3 设计要点

1. **owner 边界一眼可见**（CQS §3.6）：链上每跳即 owner 跳转
   `coord → front(owner) → [value | tree_read_domain(shard) → value]`，
   与 cross_doc §5 读路径跳转逐点一致；不引入隐藏 `on(...)`。
2. **数据源断言逐条满足**（cross_doc §4 / OV §8.0 红线）：
   - `lookup_memtable` 输入 = `rh.cat->prs->fronts[owner]` 指针
     （borrowed 入口使"来自 PRS snapshot 而非 front 当前状态"在类型
     形态上可见）❌ 不是 `front_sched.active/imms`；
   - `tree::lookup` 输入 manifest = `rh.cat->prs->tree_guard->manifest`
     ❌ 不是 tree_sched 当前 manifest；
   - 路由 = sender 内部 `current_shard_partitions()->route(key)`
     ❌ 不在 tree_sched 上执行；
   - 无任何"读期间挡 seal/flush"机制——pin 链就是全部（红线 4）。
3. **flat_map 论证**（CQS §3.5）：外层 `get_context >> flat_map` 是
   一次性 context 提取（M09 §6.3.4 同款，消除 miss 分支二次
   get_context——rh 以 `&rh` 借用进 miss 分支，context 节点地址稳定）；
   `visit() >> flat_map` 两处是运行时 variant 分支、各分支返回不同
   owner sender，无法 then 化；value/tree 分支内的 flat_map 是 owner
   sender 边界。无 helper 内藏 `just() >> get_context` 壳。
4. **单 key 的 tree::lookup**：以单元素 span/range 调公开批量入口
   （INC-003/INC-040 收敛后调用方不允许手挑 shard 指针）；
   `build_route_plan` 在 flat_map 执行期运行（front cb 之后的
   continuation），routing snapshot 经 `shared_ptr<const>` 跨线程只读。
   空树（`!has_root()`）在 plan 构建中直接短路为 absent，零路由零
   I/O——live e2e 的 missing-key 路径即此分支。
5. **结果提取**：`tree::lookup` 完成值为 `vector<lookup_result>`
   （size 1）；提取 `results[0]` 后 `visit()` 展开。提取处断言
   size == 1（panic 级 invariant：单 key 入必有单结果出）。
6. **submit 前零 owner 副作用**：组合期只构造 sender 值；acquire 的
   入队发生在 op.start（submit 后）。专测覆盖（§12 测试 10）。
7. **异常路径的 pin 释放**：异常穿透时 `pop_context` op 被跳过，
   read_handle pin 随 root context/scope 链析构释放（M06/M07/M09 既有
   形态，无新机制）。

### 6.4 与规范 pipeline 的对应

RAP §4.2 的规范 sketch 与本链逐段对应；两处实现级偏差及依据：
（a）sketch 里 miss 分支重新 `get_context<read_handle>()`，本链外提为
一次提取 + `&rh` 借用（CQS §3.5 允许的例外 2：消除子链重复
get_context）；（b）sketch 的 `front_sched[owner]->lookup_memtable`
按值携带 frs，本链经 §7 borrowed 入口（§4.5 裁决）。语义零差。

## 7. Front Lookup Borrowed-Snapshot 入口（L2 增量）

### 7.1 Surface

```cpp
// front/sender.hh 新增重载（既有 by-value 重载保留不动）
[[nodiscard]] inline auto
lookup_memtable(front_sched& sched,
                std::string_view key,
                uint64_t read_lsn,
                const core::front_read_set* frs);
```

**Pin 契约（写进 front/sender.hh 注释）**：`frs` 必须指向调用方已
pin 住的 PRS snapshot 内的 `front_read_set`（典型：read_handle 在
pipeline context 中存活到本 sender 完成之后）。指针在 sender 终结前
必须保持有效；front 不延长其生命周期。传 null 指针 →
`std::invalid_argument`（接线错，组合期或链内检查均可，但必须
fail-fast）。

### 7.2 内部形态

`_front_lookup` 的 req/op/sender 统一为双形态携带：

```cpp
struct req {
    std::string_view key;
    uint64_t read_lsn = 0;
    core::front_read_set owned;          // by-value 入口使用；borrowed 时为空（零 alloc）
    const core::front_read_set* borrowed = nullptr;
    // handle_lookup 取数源：
    //   const auto& frs = borrowed ? *borrowed : owned;
    std::move_only_function<void(
        core::owner_outcome<core::memtable_lookup_result>&&)> cb;
};
```

约束：

1. `borrowed` 指向**外部** pinned snapshot，绝不指向同 req 的
   `owned`（req 经 sender→op→`new req` 流转，自指针会随 move 悬垂；
   borrowed 与 owned 互斥，由两个入口分别填）。
2. `handle_lookup` / `lookup_memtable_now` 的查找逻辑零变更（仍是
   M02 `core::lookup_memtable(key, read_lsn, frs)`）。
3. by-value 入口的行为、签名、成本完全不变（既有测试 m05/m08/m09
   必须不改而绿——作为等价性证据）。
4. `batch_lookup` / `scan_memtable` 本步不加 borrowed 形态（无调用方；
   等 MultiGet/Scan 管线时与 INC-021 一起做，避免 dead surface）。

## 8. Lifetime 契约

| 对象 | Owner | 必须活到 | 保障方式 |
|---|---|---|---|
| `key`（string_view） | 调用方（M11 起为 facade/请求层） | point_get sender 终结（含异常路径） | 调用方契约（M05 §5.1 同款），头注释写明；tree 路径的 route plan / lookup_state 内 key view 同源 |
| `read_handle` | pipeline context（push_result 节点） | pop_context / context 链析构 | 框架管理；它 pin 住 CAT→PRS→fronts→gens + guard→manifest 全链（RAP §2.2） |
| `frs` 指针目标（`front_read_set`） | PRS 的 fronts vector（immutable，shared_ptr 持有） | front lookup sender 完成 | read_handle pin 推论；§7.1 契约 |
| manifest 指针 | `tree_guard`（shared_ptr，read_handle pin） | tree::lookup 完成 | 同上；`manifest.get()` 裸指针仅在 pin 窗口内使用 |
| `fronts` span、coord 引用 | 调用方（测试 fixture；M11 起 runtime） | sender 终结 | 调用方契约（M09 同款） |
| value body（`std::string`） | 完成值，move 给调用方 | — | read_value copy-out 产物，唯一 owning 载荷 |
| `value_nvme` provider | 按值捕获 | — | 空仿函数/指针包装，零成本 |

## 9. 内存序与并发安全

M10 零新增 atomic、零新增发布协议：

1. `acquire_read_handle` 的 CAT/durable_lsn acquire-load 链沿 M02/M03
   冻结语义；read_handle 是请求私有值。
2. `front_read_set` borrow 的跨线程安全 = PRS immutability +
   shared_ptr pin（唯一跨线程 gate 是 `shared_ptr<memtable_gen>`
   control block，RMC §3）；front 单线程上 lookup 与 insert 串行
   （RSM §3.7 线程安全段）。
3. `shard_partition_map` / manifest 均为 `shared_ptr<const>` immutable
   snapshot，多核只读（RSM §10.3/§10.4）。
4. 读路径不触碰 coord 的 gate/ready 状态（acquire 只读 catalog_store）。

## 10. 错误 / 失败语义总表

读路径无 LSN、无 owner 状态变更，所以**没有 release/fatal 映射**——
失败一律以异常向调用方传播，point_get 可安全重试：

| 场景 | 抛出点 | 类型 | 备注 |
|---|---|---|---|
| 拓扑接线错（fronts 空 / 与 PRS 尺寸不符 / owner 槽位空 / frs null） | 链内首检 | `std::invalid_argument` | 配置错误，修 fixture/topology |
| value NVMe read 失败 | `read_value` 链内 | `std::runtime_error`（既有文案） | RAP §10.1：IO error 返回错误，不影响其他 reader/写路径 |
| value body / tree page corruption（magic/CRC/decode） | value `fill_and_decode` / tree `process` | `panic_inconsistency` | INC-004 house 规则：corruption = fail-fast panic，非异常 |
| manifest resolve miss / 路由 map 未装而 has_root | tree 路径既有检查 | `panic_inconsistency` | 既有 invariant，M10 不改 |
| 单 key 结果数 != 1 | §6.3.5 提取处 | `panic_inconsistency` | pipeline invariant |
| front/coord 队列满 | owner schedule 路径既有行为 | 既有 fail-fast | 部署容量错误，M10 不新增背压 |

## 11. 热路径预算与容量估算

新增 runtime carrier：**无**（`point_get_result` 按值返回；borrowed
入口给 `_front_lookup` req 增 8B 指针字段）。容量估算：全部成本
request-scoped，无常驻/每 manifest 增量，10 亿 KV 基线无影响。

### 11.1 memtable-hit GET（canonical 热路径，逐项可数）

| 项 | 数量 | 说明 |
|---|---|---|
| queue hop | 3（coord → front(owner) → value） | 与 OV §8.1 拓扑逐点一致，零多余 hop |
| owner req 分配 | 3 次 `new/delete`（coord read / front lookup / value prepare_read） | 框架 owner 模式既有 per-op 成本，与写路径同款 |
| context 节点 | 1（`push_result_to_context(read_handle)`） | RAP §2.4 拓扑固有成本 |
| owning copy | **恰 1 次**：value body copy-out（RAP §4.5 规定，CRC 后热 cache line 上 copy） | 完成值 `std::string` 分配即此一次 |
| key copy | 0（borrowed view 全程） | |
| PRS snapshot copy | **0**（§7 borrowed 入口；对照 by-value 形态省去 imms 非空时每 GET 1 次 heap alloc + N+1 次 atomic RMW） | M10 的主要热路径改进项 |
| read_handle 成本 | 1 次 shared_ptr copy（2 次 refcount RMW）+ 1 次 atomic u64 load | RAP §2.1：acquire 无分配 |
| lookup 本体 | 0 alloc 0 copy（M02 冻结：btree 下潜 + data_ver 比较） | |
| 框架 sub-scope | 外层 flat_map + visit + 分支 flat_map ≈ 3 | WP/RAP 链形状成本 |
| NVMe | 0（read-after-write 驻留命中；§4.3，测试 1 钉住）；cache 冷时 1 次页读 | sub-LBA/1-LBA class；multi-LBA class 现行策略恒读盘（§4.3.3 已知边界） |

### 11.2 memtable-tombstone / 空树 miss

tombstone：上表去掉 value 段（2 hop、2 req、0 copy）。空树 miss：
tree::lookup 在 plan 构建中短路，**不产生任何 tree 域 hop/IO**，
增量仅 plan 两个空 vector + results(1) + with_context 节点。

### 11.3 tree-path GET（miss → 非空树）与 declared watch-item

走通用批量入口 `tree::lookup(span-1)` 的单 key 成本（n=1, K=shard 数）：
route plan 4 个小 vector（counts/cursors 各 K、entries 1、groups ≤1）+
`all_lookup_results(1)` + 2 个 with_context 节点 + `lookup_state` +
驱动协程帧 + per-level node cache probe/pin；cache 全命中时 ~10 次
request-scoped 小分配，cache miss 时叠加 NVMe 页读（10-30μs 量级，
摊没分配成本）。**declared watch-item（不许静默）**：cold-key 主导的
读负载下，单 key fast path（栈上 plan、绕开通用 scatter）是已识别的
优化点，留给模块完成阶段的 standalone profile（house 规则：开发期不
强制 perf work，但必须显式登记）；M10 不预做，避免无实测数据的结构
特化。本条写入 §15.5 并在 review 对账时复核仍未静默扩散。

## 12. 测试计划

Target：`inconel_test_m10_point_get_live_read`
（`apps/inconel/test/test_m10_point_get_live_read.cc`，CMake 照 m09
模式注册，link `inconel_real_nvme`）。

### 12.1 Fixture

1. **写侧原样复用 m09 蓝本**：fake NVMe（字节捕获 + 按序失败注入 +
   hold_call/release_held 暂扣）+ value sched 13 参构造 +
   `make_cat_from_active` + coord/fronts/wal_space 拓扑 +
   `advance_all` 轮询 + `submit_result`/`drive_until_ready` 驱动 +
   `collect_wal_entries` 解码（取 vr 用）。允许按 047 §12.1 先例提炼
   共享 helper 或复制进 m10 文件；不得放 production 目录。
2. **读侧扩展**：
   - `submit_point_get(key)`：composes `pipeline::point_get(*coord,
     fronts(), key, provider)`，submission 同款 promise 驱动。key
     bytes 由测试持有到 future ready（§8 契约）。
   - fake NVMe 读计数快照 helper（断言"GET 期间 read calls 增量
     == 0 / ≥ 1"）。
3. **tree-hit 扩展（§4.6 裁决的预热机制）**：
   - 静态 `core::tree_geometry`（lba=4096, page=4096, slots_per_range
     按 manifest 自洽取值）；`tree_read_domain<core::
     segmented_clock_cache>` 1 实例（heap DMA allocator），注册进
     `registry::tree_read_domains`（list + by_core[0]）。
   - `core::build_initial_shard_partition_map(空 leaf_order, K=1)`
     得单 shard +∞ 占位 map，`registry::install_shard_partitions`
     （在装非空树 CAT 之前——`tree::lookup` 对 has_root 且无 map
     直接 panic）。
   - 用 production `tree/page_builder.hh` 构造单 leaf page image
     （含目标 value record / tombstone record，CRC 由 builder 算）；
     manifest 手工填 `{root_slot, root_range_base, slot_map, geom}`
     （lookup 路径不消费 leaf_order/reverse_topology，留空即可——
     依据 §4.6.2 与 tree/lookup 代码事实）。
   - 预热：`make_lookup_state({key}, manifest)` → 驱动
     `lookup_sched->process(state)` → 收 `decision_need_read{frames}`
     → 对每个 frame `copy_from_contiguous(leaf_image)` → 驱动
     `submit_cache(frames)` → 再 process 至 done（顺带断言预热
     descent 解码结果 == 预期，作为 fixture 自检）。
   - 换快照：`coord->install_cat_for_testing(CAT2)`，CAT2 =
     {tree_guard 持手工 manifest, fronts = 新建空 active gens,
     durable_lsn 继承, epoch+1}。旧 CAT 仍被先前 handle pin——顺带
     验证 snapshot 语义。
4. 残留纪律：进程退出前无后台线程；fixture 析构顺序保证 registry
   clear 先于 scheduler 析构（runtime_scope 同款）。

### 12.2 测试列表

1. `m10_put_then_point_get_returns_body`（≈ 旧 step 25 主测试）
   write_batch PUT（sub-LBA 尺寸 body，如 100B）→ ack{lsn 1} →
   point_get(key)：found == true、value 逐字节等于写入 body；**GET
   期间 fake NVMe read calls 增量 == 0**（§4.3 INC-055 现状驻留的
   可证伪验收）。
2. `m10_overwrite_returns_latest`
   PUT k=v1（lsn 1）→ PUT k=v2（lsn 2）→ point_get → v2（同 key 多
   版本 bucket 的 winner 规则经全链验证）。
3. `m10_delete_returns_not_found`
   PUT k → DELETE k（独立 batch）→ point_get → found == false；且
   **GET 期间零 tree 域活动可由 read calls 增量 == 0 间接证明**
   （tombstone 命中不回退 tree，OV §8.1 规则 3）。
4. `m10_missing_key_not_found_via_empty_tree`
   未写过的 key → point_get → found == false；fake NVMe read calls
   增量 == 0（空树短路：absent 不伪装 found，完成测试 4）。
5. `m10_cross_front_point_get`
   `key_for_owner(0)` / `key_for_owner(1)` 各一条 PUT（单 batch）→
   两个 point_get 各回各 body；再各查一条该 owner 的 missing key →
   not_found（完成测试 5 + 路由一致性 `key_hash % front_count`）。
6. `m10_unpublished_write_invisible_then_visible`
   用 M08 phase senders 把 batch 驱到 `wal_durable`（parked，未
   publish）→ point_get(k) → found == false（read_lsn 门控经真实读
   管线，OV §7.3）→ finish（memtable + publish）→ point_get → found
   == true。
7. `m10_memtable_hit_reads_value_from_nvme_when_cache_cold`
   PUT k → 新建第二个 value sched 实例（同 class 配置、同 fake
   NVMe）并切换 `registry::value_alloc_sched` → point_get → found +
   body 正确，且 fake NVMe read calls 增量 ≥ 1（read_value miss →
   读盘路径经 point_get 全链；fixture 保活旧实例至测试尾）。
8. `m10_tree_hit_value_via_manifest`（§12.1.3 机制）
   PUT k（拿 durable vr，经 WAL 解码或 memtable lookup 取出）→
   手工 leaf {k → (data_ver=1, value, vr)} 预热 + CAT2（空 fronts +
   手工 manifest）→ point_get(k) → memtable miss → tree hit →
   found + body == 原 PUT body（OV §8.1 step 4-5 wired 路径）。
9. `m10_tree_tombstone_not_found`
   手工 leaf {k → tombstone(data_ver=1)} → point_get → found ==
   false（tree tombstone 读语义，RAP §7.2）。
10. `m10_memtable_winner_shadows_tree`
    在 CAT2 基础上构造 CAT3：fronts 用真实 gens（对 k 先 DELETE →
    memtable tombstone, data_ver 更大），tree_guard 仍持手工 manifest
    （k → value）→ point_get → found == false（memtable 命中绝不回退
    tree，红线/规则 3 的直接断言）；对偶用例：memtable PUT v2 vs
    tree v1 → 返回 v2。
11. `m10_compose_without_submit_has_no_owner_side_effect`
    构造 point_get sender 后直接销毁（不 submit）→ fake NVMe
    `total_calls()` 增量 == 0、coord/front 队列零活动（M08/M09 测试
    7 同款纪律）。
12. `m10_point_get_unaffected_by_closed_gate`
    `close_gate_for_testing()` → point_get 正常完成（读到当前
    read_lsn 数据）→ open。验证"读路径不依赖 gate / 无挡 seal 机制"
    （红线 4 的行为面证据）。

### 12.3 回归门

每 Phase：`cmake --build build` 全 target +
`inconel_test_m01..m10` 十个二进制全 PASS（既有 m05/m08/m09 不改而绿
= §7.2.3 by-value 入口等价性证据）；收尾另跑 `build_asan` 同名十个全
PASS（borrowed 指针 + 预热 frame 流转 + context pin 链必须 ASAN 干净）。
`inconel_test_flush_e2e` 维持编译通过。

## 13. 实现顺序（每 Phase 一个提交）

```text
Phase A  front/scheduler.hh + front/sender.hh：lookup borrowed-snapshot 入口（§7）
Phase B  pipeline/point_get.hh：point_get_result + 顶层 point_get 链（§5/§6）
Phase C  CMake 注册 m10 target + 测试 fixture（m09 蓝本复用 + 读侧扩展）+ 测试 1-5（live 读回主梯度）
Phase D  测试 6/7/11/12（可见性门控 + 冷 cache 读盘 + 无副作用 + gate）
Phase E  tree-hit fixture（页构造 + 预热 + CAT 切换）+ 测试 8-10
Phase F  全量回归（Release + ASAN，m01-m10）+ 总报告（声明跳过项）
```

依赖：B 依赖 A；C 依赖 B；D/E 依赖 C。**Phase A/B 是 production 实现
阶段，禁止打开任何测试文件**；Phase C-E 以 M10 测试作者身份工作，允许
读的既有测试仅限：`test/check.hh`、
`test_m09_production_write_batch.cc`（write_fixture / fake NVMe /
hold / WAL 解码蓝本）、`test_m08_write_baseline_inflight.cc`（phase
sender 驱动梯度，测试 6 用）、`test_m07_value_persist_read_adapter.cc`
（value fixture / read_value 用法）、
`test_m03_coord_scheduler_assign_publish_release.cc`（CAT 构造 /
install_cat / expect_throws 模式）；不得修改任何既有测试文件。

## 14. 排除范围

1. MultiGet / Scan / batch_lookup 编排、`read_page_values`（INC-021）。
2. INC-055 dedicated residency tier（§4.3 显式延期）。
3. runtime/registry/facade、operation surface（M11）。
4. seal/flush/frontier switch 编排及其与读的运行期交互（M12）。
5. 多核读、真盘读（M13）。
6. tree 模块任何改动（含 NVMe 注入面，§4.6）。
7. 047 §17.4 / 048 §17.4 watch-item（queue_depth 双用途、memtable
   fatal 注入面）——留 M11+，本步不顺手做。
8. 长读资源上界策略（RAP §8，v1 策略 2/3 属内存反压/观测，等运行时
   集成步）。

实现若需要以上任何一项才能编译，必须停下报告，不得用通用名伪装。

## 15. 相邻事项

1. **M11**：把显式拓扑参数（coord 引用 + fronts span + provider）换成
   registry/facade runtime topology，并定义对外 operation surface；
   point_get 签名保持底层形态，facade 只做包装（与 write_batch 同策，
   048 §15.2）。
2. **M12/M13**：seal 后 imms 非空的读路径（borrowed 入口的 imms
   收益真正生效）、front sealed gens → flush → tree 真实物化后的
   point_get、多核与真盘 e2e（dev plan M13 测试矩阵 9-11 即本步语义
   的环境放大）。
3. **plan 回填**：`front_wal_development_plan.md` M10 节标注"M10 的
   详细设计文档是 049_point_get_live_read_design.md"；落点行无需
   修正（§4.2：`pipeline/point_get.hh` 与 plan 原文一致），
   front/sender.hh、value read sender adapter 两行维持。
4. **known_issues**：INC-055 条目追加 M10 裁决记录（tier 显式延期 +
   测试 1 的可证伪锚点 + multi-LBA 已知边界）；INC-021 条目追加
   "M10 评估：point GET 无调用点，维持延期"。两条均不关闭、不新增。
5. **watch-item（新）**：§11.3 tree-path 单 key fast path，登记于本文
   并由模块完成阶段 profile 决定取舍；不进 known_issues（无缺陷，
   纯优化候选）。

## 16. 需要人工判断的点

无阻塞项。memtable-miss 语义（§4.1 接 tree path）、落点（§4.2）、
INC-055/INC-021（§4.3/§4.4 显式延期）、PRS 携带（§4.5 borrowed 入口）、
tree-hit 测试机制（§4.6 预热）均有唯一依据并已记录。两条硬停线：
（a）若实现发现 §6.2 链形态在 pump 下不成立（如 visit 对
`memtable_lookup_result` 的展开与 flat_map 借用组合问题），停下报告；
（b）若 §12.1.3 预热路径在 frame pool 机制下不可达，停下报告——
不得给 tree 加注入面、不得砍 tree-hit 覆盖、不得就地改设计。

## 17. Review 对账（2026-06-12，M10 实现 land 记录）

实现提交：`38e5daa`(A borrowed-snapshot lookup 入口) → `21e08a4`(B
point_get pipeline，含一次 rebase 替换：见 §17.1.2) → `d9cd993`(C
CMake + fixture + 测试 1-5) → `2342ac0`(D 测试 6/7/11/12) →
`fce587c`(E tree-hit fixture + 测试 8-10) → `5a32d25`(F 空回归
marker，与 M08/M09 同款)。production 变更仅
`front/scheduler.hh`（+50/-7，双形态 req）+ `front/sender.hh`（+11）+
`pipeline/point_get.hh`（新，149 行）+ 根 `CMakeLists.txt`（+6）；
`pump/`、`ai_context/`、tree/value/coord/wal 模块、既有测试零触碰。
§16 两条硬停线均未触发。

### 17.1 语义对照结论

§12.2 的 12 个测试全部落地，**无跳过无降级**（实现方总报告声明，
review 独立核对成立：最终 main() 含全部 12 个用例且独立验门全过）。
逐条对照通过的要点与接受的实现形态记录：

1. 链结构与 §6.2 逐段对应：一次性 `get_context >> flat_map(&rh)`、
   borrowed frs 指针进 front lookup、两层 `visit() >> flat_map`、
   tombstone/absent 折叠 found=false、单 key 结果数 ≠ 1 panic、
   `pop_context` 链尾。三条读路径红线 + cross_doc §4 数据源断言全部
   以代码形态可见。
2. **Phase B 一次 rebase 替换**（1a7399d → 21e08a4）：`tree::lookup`
   内部返回 `with_context(...)(...)` bind_back，作 flat_map 返回值
   需要 `just() >>` 前缀（与 M08 phase B 的 `loop(n)` amend 同类，
   bind_back-needs-prev 规则的 with_context 变体）。§6.2 sketch 已
   随本节回填修正。
3. Phase A 双形态 req（`owned` + `borrowed` 互斥、两 ctor 分别填、
   handle 单行三目取数源）与 §7.2 逐条一致；borrowed 绝不指向同
   req 的 owned，无 move 悬垂面。by-value 入口零改动，m05/m08/m09
   不改而绿 = §7.2.3 等价性证据。
4. tree-hit 预热（§4.6/§12.1.3）按设计机制落地：production
   `make_lookup_state → process → decision_need_read → 测试 memcpy
   leaf image → submit_cache → process done`；leaf image 用 production
   `tree::leaf_page_builder`；`install_shard_partitions` 先于非空树
   CAT 安装。接受的小偏差：预热自检断言 `resolved` 标志而非解码
   payload（payload 由各 tree 测试的 point_get 端到端断言覆盖）；
   manifest 的 `leaf_order` 填单 zero-fence span（§12.1.3 允许"留空
   或单 span"，lookup 路径不消费）。
5. 测试比设计稿强一档处：测试 11 在零 NVMe 流量外加
   `!advance_all()`（全队列零活动）+ `visible_lsn()==0`（LSN 零
   消耗）直接断言；`expect_not_found` 同时断言 `value.empty()`。
6. 测试 9 为推进 durable_lsn 加了一条另一 front 的 seed 写（tombstone
   data_ver 取 visible_lsn）——实现细节，语义与设计稿一致。
7. 测试 7 的冷 cache 实例是函数局部、析构先于 fixture，teardown 窗口
   无任何 advance/解引用；ASAN 全绿佐证。

### 17.2 运行效率审计（独立小节）

- **热路径增量对照 §11.1 预算逐项核销**：memtable-hit GET = 3 次
  owner queue hop（coord→front→value，与 OV §8.1 拓扑逐点一致，零
  多余 hop）+ 3 个 owner req `new/delete` + 1 个
  `push_result_to_context` 节点 + 恰 1 次 owning copy（value body
  copy-out，RAP §4.5 规定）。key 全程 0 copy（borrowed view）。
  **PRS snapshot 0 copy**：borrowed 入口下 req 的 `owned` 成员保持
  默认构造（null shared_ptr + 空 vector，无分配无 refcount RMW）——
  §11.1 表的核心改进项已兑现；对照 by-value 形态省去 imms 非空时
  每 GET 1 次 heap alloc + N+1 次 atomic RMW。
- **read-after-write 0 NVMe read**（INC-055 §4.3 验收）由测试 1 以
  `reads.calls` 增量 == 0 钉死在回归门；测试 3/4 同款断言覆盖
  tombstone/空树路径。
- **捕获清单核查**（CQS §3.4）：point_get 全部 lambda 捕获 = span
  按值（ptr+size）、string_view、provider（裸指针包装）、`rhp` 裸
  指针（context 节点地址稳定）——零 owning capture。
- **flat_map 论证**（CQS §3.5）：外层 1 个一次性 context 提取；2 个
  visit 后分支 flat_map 是 variant 运行时分支所迫；value/tree 分支内
  flat_map 是 owner sender 边界。无 helper 内藏 `get_context` 壳。
- **miss→tree 分支成本**：按 §11.3 预算成立——`just()` 种子节点 +
  route plan 4 个小 vector + results(1) + with_context 节点 ×2 +
  lookup_state + 协程帧（cache 全命中 ~10 次 request-scoped 小分配；
  cache miss 时 NVMe 读摊没）。**§11.3 declared watch-item（单 key
  fast path）维持登记状态，未静默扩散**——本步未做任何无实测依据的
  结构特化。
- **隐藏成本点名**：无新增 push_context 循环；链内拓扑校验为 3 次
  标量比较；validation 异常文案构造仅冷路径。M10 零新增 atomic、
  零新增常驻 carrier（front req +8B 指针字段是唯一结构增量）。

### 17.3 独立验门记录（不采信实现方自报）

`cmake --build build` 全 target 0 错；Release `inconel_test_m01..m10`
10/10 PASS；`cmake --build build_asan` 全 target 0 错；ASAN 同名
10/10 PASS、日志无 AddressSanitizer/LeakSanitizer 输出；
`inconel_test_flush_e2e` 两套构建产物在；变更范围核查（5 个预期
文件，production 净增 ~210 行）与残留扫描（production 无
TODO/stub/step-phase 数字标识符；无残留测试进程）通过。ASAN 构建
存在既有 `owner_scheduler.hh` `-Wsubobject-linkage` warning
（INC-043 已登记的遗留清理项，非 M10 引入）。

### 17.4 遗留 watch-item

1. §11.3 tree-path 单 key fast path：留模块完成阶段 standalone
   profile 裁决（feedback：开发期不强制 perf work）。
2. `batch_lookup` / `scan_memtable` 的 borrowed-snapshot 形态：等
   MultiGet/Scan 管线与 INC-021 一起做（§7.2.4）。
3. 047 §17.4.1（queue_depth 双用途）/ 048 §17.4.1（memtable fatal
   注入面）维持开放，留 M11+。
4. 不新增 known_issues 条目：本步无 production 缺陷级发现；
   INC-055/INC-021 的 M10 裁决记录随 wrap-up 写入条目备注。
