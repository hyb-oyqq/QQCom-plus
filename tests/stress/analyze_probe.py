#!/usr/bin/env python3
"""解析 latency-probe 输出，计算稳态延迟与相邻采样窗口的卡顿判定。

探针每 500ms 写一行 8 字段：
  <epoch_ms> <样本数=25> <P50> <P95> <P99> <MAX> <总条数> <总字节>
注意：25 个样本时 P99 恰好等于 MAX（ceil(0.99*25)-1 = 24 = 最大下标），
所以"单秒窗口 P99"= 该 1 秒内（两行 50 样本）的最大单拍迟到量 = max(两行 MAX)。

用法：
  python analyze_probe.py <probe_file> [--drop-ms 1000] [--threshold 50.0]
输出稳态各分位、卡顿判定、对账用最终总字节。
"""
import argparse
import sys
from statistics import median


def parse_line(line):
    parts = line.split()
    if len(parts) < 8:
        return None
    try:
        epoch = int(parts[0])
        n = int(parts[1])
        p50 = float(parts[2])
        p95 = float(parts[3])
        p99 = float(parts[4])
        mx = float(parts[5])
        records = int(parts[6])
        rbytes = int(parts[7])
    except (ValueError, IndexError):
        return None
    return dict(epoch=epoch, n=n, p50=p50, p95=p95, p99=p99, mx=mx,
                records=records, rbytes=rbytes)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("--drop-ms", type=float, default=1000.0,
                    help="启动后剔除的毫秒数（首秒尖峰不算）")
    ap.add_argument("--threshold", type=float, default=50.0,
                    help="单秒窗口 P99 卡顿阈值（ms）")
    args = ap.parse_args()

    lines = []
    with open(args.probe, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            d = parse_line(ln)
            if d:
                lines.append(d)

    if not lines:
        print("EMPTY_OR_UNPARSEABLE", file=sys.stderr)
        return 1

    first_epoch = lines[0]["epoch"]
    cutoff = first_epoch + args.drop_ms
    steady = [d for d in lines if d["epoch"] >= cutoff]

    if not steady:
        print(f"NO_STEADY_DATA (all {len(lines)} lines in first {args.drop_ms}ms)",
              file=sys.stderr)
        return 1

    p50s = [d["p50"] for d in steady]
    p95s = [d["p95"] for d in steady]
    p99s = [d["p99"] for d in steady]
    mxs = [d["mx"] for d in steady]

    # 单秒窗口（两行一组）的 P99 = max(两行 MAX)
    sec_p99s = []
    for i in range(0, len(steady) - 1, 1):
        sec_p99s.append(max(steady[i]["mx"], steady[i + 1]["mx"]))
    max_sec_p99 = max(sec_p99s) if sec_p99s else max(mxs)

    lag = max_sec_p99 > args.threshold

    print(f"lines_total={len(lines)}  lines_steady={len(steady)}  "
          f"span_ms={steady[-1]['epoch'] - steady[0]['epoch']}")
    print(f"steady_P50_median={median(p50s):.1f}  P50_max={max(p50s):.1f}")
    print(f"steady_P95_median={median(p95s):.1f}  P95_max={max(p95s):.1f}")
    print(f"steady_P99_max={max(p99s):.1f}")
    print(f"steady_MAX_overall={max(mxs):.1f}")
    print(f"sec_window_P99_max={max_sec_p99:.1f}")
    print(f"JANK={lag}  (threshold {args.threshold} ms)")
    print(f"final_records={steady[-1]['records']}  final_rxbytes={steady[-1]['rbytes']}")
    # 每行 P99(=MAX) 的时间序列，供趋势/方差查看
    series = ",".join(f"{d['mx']:.1f}" for d in steady)
    print(f"MAX_series_ms=[{series}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
