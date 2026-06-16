# 058 — 稳态 e2e 测试计划（功能 / 稳定 / 正确性，不测速度）

> 验证 steps 1-3 落地的**前台 + 稳态后台环**作为一个整体连续运行的正确性与稳定性。
> **本计划不测速度 / 不做 benchmark**（性能锚点 RocksDB×5 是另一条线，需先有 benchmark 标准文档）。
> 重点：**覆盖 flush 场景**（含 Shadow CoW 跨 round 路径）。test-author 角色实现。

---

## 1. 范围

### 1.1 做什么
一个 production-form 的 e2e harness（`apps/inconel/test/`），在**单进程多核 runtime + mock NVMe 设备**上，连续驱动：
```
write_batch(PUT/DEL/overwrite) → [周期] seal → flush_round_once → reclaim → point_get 读回
                                  ↑ 多轮持续，watch 正确性 + 不泄漏 + 不 panic
```
验三件事：**正确性**（读回 winner 始终对）、**稳定性**（多轮长跑无 panic、无泄漏）、**flush 场景覆盖**（§3）。

### 1.2 不做（本步）
- **不测速度 / 吞吐 / 延迟**（benchmark 是独立线，需先定 workload/硬件/测法标准文档）。
- **不测崩溃恢复**（recovery = step 4，未实现；durability 跨重启往返不在本步）。
- 不引入 production 注入 hook；不可干净观测的断言跳过并说明。

### 1.3 mock vs 真盘
- **本步用 mock NVMe**（复用 m13 mock backend）：确定性 + 可 inspect 设备状态（写入字节、TRIM 调用、分配/回收），使**泄漏可证伪**（counter 对账）、**正确性可断言**（读回字节比对）。
- **真盘长跑**（验真实 I/O 下稳定/不泄漏）= follow-up，本步不做（052 §12.5 真盘 smoke 同线）。

---

## 2. 驱动结构

复用现有 e2e 脚手架（m13 matrix 的 runtime 搭建 + mock device + write/seal/point_get 驱动；inconel_test_reclaim 的 reclaim 驱动 + reclaim_stats inspector）。新 target 建议 `inconel_test_steady_e2e`。

驱动一个**可配置轮数 N** 的稳态循环：
```
for round in 1..N:
    write 一批 ops（PUT 新 key / overwrite 旧 key / DEL 部分 key），分布跨多 front
    若达到 seal 条件：seal → flush_round_once
    释放部分旧 read_handle（触发 guard/gen 析构 → reclaim）
    drive 各 scheduler advance 直到本轮 work drain（reclaim consumer、WAL reclaim 跑完）
    抽样 point_get 读回，比对 expected（in-test oracle map: key → 最新 value / tombstone）
每隔若干轮 + 末轮：跑泄漏/稳定性断言（§4）
```
- `expected` oracle：test 侧维护 `map<key, optional<value>>`（None=deleted），每次 write 更新，point_get 比对。
- 多核：ops 跨 `front_count` 分布，read_domain 多 shard，验跨核稳态。

---

## 3. flush 场景覆盖（核心要求，逐条必须命中）

| # | 场景 | 如何构造 | 断言 |
|---|---|---|---|
| F1 | **root-stable flush**（普通 leaf update，next-slot，range_base 不变，**不级联**——invariant A） | 对已有 leaf 内 key 小批 overwrite → seal → flush | flush 后新 read_handle 经 tree 读回新值；旧 read_handle 经 snapshot 读回旧值；**父节点未被无谓重建**（invariant A：child range_base 不变则不 cascade） |
| F2 | **Shadow CoW 跨 round**（项目存在理由 + 历史 INC-046 panic 点） | 同一 leaf range 连续**多轮** flush 各更新一次（每轮走 next-slot，range_base 稳定） | 跨 round **不 panic**；每轮后读回该 range 的 key 都是最新 winner；`manifest.resolve(child_base)` 经 slot_map 取 current slot 正确（invariant B） |
| F3 | **shadow slot 耗尽 → consolidation → 新 range** | 对同一 range 持续更新直到 shadow slot 满 | consolidation 产新 range、旧 range 进 retired→被 reclaim（TRIM + free_ranges 复用）；读回正确 |
| F4 | **leaf split**（树叶增多） | 持续插新 key 撑爆 leaf | split 出新 leaf + 新 separator；leaf_order 正确；读回正确 |
| F5 | **internal split + root split（root-change → superblock）** | 插足够多 key 让树**升层** | root_base_paddr 变 → superblock 异步更新；新 read_handle 经新 CAT2 读回正确（多轮升层混合） |
| F6 | **overwrite 跨 gen winner** | 同 key 在多个 sealed gen 各写一次 → flush | flush fold 取 max data_ver winner；读回最新 |
| F7 | **tombstone through flush** | DEL 已存在 key → seal → flush | 读回 not_found；DEL 不存在 key 不伪装 found；delete-to-empty 走 INC-047 保守 empty-leaf（**不**断言 prune/collapse——那是 INC-047 未做项） |
| F8 | **空轮 / no-op round** | 无 eligible gen 时触发 flush_round_once | no-op：不装 CAT2、不动 imms、标志正常清（055 B3） |

> F1/F2 是 Shadow CoW 主轴——**必须**在多轮连续场景里命中（不是单测一次），因为历史 bug 正是跨 round 才暴露。harness 要显式构造"同 leaf 跨多轮 flush"的序列。

---

## 4. 稳定性 / 泄漏断言（mock device counter 对账）

| 断言 | 机制 |
|---|---|
| **无 panic / 无 crash** | 长跑 N 轮（建议 N 足够大覆盖多次 root-change + consolidation + reclaim），进程正常退出 |
| **gen 不无界增长** | 稳态下 live `memtable_gen` 数有界（seal 产 gen、flush+release_gens+CAT 退役链释放）；末轮抽查 gen 计数不随轮数线性涨 |
| **盘空间不泄漏** | tree `free_ranges` 被 reclaim 后**复用**（consolidation 退役的 range 后续被 allocate 命中）；`tree_allocator.head` 在稳态 churn 下**有界**（不随轮数单调爆涨）；mock device 的 TRIM 调用数 > 0 且与退役量对账 |
| **value 回收** | reclaim 后 `value::reclaim_values` 被调；`reclaim_stats.partial_into_untracked == 0`（INC-052，经 facade inspector）；mock device value 区写入在纯 overwrite 稳态下不无界涨 |
| **WAL 段回收** | flush 推进 flush_durable_frontier 后 sealed WAL 段被回收（sealed 数下降）；`recovery_safe_lsn` 单调推进 |
| **读一致** | 全程 point_get vs oracle 零不符；旧 read_handle snapshot 隔离（flush/reclaim 不影响其结果） |

> 泄漏验证用 **counter 对账 + 有界性**（不是 ASAN——inconel 测试不跑 ASAN，share_nothing 硬停不 drain，feedback）。若要更强内存验证，真盘长跑 + 采样 RSS 是 follow-up。

---

## 5. 实现要点 / 边界
- test-author 角色：可读/写测试、复用 m13 + reclaim 脚手架；**不改 production**（发现 production bug 停下报告——这正是 e2e 的价值）。
- mock device 需暴露/已有：写入字节查询、TRIM 调用记录、按 LBA 读回——复用 m13 mock backend 的能力；不足处在 test 侧补 inspector，不碰 production。
- 不可干净观测的断言（如精确 cascade 与否）：用现有 manifest/inspector 间接验，或跳过 + 说明。
- Release 跑；测后 `pkill` 清残留。
- N 轮 + ops 规模：先取能稳定命中 F1-F8（尤其 root-change + consolidation + 多轮 shadow-CoW）的量级；不追求大数据量（那是 benchmark 线）。

## 6. 验收
- 新 target `inconel_test_steady_e2e` 编译 + 跑通,§3 F1-F8 全命中(覆盖到的有断言、跳过的有说明)、§4 稳定/泄漏断言全过。
- full build 全绿、既有套件零回归。
- 总报告声明:F1-F8 各覆盖/跳过(+原因)、§4 各断言结果、有无顺出 production bug。

## 7. 路线位置
稳态 e2e（本文，功能/稳定/正确性）→ step 4 boot recovery → 崩溃恢复 e2e + 真盘 smoke → 单独 benchmark 标准 + 性能测试。本步是 steps 1-3 的组合验证 + step 4 的去风险（recovery 复用稳态 on-disk 不变量，先证稳态正确）。
