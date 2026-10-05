#!/usr/bin/env python3
"""浸泡趋势分析：把探针按 N 秒分桶，输出每桶 P99(=MAX) 与累计字节，看趋势拐点。

用法：python soak_trend.py <probe> [--bucket 30] [--drop-ms 2000]
"""
import argparse
import sys


def parse_line(line):
    p = line.split()
    if len(p) < 8:
        return None
    try:
        return dict(epoch=int(p[0]), mx=float(p[5]), rbytes=int(p[7]))
    except (ValueError, IndexError):
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("--bucket", type=float, default=30.0)
    ap.add_argument("--drop-ms", type=float, default=2000.0)
    args = ap.parse_args()

    rows = []
    with open(args.probe, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            d = parse_line(ln)
            if d:
                rows.append(d)
    if not rows:
        print("EMPTY", file=sys.stderr)
        return 1

    cutoff = rows[0]["epoch"] + args.drop_ms
    rows = [r for r in rows if r["epoch"] >= cutoff]

    buckets = []
    cur = []
    cur_start = None
    for r in rows:
        if cur_start is None:
            cur_start = r["epoch"]
        if r["epoch"] - cur_start >= args.bucket * 1000:
            if cur:
                buckets.append((cur_start, cur))
            cur = [r]
            cur_start = r["epoch"]
        else:
            cur.append(r)
    if cur:
        buckets.append((cur_start, cur))

    print(f"bucket_sec={args.bucket}  n_buckets={len(buckets)}")
    for i, (start, items) in enumerate(buckets):
        mx = max(x["mx"] for x in items)
        p50 = sorted(x["mx"] for x in items)[len(items) // 2]
        rbytes = items[-1]["rbytes"]
        print(f"t={i * args.bucket:5.0f}s  P50line={p50:6.1f}  P99max={mx:6.1f}  "
              f"cum_bytes={rbytes}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
