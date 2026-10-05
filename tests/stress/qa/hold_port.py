#!/usr/bin/env python3
"""占住一个 TCP 端口不放（用于 QA 验收"打开失败弹窗"）。

用法：python hold_port.py <port> [hold_seconds]
默认 hold 60 秒；进程被 taskkill 强杀时端口随之释放。
"""
import socket
import sys
import time


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: hold_port.py <port> [hold_seconds]", file=sys.stderr)
        return 2
    port = int(sys.argv[1])
    hold = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print(f"HOLDING {port}", flush=True)
    try:
        time.sleep(hold)
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
