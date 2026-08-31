#!/usr/bin/env python3
"""BLE 原始資料診斷擷取：連上模組抓 N 秒，自動分析內容型態。

    python diag_capture.py                      # 掃描名稱含 HC-42 的裝置
    python diag_capture.py --address F5:BA:...  # 指定位址
    python diag_capture.py --seconds 15

前置：GUI / 手機必須先中斷連線（BLE 一次只允許一個 central）。

輸出判讀：
  合法封包 > 0                → 鏈路正常，問題在應用層
  ASCII 比例高（看得懂文字）  → 收到的是 AT 回應 / 日誌文字
  隨機 hex、AA55 幾乎為 0     → 鮑率錯配的碎渣
  0 bytes                     → STM32→模組 段完全沒資料
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import Counter

from protocol import Commander, FrameParser
from transports import BleTransport, TransportError


def main() -> None:
    ap = argparse.ArgumentParser(description="BLE raw capture diagnostics")
    ap.add_argument("--address")
    ap.add_argument("--name", default="HC-42")
    ap.add_argument("--seconds", type=float, default=10.0)
    args = ap.parse_args()

    try:
        t = BleTransport(name=None if args.address else args.name,
                         address=args.address)
    except TransportError as exc:
        sys.exit(f"連線失敗（GUI/手機是否還連著模組？）：{exc}")

    print(f"已連線 {t.name}，擷取 {args.seconds:.0f} 秒...")
    buf = bytearray()
    t0 = time.monotonic()
    cmder = Commander()
    ping_at = -1
    while (time.monotonic() - t0) < args.seconds:
        d = t.read()
        if d:
            buf += d
        if (ping_at < 0) and ((time.monotonic() - t0) > args.seconds / 2):
            t.write(cmder.ping())          # 半程時發 PING 測雙向
            ping_at = len(buf)
            print(f"  [已送 PING @ {ping_at} bytes 處]")
    t.close()

    n = len(buf)
    print(f"\n== 統計 ==")
    print(f"共 {n} bytes（平均 {n / args.seconds:.1f} B/s）")
    if n == 0:
        print("完全沒有資料：模組→PC 這段是通的（連得上），")
        print("但 STM32→模組 沒有任何位元組。查 PA2(STM32 TX)→模組 RXD 接線。")
        return

    printable = sum(1 for b in buf if 32 <= b < 127)
    pct = printable * 100 // n
    cnt = Counter(buf)
    top = ", ".join(f"{b:02x}x{c}" for b, c in cnt.most_common(8))
    aa55 = bytes(buf).count(b"\xaa\x55")
    print(f"可列印 ASCII 比例 {pct}%")
    print(f"位元組種類 {len(cnt)}，最常見: {top}")
    print(f"0xAA 出現 {cnt.get(0xAA, 0)} 次；'AA 55' 連續對 {aa55} 次")

    p = FrameParser()
    frames = list(p.feed(bytes(buf)))
    print(f"合法封包 {len(frames)}（CRC錯 {p.crc_errors} / resync {p.resyncs}）")
    for f in frames[:5]:
        print(f"  frame type=0x{f.msg_type:02X} seq={f.seq} "
              f"payload={f.payload.hex()}")

    if pct >= 50:
        print("\n== 文字內容（ASCII 佔比高，疑似 AT 回應/日誌） ==")
        print(bytes(buf).decode("ascii", "replace")[:600])

    dump_n = min(n, 256)
    print(f"\n== 前 {dump_n} bytes hex ==")
    for off in range(0, dump_n, 16):
        print(f"{off:04x}: {bytes(buf[off:off + 16]).hex(' ')}")

    with open("diag_capture.bin", "wb") as fh:
        fh.write(buf)
    print(f"\n完整擷取已存 diag_capture.bin（{n} bytes）")


if __name__ == "__main__":
    main()
