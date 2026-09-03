#!/usr/bin/env python3
"""VOLT LAND —— 電路板世界的橫向平台跳躍遊戲（美術與角色全原創）。

    python platformer.py        （進入標題畫面後按 Space 開始）

操控（四鍵，可用 IMU-BLE 手柄映射同名鍵盤鍵）：
    ← / →   移動
    Space   跳躍（按越久跳越高；含土狼時間與跳躍緩衝）
    A       按住＝奔跑；火力型態時按下＝射火球

世界觀：小機器人 VOLT 奔跑在一塊巨大的 PCB 上——基板綠地面、
銅箔走線、IC 晶片磚、LED 道具磚、電容塔天際線。
機制：能量菇變大（可碎磚）、火力花射彈跳火球、殼獸踩縮踢殺、
頂磚、金幣 ×100 加命、檢查點、雙關卡。畫面與音效全程式生成。
"""
from __future__ import annotations

import math
import random
import time
from array import array

import pygame

# ------------------------------------------------------------------ 常數

TILE = 48
W, H = 1280, 720
FPS = 60

GRAVITY = 2600.0
JUMP_VY = -880.0
JUMP_HOLD_GRAV = 0.42
WALK_SPD = 250.0
RUN_SPD = 385.0
ACCEL = 1500.0
AIR_ACCEL = 1100.0
DECEL = 1900.0
COYOTE_S = 0.09
BUFFER_S = 0.11
INVULN_S = 2.0
FIREBALL_SPD = 460.0
SHELL_SPD = 520.0

# 調色盤：深夜電路板
SKY_TOP = (10, 16, 34)
SKY_MID = (16, 34, 56)
SKY_BOT = (22, 62, 66)
C_BOARD = (16, 74, 66)          # 基板綠
C_BOARD_DK = (10, 52, 47)
C_COPPER = (214, 138, 66)       # 銅箔
C_GOLD = (240, 190, 92)         # 金手指
C_SILK = (208, 226, 214)        # 絲印白
C_CHIP = (38, 44, 56)           # IC 環氧黑
C_CHIP_HI = (66, 76, 94)
C_LED_BODY = (30, 40, 52)
C_CYAN = (92, 224, 238)         # 主輝光
C_AMBER = (255, 190, 80)
C_MAGENTA = (208, 106, 226)     # 敵人系
C_GREEN = (110, 226, 160)
C_COIN = (255, 207, 77)
C_TEXT = (226, 240, 244)

# ------------------------------------------------------------------ 關卡
# X地面 B磚 ?金幣磚 M道具磚 P平台 c金幣 w走行獸 t殼獸 K檢查點 G終點 S出生

LEVELS = [
    [
        "                                                                                                                        ",
        "                                                                                                                        ",
        "                                                                                                                        ",
        "                                        c c c                                                              cc           ",
        "                  ?                     BBBBB                        B?B                                  PPPP           ",
        "                                                                                        ccc                             ",
        "        S                   B?BMB                    PPP                  K  w   w      PPP        t                 G   ",
        "                                             c c                                                                        ",
        "                     w         w            PPPP        t        BBMB                          BB?BB       w w           ",
        "                                                                                                                        ",
        "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX___XXXXXXXXXXXXXX___XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX____XXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
        "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX___XXXXXXXXXXXXXX___XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX____XXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    ],
    [
        "                                                                                                                        ",
        "                          c                                                                                             ",
        "                         PPP           ccc                                                    ccc                       ",
        "               ?                      BB?BB               M                 B B B                                        ",
        "                                                                                            BBBBB      t t              ",
        "        S            t          w                PPP     PPP     PPP     K          w w                              G   ",
        "                                        t                                                                               ",
        "                   BBBB          BMB           c                                 PPPP        w                          ",
        "                                              PPP                                                                       ",
        "                                                                                                                        ",
        "XXXXXXXXXXXXXXXXXXXX____XXXXXXXXXXXXXXXX____XXXXXX_____XXXXX____XXXXXXXXXXXXXXXXXXXXXXX_____XXXXXXXXXXXXXXXXXXXXXXXXXXX",
        "XXXXXXXXXXXXXXXXXXXX____XXXXXXXXXXXXXXXX____XXXXXX_____XXXXX____XXXXXXXXXXXXXXXXXXXXXXX_____XXXXXXXXXXXXXXXXXXXXXXXXXXX",
    ],
]

SOLID = set("XB?M#")
ONE_WAY = set("P")

# ------------------------------------------------------------------ 字型（含中文）


def make_font(size, bold=False):
    """優先載入系統中文字型（否則畫面中文會變成方框）；找不到才退回英文。"""
    if not pygame.font.get_init():
        pygame.font.init()
    for name in ("microsoftjhenghei", "microsoftyahei", "notosanstc",
                 "notosanscjktc", "pmingliu", "mingliu", "simhei"):
        try:
            path = pygame.font.match_font(name)
        except Exception:
            path = None
        if path:
            f = pygame.font.Font(path, size)
            f.set_bold(bold)
            return f
    return pygame.font.SysFont("arial,dejavusans", size, bold=bold)


# ------------------------------------------------------------------ 音效合成


class Sfx:
    def __init__(self) -> None:
        self.ok = False
        try:
            pygame.mixer.pre_init(22050, -16, 1, 512)
            pygame.mixer.init()
            self.ok = True
        except pygame.error:
            return
        self.bank = {
            "jump":    self._sweep(300, 700, 0.12),
            "stomp":   self._sweep(220, 90, 0.10),
            "coin":    self._tones([(988, 0.05), (1319, 0.10)]),
            "power":   self._tones([(523, .05), (659, .05), (784, .05), (1047, .09)]),
            "shrink":  self._sweep(600, 200, 0.18),
            "fire":    self._sweep(900, 500, 0.07),
            "kick":    self._sweep(500, 750, 0.06),
            "bump":    self._sweep(180, 140, 0.06),
            "break":   self._noise(0.12),
            "die":     self._tones([(660, .1), (494, .1), (392, .18)]),
            "goal":    self._tones([(523, .08), (659, .08), (784, .08),
                                    (1047, .08), (1319, .16)]),
            "1up":     self._tones([(784, .06), (988, .06), (1175, .12)]),
        }

    @staticmethod
    def _pack(samples):
        buf = array("h", (int(max(-1, min(1, s)) * 20000) for s in samples))
        return pygame.mixer.Sound(buffer=buf.tobytes())

    def _sweep(self, f0, f1, dur):
        n = int(22050 * dur)
        out, ph = [], 0.0
        for i in range(n):
            t = i / n
            ph += 2 * math.pi * (f0 + (f1 - f0) * t) / 22050
            out.append((1 if math.sin(ph) > 0 else -1) * 0.4 * (1 - t))
        return self._pack(out)

    def _tones(self, notes):
        out = []
        for f, dur in notes:
            n = int(22050 * dur)
            ph = 0.0
            for i in range(n):
                ph += 2 * math.pi * f / 22050
                env = min(1.0, (n - i) / (n * 0.6))
                out.append((1 if math.sin(ph) > 0 else -1) * 0.35 * env)
        return self._pack(out)

    def _noise(self, dur):
        n = int(22050 * dur)
        return self._pack([random.uniform(-1, 1) * 0.35 * (1 - i / n)
                           for i in range(n)])

    def play(self, name):
        if self.ok:
            self.bank[name].play()


# ------------------------------------------------------------------ 幾何與關卡


class Rect:
    """輕量浮點 AABB（pygame.Rect 為整數，物理會抖）"""

    def __init__(self, x, y, w, h):
        self.x, self.y, self.w, self.h = float(x), float(y), float(w), float(h)

    @property
    def cx(self): return self.x + self.w / 2
    @property
    def bottom(self): return self.y + self.h
    @property
    def right(self): return self.x + self.w

    def overlaps(self, o):
        return (self.x < o.right and self.right > o.x and
                self.y < o.bottom and self.bottom > o.y)


class Level:
    def __init__(self, ascii_map):
        self.rows = [list(r) for r in ascii_map]
        self.h = len(self.rows)
        self.w = max(len(r) for r in self.rows)
        for r in self.rows:
            r.extend(" " * (self.w - len(r)))
        self.spawn = (2 * TILE, 2 * TILE)
        self.checkpoint = None
        self.goal_x = (self.w - 2) * TILE
        self.enemies_init = []
        for gy, row in enumerate(self.rows):
            for gx, ch in enumerate(row):
                if ch == "S":
                    self.spawn = (gx * TILE, gy * TILE)
                    row[gx] = " "
                elif ch in "wt":
                    self.enemies_init.append((gx * TILE, gy * TILE, ch))
                    row[gx] = " "
                elif ch == "G":
                    self.goal_x = gx * TILE
                    row[gx] = " "
                elif ch == "K":
                    row[gx] = "k"

    def at(self, gx, gy):
        if gx < 0 or gx >= self.w:
            return "X"
        if gy < 0 or gy >= self.h:
            return " "
        return self.rows[gy][gx]

    def set(self, gx, gy, ch):
        if 0 <= gx < self.w and 0 <= gy < self.h:
            self.rows[gy][gx] = ch

    def solid_boxes(self, box):
        out = []
        x0, x1 = int(box.x // TILE) - 1, int(box.right // TILE) + 1
        y0, y1 = int(box.y // TILE) - 1, int(box.bottom // TILE) + 1
        for gy in range(y0, y1 + 1):
            for gx in range(x0, x1 + 1):
                ch = self.at(gx, gy)
                if ch in SOLID or ch in ONE_WAY:
                    out.append((Rect(gx * TILE, gy * TILE, TILE, TILE),
                                gx, gy, ch))
        return out


# ------------------------------------------------------------------ 實體


class Fireball:
    def __init__(self, x, y, direction):
        self.box = Rect(x, y, 16, 16)
        self.vx = FIREBALL_SPD * direction
        self.vy = 150.0
        self.alive = True
        self.age = 0.0

    def update(self, dt, level):
        self.age += dt
        self.vy += GRAVITY * dt
        self.box.x += self.vx * dt
        for t, _gx, _gy, ch in level.solid_boxes(self.box):
            if ch in ONE_WAY:
                continue
            if self.box.overlaps(t):
                self.alive = False
                return
        self.box.y += self.vy * dt
        for t, *_g in level.solid_boxes(self.box):
            if self.box.overlaps(t) and self.vy > 0:
                self.box.y = t.y - self.box.h
                self.vy = -520.0
        if self.age > 3.0:
            self.alive = False


class Enemy:
    W_WALK, W_TURTLE = 40, 44

    def __init__(self, x, y, kind):
        self.kind = kind
        w = self.W_WALK if kind == "w" else self.W_TURTLE
        self.box = Rect(x, y, w, w)
        self.vx = -70.0 if kind == "w" else -60.0
        self.vy = 0.0
        self.alive = True
        self.state = "walk"
        self.squash_t = 0.0

    def stomped(self, sfx):
        if self.kind == "w":
            self.alive = False
            self.squash_t = 0.5
            sfx.play("stomp")
            return 100
        if self.state in ("walk", "shell_move"):
            self.state = "shell"
            self.vx = 0.0
            sfx.play("stomp")
            return 100
        return 0

    def kicked(self, direction, sfx):
        self.state = "shell_move"
        self.vx = SHELL_SPD * direction
        sfx.play("kick")

    def update(self, dt, level):
        if not self.alive:
            self.squash_t -= dt
            return
        if self.state == "shell":
            self.vx = 0.0
        self.vy += GRAVITY * dt
        self.box.x += self.vx * dt
        for t, _gx, _gy, ch in level.solid_boxes(self.box):
            if ch in ONE_WAY:
                continue
            if self.box.overlaps(t):
                if self.vx > 0:
                    self.box.x = t.x - self.box.w
                else:
                    self.box.x = t.right
                self.vx = -self.vx
        if self.kind == "t" and self.state == "walk":
            ahead_x = self.box.x - 4 if self.vx < 0 else self.box.right + 4
            below = level.at(int(ahead_x // TILE),
                             int((self.box.bottom + 8) // TILE))
            if below == " ":
                self.vx = -self.vx
        self.box.y += self.vy * dt
        for t, gx, gy, ch in level.solid_boxes(self.box):
            if self.box.overlaps(t) and self.vy > 0 and \
               self.box.bottom - self.vy * dt <= t.y + 6:
                self.box.y = t.y - self.box.h
                self.vy = 0.0
        if self.box.y > level.h * TILE + 200:
            self.alive = False


class Item:
    def __init__(self, x, y, kind):
        self.kind = kind
        self.box = Rect(x + 8, y - 6, 32, 32)
        self.vx = 90.0 if kind == "mush" else 0.0
        self.vy = -220.0
        self.alive = True

    def update(self, dt, level):
        self.vy += GRAVITY * dt
        self.box.x += self.vx * dt
        for t, _gx, _gy, ch in level.solid_boxes(self.box):
            if ch in ONE_WAY:
                continue
            if self.box.overlaps(t):
                self.box.x = t.x - self.box.w if self.vx > 0 else t.right
                self.vx = -self.vx
        self.box.y += self.vy * dt
        for t, *_ in level.solid_boxes(self.box):
            if self.box.overlaps(t) and self.vy > 0:
                self.box.y = t.y - self.box.h
                self.vy = 0.0


class Player:
    SMALL_H, BIG_H = 40, 76
    PW = 34

    def __init__(self, x, y):
        self.box = Rect(x, y, self.PW, self.SMALL_H)
        self.vx = 0.0
        self.vy = 0.0
        self.power = 0
        self.facing = 1
        self.on_ground = False
        self.coyote = 0.0
        self.jump_buf = 0.0
        self.invuln = 0.0
        self.anim = 0.0
        self.dead = False
        self.squash = 1.0          # 擠壓/拉伸（1=正常，<1 扁，>1 高）

    def grow(self, to_power):
        if to_power > self.power and self.power == 0:
            self.box.y -= (self.BIG_H - self.SMALL_H)
            self.box.h = self.BIG_H
        self.power = max(self.power, to_power)

    def shrink(self):
        if self.power > 0:
            self.power = 0
            self.box.y += (self.BIG_H - self.SMALL_H)
            self.box.h = self.SMALL_H
            self.invuln = INVULN_S

    def update(self, dt, inp, level, bumps):
        self.invuln = max(0.0, self.invuln - dt)
        self.coyote = max(0.0, self.coyote - dt)
        self.jump_buf = max(0.0, self.jump_buf - dt)
        self.squash += (1.0 - self.squash) * min(1.0, 12.0 * dt)
        if inp.get("jump_press"):
            self.jump_buf = BUFFER_S

        target = 0.0
        top = RUN_SPD if inp.get("run") else WALK_SPD
        if inp.get("left"):
            target -= top
            self.facing = -1
        if inp.get("right"):
            target += top
            self.facing = 1
        a = (ACCEL if self.on_ground else AIR_ACCEL) if target != 0 else DECEL
        if self.vx < target:
            self.vx = min(target, self.vx + a * dt)
        elif self.vx > target:
            self.vx = max(target, self.vx - a * dt)

        if self.jump_buf > 0 and (self.on_ground or self.coyote > 0):
            self.vy = JUMP_VY
            self.on_ground = False
            self.coyote = 0.0
            self.jump_buf = 0.0
            self.squash = 1.28                      # 起跳拉伸
            bumps.append(("sfx", "jump"))
        g = GRAVITY * (JUMP_HOLD_GRAV if (inp.get("jump_held") and self.vy < 0)
                       else 1.0)
        fall_v = self.vy
        self.vy = min(self.vy + g * dt, 1400.0)

        self.box.x += self.vx * dt
        for t, gx, gy, ch in level.solid_boxes(self.box):
            if ch in ONE_WAY:
                continue
            if self.box.overlaps(t):
                if self.vx > 0:
                    self.box.x = t.x - self.box.w
                else:
                    self.box.x = t.right
                self.vx = 0.0

        prev_bottom = self.box.bottom - self.vy * dt
        self.box.y += self.vy * dt
        grounded = False
        for t, gx, gy, ch in level.solid_boxes(self.box):
            if not self.box.overlaps(t):
                continue
            if ch in ONE_WAY:
                if self.vy > 0 and prev_bottom <= t.y + 4:
                    self.box.y = t.y - self.box.h
                    self.vy = 0.0
                    grounded = True
                continue
            if self.vy > 0 and prev_bottom <= t.y + 8:
                self.box.y = t.y - self.box.h
                self.vy = 0.0
                grounded = True
            elif self.vy < 0:
                self.box.y = t.bottom
                self.vy = 0.0
                bumps.append(("bump", gx, gy, ch))
        if grounded:
            if not self.on_ground and fall_v > 500:
                self.squash = 0.72                  # 著地擠壓
                bumps.append(("land", self.box.cx, self.box.bottom))
            self.on_ground = True
            self.coyote = COYOTE_S
        else:
            if self.on_ground:
                self.coyote = COYOTE_S
            self.on_ground = False

        self.anim += abs(self.vx) * dt / 40.0


# ------------------------------------------------------------------ 遊戲


class Game:
    def __init__(self):
        pygame.init()
        self.screen = pygame.display.set_mode((W, H))
        pygame.display.set_caption("VOLT LAND")
        self.clock = pygame.time.Clock()
        self.font = make_font(24, bold=True)
        self.font_small = make_font(18)
        self.font_big = make_font(80, bold=True)
        self.sfx = Sfx()
        self.state = "title"
        self.level_no = 0
        self.score = 0
        self.coins = 0
        self.lives = 3
        self.shake = 0.0
        self.t = 0.0
        self.particles = []
        self.floats = []
        self.tile_cache = {}
        self.glow_cache = {}
        self.sky = self._make_sky()
        self.vignette = self._make_vignette()
        self.stars = [(random.uniform(0, W * 2), random.uniform(0, H * 0.55),
                       random.uniform(0, math.pi * 2),
                       random.choice((C_CYAN, C_AMBER, C_SILK)))
                      for _ in range(60)]
        self.load_level(0)

    # ---------------------------------------------------------- 預渲染

    @staticmethod
    def _make_sky():
        s = pygame.Surface((1, H))
        for y in range(H):
            t = y / H
            if t < 0.55:
                k = t / 0.55
                c = [SKY_TOP[i] + (SKY_MID[i] - SKY_TOP[i]) * k for i in range(3)]
            else:
                k = (t - 0.55) / 0.45
                c = [SKY_MID[i] + (SKY_BOT[i] - SKY_MID[i]) * k for i in range(3)]
            s.set_at((0, y), tuple(int(v) for v in c))
        return pygame.transform.scale(s, (W, H))

    @staticmethod
    def _make_vignette():
        v = pygame.Surface((W, H), pygame.SRCALPHA)
        for i in range(70):
            a = int(1.6 * i)
            pygame.draw.rect(v, (4, 6, 14, min(a, 110)), (0, 0, W, H), 3,
                             border_radius=0)
            v_rect = pygame.Rect(i * 2, i * 2, W - i * 4, H - i * 4)
            if v_rect.w <= 0 or v_rect.h <= 0:
                break
        # 簡潔作法：四邊漸暗帶
        v.fill((0, 0, 0, 0))
        band = 130
        for i in range(band):
            a = int(90 * (1 - i / band) ** 2)
            pygame.draw.line(v, (3, 5, 12, a), (0, H - 1 - i), (W, H - 1 - i))
            pygame.draw.line(v, (3, 5, 12, a // 2), (0, i), (W, i))
        return v

    def glow(self, r, color):
        key = (r, color)
        if key not in self.glow_cache:
            s = pygame.Surface((r * 4, r * 4), pygame.SRCALPHA)
            for i in range(3, 0, -1):
                rr = int(r * (0.7 + i * 0.42))
                pygame.draw.circle(s, (*color, 22 + 8 * (3 - i)),
                                   (r * 2, r * 2), rr)
            self.glow_cache[key] = s
        return self.glow_cache[key]

    def blit_glow(self, x, y, r, color):
        g = self.glow(r, color)
        self.screen.blit(g, (x - g.get_width() / 2, y - g.get_height() / 2),
                         special_flags=pygame.BLEND_PREMULTIPLIED
                         if False else 0)

    # ---------------------------------------------------------- 關卡流程

    def load_level(self, no):
        self.level_no = no
        self.level = Level(LEVELS[no])
        self.respawn(first=True)
        self.time_left = 300.0

    def respawn(self, first=False):
        pos = self.level.checkpoint if (not first and self.level.checkpoint) \
            else self.level.spawn
        if first:
            self.level.checkpoint = None
        self.player = Player(*pos)
        self.enemies = [Enemy(x, y, k) for x, y, k in self.level.enemies_init]
        self.items = []
        self.fireballs = []
        self.cam_x = max(0.0, self.player.box.cx - W / 3)
        self.death_t = 0.0

    def add_score(self, pts, x, y):
        self.score += pts
        self.floats.append([x, y, f"+{pts}", 0.8])

    def add_coin(self, x, y):
        self.coins += 1
        self.add_score(200, x, y)
        self.sfx.play("coin")
        if self.coins >= 100:
            self.coins -= 100
            self.lives += 1
            self.sfx.play("1up")
            self.floats.append([x, y - 30, "1UP!", 1.2])

    # ---------------------------------------------------------- 更新

    def step(self, dt, inp):
        if self.state != "play":
            return
        self.t += dt
        p = self.player
        self.time_left = max(0.0, self.time_left - dt)
        self.shake = max(0.0, self.shake - dt * 3)

        if p.dead:
            self.death_t += dt
            p.vy += GRAVITY * dt * 0.6
            p.box.y += p.vy * dt
            if self.death_t > 1.6:
                self.lives -= 1
                if self.lives <= 0:
                    self.state = "gameover"
                else:
                    self.respawn()
            return

        bumps = []
        p.update(dt, inp, self.level, bumps)
        for ev in bumps:
            if ev[0] == "sfx":
                self.sfx.play(ev[1])
            elif ev[0] == "bump":
                self.hit_block(ev[1], ev[2], ev[3])
            elif ev[0] == "land":
                self.burst(ev[1], ev[2], (120, 150, 170), n=4, spread=90)

        # A 隨時可射火球（火力型態＝更多顆、飛更快）
        max_fb = 3 if p.power == 2 else 2
        if inp.get("fire_press") and len(self.fireballs) < max_fb:
            fy = p.box.y + (18 if p.power == 0 else 40)   # 依身高調出膛點
            fb = Fireball(p.box.cx + p.facing * 20, fy, p.facing)
            if p.power == 2:
                fb.vx *= 1.35
            self.fireballs.append(fb)
            self.sfx.play("fire")

        # 地圖金幣收集
        for gy in range(int(p.box.y // TILE), int(p.box.bottom // TILE) + 1):
            for gx in range(int(p.box.x // TILE),
                            int(p.box.right // TILE) + 1):
                if self.level.at(gx, gy) == "c":
                    self.level.set(gx, gy, " ")
                    self.add_coin(gx * TILE + TILE / 2, gy * TILE)
                    self.burst(gx * TILE + TILE / 2, gy * TILE + TILE / 2,
                               C_COIN, n=5)

        gx = int(p.box.cx // TILE)
        gy = int((p.box.y + p.box.h / 2) // TILE)
        if self.level.at(gx, gy) == "k":
            self.level.set(gx, gy, "q")     # q = 已點亮的檢查點
            self.level.checkpoint = (gx * TILE, (gy - 1) * TILE)
            self.floats.append([p.box.cx, p.box.y, "CHECKPOINT", 1.2])
            self.sfx.play("power")
        if p.box.x >= self.level.goal_x:
            self.sfx.play("goal")
            self.add_score(int(self.time_left) * 10, p.box.cx, p.box.y)
            if self.level_no + 1 < len(LEVELS):
                self.load_level(self.level_no + 1)
            else:
                self.state = "win"
            return
        if p.box.y > self.level.h * TILE + 60 or self.time_left <= 0:
            self.kill_player()

        for it in self.items:
            it.update(dt, self.level)
            if it.alive and it.box.overlaps(p.box):
                it.alive = False
                self.sfx.play("power")
                self.add_score(1000, it.box.x, it.box.y)
                p.grow(1 if it.kind == "mush" else 2)
        self.items = [i for i in self.items if i.alive]

        for fb in self.fireballs:
            fb.update(dt, self.level)
            for e in self.enemies:
                if e.alive and fb.alive and fb.box.overlaps(e.box):
                    fb.alive = False
                    e.alive = False
                    e.squash_t = 0.0
                    self.add_score(200, e.box.x, e.box.y)
                    self.burst(e.box.cx, e.box.y + 10, C_AMBER)
                    self.sfx.play("kick")
        self.fireballs = [f for f in self.fireballs if f.alive]

        for e in self.enemies:
            e.update(dt, self.level)
            if not e.alive or p.dead:
                continue
            if e.state == "shell_move":
                for o in self.enemies:
                    if o is not e and o.alive and e.box.overlaps(o.box):
                        o.alive = False
                        o.squash_t = 0.0
                        self.add_score(500, o.box.x, o.box.y)
                        self.burst(o.box.cx, o.box.y + 10, C_GREEN)
            if not e.box.overlaps(p.box):
                continue
            falling_on = p.vy > 120 and (p.box.bottom - e.box.y) < 26
            if falling_on:
                pts = e.stomped(self.sfx)
                if pts:
                    self.add_score(pts, e.box.x, e.box.y)
                p.vy = JUMP_VY * 0.55
                p.squash = 1.2
                self.shake = 0.15
            elif e.kind == "t" and e.state == "shell":
                e.kicked(1 if p.box.cx < e.box.cx else -1, self.sfx)
                self.add_score(400, e.box.x, e.box.y)
            elif p.invuln <= 0:
                if p.power > 0:
                    p.shrink()
                    self.sfx.play("shrink")
                else:
                    self.kill_player()
        self.enemies = [e for e in self.enemies if e.alive or e.squash_t > 0]

        for pt in self.particles:
            pt[0] += pt[2] * dt
            pt[1] += pt[3] * dt
            pt[3] += 900 * dt
            pt[4] -= dt
        self.particles = [pt for pt in self.particles if pt[4] > 0]
        for fl in self.floats:
            fl[1] -= 50 * dt
            fl[3] -= dt
        self.floats = [f for f in self.floats if f[3] > 0]

        target = p.box.cx - W / 3 + p.facing * 60
        self.cam_x += (target - self.cam_x) * min(1.0, 5.0 * dt)
        self.cam_x = max(0.0, min(self.cam_x, self.level.w * TILE - W))

    def kill_player(self):
        p = self.player
        if p.dead:
            return
        p.dead = True
        p.vy = -700.0
        self.death_t = 0.0
        self.sfx.play("die")
        self.shake = 0.3

    def hit_block(self, gx, gy, ch):
        x, y = gx * TILE, gy * TILE
        p = self.player
        if ch == "?":
            self.level.set(gx, gy, "#")
            self.add_coin(x + TILE / 2, y)
            self.burst(x + TILE / 2, y, C_COIN)
        elif ch == "M":
            self.level.set(gx, gy, "#")
            kind = "flower" if p.power >= 1 else "mush"
            self.items.append(Item(x, y, kind))
            self.sfx.play("bump")
        elif ch == "B":
            if p.power >= 1:
                self.level.set(gx, gy, " ")
                self.add_score(50, x, y)
                self.burst(x + TILE / 2, y + TILE / 2, C_CHIP_HI, n=10)
                self.sfx.play("break")
                self.shake = 0.12
            else:
                self.sfx.play("bump")

    def burst(self, x, y, color, n=6, spread=350):
        for _ in range(n):
            self.particles.append([
                x, y, random.uniform(-spread * 0.5, spread * 0.5),
                random.uniform(-spread, -spread * 0.25),
                random.uniform(0.3, 0.6), color])

    # ---------------------------------------------------------- 磚面（快取，帶變體）

    def tile_surface(self, ch, variant=0):
        key = (ch, variant)
        if key in self.tile_cache:
            return self.tile_cache[key]
        rng = random.Random(hash(key))
        s = pygame.Surface((TILE, TILE), pygame.SRCALPHA)
        if ch == "X":
            # PCB 基板 + 金手指頂緣 + 銅走線
            s.fill(C_BOARD)
            pygame.draw.rect(s, C_BOARD_DK, (0, 14, TILE, TILE - 14))
            pygame.draw.rect(s, C_GOLD, (0, 0, TILE, 8))
            pygame.draw.rect(s, (255, 226, 150), (0, 0, TILE, 3))
            for _ in range(2):                      # 走線
                y0 = rng.randint(20, TILE - 8)
                x_mid = rng.randint(12, TILE - 12)
                pygame.draw.lines(s, C_COPPER, False,
                                  [(0, y0), (x_mid, y0),
                                   (x_mid, min(TILE - 4, y0 + rng.randint(6, 14)))], 2)
                pygame.draw.circle(s, C_COPPER, (x_mid, y0), 3)
                pygame.draw.circle(s, C_BOARD_DK, (x_mid, y0), 1)
            if rng.random() < 0.5:                  # 焊點
                px, py = rng.randint(8, TILE - 8), rng.randint(24, TILE - 8)
                pygame.draw.circle(s, C_SILK, (px, py), 3)
        elif ch == "B":
            # IC 晶片磚
            s.fill(C_CHIP)
            pygame.draw.rect(s, C_CHIP_HI, (3, 3, TILE - 6, 6))
            pygame.draw.rect(s, (24, 28, 38), (3, TILE - 8, TILE - 6, 5))
            for i in range(4):                      # 側邊接腳
                yy = 8 + i * 10
                pygame.draw.rect(s, C_GOLD, (0, yy, 4, 5))
                pygame.draw.rect(s, C_GOLD, (TILE - 4, yy, 4, 5))
            pygame.draw.circle(s, C_SILK, (12, 14), 3, 1)   # 定位點
            pygame.draw.line(s, (90, 100, 118), (10, TILE - 16),
                             (TILE - 12, TILE - 16), 2)
        elif ch in "?M":
            # LED 道具磚（發光體在 draw 時疊）
            s.fill(C_LED_BODY)
            pygame.draw.rect(s, (52, 66, 82), (0, 0, TILE, 5))
            pygame.draw.rect(s, (18, 24, 34), (0, TILE - 5, TILE, 5))
            for cx, cy in ((7, 7), (TILE - 7, 7), (7, TILE - 7),
                           (TILE - 7, TILE - 7)):
                pygame.draw.circle(s, C_GOLD, (cx, cy), 3)
            color = C_CYAN if ch == "?" else C_AMBER
            pygame.draw.circle(s, color, (TILE // 2, TILE // 2), 11)
            pygame.draw.circle(s, (255, 255, 255), (TILE // 2 - 3,
                                                    TILE // 2 - 3), 4)
        elif ch == "#":
            s.fill((44, 50, 60))
            pygame.draw.rect(s, (30, 34, 44), (0, TILE - 5, TILE, 5))
            pygame.draw.circle(s, (70, 78, 92), (TILE // 2, TILE // 2), 10, 2)
        elif ch == "P":
            # 銅排平台
            pygame.draw.rect(s, C_COPPER, (0, 2, TILE, 12), border_radius=3)
            pygame.draw.rect(s, (255, 200, 130), (0, 2, TILE, 4),
                             border_radius=3)
            pygame.draw.rect(s, (140, 84, 40), (0, 10, TILE, 4))
            pygame.draw.circle(s, C_GOLD, (10, 8), 2)
            pygame.draw.circle(s, C_GOLD, (TILE - 10, 8), 2)
        elif ch in "kq":
            lit = (ch == "q")
            pygame.draw.rect(s, (70, 78, 92), (TILE // 2 - 3, 6, 6, TILE - 6))
            pygame.draw.rect(s, (40, 46, 58), (TILE // 2 - 8, TILE - 6, 16, 6))
            color = C_GREEN if lit else (70, 90, 84)
            pygame.draw.circle(s, color, (TILE // 2, 10), 7)
        self.tile_cache[key] = s
        return s

    # ---------------------------------------------------------- 背景

    def draw_bg(self):
        self.screen.blit(self.sky, (0, 0))
        # 星點（緩慢視差 + 呼吸）
        off = self.cam_x * 0.06
        for sx, sy, ph, color in self.stars:
            x = (sx - off) % (W + 80) - 40
            a = 0.4 + 0.6 * (0.5 + 0.5 * math.sin(self.t * 1.4 + ph))
            pygame.draw.circle(self.screen,
                               tuple(int(c * a) for c in color),
                               (int(x), int(sy)), 2 if ph % 1 < 0.5 else 1)
        # 遠景：電容塔與晶片大樓剪影（兩層視差）
        for depth, (speed, base_y, col) in enumerate(
                [(0.18, H - 150, (13, 30, 46)), (0.38, H - 110, (17, 44, 56))]):
            off = self.cam_x * speed
            rng = random.Random(42 + depth)
            xs = 0
            while xs < W + 400:
                seed_x = int((xs + off) // 240)
                r2 = random.Random(seed_x * 7 + depth * 31)
                bx = xs - (off % 240)
                kind = r2.random()
                hgt = r2.randint(90, 220 - depth * 40)
                if kind < 0.5:      # 電容塔
                    pygame.draw.rect(self.screen, col,
                                     (bx, base_y - hgt, 66, hgt + 150),
                                     border_top_left_radius=14,
                                     border_top_right_radius=14)
                    pygame.draw.line(self.screen,
                                     (col[0] + 14, col[1] + 18, col[2] + 20),
                                     (bx + 12, base_y - hgt + 8),
                                     (bx + 12, base_y), 3)
                else:               # 晶片大樓
                    pygame.draw.rect(self.screen, col,
                                     (bx, base_y - hgt, 96, hgt + 150))
                    for i in range(3):
                        pygame.draw.rect(self.screen, col,
                                         (bx - 6, base_y - hgt + 14 + i * 22,
                                          6, 8))
                        pygame.draw.rect(self.screen, col,
                                         (bx + 96, base_y - hgt + 14 + i * 22,
                                          6, 8))
                    if depth == 1 and r2.random() < 0.7:   # 窗燈
                        wc = C_AMBER if r2.random() < 0.5 else C_CYAN
                        for wy in range(2):
                            pygame.draw.rect(
                                self.screen,
                                tuple(int(c * 0.55) for c in wc),
                                (bx + 18 + r2.randint(0, 40),
                                 base_y - hgt + 20 + wy * 30, 8, 5))
                xs += 240

    # ---------------------------------------------------------- 實體繪製

    def draw_player(self, ox):
        p = self.player
        if p.invuln > 0 and int(p.invuln * 12) % 2 == 0 and not p.dead:
            return
        sq = p.squash
        wd = p.box.w * (2 - sq) ** 0.7
        ht = p.box.h * sq
        x = p.box.cx - wd / 2 - ox
        y = p.box.bottom - ht
        # 落影
        sh_w = wd * (0.9 if p.on_ground else 0.6)
        pygame.draw.ellipse(self.screen, (8, 14, 20),
                            (p.box.cx - sh_w / 2 - ox, p.box.bottom - 4,
                             sh_w, 8))
        body_c = (255, 150, 60) if p.power == 2 else (86, 156, 240)
        edge_c = (30, 46, 70)
        # 機身
        pygame.draw.rect(self.screen, body_c, (x, y + 6, wd, ht - 12),
                         border_radius=12)
        pygame.draw.rect(self.screen, edge_c, (x, y + 6, wd, ht - 12),
                         2, border_radius=12)
        # 面板高光
        pygame.draw.rect(self.screen, (240, 248, 252),
                         (x + 4, y + 10, wd - 8, max(8, ht * 0.30)),
                         border_radius=9)
        # 視鏡（發光眼）
        ex = p.box.cx + p.facing * 7 - ox
        ey = y + 14 + ht * 0.10
        self.screen.blit(self.glow(7, C_CYAN), (ex - 14, ey - 14))
        pygame.draw.circle(self.screen, (16, 26, 40), (int(ex), int(ey)), 6)
        pygame.draw.circle(self.screen, C_CYAN, (int(ex), int(ey)), 3)
        # 天線 + 指示燈
        run_tilt = math.sin(p.anim * math.pi * 2) * 3 if p.on_ground else \
            (-p.vy * 0.008)
        ax = p.box.cx - ox
        tip = (ax + run_tilt, y - 10)
        pygame.draw.line(self.screen, edge_c, (ax, y + 6), tip, 3)
        lamp = C_AMBER if p.power >= 1 else C_GREEN
        self.screen.blit(self.glow(6, lamp), (tip[0] - 12, tip[1] - 12))
        pygame.draw.circle(self.screen, lamp,
                           (int(tip[0]), int(tip[1])), 4)
        # 履帶
        foot = math.sin(p.anim * math.pi * 4) * 4 if p.on_ground else 0
        pygame.draw.rect(self.screen, edge_c,
                         (x + 2 + foot, p.box.bottom - 10, wd - 4, 10),
                         border_radius=5)
        pygame.draw.rect(self.screen, (74, 92, 116),
                         (x + 6 + foot, p.box.bottom - 8, wd - 12, 3),
                         border_radius=2)
        # 噴射（上升時）
        if not p.on_ground and p.vy < -100:
            jy = p.box.bottom + 2
            self.screen.blit(self.glow(9, C_AMBER),
                             (p.box.cx - ox - 18, jy - 14))
            pygame.draw.polygon(self.screen, C_AMBER, [
                (p.box.cx - 7 - ox, jy), (p.box.cx + 7 - ox, jy),
                (p.box.cx - ox + random.uniform(-3, 3), jy + 14)])

    def draw_enemy(self, e, ox):
        x, y = e.box.x - ox, e.box.y
        w = e.box.w
        if not e.alive:
            pygame.draw.ellipse(self.screen, (60, 44, 70),
                                (x, y + w - 12, w, 12))
            return
        pygame.draw.ellipse(self.screen, (8, 14, 20),
                            (x + 3, e.box.bottom - 5, w - 6, 7))
        if e.kind == "w":
            wob = math.sin(self.t * 10 + x * 0.1) * 2
            self.screen.blit(self.glow(10, C_MAGENTA), (x + w / 2 - 20,
                                                        y + w / 2 - 20))
            pygame.draw.ellipse(self.screen, (120, 62, 134),
                                (x, y + 6 + wob, w, w - 6 - wob))
            pygame.draw.ellipse(self.screen, C_MAGENTA,
                                (x + 3, y + 9 + wob, w - 6, (w - 6) * 0.5))
            for s_ in (-1, 1):
                pygame.draw.polygon(self.screen, (90, 40, 104), [
                    (x + w / 2 + s_ * 8, y + 8 + wob),
                    (x + w / 2 + s_ * 16, y - 5 + wob),
                    (x + w / 2 + s_ * 2, y + 3 + wob)])
            for s_ in (-1, 1):
                exx, eyy = x + w / 2 + s_ * 8, y + w / 2 + 2
                pygame.draw.circle(self.screen, (250, 250, 255),
                                   (int(exx), int(eyy)), 6)
                pygame.draw.circle(self.screen, (30, 20, 40),
                                   (int(exx + s_ * 1.5), int(eyy)), 3)
        else:
            moving = e.state == "shell_move"
            shell_c = (232, 186, 84) if moving else (78, 168, 128)
            rim_c = (160, 120, 44) if moving else (44, 110, 84)
            if moving:
                self.screen.blit(self.glow(12, C_AMBER),
                                 (x + w / 2 - 24, y + w / 2 - 24))
            pygame.draw.ellipse(self.screen, shell_c, (x, y + 8, w, w - 8))
            pygame.draw.ellipse(self.screen, rim_c, (x, y + 8, w, w - 8), 3)
            pygame.draw.arc(self.screen, (240, 240, 240),
                            (x + 6, y + 12, w - 12, w - 18),
                            math.pi * 0.15, math.pi * 0.5, 3)
            for i in range(3):                       # 殼上的散熱紋
                pygame.draw.line(self.screen, rim_c,
                                 (x + 8 + i * 10, y + 16),
                                 (x + 12 + i * 10, y + w - 10), 2)
            if e.state == "walk":
                hx = x + (w + 2 if e.vx > 0 else -2)
                pygame.draw.circle(self.screen, (110, 200, 156),
                                   (int(hx), int(y + 16)), 9)
                pygame.draw.circle(self.screen, (20, 40, 30),
                                   (int(hx + (3 if e.vx > 0 else -3)),
                                    int(y + 15)), 2)
                for fx in (x + 8, x + w - 16):
                    pygame.draw.rect(self.screen, (54, 120, 92),
                                     (fx, y + w - 5, 10, 5), border_radius=2)

    def draw_coin(self, x, y, ph):
        wobble = abs(math.sin(self.t * 4 + ph))
        cw = 8 + 12 * wobble
        self.screen.blit(self.glow(9, C_COIN), (x - 18, y - 18))
        pygame.draw.ellipse(self.screen, (170, 120, 30),
                            (x - cw / 2, y - 14, cw, 28))
        pygame.draw.ellipse(self.screen, C_COIN,
                            (x - cw / 2 + 1, y - 13, cw - 2, 26))
        if wobble > 0.6:
            pygame.draw.line(self.screen, (255, 240, 170),
                             (x, y - 8), (x, y + 8), 2)

    # ---------------------------------------------------------- 主繪製

    def draw(self):
        self.draw_bg()
        shake = (random.uniform(-1, 1) * self.shake * 18,
                 random.uniform(-1, 1) * self.shake * 10)
        ox = self.cam_x + shake[0]

        g0, g1 = int(ox // TILE) - 1, int((ox + W) // TILE) + 1
        pulse = 0.5 + 0.5 * math.sin(self.t * 3)
        for gy in range(self.level.h):
            for gx in range(max(0, g0), min(self.level.w, g1)):
                ch = self.level.at(gx, gy)
                if ch in " _":
                    continue
                sx, sy = gx * TILE - ox, gy * TILE + shake[1]
                if ch == "c":
                    self.draw_coin(sx + TILE / 2, sy + TILE / 2,
                                   (gx * 13 + gy * 7) % 10)
                    continue
                variant = (gx * 7 + gy * 13) % 3 if ch == "X" else 0
                self.screen.blit(self.tile_surface(ch, variant), (sx, sy))
                if ch in "?M":                       # 呼吸輝光
                    color = C_CYAN if ch == "?" else C_AMBER
                    g = self.glow(int(10 + 5 * pulse), color)
                    self.screen.blit(g, (sx + TILE / 2 - g.get_width() / 2,
                                         sy + TILE / 2 - g.get_height() / 2))
                elif ch == "q":
                    g = self.glow(9, C_GREEN)
                    self.screen.blit(g, (sx + TILE / 2 - g.get_width() / 2,
                                         sy + 10 - g.get_height() / 2))

        # 終點：訊號天線塔
        gx_ = self.level.goal_x - ox
        base_y = H - 5 * TILE - 24
        pygame.draw.rect(self.screen, (70, 80, 96),
                         (gx_, base_y, 8, 5 * TILE))
        for i in range(3):
            yy = base_y + 26 + i * 42
            pygame.draw.line(self.screen, (70, 80, 96),
                             (gx_ - 12 + i * 4, yy), (gx_ + 20 - i * 4, yy), 3)
        beacon = 0.5 + 0.5 * math.sin(self.t * 5)
        self.screen.blit(self.glow(int(10 + 8 * beacon), C_AMBER),
                         (gx_ + 4 - 24, base_y - 24))
        pygame.draw.circle(self.screen, C_AMBER,
                           (int(gx_ + 4), int(base_y)), 7)

        for it in self.items:
            x, y = it.box.x - ox, it.box.y
            if it.kind == "mush":
                self.screen.blit(self.glow(12, C_GREEN), (x - 8, y - 10))
                pygame.draw.rect(self.screen, (236, 240, 236),
                                 (x + 9, y + 14, 14, 18), border_radius=4)
                pygame.draw.ellipse(self.screen, (94, 206, 148),
                                    (x, y, 32, 22))
                pygame.draw.ellipse(self.screen, (60, 160, 110),
                                    (x, y, 32, 22), 2)
                for dx, dy in ((8, 7), (18, 4), (25, 11)):
                    pygame.draw.circle(self.screen, (222, 250, 234),
                                       (int(x + dx), int(y + dy)), 3)
            else:
                self.screen.blit(self.glow(13, C_AMBER), (x - 6, y - 10))
                pygame.draw.rect(self.screen, (110, 190, 110),
                                 (x + 13, y + 14, 6, 18))
                for a in range(6):
                    ang = a * math.pi / 3 + self.t * 2.4
                    pygame.draw.circle(
                        self.screen, (255, 150, 60),
                        (int(x + 16 + math.cos(ang) * 9),
                         int(y + 10 + math.sin(ang) * 9)), 6)
                pygame.draw.circle(self.screen, (255, 235, 140),
                                   (int(x + 16), int(y + 10)), 6)

        for fb in self.fireballs:
            x, y = fb.box.x - ox + 8, fb.box.y + 8
            self.screen.blit(self.glow(11, C_AMBER), (x - 22, y - 22))
            pygame.draw.circle(self.screen, (255, 120, 40),
                               (int(x), int(y)), 9)
            pygame.draw.circle(self.screen, (255, 224, 130),
                               (int(x - fb.vx * 0.004), int(y - 2)), 5)

        for e in self.enemies:
            self.draw_enemy(e, ox)
        self.draw_player(ox)

        for pt in self.particles:
            life_k = max(0.0, pt[4]) / 0.6
            sz = max(2, int(7 * life_k))
            pygame.draw.rect(self.screen, pt[5],
                             (pt[0] - ox, pt[1], sz, sz), border_radius=2)
        for fx, fy, txt, life in self.floats:
            img = self.font.render(txt, True, C_TEXT)
            img.set_alpha(int(255 * min(1, life * 2)))
            self.screen.blit(img, (fx - ox - img.get_width() / 2, fy - 30))

        self.screen.blit(self.vignette, (0, 0))
        self.draw_hud()

    def draw_hud(self):
        bar = pygame.Surface((W, 52), pygame.SRCALPHA)
        bar.fill((8, 14, 26, 170))
        pygame.draw.line(bar, (*C_CYAN, 70), (0, 51), (W, 51), 2)
        self.screen.blit(bar, (0, 0))

        def label(x, k, v, color=C_TEXT):
            self.screen.blit(self.font_small.render(k, True, (130, 160, 172)),
                             (x, 7))
            self.screen.blit(self.font.render(v, True, color), (x, 22))

        label(24, "SCORE", f"{self.score:07d}")
        label(220, "COIN", f"{self.coins:02d}", C_COIN)
        label(330, "LIFE", f"{self.lives}", C_GREEN)
        label(430, "TIME", f"{int(self.time_left):3d}",
              C_AMBER if self.time_left < 60 else C_TEXT)
        label(540, "WORLD", f"{self.level_no + 1}-{len(LEVELS)}")
        pw = ["", "BIG", "FIRE"][self.player.power]
        if pw:
            label(680, "POWER", pw, C_AMBER)
        hint = "←→ MOVE   SPACE JUMP   A RUN/FIRE"
        img = self.font_small.render(hint, True, (120, 150, 165))
        self.screen.blit(img, (W - img.get_width() - 20, 18))

    def draw_center(self, lines):
        dim = pygame.Surface((W, H), pygame.SRCALPHA)
        dim.fill((6, 10, 22, 150))
        self.screen.blit(dim, (0, 0))
        y = H // 3 - 20
        for text, big in lines:
            f = self.font_big if big else self.font
            img = f.render(text, True, C_TEXT)
            if big:                                  # 標題輝光
                glow_img = f.render(text, True, C_CYAN)
                glow_img.set_alpha(70)
                for dx, dy in ((-3, 0), (3, 0), (0, -3), (0, 3)):
                    self.screen.blit(glow_img,
                                     (W / 2 - img.get_width() / 2 + dx,
                                      y + dy))
            sh = f.render(text, True, (10, 18, 34))
            self.screen.blit(sh, (W / 2 - img.get_width() / 2 + 3, y + 3))
            self.screen.blit(img, (W / 2 - img.get_width() / 2, y))
            y += img.get_height() + 20

    # ---------------------------------------------------------- 主迴圈

    def run(self):
        running = True
        while running:
            dt = min(self.clock.tick(FPS) / 1000.0, 1 / 30)
            self.t += dt if self.state != "play" else 0.0
            inp = {"jump_press": False, "fire_press": False}
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT:
                    running = False
                elif ev.type == pygame.KEYDOWN:
                    if ev.key == pygame.K_ESCAPE:
                        running = False
                    elif ev.key in (pygame.K_SPACE, pygame.K_RETURN):
                        if self.state == "title":
                            self.state = "play"
                        elif self.state in ("gameover", "win"):
                            self.__init__()
                            self.state = "play"
                        if ev.key == pygame.K_SPACE:
                            inp["jump_press"] = True
                    elif ev.key == pygame.K_a:
                        inp["fire_press"] = True
            keys = pygame.key.get_pressed()
            inp.update({
                "left": keys[pygame.K_LEFT],
                "right": keys[pygame.K_RIGHT],
                "jump_held": keys[pygame.K_SPACE],
                "run": keys[pygame.K_a],
            })

            self.step(dt, inp)
            self.draw()
            if self.state == "title":
                self.draw_center([
                    ("VOLT LAND", True),
                    ("電路板世界的小機器人冒險", False),
                    ("", False),
                    ("←→ 移動   Space 跳躍   A 奔跑 / 火球", False),
                    ("按 Space 開始", False)])
            elif self.state == "gameover":
                self.draw_center([("GAME OVER", True),
                                  (f"SCORE {self.score}", False),
                                  ("Space 重新開始", False)])
            elif self.state == "win":
                self.draw_center([("YOU WIN!", True),
                                  (f"SCORE {self.score}", False),
                                  ("Space 再玩一次", False)])
            pygame.display.flip()
        pygame.quit()


def main():
    Game().run()


if __name__ == "__main__":
    main()
