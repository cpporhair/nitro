# 062: Inconel YCSB Execution Entry Design

## 1. 背景

`apps/kv` 里的 YCSB 入口证明了一件事：项目需要一个从命令行启动、初始化 runtime、顺序执行 load/run phase、打印统计结果的第一版执行入口。Inconel 目前只有测试和运行时 surface，还没有一个用户可以直接运行的真实盘 workload binary。

本步骤把 YCSB 做成 Inconel 的第一个项目级执行入口，目标不是完整复刻 YCSB 规范，而是提供一个能稳定驱动真实 NVMe、覆盖 write/read 公共 API、并且不破坏 PUMP 异步结构的最小生产入口。

## 2. 目标

1. 新增 `inconel_ycsb` binary。
2. 使用真实 NVMe 设备启动 Inconel runtime。
3. 支持 deterministic load 和 YCSB A/B/C 三类 run phase。
4. load 完成后显式执行 seal/flush barrier，使后续读可以经过 tree/value 路径验证。
5. 只通过 runtime public surface 驱动业务：
   - `rt::write_batch(...)`
   - `rt::point_get(...)`
   - `rt::seal_once(...)`
   - `rt::flush_once(...)`
6. 顶层应用只提交一个明确的 root pipeline，禁止在 helper 内部继续制造隐藏 pipeline。
7. 输出按 logical operation 计数的吞吐统计，并额外报告 batch/canonical entry 计数。

## 3. 非目标

1. 不在本步实现 YCSB E/scan。range scan 的公开 API 和执行路径尚未作为生产入口 surface 冻结。
2. 不实现 YCSB F/read-modify-write。RMW 会引入额外读写事务语义，不适合第一个入口。
3. 不实现 Zipfian/latest distribution。第一版使用 deterministic uniform key distribution，方便复现真实盘问题。
4. 不实现网络协议、Redis 协议或长期 daemon。
5. 不实现 recovery boot。第一版必须显式 `--force-format`，避免把未完成 recovery 入口伪装成可用能力。
6. 不新增隐藏 background scheduler。维护 cadence 仍由 061 的 runtime maintenance scheduler 负责，YCSB 只拥有自己的 app driver。
7. 不在第一版实现 run phase 中的周期性 seal/flush interleave；当前只实现 load 后一次 `seal_once -> flush_once` barrier。后续若要长跑 cadence，必须仍挂在同一个 app root 下做 window driver。

## 3.1 当前实现切片

本提交落地以下能力：

1. `inconel_ycsb` target。
2. CLI parser 与 help 输出。
3. `--force-format` production helper，按 `kBootstrapFormatProfile` 写 superblock A/B。
4. deterministic load、A/B/C、load-A/B/C。
5. load 后可选一次 `seal_once -> flush_once`。
6. load 后抽样 point-get verification。
7. stats 输出。
8. 一个位于 `apps/inconel/ycsb/main.cc` 的顶层 app root submit。

刻意未落地：

1. `--seal-flush-interval-ops`。
2. latency histogram。
3. recovery boot / no-format startup。
4. scan/RMW/Zipfian。

## 4. 从 KV YCSB 借鉴与拒绝

可借鉴：

1. binary 入口负责启动 runtime。
2. workload 以 phase 串行组织：`load -> run`。
3. 每个 phase 有自己的统计上下文。
4. workload 由 batch/concurrency 参数控制，而不是单线程逐条同步调用。

必须拒绝：

1. 不使用 `spdk_get_ticks() % max` 作为随机 key；改用 seed 固定的 PRNG。
2. 不硬编码 3KB value；value size 由 CLI 配置。
3. 不做每条 load `fprintf`。
4. 不固定 `concurrent(10000)`；并发度由 CLI 配置，并设置保守默认值。
5. 不泄漏统计对象；runner 持有明确生命周期。
6. 不把 scan 入口先写死成不可验证的壳。

## 5. 目录与目标

新增目录：

```text
apps/inconel/ycsb/
  config.hh
  workload.hh
  stats.hh
  runner.hh
  format_device.hh
  main.cc
```

CMake 新增目标：

```text
inconel_ycsb
```

该目标链接真实 NVMe runtime 需要的 production library，不依赖 test helper，不 include `apps/inconel/test/**`。

## 6. CLI

第一版 CLI 直接手写 parser，避免为了一个入口引入新依赖。

必选：

1. `--pci-addr <BDF>`：真实 NVMe BDF；也允许环境变量 `INCONEL_NVME_PCI_ADDR` 作为 fallback。
2. `--force-format`：第一版强制要求。缺失时 fail-fast 并解释原因。

workload：

1. `--workload load`
2. `--workload a`
3. `--workload b`
4. `--workload c`
5. `--workload load-a`
6. `--workload load-b`
7. `--workload load-c`

规模：

1. `--records <N>`：load 的 key 数，也是 run phase 默认 key range。
2. `--operations <N>`：run phase logical operation 数。第一版要求 run phase 使用固定 operation count。
3. `--value-size <bytes>`：value body 大小。
4. `--batch-size <N>`：每个 write batch 的 client-side entry 数；默认 `1`，性能实验可显式调大。
5. `--inflight <N>`：应用层同时在飞请求数。
6. `--seed <u64>`：PRNG seed。

runtime：

1. `--cores <csv>`：PUMP/Inconel 使用的 core 列表。
2. `--main-core <id>`：提交 app root 的 core。
3. `--front-cores <csv>`：front owner core 列表；缺省从 `--cores` 派生。
4. `--value-core <id>`
5. `--owner-core <id>`
6. `--coord-core <id>`
7. `--wal-space-core <id>`
8. `--tree-cache <clock|slru>`
9. `--value-cache <clock|slru>`
10. `--tree-cache-capacity <N>`
11. `--value-cache-capacity <N>`

cadence：

1. `--flush-after-load` / `--no-flush-after-load`
2. `--verify-samples <N>`：load flush 后抽样 point-get 校验 deterministic value。
3. Deferred: `--seal-flush-interval-ops <N>`。这需要把 run phase 拆成 window driver，不能在 helper 里补一个隐藏 root。

## 7. 设备启动与格式化

第一版使用 production format helper：

```text
apps/inconel/ycsb/format_device.hh
```

职责：

1. 根据 runtime format profile 构造 layout plan。
2. 写入 superblock A/B。
3. 初始化 runtime 启动需要的空树 / 空 WAL / 空 value area 元数据。
4. 只使用 production `format/*`、`nvme/*` API。
5. 不依赖测试 helper。

强制规则：

1. `--force-format` 必须存在，否则退出。
2. 格式化只作用于显式传入的 `--pci-addr` 设备。
3. 文档和提示必须继续强调不要使用系统盘 BDF。

后续 recovery 入口完成后，再把 `--force-format` 改成显式 destructive option，并允许普通 boot。

## 8. Runtime 启动方式

`runtime::start_runtime(...)` 当前是固定启动封装，没有 workload callback。YCSB 入口需要使用更低层组合：

```text
runtime::build_runtime(...)
rt::start(rt, cores, main_core, on_init)
```

`on_init` 规则：

1. 每个 core 都会进入 `on_init`。
2. 只有 `core == main_core` 时提交 workload app root。
3. 其他 core 只参与 scheduler advance。
4. app root 完成后，统一清理 runtime run flags，让 `rt::start` 返回。
5. app root 捕获异常，`rt::start` 返回后在 main thread 重新抛出或转成 non-zero exit。

根提交规则：

1. `apps/inconel/ycsb/main.cc` 允许出现一个顶层 `make_root_context + submit`。
2. `runner.hh`、`workload.hh`、`stats.hh` 不允许内部 root submit。
3. seal/flush cadence 由同一个 app root 串行或组合驱动，不能拆成隐藏 background pipeline。
4. 061 maintenance scheduler 是已知 runtime cadence 例外；YCSB 不新增第二套维护提交。

## 9. Workload 定义

Key：

```text
<key-prefix><zero-padded decimal id>
```

默认 prefix 为 `user`，默认宽度足够覆盖 `records`。

Value：

1. deterministic。
2. 包含 key id、phase kind、update generation 和 seed 派生内容。
3. 精确填充到 `--value-size`。
4. 校验时按同一函数重建 expected value。

PRNG：

1. 使用简单固定算法，例如 splitmix64。
2. 每个 phase 从 `seed` 派生独立 stream。
3. 不读时间、不读设备 tick 作为随机源。

Workload mix：

1. Load：顺序 PUT `[0, records)`。
2. A：50% point-get，50% update PUT。
3. B：95% point-get，5% update PUT。
4. C：100% point-get。

Distribution：

1. v1 uniform in `[0, records)`。
2. update 不扩展 key range，只覆盖已有 key。
3. 后续 Zipfian/latest 要独立设计，不能把统计语义混进 v1。

## 10. Batch 语义

Inconel `write_batch` 的 durable boundary 是 canonicalized client batch。YCSB 统计里的 operation 则是 logical client operation。

因此第一版统计分三类：

1. `generated_ops`：YCSB logical ops。
2. `submitted_batches`：调用 `rt::write_batch` 的次数。
3. `acked_entries`：runtime 返回的 canonical entry 数。

默认 `--batch-size=1`，这样 YCSB logical op 和 canonical entry 不会因为 same-key last-wins 混淆。性能实验可以显式提高 batch size；这时报告必须同时显示 generated ops 和 acked canonical entries。

读请求不与写请求合并成一个 Inconel batch；A/B workload 的 mix 在 runner 层生成，分别调用 `point_get` 或 `write_batch`。

## 11. Phase 编排

### 11.1 Load

```text
for id in [0, records):
  build PUT
  submit write_batch
  bounded inflight

if flush_after_load:
  seal_once()
  flush_once()

if verify_samples > 0:
  sample point_get keys
  compare deterministic value
```

load flush 的目的不是为了让数据“刚写完就可见”；write ACK 已经表示 read-handle 可见。它的目的是把第一版入口覆盖到 tree/value read path，而不是只读 memtable。

### 11.2 Run A/B/C

```text
for op_index in [0, operations):
  choose op by workload mix
  choose key by deterministic uniform stream
  submit point_get or write_batch
  bounded inflight
  optionally insert seal/flush barrier every N completed ops
```

第一版建议默认 run phase 不做周期 seal/flush；真实盘长跑或 cache/tree 路径验证时显式设置 `--seal-flush-interval-ops`。

### 11.3 Seal/Flush Barrier

barrier 是同一个 app driver 中的 sender sequence：

```text
seal_once()
flush_once()
```

当前策略：

1. 第一版只在 load 后执行一次。
2. `seal_once()` 当前没有 no-op 结果；因此不做“重复直到 seal/flush 都 no-op”的循环，避免人为制造空 sealed gen。
3. 不调用 `reclaim_once()` 作为 barrier 的一部分；061 production maintenance cadence 负责 reclaim/trim。
4. 后续若需要 run phase 周期 barrier，应把 run phase 切成 fixed windows，在同一个 app root 内按 `window -> barrier -> next window` 串行，不新增 root submit。

## 12. 统计

每个 phase 输出：

1. phase name。
2. elapsed seconds。
3. generated logical ops。
4. submitted batches。
5. acked canonical entries。
6. read found。
7. read miss。
8. write errors。
9. read errors。
10. logical ops/sec。
11. batches/sec。

第一版不做精细 latency histogram。后续如果要做 latency：

1. 默认 sampling，避免每 op 分配/记录。
2. 分 read/write phase。
3. 明确统计时钟来源和 percentile 算法。

## 13. 错误处理

1. CLI 错误直接打印 usage 并返回 2。
2. runtime/build/device 错误返回 1。
3. workload 内任一 sender 异常保存到 shared run state，停止 runtime 后在 main thread 报告。
4. read miss 在 load verification 中是 correctness failure；在普通 run phase 中按 workload 统计为 miss。
5. YCSB runner 不吞掉 Inconel panic/contract violation。

## 14. 与异步碎片化问题的关系

本步骤必须避免之前 `submit_reclaim_invalidate` 一类问题复发。

允许：

1. `main.cc` 提交一个顶层 app root。
2. 061 maintenance scheduler 作为 runtime-owned cadence 提交 maintenance root。

禁止：

1. workload helper 内部调用 `submit`。
2. seal/flush helper 内部创建 root context。
3. stats 或 verification helper 内部提交独立 pipeline。
4. runtime scheduler handler 内部为了方便继续提交新 pipeline。

检查方式：

```text
rg -n 'make_root_context|pump::sender::submit|the_null_receiver|submit\\(' apps/inconel \
  -g '*.hh' -g '*.cc' -g '!apps/inconel/test/**'
```

预期新增命中只应落在 `apps/inconel/ycsb/main.cc`，并且是顶层 app root。

## 15. 验证计划

静态：

1. `git diff --check`
2. root submit scan
3. production code `virtual/override` scan

构建：

```text
cmake --build build_real --target inconel_ycsb -j2
```

真实盘 smoke：

```text
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib \
  timeout 300s build_real/inconel_ycsb \
  --pci-addr 0000:04:00.0 \
  --force-format \
  --workload load-c \
  --records 10000 \
  --operations 10000 \
  --value-size 256 \
  --verify-samples 128
```

真实盘约束继续沿用 `ai_context/inconel/real_nvme_test_guide.md`：

1. 不使用系统盘 `0000:03:00.0`。
2. 优先使用 scratch 盘 `0000:04:00.0`。
3. 使用 `build_real`。
4. 使用 vendored SPDK/DPDK `LD_LIBRARY_PATH`。

## 16. 后续步骤

1. recovery boot 完成后，把 `--force-format` 从强制项改成 destructive option。
2. range scan public API 落地后，实现 YCSB E。
3. 读写路径稳定后增加 Zipfian/latest distributions。
4. 需要长期压测时，把 app driver 扩展成单 root 下的 workload loop + seal/flush cadence loop，而不是新增隐藏 root。
5. 当网络协议入口出现后，复用 workload/config/stats，但保留 `inconel_ycsb` 作为直接真实盘诊断入口。
