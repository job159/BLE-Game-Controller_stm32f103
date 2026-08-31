#!/usr/bin/env python3
"""類跑跑卡丁車賽車遊戲 —— 用 IMU-BLE 手柄操控。

    python kart_game.py --ble --name HC-42     # BLE 連裝置
    python kart_game.py --port COM5            # Serial 直連
    python kart_game.py                        # 無裝置：純鍵盤模式

操控：
    裝置 roll 傾角  = 方向盤（像握著方向盤左右轉）
    甩尾鍵（按住）  = 漂移：車尾滑出、轉向更兇、同時「集氣」
    氮氣鍵          = 消耗一顆氮氣珠爆發加速（集氣滿一條=一顆，最多存 2）

    鍵盤備援：←/→ 轉向、LShift 甩尾、LCtrl 氮氣
    C = 方向盤置中校正（把目前握持角度設為 0）
    F1 = 重新綁定裝置按鍵（跟著畫面提示按實體鍵）
    R = 重新開始    ESC = 離開

按鍵配置存於 kart_config.json（可直接編輯）。
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time

from protocol import (Attitude, BTN_PRESS, BTN_RELEASE, BtnReport, Commander,
                      FrameParser)

CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "kart_config.json")
DEFAULT_CONFIG = {"drift_key": 2, "nitro_key": 3,
                  "steer_gain": 1.0, "invert_steer": False}

# ------------------------------------------------------------------ 賽道


class Track:
    """封閉賽道：控制點 → Catmull-Rom 平滑取樣 → 中心線折線。"""

    CONTROL = [
        (0, 0), (700, 0), (1150, -90), (1420, -380),
        (1350, -760), (950, -880), (560, -760), (380, -470),
        (-40, -560), (-430, -420), (-560, -40), (-330, 260), (120, 210),
    ]
    HALF_W = 95.0          # 路面半寬
    SAMPLES_PER_SEG = 10

    def __init__(self) -> None:
        self.pts: list[tuple[float, float]] = []
        n = len(self.CONTROL)
        for i in range(n):
            p0 = self.CONTROL[(i - 1) % n]
            p1 = self.CONTROL[i]
            p2 = self.CONTROL[(i + 1) % n]
            p3 = self.CONTROL[(i + 2) % n]
            for s in range(self.SAMPLES_PER_SEG):
                t = s / self.SAMPLES_PER_SEG
                self.pts.append(self._catmull(p0, p1, p2, p3, t))
        self.count = len(self.pts)

    @staticmethod
    def _catmull(p0, p1, p2, p3, t):
        t2, t3 = t * t, t * t * t
        x = 0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t +
                   (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                   (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3)
        y = 0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t +
                   (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                   (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3)
        return (x, y)

    def nearest(self, x: float, y: float) -> tuple[int, float]:
        """@return (最近中心線取樣點索引, 距離)"""
        best_i, best_d2 = 0, float("inf")
        for i, (px, py) in enumerate(self.pts):
            d2 = (px - x) ** 2 + (py - y) ** 2
            if d2 < best_d2:
                best_i, best_d2 = i, d2
        return best_i, math.sqrt(best_d2)

    def start_pose(self) -> tuple[float, float, float]:
        x0, y0 = self.pts[0]
        x1, y1 = self.pts[3]
        return x0, y0, math.atan2(y1 - y0, x1 - x0)


class LapTracker:
    """分區走訪 + 通過起點 → 圈數與計時（防抄近路）。"""

    def __init__(self, track: Track) -> None:
        self.track = track
        self.sectors = max(8, track.count // 10)
        self.reset()

    def reset(self) -> None:
        self.visited: set[int] = set()
        self.lap = 0
        self.lap_start = time.monotonic()
        self.last_lap: float | None = None
        self.best_lap: float | None = None
        self._prev_idx = 0

    def update(self, idx: int, on_road: bool) -> bool:
        """@return True = 剛完成一圈"""
        if on_road:
            self.visited.add(idx * self.sectors // self.track.count)
        completed = False
        # 通過起點區（索引環繞：大 → 小）且走訪率足夠
        if (self._prev_idx > self.track.count * 3 // 4) and \
           (idx < self.track.count // 8):
            if len(self.visited) >= self.sectors * 7 // 10:
                now = time.monotonic()
                self.last_lap = now - self.lap_start
                if (self.best_lap is None) or (self.last_lap < self.best_lap):
                    self.best_lap = self.last_lap
                self.lap += 1
                self.lap_start = now
                completed = True
            self.visited.clear()
        self._prev_idx = idx
        return completed


# ------------------------------------------------------------------ 卡丁車


class Kart:
    """街機風漂移物理：純數學、無 pygame 相依（可單元測試）。"""

    MAX_SPEED = 430.0
    NITRO_MULT = 1.42
    NITRO_TIME = 2.3
    OFFROAD_MULT = 0.42
    GAUGE_FILL_S = 1.6      # 全力漂移集滿一條的秒數
    MAX_CHARGES = 2
    MIN_DRIFT_SPEED = 110.0

    def __init__(self, x: float, y: float, heading: float) -> None:
        self.x, self.y = x, y
        self.heading = heading        # 車頭方向（rad）
        self.vel_dir = heading        # 速度方向（漂移時落後車頭 → 甩尾）
        self.speed = 0.0
        self.drifting = False
        self.gauge = 0.0              # 0..1 集氣條
        self.charges = 0              # 氮氣珠
        self.nitro_t = 0.0            # 氮氣剩餘秒數
        self.gauge_full_flash = 0.0

    @staticmethod
    def _approach_angle(cur: float, target: float, rate: float,
                        dt: float) -> float:
        """指數式追趕：平衡滑移角 = 車頭角速度 / rate。
        （若用線性步進，rate 大於角速度時滑移會歸零 → 甩不出去）"""
        diff = (target - cur + math.pi) % (2 * math.pi) - math.pi
        return cur + diff * min(1.0, rate * dt)

    def try_nitro(self) -> bool:
        if (self.charges > 0) and (self.nitro_t <= 0.0):
            self.charges -= 1
            self.nitro_t = self.NITRO_TIME
            self.speed += 70.0
            return True
        return False

    def update(self, dt: float, steer: float, drift_held: bool,
               on_road: bool) -> None:
        steer = max(-1.0, min(1.0, steer))
        self.nitro_t = max(0.0, self.nitro_t - dt)
        self.gauge_full_flash = max(0.0, self.gauge_full_flash - dt)

        # --- 縱向：速度朝目標收斂 ---
        target = self.MAX_SPEED
        if self.nitro_t > 0.0:
            target *= self.NITRO_MULT
        if not on_road:
            target *= self.OFFROAD_MULT
        rate = 1.1 if (target >= self.speed) else 2.2   # 減速比加速快
        if not on_road:
            rate = 3.0 if target < self.speed else rate
        self.speed += (target - self.speed) * min(1.0, rate * dt)

        # --- 漂移狀態 ---
        self.drifting = drift_held and (self.speed > self.MIN_DRIFT_SPEED)

        # --- 轉向：漂移時角速度更高、抓地更低（車尾滑出） ---
        sf = min(self.speed / 130.0, 1.0)
        yaw_rate = (3.4 if self.drifting else 2.4) * sf
        self.heading += steer * yaw_rate * dt
        # 平衡滑移角 ≈ yaw_rate/grip：漂移滿舵 ~28°、正常行駛 ~7°
        grip = 5.5 if self.drifting else 20.0
        self.vel_dir = self._approach_angle(self.vel_dir, self.heading,
                                            grip, dt)

        # --- 集氣：漂移中且真的在轉才累積 ---
        if self.drifting and (abs(steer) > 0.22):
            self.gauge += dt * (0.35 + 0.75 * abs(steer)) / self.GAUGE_FILL_S
            if self.gauge >= 1.0:
                self.gauge = 0.0
                if self.charges < self.MAX_CHARGES:
                    self.charges += 1
                self.gauge_full_flash = 0.6

        # --- 位移 ---
        self.x += math.cos(self.vel_dir) * self.speed * dt
        self.y += math.sin(self.vel_dir) * self.speed * dt

    @property
    def slip(self) -> float:
        """車頭與速度方向的夾角（甩尾視覺/煙量用）"""
        return abs((self.heading - self.vel_dir + math.pi) %
                   (2 * math.pi) - math.pi)


# ------------------------------------------------------------------ 裝置輸入


class DeviceInput:
    """接收 ATTITUDE(roll→方向盤) 與 BTN(press/release) 的薄封裝。"""

    def __init__(self, transport) -> None:
        self.transport = transport
        self.parser = FrameParser()
        self.roll = 0.0
        self.center = 0.0            # 置中校正偏移
        self.pressed: set[int] = set()
        self.events: list[tuple[int, int]] = []   # (key_id, action)
        self.rx_frames = 0

    def poll(self) -> list[tuple[int, int]]:
        if self.transport is None:
            return []
        data = self.transport.read_nowait()
        if data:
            for frame in self.parser.feed(data):
                msg = frame.decode()
                if isinstance(msg, Attitude):
                    self.roll = msg.roll
                    self.rx_frames += 1
                elif isinstance(msg, BtnReport):
                    if msg.action == BTN_PRESS:
                        self.pressed.add(msg.key_id)
                        self.events.append((msg.key_id, BTN_PRESS))
                    elif msg.action == BTN_RELEASE:
                        self.pressed.discard(msg.key_id)
                        self.events.append((msg.key_id, BTN_RELEASE))
        out = self.events
        self.events = []
        return out

    def steer(self, gain: float, invert: bool) -> float:
        deg = self.roll - self.center
        if abs(deg) < 1.5:            # 死區：手持微晃不觸發
            return 0.0
        s = (deg / 40.0) * gain       # 傾 40° = 滿舵
        if invert:
            s = -s
        return max(-1.0, min(1.0, s))


# ------------------------------------------------------------------ 遊戲主體


def load_config() -> dict:
    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            cfg.update(json.load(f))
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg: dict) -> None:
    try:
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except OSError:
        pass


class Game:
    W, H = 1280, 720

    def __init__(self, transport) -> None:
        import pygame
        self.pg = pygame
        pygame.init()
        pygame.display.set_caption("IMU Kart — 手柄賽車")
        self.screen = pygame.display.set_mode((self.W, self.H))
        self.clock = pygame.time.Clock()
        self.font = pygame.font.SysFont("consolas,arial", 22)
        self.font_big = pygame.font.SysFont("consolas,arial", 64, bold=True)
        self.font_mid = pygame.font.SysFont("consolas,arial", 34, bold=True)

        self.cfg = load_config()
        self.track = Track()
        self.laps = LapTracker(self.track)
        x, y, h = self.track.start_pose()
        self.kart = Kart(x, y, h)
        self.dev = DeviceInput(transport)
        self.cmder = Commander()
        if transport is not None:
            transport.write(self.cmder.set_rate(30))   # 方向盤要順

        self.particles: list[list] = []
        self.countdown_t = 3.2
        self.bind_state: str | None = None   # None/'drift'/'nitro'
        self.msg = ""
        self.msg_t = 0.0
        self.kb_steer = 0.0

    # ---------------------------------------------------------- 輸入

    def _toast(self, text: str, secs: float = 2.0) -> None:
        self.msg, self.msg_t = text, secs

    def handle_input(self, dt: float) -> tuple[float, bool, bool]:
        pg = self.pg
        nitro_pressed = False

        for key_id, action in self.dev.poll():
            if action != BTN_PRESS:
                continue
            if self.bind_state == "drift":
                self.cfg["drift_key"] = key_id
                self.bind_state = "nitro"
                continue
            if self.bind_state == "nitro":
                self.cfg["nitro_key"] = key_id
                self.bind_state = None
                save_config(self.cfg)
                self._toast(f"綁定完成：甩尾=KEY{self.cfg['drift_key']} "
                            f"氮氣=KEY{self.cfg['nitro_key']}")
                continue
            if key_id == self.cfg["nitro_key"]:
                nitro_pressed = True

        for ev in pg.event.get():
            if ev.type == pg.QUIT:
                self.running = False
            elif ev.type == pg.KEYDOWN:
                if ev.key == pg.K_ESCAPE:
                    if self.bind_state:
                        self.bind_state = None
                    else:
                        self.running = False
                elif ev.key == pg.K_LCTRL:
                    nitro_pressed = True
                elif ev.key == pg.K_c:
                    self.dev.center = self.dev.roll
                    self._toast("方向盤已置中")
                elif ev.key == pg.K_r:
                    self.reset_race()
                elif ev.key == pg.K_F1:
                    self.bind_state = "drift"
                elif ev.key == pg.K_LEFTBRACKET:
                    self.cfg["steer_gain"] = max(0.4, self.cfg["steer_gain"] - 0.1)
                    save_config(self.cfg)
                elif ev.key == pg.K_RIGHTBRACKET:
                    self.cfg["steer_gain"] = min(2.5, self.cfg["steer_gain"] + 0.1)
                    save_config(self.cfg)

        keys = pg.key.get_pressed()
        target_kb = (-1.0 if keys[pg.K_LEFT] else 0.0) + \
                    (1.0 if keys[pg.K_RIGHT] else 0.0)
        self.kb_steer += (target_kb - self.kb_steer) * min(1.0, 8.0 * dt)

        steer_dev = self.dev.steer(self.cfg["steer_gain"],
                                   self.cfg["invert_steer"])
        steer = steer_dev if abs(steer_dev) > abs(self.kb_steer) else self.kb_steer

        drift = (self.cfg["drift_key"] in self.dev.pressed) or keys[pg.K_LSHIFT]
        return steer, drift, nitro_pressed

    def reset_race(self) -> None:
        x, y, h = self.track.start_pose()
        self.kart = Kart(x, y, h)
        self.laps.reset()
        self.countdown_t = 3.2
        self.particles.clear()

    # ---------------------------------------------------------- 更新

    def update(self, dt: float, steer: float, drift: bool,
               nitro_pressed: bool) -> None:
        if self.countdown_t > 0.0:
            self.countdown_t -= dt
            if self.countdown_t <= 0.0:
                self.laps.reset()
            return

        if nitro_pressed and self.kart.try_nitro():
            self._toast("NITRO!", 0.8)

        idx, dist = self.track.nearest(self.kart.x, self.kart.y)
        on_road = dist <= Track.HALF_W
        self.kart.update(dt, steer, drift, on_road)
        if self.laps.update(idx, on_road):
            self._toast(f"LAP {self.laps.lap}!  "
                        f"{self.laps.last_lap:.2f}s", 2.5)

        self._spawn_particles(dt)
        self.msg_t = max(0.0, self.msg_t - dt)

    def _spawn_particles(self, dt: float) -> None:
        k = self.kart
        rear_x = k.x - math.cos(k.heading) * 16
        rear_y = k.y - math.sin(k.heading) * 16
        import random
        if k.drifting and (k.slip > 0.08):
            for _ in range(2):
                self.particles.append([
                    rear_x + random.uniform(-6, 6),
                    rear_y + random.uniform(-6, 6),
                    random.uniform(-25, 25), random.uniform(-25, 25),
                    0.55, 0.55, random.uniform(4, 9), (200, 200, 205)])
        if k.nitro_t > 0.0:
            bx = -math.cos(k.heading)
            by = -math.sin(k.heading)
            self.particles.append([
                rear_x, rear_y,
                bx * 220 + random.uniform(-40, 40),
                by * 220 + random.uniform(-40, 40),
                0.3, 0.3, random.uniform(5, 10), (255, 150, 40)])
        for p in self.particles:
            p[0] += p[2] * dt
            p[1] += p[3] * dt
            p[4] -= dt
        self.particles = [p for p in self.particles if p[4] > 0]

    # ---------------------------------------------------------- 繪製

    def world_to_screen(self, x: float, y: float) -> tuple[int, int]:
        return (int(x - self.kart.x + self.W / 2),
                int(y - self.kart.y + self.H / 2))

    def draw(self) -> None:
        pg = self.pg
        s = self.screen
        s.fill((36, 82, 42))                       # 草地

        # 路面：中心線取樣點畫粗圓連成路帶
        pts = self.track.pts
        n = self.track.count
        for i in range(n):
            x, y = self.world_to_screen(*pts[i])
            if -150 < x < self.W + 150 and -150 < y < self.H + 150:
                pg.draw.circle(s, (52, 52, 58), (x, y), int(Track.HALF_W))
        for i in range(n):                          # 邊界紅白路緣
            if i % 2 == 0:
                continue
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % n]
            ang = math.atan2(y1 - y0, x1 - x0)
            for side in (-1, 1):
                ex = x0 + math.cos(ang + side * math.pi / 2) * Track.HALF_W
                ey = y0 + math.sin(ang + side * math.pi / 2) * Track.HALF_W
                sx, sy = self.world_to_screen(ex, ey)
                if 0 <= sx < self.W and 0 <= sy < self.H:
                    color = (220, 60, 60) if (i // 2) % 2 == 0 else (235, 235, 235)
                    pg.draw.circle(s, color, (sx, sy), 5)
        for i in range(0, n, 2):                    # 中線黃虛線
            x, y = self.world_to_screen(*pts[i])
            if 0 <= x < self.W and 0 <= y < self.H:
                pg.draw.circle(s, (210, 190, 60), (x, y), 3)
        # 起跑線
        sx, sy = self.world_to_screen(*pts[0])
        pg.draw.circle(s, (250, 250, 250), (sx, sy), 8, 2)

        # 粒子
        for x, y, _vx, _vy, life, life0, size, color in \
                [tuple(p) for p in self.particles]:
            px, py = self.world_to_screen(x, y)
            r = max(1, int(size * (life / life0)))
            pg.draw.circle(s, color, (px, py), r)

        self._draw_kart()
        self._draw_hud()
        pg.display.flip()

    def _draw_kart(self) -> None:
        pg = self.pg
        k = self.kart
        cx, cy = self.W / 2, self.H / 2

        def rot(dx: float, dy: float) -> tuple[float, float]:
            c, si = math.cos(k.heading), math.sin(k.heading)
            return (cx + dx * c - dy * si, cy + dx * si + dy * c)

        body = [rot(18, 0), rot(-14, -11), rot(-9, 0), rot(-14, 11)]
        wheelf = [rot(10, -12), rot(10, 12)]
        color = (255, 120, 30) if k.nitro_t > 0 else (230, 60, 50)
        for wx, wy in wheelf + [rot(-12, -13), rot(-12, 13)]:
            pg.draw.circle(self.screen, (25, 25, 25), (int(wx), int(wy)), 5)
        pg.draw.polygon(self.screen, color, body)
        pg.draw.polygon(self.screen, (255, 235, 220), [rot(12, 0), rot(4, -5), rot(4, 5)])

    def _draw_hud(self) -> None:
        pg = self.pg
        s = self.screen
        k = self.kart

        # 圈數/計時
        cur = time.monotonic() - self.laps.lap_start if self.countdown_t <= 0 else 0
        lines = [f"LAP {self.laps.lap}", f"TIME {cur:6.2f}"]
        if self.laps.best_lap:
            lines.append(f"BEST {self.laps.best_lap:6.2f}")
        for i, t in enumerate(lines):
            s.blit(self.font_mid.render(t, True, (255, 255, 255)), (20, 16 + i * 36))

        # 速度
        spd = self.font_big.render(f"{int(k.speed / 4)}", True, (255, 255, 255))
        s.blit(spd, (30, self.H - 110))
        s.blit(self.font.render("km/h", True, (200, 200, 200)), (32, self.H - 44))

        # 集氣條（KR 風）
        bar_w, bar_h = 320, 22
        bx, by = self.W // 2 - bar_w // 2, self.H - 56
        pg.draw.rect(s, (30, 30, 30), (bx - 2, by - 2, bar_w + 4, bar_h + 4),
                     border_radius=6)
        fill = int(bar_w * k.gauge)
        flash = k.gauge_full_flash > 0 and int(k.gauge_full_flash * 10) % 2 == 0
        pg.draw.rect(s, (255, 240, 90) if flash else (255, 170, 30),
                     (bx, by, fill, bar_h), border_radius=6)
        s.blit(self.font.render("DRIFT=集氣", True, (230, 230, 230)),
               (bx + bar_w + 14, by - 2))
        for i in range(Kart.MAX_CHARGES):        # 氮氣珠
            cxp = bx - 30 - i * 34
            col = (90, 190, 255) if i < k.charges else (60, 60, 65)
            pg.draw.circle(s, col, (cxp, by + bar_h // 2), 13)
            pg.draw.circle(s, (240, 240, 240), (cxp, by + bar_h // 2), 13, 2)

        # 方向盤指示 + 連線狀態
        wx, wy, wr = self.W - 90, 84, 52
        pg.draw.circle(s, (230, 230, 230), (wx, wy), wr, 3)
        ang = -math.pi / 2 + self.dev.steer(self.cfg["steer_gain"],
                                            self.cfg["invert_steer"]) * 1.2 \
            if self.dev.transport else -math.pi / 2 + self.kb_steer * 1.2
        pg.draw.line(s, (255, 90, 60), (wx, wy),
                     (wx + math.cos(ang) * wr, wy + math.sin(ang) * wr), 5)
        status = "裝置已連線" if self.dev.transport else "鍵盤模式"
        s.blit(self.font.render(
            f"{status}  甩尾=KEY{self.cfg['drift_key']} "
            f"氮氣=KEY{self.cfg['nitro_key']}  增益{self.cfg['steer_gain']:.1f}",
            True, (235, 235, 235)), (self.W - 480, self.H - 34))

        # 倒數 / 提示
        if self.countdown_t > 0:
            n = int(self.countdown_t) + 1 if self.countdown_t > 0.2 else 0
            text = str(n) if n else "GO!"
            img = self.font_big.render(text, True, (255, 230, 80))
            s.blit(img, (self.W // 2 - img.get_width() // 2, self.H // 3))
        if self.bind_state:
            tip = ("請按『甩尾』要用的實體鍵..." if self.bind_state == "drift"
                   else "請按『氮氣』要用的實體鍵...")
            img = self.font_mid.render(tip, True, (120, 220, 255))
            s.blit(img, (self.W // 2 - img.get_width() // 2, self.H // 3))
        elif self.msg_t > 0:
            img = self.font_mid.render(self.msg, True, (255, 240, 120))
            s.blit(img, (self.W // 2 - img.get_width() // 2, self.H // 4))

    # ---------------------------------------------------------- 主迴圈

    def run(self) -> None:
        self.running = True
        while self.running:
            dt = min(self.clock.tick(60) / 1000.0, 0.05)
            steer, drift, nitro = self.handle_input(dt)
            self.update(dt, steer, drift, nitro)
            self.draw()
        if self.dev.transport is not None:
            self.dev.transport.write(self.cmder.set_rate(10))
            self.dev.transport.close()
        self.pg.quit()


def main() -> None:
    ap = argparse.ArgumentParser(description="IMU Kart game")
    tr = ap.add_mutually_exclusive_group()
    tr.add_argument("--port")
    tr.add_argument("--ble", action="store_true")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--name", default="HC-42")
    ap.add_argument("--address")
    args = ap.parse_args()

    transport = None
    if args.port or args.ble:
        from transports import BleTransport, SerialTransport, TransportError
        try:
            if args.port:
                transport = SerialTransport(args.port, args.baud, timeout=0)
            else:
                transport = BleTransport(name=None if args.address else args.name,
                                         address=args.address)
            print(f"已連線：{transport.name}")
        except TransportError as exc:
            print(f"連線失敗（改用鍵盤模式）：{exc}")

    Game(transport).run()


if __name__ == "__main__":
    main()
