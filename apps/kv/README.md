# KV — 快照隔离 KV 存储引擎

基于 PUMP 框架的全异步 KV 引擎。五类 Scheduler 协作，MVCC 快照隔离，Leader/Follower 批量合并，全链路无锁。

## 架构

```
                        ┌─────────────────────────────────────────┐
                        │              batch scheduler            │
                        │     快照分配 · 版本号递增 · 发布/GC       │
                        └────────────────┬────────────────────────┘
                                         │
              ┌──────────────────────────┼──────────────────────────┐
              ▼                          ▼                          ▼
   ┌──────────────────┐      ┌──────────────────┐      ┌──────────────────┐
   │ index scheduler 0│      │ index scheduler 1│      │ index scheduler 2│
   │   B-tree 分片 0   │      │   B-tree 分片 1   │      │   B-tree 分片 2   │
   └────────┬─────────┘      └────────┬─────────┘      └────────┬─────────┘
            │      hash(key) % N 路由   │                        │
            └──────────────────────────┼────────────────────────┘
                                       │
                        ┌──────────────▼──────────────┐
                        │         fs scheduler         │
                        │  页面分配 · Leader/Follower   │
                        └──────────────┬──────────────┘
                                       │
         ┌───────────┬───────────┬─────┴─────┬───────────┬───────────┐
         ▼           ▼           ▼           ▼           ▼           ▼
   ┌──────────┐┌──────────┐┌──────────┐┌──────────┐┌──────────┐┌──────────┐
   │ nvme  0  ││ nvme  1  ││ nvme  2  ││ nvme  3  ││ nvme  4  ││ nvme  5  │
   │ SPDK DMA ││ SPDK DMA ││ SPDK DMA ││ SPDK DMA ││ SPDK DMA ││ SPDK DMA │
   └──────────┘└──────────┘└──────────┘└──────────┘└──────────┘└──────────┘
```

### 五类 Scheduler

| Scheduler | 实例 | 职责 |
|-----------|------|------|
| **batch** | 1 | 快照分配与发布、版本号递增、旧快照 GC |
| **index** | N | per-core B-tree 索引、版本链查询、数据页缓存与 waiter 协调 |
| **fs** | 1 | NVMe 页面分配/释放、Leader/Follower 合并写 |
| **nvme** | M | SPDK DMA 异步读写、per-core qpair |
| **task** | * | 通用异步任务切换（框架内置） |

## 写入路径

```
put(key, value)                    ── 暂存到 batch 缓存
apply()                            ── 提交事务：
  │
  ├─ batch::allocate_put_id()      ── 分配版本号（snapshot serial++）
  │
  ├─ for_each(cache)               ── 并发更新索引
  │  >> concurrent()
  │  >> index::update()            ── hash(key) 路由到对应 B-tree 分片
  │
  ├─ fs::allocate_data_page()      ── Leader/Follower 合并
  │  ├─ Leader：聚合多个 batch 的页面需求，一次分配
  │  └─ Follower：挂起等待 Leader 结果
  │
  ├─ [Leader]
  │  ├─ for_each(spans)
  │  │  >> concurrent()
  │  │  >> nvme::put_span()        ── SPDK DMA 写入
  │  └─ notify_follower()          ── 唤醒所有 Follower
  │
  └─ index::cache()                ── 将写入的页面加入内存缓存

finish_batch()                     ── 发布快照，新版本对读者可见
```

**Leader/Follower 合并**：多个并发 `apply()` 到达 fs scheduler 时，第一个成为 Leader 聚合后续请求（最多 4096 页），合并为一次 NVMe 写入。Follower 零 IO 等待。

## 读取路径

```
get(key)
  │
  ├─ index::get(key, read_snapshot_sn)
  │  └─ B-tree 查找 → 版本链中取 sn ≤ read_snapshot 的最新版本
  │
  ├─ [命中缓存] → 直接返回
  │
  ├─ [未缓存，首次读] → 成为 reader-leader
  │  ├─ nvme::get_page()           ── SPDK DMA 读取
  │  └─ notify_waiters()           ── 唤醒等待同一 key 的读者
  │
  └─ [未缓存，已有读者] → 挂起为 waiter，等通知
```

**Page Waiter 模式**：并发读同一 key 时，首个读者执行 NVMe IO，后续读者挂 waiter 队列零开销等待。

## MVCC

```
时间 ──────────────────────────────────────────────►

  sn=1        sn=2        sn=3
  ┌───┐       ┌───┐       ┌───┐
  │ S1│──────▶│ S2│──────▶│ S3│   current_readable
  └───┘       └───┘       └───┘
    │                       ▲
    │ ref > 0               │ publish()
    │ (读者仍持有)            │
    └── 暂不回收              └── 新读者从此版本开始
```

- 每个 batch 创建时获取 `read_snapshot`（pin 当前版本）
- 写入分配新 `put_snapshot`（版本号递增）
- `finish_batch()` 发布 → `current_readable` 原子切换
- 旧快照引用计数归零后 GC 回收

## 配置

```json
{
  "batch": { "core": [0] },
  "index": { "core": [1, 2, 3] },
  "fs":    { "core": [0] },
  "io": {
    "nvme": [{
      "name": "nvme0n1",
      "core": [4, 5, 6, 7, 8, 9],
      "qpair_count": 4,
      "qpair_depth": 256,
      "fua": false
    }]
  }
}
```

| 参数 | 说明 |
|------|------|
| `core` | Scheduler 绑定的 CPU 核心列表 |
| `qpair_count` | 每块 SSD 的提交队列数 |
| `qpair_depth` | 每个队列的深度 |
| `fua` | Force Unit Access（绕过 SSD 缓存，保证持久化） |

## YCSB 基准测试

```bash
./kv config.json
```

内置三阶段 YCSB 工作负载：

| 阶段 | 操作 | 并发度 |
|------|------|--------|
| load | 顺序插入 N 条 KV | 10000 |
| update | 随机更新已有 key | 10000 |
| read | 随机读取已有 key | 10000 |

每阶段结束输出吞吐量与延迟统计。

## 目录结构

```
kv/
├── main.cc          入口：start_db → YCSB load/update/read → stop_db
├── senders/         公开 API：put, get, scan, apply, finish_batch, start/stop_db
├── batch/           快照管理：分配、发布、GC、snapshot_manager
├── index/           B-tree 索引：per-core 分片、版本链、缓存、waiter 协调
├── fs/              页面分配：Leader/Follower 合并、元数据管理
├── nvme/            NVMe 封装：SPDK DMA 读写 sender
├── data/            数据结构：key_value, data_page, data_file, batch, snapshot, slice, b_tree
├── runtime/         运行时：配置解析、Scheduler 对象管理、常量定义
└── ycsb/            基准测试：load, update, read 工作负载
```
