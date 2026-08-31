"""keymapper.py — 手柄按鍵 → PC 鍵盤/滑鼠動作映射引擎。

映射規格字串（GUI 下拉可選、也可手打）：
  ""                        不動作
  "mouse:left|right|middle|x1|x2"  滑鼠鍵（按住=按住，可拖曳）
  "mouse:double"            按下時左鍵雙擊
  "scroll:up|down|left|right"  滾輪（按住每 150ms 連發）
  "text:任意文字"           按下時輸入整段文字
  "run:notepad"             按下時啟動程式（如 Stream Deck）
  其他                      鍵盤按鍵或組合，'+' 分隔：
                            "space"、"w"、"f5"、"ctrl+c"、
                            "alt+tab"、"play_pause"、"volume_up"
                            （按住=按住，放開才釋放 → 遊戲可壓 WASD）

實作注意：
  - pynput 延遲載入且可注入假物件 → 單元測試不需要真的動滑鼠。
  - press 依序壓下、release 反序放開（組合鍵的正確順序）。
"""
from __future__ import annotations

import subprocess
import time
from typing import Callable, Dict, List, Optional, Tuple

from protocol import BTN_PRESS, BTN_RELEASE

# 常用別名 → pynput.keyboard.Key 屬性名（f1~f20 等直接同名，毋須列出）
KEY_ALIASES = {
    "ctrl": "ctrl", "alt": "alt", "shift": "shift", "win": "cmd",
    "windows": "cmd", "super": "cmd", "cmd": "cmd",
    "esc": "esc", "tab": "tab", "enter": "enter", "return": "enter",
    "space": "space", "backspace": "backspace",
    "delete": "delete", "del": "delete",
    "up": "up", "down": "down", "left": "left", "right": "right",
    "home": "home", "end": "end",
    "pageup": "page_up", "pgup": "page_up",
    "pagedown": "page_down", "pgdn": "page_down",
    "insert": "insert", "capslock": "caps_lock",
    "numlock": "num_lock", "scrolllock": "scroll_lock",
    "printscreen": "print_screen", "prtsc": "print_screen",
    "pause": "pause", "menu": "menu",
    "play_pause": "media_play_pause", "next_track": "media_next",
    "prev_track": "media_previous", "volume_up": "media_volume_up",
    "volume_down": "media_volume_down", "mute": "media_volume_mute",
}

SCROLL_REPEAT_S = 0.15
SCROLL_STEP = 2

# GUI 下拉選單：分類 → 選項（皆為合法規格字串）
PRESET_GROUPS = [
    ("滑鼠", ["mouse:left", "mouse:right", "mouse:middle", "mouse:double",
              "mouse:x1", "mouse:x2"]),
    ("滾輪", ["scroll:up", "scroll:down", "scroll:left", "scroll:right"]),
    ("導覽", ["up", "down", "left", "right", "home", "end",
              "pageup", "pagedown", "tab", "esc", "enter", "space",
              "backspace", "delete"]),
    ("簡報", ["f5", "shift+f5", "b", "pageup", "pagedown", "esc"]),
    ("媒體", ["play_pause", "next_track", "prev_track",
              "volume_up", "volume_down", "mute"]),
    ("編輯", ["ctrl+c", "ctrl+v", "ctrl+x", "ctrl+z", "ctrl+y",
              "ctrl+a", "ctrl+s", "ctrl+f"]),
    ("視窗/系統", ["alt+tab", "alt+f4", "win+d", "win+e", "win+tab",
                   "print_screen", "win+shift+s"]),
    ("瀏覽器", ["ctrl+t", "ctrl+w", "ctrl+shift+t", "ctrl+tab",
                "alt+left", "alt+right", "f11", "f12"]),
    ("遊戲", ["w", "a", "s", "d", "q", "e", "r", "f",
              "shift", "ctrl", "space"]),
    ("功能鍵", ["f1", "f2", "f3", "f4", "f6", "f7", "f8", "f9", "f10"]),
    ("文字/程式", ["text:Hello from IMU-BLE!", "run:notepad",
                   "run:calc", "run:mspaint"]),
]

# 攤平清單（測試 / CLI 參考用）
PRESETS = [""] + [it for _, items in PRESET_GROUPS for it in items]


class MapError(ValueError):
    """映射規格字串不合法。"""


class KeyMapper:
    """按鍵事件 → 動作。kb/mouse 可注入（測試用），預設延遲載入 pynput。"""

    def __init__(self, kb=None, mouse=None) -> None:
        self._kb = kb
        self._mouse = mouse
        self.enabled = False
        self._map: Dict[int, Tuple] = {}          # key_id -> 已解析動作
        self._held_keys: Dict[int, List] = {}     # 按住中的鍵（供釋放）
        self._held_scroll: Dict[int, float] = {}  # 按住中的滾輪（連發計時）

    # ------------------------------------------------------------ pynput

    def _ensure_backend(self) -> None:
        if self._kb is not None:
            return
        try:
            from pynput.keyboard import Controller as Kb
            from pynput.mouse import Controller as Mouse
        except ImportError:
            raise MapError("手柄控制需要 pynput：pip install pynput")
        self._kb = Kb()
        self._mouse = Mouse()

    def _key_obj(self, token: str):
        """token → pynput 按鍵物件（單字元 or Key.<名稱>）"""
        token = token.strip().lower()
        if len(token) == 1:
            return token
        name = KEY_ALIASES.get(token, token)
        try:
            from pynput.keyboard import Key
        except ImportError:
            return name   # 測試注入模式：無 pynput 時略過名稱驗證
        obj = getattr(Key, name, None)
        if obj is None:
            raise MapError(f"未知按鍵 '{token}'")
        return obj

    def _mouse_btn(self, name: str):
        try:
            from pynput.mouse import Button
        except ImportError:
            return name   # 測試注入模式
        btn = getattr(Button, name, None)
        if btn is None:
            raise MapError(f"未知滑鼠鍵 '{name}'")
        return btn

    # ------------------------------------------------------------ 設定

    def set_mapping(self, key_id: int, spec: str) -> None:
        """設定一顆鍵的映射。規格不合法丟 MapError（原設定保留）。"""
        spec = spec.strip()
        if not spec or spec == "none":
            self._map.pop(key_id, None)
            return

        if spec.startswith("mouse:"):
            what = spec[6:]
            if what == "double":
                self._map[key_id] = ("mouse_double",)
            elif what in ("left", "right", "middle", "x1", "x2"):
                self._map[key_id] = ("mouse", what)
            else:
                raise MapError(f"未知滑鼠動作 '{spec}'")
            return

        if spec.startswith("scroll:"):
            deltas = {"up": (0, SCROLL_STEP), "down": (0, -SCROLL_STEP),
                      "left": (-SCROLL_STEP, 0), "right": (SCROLL_STEP, 0)}
            d = deltas.get(spec[7:])
            if d is None:
                raise MapError(f"未知滾輪方向 '{spec}'")
            self._map[key_id] = ("scroll", d[0], d[1])
            return

        if spec.startswith("text:"):
            if not spec[5:]:
                raise MapError("text: 後面要接文字")
            self._map[key_id] = ("text", spec[5:])
            return

        if spec.startswith("run:"):
            if not spec[4:].strip():
                raise MapError("run: 後面要接程式/命令")
            self._map[key_id] = ("run", spec[4:].strip())
            return

        tokens = [t for t in spec.split("+") if t.strip()]
        if not tokens:
            raise MapError("空的按鍵組合")
        # 立即驗證 token 合法性（需要 pynput 的 Key 表）
        self._ensure_backend()
        objs = [self._key_obj(t) for t in tokens]
        self._map[key_id] = ("keys", objs)

    def mapping_desc(self, key_id: int) -> str:
        return "-" if key_id not in self._map else str(self._map[key_id][0])

    # ------------------------------------------------------------ 執行

    def on_button(self, key_id: int, action: int) -> Optional[str]:
        """處理一筆按鍵回報。@return 有執行動作時回傳描述字串（供記錄）。"""
        if not self.enabled:
            return None
        entry = self._map.get(key_id)
        if entry is None:
            return None
        self._ensure_backend()
        kind = entry[0]

        if kind == "mouse":
            btn = self._mouse_btn(entry[1])
            if action == BTN_PRESS:
                self._mouse.press(btn)
                return f"mouse {entry[1]} down"
            if action == BTN_RELEASE:
                self._mouse.release(btn)
            return None

        if kind == "mouse_double":
            if action == BTN_PRESS:
                self._mouse.click(self._mouse_btn("left"), 2)
                return "double click"
            return None

        if kind == "scroll":
            if action == BTN_PRESS:
                self._mouse.scroll(entry[1], entry[2])
                self._held_scroll[key_id] = time.monotonic()
                return f"scroll ({entry[1]:+d},{entry[2]:+d})"
            if action == BTN_RELEASE:
                self._held_scroll.pop(key_id, None)
            return None

        if kind == "text":
            if action == BTN_PRESS:
                self._kb.type(entry[1])
                return f"type '{entry[1][:24]}'"
            return None

        if kind == "run":
            if action == BTN_PRESS:
                try:
                    subprocess.Popen(entry[1], shell=True)
                except OSError as exc:
                    raise MapError(f"啟動失敗：{exc}")
                return f"run {entry[1]}"
            return None

        if kind == "keys":
            if action == BTN_PRESS:
                for obj in entry[1]:
                    self._kb.press(obj)
                self._held_keys[key_id] = entry[1]
                return "keys down"
            if action == BTN_RELEASE:
                for obj in reversed(self._held_keys.pop(key_id, [])):
                    self._kb.release(obj)
            return None

        return None

    def poll(self) -> None:
        """週期呼叫：滾輪按住連發。"""
        if not self.enabled or not self._held_scroll:
            return
        now = time.monotonic()
        for key_id, last in list(self._held_scroll.items()):
            if (now - last) >= SCROLL_REPEAT_S:
                entry = self._map.get(key_id)
                if entry and entry[0] == "scroll":
                    self._mouse.scroll(entry[1], entry[2])
                self._held_scroll[key_id] = now

    def release_all(self) -> None:
        """安全網：停用/斷線時釋放所有按住中的鍵，避免鍵盤卡死。"""
        if self._kb is not None:
            for objs in self._held_keys.values():
                for obj in reversed(objs):
                    try:
                        self._kb.release(obj)
                    except Exception:
                        pass
        self._held_keys.clear()
        self._held_scroll.clear()
