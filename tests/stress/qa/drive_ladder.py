#!/usr/bin/env python3
"""驱动整条主阶梯（TCP 256B，多档 × 2 次），把每档结果追加到摘要文件。

用法：python drive_ladder.py --tag ladder_256 --rates 100,500,1000,2000,5000,10000,20000 \
          --size 256 --duration 30 --runs 2 --port0 9300
"""
import argparse
import subprocess
import sys
import time

RUN_TIER = r"D:/AWork/myself/QTProject/easyDebug/easyDebug/tests/stress/qa/run_tier.py"
PY = r"C:/Users/qinjm/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
QA = r"D:/AWork/myself/QTProject/easyDebug/easyDebug/tests/stress/qa"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--rates", required=True)
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--port0", type=int, default=9300)
    ap.add_argument("--proto", default="tcp")
    args = ap.parse_args()

    rates = [float(x) for x in args.rates.split(",")]
    summary = f"{QA}/{args.tag}_summary.txt"
    with open(summary, "w", encoding="utf-8") as s:
        s.write(f"# {args.tag} proto={args.proto} size={args.size} "
                f"duration={args.duration} runs={args.runs}\n")

    port = args.port0
    for rate in rates:
        for run in range(1, args.runs + 1):
            probe = f"{QA}/{args.tag}_rate{int(rate)}_r{run}.txt"
            cmd = [PY, RUN_TIER, "--proto", args.proto, "--port", str(port),
                   "--rate", str(rate), "--size", str(args.size),
                   "--duration", str(args.duration), "--probe", probe]
            print(f"\n===== {args.tag} rate={rate} run={run} port={port} =====",
                  flush=True)
            r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                               errors="replace")
            print(r.stdout, end="", flush=True)
            if r.stderr.strip():
                print(r.stderr, end="", file=sys.stderr, flush=True)
            with open(summary, "a", encoding="utf-8") as s:
                s.write(f"\n## {args.tag} rate={rate} run={run} port={port}\n")
                s.write(r.stdout)
            if r.returncode == 2:  # 并发守卫触发：停止整个阶梯，等待机器空闲
                print(f"\n[drive_ladder] ABORT at {args.tag} rate={rate} run={run} (并发守卫)",
                      flush=True)
                return 2
            port += 1
            time.sleep(1.0)  # 端口/进程收尾
    print(f"\nDONE {args.tag} -> {summary}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
