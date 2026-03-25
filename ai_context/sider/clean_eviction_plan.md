# Sider Clean Eviction 方案（entry 级 dirty 追踪）

## 核心思路

旧 promote（单 entry 复制到 partial page）保持不变，加入 entry 级 NVMe 备份追踪。
淘汰时先退回 clean entry 到旧 ON_NVME 页，剩余 dirty entry 才写 NVMe。

## 数据结构变更

### entry 新增字段
```cpp
uint32_t nvme_page_id = UINT32_MAX;  // NVMe 备份所在页 (INVALID = 无备份)
uint8_t  nvme_slot = 0;              // NVMe 备份所在 slot
```

### page_entry 新增字段
```cpp
uint64_t dirty_bitmap = 0;  // 1=dirty(需写NVMe), 0=clean(有旧备份)
```

### hash_table 新增方法
```cpp
entry* lookup_by_page(uint32_t key_hash, uint32_t page_id, uint8_t slot_index);
```
通过 slot_key_hashes 反查 entry，用于 begin_eviction 返回 clean entry。

## 操作语义

### Promote（冷读 entry A 从 ON_NVME page P, slot s）
1. 复制 value 到 partial page Y slot t（现有逻辑）
2. A.page_id=Y, A.slot_index=t
3. A.nvme_page_id=P, A.nvme_slot=s
4. Y.dirty_bitmap bit t = 0 (clean)
5. **不调** nvme_page_dec_live(P) — P 保持活着
6. inline promote（≤16B）不参与：照旧 dec_live，不设 nvme_page_id

### SET（更新 entry A）

#### A 在 IN_MEMORY page Y, slot t:
1. 如果 dirty_bitmap[t]==0 且 nvme_page_id 有效:
   - nvme_page_dec_live(nvme_page_id, nvme_slot)
   - nvme_page_id = INVALID
2. dirty_bitmap[t] = 1
3. 正常 SET 逻辑（原地更新或换页）
4. 如果换页到 Z slot u: Z.dirty_bitmap[u] = 1

#### A 在 ON_NVME page X:
1. 现有逻辑: 分配新存储, nvme_page_dec_live(X, old_slot)
2. **新增**: A.nvme_page_id = INVALID（不额外 dec_live，避免 double dec）

### DEL（删除 entry A）
1. 如果 A 在 IN_MEMORY 且 clean（dirty_bitmap[slot]==0, nvme_page_id 有效）:
   - nvme_page_dec_live(nvme_page_id, nvme_slot)
2. 如果 A 在 ON_NVME:
   - nvme_page_dec_live(page_id, slot)（现有逻辑）
   - nvme_page_id = INVALID
3. 正常删除逻辑

### 新 key SET
1. 分配 slab slot 或 large page
2. entry.nvme_page_id = INVALID
3. dirty_bitmap[slot] = 1

### Begin Eviction（page Y）
```
1. 遍历 Y 上 slot_bitmap=1 且 dirty_bitmap=0 的 slot（clean entry）:
   entry = hash_table.lookup_by_page(slot_key_hashes[slot], Y, slot)
   entry.page_id = entry.nvme_page_id
   entry.slot_index = entry.nvme_slot
   Y.slot_bitmap &= ~(1 << slot)
   Y.live_count--

2. if Y.live_count == 0:
   // 全 clean → 直接释放，不写 NVMe
   slab.evict_page(Y) 或 free 内存
   return DONE

3. // 剩下全 dirty → 正常淘汰写 NVMe
   Y.state = EVICTING
   slab.remove_from_partials(Y)
   正常 NVMe write pipeline...
```

### Complete Eviction（page Y）
不需要遍历 entry。所有剩余 entry 都是 dirty（nvme_page_id=INVALID）。
下次 promote 时自然设置 nvme_page_id。现有逻辑不变。

## 关键不变量

1. **每个 entry 对任一 ON_NVME 页最多持有一个引用**
   - ON_NVME 状态: 引用通过 page_id 体现
   - IN_MEMORY clean: 引用通过 nvme_page_id 体现
   - IN_MEMORY dirty: 无引用（SET 时已释放）

2. **live_count 精确等于引用该页的 entry 数**
   - promote 不减（引用从 page_id 转到 nvme_page_id）
   - clean evict return 不减（引用从 nvme_page_id 转回 page_id）
   - SET/DEL 减（引用被释放）

3. **ON_NVME page 不会在被引用时释放**
   - 只有 live_count=0 才释放
   - clean entry 保持 live_count > 0

## nvme_page_dec_live 变更

需要接受 slot_index 参数，清除 slot_bitmap 对应位（防止 ghost slot，
对 begin_eviction 退回 entry 后再次 promote 时页面 bitmap 正确）：
```cpp
void nvme_page_dec_live(uint32_t page_id, uint8_t slot_index) {
    auto& pe = pt[page_id];
    pe.slot_bitmap &= ~(1ULL << slot_index);
    pe.live_count--;
    if (pe.live_count == 0) {
        for (uint32_t i = 0; i < pe.page_count; i++)
            pending_nvme_frees_.push_back({pe.disk_id, pe.nvme_lba + i});
        pt.free_page_id(page_id);
    }
}
```

## 测试矩阵

### A. 引用计数
| # | 场景 | 验证 |
|---|------|------|
| A1 | promote A 从 X | X.live_count 不变 |
| A2 | SET clean A (nvme_page_id=X) | X.live_count -1 |
| A3 | DEL clean A (nvme_page_id=X) | X.live_count -1 |
| A4 | SET ON_NVME A (page_id=X, nvme_page_id=X) | X.live_count 只减一次 |
| A5 | X 所有引用释放 | live_count=0, page_id 回收, LBA 进 pending_frees |

### B. dirty_bitmap
| # | 场景 | 验证 |
|---|------|------|
| B1 | promote 到 Y slot t | dirty_bitmap[t]=0 |
| B2 | SET 更新 Y 上 entry | dirty_bitmap[t]=1 |
| B3 | 新 key SET 到 Y slot t | dirty_bitmap[t]=1 |
| B4 | promote 多个到同页 | 各 bit 独立 |

### C. clean eviction
| # | 场景 | 验证 |
|---|------|------|
| C1 | Y 全 clean → begin_eviction | entry 退回旧页, Y 释放, 返回 DONE |
| C2 | Y 混合 → begin_eviction | clean 退回, dirty 留下, 继续写 |
| C3 | Y 全 dirty → begin_eviction | 无退回, 正常写 |
| C4 | clean evict 后 get() | 返回 cold_result, lba/slot 正确 |

### D. bounce 循环
| # | 场景 | 验证 |
|---|------|------|
| D1 | promote→clean evict ×5 | live_count 稳定, nvme_page_id 不变 |
| D2 | promote→clean evict→SET | old page live_count -1 |
| D3 | 同页两 entry, 一 SET 一不 SET | 各自 live_count 正确 |

### E. 边界
| # | 场景 | 验证 |
|---|------|------|
| E1 | promote 后换 size class SET | dec_live 旧页 + 新 slot dirty=1 |
| E2 | inline promote | 照旧 dec_live, 不追踪 |
| E3 | expire clean entry | 旧备份释放 |
| E4 | lookup_by_page 反查 | 正确找到 entry |

### F. 集成 (NVMe stats)
| # | 场景 | 验证 |
|---|------|------|
| F1 | SET fill → GET R1 → GET R2 | R2 NVMe write ≈ 0 |
| F2 | 混合 GET+SET | writes < 无优化版本 |
