# 001 — mock_nvme 模块实现

> 实现第一步。用连续内存模拟 NVMe scheduler，供开发阶段功能验证。

## 文件结构

```
apps/inconel/mock_nvme/
├── device.hh        — 内存模拟设备 + test helpers
├── scheduler.hh     — PUMP scheduler（6 组件模式）
└── sender.hh        — 对外唯一接口，含 batch pipeline
```

## device.hh — mock_device

- `char*` 连续内存，按 LBA 寻址
- `do_write/do_read/do_flush/do_trim` — I/O 操作（scheduler 内部调用）
- `test_read_raw(lba)` — 直接返回内存指针，验证写入内容
- `test_write_raw(lba)` — 直接写入内存，绕过 scheduler
- `test_is_trimmed(lba)` — 检查 TRIM 状态
- per-LBA trim 跟踪（`vector<bool>`）

## scheduler.hh — PUMP scheduler

遵循 PUMP 自建 scheduler 6 组件模式：
1. **req** — 统一请求结构，`op_type` 枚举区分 write/read/flush/trim
2. **op** — `mock_nvme_op` 类型标记
3. **sender** — 统一 sender 类型，4 种操作共享
4. **scheduler** — `per_core::queue<req*>` + `advance()` 同步 memcpy 执行
5. **op_pusher 特化** — `requires mock_nvme_op`
6. **compute_sender_type 特化** — 输出类型 `bool`

单条操作 sender：
```cpp
sched->write(lba, data, num_lbas, flags)  → bool
sched->read(lba, buf, num_lbas)           → bool
sched->flush()                             → bool
sched->trim(lba, num_lbas)                → bool
```

## sender.hh — 对外接口

I/O 描述符：
```cpp
write_desc { lba, data, num_lbas, flags }
read_desc  { lba, buf, num_lbas }
trim_desc  { lba, num_lbas }
```

单条 sender（透传 scheduler 方法）：
```cpp
write_one(s, lba, data, num_lbas, flags)
read_one(s, lba, buf, num_lbas)
trim_one(s, lba, num_lbas)
```

Batch pipeline（`as_stream >> concurrent >> flat_map >> all → bool`）：
```cpp
just() >> write_batch(descs, sched) >> then([](bool ok) { ... })
just() >> read_batch(descs, sched)  >> then([](bool ok) { ... })
just() >> trim_batch(descs, sched)  >> then([](bool ok) { ... })
```

## 验证

- 基础测试：device 创建、write/read/trim 正确性、边界检查 — 通过
- Pipeline 集成测试：单条 write→read→verify、trim、flush 的 PUMP sender 组合 — 通过
- Batch pipeline 测试：3 个非连续页的 write_batch/read_batch/trim_batch — 通过
