# Nitro

基于 [PUMP](https://github.com/cpporhair/pump) 框架构建的高性能存储引擎集合 — 无锁、Share-Nothing、声明式异步 Pipeline。

## 应用

- **[KV](kv/)** — 快照隔离 KV 存储引擎。五类 Scheduler 协作，MVCC 多版本并发，Leader/Follower 批量合并写入。
- **[AiSAQ](aisaq/)** — NVMe 向量搜索引擎。DiskANN/Vamana 图索引 + 内联 PQ 编码，SPDK 直读 NVMe 搜索十亿级向量，对比基线 11.6×–13.1× 加速。

## 构建

依赖：C++26 编译器、SPDK、CUDA（AiSAQ 需要）。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## License

TBD
