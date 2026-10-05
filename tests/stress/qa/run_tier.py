#!/usr/bin/env python3
"""一键跑一档压测：起 easyDebug → 发压 → 收尾 → 分析探针。

用法：
  python run_tier.py --proto tcp --port 9200 --rate 1000 --size 256 \
      --duration 30 --probe out/probe.txt [--ramp] [--settle 2.0]

流程：
  1. 启动 easyDebug.exe（真实窗口）--auto-open <proto>:<port> --latency-probe <probe>
  2. 等 app 起好（TCP 用连接探活，UDP 固定等 2.5s）
  3. 调用 traffic_stress.py 发压，透传其 stdout
  4. 等 settle 秒让 RX 字节追平
  5. taskkill /F /PID 杀掉 app
  6. 调用 analyze_probe.py 分析探针，透传其 stdout

标准库 only。
"""
import argparse
import os
import socket
import subprocess
import sys
import time

EASYDEBUG_EXE = r"D:/AWork/myself/QTProject/easyDebug/easyDebug/build/Desktop_Qt_6_10_1_MSVC2022_64bit-Debug/debug/easyDebug.exe"
STRESS_PY = r"D:/AWork/myself/QTProject/easyDebug/easyDebug/tests/stress/traffic_stress.py"
ANALYZE_PY = r"D:/AWork/myself/QTProject/easyDebug/easyDebug/tests/stress/qa/analyze_probe.py"
PY = r"C:/Users/qinjm/.workbuddy/binaries/python/envs/default/Scripts/python.exe"
PREFS_PATH = r"C:/Users/qinjm/AppData/Local/easyDebug/easyDebug/preferences.ini"


def write_prefs(record: bool):
    """起 app 前写确定性配置：自动换行 ON + 滚动到底部 ON + HEX。"""
    import os
    lines = [
        "[traffic]",
        "format=HEX",
        "wrap=true",      # 自动换行 ON（用户钦定的最重路径）
        "follow=true",    # 滚动到底部 ON
        "number=true",
        "timestamp=true",
        "direction=true",
        "length=false",
        "hex=false",
        "pure=false",
        f"record={'true' if record else 'false'}",
    ]
    if record:
        lines.append(r"recordPath=D:/AWork/myself/QTProject/easyDebug/easyDebug/tests/stress/qa/qa_log.txt")
    os.makedirs(os.path.dirname(PREFS_PATH), exist_ok=True)
    with open(PREFS_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def wait_tcp_ready(host, port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def count_running_easydebug() -> int:
    """返回当前在跑的 easyDebug.exe 实例数（用于并发守卫）。"""
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq easyDebug.exe"],
                             capture_output=True, text=True, errors="replace").stdout
        return out.count("easyDebug.exe")
    except OSError:
        return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", choices=("tcp", "udp"), required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--rate", type=float, required=True)
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--duration", type=float, required=True)
    ap.add_argument("--probe", required=True)
    ap.add_argument("--ramp", action="store_true")
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    # 并发守卫：机器上已有别的 easyDebug 实例（例如另一位队友在压测）时直接中止，
    # 避免 CPU 争抢把双方的 P99 都污染掉。
    others = count_running_easydebug()
    if others > 0:
        print(f"[run_tier] ABORT: 已有 {others} 个 easyDebug.exe 在跑，疑似并发压测，跳过本档",
              file=sys.stderr, flush=True)
        return 2

    probe = os.path.abspath(args.probe)
    os.makedirs(os.path.dirname(probe), exist_ok=True)
    if os.path.exists(probe):
        os.remove(probe)

    write_prefs(record=False)  # 阶梯测量：自动换行 ON + 跟随 ON，不记录

    scheme = "tcp-server" if args.proto == "tcp" else "udp"
    spec = f"{scheme}:{args.port}"

    app = subprocess.Popen(
        [EASYDEBUG_EXE, "--auto-open", spec, "--latency-probe", probe],
        cwd=os.path.dirname(EASYDEBUG_EXE),
    )
    print(f"[run_tier] app pid={app.pid} spec={spec}", flush=True)

    if args.proto == "tcp":
        ready = wait_tcp_ready(args.host, args.port)
        print(f"[run_tier] tcp_ready={ready}", flush=True)
        if not ready:
            print("[run_tier] ERROR: app 未在超时内监听", file=sys.stderr, flush=True)
    else:
        time.sleep(2.5)

    stress_cmd = [PY, STRESS_PY, "--proto", args.proto, "--host", args.host,
                  "--port", str(args.port), "--rate", str(args.rate),
                  "--size", str(args.size), "--duration", str(args.duration)]
    if args.ramp:
        stress_cmd.append("--ramp")
    print(f"[run_tier] stress: {' '.join(stress_cmd)}", flush=True)
    stress = subprocess.run(stress_cmd, capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    print(stress.stdout, end="", flush=True)
    if stress.stderr.strip():
        print(stress.stderr, end="", file=sys.stderr, flush=True)
    print(f"[run_tier] stress_exit={stress.returncode}", flush=True)

    time.sleep(args.settle)

    subprocess.run(["taskkill", "/F", "/PID", str(app.pid)],
                   capture_output=True, text=True, errors="replace")
    time.sleep(0.5)

    if not os.path.exists(probe):
        print("[run_tier] ERROR: 探针文件未生成", file=sys.stderr, flush=True)
        return 1

    print("[run_tier] analyze:", flush=True)
    ana = subprocess.run([PY, ANALYZE_PY, probe], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    print(ana.stdout, end="", flush=True)
    if ana.stderr.strip():
        print(ana.stderr, end="", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
