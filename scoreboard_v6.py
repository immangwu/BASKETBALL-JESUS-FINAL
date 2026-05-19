#!/usr/bin/env python3
"""
Basketball Scoreboard v6.0
New vs v5.0:
  - Event name 32 char   : thin font, char counter X/32, "Scroll on display" checkbox
  - Team name 15 char    : right-aligned input, X/15 char counter
  - GAME CLOCK label     : bold section header directly above clock display
  - SHOT CLOCK label     : bold section header directly above shot clock display
  - TOL → TIME OUT LEFT  : renamed throughout UI
  - Possession arrow     : font size reduced ~20%
  - Reset All Slaves     : moved to Settings tab (red button)
  - RESET MATCH button   : added in banner row, resets all game state
  - Marketing Display    : Settings tab sub-section, Slave 10 / "Marketing Panel"
  - N packet extended    : N,eventName,teamA,teamB,scrollFlag,marketingText
  - "Slave" word removed : from all user-facing labels/buttons
  - Version string       : PointiQ v6
"""

import sys, platform, os
import serial
import serial.tools.list_ports
from datetime import datetime
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QLineEdit, QGroupBox, QTabWidget, QTextEdit,
    QSpinBox, QComboBox, QFrame, QFormLayout, QSizePolicy, QDialog,
    QGridLayout, QScrollArea, QMessageBox, QSplashScreen, QCheckBox
)
from PyQt5.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt5.QtGui import QFont, QTextCursor, QPixmap, QPainter, QColor

# ─────────────────────────────────────────────────────────────────────────────
# GPIO pin map
# ─────────────────────────────────────────────────────────────────────────────
#  Function            BCM   Physical
#  Game clock START     17      11
#  Game clock STOP      27      13
#  Shot clock 24 s      22      15
#  Shot clock START     23      16
#  Shot clock STOP      24      18
#  Shot clock 14 s      25      22
#  Team A  +1 score      4       7
#  Team A  +2 score      5      29
#  Team A  +3 score      6      31
#  Team A  -1 score     18      12
#  Team A  foul +       12      32
#  Team A  foul -       21      40
#  Team A  possession   10      19
#  Team B  +1 score     13      33
#  Team B  +2 score     16      36
#  Team B  +3 score     19      35
#  Team B  -1 score     20      38
#  Team B  foul +       26      37
#  Team B  foul -        9      21
#  Team B  possession   11      23
#  GND                  —    6/9/14/25/30/34/39  (any ground pin)
#
#  Wiring: one side of each button to GPIO pin, other side to GND.
#  Internal pull-up enabled — button press = FALLING edge.
# ─────────────────────────────────────────────────────────────────────────────
PIN_GCC_START  = 17
PIN_GCC_STOP   = 27
PIN_SCC_24     = 22
PIN_SCC_START  = 23
PIN_SCC_STOP   = 24
PIN_SCC_14     = 25
PIN_A_SCORE1   = 4
PIN_A_SCORE2   = 5
PIN_A_SCORE3   = 6
PIN_A_SCOREM1  = 18
PIN_A_FOUL_P   = 12
PIN_A_FOUL_M   = 21
PIN_A_POSS     = 10
PIN_B_SCORE1   = 13
PIN_B_SCORE2   = 16
PIN_B_SCORE3   = 19
PIN_B_SCOREM1  = 20
PIN_B_FOUL_P   = 26
PIN_B_FOUL_M   = 9
PIN_B_POSS     = 11

# ── NEW pins  (all remaining free GPIO on the 40-pin header) ─────────────────
#  Function              BCM   Physical   Note
#  Quarter +1             7      26        SPI CE1 — safe as GPIO
#  Game clock RESET       8      24        SPI CE0 — safe as GPIO
#  Team A Timeout use     2       3        I2C SDA — safe if I2C not used
#  Team B Timeout use     3       5        I2C SCL — safe if I2C not used
#  YES VISIBLE           14       8        UART TX — needs UART console disabled*
#  START MATCH           15      10        UART RX — needs UART console disabled*
#
#  * To free GPIO 14/15: raspi-config → Interface Options → Serial Port
#    → "login shell over serial" = No  →  "serial hardware" = Yes  → reboot
# ─────────────────────────────────────────────────────────────────────────────
PIN_QTR_P      = 7    # Quarter +1         physical pin 26
PIN_GCC_RESET  = 8    # Game clock RESET   physical pin 24
PIN_A_TO       = 2    # Team A Timeout     physical pin 3
PIN_B_TO       = 3    # Team B Timeout     physical pin 5
PIN_YES_VIS    = 14   # YES VISIBLE        physical pin 8  ⚠ needs UART disabled
PIN_START_MTH  = 15   # START MATCH        physical pin 10 ⚠ needs UART disabled

DEFAULT_QTR_MIN = 10
DEFAULT_SHOT    = 24
MAX_FOULS       = 5
MAX_DOTS        = 3      # maximum timeout dots (Q3/Q4 early)

class GPIOSignals(QObject):
    gcc_start  = pyqtSignal()
    gcc_stop   = pyqtSignal()
    scc_24     = pyqtSignal()
    scc_start  = pyqtSignal()
    scc_stop   = pyqtSignal()
    scc_14     = pyqtSignal()
    a_score1   = pyqtSignal()
    a_score2   = pyqtSignal()
    a_score3   = pyqtSignal()
    a_score_m1 = pyqtSignal()
    a_foul_p   = pyqtSignal()
    a_foul_m   = pyqtSignal()
    a_poss     = pyqtSignal()
    b_score1   = pyqtSignal()
    b_score2   = pyqtSignal()
    b_score3   = pyqtSignal()
    b_score_m1 = pyqtSignal()
    b_foul_p   = pyqtSignal()
    b_foul_m   = pyqtSignal()
    b_poss     = pyqtSignal()
    qtr_p      = pyqtSignal()
    gcc_reset  = pyqtSignal()
    a_to       = pyqtSignal()
    b_to       = pyqtSignal()
    yes_vis    = pyqtSignal()
    start_mth  = pyqtSignal()

import queue as _queue
GPIO_OK  = False
gpio_sig = GPIOSignals()
_gpio_q  = _queue.Queue()   # thread-safe: gpiozero callback → main thread QTimer
_gz_buttons = []            # keep Button objects alive (gpiozero GC safety)

try:
    from gpiozero import Button as _GZButton

    _pin_map = [
        (PIN_GCC_START,  gpio_sig.gcc_start),
        (PIN_GCC_STOP,   gpio_sig.gcc_stop),
        (PIN_SCC_24,     gpio_sig.scc_24),
        (PIN_SCC_START,  gpio_sig.scc_start),
        (PIN_SCC_STOP,   gpio_sig.scc_stop),
        (PIN_SCC_14,     gpio_sig.scc_14),
        (PIN_A_SCORE1,   gpio_sig.a_score1),
        (PIN_A_SCORE2,   gpio_sig.a_score2),
        (PIN_A_SCORE3,   gpio_sig.a_score3),
        (PIN_A_SCOREM1,  gpio_sig.a_score_m1),
        (PIN_A_FOUL_P,   gpio_sig.a_foul_p),
        (PIN_A_FOUL_M,   gpio_sig.a_foul_m),
        (PIN_A_POSS,     gpio_sig.a_poss),
        (PIN_B_SCORE1,   gpio_sig.b_score1),
        (PIN_B_SCORE2,   gpio_sig.b_score2),
        (PIN_B_SCORE3,   gpio_sig.b_score3),
        (PIN_B_SCOREM1,  gpio_sig.b_score_m1),
        (PIN_B_FOUL_P,   gpio_sig.b_foul_p),
        (PIN_B_FOUL_M,   gpio_sig.b_foul_m),
        (PIN_B_POSS,     gpio_sig.b_poss),
        # new pins
        (PIN_QTR_P,      gpio_sig.qtr_p),
        (PIN_GCC_RESET,  gpio_sig.gcc_reset),
        (PIN_A_TO,       gpio_sig.a_to),
        (PIN_B_TO,       gpio_sig.b_to),
        (PIN_YES_VIS,    gpio_sig.yes_vis),
        (PIN_START_MTH,  gpio_sig.start_mth),
    ]
    _ok, _fail = 0, []
    for _pin, _sig in _pin_map:
        try:
            _btn = _GZButton(_pin, pull_up=True, bounce_time=0.05)
            _btn.when_pressed = lambda s=_sig: _gpio_q.put(s)
            _gz_buttons.append(_btn)
            _ok += 1
        except Exception as _pe:
            _fail.append(_pin)
            print(f"[GPIO] Button failed pin {_pin}: {_pe}")

    GPIO_OK = True
    print(f"[GPIO] gpiozero active — {_ok} buttons OK" +
          (f", failed: {_fail}" if _fail else ""))
except Exception as _e:
    print(f"[GPIO] Unavailable ({_e}) — software controls only")


# ═════════════════════════════════════════════════════════════════════════════
#  STYLES
# ═════════════════════════════════════════════════════════════════════════════
BG     = "#0f172a"
CARD   = "#1e293b"
BORDER = "#334155"
TEXT   = "#f1f5f9"
TEXT2  = "#cbd5e1"
TEXT3  = "#94a3b8"
BTN_BG = "#334155"
BTN_HOV= "#3b82f6"
GREEN  = "#4ade80"
AMBER  = "#fbbf24"
ORANGE = "#fb923c"
RED    = "#f87171"
VIOLET = "#c084fc"
BLUE   = "#60a5fa"

DIALOG_STYLE = f"""
    QDialog            {{ background:{BG}; color:{TEXT}; }}
    QWidget            {{ background:{BG}; color:{TEXT}; font-size:14px; }}
    QPushButton        {{ background:{BTN_BG}; border:1px solid {BORDER};
                          color:{TEXT}; border-radius:8px; font-size:14px;
                          min-height:52px; padding:0 12px; }}
    QPushButton:hover  {{ background:{BTN_HOV}; border-color:{BTN_HOV}; color:#fff; }}
    QPushButton:pressed{{ background:#2563eb; }}
    QLabel             {{ color:{TEXT}; }}
    QLineEdit          {{ background:{CARD}; border:2px solid {BORDER};
                          border-radius:8px; color:{TEXT}; padding:8px;
                          font-size:16px; }}
"""


# ═════════════════════════════════════════════════════════════════════════════
#  VIRTUAL KEYBOARD
# ═════════════════════════════════════════════════════════════════════════════
class VirtualKeyboard(QDialog):
    _ROWS = [
        ["1","2","3","4","5","6","7","8","9","0","⌫"],
        ["Q","W","E","R","T","Y","U","I","O","P"],
        ["A","S","D","F","G","H","J","K","L"],
        ["Z","X","C","V","B","N","M",".","-","'"],
    ]
    KEY_W, KEY_H, GAP = 74, 64, 5

    def __init__(self, title="Enter Text", current="", max_len=30, parent=None):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setModal(True)
        self.setWindowFlags(Qt.Dialog | Qt.FramelessWindowHint)
        self.text    = current
        self.max_len = max_len
        self._build(title)

    @staticmethod
    def getText(parent, title="Enter Text", current="", max_len=30):
        kb = VirtualKeyboard(title, current, max_len, parent)
        if kb.exec_() == QDialog.Accepted:
            return kb.text, True
        return current, False

    def _build(self, title):
        self.setStyleSheet(DIALOG_STYLE)
        lo = QVBoxLayout(self)
        lo.setSpacing(6)
        lo.setContentsMargins(16, 16, 16, 16)

        t = QLabel(title)
        t.setStyleSheet(f"color:{AMBER};font-size:18px;font-weight:bold;")
        t.setAlignment(Qt.AlignCenter)
        lo.addWidget(t)

        self.char_lbl = QLabel(f"{len(self.text)} / {self.max_len}")
        self.char_lbl.setAlignment(Qt.AlignCenter)
        self.char_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        lo.addWidget(self.char_lbl)

        self.display = QLineEdit(self.text)
        self.display.setReadOnly(True)
        self.display.setStyleSheet(
            f"background:{CARD};border:2px solid {AMBER};border-radius:8px;"
            f"color:{TEXT};font-size:22px;font-weight:bold;padding:10px;"
            f"min-height:54px;"
        )
        lo.addWidget(self.display)

        widest = max(len(r) for r in self._ROWS)
        for row in self._ROWS:
            rl = QHBoxLayout()
            rl.setSpacing(self.GAP)
            if len(row) < widest:
                rl.addStretch()
            for key in row:
                rl.addWidget(self._make_key(key))
            if len(row) < widest:
                rl.addStretch()
            lo.addLayout(rl)

        br = QHBoxLayout()
        br.setSpacing(self.GAP)
        sp = QPushButton("S P A C E")
        sp.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        sp.setFixedHeight(self.KEY_H)
        sp.setStyleSheet(f"background:#1e3a8a;color:#fff;font-size:14px;"
                         f"font-weight:bold;border-radius:8px;border:none;")
        sp.clicked.connect(lambda: self._insert(" "))
        clr = QPushButton("CLR")
        clr.setFixedSize(100, self.KEY_H)
        clr.setStyleSheet(f"background:#78350f;color:#fef3c7;font-size:14px;"
                          f"font-weight:bold;border-radius:8px;border:none;")
        clr.clicked.connect(lambda: self._set(""))
        can = QPushButton("✕  Cancel")
        can.setFixedSize(140, self.KEY_H)
        can.setStyleSheet(f"background:#7f1d1d;color:#fecaca;font-size:14px;"
                          f"font-weight:bold;border-radius:8px;border:none;")
        can.clicked.connect(self.reject)
        ok = QPushButton("✓  OK")
        ok.setFixedSize(140, self.KEY_H)
        ok.setStyleSheet(f"background:#14532d;color:#bbf7d0;font-size:16px;"
                         f"font-weight:bold;border-radius:8px;border:none;")
        ok.clicked.connect(self.accept)
        br.addWidget(sp); br.addWidget(clr); br.addWidget(can); br.addWidget(ok)
        lo.addLayout(br)

        total_w = widest * self.KEY_W + (widest - 1) * self.GAP + 32
        self.setMinimumWidth(max(total_w, 860))

    def _make_key(self, key):
        btn = QPushButton(key)
        btn.setFont(QFont("Arial", 15, QFont.Bold))
        if key == "⌫":
            btn.setFixedSize(self.KEY_W + 20, self.KEY_H)
            btn.setStyleSheet(f"background:#7f1d1d;color:#fca5a5;border-radius:8px;"
                              f"font-size:18px;border:none;")
            btn.clicked.connect(self._backspace)
        else:
            btn.setFixedSize(self.KEY_W, self.KEY_H)
            btn.setStyleSheet(f"background:{CARD};border:1px solid {BORDER};"
                              f"color:{TEXT};border-radius:8px;font-size:15px;")
            btn.clicked.connect(lambda _, k=key: self._insert(k))
        return btn

    def _insert(self, char):
        if len(self.text) < self.max_len:
            self.text += char
            self.display.setText(self.text)
            self.char_lbl.setText(f"{len(self.text)} / {self.max_len}")

    def _backspace(self):
        self.text = self.text[:-1]
        self.display.setText(self.text)
        self.char_lbl.setText(f"{len(self.text)} / {self.max_len}")

    def _set(self, t):
        self.text = t
        self.display.setText(t)
        self.char_lbl.setText(f"{len(self.text)} / {self.max_len}")


# ═════════════════════════════════════════════════════════════════════════════
#  GAME CLOCK DIALOG  — simplified: only + and − per unit
# ═════════════════════════════════════════════════════════════════════════════
class GameClockDialog(QDialog):
    def __init__(self, current_secs, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Set Game Clock")
        self.setModal(True)
        self.setWindowFlags(Qt.Dialog | Qt.FramelessWindowHint)
        self.setStyleSheet(DIALOG_STYLE)
        self.mins = current_secs // 60
        self.secs = current_secs % 60
        self._build()

    def result_secs(self):
        return self.mins * 60 + self.secs

    def _build(self):
        lo = QVBoxLayout(self)
        lo.setSpacing(12)
        lo.setContentsMargins(24, 24, 24, 24)

        t = QLabel("⏱  SET GAME CLOCK")
        t.setAlignment(Qt.AlignCenter)
        t.setStyleSheet(f"color:{AMBER};font-size:20px;font-weight:bold;")
        lo.addWidget(t)

        self.disp = QLabel()
        self.disp.setAlignment(Qt.AlignCenter)
        self.disp.setStyleSheet(
            f"background:{CARD};border:2px solid {AMBER};border-radius:12px;"
            f"color:{AMBER};font-size:72px;font-weight:bold;padding:12px;"
        )
        lo.addWidget(self.disp)
        self._refresh()

        # +/− adjusters (minutes | seconds)
        adj = QHBoxLayout()
        adj.setSpacing(24)
        adj.addLayout(self._adjuster("MINUTES", self._adj_min, 0, 99))
        sep = QFrame(); sep.setFrameShape(QFrame.VLine)
        sep.setStyleSheet(f"color:{BORDER};")
        adj.addWidget(sep)
        adj.addLayout(self._adjuster("SECONDS", self._adj_sec, 0, 59))
        lo.addLayout(adj)

        # Quick presets: 2, 5, 8, 10 min
        pl = QLabel("QUICK PRESETS")
        pl.setStyleSheet(f"color:{TEXT3};font-size:11px;font-weight:bold;"
                         f"letter-spacing:1px;margin-top:4px;")
        lo.addWidget(pl)
        pr = QHBoxLayout()
        pr.setSpacing(8)
        for label, m in [("2 min", 2), ("5 min", 5), ("8 min", 8), ("10 min", 10)]:
            b = QPushButton(label)
            b.setFixedHeight(56)
            b.setStyleSheet(f"background:#1e3a8a;color:#bfdbfe;font-weight:bold;"
                            f"border-radius:8px;border:none;font-size:16px;")
            b.clicked.connect(lambda _, v=m: self._preset(v))
            pr.addWidget(b)
        lo.addLayout(pr)

        bot = QHBoxLayout()
        bot.setSpacing(10)
        can = QPushButton("✕  Cancel")
        can.setFixedHeight(54)
        can.setStyleSheet(f"background:#7f1d1d;color:#fecaca;font-weight:bold;"
                          f"border-radius:8px;border:none;font-size:16px;")
        can.clicked.connect(self.reject)
        ok = QPushButton("✓  Set Clock")
        ok.setFixedHeight(54)
        ok.setStyleSheet(f"background:#14532d;color:#bbf7d0;font-weight:bold;"
                         f"border-radius:8px;border:none;font-size:16px;")
        ok.clicked.connect(self.accept)
        bot.addWidget(can); bot.addWidget(ok)
        lo.addLayout(bot)
        self.setMinimumWidth(460)

    def _adjuster(self, label, fn, lo_lim, hi_lim):
        vl = QVBoxLayout()
        vl.setSpacing(8)
        lbl = QLabel(label)
        lbl.setAlignment(Qt.AlignCenter)
        lbl.setStyleSheet(f"color:{TEXT3};font-size:13px;font-weight:bold;"
                          f"letter-spacing:1px;")
        vl.addWidget(lbl)
        plus = QPushButton("＋")
        plus.setFixedHeight(80)
        plus.setStyleSheet(f"background:#145a32;color:{TEXT};border-radius:10px;"
                           f"font-size:32px;font-weight:bold;border:none;")
        plus.clicked.connect(lambda: fn(1, lo_lim, hi_lim))
        vl.addWidget(plus)
        minus = QPushButton("－")
        minus.setFixedHeight(80)
        minus.setStyleSheet(f"background:#641e16;color:{TEXT};border-radius:10px;"
                            f"font-size:32px;font-weight:bold;border:none;")
        minus.clicked.connect(lambda: fn(-1, lo_lim, hi_lim))
        vl.addWidget(minus)
        return vl

    def _adj_min(self, d, lo, hi):
        self.mins = max(lo, min(hi, self.mins + d)); self._refresh()

    def _adj_sec(self, d, lo, hi):
        self.secs = max(lo, min(hi, self.secs + d)); self._refresh()

    def _preset(self, m):
        self.mins = m; self.secs = 0; self._refresh()

    def _refresh(self):
        self.disp.setText(f"{self.mins:02d}:{self.secs:02d}")


# ═════════════════════════════════════════════════════════════════════════════
#  SHOT CLOCK DIALOG  (unchanged from v3)
# ═════════════════════════════════════════════════════════════════════════════
class ShotClockDialog(QDialog):
    def __init__(self, current_secs, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Set Shot Clock")
        self.setModal(True)
        self.setWindowFlags(Qt.Dialog | Qt.FramelessWindowHint)
        self.setStyleSheet(DIALOG_STYLE)
        self.secs = current_secs
        self._build()

    def result_secs(self):
        return self.secs

    def _build(self):
        lo = QVBoxLayout(self)
        lo.setSpacing(10)
        lo.setContentsMargins(24, 24, 24, 24)

        t = QLabel("🔴  SET SHOT CLOCK")
        t.setAlignment(Qt.AlignCenter)
        t.setStyleSheet(f"color:{ORANGE};font-size:20px;font-weight:bold;")
        lo.addWidget(t)

        self.disp = QLabel(str(self.secs))
        self.disp.setAlignment(Qt.AlignCenter)
        self.disp.setStyleSheet(
            f"background:{CARD};border:2px solid {ORANGE};border-radius:12px;"
            f"color:{ORANGE};font-size:80px;font-weight:bold;padding:10px;"
            f"min-height:120px;"
        )
        lo.addWidget(self.disp)

        adj = QHBoxLayout()
        adj.setSpacing(8)
        for delta, text, col in [(-10,"−10","#641e16"), (-5,"−5","#7b241c"),
                                   (-1,"−1","#922b21"), (+1,"+1","#145a32"),
                                   (+5,"+5","#1e8449"), (+10,"+10","#239b56")]:
            b = QPushButton(text)
            b.setFixedHeight(60)
            b.setStyleSheet(f"background:{col};color:{TEXT};border-radius:8px;"
                            f"font-size:16px;font-weight:bold;border:none;")
            b.clicked.connect(lambda _, d=delta: self._adj(d))
            adj.addWidget(b)
        lo.addLayout(adj)

        l = QLabel("QUICK PRESETS")
        l.setStyleSheet(f"color:{TEXT3};font-size:11px;font-weight:bold;"
                        f"letter-spacing:1px;margin-top:4px;")
        lo.addWidget(l)
        pr = QHBoxLayout()
        pr.setSpacing(8)
        for v in [8, 14, 24, 30]:
            b = QPushButton(f"{v}s")
            b.setFixedHeight(56)
            b.setStyleSheet(f"background:#7c2d12;color:#fed7aa;font-size:18px;"
                            f"font-weight:bold;border-radius:8px;border:none;")
            b.clicked.connect(lambda _, s=v: self._preset(s))
            pr.addWidget(b)
        lo.addLayout(pr)

        bot = QHBoxLayout()
        bot.setSpacing(10)
        can = QPushButton("✕  Cancel")
        can.setFixedHeight(54)
        can.setStyleSheet(f"background:#7f1d1d;color:#fecaca;font-weight:bold;"
                          f"border-radius:8px;border:none;font-size:16px;")
        can.clicked.connect(self.reject)
        ok = QPushButton("✓  Set Shot Clock")
        ok.setFixedHeight(54)
        ok.setStyleSheet(f"background:#14532d;color:#bbf7d0;font-weight:bold;"
                         f"border-radius:8px;border:none;font-size:16px;")
        ok.clicked.connect(self.accept)
        bot.addWidget(can); bot.addWidget(ok)
        lo.addLayout(bot)
        self.setFixedWidth(440)

    def _adj(self, d):
        self.secs = max(1, min(60, self.secs + d))
        self.disp.setText(str(self.secs))

    def _preset(self, v):
        self.secs = v
        self.disp.setText(str(v))


# ═════════════════════════════════════════════════════════════════════════════
#  MAIN APPLICATION
# ═════════════════════════════════════════════════════════════════════════════
class ScoreboardApp(QMainWindow):

    def __init__(self):
        super().__init__()
        self._init_state()
        self._build_ui()
        self._connect_gpio()
        self.setWindowTitle("PointiQ v6")
        self.showMaximized()
        self._connect_serial()

    # ── State ──────────────────────────────────────────────────────────────
    def _init_state(self):
        self.event_name    = "BASKETBALL 2026"
        self.team_a        = "TEAM A"
        self.team_b        = "TEAM B"
        self.team_names    = f"{self.team_a:<16}{self.team_b:<16}"  # 32-char combined
        self.score_a       = 0
        self.score_b       = 0
        self.fouls_a       = 0
        self.fouls_b       = 0
        self.timeouts_a    = 2
        self.timeouts_b    = 2
        self.timeout_max   = 2        # changes per quarter
        self.quarter       = 1
        self.possession    = "N"
        self.clock_secs    = DEFAULT_QTR_MIN * 60
        self.clock_tenths  = 0
        self.clock_running = False
        self.shot_secs     = DEFAULT_SHOT
        self.shot_tenths   = 0
        self.shot_running  = False
        self.font_score    = 3
        self.font_clock    = 3
        self.font_foul     = 2
        self.font_shot     = 2
        self.qtr_mins      = DEFAULT_QTR_MIN
        self.serial_port   = None
        self.port          = ""
        self.packets_sent  = 0
        # v4 additions
        self.board_confirmed = False   # YES VISIBLE clicked
        self.match_started   = False   # START MATCH clicked
        self.break_running   = False
        self.break_secs      = 0
        self.break_tenths    = 0
        self.break_after_qtr = 0
        self.q4_2min_done    = False
        # v6 additions
        self.event_scroll    = False   # "Scroll on display" checkbox state
        self.marketing_text  = ""      # Marketing Panel (Panel 10) text

        self._timer = QTimer()
        self._timer.setInterval(100)
        self._timer.timeout.connect(self._tick)
        self._timer.start()

    # ── GPIO ───────────────────────────────────────────────────────────────
    def _connect_gpio(self):
        if not GPIO_OK:
            return
        # Map each signal to its action — called from the main thread via _drain_gpio
        self._gpio_actions = {
            gpio_sig.gcc_start:  self._start_clock,
            gpio_sig.gcc_stop:   self._stop_clock,
            gpio_sig.scc_24:     lambda: self._reset_shot(24),
            gpio_sig.scc_start:  self._start_shot,
            gpio_sig.scc_stop:   self._stop_shot,
            gpio_sig.scc_14:     lambda: self._reset_shot(14),
            gpio_sig.a_score1:   lambda: self._add_score("A", 1),
            gpio_sig.a_score2:   lambda: self._add_score("A", 2),
            gpio_sig.a_score3:   lambda: self._add_score("A", 3),
            gpio_sig.a_score_m1: lambda: self._add_score("A", -1),
            gpio_sig.a_foul_p:   lambda: self._delta_fouls("A", 1),
            gpio_sig.a_foul_m:   lambda: self._delta_fouls("A", -1),
            gpio_sig.a_poss:     lambda: self._set_poss("A"),
            gpio_sig.b_score1:   lambda: self._add_score("B", 1),
            gpio_sig.b_score2:   lambda: self._add_score("B", 2),
            gpio_sig.b_score3:   lambda: self._add_score("B", 3),
            gpio_sig.b_score_m1: lambda: self._add_score("B", -1),
            gpio_sig.b_foul_p:   lambda: self._delta_fouls("B", 1),
            gpio_sig.b_foul_m:   lambda: self._delta_fouls("B", -1),
            gpio_sig.b_poss:     lambda: self._set_poss("B"),
            # new buttons
            gpio_sig.qtr_p:      self._next_quarter,
            gpio_sig.gcc_reset:  self._reset_clock,
            gpio_sig.a_to:       lambda: self._add_timeout("A"),
            gpio_sig.b_to:       lambda: self._add_timeout("B"),
            gpio_sig.yes_vis:    self._confirm_board,
            gpio_sig.start_mth:  self._start_match,
        }
        # QTimer polls the thread-safe queue every 30 ms in the main thread
        self._gpio_timer = QTimer()
        self._gpio_timer.timeout.connect(self._drain_gpio)
        self._gpio_timer.start(30)

    def _drain_gpio(self):
        """Called from main thread every 30 ms — drains GPIO button events safely."""
        while not _gpio_q.empty():
            try:
                sig = _gpio_q.get_nowait()
                action = self._gpio_actions.get(sig)
                if action:
                    action()
            except Exception:
                pass

    # ─────────────────────────────────────────────────────────────────────
    # UI BUILD
    # ─────────────────────────────────────────────────────────────────────
    def _build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        vbox = QVBoxLayout(root)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        self.tabs = QTabWidget()
        self.tabs.setStyleSheet(f"""
            QTabWidget::pane  {{ border:none; background:{BG}; }}
            QTabBar::tab      {{ background:{CARD}; color:{TEXT3}; padding:14px 30px;
                                 margin-right:2px; border:1px solid {BORDER};
                                 border-bottom:none; border-top-left-radius:8px;
                                 border-top-right-radius:8px; font-size:14px;
                                 font-weight:bold; }}
            QTabBar::tab:selected {{ background:{BG}; color:{TEXT};
                                     border-bottom:3px solid {BTN_HOV}; }}
            QTabBar::tab:hover    {{ background:#273549; color:{TEXT}; }}
        """)
        self.tabs.addTab(self._make_scoreboard_tab(), "🏀  Scoreboard")
        self.tabs.addTab(self._make_debug_tab(),      "🔧  Debug")
        self.tabs.addTab(self._make_settings_tab(),   "⚙  Settings")
        vbox.addWidget(self.tabs)

        self.setStyleSheet(f"""
            QMainWindow,QWidget {{ background:{BG}; color:{TEXT}; font-size:14px; }}
            QPushButton {{
                background:{BTN_BG}; border:1px solid {BORDER};
                color:{TEXT}; border-radius:8px; font-size:14px;
                min-height:50px; padding:0 10px;
            }}
            QPushButton:hover   {{ background:{BTN_HOV}; border-color:{BTN_HOV}; color:#fff; }}
            QPushButton:pressed {{ background:#2563eb; }}
            QPushButton:checked {{ background:#7c3aed; border-color:#7c3aed; color:#fff; }}
            QLineEdit  {{ background:{CARD}; border:1px solid {BORDER}; padding:8px;
                          border-radius:8px; color:{TEXT}; font-size:14px; }}
            QGroupBox  {{ border:1px solid {BORDER}; border-radius:12px;
                          margin-top:16px; padding-top:14px;
                          color:{BLUE}; font-weight:bold; font-size:14px; }}
            QGroupBox::title {{ subcontrol-origin:margin; left:12px; padding:0 6px; }}
            QComboBox,QSpinBox {{
                background:{CARD}; border:1px solid {BORDER};
                color:{TEXT}; padding:8px; border-radius:8px; font-size:14px;
                min-height:40px;
            }}
            QScrollArea {{ border:none; background:{BG}; }}
            QCheckBox {{ color:{TEXT}; font-size:13px; }}
            QCheckBox::indicator {{ width:18px; height:18px; }}
        """)

    # ── Tab 1: Scoreboard ─────────────────────────────────────────────────
    def _make_scoreboard_tab(self):
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setSpacing(6)
        lo.setContentsMargins(10, 10, 10, 10)

        # ── Banner: YES VISIBLE + START MATCH + RESET MATCH ──────────────
        banner = QHBoxLayout()
        banner.setSpacing(8)

        self.yes_visible_btn = QPushButton("✓  YES VISIBLE — Confirm Boards")
        self.yes_visible_btn.setFixedHeight(54)
        self.yes_visible_btn.setStyleSheet(
            f"background:#78350f;color:#fef3c7;font-size:16px;font-weight:bold;"
            f"border-radius:10px;border:2px solid {AMBER};"
        )
        self.yes_visible_btn.clicked.connect(self._confirm_board)
        banner.addWidget(self.yes_visible_btn, 2)

        self.start_match_btn = QPushButton("🏀  START MATCH")
        self.start_match_btn.setFixedHeight(54)
        self.start_match_btn.setStyleSheet(
            f"background:#14532d;color:#bbf7d0;font-size:16px;font-weight:bold;"
            f"border-radius:10px;border:1px solid {GREEN};"
        )
        self.start_match_btn.clicked.connect(self._start_match)
        banner.addWidget(self.start_match_btn, 1)

        # RESET MATCH button — added right after START MATCH
        reset_match_btn = QPushButton("↺  RESET MATCH")
        reset_match_btn.setFixedHeight(54)
        reset_match_btn.setStyleSheet(
            f"background:#7c2d12;color:#fed7aa;font-size:15px;font-weight:bold;"
            f"border-radius:10px;border:1px solid {ORANGE};"
        )
        reset_match_btn.clicked.connect(self._reset_match)
        banner.addWidget(reset_match_btn, 1)

        min_btn = QPushButton("─")
        min_btn.setFixedSize(54, 54)
        min_btn.setStyleSheet(
            f"background:#1c1917;color:#a8a29e;font-size:20px;font-weight:bold;"
            f"border-radius:10px;border:1px solid #57534e;"
        )
        min_btn.setToolTip("Minimise")
        min_btn.clicked.connect(self.showMinimized)
        banner.addWidget(min_btn)

        max_btn = QPushButton("□")
        max_btn.setFixedSize(54, 54)
        max_btn.setStyleSheet(
            f"background:#1c1917;color:#a8a29e;font-size:18px;font-weight:bold;"
            f"border-radius:10px;border:1px solid #57534e;"
        )
        max_btn.setToolTip("Maximise / Fullscreen")
        def _toggle_max():
            if self.isFullScreen():
                self.showMaximized()
                max_btn.setText("⛶")
            else:
                self.showFullScreen()
                max_btn.setText("□")
        max_btn.clicked.connect(_toggle_max)
        banner.addWidget(max_btn)

        gpio_lbl = QLabel("GPIO: ✓ OK" if GPIO_OK else "GPIO: ✗ OFF")
        gpio_lbl.setFixedHeight(54)
        gpio_lbl.setAlignment(Qt.AlignCenter)
        gpio_lbl.setStyleSheet(
            f"font-size:13px;font-weight:bold;padding:0 10px;"
            f"color:{'#4ade80' if GPIO_OK else '#f87171'};"
        )
        banner.addWidget(gpio_lbl)

        lo.addLayout(banner)

        # ── Top bar: event title + serial ─────────────────────────────────
        top = QHBoxLayout()
        top.setSpacing(8)
        ev_lbl = QLabel("EVENT:")
        ev_lbl.setStyleSheet(f"color:{TEXT3};font-weight:bold;font-size:13px;"
                             f"letter-spacing:1px;min-width:60px;")
        top.addWidget(ev_lbl)

        self.event_edit = QLineEdit(self.event_name)
        self.event_edit.setMaxLength(32)
        # Thin / Light font weight (200) for event name
        event_font = QFont()
        event_font.setWeight(QFont.Light)  # weight 25 maps to Light (closest to 200)
        self.event_edit.setFont(event_font)
        self.event_edit.setStyleSheet(
            f"background:{CARD};border:1px solid {BORDER};color:{TEXT};"
            f"font-size:15px;border-radius:8px;padding:8px;"
            f"min-height:44px;"
        )
        self.event_edit.textChanged.connect(self._on_event_changed)
        top.addWidget(self.event_edit, 2)

        self.event_char_lbl = QLabel(f"{len(self.event_name)}/32")
        self.event_char_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;min-width:40px;")
        top.addWidget(self.event_char_lbl)

        # "Scroll on display" checkbox
        self.event_scroll_chk = QCheckBox("Scroll on display")
        self.event_scroll_chk.setChecked(self.event_scroll)
        self.event_scroll_chk.stateChanged.connect(self._on_event_scroll_changed)
        top.addWidget(self.event_scroll_chk)

        kb_ev = self._kb_btn()
        kb_ev.clicked.connect(self._open_kb_event)
        top.addWidget(kb_ev)
        ok_ev = self._ok_btn()
        ok_ev.clicked.connect(self._send_name)
        top.addWidget(ok_ev)

        top.addSpacing(12)
        self.serial_lbl = QLabel("⚠  Not Connected")
        self.serial_lbl.setStyleSheet(
            f"color:{RED};font-weight:bold;padding:8px 12px;"
            f"background:{CARD};border-radius:8px;font-size:13px;"
        )
        top.addWidget(self.serial_lbl)
        con_btn = QPushButton("📡  Connect")
        con_btn.setFixedHeight(46)
        con_btn.clicked.connect(self._connect_serial)
        top.addWidget(con_btn)
        lo.addLayout(top)

        # ── Team names bar (single 32-char combined input) ────────────────
        tbar = QHBoxLayout()
        tbar.setSpacing(8)
        tn_lbl = QLabel("TEAMS:")
        tn_lbl.setStyleSheet(f"color:{TEXT3};font-weight:bold;font-size:13px;"
                             f"letter-spacing:1px;min-width:60px;")
        tbar.addWidget(tn_lbl)

        self.teams_edit = QLineEdit(self.team_names)
        self.teams_edit.setMaxLength(32)
        teams_font = QFont()
        teams_font.setWeight(QFont.Light)
        self.teams_edit.setFont(teams_font)
        self.teams_edit.setStyleSheet(
            f"background:{CARD};border:1px solid {BORDER};color:{TEXT};"
            f"font-size:15px;border-radius:8px;padding:8px;min-height:44px;"
        )
        self.teams_edit.textChanged.connect(self._on_teams_changed)
        tbar.addWidget(self.teams_edit, 2)

        self.teams_char_lbl = QLabel(f"{len(self.team_names)}/32")
        self.teams_char_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;min-width:40px;")
        tbar.addWidget(self.teams_char_lbl)

        kb_tn = self._kb_btn()
        kb_tn.clicked.connect(self._open_kb_teams)
        tbar.addWidget(kb_tn)
        ok_tn = self._ok_btn()
        ok_tn.clicked.connect(self._send_name)
        tbar.addWidget(ok_tn)
        tbar.addStretch()
        lo.addLayout(tbar)

        # ── 3-column main panel ────────────────────────────────────────────
        cols = QHBoxLayout()
        cols.setSpacing(10)
        cols.addWidget(self._make_team_panel("A"), 1)
        cols.addWidget(self._make_center_panel(), 1)
        cols.addWidget(self._make_team_panel("B"), 1)
        lo.addLayout(cols, 1)

        # ── Status bar ────────────────────────────────────────────────────
        bar = QHBoxLayout()
        self.pkt_lbl = QLabel("Packets Sent: 0")
        self.pkt_lbl.setStyleSheet(f"color:{GREEN};font-weight:bold;font-size:13px;")
        bar.addWidget(self.pkt_lbl)
        bar.addStretch()
        g = QLabel("GPIO: " + ("✓ Hardware Active" if GPIO_OK else "✗ Simulation"))
        g.setStyleSheet(f"color:{'#4ade80' if GPIO_OK else TEXT3};font-size:13px;")
        bar.addWidget(g)
        lo.addLayout(bar)
        return w

    # ── Team panel ─────────────────────────────────────────────────────────
    def _make_team_panel(self, side):
        is_a  = side == "A"
        tname = self.team_a if is_a else self.team_b
        card  = self._card()
        lo    = card.layout()
        lo.setSpacing(8)
        lo.setContentsMargins(14, 14, 14, 14)

        # Team label (static — name is set via the combined TEAMS bar)
        team_hdr = QLabel(f"TEAM {side}")
        team_hdr.setAlignment(Qt.AlignCenter)
        team_hdr.setStyleSheet(
            f"color:{TEXT3};font-size:13px;font-weight:bold;letter-spacing:2px;"
            f"padding:4px 0;"
        )
        lo.addWidget(team_hdr)

        # Score display — centered
        score_lbl = QLabel("0")
        score_lbl.setAlignment(Qt.AlignCenter)
        score_lbl.setStyleSheet(
            f"font-size:84px;font-weight:bold;color:{GREEN};"
            f"background:#0a1628;border-radius:12px;"
            f"min-height:100px;padding:4px;"
        )
        if is_a:
            self.score_a_lbl = score_lbl
        else:
            self.score_b_lbl = score_lbl
        lo.addWidget(score_lbl)

        # +3  +2  +1
        r1 = QHBoxLayout(); r1.setSpacing(6)
        for pts in [3, 2, 1]:
            b = QPushButton(f"+{pts}")
            b.setFixedHeight(62)
            b.setStyleSheet(f"background:#1d4ed8;color:#fff;font-size:20px;"
                            f"font-weight:bold;border-radius:8px;border:none;")
            b.clicked.connect(lambda _, p=pts, s=side: self._add_score(s, p))
            r1.addWidget(b)
        lo.addLayout(r1)

        # −1  |  RST
        r2 = QHBoxLayout(); r2.setSpacing(6)
        m1 = QPushButton("−1")
        m1.setFixedHeight(50)
        m1.setStyleSheet(f"background:#7f1d1d;color:#fca5a5;font-size:16px;"
                         f"font-weight:bold;border-radius:8px;border:none;")
        m1.clicked.connect(lambda _, s=side: self._add_score(s, -1))
        rst = QPushButton("RST Score")
        rst.setFixedHeight(50)
        rst.setStyleSheet(f"background:{BTN_BG};color:{TEXT2};font-size:14px;"
                          f"border-radius:8px;border:1px solid {BORDER};")
        rst.clicked.connect(lambda _, s=side: self._reset_score(s))
        r2.addWidget(m1); r2.addWidget(rst)
        lo.addLayout(r2)

        lo.addWidget(self._hr())

        # Fouls
        fl = QHBoxLayout(); fl.setSpacing(8)
        f_lbl = QLabel("FOULS")
        f_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                            f"letter-spacing:1px;")
        fl.addWidget(f_lbl)
        fv = QLabel("0")
        fv.setStyleSheet(f"font-size:34px;font-weight:bold;color:{RED};"
                         f"min-width:44px;")
        fv.setAlignment(Qt.AlignCenter)
        if is_a:
            self.fouls_a_lbl = fv
        else:
            self.fouls_b_lbl = fv
        fl.addWidget(fv)
        fl.addStretch()
        for text, d in [("+", 1), ("−", -1)]:
            b = QPushButton(text)
            b.setFixedSize(52, 52)
            b.setStyleSheet(
                f"background:{'#14532d' if d > 0 else '#7f1d1d'};"
                f"color:#fff;font-size:20px;font-weight:bold;"
                f"border-radius:8px;border:none;"
            )
            b.clicked.connect(lambda _, s=side, dv=d: self._delta_fouls(s, dv))
            fl.addWidget(b)
        lo.addLayout(fl)

        # Timeouts — label changed from "TOL" / "TIMEOUTS" to "TIME OUT LEFT"
        tl = QHBoxLayout(); tl.setSpacing(8)
        t_lbl = QLabel("TIME OUT LEFT")
        t_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                            f"letter-spacing:1px;")
        tl.addWidget(t_lbl)
        tv = QLabel("2")
        tv.setStyleSheet(f"font-size:34px;font-weight:bold;color:{AMBER};"
                         f"min-width:44px;")
        tv.setAlignment(Qt.AlignCenter)
        if is_a:
            self.to_a_lbl = tv
        else:
            self.to_b_lbl = tv
        tl.addWidget(tv)
        tl.addStretch()
        for text, fn_suffix in [("USE", "use"), ("ADD", "add"), ("RST", "rst")]:
            b = QPushButton(text)
            b.setFixedHeight(52)
            cols_map  = {"USE": "#92400e", "ADD": "#14532d", "RST": BTN_BG}
            text_map  = {"USE": "#fef3c7", "ADD": "#bbf7d0", "RST": TEXT2}
            b.setStyleSheet(
                f"background:{cols_map[text]};color:{text_map[text]};"
                f"font-size:13px;font-weight:bold;border-radius:8px;border:none;"
                f"min-width:52px;"
            )
            b.clicked.connect(
                lambda _, s=side, f=fn_suffix:
                    self._use_timeout(s) if f == "use"
                    else self._add_timeout(s) if f == "add"
                    else self._reset_timeouts(s)
            )
            tl.addWidget(b)
        lo.addLayout(tl)

        # Timeout dots — MAX_DOTS = 3; show only timeout_max at a time
        dot_row = QHBoxLayout()
        dot_row.setAlignment(Qt.AlignCenter)
        dot_row.setSpacing(8)
        dots = []
        for i in range(MAX_DOTS):
            d = QLabel("●")
            d.setStyleSheet(f"font-size:26px;color:{AMBER};")
            d.setVisible(i < self.timeout_max)
            dots.append(d)
            dot_row.addWidget(d)
        if is_a:
            self.to_a_dots = dots
        else:
            self.to_b_dots = dots
        dw = QWidget(); dw.setLayout(dot_row)
        lo.addWidget(dw)

        return card

    # ── Center panel ───────────────────────────────────────────────────────
    def _make_center_panel(self):
        card = self._card()
        lo   = card.layout()
        lo.setSpacing(8)
        lo.setContentsMargins(14, 14, 14, 14)

        # ── GAME CLOCK ────────────────────────────────────────────────────
        # Bold "GAME CLOCK" label directly above the clock display
        gc_hdr = QLabel("GAME CLOCK")
        gc_hdr.setStyleSheet(
            f"font-size:11px; font-weight:bold; color:{TEXT3}; letter-spacing:2px;"
        )
        lo.addWidget(gc_hdr)

        clk_row = QHBoxLayout(); clk_row.setSpacing(8)
        self.clock_lbl = QLabel(self._fmt_clock())
        self.clock_lbl.setAlignment(Qt.AlignCenter)
        self.clock_lbl.setStyleSheet(
            f"font-size:58px;font-weight:bold;color:{AMBER};"
            f"background:#0a1628;border-radius:10px;"
            f"min-width:180px;padding:4px 12px;"
        )
        clk_row.addWidget(self.clock_lbl, 1)
        set_clk = QPushButton("✏  SET")
        set_clk.setFixedSize(80, 58)
        set_clk.setStyleSheet(f"background:#78350f;color:#fef3c7;font-size:13px;"
                              f"font-weight:bold;border-radius:8px;border:none;")
        set_clk.clicked.connect(self._open_clock_dialog)
        clk_row.addWidget(set_clk)
        lo.addLayout(clk_row)

        gc = QHBoxLayout(); gc.setSpacing(8)
        self.clk_start_btn = QPushButton("▶  START")
        self.clk_start_btn.setFixedHeight(56)
        self.clk_start_btn.setStyleSheet(f"background:#166534;color:#bbf7d0;"
                                         f"font-size:16px;font-weight:bold;"
                                         f"border-radius:8px;border:none;")
        self.clk_start_btn.clicked.connect(self._start_clock)
        self.clk_stop_btn = QPushButton("■  STOP")
        self.clk_stop_btn.setFixedHeight(56)
        self.clk_stop_btn.setStyleSheet(f"background:#991b1b;color:#fecaca;"
                                        f"font-size:16px;font-weight:bold;"
                                        f"border-radius:8px;border:none;")
        self.clk_stop_btn.clicked.connect(self._stop_clock)
        gc.addWidget(self.clk_start_btn); gc.addWidget(self.clk_stop_btn)
        lo.addLayout(gc)

        rst_clk = QPushButton("↺  Reset Clock to Quarter Time")
        rst_clk.setFixedHeight(46)
        rst_clk.setStyleSheet(f"background:{BTN_BG};color:{TEXT2};font-size:13px;"
                              f"border-radius:8px;border:1px solid {BORDER};")
        rst_clk.clicked.connect(self._reset_clock)
        lo.addWidget(rst_clk)

        # Break display (hidden when no break running)
        self.break_frame = QFrame()
        self.break_frame.setStyleSheet(
            f"background:#1c1517;border:2px solid {AMBER};border-radius:10px;"
        )
        bf = QVBoxLayout(self.break_frame)
        bf.setContentsMargins(10, 8, 10, 8)
        self.break_title_lbl = QLabel("BREAK")
        self.break_title_lbl.setAlignment(Qt.AlignCenter)
        self.break_title_lbl.setStyleSheet(
            f"color:{AMBER};font-size:13px;font-weight:bold;letter-spacing:2px;"
        )
        self.break_count_lbl = QLabel("0:00")
        self.break_count_lbl.setAlignment(Qt.AlignCenter)
        self.break_count_lbl.setStyleSheet(
            f"color:{AMBER};font-size:40px;font-weight:bold;"
        )
        bf.addWidget(self.break_title_lbl)
        bf.addWidget(self.break_count_lbl)
        self.break_frame.setVisible(False)
        lo.addWidget(self.break_frame)

        lo.addWidget(self._hr())

        # ── SHOT CLOCK ────────────────────────────────────────────────────
        # Bold "SHOT CLOCK" label directly above the shot clock display
        sc_hdr = QLabel("SHOT CLOCK")
        sc_hdr.setStyleSheet(
            f"font-size:11px; font-weight:bold; color:{TEXT3}; letter-spacing:2px;"
        )
        lo.addWidget(sc_hdr)

        sh_row = QHBoxLayout(); sh_row.setSpacing(8)
        self.shot_lbl = QLabel(self._fmt_shot())
        self.shot_lbl.setAlignment(Qt.AlignCenter)
        self.shot_lbl.setStyleSheet(
            f"font-size:52px;font-weight:bold;color:{ORANGE};"
            f"background:#0a1628;border-radius:10px;"
            f"min-width:140px;padding:4px 12px;"
        )
        sh_row.addWidget(self.shot_lbl, 1)
        set_sh = QPushButton("✏  SET")
        set_sh.setFixedSize(80, 52)
        set_sh.setStyleSheet(f"background:#7c2d12;color:#fed7aa;font-size:13px;"
                             f"font-weight:bold;border-radius:8px;border:none;")
        set_sh.clicked.connect(self._open_shot_dialog)
        sh_row.addWidget(set_sh)
        lo.addLayout(sh_row)

        sr1 = QHBoxLayout(); sr1.setSpacing(8)
        b24 = QPushButton("RESET  24s")
        b24.setFixedHeight(54)
        b24.setStyleSheet(f"background:#c2410c;color:#fff;font-size:15px;"
                          f"font-weight:bold;border-radius:8px;border:none;")
        b24.clicked.connect(lambda: self._reset_shot(24))
        b14 = QPushButton("RESET  14s")
        b14.setFixedHeight(54)
        b14.setStyleSheet(f"background:#9a3412;color:#fff;font-size:15px;"
                          f"font-weight:bold;border-radius:8px;border:none;")
        b14.clicked.connect(lambda: self._reset_shot(14))
        sr1.addWidget(b24); sr1.addWidget(b14)
        lo.addLayout(sr1)

        sr2 = QHBoxLayout(); sr2.setSpacing(8)
        self.shot_start_btn = QPushButton("▶")
        self.shot_start_btn.setFixedHeight(50)
        self.shot_start_btn.setStyleSheet(f"background:#166534;color:#bbf7d0;"
                                          f"font-size:20px;font-weight:bold;"
                                          f"border-radius:8px;border:none;")
        self.shot_start_btn.clicked.connect(self._start_shot)
        self.shot_stop_btn = QPushButton("■")
        self.shot_stop_btn.setFixedHeight(50)
        self.shot_stop_btn.setStyleSheet(f"background:#991b1b;color:#fecaca;"
                                         f"font-size:20px;font-weight:bold;"
                                         f"border-radius:8px;border:none;")
        self.shot_stop_btn.clicked.connect(self._stop_shot)
        sr2.addWidget(self.shot_start_btn); sr2.addWidget(self.shot_stop_btn)
        lo.addLayout(sr2)

        lo.addWidget(self._hr())

        # ── QUARTER ───────────────────────────────────────────────────────
        qr = QHBoxLayout(); qr.setSpacing(6)
        ql = QLabel("QTR:")
        ql.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                         f"letter-spacing:1px;")
        qr.addWidget(ql)
        self.qtr_lbl = QLabel("1")
        self.qtr_lbl.setStyleSheet(f"font-size:28px;font-weight:bold;color:{VIOLET};"
                                   f"min-width:40px;")
        qr.addWidget(self.qtr_lbl)
        for label, v in [("Q1", 1), ("Q2", 2), ("Q3", 3), ("Q4", 4), ("OT", 5)]:
            b = QPushButton(label)
            b.setFixedHeight(46)
            b.setStyleSheet(f"font-size:13px;font-weight:bold;")
            b.clicked.connect(lambda _, x=v: self._set_quarter(x))
            qr.addWidget(b)
        lo.addLayout(qr)

        nq = QPushButton("⏭  Next Quarter")
        nq.setFixedHeight(50)
        nq.setStyleSheet(f"background:#4c1d95;color:#ddd6fe;font-size:13px;"
                         f"font-weight:bold;border-radius:8px;border:none;")
        nq.clicked.connect(self._next_quarter)
        lo.addWidget(nq)

        lo.addWidget(self._hr())

        # ── POSSESSION ────────────────────────────────────────────────────
        lo.addWidget(self._section_lbl("POSSESSION"))
        pr = QHBoxLayout(); pr.setSpacing(8)
        # Possession arrow buttons with font size reduced ~20%
        # Original was default button font (~14px); reduced to ~11px
        self.poss_a_btn    = QPushButton("◀  TEAM A")
        self.poss_none_btn = QPushButton("—  NONE  —")
        self.poss_b_btn    = QPushButton("TEAM B  ▶")
        for b in [self.poss_a_btn, self.poss_none_btn, self.poss_b_btn]:
            b.setCheckable(True)
            b.setFixedHeight(50)
            b.setStyleSheet(
                f"font-size:11px;font-weight:bold;"  # ~20% smaller than default 14px
            )
        self.poss_none_btn.setChecked(True)
        self.poss_a_btn.clicked.connect(lambda: self._set_poss("A"))
        self.poss_none_btn.clicked.connect(lambda: self._set_poss("N"))
        self.poss_b_btn.clicked.connect(lambda: self._set_poss("B"))
        pr.addWidget(self.poss_a_btn)
        pr.addWidget(self.poss_none_btn)
        pr.addWidget(self.poss_b_btn)
        lo.addLayout(pr)

        lo.addStretch()
        return card

    # ── Tab 2: Debug ───────────────────────────────────────────────────────
    def _make_debug_tab(self):
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setContentsMargins(12, 12, 12, 12)

        t = QLabel("🔧  Debug Console")
        t.setStyleSheet(f"font-size:20px;font-weight:bold;color:{BLUE};padding:6px 0;")
        lo.addWidget(t)

        cards = QHBoxLayout(); cards.setSpacing(10)

        c1 = self._card()
        self.dbg_serial   = QLabel("❌  Not Connected")
        self.dbg_serial.setStyleSheet(f"font-size:14px;font-weight:bold;color:{RED};")
        self.dbg_port_lbl = QLabel("Port: —")
        self.dbg_port_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        c1.layout().addWidget(QLabel("SERIAL"))
        c1.layout().itemAt(0).widget().setStyleSheet(
            f"color:{TEXT3};font-size:11px;font-weight:bold;letter-spacing:1px;")
        c1.layout().addWidget(self.dbg_serial)
        c1.layout().addWidget(self.dbg_port_lbl)
        cards.addWidget(c1)

        c2 = self._card()
        self.dbg_pkt_cnt  = QLabel("Sent: 0")
        self.dbg_pkt_cnt.setStyleSheet(f"font-size:14px;font-weight:bold;color:{GREEN};")
        self.dbg_last_pkt = QLabel("—")
        self.dbg_last_pkt.setWordWrap(True)
        self.dbg_last_pkt.setStyleSheet(f"color:{TEXT3};font-size:11px;")
        c2.layout().addWidget(QLabel("PACKETS"))
        c2.layout().itemAt(0).widget().setStyleSheet(
            f"color:{TEXT3};font-size:11px;font-weight:bold;letter-spacing:1px;")
        c2.layout().addWidget(self.dbg_pkt_cnt)
        c2.layout().addWidget(self.dbg_last_pkt)
        cards.addWidget(c2)

        c3 = self._card()
        self.dbg_clock = QLabel("⏱  Stopped")
        self.dbg_clock.setStyleSheet(f"font-size:14px;font-weight:bold;color:{ORANGE};")
        self.dbg_shot  = QLabel("🔴  Shot: Stopped")
        self.dbg_shot.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        c3.layout().addWidget(QLabel("CLOCK STATE"))
        c3.layout().itemAt(0).widget().setStyleSheet(
            f"color:{TEXT3};font-size:11px;font-weight:bold;letter-spacing:1px;")
        c3.layout().addWidget(self.dbg_clock)
        c3.layout().addWidget(self.dbg_shot)
        cards.addWidget(c3)

        lo.addLayout(cards)

        lm = QLabel("LIVE PACKET MONITOR")
        lm.setStyleSheet(f"color:{TEXT3};font-size:11px;font-weight:bold;"
                         f"letter-spacing:1px;margin-top:8px;")
        lo.addWidget(lm)

        self.console = QTextEdit()
        self.console.setReadOnly(True)
        self.console.setStyleSheet(f"""
            QTextEdit {{
                background:#0d1117; border:1px solid {BORDER};
                border-radius:8px; color:#c9d1d9;
                font-family:'Courier New',monospace;
                font-size:12px; padding:10px;
            }}
        """)
        lo.addWidget(self.console, 1)

        br = QHBoxLayout(); br.setSpacing(8)
        for label, fn in [("🗑  Clear Console", self.console.clear),
                          ("📤  Send Test S Packet", self._send_score),
                          ("🔄  Reconnect Serial", self._connect_serial)]:
            b = QPushButton(label)
            b.setFixedHeight(48)
            b.clicked.connect(fn)
            br.addWidget(b)
        br.addStretch()
        lo.addLayout(br)
        return w

    # ── Tab 3: Settings ────────────────────────────────────────────────────
    def _make_settings_tab(self):
        outer = QScrollArea()
        outer.setWidgetResizable(True)
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setContentsMargins(20, 20, 20, 20)
        lo.setSpacing(14)
        outer.setWidget(w)

        t = QLabel("⚙  System Settings")
        t.setStyleSheet(f"font-size:20px;font-weight:bold;color:{BLUE};padding:6px 0;")
        lo.addWidget(t)

        # Serial
        sg = QGroupBox("Serial Port")
        sf = QFormLayout(sg)
        sf.setSpacing(10)
        self.port_combo = QComboBox()
        self._refresh_ports()
        ref = QPushButton("🔄"); ref.setFixedSize(46, 46)
        ref.clicked.connect(self._refresh_ports)
        ph = QHBoxLayout()
        ph.addWidget(self.port_combo, 1); ph.addWidget(ref)
        sf.addRow("Port:", ph)
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["9600","19200","38400","57600","115200"])
        self.baud_combo.setCurrentText("115200")
        sf.addRow("Baud Rate:", self.baud_combo)
        ap = QPushButton("✓  Apply and Connect")
        ap.setFixedHeight(52)
        ap.setStyleSheet(f"background:#166534;color:#bbf7d0;font-weight:bold;"
                         f"border-radius:8px;border:none;font-size:15px;")
        ap.clicked.connect(self._apply_serial)
        sf.addRow("", ap)

        # detected platform
        plat = "Raspberry Pi" if platform.machine().startswith("arm") or \
               platform.machine().startswith("aarch") else platform.system()
        pf_lbl = QLabel(f"Platform detected: {plat}  |  Python {sys.version.split()[0]}"
                        f"  |  GPIO: {'Active' if GPIO_OK else 'Inactive'}")
        pf_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;padding:6px 0;")
        sf.addRow("", pf_lbl)
        lo.addWidget(sg)

        # Game settings
        gg = QGroupBox("Game Settings")
        gf = QFormLayout(gg)
        gf.setSpacing(10)
        self.qtr_spin = QSpinBox()
        self.qtr_spin.setRange(1, 30)
        self.qtr_spin.setValue(self.qtr_mins)
        self.qtr_spin.setSuffix(" min")
        self.qtr_spin.valueChanged.connect(lambda v: setattr(self, "qtr_mins", v))
        gf.addRow("Quarter Duration:", self.qtr_spin)
        lo.addWidget(gg)

        # ── Marketing Display (Panel 10) ───────────────────────────────────
        mg = QGroupBox("Marketing Display (Panel 10)")
        mf = QVBoxLayout(mg)
        mf.setSpacing(10)

        ml_desc = QLabel("Text shown on the marketing display panel (max 20 chars).")
        ml_desc.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        mf.addWidget(ml_desc)

        mrow = QHBoxLayout()
        mrow.setSpacing(8)
        self.marketing_edit = QLineEdit(self.marketing_text)
        self.marketing_edit.setMaxLength(20)
        self.marketing_edit.setPlaceholderText("Enter marketing text…")
        self.marketing_edit.setStyleSheet(
            f"background:{CARD};border:1px solid {BORDER};color:{TEXT};"
            f"font-size:14px;border-radius:8px;padding:8px;min-height:44px;"
        )
        self.marketing_edit.textChanged.connect(self._on_marketing_changed)
        mrow.addWidget(self.marketing_edit, 1)

        self.marketing_char_lbl = QLabel(f"{len(self.marketing_text)}/20")
        self.marketing_char_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;min-width:40px;")
        mrow.addWidget(self.marketing_char_lbl)

        send_mkt_btn = QPushButton("Send")
        send_mkt_btn.setFixedHeight(44)
        send_mkt_btn.setStyleSheet(
            f"background:#1e3a8a;color:#bfdbfe;font-weight:bold;"
            f"border-radius:8px;border:none;font-size:14px;min-width:80px;"
        )
        send_mkt_btn.clicked.connect(self._send_marketing)
        mrow.addWidget(send_mkt_btn)
        mf.addLayout(mrow)
        lo.addWidget(mg)

        # ── Reset All Panels (previously "Reset All Slaves") ───────────────
        rg = QGroupBox("Panel Control")
        rl = QVBoxLayout(rg)
        rl.setSpacing(10)

        rst_desc = QLabel("Send a hardware reset command to all connected display panels.")
        rst_desc.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        rl.addWidget(rst_desc)

        rst_panels_btn = QPushButton("↺  Reset All Panels")
        rst_panels_btn.setFixedHeight(52)
        rst_panels_btn.setStyleSheet(
            f"background:#7f1d1d;color:#fecaca;font-size:15px;font-weight:bold;"
            f"border-radius:10px;border:2px solid {RED};"
        )
        rst_panels_btn.clicked.connect(self._reset_slaves)
        rl.addWidget(rst_panels_btn)
        lo.addWidget(rg)

        # GPIO pin diagram
        pg = QGroupBox("GPIO Pin Map  (Raspberry Pi BCM)")
        pl = QVBoxLayout(pg)
        def _pr(fn, bcm, phy, col="#f1f5f9"):
            return (f"<tr><td style='padding:3px 16px 3px 4px;color:{col};'>{fn}</td>"
                    f"<td style='padding:3px 16px;color:{col};'>{bcm}</td>"
                    f"<td style='padding:3px;color:{col};'>{phy}</td></tr>")
        def _ph(text):
            return (f"<tr><td colspan='3' style='padding:6px 4px 2px 4px;"
                    f"color:#fbbf24;font-weight:bold;font-size:12px;"
                    f"letter-spacing:1px;'>{text}</td></tr>")
        CA = "#4ade80"   # Team A green
        CB = "#60a5fa"   # Team B blue
        CW = "#f1f5f9"   # white/default
        CY = "#fbbf24"   # amber / new pins
        pin_info = (
            "<table style='font-size:13px;color:#f1f5f9;border-collapse:collapse;width:100%;'>"
            "<tr>"
            "<th style='text-align:left;padding:4px 16px 4px 4px;color:#fbbf24;'>Function</th>"
            "<th style='padding:4px 16px;color:#fbbf24;'>BCM</th>"
            "<th style='padding:4px 4px;color:#fbbf24;'>Physical Pin</th>"
            "</tr>"
            + _ph("── GAME CLOCK ──────────────────────────")
            + _pr("Game Clock  START",   17, 11)
            + _pr("Game Clock  STOP",    27, 13)
            + _pr("Game Clock  RESET",    8, 24, CY)
            + _ph("── SHOT CLOCK ──────────────────────────")
            + _pr("Shot Clock  RESET 24s", 22, 15)
            + _pr("Shot Clock  START",    23, 16)
            + _pr("Shot Clock  STOP",     24, 18)
            + _pr("Shot Clock  RESET 14s",25, 22)
            + _ph("── TEAM A (green) ──────────────────────")
            + _pr("Team A  +1 score",  4,  7,  CA)
            + _pr("Team A  +2 score",  5,  29, CA)
            + _pr("Team A  +3 score",  6,  31, CA)
            + _pr("Team A  −1 score", 18,  12, CA)
            + _pr("Team A  Foul  +",  12,  32, CA)
            + _pr("Team A  Foul  −",  21,  40, CA)
            + _pr("Team A  Possession", 10, 19, CA)
            + _pr("Team A  Timeout",    2,   3, CA)
            + _ph("── TEAM B (blue) ───────────────────────")
            + _pr("Team B  +1 score", 13,  33, CB)
            + _pr("Team B  +2 score", 16,  36, CB)
            + _pr("Team B  +3 score", 19,  35, CB)
            + _pr("Team B  −1 score", 20,  38, CB)
            + _pr("Team B  Foul  +",  26,  37, CB)
            + _pr("Team B  Foul  −",   9,  21, CB)
            + _pr("Team B  Possession",11,  23, CB)
            + _pr("Team B  Timeout",   3,   5, CB)
            + _ph("── MATCH CONTROL ───────────────────────")
            + _pr("Quarter  +1",        7,  26, CY)
            + _pr("YES VISIBLE",       14,   8, CY)
            + _pr("START MATCH",       15,  10, CY)
            + _ph("── GROUND ──────────────────────────────")
            + _pr("GND  (use any)", "—", "6 / 9 / 14 / 20 / 25 / 30 / 34 / 39", "#94a3b8")
            + "</table>"
            "<p style='color:#94a3b8;font-size:12px;margin-top:10px;'>"
            "Wiring: one side of button → GPIO pin, other side → GND.<br>"
            "Pull-up enabled — button press = LOW signal.<br>"
            "<span style='color:#fbbf24;'>★ GPIO 14 &amp; 15 (physical 8 &amp; 10):</span>"
            " disable UART console first via raspi-config → Interface → Serial Port.</p>"
        )
        pin_lbl = QLabel(pin_info)
        pin_lbl.setStyleSheet(f"padding:10px;background:{CARD};border-radius:8px;")
        pl.addWidget(pin_lbl)
        lo.addWidget(pg)
        lo.addStretch()
        return outer

    # ─────────────────────────────────────────────────────────────────────
    # Helper widgets
    # ─────────────────────────────────────────────────────────────────────
    @staticmethod
    def _card():
        f = QFrame()
        f.setStyleSheet(f"QFrame{{background:{CARD};border:1px solid {BORDER};"
                        f"border-radius:12px;}}")
        QVBoxLayout(f)
        return f

    @staticmethod
    def _hr():
        f = QFrame(); f.setFrameShape(QFrame.HLine)
        f.setStyleSheet(f"color:{BORDER};margin:2px 0;")
        return f

    @staticmethod
    def _kb_btn():
        b = QPushButton("⌨")
        b.setFixedSize(46, 46)
        b.setStyleSheet(f"background:#1e3a8a;color:#93c5fd;font-size:20px;"
                        f"border-radius:8px;border:none;min-height:46px;")
        return b

    @staticmethod
    def _ok_btn():
        b = QPushButton("✓  OK")
        b.setFixedHeight(46); b.setMinimumWidth(72)
        b.setStyleSheet(f"background:#166534;color:#bbf7d0;font-size:14px;"
                        f"font-weight:bold;border-radius:8px;border:none;")
        return b

    @staticmethod
    def _section_lbl(text):
        l = QLabel(text)
        l.setStyleSheet(f"color:{TEXT3};font-size:11px;font-weight:bold;"
                        f"letter-spacing:2px;margin-top:2px;")
        return l

    # ─────────────────────────────────────────────────────────────────────
    # Text change helpers
    # ─────────────────────────────────────────────────────────────────────
    def _on_event_changed(self, t):
        self.event_name = t
        self.event_char_lbl.setText(f"{len(t)}/32")

    def _on_event_scroll_changed(self, state):
        self.event_scroll = (state == Qt.Checked)

    def _on_teams_changed(self, t):
        self.team_names = t.ljust(32)[:32]
        self.team_a = self.team_names[:16].rstrip()
        self.team_b = self.team_names[16:32].rstrip()
        if hasattr(self, "teams_char_lbl"):
            self.teams_char_lbl.setText(f"{len(t)}/32")

    def _on_marketing_changed(self, t):
        self.marketing_text = t
        if hasattr(self, "marketing_char_lbl"):
            self.marketing_char_lbl.setText(f"{len(t)}/20")

    # ─────────────────────────────────────────────────────────────────────
    # Keyboard / Dialog openers
    # ─────────────────────────────────────────────────────────────────────
    def _open_kb_event(self):
        text, ok = VirtualKeyboard.getText(self, "Enter Event Title", self.event_name, 32)
        if ok:
            self.event_name = text
            self.event_edit.setText(text)

    def _open_kb_teams(self):
        text, ok = VirtualKeyboard.getText(self, "Enter Team Names (32 chars)",
                                           self.team_names.rstrip(), 32)
        if ok:
            padded = text.ljust(32)[:32]
            self.team_names = padded
            self.team_a = padded[:16].rstrip()
            self.team_b = padded[16:32].rstrip()
            if hasattr(self, "teams_edit"):
                self.teams_edit.setText(text)
            self._send_name()

    def _open_clock_dialog(self):
        dlg = GameClockDialog(self.clock_secs, self)
        if dlg.exec_() == QDialog.Accepted:
            self._stop_clock()
            self.clock_secs   = dlg.result_secs()
            self.clock_tenths = 0
            # whatever time is set becomes the quarter duration for all quarters
            self.qtr_mins = self.clock_secs // 60
            if hasattr(self, "qtr_spin"):
                self.qtr_spin.blockSignals(True)
                self.qtr_spin.setValue(self.qtr_mins)
                self.qtr_spin.blockSignals(False)
            self.clock_lbl.setText(self._fmt_clock())
            self._log(f"Clock set to {self._fmt_clock()} — quarter duration updated to {self.qtr_mins} min")
            self._send_score()

    def _open_shot_dialog(self):
        dlg = ShotClockDialog(self.shot_secs, self)
        if dlg.exec_() == QDialog.Accepted:
            self._stop_shot()
            self.shot_secs   = dlg.result_secs()
            self.shot_tenths = 0
            self.shot_lbl.setText(self._fmt_shot())
            self._log(f"Shot clock set to {self.shot_secs}s")
            self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # v4: Board confirmation + match start
    # ─────────────────────────────────────────────────────────────────────
    def _confirm_board(self):
        self.board_confirmed = True
        self.yes_visible_btn.setText("✓  BOARDS CONFIRMED")
        self.yes_visible_btn.setStyleSheet(
            f"background:#14532d;color:#bbf7d0;font-size:16px;font-weight:bold;"
            f"border-radius:10px;border:2px solid {GREEN};"
        )
        self._log("✓ Board confirmed — clocks unlocked", "success")

    def _start_match(self):
        if not self.board_confirmed:
            self._log("⚠ Click YES VISIBLE first to confirm boards", "warn")
            return
        # Full game reset
        self.score_a = 0;     self.score_b = 0
        self.fouls_a = 0;     self.fouls_b = 0
        self.quarter = 1
        self.possession = "N"
        self.clock_secs   = self.qtr_mins * 60
        self.clock_tenths = 0
        self.clock_running = False
        self.shot_secs   = DEFAULT_SHOT
        self.shot_tenths = 0
        self.shot_running = False
        self.break_running = False
        self.break_frame.setVisible(False)
        self.match_started = True
        self.q4_2min_done  = False

        self._update_timeout_max(1)
        self.score_a_lbl.setText("0"); self.score_b_lbl.setText("0")
        self.fouls_a_lbl.setText("0"); self.fouls_b_lbl.setText("0")
        self.qtr_lbl.setText("1")
        self.clock_lbl.setText(self._fmt_clock())
        self.shot_lbl.setText(self._fmt_shot())
        self._set_poss("N")

        # Q1 auto-starts immediately
        self.clock_running = True
        self._log("🏀 MATCH STARTED — Q1 running", "success")
        self._send_name()
        self._send_score()

    def _reset_match(self):
        """Reset all game state back to initial defaults without requiring board confirmation."""
        self.score_a       = 0
        self.score_b       = 0
        self.fouls_a       = 0
        self.fouls_b       = 0
        self.quarter       = 1
        self.possession    = "N"
        self.clock_secs    = self.qtr_mins * 60
        self.clock_tenths  = 0
        self.clock_running = False
        self.shot_secs     = DEFAULT_SHOT
        self.shot_tenths   = 0
        self.shot_running  = False
        self.break_running = False
        self.match_started = False
        self.board_confirmed = False
        self.q4_2min_done  = False

        # Reset UI
        if hasattr(self, "break_frame"):
            self.break_frame.setVisible(False)
        if hasattr(self, "score_a_lbl"):
            self.score_a_lbl.setText("0")
        if hasattr(self, "score_b_lbl"):
            self.score_b_lbl.setText("0")
        if hasattr(self, "fouls_a_lbl"):
            self.fouls_a_lbl.setText("0")
        if hasattr(self, "fouls_b_lbl"):
            self.fouls_b_lbl.setText("0")
        if hasattr(self, "qtr_lbl"):
            self.qtr_lbl.setText("1")
        if hasattr(self, "clock_lbl"):
            self.clock_lbl.setText(self._fmt_clock())
        if hasattr(self, "shot_lbl"):
            self.shot_lbl.setText(self._fmt_shot())

        # Reset the YES VISIBLE button appearance
        if hasattr(self, "yes_visible_btn"):
            self.yes_visible_btn.setText("✓  YES VISIBLE — Confirm Boards")
            self.yes_visible_btn.setStyleSheet(
                f"background:#78350f;color:#fef3c7;font-size:16px;font-weight:bold;"
                f"border-radius:10px;border:2px solid {AMBER};"
            )

        self._update_timeout_max(1)
        if hasattr(self, "poss_a_btn"):
            self._set_poss("N")

        self._log("↺ MATCH RESET — all values cleared", "warn")
        self._send_score()
        self._send_name()

    # ─────────────────────────────────────────────────────────────────────
    # Game control
    # ─────────────────────────────────────────────────────────────────────
    def _add_score(self, side, pts):
        attr = f"score_{side.lower()}"
        setattr(self, attr, max(0, getattr(self, attr) + pts))
        getattr(self, f"score_{side.lower()}_lbl").setText(str(getattr(self, attr)))
        self._log(f"Score {side}: {getattr(self, attr)} ({pts:+d})")
        self._send_score()

    def _reset_score(self, side):
        setattr(self, f"score_{side.lower()}", 0)
        getattr(self, f"score_{side.lower()}_lbl").setText("0")
        self._send_score()

    def _delta_fouls(self, side, d):
        attr = f"fouls_{side.lower()}"
        setattr(self, attr, max(0, min(MAX_FOULS, getattr(self, attr) + d)))
        getattr(self, f"fouls_{side.lower()}_lbl").setText(str(getattr(self, attr)))
        self._send_score()

    def _use_timeout(self, side):
        attr = f"timeouts_{side.lower()}"
        if getattr(self, attr) > 0:
            setattr(self, attr, getattr(self, attr) - 1)
            self._refresh_to_ui(side)
            self._log(f"Team {side} timeout used — {getattr(self, attr)} left")
            self._send_score()

    def _add_timeout(self, side):
        attr = f"timeouts_{side.lower()}"
        if getattr(self, attr) < self.timeout_max:
            setattr(self, attr, getattr(self, attr) + 1)
            self._refresh_to_ui(side)
            self._send_score()

    def _reset_timeouts(self, side):
        setattr(self, f"timeouts_{side.lower()}", self.timeout_max)
        self._refresh_to_ui(side)
        self._send_score()

    def _refresh_to_ui(self, side):
        count = getattr(self, f"timeouts_{side.lower()}")
        getattr(self, f"to_{side.lower()}_lbl").setText(str(count))
        dots = getattr(self, f"to_{side.lower()}_dots")
        for i, dot in enumerate(dots):
            if i < self.timeout_max:
                dot.setVisible(True)
                dot.setStyleSheet(
                    f"font-size:26px;color:{AMBER};" if i < count
                    else f"font-size:26px;color:{BORDER};"
                )
            else:
                dot.setVisible(False)

    def _update_dot_visibility(self):
        self._refresh_to_ui("A")
        self._refresh_to_ui("B")

    def _update_timeout_max(self, q):
        if q <= 2:
            self.timeout_max = 2
            self.timeouts_a  = min(self.timeouts_a, 2)
            self.timeouts_b  = min(self.timeouts_b, 2)
        elif q == 3:
            self.timeout_max = 3
            self.timeouts_a  = 3
            self.timeouts_b  = 3
        else:  # Q4 / OT — start at 3, will be capped after 2 min
            self.timeout_max = 3
            self.timeouts_a  = 3
            self.timeouts_b  = 3
        self._update_dot_visibility()

    def _apply_q4_timeout_rule(self):
        if self.timeouts_a > 2: self.timeouts_a = 2
        if self.timeouts_b > 2: self.timeouts_b = 2
        self.timeout_max = 2
        self._update_dot_visibility()
        self._log("Q4 2-min mark — timeouts capped to 2 per team", "warn")
        self._send_score()

    def _set_quarter(self, v):
        self._stop_clock()
        self._stop_shot()
        self.quarter      = v
        self.clock_secs   = self.qtr_mins * 60
        self.clock_tenths = 0
        self.break_running = False
        self.break_frame.setVisible(False)
        if v == 4: self.q4_2min_done = False
        self._update_timeout_max(v)
        self.qtr_lbl.setText(str(v) if v <= 4 else "OT")
        self.clock_lbl.setText(self._fmt_clock())
        self._log(f"Quarter → {self.qtr_lbl.text()}")
        # Q1 auto-starts when match is live
        if v == 1 and self.board_confirmed and self.match_started:
            self.clock_running = True
            self._log("Q1 clock AUTO-STARTED")
        self._send_score()

    def _set_poss(self, v):
        self.possession = v
        self.poss_a_btn.setChecked(v == "A")
        self.poss_none_btn.setChecked(v == "N")
        self.poss_b_btn.setChecked(v == "B")
        self._send_score()

    def _next_quarter(self):
        if self.quarter < 5:
            self._stop_clock(); self._stop_shot()
            self._advance_to_quarter(self.quarter + 1)

    # ─────────────────────────────────────────────────────────────────────
    # Quarter auto-flow
    # ─────────────────────────────────────────────────────────────────────
    def _on_clock_expired(self):
        if not self.match_started or self.break_running:
            return
        if self.quarter == 1:
            self._start_break(120, 1)          # 2-min break after Q1
        elif self.quarter == 2:
            self._start_break(600, 2)          # 10-min halftime after Q2
        elif self.quarter == 3:
            self._start_break(120, 3)          # 2-min break after Q3
        elif self.quarter == 4:
            self._log("🏆 GAME OVER — Q4 clock expired", "success")

    def _start_break(self, secs, after_qtr):
        self.break_running   = True
        self.break_secs      = secs
        self.break_tenths    = 0
        self.break_after_qtr = after_qtr
        label = "HALF TIME" if after_qtr == 2 else f"BREAK  (Q{after_qtr+1} next)"
        self.break_title_lbl.setText(label)
        self.break_frame.setVisible(True)
        self._log(f"⏸ {label} — {secs//60}:{secs%60:02d} countdown")

    def _on_break_expired(self):
        next_q = self.break_after_qtr + 1
        self.break_running = False
        self.break_frame.setVisible(False)
        self._advance_to_quarter(next_q)
        self._log(f"Break ended → Q{next_q} ready. Press START.", "success")

    def _advance_to_quarter(self, q):
        self.quarter      = q
        self.clock_secs   = self.qtr_mins * 60
        self.clock_tenths = 0
        self.clock_running = False
        if q == 4: self.q4_2min_done = False
        self._update_timeout_max(q)
        self.qtr_lbl.setText(str(q) if q <= 4 else "OT")
        self.clock_lbl.setText(self._fmt_clock())
        self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # Clocks
    # ─────────────────────────────────────────────────────────────────────
    def _start_clock(self):
        if not self.board_confirmed:
            self._log("⚠ Click YES VISIBLE first", "warn"); return
        if not self.clock_running and (self.clock_secs > 0 or self.clock_tenths > 0):
            self.clock_running = True
            self._log("Game clock STARTED"); self._send_score()

    def _stop_clock(self):
        if self.clock_running:
            self.clock_running = False
            self._log("Game clock STOPPED"); self._send_score()

    def _reset_clock(self):
        self.clock_running = False
        self.clock_secs    = self.qtr_mins * 60
        self.clock_tenths  = 0
        self.clock_lbl.setText(self._fmt_clock())
        self._send_score()

    def _start_shot(self):
        if not self.board_confirmed:
            self._log("⚠ Click YES VISIBLE first", "warn"); return
        if not self.shot_running and (self.shot_secs > 0 or self.shot_tenths > 0):
            self.shot_running = True
            self._log("Shot clock STARTED"); self._send_score()

    def _stop_shot(self):
        if self.shot_running:
            self.shot_running = False
            self._log("Shot clock STOPPED"); self._send_score()

    def _reset_shot(self, secs):
        self.shot_secs   = secs
        self.shot_tenths = 0
        self.shot_running = False
        self.shot_lbl.setText(self._fmt_shot())
        self._log(f"Shot clock reset to {secs}s"); self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # 100 ms tick
    # ─────────────────────────────────────────────────────────────────────
    def _tick(self):
        changed = False

        # Break countdown
        if self.break_running and (self.break_secs > 0 or self.break_tenths > 0):
            self.break_tenths -= 1
            if self.break_tenths < 0:
                self.break_tenths = 9; self.break_secs -= 1
            if self.break_secs <= 0 and self.break_tenths <= 0:
                self.break_secs = 0; self.break_tenths = 0
                self._on_break_expired()
            else:
                m = self.break_secs // 60; s = self.break_secs % 60
                self.break_count_lbl.setText(f"{m}:{s:02d}")
            changed = True

        # Game clock
        if self.clock_running and (self.clock_secs > 0 or self.clock_tenths > 0):
            self.clock_tenths -= 1
            if self.clock_tenths < 0:
                self.clock_tenths = 9; self.clock_secs -= 1
            if self.clock_secs <= 0 and self.clock_tenths <= 0:
                self.clock_secs = 0; self.clock_tenths = 0
                self.clock_running = False
                self._log("⏰ Game clock EXPIRED")
                self._on_clock_expired()
            self.clock_lbl.setText(self._fmt_clock())
            self._update_clock_debug()
            changed = True

            # Q4 2-minute rule
            if self.quarter == 4 and not self.q4_2min_done:
                if self.clock_secs < (self.qtr_mins * 60 - 120):
                    self.q4_2min_done = True
                    self._apply_q4_timeout_rule()

        # Shot clock
        if self.shot_running and (self.shot_secs > 0 or self.shot_tenths > 0):
            self.shot_tenths -= 1
            if self.shot_tenths < 0:
                self.shot_tenths = 9; self.shot_secs -= 1
            if self.shot_secs <= 0 and self.shot_tenths <= 0:
                self.shot_secs = 0; self.shot_tenths = 0
                self.shot_running = False
                self._log("⏰ Shot clock EXPIRED")
            self.shot_lbl.setText(self._fmt_shot())
            self._update_shot_debug()

    def _update_clock_debug(self):
        if not hasattr(self, "dbg_clock"): return
        self.dbg_clock.setText("⏱  Running" if self.clock_running else "⏱  Stopped")
        self.dbg_clock.setStyleSheet(
            f"font-size:14px;font-weight:bold;"
            f"color:{'#4ade80' if self.clock_running else ORANGE};"
        )

    def _update_shot_debug(self):
        if hasattr(self, "dbg_shot"):
            self.dbg_shot.setText(
                "🔴  Shot: Running" if self.shot_running else "🔴  Shot: Stopped"
            )

    # ─────────────────────────────────────────────────────────────────────
    # Format helpers
    # ─────────────────────────────────────────────────────────────────────
    def _fmt_clock(self):
        s, t = self.clock_secs, self.clock_tenths
        return f"{s // 60}:{s % 60:02d}" if s >= 60 else f"{s}.{t}"

    def _fmt_shot(self):
        s, t = self.shot_secs, self.shot_tenths
        return str(s) if s >= 10 else f"{s}.{t}"

    # ─────────────────────────────────────────────────────────────────────
    # Serial — smart auto-detect Pi vs Windows
    # ─────────────────────────────────────────────────────────────────────
    def _refresh_ports(self):
        self.port_combo.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo.addItems(ports or ["No ports found"])
        # Pre-select best candidate
        best = self._best_port(ports)
        if best and best in ports:
            self.port_combo.setCurrentText(best)

    def _best_port(self, ports):
        if not ports: return None
        if platform.system() == "Windows":
            for p in ports:
                if p.startswith("COM") and p != "COM1":
                    return p
        else:
            for prefix in ("ttyUSB", "ttyACM", "ttyAMA", "serial"):
                for p in ports:
                    if prefix in p:
                        return p
        return ports[0]

    def _apply_serial(self):
        self.port = self.port_combo.currentText()
        self._connect_serial()

    def _connect_serial(self):
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if not ports:
            self._set_serial_ui(False, "No ports found")
            self._log("❌ No serial ports found", "error"); return
        target = self.port if self.port in ports else self._best_port(ports)
        try:
            baud = int(self.baud_combo.currentText()) if hasattr(self, "baud_combo") else 115200
            self.serial_port = serial.Serial(target, baud, timeout=0.1)
            self.port = target
            self._set_serial_ui(True, target)
            self._log(f"✅ Connected: {target} @ {baud}", "success")
            # Auto-send marketing text on connect, then send full state
            self._send_marketing()
            self._send()
        except Exception as e:
            self._set_serial_ui(False, str(e)[:40])
            self._log(f"❌ {e}", "error")

    def _set_serial_ui(self, ok, detail=""):
        color = GREEN if ok else RED
        self.serial_lbl.setText(("✓  " if ok else "⚠  ") + detail)
        self.serial_lbl.setStyleSheet(
            f"color:{color};font-weight:bold;padding:8px 12px;"
            f"background:{CARD};border-radius:8px;font-size:13px;"
        )
        if hasattr(self, "dbg_serial"):
            self.dbg_serial.setText("✅  Connected" if ok else "❌  Disconnected")
            self.dbg_serial.setStyleSheet(f"font-size:14px;font-weight:bold;color:{color};")
            self.dbg_port_lbl.setText(f"Port: {detail}")

    # ─────────────────────────────────────────────────────────────────────
    # Packet TX
    # ─────────────────────────────────────────────────────────────────────
    def _send_name(self):
        scroll_flag = 1 if self.event_scroll else 0
        # Split 32-char team_names: first 16 → teamA field, last 16 → teamB field
        tn = self.team_names.ljust(32)[:32]
        ta = tn[:16].rstrip()
        tb = tn[16:32].rstrip()
        pkt = f"N,{self.event_name},{ta},{tb},{scroll_flag},{self.marketing_text}\n"
        if not self.serial_port or not self.serial_port.is_open:
            self._log("⚠ Serial not connected", "warn"); return
        try:
            self.serial_port.write(pkt.encode())
            self.serial_port.flush()
            self._log(f"TX N → {pkt.strip()}", "success")
        except Exception as e:
            self._log(f"❌ {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    def _send_score(self):
        poss  = self.possession if self.possession in ("A", "B") else "N"
        # During a break, push the break countdown into the game clock slot
        if self.break_running:
            clk_s, clk_t, clk_run = self.break_secs, self.break_tenths, 1
        else:
            clk_s, clk_t, clk_run = self.clock_secs, self.clock_tenths, 1 if self.clock_running else 0
        s_pkt = (
            f"S,"
            f"{self.score_a},{self.score_b},"
            f"{clk_s},{clk_t},"
            f"{self.quarter},{poss},"
            f"{self.fouls_a},{self.fouls_b},"
            f"{self.timeouts_a},{self.timeouts_b},"
            f"{clk_run},"
            f"{self.shot_secs},{self.shot_tenths},"
            f"{1 if self.shot_running else 0},"
            f"{self.font_score},{self.font_clock},"
            f"{self.font_foul},{self.font_shot}\n"
        )
        if not self.serial_port or not self.serial_port.is_open:
            return
        try:
            self.serial_port.write(s_pkt.encode())
            self.serial_port.flush()
            self.packets_sent += 1
            self.pkt_lbl.setText(f"Packets Sent: {self.packets_sent}")
            if hasattr(self, "dbg_pkt_cnt"):
                self.dbg_pkt_cnt.setText(f"Sent: {self.packets_sent}")
            if hasattr(self, "dbg_last_pkt"):
                self.dbg_last_pkt.setText(s_pkt.strip())
        except Exception as e:
            self._log(f"❌ {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    def _send_marketing(self):
        """Send marketing text packet M,<text> to serial."""
        text = self.marketing_text if hasattr(self, "marketing_text") else ""
        pkt = f"M,{text}\n"
        if not self.serial_port or not self.serial_port.is_open:
            return
        try:
            self.serial_port.write(pkt.encode())
            self.serial_port.flush()
            self._log(f"TX M → {pkt.strip()}", "success")
        except Exception as e:
            self._log(f"❌ Marketing send failed: {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    def _send(self):
        self._send_name(); self._send_score()

    def _reset_slaves(self):
        """Send hardware reset command to all display panels."""
        if not self.serial_port or not self.serial_port.is_open:
            self._log("⚠ Serial not connected — cannot reset panels", "warn"); return
        try:
            self.serial_port.write(b"R\n")
            self.serial_port.flush()
            self._log("↺ Reset command sent to all display panels", "success")
        except Exception as e:
            self._log(f"❌ {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    # ─────────────────────────────────────────────────────────────────────
    # Debug console
    # ─────────────────────────────────────────────────────────────────────
    def _log(self, msg, level="info"):
        if not hasattr(self, "console"): return
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        colors = {"info":"#58a6ff","success":"#3fb950","warn":"#d29922",
                  "error":"#f85149","packet":"#6b7280"}
        c = colors.get(level, "#c9d1d9")
        self.console.append(
            f'<span style="color:#444;">[{ts}]</span> '
            f'<span style="color:{c};">{msg}</span>'
        )
        self.console.moveCursor(QTextCursor.End)
        doc = self.console.document()
        while doc.blockCount() > 500:
            cur = QTextCursor(doc.findBlockByNumber(0))
            cur.select(QTextCursor.BlockUnderCursor)
            cur.removeSelectedText(); cur.deleteChar()

    def closeEvent(self, event):
        for _b in _gz_buttons:
            try: _b.close()
            except Exception: pass
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        event.accept()


# ─────────────────────────────────────────────────────────────────────────────
def main():
    app = QApplication(sys.argv)
    app.setFont(QFont("Segoe UI", 11))

    # ── Splash screen (no extra libraries needed — pure PyQt5) ──
    screen      = app.primaryScreen()
    screen_rect = screen.geometry()
    sw, sh      = screen_rect.width(), screen_rect.height()

    logo_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "PRODUCT LAUNCH",
                             "WhatsApp Image 2026-05-15 at 12.24.19 PM.jpeg")

    # Basketball orange background
    pix = QPixmap(sw, sh)
    pix.fill(QColor("#E87722"))

    p = QPainter(pix)
    p.setRenderHint(QPainter.SmoothPixmapTransform)

    if os.path.exists(logo_path):
        logo = QPixmap(logo_path)
        # Scale logo to at most 60% of screen, preserving aspect ratio
        max_w = int(sw * 0.60)
        max_h = int(sh * 0.60)
        logo  = logo.scaled(max_w, max_h, Qt.KeepAspectRatio, Qt.SmoothTransformation)
        # Draw centered
        lx = (sw - logo.width())  // 2
        ly = (sh - logo.height()) // 2
        p.drawPixmap(lx, ly, logo)
    else:
        # Fallback: white "PointiQ v6" text centered
        p.setPen(QColor("#ffffff"))
        p.setFont(QFont("Segoe UI", 72, QFont.Bold))
        p.drawText(pix.rect(), Qt.AlignCenter, "PointiQ v6")

    p.end()

    splash = QSplashScreen(pix, Qt.WindowStaysOnTopHint)
    splash.setGeometry(screen_rect)
    splash.showFullScreen()
    app.processEvents()

    win = ScoreboardApp()

    def _launch():
        splash.finish(win)
        win.showFullScreen()

    QTimer.singleShot(3000, _launch)   # logo shows for 3 seconds
    sys.exit(app.exec_())

if __name__ == "__main__":
    main()
