#!/usr/bin/env python3
"""easyDebug 发压脚本（仅标准库）。

按固定或线性爬升速率向 easyDebug 端点发送可复现报文，结束时打印
总发送条数 / 字节数 / 实际速率，用于与 easyDebug 的 RX 计数对账判断丢数。

每条报文布局：<8 字节递增序号><8 字节毫秒时间戳><填充>（共 --size 字节）。
序号从 0 单调递增，接收端据此数出"丢了几条、丢在哪"；时间戳保留到达时刻，
方便与 easyDebug 侧的接收时间比对。填充字节由序号派生，同一条报文每次重放一致。

用法示例：
  python traffic_stress.py --proto tcp --host 127.0.0.1 --port 5000 \
      --rate 500 --size 256 --duration 30
  python traffic_stress.py --proto udp --host 127.0.0.1 --port 5000 \
      --rate 2000 --size 1024 --duration 60 --ramp
"""

import argparse
import math
import socket
import struct
import sys
import time


def build_packet(seq: int, size: int) -> bytes:
    """构造一条可复现报文：8 字节序号 + 8 字节毫秒时间戳 + 填充。"""
    header = struct.pack(">Qq", seq & 0xFFFFFFFFFFFFFFFF, int(time.time() * 1000))
    payload = header[:size]
    if size > len(header):
        # 填充由序号派生，保证同序号同内容；取低字节循环，便于对账时肉眼识别。
        pattern = bytes((seq + i) % 256 for i in range(256))
        payload = (header + (pattern * ((size - len(header)) // len(pattern) + 1)))[:size]
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description="easyDebug 发压脚本（TCP/UDP）")
    parser.add_argument("--proto", choices=("tcp", "udp"), required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--rate", type=float, required=True, help="目标速率（条/秒）")
    parser.add_argument("--size", type=int, required=True, help="每条字节数（>=16）")
    parser.add_argument("--duration", type=float, required=True, help="持续秒数")
    parser.add_argument("--ramp", action="store_true",
                        help="速率从 0 线性升到 --rate（而非恒定速率）")
    args = parser.parse_args()

    if args.rate <= 0:
        print("--rate 必须 > 0", file=sys.stderr)
        return 2
    if args.size < 16:
        print("--size 至少 16（否则放不下序号+时间戳头）", file=sys.stderr)
        return 2
    if args.duration <= 0:
        print("--duration 必须 > 0", file=sys.stderr)
        return 2

    sock = None
    if args.proto == "tcp":
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        try:
            sock.connect((args.host, args.port))
        except OSError as exc:
            print(f"TCP 连接失败 {args.host}:{args.port}：{exc}", file=sys.stderr)
            return 1
    else:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    start = time.monotonic()
    sent = 0
    bytes_sent = 0
    errors = 0
    send = (lambda pkt: sock.sendall(pkt) or len(pkt)) if args.proto == "tcp" \
        else (lambda pkt: sock.sendto(pkt, (args.host, args.port)) or len(pkt))

    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if elapsed >= args.duration:
                break
            # 允许已发送量 = 速率对时间的积分；--ramp 时速率 0→rate 线性上升，
            # 积分是 rate*t²/(2*duration)，实现平滑爬坡而不是阶跃。
            if args.ramp:
                allowed = args.rate * elapsed * elapsed / (2.0 * args.duration)
            else:
                allowed = args.rate * elapsed
            # 追平允许量：高速率下每拍多发几条，避免逐条 sleep 的时间粒度吃掉吞吐。
            while sent < allowed:
                try:
                    packet = build_packet(sent, args.size)
                    send(packet)
                    bytes_sent += len(packet)
                    sent += 1
                except OSError as exc:
                    errors += 1
                    print(f"发送失败（第 {sent} 条）：{exc}", file=sys.stderr)
                    # 连接类错误继续发没有意义，直接结束统计。
                    if args.proto == "tcp":
                        raise
                    break
            # 等到下一个"允许量"到来的时刻：先粗睡，最后 1ms 自旋，把速率压准。
            if args.ramp:
                # 允许量 sent+1 对应的时刻：rate*t²/(2*duration) = sent+1 反解。
                next_due = start + math.sqrt(2.0 * (sent + 1) * args.duration / args.rate)
            else:
                next_due = start + (sent + 1) / args.rate
            remaining = next_due - time.monotonic()
            if remaining > 0.002:
                time.sleep(remaining - 0.001)
            while time.monotonic() < next_due:
                pass
    except KeyboardInterrupt:
        print("被中断", file=sys.stderr)
    finally:
        if sock is not None:
            sock.close()

    elapsed = time.monotonic() - start
    actual_rate = sent / elapsed if elapsed > 0 else 0.0
    print(f"sent_packets={sent} sent_bytes={bytes_sent} errors={errors}")
    print(f"actual_rate={actual_rate:.1f} pkt/s  elapsed={elapsed:.2f}s  "
          f"avg_packet={bytes_sent / sent if sent else 0:.1f}B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
