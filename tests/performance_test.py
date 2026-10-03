#!/usr/bin/env python3
"""
自动换行性能测试工具
模拟高频消息发送（50ms/包），测试界面是否流畅
"""

import sys
import time
import socket
import random
from datetime import datetime

def generate_test_data(size_kb=1):
    """生成测试数据包"""
    size = size_kb * 1024
    # 生成可读的测试数据（包含换行符，触发自动换行）
    data = []
    for i in range(0, size, 64):
        line = f"Line{i:04d}: " + "".join(random.choices("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", k=50)) + "\n"
        data.append(line.encode('utf-8'))
    return b"".join(data)[:size]

def test_high_frequency_send(host='127.0.0.1', port=5000, interval_ms=50, duration_sec=10):
    """
    高频发送测试

    Args:
        host: 目标主机
        port: 目标端口
        interval_ms: 发送间隔（毫秒）
        duration_sec: 测试时长（秒）
    """
    print(f"[测试配置]")
    print(f"  目标: {host}:{port}")
    print(f"  发送间隔: {interval_ms}ms")
    print(f"  测试时长: {duration_sec}s")
    print(f"  预计消息数: {int(duration_sec * 1000 / interval_ms)} 条")
    print(f"\n请确保 easyDebug 已启动并监听 TCP Server 端口 {port}，开启自动换行功能")
    input("准备好后按 Enter 开始测试...")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print(f"\n[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] 已连接")

        start_time = time.time()
        count = 0
        interval_sec = interval_ms / 1000.0

        while time.time() - start_time < duration_sec:
            # 生成 1KB 测试数据
            data = generate_test_data(1)
            sock.sendall(data)
            count += 1

            # 每秒输出一次进度
            if count % (1000 // interval_ms) == 0:
                elapsed = time.time() - start_time
                print(f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] 已发送 {count} 条 ({elapsed:.1f}s)")

            # 控制发送频率
            time.sleep(interval_sec)

        elapsed = time.time() - start_time
        print(f"\n[测试完成]")
        print(f"  实际时长: {elapsed:.2f}s")
        print(f"  发送消息数: {count}")
        print(f"  平均速率: {count / elapsed:.1f} 条/秒")
        print(f"\n请观察 easyDebug 界面是否流畅，有无卡顿")

        sock.close()

    except ConnectionRefusedError:
        print(f"\n[错误] 无法连接到 {host}:{port}")
        print("请确保 easyDebug 已启动并配置 TCP Server 监听该端口")
        return False
    except KeyboardInterrupt:
        print(f"\n[中断] 用户取消测试")
        return False
    except Exception as e:
        print(f"\n[错误] {e}")
        return False

    return True

def test_extreme_frequency(host='127.0.0.1', port=5000):
    """极限测试：20ms/包"""
    print("\n=== 极限频率测试 ===")
    return test_high_frequency_send(host, port, interval_ms=20, duration_sec=5)

def main():
    print("=== easyDebug 自动换行性能测试 ===\n")

    # 场景1：50ms/包
    print("=== 场景1：正常高频测试 (50ms/包) ===")
    success = test_high_frequency_send(interval_ms=50, duration_sec=10)

    if success:
        input("\n按 Enter 继续极限测试...")
        # 场景2：20ms/包
        test_extreme_frequency()

    print("\n测试结束！")

if __name__ == "__main__":
    main()
