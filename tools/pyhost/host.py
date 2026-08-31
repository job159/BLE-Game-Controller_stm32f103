#!/usr/bin/env python3
"""IMU-BLE Node 主機端工具（CLI 版）：即時儀表板 / 命令下發 / CSV 記錄。

圖形介面版見 host_gui.py（pip install PySide6 後執行）。

兩種傳輸（同一套協定，實作在 transports.py）：
  1. Serial：USB-TTL 直連 STM32 USART2（開發期免 BLE 快速驗證）
       python host.py --port COM5
  2. BLE：nRF52832 透傳模組（需 pip install bleak）
       python host.py --ble --name <裝置名>

BLE UART 服務會自動偵測（NUS / FFE0 / FFF0 / 泛用）；失敗時：
  python host.py --scan                                     ← 看模組廣播內容
  python host.py --ble --uuid-notify FFE1 --uuid-write FFE1 ← 手動指定

互動命令（執行中直接輸入）：
  rate <hz> | cal | reset | ping | info | quit
"""
from __future__ import annotations

import argparse
import csv
import queue
import sys
import threading
import time
from typing import Optional

from protocol import (Ack, Attitude, Commander, Event, EventId, Frame,
                      FrameParser, Info, SysStat)
from transports import (BleTransport, SerialTransport, TransportError,
                        ble_scan)

FLAG_DESC = [(0x01, "IMU"), (0x02, "OLED"), (0x04, "EEPROM")]


class Dashboard:
    """單行即時更新的姿態顯示 + 事件逐行列印。"""

    def __init__(self, raw: bool) -> None:
        self.raw = raw
        self._last_att: Optional[Attitude] = None
        self._att_count = 0
        self._t0 = time.monotonic()
        self._rate = 0.0

    def _println(self, text: str) -> None:
        sys.stdout.write("\r\x1b[2K" + text + "\n")
        self._refresh()

    def _refresh(self) -> None:
        att = self._last_att
        if att is None:
            return
        line = (f"roll {att.roll:+8.2f}  pitch {att.pitch:+8.2f}  "
                f"yaw {att.yaw:+8.2f}  {att.temp_c:5.1f}C  "
                f"clicks {att.clicks:<6d} {self._rate:4.1f}Hz")
        sys.stdout.write("\r\x1b[2K" + line)
        sys.stdout.flush()

    def on_frame(self, frame: Frame) -> None:
        msg = frame.decode()
        if self.raw:
            self._println(f"[raw] type=0x{frame.msg_type:02X} seq={frame.seq} "
                          f"payload={frame.payload.hex()}")
        if isinstance(msg, Attitude):
            self._last_att = msg
            self._att_count += 1
            dt = time.monotonic() - self._t0
            if dt >= 2.0:
                self._rate = self._att_count / dt
                self._att_count = 0
                self._t0 = time.monotonic()
            self._refresh()
        elif isinstance(msg, SysStat):
            bad = [n for bit, n in FLAG_DESC if not (msg.flags & bit)]
            health = ("degraded: " + ",".join(bad)) if bad else "healthy"
            self._println(f"[stat] cpu {msg.cpu}%  up {msg.uptime_s}s  "
                          f"boots {msg.boot_count}  err {msg.err_count}  {health}")
        elif isinstance(msg, Event):
            if msg.event_id == EventId.BOOT:
                self._println(f"[event] 裝置開機（重置原因旗標 0x{msg.arg:02X}）")
            else:
                self._println(f"[event] {msg.describe()}")
        elif isinstance(msg, Ack):
            self._println(f"[ack] {msg.describe()}")
        elif isinstance(msg, Info):
            self._println(f"[info] fw v{msg.fw_ver}  proto v{msg.proto_ver}  "
                          f"uid {msg.uid}")


def stdin_worker(cmdq: "queue.Queue[str]") -> None:
    for line in sys.stdin:
        cmdq.put(line.strip())


def handle_command(line: str, transport, cmder: Commander) -> bool:
    """回傳 False 表示要求離開。"""
    parts = line.split()
    if not parts:
        return True
    cmd = parts[0].lower()
    try:
        if cmd in ("quit", "exit", "q"):
            return False
        if cmd == "ping":
            transport.write(cmder.ping())
        elif cmd == "info":
            transport.write(cmder.get_info())
        elif cmd == "cal":
            transport.write(cmder.cal_gyro())
        elif cmd == "reset":
            transport.write(cmder.reset_clicks())
        elif cmd == "rate" and len(parts) >= 2:
            transport.write(cmder.set_rate(int(parts[1])))
        else:
            print("\r可用：rate <hz> | cal | reset | ping | info | quit")
    except ValueError as exc:
        print(f"\r參數錯誤：{exc}")
    return True


def do_scan() -> None:
    print("掃描 6 秒...")
    try:
        found = ble_scan()
    except TransportError as exc:
        sys.exit(str(exc))
    if not found:
        print("（沒掃到任何裝置：確認模組供電/未被其他主機連走）")
        return
    for d in found:
        svcs = ",".join(d["services"]) or "-"
        print(f"{d['address']}  rssi {d['rssi']:4d}  "
              f"{(d['name'] or '-'):24s} svc [{svcs}]")


def main() -> None:
    ap = argparse.ArgumentParser(description="IMU-BLE Node host tool (CLI)")
    tr = ap.add_mutually_exclusive_group(required=True)
    tr.add_argument("--port", help="序列埠（例：COM5 或 /dev/ttyUSB0）")
    tr.add_argument("--ble", action="store_true", help="BLE 傳輸（需 bleak）")
    tr.add_argument("--scan", action="store_true",
                    help="掃描並列出附近 BLE 裝置與服務後離開")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--name", help="BLE 裝置名稱過濾")
    ap.add_argument("--address", help="BLE 裝置位址（跳過掃描）")
    ap.add_argument("--uuid-notify", metavar="UUID",
                    help="手動指定 notify 特徵（16-bit 縮寫如 FFE1 或完整 UUID）")
    ap.add_argument("--uuid-write", metavar="UUID",
                    help="手動指定 write 特徵")
    ap.add_argument("--csv", metavar="FILE", help="姿態資料記錄成 CSV")
    ap.add_argument("--raw", action="store_true", help="列印所有原始封包")
    ap.add_argument("--rate", type=int, help="啟動時設定遙測頻率")
    args = ap.parse_args()

    if args.scan:
        do_scan()
        return

    try:
        if args.port:
            transport = SerialTransport(args.port, args.baud)
        else:
            transport = BleTransport(args.name, args.address,
                                     args.uuid_notify, args.uuid_write)
    except TransportError as exc:
        sys.exit(str(exc))
    print(f"已連線：{transport.name}（Ctrl+C 或輸入 quit 離開）")

    parser = FrameParser()
    cmder = Commander()
    dash = Dashboard(raw=args.raw)

    csv_file = None
    csv_writer = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="", encoding="utf-8")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["host_time", "uptime_ms", "roll", "pitch",
                             "yaw", "temp_c", "clicks"])

    if args.rate is not None:
        transport.write(cmder.set_rate(args.rate))

    cmdq: "queue.Queue[str]" = queue.Queue()
    threading.Thread(target=stdin_worker, args=(cmdq,), daemon=True).start()

    try:
        while True:
            if transport.runtime_error:
                print(f"\n{transport.runtime_error}")
                break
            data = transport.read()
            if data:
                for frame in parser.feed(data):
                    dash.on_frame(frame)
                    if csv_writer is not None:
                        msg = frame.decode()
                        if isinstance(msg, Attitude):
                            csv_writer.writerow(
                                [f"{time.time():.3f}", msg.uptime_ms,
                                 msg.roll, msg.pitch, msg.yaw,
                                 msg.temp_c, msg.clicks])
            try:
                line = cmdq.get_nowait()
                if not handle_command(line, transport, cmder):
                    break
            except queue.Empty:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        print(f"\n統計：frames {parser.rx_frames}  crc_err {parser.crc_errors}  "
              f"resync {parser.resyncs}")
        if csv_file is not None:
            csv_file.close()
            print(f"CSV 已存檔：{args.csv}")
        transport.close()


if __name__ == "__main__":
    main()
