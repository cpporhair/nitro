# 066: Inconel YCSB Consistency Test Design

## 状态

064 recovery boot 已覆盖 empty boot、WAL replay、existing tree scanner、
same-shape WAL delta replay 和 recovery-local full CoW replay。065 已把
YCSB 入口收敛到可复现的 JSON config + CLI override 模型。

本设计定义 066 的一致性测试体系：用现有 `inconel_ycsb`、后续 test-only
oracle/harness 和 real NVMe scratch 设备覆盖持续读写、读写并发、自动
maintenance flush、显式 flush、clean restart recovery、WAL replay、
tombstone frontier carrier、各种版本切换和后续写入不回退。

当前已人工验证过的 scratch NVMe smoke：

- force-format YCSB load / mixed read-update。
- no-force recovery 后 workload C 读回。
- delete-only WAL replay 后 tombstone 读 miss。
- `--flush-after-load` 显式 seal+flush 后 verify。
- `inconel_test_flush_e2e` 三轮 flush。
- 低 maintenance 阈值下完整 `load-a` 持续写自动触发大量 flush：
  `seal=189 flush=197 non_noop_flush=188`，读写错误为 0。
- `inconel_test_ycsb_concurrency_checker_e2e` 已覆盖 C1/C2/C3/C4/C5/C6：put/read
  interval checker 在 maintenance-off 和 aggressive auto-flush 两种 real
  NVMe 场景下通过，C2 观察到 automatic seal，C3 观察到 non-noop flush；
  delete/read/put race 已通过 tombstone-aware checker；batch ACK barrier
  已验证 ACK 后同 batch 全部 key 可见；frontier switch barrier 已验证
  sentinel batch 在后续 non-noop flush/frontier switch 后仍逐 key 可见。
- `apps/inconel/scripts/ycsb_consistency.sh` 已覆盖 C7/C8 脚本级 recovery
  continuation：existing tree + WAL update delta recovery 后继续写入并重启
  校验；tombstone WAL delta recovery 后继续 PUT 并重启校验。
- `inconel_test_recovery_boot_ram_device` 已作为 066D deterministic RAM-image
  harness 覆盖首批 recovery crash-point image oracle：WAL torn tail、
  incomplete batch、duplicate key fail-fast、same-shape next-slot before WAL
  reset、full-CoW superblock switch before WAL reset、partial WAL reset，以及
  tree free range scrub continuation。

这些 smoke 证明了当前路径能工作，但还不是完整一致性测试体系。066
的目标是把这些场景固化成可复跑、可审查、可扩展的测试标准。

## 背景

Inconel 当前的核心风险已经从“能否启动/写入”转到“跨 flush、跨 WAL
reset、跨 recovery 后是否仍能保持同一份 logical state”。尤其是：

- automatic maintenance 会在前台持续写入时并发 seal/flush。
- recovery 会把 WAL delta 合并进 existing tree，并 reset WAL。
- delete-only / no-op tombstone 必须持久携带 durable LSN frontier。
- tree snapshot 自身的 `max_data_ver` 必须参与 `next_lsn` 推导，不能只看 WAL。
- Value Area 不能被 recovery 扫描，`live_value_refs` 必须完全来自 clean tree
  + complete WAL winners。

单次命令输出 `write_errors=0/read_errors=0` 只能说明一次运行没有显式
报错。066 要把“正确”拆成可证伪的不变量，并规定每类场景怎么构造、
怎么验收、哪些结论不能从当前工具得出。

## 目标

1. 定义 YCSB 持续读写 + automatic maintenance flush 的 real NVMe 测试。
2. 定义 flush 后 recovery、WAL-only recovery、existing tree + WAL delta
   recovery 的一致性测试。
3. 定义 update winner、delete tombstone、frontier carrier、next_lsn 不回退
   的测试序列。
4. 定义读写并发下的强一致性验收：ACK 后可见、单 key 版本不回退、
   delete 不穿透、flush/frontier/recovery 切换不复活旧版本。
5. 把每个测试的命令形态、前置条件、验收输出写清楚。
6. 区分当前工具能验证的结论和需要后续 harness/oracle 才能验证的结论。
7. 不扩展 Inconel public KV API；所有测试通过 YCSB / existing runtime
   hooks / recovery boot 入口完成。

## 非目标

- 不新增 MultiGet、scan、transaction 或外部 KV API。
- 不把测试结果当性能 benchmark。吞吐数只用于确认负载真实执行。
- 不在 recovery 中扫描 Value Area。
- 不靠 test fixture 反推 production spec。
- 不并行运行多个 real-NVMe 测试进程抢同一个 BDF。
- 不把 `0000:03:00.0` 纳入任何测试；它是系统盘。

## 安全前置

所有 real NVMe 测试必须先执行：

```bash
sudo -n /home/null/work/kv/spdk/scripts/setup.sh status
```

验收：

- `0000:04:00.0` 是 `vfio-pci`。
- `0000:03:00.0` 显示 mounted / active，并且绝不出现在测试命令中。
- 不存在另一个 Inconel/SPDK 进程占用 `0000:04:00.0`。

运行环境：

```bash
export INCONEL_REAL_NVME_LIBS=/home/null/work/kv/spdk/build/lib:/home/null/work/kv/spdk/dpdk/build/lib
```

命令统一使用：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb ...
```

构建门禁：

```bash
cmake --build build_real --target \
  inconel_ycsb \
  inconel_real_nvme_compile_check \
  inconel_test_ycsb_concurrency_checker_e2e \
  -j2
```

涉及 legacy e2e 时按 `real_nvme_test_guide.md` 追加对应 target。

## 强一致性模型

066 的“强一致性”不是简单的“最后读到了某个值”。测试必须按 operation
history 判断每一次读是否可以线性化到某个合法版本点。

### Write ACK 规则

对任意 key，write batch ACK 返回后：

- 同一进程内随后开始的 point_get 必须看到该 key 的最新 acknowledged
  value，或看到更新的 acknowledged tombstone。
- 如果后续没有 newer write/delete ACK，不能再读到 ACK 前的旧版本。
- ACK 后跨 seal、flush、frontier switch、WAL reset、recovery restart 仍然
  必须满足同一规则。

### Concurrent read/write 规则

读和写时间区间重叠时，point_get 可以返回：

- read start 前已经 ACK 的最新版本。
- 与该 read 区间重叠并最终 ACK 的某个 newer write 版本。

但不能返回：

- 比 read start 前已 ACK 最新版本更旧的版本。
- 从未 ACK 的 future version。
- 被 read start 前已 ACK tombstone 遮蔽的旧 value。

单 key put-only 场景下，每个 reader 线程观察到的 generation 必须单调不降。
delete/put 交错时，reader 观察到的状态序列必须能解释为 ACK history 的合法
前缀：value -> tombstone -> newer value 可以出现，tombstone 之后不能再看到
older value。

### Batch ACK 规则

YCSB / write_batch 可以把多个 ops 合成一个 batch。batch ACK 后，同一 batch
内所有 key 的 updates 都必须对后续 reads 可见。066 不要求现有 point_get API
提供 multi-key snapshot；但 test-only harness 可以在 ACK 后逐 key 校验同一
batch 的所有 sentinel keys 都更新完成，防止部分 batch publish。

### Version switch 规则

以下版本切换都不能改变 logical winner：

- active memtable -> immutable/sealed gen。
- sealed gen -> tree-local flush candidate。
- old CAT/PRS -> new CAT/PRS frontier switch。
- WAL-only state -> recovered clean tree。
- existing clean tree + WAL delta -> recovered CoW tree。
- WAL reset 后 clean restart。

每个切换点之后，按 operation history 校验的 winner 必须一致。

## 一致性不变量

### I1: Read-your-load

`force-format load` 后，按同一 key/value generator 采样读必须命中并返回
generation 0 的 value。

可由现有 `--verify-samples N` 覆盖。

### I2: Update winner

同一 key 多次 PUT 后，读路径必须返回最大 logical generation / LSN 的 value。

当前 `inconel_ycsb` 对 pure `update` workload 可以用
`--verify-existing-updates` 校验。对 mixed YCSB-A/YCSB-B，当前 helper
不能精确校验所有 value 版本，因为 existing-update 预期函数必须同时考虑
`choose_operation()` 是否真的选择了 update。066B 需要补 external oracle
或修正 YCSB expected-state 计算后，才能宣称 mixed workload 的 value-exact
一致性。

### I3: Tombstone opacity

DELETE 后读取同 key 必须 miss。tombstone 不允许被 older tree value、
older WAL value 或 recovery merge 顺序穿透。

可由 `--verify-existing-deletes` 和 read-only workload C 覆盖。

### I4: Tombstone frontier carrier

delete-only / no-op tombstone 即使没有 live value，也必须写入 tree，作为
durable frontier carrier。否则 WAL reset 后下一次 boot 的 `next_lsn`
可能回退。

测试必须覆盖：

1. load + flush 建立 clean tree。
2. no-force delete-only 写入 WAL。
3. no-force boot recovery replay tombstones 并 reset WAL。
4. 再次 no-force 写同一批 key。
5. 再次 no-force boot 后读到新 PUT，而不是旧 tombstone。

### I5: Flush visibility

显式 flush 或 automatic maintenance non-noop flush 完成后，后续 read_handle
必须能从 clean tree 读到 flushed logical winners。

可由 `--flush-after-load` + verify、以及 no-force restart 后 workload C 覆盖。

### I6: Automatic maintenance equivalence

前台持续写入过程中 automatic maintenance 触发的 seal/flush，与手动
`seal_once + flush_once` 在 logical state 上等价。验收不能只看 flush
counter；还必须重启后读回。

### I7: WAL-only recovery

未显式 flush 的完整 WAL batch 必须在 no-force boot 时 replay 到 clean
tree。recovery 后同 key 读结果必须等价于崩溃前 logical state。

### I8: Existing tree + WAL delta recovery

已有 root/tree 时，如果 WAL 中存在 newer winner：

- same-shape 可用时写 next shadow slot。
- same-shape 不可用时走 recovery-local full CoW。
- 必要时更新 inactive superblock。
- tree writes 和 superblock update durable 后才 reset WAL。

测试需要分别构造 no split、leaf split、root range change 三种 shape。

### I9: Clean restart idempotence

一次 recovery reset WAL 后，第二次 no-force boot 应走 clean tree + WAL empty
路径，不再改变 logical state。读回结果必须稳定。

### I10: Allocator no-overlap

recovery 安装 runtime 后，继续写入和 flush 不得让 tree allocation 覆盖
live value area。测试通过后续 write+flush+restart 间接覆盖；更强验证需要
allocator telemetry 或 offline scanner。

### I11: ACK-after-read barrier

每个 writer ACK 后插入一个 reader barrier。barrier 之后发起的 reads 必须
看到该 ACK 或更新版本。这个不变量是最小强一致性门槛。

### I12: Concurrent single-key monotonicity

对同一个 hot key，writer 连续 PUT generation `1..N`，多个 reader 并发
point_get。每个 reader 看到的 generation 不能下降；任一 read 返回的
generation 必须落在它的 read interval 可线性化的 ACK 集合内。

### I13: Concurrent delete opacity

对同一个 key 执行 `PUT g1 -> DELETE -> PUT g2`，并发 readers 不能在 DELETE
ACK 后再看到 `g1`。如果读到 value，必须是 DELETE 前合法并发读到的 `g1`，
或 PUT g2 ACK 后合法读到的 `g2`。

### I14: Frontier switch under readers

读线程持续读 hot/cold key 集合，同时 maintenance 触发 seal/flush/frontier
switch。reader 不能看到 version regression，也不能出现 mismatch、miss
spike 或 tombstone 穿透。

### I15: Recovery version monotonicity

任意 no-force boot 后继续写入，新 ACK 的 LSN/data_ver 必须大于 recovered
durable frontier。测试上表现为：recovery 后的新 PUT 必须能覆盖 recovery
前的 value/tombstone winner，且再次重启后仍保持新 winner。

## 测试分层

### 066A: Manual real-NVMe consistency suite

目标是用现有 `inconel_ycsb` 命令固化一套可手动复跑的 smoke。无需改
production code，也不需要新 public API。

适用结论：

- load/read/update/delete 基本路径。
- automatic maintenance 确实触发 non-noop flush。
- flush/recovery 后 logical read 结果正确。
- tombstone recovery 不穿透。

限制：

- mixed YCSB-A/B 的 exact value winner 只能部分验证。
- 不覆盖真实进程崩溃点。
- 不覆盖 long soak。

### 066B: Scripted YCSB oracle suite

目标是在不扩展 public KV API 的前提下，为 YCSB 增加测试侧 oracle：

- 根据 deterministic workload generator 记录每个 key 的 expected state。
- 跨多次 process run 持久化 oracle 文件。
- 每次 no-force restart 后按 oracle 抽样或全量 point_get verify。
- mixed A/B 必须只对实际 update 操作推进 expected generation。
- DELETE 把 key 标成 tombstone；后续 PUT 可重新置为 value。

当前已落地为 YCSB 内部 test-only flags：`--expect-file` /
`--write-expect-file` / `--expect-samples` / `--expect-all`。它们不改变
Inconel public KV API，只扩展 `inconel_ycsb` harness。

### 066C: Read/write concurrency linearizability suite

目标是覆盖真实读写并发和各种版本切换。066A/YCSB phase 统计只能证明“没有
报错”，不能证明每次并发读都符合 strong consistency。

066C 需要一个 test-only harness：

- 复用当前 YCSB key/value generator。
- 多 writer、多 reader 同时运行。
- 每个 operation 记录 start tick、end tick、key、op、generation、result。
- writer ACK 时把 operation 放入 per-key acknowledged history。
- reader 结果用 per-key history 做线性化检查。
- harness 运行期间主动或通过低 maintenance 阈值触发 seal/flush。
- harness 可在 process 退出后保存 history，no-force restart 后继续校验。

这仍然不扩展 public KV API；它只是测试程序或 YCSB test mode。

当前已落地的 066C 入口是
`inconel_test_ycsb_concurrency_checker_e2e`，脚本场景为 `c1`、`c2`、`c3`、
`c4`、`c5`、`c6`、`c7` 和 `c8`：

```bash
apps/inconel/scripts/ycsb_consistency.sh c1
apps/inconel/scripts/ycsb_consistency.sh c2
apps/inconel/scripts/ycsb_consistency.sh c3
apps/inconel/scripts/ycsb_consistency.sh c4
apps/inconel/scripts/ycsb_consistency.sh c5
apps/inconel/scripts/ycsb_consistency.sh c6
apps/inconel/scripts/ycsb_consistency.sh c7
apps/inconel/scripts/ycsb_consistency.sh c8
```

实现状态：

- C1：maintenance-off 的 hot-key PUT/point_get 并发 interval checker。
- C2：auto-seal profile 下同一 checker，并要求
  `checker_maintenance.seal > 0`。当前 production maintenance 会在
  `seal_requested` 时同轮允许 flush，因此 C2 不断言
  `flush/non_noop_flush == 0`。
- C3：aggressive auto-flush profile 下同一 checker，并要求
  `checker_maintenance.seal/flush/non_noop_flush > 0`。
- C4：maintenance-off 的 hot-key DELETE/point_get/PUT 并发 checker，按
  per-key value/tombstone history 验证 tombstone 不穿透、旧 value 不复活。
- C5：aggressive auto-flush profile 下每个 write batch ACK 后逐 key
  point_get exact barrier，并要求 `checker_barrier.reads == 4096` 以及
  `checker_maintenance.seal/flush/non_noop_flush > 0`。
- C6：aggressive auto-flush profile 下先 flush preload 并等 clean baseline，
  再写覆盖所有 hot keys 的 sentinel batch；从 baseline 之后等待新的
  non-noop flush/frontier switch，随后逐 key exact point_get，并要求
  ACK 后 immediate `checker_barrier.reads == 64`、post-frontier
  `checker_frontier_barrier.reads == 64`、`generation == 2`、
  `checker_frontier_window.reads > 0` 以及
  `checker_maintenance.seal/flush/non_noop_flush > 0`。
- C7：脚本级 existing-tree + WAL update delta recovery continuation。先
  `load + flush` 建 clean tree 并写出 expected-state 文件，再写 update WAL
  delta 并产出 delta oracle；no-force recovery 后用 `--expect-all` exact
  校验；随后用不同 seed 全量 PUT 覆盖所有 key，第二次 no-force restart 后
  用新的 oracle 校验没有版本回退。
- C8：脚本级 tombstone frontier continuation。先 `load + flush` 建 clean tree，
  再写 delete-only WAL delta 并产出 tombstone oracle；no-force recovery 后用
  `--expect-all` 校验 tombstone；随后继续 PUT 全量 key 并产出新 oracle，第二次
  no-force restart 后 exact 读回 found，证明 tombstone frontier 没有遮蔽新写入。
- 尚未实现：跨 process interval history checker；C7/C8 当前是
  phase-level YCSB/recovery 脚本，不是并发 reader checker。

### 066D: Fault-injection recovery suite

目标是覆盖 crash-point correctness，而不是 clean process restart：

- WAL torn tail。
- incomplete batch。
- tree page written but superblock not updated。
- inactive superblock written but WAL not reset。
- WAL reset interrupted。
- same-shape next slot write interrupted。
- full CoW leaf split / root rewrite interrupted。

这层需要 fake/block-device harness 或可控 fault injection。不能用“手动
kill 进程”替代所有 crash 点，因为 kill timing 不可复现，无法保证命中了
目标持久化边界。

## 066A 测试矩阵

066A 已有脚本入口：

```bash
apps/inconel/scripts/ycsb_consistency.sh [all|a0|a1|a2|a3|a4|a5|a6|a7|a8|a9|a10|c1|c2|c3|c4|c5|c6|c7|c8]
```

脚本职责：

- 构建 `inconel_ycsb`、`inconel_real_nvme_compile_check` 和
  `inconel_test_ycsb_concurrency_checker_e2e`。
- 检查 scratch BDF 绑定到 `vfio-pci`。
- 显式拒绝 known system-disk BDF `0000:03:00.0`。
- 用 per-BDF lock 防止多个 real-NVMe 测试进程抢同一块盘。
- 串行运行 real-NVMe YCSB 命令，避免多个进程抢同一块盘。
- 解析 phase / maintenance counters，要求所有 real run 的
  YCSB `maintenance.failed=0`；对 C2/C3/C5/C6 要求
  `checker_maintenance.failed=0`，并按本节验收规则 fail-fast。

`all` 执行 A0-A10 和当前已落地的 C1/C2/C3/C4/C5/C6/C7/C8。单项场景适合调试或重跑失败项。

### A0: Config and device safety

目的：确认测试不会误伤系统盘，且 dry-run 不打开 NVMe。

命令：

```bash
build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --dry-run \
  --no-print-config \
  --dump-config
```

负向：

```bash
build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --dry-run \
  --no-print-config \
  --pci 0000:03:00.0
```

验收：

- 正向 exit 0。
- dump 中 `device.pci_addr == "0000:04:00.0"`。
- 负向 exit 2，拒绝 known system-disk BDF。

### A1: Force-format load + point verification

目的：覆盖 fresh format、load writes、read-your-load。

命令：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load \
  --records 1000 \
  --operations 1000 \
  --verify-samples 64 \
  --no-print-config
```

验收：

- `load.write_errors=0`
- `verify.read_found=64`
- `verify.read_miss=0`
- exit 0

### A2: Explicit flush after load

目的：覆盖 `seal_once + flush_once`，确认 flush 后仍可读。

命令：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load \
  --records 1000 \
  --operations 1000 \
  --verify-samples 64 \
  --flush-after-load \
  --no-print-config
```

验收：

- `load-flush.ops=1`
- `load-flush.batches=1`
- `verify.read_found=64`
- 所有 error counters 为 0。

### A3: Restart after explicit flush

目的：覆盖 clean tree + WAL empty/no-op boot 后读回。

命令：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --workload c \
  --records 1000 \
  --operations 1000 \
  --verify-samples 0 \
  --no-print-config
```

验收：

- `run.read_found=1000`
- `run.read_miss=0`
- 所有 error counters 为 0。

### A4: WAL-only load recovery

目的：覆盖未 flush 的 complete WAL replay。

序列：

1. `--force-format --workload load --records 1000 --verify-samples 64`
2. no-force `--workload c --records 1000 --operations 1000`
3. 再次 no-force `--workload c --records 1000 --operations 1000`

验收：

- 第 2 步 `run.read_found=1000 read_miss=0`。
- 第 3 步同样 `read_found=1000 read_miss=0`，证明 WAL reset 后 clean restart
  稳定。

### A5: Automatic maintenance flush under continuous writes

目的：覆盖完整 YCSB 持续写自然触发 production maintenance seal/flush。

命令使用低阈值测试 profile，目的是强制覆盖路径，不代表默认阈值的频率：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --force-format \
  --workload load-a \
  --records 10000 \
  --operations 10000 \
  --value-size 256 \
  --verify-samples 64 \
  --maintenance-seal-active-bytes 65536 \
  --maintenance-total-memtable-bytes 262144 \
  --maintenance-wal-seal-percent 5 \
  --maintenance-max-sealed-gens-per-front 1 \
  --no-print-config
```

验收：

- `load.write_errors=0`
- `verify.read_found=64`
- `run.write_errors=0`
- `run.read_errors=0`
- `maintenance.failed=0`
- `maintenance.seal > 0`
- `maintenance.flush > 0`
- `maintenance.non_noop_flush > 0`

注意：对 mixed `load-a`，当前只能证明运行无错误且读未 miss；不能证明所有
updated values 都是最新 generation。exact winner 校验归 066B。

### A6: Restart after automatic flush

目的：确认 A5 的 automatic flush 后盘面可 recovery。

命令：

```bash
sudo -n env XDG_RUNTIME_DIR=/tmp \
  LD_LIBRARY_PATH="$INCONEL_REAL_NVME_LIBS" \
  timeout 300s build_real/inconel_ycsb \
  --config apps/inconel/ycsb/config.sample.json \
  --workload c \
  --records 10000 \
  --operations 10000 \
  --verify-samples 0 \
  --no-print-config
```

验收：

- 如果 A5 没有 DELETE，`run.read_found=10000 read_miss=0`。
- 所有 error counters 为 0。

### A7: Pure update winner across restart

目的：在当前工具能力内覆盖 exact update value winner。

序列：

1. force-format `load --records 1000 --verify-samples 64`
2. no-force `update --records 1000 --operations 2000 --verify-samples 0`
3. no-force `c --records 1000 --operations 1000 --verify-existing-updates --verify-samples 128`

验收：

- 第 2 步写错误为 0。
- 第 3 步 `verify.read_found=128 read_errors=0`。
- 第 3 步 workload C `read_found=1000 read_miss=0`。

### A8: Delete-only tombstone recovery

目的：覆盖 tombstone replay 和不穿透。

序列：

1. force-format `load --records 1000 --verify-samples 64`
2. no-force `del --records 1000 --operations 1000 --verify-samples 0`
3. no-force `c --records 1000 --operations 1000 --verify-existing-deletes --verify-samples 64`

验收：

- 第 2 步 `run.acked_entries=1000`。
- 第 3 步 `verify.read_found=0 read_miss=64`。
- 第 3 步 `run.read_found=0 read_miss=1000`。

### A9: Tombstone frontier no-regression

目的：证明 delete-only recovery 后 WAL reset 不会让 `next_lsn` 回退。

序列：

1. 执行 A8 到第 3 步，让 tombstones 进入 clean tree 并 reset WAL。
2. no-force `load --records 1000 --operations 1000 --verify-samples 64`
3. no-force `c --records 1000 --operations 1000 --verify-samples 0`

验收：

- 第 2 步 verify 能读到 generation 0 value。
- 第 3 步 `run.read_found=1000 read_miss=0`。

如果 `next_lsn` 回退到 tombstone data_ver 以下，第 2/3 步会被 older
tombstone 遮蔽，读会 miss 或 exact verify 失败。

### A10: Expected-state oracle for mixed workloads

目的：覆盖 mixed YCSB-A/B 的 exact winner 校验，并证明 oracle 文件跨
process run 生效。

序列：

1. force-format `load-a --records 1000 --operations 1000 --inflight 1
   --expect-all --write-expect-file <file>`
2. no-force `c --records 1000 --operations 1000 --expect-file <file>
   --expect-all`
3. force-format `load-b --records 1000 --operations 1000 --inflight 1
   --expect-all --write-expect-file <file>`
4. no-force `c --records 1000 --operations 1000 --expect-file <file>
   --expect-all`

验收：

- A/B 两段的 `run.acked_entries` 分别等于对应 mixed workload 中实际 update 数。
- A/B 两段的 `expect.read_found=1000 read_miss=0 read_errors=0`。
- A/B 两个 expected-state 文件都存在且非空。
- 两次 no-force restart 后 `run.read_found=1000 read_miss=0`。
- 两次 no-force restart 后 `expect.read_found=1000 read_miss=0 read_errors=0`。
- 所有 real run 的 `maintenance.failed=0`。

`--inflight 1` 是故意的：YCSB expected-state oracle 是 ACK 后精确状态机，
只覆盖串行 mutating workload。并发 mutation 的合法线性化检查归 066C。

### Existing tree + WAL delta shape coverage

当前 YCSB 缺一个“一次运行校验多个 key_prefix / 多个 oracle 集合”的能力。
因此 full CoW real-NVMe exact 验证仍是 066B 后续扩展项：需要让脚本或
oracle 支持多个 key ranges / key_prefix 集合后，再把 same-shape、leaf split
和 root range change 三类 recovery delta 固化为 real-NVMe 场景。

## 066B Oracle Design

### Expected-state model

Oracle 按 logical key 存：

```text
key -> { kind: value | tombstone, generation, value_hash }
```

规则：

- load/PUT：`kind=value`，`generation` 使用 YCSB generator 的 generation。
- update：只有 `choose_operation()` 返回 update 时才修改 oracle。
- delete：`kind=tombstone`。
- read：不修改 oracle。

Oracle 文件必须包含：

- config fingerprint：`records`、`value_size`、`seed`、`key_prefix`。
- workload phase history。
- 每个 key 的 expected state，或可重放的 compact phase log。

### Verify modes

已新增 YCSB test-only flags：

- `--expect-file PATH`：读取 expected state，用 point_get 校验。
- `--write-expect-file PATH`：运行结束后写出 expected state。
- `--expect-samples N`：采样校验。
- `--expect-all`：全量校验，soak 前后使用。

这些 flags 不改变 public KV API，只扩展 YCSB harness。

### Mixed workload exact verification

YCSB-A/B 的 exact 校验必须使用同一套 `choose_operation()` 和
`operation_key_id()`。不能用“所有 op_index 都按 update 推进 generation”的
近似算法，否则 read op 会错误提高 expected generation。

## 066C Read/write Concurrency Design

### Operation log

每个测试线程记录：

```text
op_id
thread_id
start_seq
end_seq
key
op = get | put | del
generation
acked
read_found
read_generation
read_value_hash
error
```

`start_seq/end_seq` 来自 harness 内单调原子计数器，不需要 wall clock。它只
用于判断 operation interval 是否重叠。

### Per-key checker

对每个 key 独立检查，因为当前 public read API 是 point_get：

1. 收集该 key 所有 ACKed writes/deletes。
2. 对每个 read，找到所有 `write.end_seq < read.start_seq` 的 completed
   writes；其中最大 generation 是 read start 前必须可见的下界。
3. 如果 read 返回 value，它的 generation 不能小于该下界。
4. 如果 read 返回 tombstone/miss，则 read start 前的下界必须是 tombstone，
   或存在与 read interval 重叠的 DELETE 可以作为线性化点。
5. 如果 read 返回 generation `g`，必须存在同 key 的 ACKed PUT `g`，且其
   interval 与 read interval 可线性化。
6. 同一 reader 线程对同 key 的 put-only observations 必须单调不降。

这个 checker 不证明跨 key snapshot，因为 point_get 没有 multi-key 读语义。
batch ACK 后的跨 key 完整性用 sentinel batch barrier 单独检查。

### C1: Hot-key put/read race

目的：覆盖 active memtable 并发读写的线性化。

形态：

- 1 个 writer 对 64 个 hot keys 轮流 PUT generation `1..N`。
- 3 个 readers 随机 point_get hot keys。
- 不触发 flush，先只覆盖 active/imms 读路径。

验收：

- checker 无 regression。
- ACK barrier reads 全部看到 >= ACKed generation。
- no read error。

### C2: Hot-key put/read with automatic seal

目的：覆盖 active -> sealed 切换期间 reader 正确性。

形态：

- 同 C1。
- 把 `maintenance_seal_active_bytes` 调低。
- 允许 seal，但不强制 flush；或只要求 `maintenance.seal > 0`。

验收：

- checker 无 regression。
- `maintenance.seal > 0`。

### C3: Hot-key put/read with automatic flush

目的：覆盖 sealed -> tree flush、CAT/PRS frontier switch 期间 reader 正确性。

形态：

- 同 C1。
- 使用 fast-auto-flush maintenance profile。
- 运行到 `maintenance.non_noop_flush > 0`。

验收：

- checker 无 regression。
- `maintenance.flush > 0`。
- `maintenance.non_noop_flush > 0`。
- no mismatch / no read miss for live keys after ACK barrier。

### C4: Delete/read/put race

目的：覆盖 tombstone 并发不穿透。

形态：

- 对 hot keys 重复 `PUT g -> DELETE -> PUT g+1`。
- readers 持续 point_get。

验收：

- DELETE ACK 后开始的 read 不能返回旧 `g`。
- PUT `g+1` ACK 后开始的 read 不能继续 miss，除非有更新 DELETE。
- checker 无 illegal resurrection。

### C5: Batch ACK completeness

目的：覆盖 batch ACK 后无 partial publish。

形态：

- 每轮 writer 写一个 sentinel batch：`sentinel_0..sentinel_K` 同 generation。
- batch ACK 后立刻逐 key point_get。
- 同时运行 background readers 和 maintenance flush。

验收：

- ACK 后所有 sentinel keys 都是同一 generation。
- 不允许部分 key 仍停在旧 generation。

### C6: Frontier switch barrier

目的：覆盖 flush frontier switch 的版本边界。

形态：

1. force-format runtime 后写 preload generation 1。
2. 等 automatic non-noop flush 完成 preload，并等待之后一个 noop round，建立 clean baseline。
3. 启动 background readers。
4. 写覆盖全部 64 个 hot keys 的 sentinel generation 2。
5. ACK 后立刻逐 key barrier read，证明可见性不依赖 flush。
6. 从 clean baseline 之后等待新的 automatic non-noop flush/frontier switch。
7. flush 后逐 key barrier read。

验收：

- ACK 后 immediate barrier reads 全部 found，并且 exact value generation == 2。
- `checker_barrier.reads == 64`。
- barrier reads 全部 found，并且 exact value generation == 2。
- `checker_frontier_barrier.reads == 64`。
- `checker_frontier_barrier.generation == 2`。
- `checker_frontier_window.reads > 0`，证明 background readers 确实覆盖了
  sentinel flush/frontier switch 等待窗口。
- `checker_maintenance.flush/non_noop_flush > 0`。
- reader log 中 generation 不下降。

### C7: Recovery monotonic continuation

目的：覆盖 recovery 后版本继续单调。

形态：

1. `load + flush` 建立 clean tree。
2. 写 `update` WAL delta，并用 expected-state oracle 记录 ACK 后 winners。
3. no-force boot recovery，按 `--expect-all` 校验 update winner。
4. no-force 用不同 seed 的 `load` 全量覆盖所有 key，并写出新的 oracle。
5. 再次 no-force boot 后按新的 oracle 全量校验。

验收：

- recovery 后新 ACK 的 PUT 能覆盖 recovery 前 winners。
- 再次 boot 后没有 regression。
- 注意：YCSB value 内嵌的 `gen` 来自单次进程的 `op_index + 1`，不是跨进程
  全局版本。C7 验证的是 recovered `next_lsn/data_ver` 不回退，而不是 value
  字符串里的 generation 全局递增。

### C8: Tombstone frontier continuation

目的：覆盖 tombstone frontier carrier 的并发版本。

形态：

1. `load + flush` 建立 clean tree。
2. DELETE 所有 keys，等待 ACK，并用 expected-state oracle 记录 tombstone。
3. no-force boot recovery，用 `--expect-all` 校验所有 reads miss。
4. PUT 所有 keys，并写出新的 expected-state oracle。
5. no-force boot 后用 `--expect-all` 校验所有 reads found。

验收：

- 第 3 步后 reads 全 miss。
- 第 5 步后 reads 全 found，且这些 PUT 的 ACK/data_ver 必须遮蔽已恢复的
  tombstone；YCSB value 内嵌 generation 可能重新从 0 开始。
- 不出现 tombstone 复活旧 value，也不出现 next_lsn 回退导致新 PUT 被遮蔽。
- 当前 C8 是 phase-level recovery continuation；并发 delete/read/put 已由 C4
  覆盖，但尚未把 C4 history 跨进程带到 recovery 后继续检查。

## 066D Fault Injection Design

### Required fault points

Recovery crash consistency 至少要覆盖：

1. WAL entry torn tail：完整 entry 后半部缺失。
2. WAL incomplete batch：`observed < entry_count`。
3. WAL duplicate key in one LSN：应 fail-fast。
4. same-shape replay 写了部分 leaf next slots，未 reset WAL。
5. full CoW 写了新 leaves，未更新 root/superblock。
6. full CoW 更新 inactive superblock，未 reset WAL。
7. WAL reset 写了一部分。
8. tree free range scrub 中断。

### Harness direction

优先选择 fake/block-device harness：

- 用 deterministic block image 表示 superblock/WAL/tree/value refs。
- 在每个 persistence boundary 后 clone image。
- 对每个 cloned image 启动 recovery。
- 验证 logical state 和 fail-fast 分类。

不建议首先做 real NVMe `SIGKILL` crash tests，因为 kill timing 不稳定，
无法保证命中目标边界。real NVMe crash tests 可作为 soak/chaos 层，不作为
精确 fault-point oracle。

## Output Parsing

066A 可以先用 shell runner 解析 stdout。必须解析：

- phase 行：`load`、`load-flush`、`verify`、`run`
- `write_errors`
- `read_errors`
- `read_found`
- `read_miss`
- `acked_entries`
- `maintenance failed`
- `maintenance seal`
- `maintenance flush`
- `maintenance non_noop_flush`
- 066C checker 行：`checker writes/reads/hot_keys`
- 066C batch barrier 行：`checker_barrier reads`
- 066C frontier barrier 行：`checker_frontier_barrier reads/generation`
- 066C frontier window 行：`checker_frontier_window reads`
- 066C maintenance 行：`checker_maintenance failed/seal/flush/non_noop_flush`

任何 error counter 非 0 都是失败。

对 A5/A6，`maintenance.non_noop_flush == 0` 是失败，因为该 profile 的目的
就是强制 automatic flush。

对 default-threshold long run，`non_noop_flush == 0` 不一定失败，除非测试
声明数据量必须触发默认阈值。

## Test Profiles

### fast-auto-flush

用于路径覆盖：

```text
records=10000
operations=10000
value_size=256
maintenance_seal_active_bytes=65536
maintenance_total_memtable_bytes=262144
maintenance_wal_seal_percent=5
maintenance_max_sealed_gens_per_front=1
```

预期几秒内触发大量 non-noop flush。

### default-threshold-smoke

用于确认默认 profile 不回归：

```text
records=10000
operations=10000
value_size=256
maintenance defaults
```

不要求触发 flush。

### default-threshold-soak

用于后续人工批准的长跑：

```text
records >= 1,000,000
operations >= 1,000,000
value_size >= 256
maintenance defaults
timeout >= 1800s
```

这会消耗更多真实设备写入次数，应单独确认后执行。

## Completion Criteria

066A 完成条件：

- A0-A10 脚本化或手动复跑记录齐全。
- 每个场景都有 command、exit code、关键 counters。
- fast-auto-flush 至少一次 `non_noop_flush > 0` 且 restart 后读回成功。
- tombstone frontier no-regression 通过。

066B 完成条件：

- mixed YCSB-A/B exact oracle 可复跑。
- oracle 文件跨 process run 生效。
- 后续扩展 full CoW replay 场景时，能验证多个 key-prefix / key ranges。

066B 当前状态：

- `inconel_ycsb` 已支持 `--expect-file`、`--write-expect-file`、
  `--expect-samples`、`--expect-all`。
- mutating workload 使用 expected-state oracle 时要求 `--inflight 1`；
  并发 mutation 合法性归 066C interval checker。
- `apps/inconel/scripts/ycsb_consistency.sh a10` 已覆盖 mixed YCSB-A/B exact
  oracle + no-force restart 后全量 oracle verify。
- 多 key-prefix / 多 key range 的 full CoW replay exact 验证仍未落地。

066C 完成条件：

- C1-C6 在 real-NVMe in-process checker 跑通；C7-C8 在 real-NVMe scripted
  recovery continuation 跑通。
- C3/C6 在 real NVMe scratch 上至少跑一轮。
- checker 能报告具体 key/op_id/start_seq/end_seq，失败可复现。
- mixed read/write 不再只依赖 aggregate stats。

066C 当前状态：

- `inconel_test_ycsb_concurrency_checker_e2e` 已实现 C1/C2/C3/C4/C5/C6。
- `apps/inconel/scripts/ycsb_consistency.sh c1` 校验 maintenance-off
  put/read race，并要求 checker 通过。
- `apps/inconel/scripts/ycsb_consistency.sh c2` 校验 auto-seal put/read
  race，并要求 checker 通过且 `checker_maintenance.seal > 0`。由于当前
  production maintenance 不拆分 auto seal / auto flush，C2 不断言 flush
  counters 为 0。
- `apps/inconel/scripts/ycsb_consistency.sh c3` 校验 aggressive auto-flush
  put/read race，并要求 checker 通过且 maintenance counters 非零。
- `apps/inconel/scripts/ycsb_consistency.sh c4` 校验 delete/read/put race，
  并要求 tombstone-aware checker 通过。
- `apps/inconel/scripts/ycsb_consistency.sh c5` 校验 batch ACK completeness：
  每个 batch ACK 后逐 key exact point_get，要求
  `checker_barrier.reads=4096`，并同时要求 non-noop flush counters。
- `apps/inconel/scripts/ycsb_consistency.sh c6` 校验 frontier switch barrier：
  sentinel batch ACK 后等待新的 non-noop flush/frontier switch，要求
  ACK 后 `checker_barrier.reads=64`、frontier 后
  `checker_frontier_barrier.reads=64`、`generation=2`、
  `checker_frontier_window.reads>0`，并同时要求 non-noop flush counters。
- `apps/inconel/scripts/ycsb_consistency.sh c7` 校验 existing tree + WAL
  update delta recovery 后继续 PUT，并在第二次 no-force restart 后按
  exact expected-state oracle 校验。
- `apps/inconel/scripts/ycsb_consistency.sh c8` 校验 tombstone WAL delta
  recovery 后继续 PUT，并在第二次 no-force restart 后按 exact expected-state
  oracle 读回所有 key。
- 尚未落地：跨 process interval history checker；066D fault-image oracle 已有
  RAM recovery target 覆盖，尚未扩展到 real-NVMe chaos/soak。

066D 完成条件：

- fault-point image harness 能稳定复现每个 persistence boundary。
- torn WAL / incomplete batch / superblock switch / WAL reset interruption
  都有 positive 或 fail-fast oracle。

066D 当前状态：

- `apps/inconel/test/test_recovery_boot_ram_device.cc` 已扩展为 boot-phase
  deterministic RAM-image fault harness；它不依赖 real NVMe kill timing。
- 已覆盖 WAL torn tail、incomplete batch discard、duplicate key fail-fast、
  same-shape leaf next-slot 已落盘但 WAL 未 reset、full-CoW 新 root 已写但
  superblock 未切换、full-CoW inactive superblock 已切换但 WAL 未 reset、
  WAL reset 只写了前缀、tree free range scrub continuation。
- 这不是 C7/C8 的跨进程 interval checker，也不是 real-NVMe SIGKILL crash
  test；后者只适合作为后续 soak/chaos 层。

## 当前相邻项

本设计暴露出两个应优先做的相邻项：

1. 后续如果要把 C7/C8 提升为真正的 concurrency checker，需要把 C4/C6 的
   operation history 持久化并跨 no-force process run 继续校验。
   现在已经能证明 put/read race 在 active、auto-seal 和 auto-flush 场景下
   的 interval 合法性，也能证明 delete/read/put race 下 tombstone 不穿透；
   batch ACK 后逐 key 可见性、frontier switch barrier、脚本级 recovery
   continuation 都已覆盖。
2. full CoW recovery delta 的 real-NVMe exact 验证还需要多个 key ranges /
   key_prefix 的 oracle 支持；当前 A10 只覆盖单 key range 的 mixed exact
   winner。

另一个相邻项是把 066A 命令固化成脚本，避免再次手动拼命令或误并发抢同一
块 NVMe；当前脚本已包含 per-BDF lock 和 `maintenance.failed=0` 门禁。
