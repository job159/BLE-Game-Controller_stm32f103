#!/usr/bin/env python3
"""GATT 全服務探索：列出模組所有服務/特徵，同時訂閱全部 notify，
標記每個位元組來自哪個特徵 —— 用來釐清「資料到底走哪個服務」。

    python ble_explore.py                       # 掃描名稱含 HC-42
    python ble_explore.py --address F5:BA:...
    python ble_explore.py --seconds 12 --probe  # 加寫入探測（送 AT 到可寫特徵）

前置：GUI / 手機必須先中斷連線。
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time
from collections import defaultdict

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("需要 bleak：pip install bleak")


def short(uuid: str) -> str:
    u = uuid.lower()
    if u.endswith("-0000-1000-8000-00805f9b34fb") and u.startswith("0000"):
        return u[4:8]
    return u


async def run(address: str | None, name: str, seconds: float,
              probe: bool) -> None:
    if address is None:
        print(f"掃描（名稱含 '{name}'）...")
        for d in await BleakScanner.discover(timeout=5.0):
            if d.name and name.lower() in d.name.lower():
                address = d.address
                print(f"找到 {d.name} [{d.address}]")
                break
        if address is None:
            sys.exit("找不到裝置（GUI 是否還連著？）")

    rx: dict[str, bytearray] = defaultdict(bytearray)

    async with BleakClient(address) as client:
        print(f"\n== GATT 服務表 ({address}) ==")
        notify_chars = []
        write_chars = []
        for svc in client.services:
            print(f"服務 {short(svc.uuid)}")
            for ch in svc.characteristics:
                props = ",".join(ch.properties)
                print(f"  特徵 {short(ch.uuid)}  [{props}]")
                if ("notify" in ch.properties) or ("indicate" in ch.properties):
                    notify_chars.append(ch)
                if ("write" in ch.properties) or \
                   ("write-without-response" in ch.properties):
                    write_chars.append(ch)

        for ch in notify_chars:
            uuid = short(ch.uuid)

            def cb(_h, data: bytearray, u=uuid) -> None:
                rx[u].extend(data)

            try:
                await client.start_notify(ch, cb)
                print(f"已訂閱 {uuid}")
            except Exception as exc:  # noqa: BLE001
                print(f"訂閱 {uuid} 失敗：{exc}")

        print(f"\n聆聽 {seconds:.0f} 秒...")
        t0 = time.monotonic()
        probed = False
        while (time.monotonic() - t0) < seconds:
            await asyncio.sleep(0.25)
            if probe and (not probed) and ((time.monotonic() - t0) > seconds / 2):
                probed = True
                for ch in write_chars:
                    wnr = "write-without-response" in ch.properties
                    for payload in (b"AT", b"AT\r\n"):
                        try:
                            await client.write_gatt_char(ch, payload,
                                                         response=not wnr)
                            print(f"  已寫入 {payload!r} → {short(ch.uuid)}")
                            await asyncio.sleep(0.3)
                        except Exception as exc:  # noqa: BLE001
                            print(f"  寫入 {short(ch.uuid)} 失敗：{exc}")

        print("\n== 各特徵收到的資料 ==")
        if not rx:
            print("（所有 notify 特徵都沒有資料）")
        for uuid, buf in rx.items():
            n = len(buf)
            printable = sum(1 for b in buf if 32 <= b < 127)
            aa55 = bytes(buf).count(b"\xaa\x55")
            print(f"\n特徵 {uuid}: {n} bytes（ASCII {printable * 100 // max(n,1)}%"
                  f"，AA55 對 {aa55}）")
            if printable * 2 > n:
                print("  文字：", bytes(buf).decode("ascii", "replace")[:300])
            for off in range(0, min(n, 96), 16):
                print(f"  {off:04x}: {bytes(buf[off:off + 16]).hex(' ')}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--address")
    ap.add_argument("--name", default="HC-42")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--probe", action="store_true",
                    help="聆聽半程後對每個可寫特徵送 AT 測試")
    a = ap.parse_args()
    asyncio.run(run(a.address, a.name, a.seconds, a.probe))


if __name__ == "__main__":
    main()
