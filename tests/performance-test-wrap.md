# 已合并到 PERFORMANCE_TEST.md

本文描述的方案（`ResizeToContents` → 固定估算行高 + 200 ms 防抖
`resizeRowsToContents()`）**已被取代**，相关代码（`m_resizeDebounce`、
`resizeRowsToContents()`）已从 `trafficpanel.cpp` 删除。

它记录的两条经验仍然成立，也是后来方案的起点：

- `ResizeToContents` / 逐行 `sizeHint()` 会测量**屏幕外**的每一行，代价是
  "行数 × 整段文本布局"，本机实测约 0.9 ms/行；
- 200 ms 防抖只降低触发频率，不改变单次代价：数据一停顿就是一次整秒冻结，
  持续高速时又一直被推迟——两头都不对。

当前实现（统一行高、按列宽裁剪单元格文本、预算溢出走增量通知）与实测数据见
[docs/performance-optimization-wrap-mode.md](../docs/performance-optimization-wrap-mode.md)；
可复现的测试步骤见 [PERFORMANCE_TEST.md](PERFORMANCE_TEST.md)。
