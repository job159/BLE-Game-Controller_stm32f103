"""IMU-BLE Node 二進位協定 —— Python 鏡像實作。

與韌體 Core/Src/mw/proto.c 完全對應（框架、CRC、payload 佈局）。
規格文件：Docs/protocol.md
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Callable, Iterator, Optional

SOF0 = 0xAA
SOF1 = 0x55
MAX_PAYLOAD = 96
OVERHEAD = 7


class MsgType(IntEnum):
    # 裝置 → 主機
    ATTITUDE = 0x01
    SYSSTAT = 0x02
    EVENT = 0x03
    LOG = 0x04
    INFO = 0x05
    BTN = 0x06
    KEYMAP = 0x07
    ACK = 0x7F
    # 主機 → 裝置
    PING = 0x80
    SET_RATE = 0x81
    RESET_CLICKS = 0x82
    CAL_GYRO = 0x83
    GET_INFO = 0x84
    SET_KEYMAP = 0x85
    GET_KEYMAP = 0x86


class EventId(IntEnum):
    BOOT = 1
    BTN_CLICK = 2
    CLICKS_RESET = 3
    CAL_DONE = 4
    BTN_DOUBLE = 5


ACK_STATUS = {0: "OK", 1: "ERR", 2: "UNKNOWN", 3: "BUSY"}


def crc16_ccitt(data: bytes, seed: int = 0xFFFF) -> int:
    """CRC16-CCITT-FALSE：poly 0x1021, init 0xFFFF（驗證向量 '123456789' → 0x29B1）"""
    crc = seed
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def build_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = bytes([msg_type & 0xFF, seq & 0xFF, len(payload)]) + payload
    crc = crc16_ccitt(body)
    return bytes([SOF0, SOF1]) + body + struct.pack("<H", crc)


# ---------------------------------------------------------------- payloads

@dataclass
class Attitude:
    roll: float
    pitch: float
    yaw: float
    temp_c: float
    clicks: int
    uptime_ms: int

    _FMT = "<hhhhII"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "Attitude":
        r, p, y, t, clicks, up = struct.unpack(cls._FMT, b[: cls.SIZE])
        return cls(r / 100, p / 100, y / 100, t / 100, clicks, up)


@dataclass
class SysStat:
    cpu: int
    flags: int
    err_count: int
    boot_count: int
    uptime_s: int

    _FMT = "<BBHII"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "SysStat":
        return cls(*struct.unpack(cls._FMT, b[: cls.SIZE]))

    @property
    def imu_ok(self) -> bool:
        return bool(self.flags & 0x01)

    @property
    def oled_ok(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def eeprom_ok(self) -> bool:
        return bool(self.flags & 0x04)


@dataclass
class Event:
    event_id: int
    arg: int

    _FMT = "<BI"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "Event":
        return cls(*struct.unpack(cls._FMT, b[: cls.SIZE]))

    def describe(self) -> str:
        try:
            name = EventId(self.event_id).name
        except ValueError:
            name = f"UNKNOWN({self.event_id})"
        return f"{name} arg={self.arg}"


# 與韌體 mw/button.h 的 btn_event_t 數值一致
BTN_ACTIONS = {0: "press", 1: "release", 2: "click", 3: "double", 4: "long"}
BTN_PRESS = 0
BTN_RELEASE = 1

# 與韌體 PROTO_KEYMAP_SPEC_MAX 一致（映射規格字串上限，ASCII）
KEYMAP_SPEC_MAX = 35


@dataclass
class KeymapEntry:
    """裝置端保存的一鍵映射（回應 GET_KEYMAP，每鍵一幀）。"""
    key_id: int
    spec: str

    SIZE = 2   # 最小長度（key_id + len）

    @classmethod
    def parse(cls, b: bytes) -> "KeymapEntry":
        key_id, ln = b[0], b[1]
        spec = b[2:2 + ln].decode("ascii", "replace")
        return cls(key_id, spec)

    def describe(self) -> str:
        return f"KEY{self.key_id} = {self.spec or '(未設定)'}"


@dataclass
class BtnReport:
    key_id: int
    action: int

    _FMT = "<BB"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "BtnReport":
        return cls(*struct.unpack(cls._FMT, b[: cls.SIZE]))

    def describe(self) -> str:
        act = BTN_ACTIONS.get(self.action, f"?{self.action}")
        return f"KEY{self.key_id} {act}"


@dataclass
class Ack:
    req_type: int
    req_seq: int
    status: int

    _FMT = "<BBB"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "Ack":
        return cls(*struct.unpack(cls._FMT, b[: cls.SIZE]))

    def describe(self) -> str:
        try:
            req = MsgType(self.req_type).name
        except ValueError:
            req = f"0x{self.req_type:02X}"
        return f"{req} seq={self.req_seq} -> {ACK_STATUS.get(self.status, self.status)}"


@dataclass
class Info:
    proto_ver: int
    fw_ver: str
    uid: str

    _FMT = "<B11sIII"
    SIZE = struct.calcsize(_FMT)

    @classmethod
    def parse(cls, b: bytes) -> "Info":
        ver, fw, u0, u1, u2 = struct.unpack(cls._FMT, b[: cls.SIZE])
        return cls(ver, fw.split(b"\x00")[0].decode("ascii", "replace"),
                   f"{u0:08X}-{u1:08X}-{u2:08X}")


@dataclass
class Frame:
    msg_type: int
    seq: int
    payload: bytes

    def decode(self):
        """依型別解出 payload dataclass；未知型別回傳 None"""
        try:
            t = MsgType(self.msg_type)
        except ValueError:
            return None
        parsers = {
            MsgType.ATTITUDE: Attitude,
            MsgType.SYSSTAT: SysStat,
            MsgType.EVENT: Event,
            MsgType.BTN: BtnReport,
            MsgType.KEYMAP: KeymapEntry,
            MsgType.ACK: Ack,
            MsgType.INFO: Info,
        }
        cls = parsers.get(t)
        if cls is None or len(self.payload) < cls.SIZE:
            return None
        return cls.parse(self.payload)


# ---------------------------------------------------------------- parser

class FrameParser:
    """位元組串流 → Frame 解析器（與韌體同構的狀態機）。"""

    _S_SOF0, _S_SOF1, _S_TYPE, _S_SEQ, _S_LEN, _S_PAYLOAD, _S_CRC_LO, _S_CRC_HI = range(8)

    def __init__(self) -> None:
        self.rx_frames = 0
        self.crc_errors = 0
        self.resyncs = 0
        self._state = self._S_SOF0
        self._type = 0
        self._seq = 0
        self._len = 0
        self._payload = bytearray()
        self._crc_rx = 0

    def _resync(self) -> None:
        self.resyncs += 1
        self._state = self._S_SOF0

    def feed(self, data: bytes) -> Iterator[Frame]:
        """餵入位元組，yield 每個 CRC 驗證通過的完整封包。"""
        for b in data:
            if self._state == self._S_SOF0:
                if b == SOF0:
                    self._state = self._S_SOF1
            elif self._state == self._S_SOF1:
                if b == SOF1:
                    self._state = self._S_TYPE
                elif b != SOF0:  # 連續 0xAA 時停留（0xAA 0xAA 0x55 亦可同步）
                    self._state = self._S_SOF0
            elif self._state == self._S_TYPE:
                self._type = b
                self._state = self._S_SEQ
            elif self._state == self._S_SEQ:
                self._seq = b
                self._state = self._S_LEN
            elif self._state == self._S_LEN:
                if b > MAX_PAYLOAD:
                    self._resync()
                    continue
                self._len = b
                self._payload = bytearray()
                self._state = self._S_PAYLOAD if b else self._S_CRC_LO
            elif self._state == self._S_PAYLOAD:
                self._payload.append(b)
                if len(self._payload) >= self._len:
                    self._state = self._S_CRC_LO
            elif self._state == self._S_CRC_LO:
                self._crc_rx = b
                self._state = self._S_CRC_HI
            elif self._state == self._S_CRC_HI:
                self._crc_rx |= b << 8
                body = bytes([self._type, self._seq, self._len]) + bytes(self._payload)
                if crc16_ccitt(body) == self._crc_rx:
                    self.rx_frames += 1
                    self._state = self._S_SOF0
                    yield Frame(self._type, self._seq, bytes(self._payload))
                else:
                    self.crc_errors += 1
                    self._resync()


class Commander:
    """帶自動序號的命令封包產生器。"""

    def __init__(self) -> None:
        self._seq = 0

    def _next(self) -> int:
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def ping(self) -> bytes:
        return build_frame(MsgType.PING, self._next())

    def set_rate(self, hz: int) -> bytes:
        if not 0 <= hz <= 50:
            raise ValueError("rate 0..50")
        return build_frame(MsgType.SET_RATE, self._next(), bytes([hz]))

    def reset_clicks(self) -> bytes:
        return build_frame(MsgType.RESET_CLICKS, self._next())

    def cal_gyro(self) -> bytes:
        return build_frame(MsgType.CAL_GYRO, self._next())

    def get_info(self) -> bytes:
        return build_frame(MsgType.GET_INFO, self._next())

    def set_keymap(self, key_id: int, spec: str) -> bytes:
        """寫入裝置端一鍵映射（spec 須為 ASCII，len 0 = 清除）。"""
        enc = spec.encode("ascii")   # 非 ASCII 由呼叫端先行攔截
        if len(enc) > KEYMAP_SPEC_MAX:
            raise ValueError(f"spec 超過 {KEYMAP_SPEC_MAX} bytes")
        payload = bytes([key_id, len(enc)]) + enc
        return build_frame(MsgType.SET_KEYMAP, self._next(), payload)

    def get_keymap(self) -> bytes:
        return build_frame(MsgType.GET_KEYMAP, self._next())
