#!/usr/bin/env python3
"""IMU-BLE Node 主機端工具（Qt 圖形介面版）。

    pip install PySide6 pyserial          # BLE 另需 pip install bleak
    python host_gui.py

功能：
  - Serial / BLE 連線（BLE 掃描、UART 服務自動偵測、UUID 手動覆寫）
  - 即時姿態儀：人工地平線 + 航向儀 + 大字讀數
  - 20 秒滾動曲線圖（roll/pitch/yaw）
  - 系統狀態面板（點擊數/CPU/開機次數/健康燈號/鏈路統計）
  - 命令下發（遙測頻率/校正/歸零/Ping/裝置資訊）與 CSV 記錄

架構：transports.py 的傳輸物件在背景執行緒收料進 queue，
GUI 以 20ms QTimer 輪詢解包 —— Qt 物件只在主執行緒被觸碰。
"""
from __future__ import annotations

import csv
import json
import os
import queue
import sys
import threading
import time
from collections import deque
from typing import Optional

try:
    from PySide6.QtCore import Qt, QTimer, QRectF, QPointF
    from PySide6.QtGui import (QColor, QFont, QPainter, QPainterPath, QPen,
                               QPolygonF)
    from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox,
                                   QFileDialog, QGridLayout, QGroupBox,
                                   QHBoxLayout, QLabel, QLineEdit,
                                   QMainWindow, QMessageBox, QPlainTextEdit,
                                   QPushButton, QSpinBox, QStackedWidget,
                                   QVBoxLayout, QWidget)
except ImportError:
    sys.exit("圖形介面需要 PySide6：pip install PySide6")

from protocol import (Ack, Attitude, BTN_PRESS, BTN_RELEASE, BtnReport,
                      Commander, Event, EventId, Frame, FrameParser, Info,
                      KEYMAP_SPEC_MAX, KeymapEntry, SysStat)
from transports import (BleTransport, SerialTransport, TransportError,
                        ble_scan, list_serial_ports)
from keymapper import AirMouse, KeyMapper, MapError, PRESET_GROUPS

KEYMAP_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "keymap.json")
GAMEPAD_KEYS = (2, 3, 4, 5)   # 手柄鍵（KEY0/1 有裝置本地功能，不開放映射）
DEFAULT_KEYMAP = {2: "mouse:left", 3: "mouse:right",
                  4: "scroll:up", 5: "scroll:down"}

# 系列色（曲線 / 大字讀數共用）
C_ROLL = QColor("#e05c5c")
C_PITCH = QColor("#37a86c")
C_YAW = QColor("#5b8def")
C_OK = "#2f9e44"
C_BAD = "#e03131"
C_UNKNOWN = "#adb5bd"


# ------------------------------------------------------------------ 儀表元件

class HorizonWidget(QWidget):
    """人工地平線：天空/地面隨 roll 旋轉、pitch 平移，中央符號固定。"""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(180, 180)
        self._roll = 0.0
        self._pitch = 0.0

    def set_attitude(self, roll: float, pitch: float) -> None:
        self._roll = roll
        self._pitch = max(-60.0, min(60.0, pitch))
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        side = min(self.width(), self.height()) - 8
        radius = side / 2.0
        cx, cy = self.width() / 2.0, self.height() / 2.0

        clip = QPainterPath()
        clip.addEllipse(QRectF(cx - radius, cy - radius, side, side))
        p.setClipPath(clip)

        p.translate(cx, cy)
        p.save()
        p.rotate(-self._roll)
        pitch_px = self._pitch / 45.0 * radius   # 45° = 半徑
        big = radius * 3
        p.fillRect(QRectF(-big, -big, 2 * big, big + pitch_px),
                   QColor("#4f83c2"))            # 天
        p.fillRect(QRectF(-big, pitch_px, 2 * big, big * 2),
                   QColor("#8a6844"))            # 地
        p.setPen(QPen(Qt.white, 2))
        p.drawLine(QPointF(-radius, pitch_px), QPointF(radius, pitch_px))

        # pitch 刻度（每 10°）
        p.setPen(QPen(QColor(255, 255, 255, 200), 1))
        for deg in (-30, -20, -10, 10, 20, 30):
            y = pitch_px - deg / 45.0 * radius
            w = radius * (0.5 if deg % 30 == 0 else 0.3)
            p.drawLine(QPointF(-w / 2, y), QPointF(w / 2, y))
        p.restore()

        # 固定機徽
        p.setPen(QPen(QColor("#ffd43b"), 3))
        p.drawLine(QPointF(-radius * 0.55, 0), QPointF(-radius * 0.18, 0))
        p.drawLine(QPointF(radius * 0.18, 0), QPointF(radius * 0.55, 0))
        p.drawEllipse(QPointF(0, 0), 2.5, 2.5)

        # 外框
        p.setClipping(False)
        p.setPen(QPen(QColor("#495057"), 2))
        p.setBrush(Qt.NoBrush)
        p.drawEllipse(QRectF(cx - radius, cy - radius, side, side))


class CompassWidget(QWidget):
    """航向儀（yaw 為相對值：長按 KEY1 前次校正時的機頭方向為 0）。"""

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(180, 180)
        self._yaw = 0.0

    def set_yaw(self, yaw: float) -> None:
        self._yaw = yaw
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        side = min(self.width(), self.height()) - 8
        radius = side / 2.0
        cx, cy = self.width() / 2.0, self.height() / 2.0

        p.setPen(QPen(QColor("#495057"), 2))
        p.setBrush(QColor("#f1f3f5"))
        p.drawEllipse(QRectF(cx - radius, cy - radius, side, side))

        p.translate(cx, cy)
        p.setPen(QPen(QColor("#868e96"), 1))
        for deg in range(0, 360, 30):
            p.save()
            p.rotate(deg)
            inner = radius * (0.82 if deg % 90 == 0 else 0.9)
            p.drawLine(QPointF(0, -inner), QPointF(0, -radius * 0.97))
            p.restore()

        # 指針（機頭方向）
        p.save()
        p.rotate(self._yaw)
        needle = QPolygonF([QPointF(0, -radius * 0.8),
                            QPointF(-radius * 0.1, radius * 0.12),
                            QPointF(radius * 0.1, radius * 0.12)])
        p.setPen(Qt.NoPen)
        p.setBrush(C_YAW)
        p.drawPolygon(needle)
        p.restore()

        p.setPen(QColor("#343a40"))
        font = QFont(self.font())
        font.setPointSize(11)
        font.setBold(True)
        p.setFont(font)
        p.drawText(QRectF(-radius, radius * 0.3, 2 * radius, radius * 0.5),
                   Qt.AlignCenter, f"{self._yaw:+.1f}°")


class RollingPlot(QWidget):
    """20 秒滾動曲線（roll/pitch/yaw），純 QPainter、零額外相依。"""

    WINDOW_S = 20.0

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumHeight(170)
        self._data: deque = deque()   # (t, roll, pitch, yaw)

    def add(self, roll: float, pitch: float, yaw: float) -> None:
        now = time.monotonic()
        self._data.append((now, roll, pitch, yaw))
        cutoff = now - self.WINDOW_S
        while self._data and (self._data[0][0] < cutoff):
            self._data.popleft()
        self.update()

    def clear(self) -> None:
        self._data.clear()
        self.update()

    def paintEvent(self, _ev) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        ml, mr, mt, mb = 40, 10, 8, 20
        w = self.width() - ml - mr
        h = self.height() - mt - mb
        if (w <= 10) or (h <= 10):
            return
        p.fillRect(self.rect(), QColor("#fcfcfd"))
        p.setPen(QPen(QColor("#dee2e6"), 1))
        p.drawRect(ml, mt, w, h)

        # y 範圍：對稱、至少 ±10°
        y_max = 10.0
        for _, r, pi, y in self._data:
            y_max = max(y_max, abs(r), abs(pi), abs(y))
        y_max = min(200.0, y_max * 1.1)

        def to_xy(t: float, v: float, now: float) -> QPointF:
            x = ml + w * (1.0 - (now - t) / self.WINDOW_S)
            y = mt + h / 2.0 - (v / y_max) * (h / 2.0)
            return QPointF(x, y)

        # 水平格線
        p.setPen(QPen(QColor("#e9ecef"), 1))
        p.setFont(QFont(self.font().family(), 7))
        for frac, val in ((0.0, y_max), (0.5, 0.0), (1.0, -y_max)):
            gy = mt + h * frac
            p.drawLine(ml, int(gy), ml + w, int(gy))
            p.setPen(QColor("#868e96"))
            p.drawText(QRectF(0, gy - 7, ml - 4, 14),
                       Qt.AlignRight | Qt.AlignVCenter, f"{val:+.0f}")
            p.setPen(QPen(QColor("#e9ecef"), 1))

        if len(self._data) >= 2:
            now = time.monotonic()
            for idx, color in ((1, C_ROLL), (2, C_PITCH), (3, C_YAW)):
                poly = QPolygonF([to_xy(d[0], d[idx], now)
                                  for d in self._data])
                p.setPen(QPen(color, 1.6))
                p.drawPolyline(poly)

        # 圖例
        p.setFont(QFont(self.font().family(), 8, QFont.Bold))
        x = ml + 8
        for text, color in (("roll", C_ROLL), ("pitch", C_PITCH),
                            ("yaw", C_YAW)):
            p.setPen(color)
            p.drawText(x, mt + 14, text)
            x += 46


# ------------------------------------------------------------------ 主視窗

class MainWindow(QMainWindow):
    POLL_MS = 5   # 手柄/空中滑鼠延遲優化（原 20ms）

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("IMU-BLE Node 主機工具")
        self.resize(1080, 720)

        self.transport = None
        self.parser = FrameParser()
        self.cmder = Commander()
        self.csv_file = None
        self.csv_writer = None
        self._fps_count = 0
        self._fps_t0 = time.monotonic()
        self._fps = 0.0
        self._rx_bytes = 0   # 原始位元組計數：>0 但封包=0 → 鮑率/資料損毀

        self.mapper = KeyMapper()   # 手柄按鍵 → 鍵盤/滑鼠
        self.airmouse = AirMouse()  # 姿態 → 游標（KEY0 雙擊切換）
        self.air_on = False         # 裝置端模式狀態（EVENT/STAT 同步）
        self._km_syncing = False    # 套用「裝置回傳的配置」時避免回射 SET

        # 背景執行緒 → GUI 的訊息通道（Qt 物件只能在主執行緒操作）
        self._log_q: "queue.Queue[str]" = queue.Queue()
        self._conn_q: "queue.Queue[tuple]" = queue.Queue()
        self._scan_q: "queue.Queue[tuple]" = queue.Queue()

        self._build_ui()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(self.POLL_MS)

    # ------------------------------------------------------------ UI 組裝

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)

        layout.addWidget(self._build_conn_bar())

        mid = QHBoxLayout()
        self.horizon = HorizonWidget()
        self.compass = CompassWidget()
        mid.addWidget(self.horizon, 1)
        mid.addWidget(self.compass, 1)
        mid.addWidget(self._build_readout(), 1)
        mid.addWidget(self._build_status(), 1)
        layout.addLayout(mid)

        self.plot = RollingPlot()
        layout.addWidget(self.plot, 1)

        bottom = QHBoxLayout()
        bottom.addWidget(self._build_commands())
        bottom.addWidget(self._build_keymap())
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(500)
        self.log_view.setFont(QFont("Consolas", 9))
        bottom.addWidget(self.log_view, 1)
        layout.addLayout(bottom)
        self._load_keymap()

    def _build_conn_bar(self) -> QGroupBox:
        box = QGroupBox("連線")
        lay = QHBoxLayout(box)

        self.tr_combo = QComboBox()
        self.tr_combo.addItems(["Serial", "BLE"])
        lay.addWidget(self.tr_combo)

        self.stack = QStackedWidget()

        # --- Serial 參數 ---
        ser_w = QWidget()
        ser_l = QHBoxLayout(ser_w)
        ser_l.setContentsMargins(0, 0, 0, 0)
        ser_l.addWidget(QLabel("埠："))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        ser_l.addWidget(self.port_combo)
        btn_refresh = QPushButton("重新整理")
        btn_refresh.clicked.connect(self._refresh_ports)
        ser_l.addWidget(btn_refresh)
        ser_l.addWidget(QLabel("鮑率："))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "9600", "57600", "230400"])
        ser_l.addWidget(self.baud_combo)
        ser_l.addStretch(1)
        self.stack.addWidget(ser_w)

        # --- BLE 參數 ---
        ble_w = QWidget()
        ble_l = QHBoxLayout(ble_w)
        ble_l.setContentsMargins(0, 0, 0, 0)
        self.ble_scan_btn = QPushButton("掃描")
        self.ble_scan_btn.clicked.connect(self._start_scan)
        ble_l.addWidget(self.ble_scan_btn)
        self.ble_dev_combo = QComboBox()
        self.ble_dev_combo.setMinimumWidth(260)
        ble_l.addWidget(self.ble_dev_combo)
        ble_l.addWidget(QLabel("UUID(選填)："))
        self.uuid_notify_edit = QLineEdit()
        self.uuid_notify_edit.setPlaceholderText("notify 自動")
        self.uuid_notify_edit.setMaximumWidth(110)
        ble_l.addWidget(self.uuid_notify_edit)
        self.uuid_write_edit = QLineEdit()
        self.uuid_write_edit.setPlaceholderText("write 自動")
        self.uuid_write_edit.setMaximumWidth(110)
        ble_l.addWidget(self.uuid_write_edit)
        ble_l.addStretch(1)
        self.stack.addWidget(ble_w)

        self.tr_combo.currentIndexChanged.connect(self.stack.setCurrentIndex)
        lay.addWidget(self.stack, 1)

        self.conn_btn = QPushButton("連線")
        self.conn_btn.setMinimumWidth(90)
        self.conn_btn.clicked.connect(self._toggle_connect)
        lay.addWidget(self.conn_btn)

        self._refresh_ports()
        return box

    def _build_readout(self) -> QGroupBox:
        box = QGroupBox("姿態讀數")
        grid = QGridLayout(box)
        font_big = QFont("Consolas", 20, QFont.Bold)
        font_small = QFont("Consolas", 12)

        self.lab_vals = {}
        for row, (key, color) in enumerate(
                (("roll", C_ROLL), ("pitch", C_PITCH), ("yaw", C_YAW))):
            name = QLabel(key.upper())
            name.setStyleSheet(f"color:{color.name()};font-weight:bold;")
            val = QLabel("--")
            val.setFont(font_big)
            val.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            grid.addWidget(name, row, 0)
            grid.addWidget(val, row, 1)
            self.lab_vals[key] = val

        self.lab_temp = QLabel("溫度 --")
        self.lab_temp.setFont(font_small)
        grid.addWidget(self.lab_temp, 3, 0, 1, 2)
        return box

    def _build_status(self) -> QGroupBox:
        box = QGroupBox("系統狀態")
        grid = QGridLayout(box)

        self.lab_clicks = QLabel("--")
        self.lab_clicks.setFont(QFont("Consolas", 22, QFont.Bold))
        grid.addWidget(QLabel("點擊數"), 0, 0)
        grid.addWidget(self.lab_clicks, 0, 1)

        self.lab_stat = QLabel("運行 --  CPU --%")
        self.lab_boot = QLabel("開機 --  錯誤 --")
        self.lab_link = QLabel("遙測 -- Hz  收包 0  CRC錯 0")
        for row, lab in ((1, self.lab_stat), (2, self.lab_boot),
                         (3, self.lab_link)):
            grid.addWidget(lab, row, 0, 1, 2)

        dots = QHBoxLayout()
        self.health = {}
        for key in ("IMU", "OLED", "EEPROM"):
            lab = QLabel(f"● {key}")
            lab.setStyleSheet(f"color:{C_UNKNOWN};font-weight:bold;")
            self.health[key] = lab
            dots.addWidget(lab)
        dots.addStretch(1)
        grid.addLayout(dots, 4, 0, 1, 2)
        return box

    def _build_commands(self) -> QGroupBox:
        box = QGroupBox("命令")
        grid = QGridLayout(box)

        grid.addWidget(QLabel("遙測頻率"), 0, 0)
        self.rate_spin = QSpinBox()
        self.rate_spin.setRange(0, 50)
        self.rate_spin.setValue(10)
        self.rate_spin.setSuffix(" Hz")
        grid.addWidget(self.rate_spin, 0, 1)
        btn_rate = QPushButton("套用")
        btn_rate.clicked.connect(
            lambda: self._send(self.cmder.set_rate(self.rate_spin.value())))
        grid.addWidget(btn_rate, 0, 2)

        actions = [
            ("校正陀螺儀", lambda: self._send(self.cmder.cal_gyro())),
            ("點擊歸零", lambda: self._send(self.cmder.reset_clicks())),
            ("Ping", lambda: self._send(self.cmder.ping())),
            ("裝置資訊", lambda: self._send(self.cmder.get_info())),
        ]
        for i, (text, fn) in enumerate(actions):
            btn = QPushButton(text)
            btn.clicked.connect(fn)
            grid.addWidget(btn, 1 + i // 2, (i % 2) * 2, 1, 2)

        self.csv_btn = QPushButton("開始記錄 CSV")
        self.csv_btn.clicked.connect(self._toggle_csv)
        grid.addWidget(self.csv_btn, 3, 0, 1, 3)

        self.raw_check = QCheckBox("顯示原始封包")
        grid.addWidget(self.raw_check, 4, 0, 1, 3)
        self.rawbytes_check = QCheckBox("顯示原始位元組(hex)")
        self.rawbytes_check.setToolTip(
            "把收到的每一段原始位元組以 hex 印進日誌 —— 鮑率錯配時"
            "看到的是隨機碎渣；正常時應看到 aa 55 開頭的封包")
        grid.addWidget(self.rawbytes_check, 5, 0, 1, 3)
        return box

    def _build_keymap(self) -> QGroupBox:
        box = QGroupBox("手柄映射（KEY2~KEY5）")
        grid = QGridLayout(box)

        self.km_dots = {}
        self.km_combos = {}
        for row, key_id in enumerate(GAMEPAD_KEYS):
            dot = QLabel("●")
            dot.setStyleSheet(f"color:{C_UNKNOWN};")
            self.km_dots[key_id] = dot
            grid.addWidget(dot, row, 0)
            grid.addWidget(QLabel(f"KEY{key_id}"), row, 1)

            combo = QComboBox()
            combo.setEditable(True)
            self._fill_mapping_combo(combo)
            combo.setMinimumWidth(150)
            combo.setMaxVisibleItems(24)
            combo.setToolTip(
                "可直接輸入，格式：\n"
                "  mouse:left/right/middle/double/x1/x2\n"
                "  scroll:up/down/left/right（按住連發）\n"
                "  鍵盤鍵或組合：space、w、f5、ctrl+c、alt+tab、\n"
                "  volume_up、play_pause…（按住=按住）\n"
                "  text:要輸入的文字\n"
                "  run:程式或命令（如 run:notepad）\n"
                "留空 = 不動作")
            combo.setCurrentText(DEFAULT_KEYMAP.get(key_id, ""))
            combo.currentTextChanged.connect(
                lambda text, k=key_id: self._apply_mapping(k, text))
            self.km_combos[key_id] = combo
            grid.addWidget(combo, row, 2)

        self.km_enable = QCheckBox("啟用控制電腦")
        self.km_enable.setToolTip("勾選後裝置按鍵將真的觸發滑鼠/鍵盤；"
                                  "為安全起見每次啟動都預設關閉")
        self.km_enable.toggled.connect(self._toggle_gamepad)
        grid.addWidget(self.km_enable, len(GAMEPAD_KEYS), 0, 1, 3)

        # --- 空中滑鼠（裝置 KEY0 雙擊切換模式；此處調手感） ---
        row = len(GAMEPAD_KEYS) + 1
        self.air_dot = QLabel("●")
        self.air_dot.setStyleSheet(f"color:{C_UNKNOWN};")
        grid.addWidget(self.air_dot, row, 0)
        grid.addWidget(QLabel("空中滑鼠(KEY0雙擊)"), row, 1, 1, 2)

        from PySide6.QtWidgets import QSlider
        self.air_slider = QSlider(Qt.Horizontal)
        self.air_slider.setRange(5, 80)
        self.air_slider.setValue(int(self.airmouse.gain))
        self.air_slider.setToolTip("靈敏度（px/度）")
        self.air_slider.valueChanged.connect(
            lambda v: setattr(self.airmouse, "gain", float(v)))
        grid.addWidget(QLabel("靈敏度"), row + 1, 0, 1, 1)
        grid.addWidget(self.air_slider, row + 1, 1, 1, 2)

        inv = QHBoxLayout()
        self.air_inv_x = QCheckBox("反轉X")
        self.air_inv_y = QCheckBox("反轉Y")
        self.air_inv_x.toggled.connect(
            lambda on: setattr(self.airmouse, "invert_x", on))
        self.air_inv_y.toggled.connect(
            lambda on: setattr(self.airmouse, "invert_y", on))
        inv.addWidget(self.air_inv_x)
        inv.addWidget(self.air_inv_y)
        inv.addStretch(1)
        grid.addLayout(inv, row + 2, 0, 1, 3)
        return box

    def _set_air_on(self, on: bool, source: str) -> None:
        """同步裝置端空中滑鼠狀態（EVENT 即時 / STAT 每秒對帳）。"""
        if on == self.air_on:
            return
        self.air_on = on
        self.air_dot.setStyleSheet(
            f"color:{C_OK if on else C_UNKNOWN};")
        self._log(f"[空中滑鼠] {'開啟' if on else '關閉'}（{source}）")
        if on:
            # 拉高遙測頻率讓游標平滑（9600 鮑率下 30Hz 仍在頻寬內）
            self._send(self.cmder.set_rate(30))
            if not self.km_enable.isChecked():
                self._log("[空中滑鼠] 提示：勾選「啟用控制電腦」才會實際移動游標")
        else:
            self._send(self.cmder.set_rate(10))
        self._update_air_active()

    def _update_air_active(self) -> None:
        self.airmouse.active = self.air_on and self.km_enable.isChecked()
        if not self.airmouse.active:
            self.airmouse.reset()

    # ------------------------------------------------------------ 手柄映射

    @staticmethod
    def _fill_mapping_combo(combo: QComboBox) -> None:
        """分類填充：分類標題列不可選，避免長清單捲到眼花。"""
        combo.addItem("")
        for title, items in PRESET_GROUPS:
            combo.addItem(f"── {title} ──")
            header = combo.model().item(combo.count() - 1)
            header.setEnabled(False)
            for it in items:
                combo.addItem(it)

    def _apply_mapping(self, key_id: int, spec: str) -> None:
        combo = self.km_combos[key_id]
        try:
            self.mapper.set_mapping(key_id, spec)
            combo.setStyleSheet("")
            self._save_keymap()
        except MapError as exc:
            combo.setStyleSheet("border:1px solid #e03131;")
            self._log(f"[手柄] KEY{key_id} 映射無效：{exc}")
            return
        self._push_mapping_to_device(key_id, spec.strip())

    def _push_mapping_to_device(self, key_id: int, spec: str) -> None:
        """使用者編輯 → 同步寫入裝置 EEPROM（手柄自帶配置）。"""
        if self._km_syncing or (self.transport is None):
            return
        try:
            enc = spec.encode("ascii")
        except UnicodeEncodeError:
            self._log(f"[手柄] KEY{key_id} 含非 ASCII，僅存於本機不寫入裝置")
            return
        if len(enc) > KEYMAP_SPEC_MAX:
            self._log(f"[手柄] KEY{key_id} 超過 {KEYMAP_SPEC_MAX} 字元，"
                      f"僅存於本機不寫入裝置")
            return
        self.transport.write(self.cmder.set_keymap(key_id, spec))

    def _toggle_gamepad(self, on: bool) -> None:
        if on:
            # 先確認映射後端可用（pynput 缺席時立即提示而非按下才炸）
            try:
                self.mapper._ensure_backend()
            except MapError as exc:
                QMessageBox.warning(self, "手柄", str(exc))
                self.km_enable.setChecked(False)
                return
            self.km_enable.setStyleSheet("color:#e8590c;font-weight:bold;")
            self._log("[手柄] 已啟用：裝置按鍵將控制此電腦")
        else:
            self.mapper.release_all()
            self.km_enable.setStyleSheet("")
            self._log("[手柄] 已停用")
        self.mapper.enabled = on
        self._update_air_active()

    def _load_keymap(self) -> None:
        mapping = dict(DEFAULT_KEYMAP)
        try:
            with open(KEYMAP_FILE, "r", encoding="utf-8") as f:
                saved = json.load(f).get("map", {})
            mapping.update({int(k): v for k, v in saved.items()})
        except (OSError, ValueError):
            pass   # 無設定檔/損毀 → 用預設
        for key_id in GAMEPAD_KEYS:
            spec = mapping.get(key_id, "")
            self.km_combos[key_id].setCurrentText(spec)
            try:
                self.mapper.set_mapping(key_id, spec)
            except MapError:
                pass

    def _save_keymap(self) -> None:
        data = {"map": {str(k): self.km_combos[k].currentText().strip()
                        for k in GAMEPAD_KEYS}}
        try:
            with open(KEYMAP_FILE, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except OSError as exc:
            self._log(f"[手柄] 設定存檔失敗：{exc}")

    # ------------------------------------------------------------ 連線

    def _refresh_ports(self) -> None:
        self.port_combo.clear()
        for dev, desc in list_serial_ports():
            self.port_combo.addItem(f"{dev} — {desc}", dev)
        if self.port_combo.count() == 0:
            self.port_combo.addItem("（找不到序列埠）", None)

    def _start_scan(self) -> None:
        self.ble_scan_btn.setEnabled(False)
        self.ble_scan_btn.setText("掃描中...")
        self._log("BLE 掃描 6 秒...")

        def work() -> None:
            try:
                self._scan_q.put(("ok", ble_scan()))
            except TransportError as exc:
                self._scan_q.put(("err", str(exc)))
            except Exception as exc:  # noqa: BLE001
                self._scan_q.put(("err", f"掃描失敗：{exc}"))

        threading.Thread(target=work, daemon=True).start()

    def _toggle_connect(self) -> None:
        if self.transport is not None:
            self._disconnect("已中斷連線")
            return

        kind = self.tr_combo.currentText()
        if kind == "Serial":
            port = self.port_combo.currentData()
            if not port:
                QMessageBox.warning(self, "連線", "請先選擇序列埠")
                return
            baud = int(self.baud_combo.currentText())
            params = ("serial", port, baud)
        else:
            addr = self.ble_dev_combo.currentData()
            uuid_n = self.uuid_notify_edit.text().strip() or None
            uuid_w = self.uuid_write_edit.text().strip() or None
            params = ("ble", addr, uuid_n, uuid_w)

        self.conn_btn.setEnabled(False)
        self.conn_btn.setText("連線中...")

        def work() -> None:
            try:
                if params[0] == "serial":
                    t = SerialTransport(params[1], params[2], timeout=0,
                                        log=self._log_q.put)
                else:
                    t = BleTransport(address=params[1],
                                     uuid_notify=params[2],
                                     uuid_write=params[3],
                                     log=self._log_q.put)
                self._conn_q.put(("ok", t))
            except TransportError as exc:
                self._conn_q.put(("err", str(exc)))
            except Exception as exc:  # noqa: BLE001
                self._conn_q.put(("err", f"未預期錯誤：{exc}"))

        threading.Thread(target=work, daemon=True).start()

    def _disconnect(self, reason: str) -> None:
        self.mapper.release_all()   # 斷線時放開所有按住中的鍵
        if self.transport is not None:
            self.transport.close()
            self.transport = None
        self.conn_btn.setText("連線")
        self.conn_btn.setEnabled(True)
        self.statusBar().showMessage(reason)
        self._log(reason)

    # ------------------------------------------------------------ 輪詢

    def _poll(self) -> None:
        # 背景執行緒訊息
        while True:
            try:
                self._log(self._log_q.get_nowait())
            except queue.Empty:
                break

        try:
            status, payload = self._conn_q.get_nowait()
            if status == "ok":
                self.transport = payload
                self.parser = FrameParser()
                self._rx_bytes = 0
                self.plot.clear()
                self.conn_btn.setText("中斷連線")
                self.conn_btn.setEnabled(True)
                self.statusBar().showMessage(f"已連線：{payload.name}")
                self._log(f"已連線：{payload.name}")
                # 手柄配置以裝置（EEPROM）為準：連上即讀回
                self.transport.write(self.cmder.get_keymap())
            else:
                self.conn_btn.setText("連線")
                self.conn_btn.setEnabled(True)
                QMessageBox.warning(self, "連線失敗", payload)
        except queue.Empty:
            pass

        try:
            status, payload = self._scan_q.get_nowait()
            self.ble_scan_btn.setEnabled(True)
            self.ble_scan_btn.setText("掃描")
            if status == "ok":
                self.ble_dev_combo.clear()
                for d in payload:
                    svc = ",".join(d["services"][:3]) or "-"
                    label = (f"{d['name'] or '(無名稱)'}  {d['address']}  "
                             f"rssi {d['rssi']}  [{svc}]")
                    self.ble_dev_combo.addItem(label, d["address"])
                self._log(f"掃描到 {len(payload)} 個裝置")
            else:
                QMessageBox.warning(self, "掃描", payload)
        except queue.Empty:
            pass

        if self.transport is None:
            return
        if self.transport.runtime_error:
            self._disconnect(self.transport.runtime_error)
            return

        data = self.transport.read_nowait()
        if data:
            self._rx_bytes += len(data)
            if self.rawbytes_check.isChecked():
                for off in range(0, min(len(data), 256), 32):
                    self._log(f"[bytes] {data[off:off + 32].hex(' ')}")
            for frame in self.parser.feed(data):
                self._on_frame(frame)
        self.mapper.poll()   # 滾輪按住連發

        now = time.monotonic()
        if (now - self._fps_t0) >= 1.0:
            self._fps = self._fps_count / (now - self._fps_t0)
            self._fps_count = 0
            self._fps_t0 = now
            self.lab_link.setText(
                f"遙測 {self._fps:.1f} Hz  位元組 {self._rx_bytes}  "
                f"封包 {self.parser.rx_frames}  CRC錯 {self.parser.crc_errors}")

    # ------------------------------------------------------------ 封包處理

    def _on_frame(self, frame: Frame) -> None:
        msg = frame.decode()
        if self.raw_check.isChecked():
            self._log(f"[raw] type=0x{frame.msg_type:02X} seq={frame.seq} "
                      f"payload={frame.payload.hex()}")

        if isinstance(msg, Attitude):
            self._fps_count += 1
            self.horizon.set_attitude(msg.roll, msg.pitch)
            self.compass.set_yaw(msg.yaw)
            self.lab_vals["roll"].setText(f"{msg.roll:+8.2f}°")
            self.lab_vals["pitch"].setText(f"{msg.pitch:+8.2f}°")
            self.lab_vals["yaw"].setText(f"{msg.yaw:+8.2f}°")
            self.lab_temp.setText(f"溫度 {msg.temp_c:.1f} °C")
            self.lab_clicks.setText(str(msg.clicks))
            self.plot.add(msg.roll, msg.pitch, msg.yaw)
            try:
                self.airmouse.feed(msg.yaw, msg.pitch)
            except MapError as exc:
                self.airmouse.active = False
                self._log(f"[空中滑鼠] {exc}")
            if self.csv_writer is not None:
                self.csv_writer.writerow(
                    [f"{time.time():.3f}", msg.uptime_ms, msg.roll,
                     msg.pitch, msg.yaw, msg.temp_c, msg.clicks])
        elif isinstance(msg, SysStat):
            up_h, up_rem = divmod(msg.uptime_s, 3600)
            up_m, up_s = divmod(up_rem, 60)
            self.lab_stat.setText(
                f"運行 {up_h:02d}:{up_m:02d}:{up_s:02d}  CPU {msg.cpu}%")
            self.lab_boot.setText(
                f"開機 {msg.boot_count} 次  錯誤 {msg.err_count}")
            for key, ok in (("IMU", msg.imu_ok), ("OLED", msg.oled_ok),
                            ("EEPROM", msg.eeprom_ok)):
                color = C_OK if ok else C_BAD
                self.health[key].setStyleSheet(
                    f"color:{color};font-weight:bold;")
            self._set_air_on(msg.air_mouse, "狀態同步")
        elif isinstance(msg, BtnReport):
            if msg.key_id in self.km_dots:
                if msg.action == BTN_PRESS:
                    self.km_dots[msg.key_id].setStyleSheet(f"color:{C_OK};")
                elif msg.action == BTN_RELEASE:
                    self.km_dots[msg.key_id].setStyleSheet(
                        f"color:{C_UNKNOWN};")
            try:
                fired = self.mapper.on_button(msg.key_id, msg.action)
            except MapError as exc:
                fired = None
                self._log(f"[手柄] 執行失敗：{exc}")
            if fired:
                self._log(f"[手柄] {msg.describe()} → {fired}")
        elif isinstance(msg, KeymapEntry):
            # 裝置回傳的持久化配置：覆蓋面板但不回射 SET
            if (msg.key_id in self.km_combos) and msg.spec:
                self._km_syncing = True
                try:
                    self.km_combos[msg.key_id].setCurrentText(msg.spec)
                finally:
                    self._km_syncing = False
                self._log(f"[手柄] 裝置配置 {msg.describe()}")
        elif isinstance(msg, Event):
            if msg.event_id == EventId.BOOT:
                self._log(f"[事件] 裝置開機（重置原因旗標 0x{msg.arg:02X}）")
            elif msg.event_id == EventId.AIRMOUSE:
                self._set_air_on(bool(msg.arg), "KEY0 雙擊")
            else:
                self._log(f"[事件] {msg.describe()}")
        elif isinstance(msg, Ack):
            self._log(f"[回覆] {msg.describe()}")
        elif isinstance(msg, Info):
            self._log(f"[資訊] fw v{msg.fw_ver}  proto v{msg.proto_ver}  "
                      f"uid {msg.uid}")

    # ------------------------------------------------------------ 工具

    def _send(self, frame: bytes) -> None:
        if self.transport is None:
            self.statusBar().showMessage("尚未連線")
            return
        self.transport.write(frame)

    def _toggle_csv(self) -> None:
        if self.csv_file is not None:
            path = self.csv_file.name
            self.csv_file.close()
            self.csv_file = None
            self.csv_writer = None
            self.csv_btn.setText("開始記錄 CSV")
            self._log(f"CSV 已存檔：{path}")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "儲存 CSV", f"imu_{time.strftime('%Y%m%d_%H%M%S')}.csv",
            "CSV (*.csv)")
        if not path:
            return
        self.csv_file = open(path, "w", newline="", encoding="utf-8")
        self.csv_writer = csv.writer(self.csv_file)
        self.csv_writer.writerow(["host_time", "uptime_ms", "roll", "pitch",
                                  "yaw", "temp_c", "clicks"])
        self.csv_btn.setText("停止記錄 CSV")
        self._log(f"CSV 記錄中：{path}")

    def _log(self, text: str) -> None:
        self.log_view.appendPlainText(f"{time.strftime('%H:%M:%S')} {text}")

    def closeEvent(self, ev) -> None:
        self.mapper.release_all()
        if self.transport is not None:
            self.transport.close()
        if self.csv_file is not None:
            self.csv_file.close()
        ev.accept()


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
