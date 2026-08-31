"""傳輸層（CLI host.py 與 GUI host_gui.py 共用）。

Serial：pyserial。
BLE：bleak（asyncio 包成同步介面），UART 服務自動偵測 ——
不同透傳韌體的 GATT UUID 各異（Nordic NUS / FFE0 / FFF0），
連線後依「已知組合 → 泛用規則」順序挑出 notify/write 特徵。

設計約定：
  - 建線失敗一律 raise TransportError（訊息可直接給使用者看）；
    由呼叫端決定 sys.exit（CLI）或彈窗（GUI）。
  - 連線後的中斷不丟例外，寫入 self.runtime_error 由輪詢端檢查
    （GUI 事件迴圈與 CLI while 迴圈都好處理）。
  - log 參數注入輸出函式（CLI 用 print，GUI 導到訊息面板）。
"""
from __future__ import annotations

import queue
import threading
from typing import Callable, List, Optional, Tuple

BT_BASE_SUFFIX = "-0000-1000-8000-00805f9b34fb"


class TransportError(Exception):
    """建立連線失敗（人類可讀訊息）。"""


def norm_uuid(u: str) -> str:
    """接受 16-bit 縮寫（FFE1）或完整 128-bit UUID，一律轉為小寫完整形。"""
    u = u.strip().lower()
    if len(u) == 4:
        return f"0000{u}{BT_BASE_SUFFIX}"
    return u


# Nordic UART Service（NUS）標準 UUID
NUS_RX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # 主機寫入 → 裝置
NUS_TX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 裝置通知 → 主機

# 已知透傳韌體的（標籤, notify 特徵, write 特徵）組合，依序嘗試
KNOWN_UART_PAIRS = [
    ("Nordic NUS",  NUS_TX_CHAR,       NUS_RX_CHAR),
    ("FFE0 單特徵", norm_uuid("ffe1"), norm_uuid("ffe1")),
    ("FFE0 雙特徵", norm_uuid("ffe1"), norm_uuid("ffe2")),
    ("FFF0",        norm_uuid("fff1"), norm_uuid("fff2")),
]

# 泛用偵測時略過的標準服務（GAP/GATT/Device Info/Battery）
STD_SVC_PREFIXES = ("00001800", "00001801", "0000180a", "0000180f")


def list_serial_ports() -> List[Tuple[str, str]]:
    """@return [(裝置名, 描述), ...]；pyserial 未安裝時回空表"""
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    return [(p.device, p.description) for p in list_ports.comports()]


def _pick_uart_chars(services, want_notify: Optional[str],
                     want_write: Optional[str]):
    """從 GATT 服務表挑出 (notify特徵, write特徵, 說明標籤)。

    優先序：使用者指定 → 已知韌體組合 → 泛用規則。
    找不到回傳 (None, None, 原因)。
    """
    chars = {}          # uuid(str) -> characteristic 物件
    for svc in services:
        for ch in svc.characteristics:
            chars[ch.uuid.lower()] = ch

    def has(uuid: str, prop_any) -> bool:
        ch = chars.get(uuid)
        return (ch is not None) and any(p in ch.properties for p in prop_any)

    # 1) 使用者指定
    if want_notify and want_write:
        n = norm_uuid(want_notify)
        w = norm_uuid(want_write)
        if (n in chars) and (w in chars):
            return chars[n], chars[w], "手動指定"
        return None, None, "指定的 UUID 不在裝置服務表中"

    # 2) 已知組合
    for label, n_uuid, w_uuid in KNOWN_UART_PAIRS:
        if has(n_uuid, ("notify", "indicate")) and \
           has(w_uuid, ("write", "write-without-response")):
            return chars[n_uuid], chars[w_uuid], label

    # 3) 泛用：非標準服務中，同服務內找 notify + write（可為同一特徵）
    for svc in services:
        if svc.uuid.lower().startswith(STD_SVC_PREFIXES):
            continue
        notify_c = None
        write_c = None
        for ch in svc.characteristics:
            props = ch.properties
            if (notify_c is None) and (("notify" in props) or
                                       ("indicate" in props)):
                notify_c = ch
            if (write_c is None) and (("write" in props) or
                                      ("write-without-response" in props)):
                write_c = ch
        if (notify_c is not None) and (write_c is not None):
            return notify_c, write_c, f"泛用偵測（服務 {svc.uuid[4:8]}）"

    return None, None, "找不到同時具備 notify 與 write 的服務"


# ------------------------------------------------------------------ Serial

class SerialTransport:
    def __init__(self, port: str, baud: int, timeout: float = 0.05,
                 log: Callable[[str], None] = print) -> None:
        try:
            import serial  # pyserial
        except ImportError:
            raise TransportError("需要 pyserial：pip install pyserial")
        try:
            self._ser = serial.Serial(port, baud, timeout=timeout)
        except Exception as exc:
            raise TransportError(f"開啟 {port} 失敗：{exc}")
        self.name = f"serial {port}@{baud}"
        self.runtime_error: Optional[str] = None
        self._log = log

    def read(self) -> bytes:
        try:
            return self._ser.read(256)
        except Exception as exc:
            self.runtime_error = f"序列埠中斷：{exc}"
            return b""

    def read_nowait(self) -> bytes:
        try:
            n = self._ser.in_waiting
            return self._ser.read(n) if n else b""
        except Exception as exc:
            self.runtime_error = f"序列埠中斷：{exc}"
            return b""

    def write(self, data: bytes) -> None:
        try:
            self._ser.write(data)
        except Exception as exc:
            self.runtime_error = f"序列埠中斷：{exc}"

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


# ------------------------------------------------------------------ BLE

class BleTransport:
    """bleak（asyncio）包成同步介面：背景執行緒跑事件迴圈。"""

    def __init__(self, name: Optional[str] = None,
                 address: Optional[str] = None,
                 uuid_notify: Optional[str] = None,
                 uuid_write: Optional[str] = None,
                 log: Callable[[str], None] = print) -> None:
        try:
            import asyncio
            from bleak import BleakClient, BleakScanner
        except ImportError:
            raise TransportError("BLE 傳輸需要 bleak：pip install bleak")

        self._rxq: "queue.Queue[bytes]" = queue.Queue()
        self._txq: "queue.Queue[bytes]" = queue.Queue()
        self._stop = threading.Event()
        self._ready = threading.Event()
        self._error: Optional[str] = None
        self.runtime_error: Optional[str] = None
        self._connected = False
        self._log = log
        self.name = "ble (connecting...)"

        async def run() -> None:
            target = address
            if target is None:
                self._log(f"掃描 BLE 裝置（名稱含 '{name or ''}'）...")
                devices = await BleakScanner.discover(timeout=5.0)
                for d in devices:
                    if d.name and (name is None or
                                   name.lower() in d.name.lower()):
                        target = d.address
                        self._log(f"找到 {d.name} [{d.address}]")
                        break
            if target is None:
                self._error = "找不到裝置（掃描檢視、名稱過濾或直接指定位址）"
                self._ready.set()
                return

            async with BleakClient(target) as client:
                notify_c, write_c, label = _pick_uart_chars(
                    client.services, uuid_notify, uuid_write)
                if notify_c is None:
                    self._error = (f"{label}；請掃描確認服務後手動指定 "
                                   f"notify/write UUID")
                    self._ready.set()
                    return
                self._log(f"UART 服務：{label}  notify={notify_c.uuid[4:8]} "
                          f"write={write_c.uuid[4:8]}")

                # 偏好 write-without-response（快）；不支援則帶回應
                w_response = "write-without-response" not in write_c.properties

                def on_notify(_h, data: bytearray) -> None:
                    self._rxq.put(bytes(data))

                await client.start_notify(notify_c, on_notify)
                self.name = f"ble {target}"
                self._connected = True
                self._ready.set()
                while not self._stop.is_set():
                    try:
                        data = self._txq.get_nowait()
                        # 20B 為最保守分段（MTU-3 的最低值）
                        for i in range(0, len(data), 20):
                            await client.write_gatt_char(
                                write_c, data[i:i + 20], response=w_response)
                    except queue.Empty:
                        await asyncio.sleep(0.02)

        def thread_main() -> None:
            try:
                asyncio.run(run())
                if self._connected and not self._stop.is_set():
                    self.runtime_error = "BLE 連線中斷"
            except Exception as exc:  # noqa: BLE001 - 顯示給使用者
                if self._connected:
                    self.runtime_error = f"BLE 中斷：{exc}"
                else:
                    self._error = str(exc)
            finally:
                self._ready.set()

        threading.Thread(target=thread_main, daemon=True).start()
        self._ready.wait(timeout=20.0)
        if self._error:
            raise TransportError(f"BLE 連線失敗：{self._error}")
        if not self._connected:
            self._stop.set()
            raise TransportError("BLE 連線逾時")

    def read(self) -> bytes:
        try:
            return self._rxq.get(timeout=0.05)
        except queue.Empty:
            return b""

    def read_nowait(self) -> bytes:
        out = b""
        while True:
            try:
                out += self._rxq.get_nowait()
            except queue.Empty:
                return out

    def write(self, data: bytes) -> None:
        self._txq.put(data)

    def close(self) -> None:
        self._stop.set()


def ble_scan(timeout: float = 6.0) -> List[dict]:
    """掃描附近裝置。@return 依 RSSI 排序的
    [{"address","name","rssi","services":[...]}, ...]"""
    try:
        import asyncio
        from bleak import BleakScanner
    except ImportError:
        raise TransportError("BLE 掃描需要 bleak：pip install bleak")

    async def run() -> List[dict]:
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        out = []
        for dev, adv in found.values():
            svcs = [u[4:8] if u.endswith(BT_BASE_SUFFIX) else u[:8]
                    for u in (adv.service_uuids or [])]
            out.append({"address": dev.address, "name": dev.name or "",
                        "rssi": adv.rssi, "services": svcs})
        return sorted(out, key=lambda d: -d["rssi"])

    return asyncio.run(run())
