#!/usr/bin/env python3
"""写一份确定性的 preferences.ini（压测用）：自动换行 ON + 滚动到底部 ON + HEX。

用法：python set_prefs.py [--record] [--format HEX|UTF-8]
默认 record=false（阶梯测量不加日志写盘开销）。
"""
import argparse
import os

PREFS_PATH = r"C:/Users/qinjm/AppData/Local/easyDebug/easyDebug/preferences.ini"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", action="store_true", help="开启记录到文件（spot 验收用）")
    ap.add_argument("--format", default="HEX")
    ap.add_argument("--record-path", default="")
    args = ap.parse_args()

    lines = [
        "[traffic]",
        f"format={args.format}",
        "wrap=true",     # 自动换行 ON（用户钦定的最重路径）
        "follow=true",   # 滚动到底部 ON
        "number=true",
        "timestamp=true",
        "direction=true",
        "length=false",
        "hex=false",
        "pure=false",
        f"record={'true' if args.record else 'false'}",
    ]
    if args.record and args.record_path:
        lines.append(f"recordPath={args.record_path}")

    os.makedirs(os.path.dirname(PREFS_PATH), exist_ok=True)
    with open(PREFS_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"WROTE {PREFS_PATH}")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
