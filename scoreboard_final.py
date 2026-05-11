#!/usr/bin/env python3
"""
Basketball Scoreboard v3.0 — Final with Virtual Keyboard + Clock Dialogs
Raspberry Pi + Master ESP32 (USB) + Slave ESP32s (ESP-NOW) + P10 LED Matrix

Requirements: pip3 install PyQt5 pyserial --break-system-packages
"""

import sys
import serial
import serial.tools.list_ports
from datetime import datetime
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QLineEdit, QGroupBox, QTabWidget, QTextEdit,
    QSpinBox, QComboBox, QFrame, QFormLayout, QSizePolicy, QDialog,
    QGridLayout, QScrollArea
)
from PyQt5.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt5.QtGui import QFont, QTextCursor

# ─────────────────────────────────────────────────────────────────────────────
# GPIO
# ─────────────────────────────────────────────────────────────────────────────
PIN_GCC_START = 17
PIN_GCC_STOP  = 27
PIN_SCC_24    = 22
PIN_SCC_START = 23
PIN_SCC_STOP  = 24
PIN_SCC_14    = 25

DEFAULT_QTR_MIN = 10
DEFAULT_SHOT    = 24
MAX_TIMEOUTS    = 5
MAX_FOULS       = 10

class GPIOSignals(QObject):
    gcc_start = pyqtSignal()
    gcc_stop  = pyqtSignal()
    scc_24    = pyqtSignal()
    scc_start = pyqtSignal()
    scc_stop  = pyqtSignal()
    scc_14    = pyqtSignal()

GPIO_OK  = False
gpio_sig = GPIOSignals()

try:
    import RPi.GPIO as GPIO
    GPIO.setmode(GPIO.BCM)
    GPIO.setwarnings(False)
    for _p in [PIN_GCC_START, PIN_GCC_STOP, PIN_SCC_24,
               PIN_SCC_START, PIN_SCC_STOP, PIN_SCC_14]:
        GPIO.setup(_p, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.add_event_detect(PIN_GCC_START, GPIO.FALLING,
                          callback=lambda _: gpio_sig.gcc_start.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_GCC_STOP,  GPIO.FALLING,
                          callback=lambda _: gpio_sig.gcc_stop.emit(),  bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_24,    GPIO.FALLING,
                          callback=lambda _: gpio_sig.scc_24.emit(),    bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_START, GPIO.FALLING,
                          callback=lambda _: gpio_sig.scc_start.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_STOP,  GPIO.FALLING,
                          callback=lambda _: gpio_sig.scc_stop.emit(),  bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_14,    GPIO.FALLING,
                          callback=lambda _: gpio_sig.scc_14.emit(),    bouncetime=200)
    GPIO_OK = True
    print("[GPIO] Hardware buttons active")
except Exception as _e:
    print(f"[GPIO] Unavailable ({_e}) — software controls only")


# ═════════════════════════════════════════════════════════════════════════════
#  SHARED STYLES
# ═════════════════════════════════════════════════════════════════════════════
BG      = "#0f172a"    # page background
CARD    = "#1e293b"    # card / panel background
BORDER  = "#334155"    # card border
TEXT    = "#f1f5f9"    # primary text — near-white (high contrast)
TEXT2   = "#cbd5e1"    # secondary text — still readable (5:1+ on CARD)
TEXT3   = "#94a3b8"    # tertiary (labels, hints) — 4.6:1 on CARD
BTN_BG  = "#334155"    # neutral button
BTN_HOV = "#3b82f6"    # hover — blue
GREEN   = "#4ade80"
AMBER   = "#fbbf24"
ORANGE  = "#fb923c"
RED     = "#f87171"
VIOLET  = "#c084fc"
BLUE    = "#60a5fa"

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
#  VIRTUAL KEYBOARD DIALOG
# ═════════════════════════════════════════════════════════════════════════════
class VirtualKeyboard(QDialog):
    """Full-screen touch keyboard — uppercase letters, digits, common symbols."""

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

    # ── Build UI ───────────────────────────────────────────────────────────
    def _build(self, title):
        self.setStyleSheet(DIALOG_STYLE)
        lo = QVBoxLayout(self)
        lo.setSpacing(6)
        lo.setContentsMargins(16, 16, 16, 16)

        # ── Title ──────────────────────────────────────────────────────────
        t = QLabel(title)
        t.setStyleSheet(f"color:{AMBER};font-size:18px;font-weight:bold;")
        t.setAlignment(Qt.AlignCenter)
        lo.addWidget(t)

        # ── Text display ───────────────────────────────────────────────────
        self.display = QLineEdit(self.text)
        self.display.setReadOnly(True)
        self.display.setStyleSheet(
            f"background:{CARD};border:2px solid {AMBER};border-radius:8px;"
            f"color:{TEXT};font-size:22px;font-weight:bold;padding:10px;"
            f"min-height:54px;"
        )
        lo.addWidget(self.display)

        # ── Key rows ───────────────────────────────────────────────────────
        widest = max(len(r) for r in self._ROWS)
        for row in self._ROWS:
            rl = QHBoxLayout()
            rl.setSpacing(self.GAP)
            if len(row) < widest:
                rl.addStretch()
            for key in row:
                btn = self._make_key(key)
                rl.addWidget(btn)
            if len(row) < widest:
                rl.addStretch()
            lo.addLayout(rl)

        # ── Space / Clear / Cancel / OK ────────────────────────────────────
        br = QHBoxLayout()
        br.setSpacing(self.GAP)

        sp = QPushButton("S P A C E")
        sp.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        sp.setFixedHeight(self.KEY_H)
        sp.setStyleSheet(f"background:#1e3a8a;color:#fff;font-size:14px;font-weight:bold;"
                         f"border-radius:8px;border:none;")
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

        br.addWidget(sp)
        br.addWidget(clr)
        br.addWidget(can)
        br.addWidget(ok)
        lo.addLayout(br)

        # Size: enough for widest row (11 keys)
        total_w = widest * self.KEY_W + (widest - 1) * self.GAP + 32
        self.setMinimumWidth(max(total_w, 860))

    def _make_key(self, key):
        btn = QPushButton(key)
        btn.setFont(QFont("Arial", 15, QFont.Bold))
        if key == "⌫":
            btn.setFixedSize(self.KEY_W + 20, self.KEY_H)
            btn.setStyleSheet(
                f"background:#7f1d1d;color:#fca5a5;border-radius:8px;"
                f"font-size:18px;border:none;"
            )
            btn.clicked.connect(self._backspace)
        else:
            btn.setFixedSize(self.KEY_W, self.KEY_H)
            btn.setStyleSheet(
                f"background:{CARD};border:1px solid {BORDER};color:{TEXT};"
                f"border-radius:8px;font-size:15px;"
            )
            btn.clicked.connect(lambda _, k=key: self._insert(k))
        return btn

    def _insert(self, char):
        if len(self.text) < self.max_len:
            self.text += char
            self.display.setText(self.text)

    def _backspace(self):
        self.text = self.text[:-1]
        self.display.setText(self.text)

    def _set(self, t):
        self.text = t
        self.display.setText(t)


# ═════════════════════════════════════════════════════════════════════════════
#  GAME CLOCK DIALOG
# ═════════════════════════════════════════════════════════════════════════════
class GameClockDialog(QDialog):
    """Set game clock by adjusting minutes and seconds with big touch buttons."""

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
        lo.setSpacing(10)
        lo.setContentsMargins(20, 20, 20, 20)

        # Title
        t = QLabel("⏱  SET GAME CLOCK")
        t.setAlignment(Qt.AlignCenter)
        t.setStyleSheet(f"color:{AMBER};font-size:20px;font-weight:bold;")
        lo.addWidget(t)

        # ── Big display ────────────────────────────────────────────────────
        self.disp = QLabel()
        self.disp.setAlignment(Qt.AlignCenter)
        self.disp.setStyleSheet(
            f"background:{CARD};border:2px solid {AMBER};border-radius:12px;"
            f"color:{AMBER};font-size:64px;font-weight:bold;padding:10px;"
        )
        lo.addWidget(self.disp)
        self._refresh()

        # ── Adjusters: side by side ────────────────────────────────────────
        adj = QHBoxLayout()
        adj.setSpacing(20)
        adj.addLayout(self._adjuster("MINUTES", self._adj_min, 0, 99))
        adj.addWidget(self._vsep())
        adj.addLayout(self._adjuster("SECONDS", self._adj_sec, 0, 59))
        lo.addLayout(adj)

        # ── Quick presets ──────────────────────────────────────────────────
        lo.addWidget(self._sec_label("QUICK PRESETS"))
        pr = QHBoxLayout()
        pr.setSpacing(8)
        for label, m in [("5 min", 5), ("8 min", 8), ("10 min", 10),
                          ("12 min", 12), ("15 min", 15)]:
            b = QPushButton(label)
            b.setFixedHeight(52)
            b.setStyleSheet(f"background:#1e3a8a;color:#bfdbfe;font-weight:bold;"
                            f"border-radius:8px;border:none;font-size:14px;")
            b.clicked.connect(lambda _, v=m: self._preset(v))
            pr.addWidget(b)
        lo.addLayout(pr)

        # ── Cancel / OK ────────────────────────────────────────────────────
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
        bot.addWidget(can)
        bot.addWidget(ok)
        lo.addLayout(bot)

        self.setMinimumWidth(520)

    def _adjuster(self, label, fn, lo_lim, hi_lim):
        vl = QVBoxLayout()
        vl.setSpacing(6)
        lbl = QLabel(label)
        lbl.setAlignment(Qt.AlignCenter)
        lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                          f"letter-spacing:1px;")
        vl.addWidget(lbl)
        for delta, text, col in [(+10,"▲▲","#0f4c75"), (+5,"▲","#1a5276"),
                                  (+1,"＋","#1e3a5f"), (-1,"－","#4a1515"),
                                  (-5,"▼","#641e16"), (-10,"▼▼","#7b241c")]:
            b = QPushButton(text)
            b.setFixedHeight(48)
            b.setStyleSheet(f"background:{col};color:{TEXT};border-radius:6px;"
                            f"font-size:16px;font-weight:bold;border:none;")
            b.clicked.connect(lambda _, d=delta, f=fn, lo=lo_lim, hi=hi_lim:
                              f(d, lo, hi))
            vl.addWidget(b)
        return vl

    def _adj_min(self, d, lo, hi):
        self.mins = max(lo, min(hi, self.mins + d))
        self._refresh()

    def _adj_sec(self, d, lo, hi):
        self.secs = max(lo, min(hi, self.secs + d))
        self._refresh()

    def _preset(self, m):
        self.mins = m
        self.secs = 0
        self._refresh()

    def _refresh(self):
        self.disp.setText(f"{self.mins:02d}:{self.secs:02d}")

    def _vsep(self):
        f = QFrame()
        f.setFrameShape(QFrame.VLine)
        f.setStyleSheet(f"color:{BORDER};")
        return f

    @staticmethod
    def _sec_label(text):
        l = QLabel(text)
        l.setStyleSheet(f"color:{TEXT3};font-size:11px;font-weight:bold;"
                        f"letter-spacing:1px;margin-top:4px;")
        return l


# ═════════════════════════════════════════════════════════════════════════════
#  SHOT CLOCK DIALOG
# ═════════════════════════════════════════════════════════════════════════════
class ShotClockDialog(QDialog):
    """Set shot clock in seconds only."""

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

        # Big display
        self.disp = QLabel(str(self.secs))
        self.disp.setAlignment(Qt.AlignCenter)
        self.disp.setStyleSheet(
            f"background:{CARD};border:2px solid {ORANGE};border-radius:12px;"
            f"color:{ORANGE};font-size:80px;font-weight:bold;padding:10px;"
            f"min-height:120px;"
        )
        lo.addWidget(self.disp)

        # +/- row
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

        # Quick presets
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

        # Cancel / OK
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
        bot.addWidget(can)
        bot.addWidget(ok)
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
        self.setWindowTitle("Basketball Scoreboard v3.0")
        self.showMaximized()
        self._connect_serial()

    # ── State ──────────────────────────────────────────────────────────────
    def _init_state(self):
        self.event_name    = "STATE LEVEL BASKETBALL CHAMPIONSHIP 2026"
        self.team_a        = "CHENNAI TIGERS"
        self.team_b        = "MUMBAI HAWKS"
        self.score_a       = 0
        self.score_b       = 0
        self.fouls_a       = 0
        self.fouls_b       = 0
        self.timeouts_a    = MAX_TIMEOUTS
        self.timeouts_b    = MAX_TIMEOUTS
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
        self.event_scroll_mode = False   # False = Static (16 char limit), True = Scroll (64 char limit)
        self.qtr_mins      = DEFAULT_QTR_MIN
        self.serial_port   = None
        self.port          = "/dev/ttyUSB0"
        self.packets_sent  = 0
        self._timer = QTimer()
        self._timer.setInterval(100)
        self._timer.timeout.connect(self._tick)
        self._timer.start()

    # ── GPIO ───────────────────────────────────────────────────────────────
    def _connect_gpio(self):
        if not GPIO_OK:
            return
        gpio_sig.gcc_start.connect(self._start_clock)
        gpio_sig.gcc_stop.connect(self._stop_clock)
        gpio_sig.scc_24.connect(lambda: self._reset_shot(24))
        gpio_sig.scc_start.connect(self._start_shot)
        gpio_sig.scc_stop.connect(self._stop_shot)
        gpio_sig.scc_14.connect(lambda: self._reset_shot(14))

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
            QScrollArea        {{ border:none; background:{BG}; }}
        """)

    # ── Tab 1: Scoreboard ──────────────────────────────────────────────────
    def _make_scoreboard_tab(self):
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setSpacing(8)
        lo.setContentsMargins(10, 10, 10, 10)

        # ── Top bar (event title) ──────────────────────────────────────────
        top = QHBoxLayout()
        top.setSpacing(8)

        ev_lbl = QLabel("EVENT:")
        ev_lbl.setStyleSheet(f"color:{TEXT3};font-weight:bold;font-size:13px;"
                             f"letter-spacing:1px;min-width:60px;")
        top.addWidget(ev_lbl)

        self.event_edit = QLineEdit(self.event_name)
        self.event_edit.setMaxLength(16)   # 16 chars in static mode; 64 in scroll mode
        self.event_edit.setStyleSheet(
            f"background:{CARD};border:1px solid {BORDER};color:{TEXT};"
            f"font-size:15px;font-weight:bold;border-radius:8px;padding:8px;"
            f"min-height:44px;"
        )
        self.event_edit.textChanged.connect(lambda t: setattr(self, "event_name", t))
        top.addWidget(self.event_edit, 2)

        kb_ev = self._kb_btn()
        kb_ev.clicked.connect(self._open_kb_event)
        top.addWidget(kb_ev)

        # ── Static / Scroll mode toggle ────────────────────────────────────
        self.ev_static_btn = QPushButton("📌  STATIC")
        self.ev_static_btn.setFixedHeight(46)
        self.ev_static_btn.setStyleSheet(
            f"background:#166534;color:#bbf7d0;font-weight:bold;font-size:13px;"
            f"border-radius:8px;border:none;min-width:90px;"
        )
        self.ev_static_btn.clicked.connect(lambda: self._set_event_mode(False))
        top.addWidget(self.ev_static_btn)

        self.ev_scroll_btn = QPushButton("↔  SCROLL")
        self.ev_scroll_btn.setFixedHeight(46)
        self.ev_scroll_btn.setStyleSheet(
            f"background:{BTN_BG};color:{TEXT2};font-weight:bold;font-size:13px;"
            f"border-radius:8px;border:1px solid {BORDER};min-width:90px;"
        )
        self.ev_scroll_btn.clicked.connect(lambda: self._set_event_mode(True))
        top.addWidget(self.ev_scroll_btn)

        ok_ev = self._ok_btn()
        ok_ev.setToolTip("Send event title to the P10 display now")
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
        is_a   = side == "A"
        tname  = self.team_a if is_a else self.team_b
        card = self._card()
        lo   = card.layout()
        lo.setSpacing(8)
        lo.setContentsMargins(14, 14, 14, 14)

        # Section header
        hdr = QLabel(f"◀  TEAM {side}" if is_a else f"TEAM {side}  ▶")
        hdr.setAlignment(Qt.AlignCenter)
        hdr.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                          f"letter-spacing:2px;")
        lo.addWidget(hdr)

        # Team name row: edit field + keyboard button
        nr = QHBoxLayout()
        nr.setSpacing(6)
        name_edit = QLineEdit(tname)
        name_edit.setMaxLength(15)
        name_edit.setAlignment(Qt.AlignCenter)
        name_edit.setStyleSheet(
            f"background:#0f172a;border:2px solid {BORDER};color:{TEXT};"
            f"font-size:16px;font-weight:bold;border-radius:8px;padding:8px;"
            f"min-height:46px;"
        )
        if is_a:
            self.team_a_edit = name_edit
            name_edit.textChanged.connect(lambda t: setattr(self, "team_a", t))
        else:
            self.team_b_edit = name_edit
            name_edit.textChanged.connect(lambda t: setattr(self, "team_b", t))
        nr.addWidget(name_edit, 1)

        kb = self._kb_btn()
        kb.clicked.connect(lambda _, s=side: self._open_kb_team(s))
        nr.addWidget(kb)

        ok_team = self._ok_btn()
        ok_team.setToolTip("Send team names + event title to display now")
        ok_team.clicked.connect(self._send_name)
        nr.addWidget(ok_team)

        lo.addLayout(nr)

        # Score display
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
        r1 = QHBoxLayout()
        r1.setSpacing(6)
        for pts in [3, 2, 1]:
            b = QPushButton(f"+{pts}")
            b.setFixedHeight(62)
            b.setStyleSheet(
                f"background:#1d4ed8;color:#fff;font-size:20px;font-weight:bold;"
                f"border-radius:8px;border:none;"
            )
            b.clicked.connect(lambda _, p=pts, s=side: self._add_score(s, p))
            r1.addWidget(b)
        lo.addLayout(r1)

        # -1  |  RST
        r2 = QHBoxLayout()
        r2.setSpacing(6)
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
        r2.addWidget(m1)
        r2.addWidget(rst)
        lo.addLayout(r2)

        lo.addWidget(self._hr())

        # Fouls
        fl = QHBoxLayout()
        fl.setSpacing(8)
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

        # Timeouts
        tl = QHBoxLayout()
        tl.setSpacing(8)
        t_lbl = QLabel("TIMEOUTS")
        t_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                            f"letter-spacing:1px;")
        tl.addWidget(t_lbl)
        tv = QLabel(str(MAX_TIMEOUTS))
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
            cols_map = {"USE": "#92400e", "ADD": "#14532d", "RST": BTN_BG}
            text_map = {"USE": "#fef3c7", "ADD": "#bbf7d0", "RST": TEXT2}
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

        # Timeout dots
        dot_row = QHBoxLayout()
        dot_row.setAlignment(Qt.AlignCenter)
        dot_row.setSpacing(6)
        dots = []
        for i in range(MAX_TIMEOUTS):
            d = QLabel("●")
            d.setStyleSheet(f"font-size:22px;color:{AMBER};")
            dots.append(d)
            dot_row.addWidget(d)
        if is_a:
            self.to_a_dots = dots
        else:
            self.to_b_dots = dots
        dw = QWidget()
        dw.setLayout(dot_row)
        lo.addWidget(dw)

        return card

    # ── Center control panel ───────────────────────────────────────────────
    def _make_center_panel(self):
        card = self._card()
        lo   = card.layout()
        lo.setSpacing(8)
        lo.setContentsMargins(14, 14, 14, 14)

        # ═══ GAME CLOCK ════════════════════════════════════════════════════
        lo.addWidget(self._section_lbl("GAME CLOCK"))

        clk_row = QHBoxLayout()
        clk_row.setSpacing(8)
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
        set_clk.setStyleSheet(
            f"background:#78350f;color:#fef3c7;font-size:13px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        set_clk.clicked.connect(self._open_clock_dialog)
        clk_row.addWidget(set_clk)
        lo.addLayout(clk_row)

        gc = QHBoxLayout()
        gc.setSpacing(8)
        self.clk_start_btn = QPushButton("▶  START")
        self.clk_start_btn.setFixedHeight(56)
        self.clk_start_btn.setStyleSheet(
            f"background:#166534;color:#bbf7d0;font-size:16px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        self.clk_start_btn.clicked.connect(self._start_clock)
        self.clk_stop_btn = QPushButton("■  STOP")
        self.clk_stop_btn.setFixedHeight(56)
        self.clk_stop_btn.setStyleSheet(
            f"background:#991b1b;color:#fecaca;font-size:16px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        self.clk_stop_btn.clicked.connect(self._stop_clock)
        gc.addWidget(self.clk_start_btn)
        gc.addWidget(self.clk_stop_btn)
        lo.addLayout(gc)

        rst_clk = QPushButton("↺  Reset Clock to Quarter Time")
        rst_clk.setFixedHeight(46)
        rst_clk.setStyleSheet(f"background:{BTN_BG};color:{TEXT2};font-size:13px;"
                              f"border-radius:8px;border:1px solid {BORDER};")
        rst_clk.clicked.connect(self._reset_clock)
        lo.addWidget(rst_clk)

        lo.addWidget(self._hr())

        # ═══ SHOT CLOCK ════════════════════════════════════════════════════
        lo.addWidget(self._section_lbl("SHOT CLOCK"))

        sh_row = QHBoxLayout()
        sh_row.setSpacing(8)
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
        set_sh.setStyleSheet(
            f"background:#7c2d12;color:#fed7aa;font-size:13px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        set_sh.clicked.connect(self._open_shot_dialog)
        sh_row.addWidget(set_sh)
        lo.addLayout(sh_row)

        sr1 = QHBoxLayout()
        sr1.setSpacing(8)
        b24 = QPushButton("RESET  24s")
        b24.setFixedHeight(54)
        b24.setStyleSheet(
            f"background:#c2410c;color:#fff;font-size:15px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        b24.clicked.connect(lambda: self._reset_shot(24))
        b14 = QPushButton("RESET  14s")
        b14.setFixedHeight(54)
        b14.setStyleSheet(
            f"background:#9a3412;color:#fff;font-size:15px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        b14.clicked.connect(lambda: self._reset_shot(14))
        sr1.addWidget(b24)
        sr1.addWidget(b14)
        lo.addLayout(sr1)

        sr2 = QHBoxLayout()
        sr2.setSpacing(8)
        self.shot_start_btn = QPushButton("▶")
        self.shot_start_btn.setFixedHeight(50)
        self.shot_start_btn.setStyleSheet(
            f"background:#166534;color:#bbf7d0;font-size:20px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        self.shot_start_btn.clicked.connect(self._start_shot)
        self.shot_stop_btn = QPushButton("■")
        self.shot_stop_btn.setFixedHeight(50)
        self.shot_stop_btn.setStyleSheet(
            f"background:#991b1b;color:#fecaca;font-size:20px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        self.shot_stop_btn.clicked.connect(self._stop_shot)
        sr2.addWidget(self.shot_start_btn)
        sr2.addWidget(self.shot_stop_btn)
        lo.addLayout(sr2)

        lo.addWidget(self._hr())

        # ═══ QUARTER ═══════════════════════════════════════════════════════
        qr = QHBoxLayout()
        qr.setSpacing(6)
        ql = QLabel("QTR:")
        ql.setStyleSheet(f"color:{TEXT3};font-size:12px;font-weight:bold;"
                         f"letter-spacing:1px;")
        qr.addWidget(ql)
        self.qtr_lbl = QLabel("1")
        self.qtr_lbl.setStyleSheet(
            f"font-size:28px;font-weight:bold;color:{VIOLET};"
            f"min-width:40px;"
        )
        qr.addWidget(self.qtr_lbl)
        for label, v in [("Q1", 1), ("Q2", 2), ("Q3", 3), ("Q4", 4), ("OT", 5)]:
            b = QPushButton(label)
            b.setFixedHeight(46)
            b.setStyleSheet(f"font-size:13px;font-weight:bold;")
            b.clicked.connect(lambda _, x=v: self._set_quarter(x))
            qr.addWidget(b)
        lo.addLayout(qr)

        nq = QPushButton("⏭  Next Quarter  —  Stop Clock + Advance + Reset")
        nq.setFixedHeight(50)
        nq.setStyleSheet(
            f"background:#4c1d95;color:#ddd6fe;font-size:13px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        nq.clicked.connect(self._next_quarter)
        lo.addWidget(nq)

        lo.addWidget(self._hr())

        # ═══ POSSESSION ════════════════════════════════════════════════════
        lo.addWidget(self._section_lbl("POSSESSION"))
        pr = QHBoxLayout()
        pr.setSpacing(8)
        self.poss_a_btn    = QPushButton("◀  TEAM A")
        self.poss_none_btn = QPushButton("—  NONE  —")
        self.poss_b_btn    = QPushButton("TEAM B  ▶")
        for b in [self.poss_a_btn, self.poss_none_btn, self.poss_b_btn]:
            b.setCheckable(True)
            b.setFixedHeight(50)
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

    # ── Tab 2: Send to Display ────────────────────────────────────────────
    def _make_display_tab(self):
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setContentsMargins(24, 24, 24, 24)
        lo.setSpacing(16)

        hdr = QLabel("📺  Send Event Title to Slave 1 Display")
        hdr.setStyleSheet(f"font-size:20px;font-weight:bold;color:{BLUE};padding:6px 0;")
        lo.addWidget(hdr)

        card = self._card()
        cl   = card.layout()
        cl.setContentsMargins(24, 20, 24, 20)
        cl.setSpacing(14)

        # ── Mode row ──────────────────────────────────────────────────────
        cl.addWidget(self._section_lbl("DISPLAY MODE"))
        mode_row = QHBoxLayout()
        mode_row.setSpacing(10)

        on_sty  = (f"background:#166534;color:#bbf7d0;font-weight:bold;font-size:15px;"
                   f"border-radius:8px;border:none;min-width:130px;min-height:50px;")
        off_sty = (f"background:{BTN_BG};color:{TEXT2};font-weight:bold;font-size:15px;"
                   f"border-radius:8px;border:1px solid {BORDER};min-width:130px;min-height:50px;")

        self.disp_static_btn = QPushButton("📌  STATIC")
        self.disp_static_btn.setStyleSheet(on_sty)   # default: static active
        self.disp_static_btn.clicked.connect(lambda: self._set_event_mode(False))

        self.disp_scroll_btn = QPushButton("↔  SCROLL")
        self.disp_scroll_btn.setStyleSheet(off_sty)
        self.disp_scroll_btn.clicked.connect(lambda: self._set_event_mode(True))

        mode_row.addWidget(self.disp_static_btn)
        mode_row.addWidget(self.disp_scroll_btn)
        mode_row.addStretch()
        cl.addLayout(mode_row)

        # ── Event name edit ───────────────────────────────────────────────
        cl.addWidget(self._section_lbl("EVENT TITLE"))
        nr = QHBoxLayout()
        nr.setSpacing(8)

        self.disp_event_edit = QLineEdit(self.event_name)
        self.disp_event_edit.setMaxLength(16)
        self.disp_event_edit.setStyleSheet(
            f"background:#0f172a;border:2px solid {AMBER};color:{TEXT};"
            f"font-size:26px;font-weight:bold;border-radius:10px;padding:12px;"
            f"min-height:62px;"
        )
        self.disp_event_edit.textChanged.connect(self._disp_edit_changed)
        nr.addWidget(self.disp_event_edit, 1)

        kb = self._kb_btn()
        kb.setFixedSize(56, 62)
        kb.clicked.connect(self._open_kb_event_disp)
        nr.addWidget(kb)
        cl.addLayout(nr)

        self.disp_char_lbl = QLabel()
        self.disp_char_lbl.setStyleSheet(f"color:{TEXT3};font-size:12px;")
        cl.addWidget(self.disp_char_lbl)
        self._update_char_count()

        # ── SEND button ───────────────────────────────────────────────────
        send_btn = QPushButton("▶   SEND TO DISPLAY")
        send_btn.setFixedHeight(80)
        send_btn.setStyleSheet(
            f"background:#166534;color:#bbf7d0;font-size:24px;font-weight:bold;"
            f"border-radius:14px;border:none;letter-spacing:1px;"
        )
        send_btn.clicked.connect(self._send_name)
        cl.addWidget(send_btn)

        # ── Status ────────────────────────────────────────────────────────
        self.disp_status_lbl = QLabel("—  Not sent yet")
        self.disp_status_lbl.setAlignment(Qt.AlignCenter)
        self.disp_status_lbl.setStyleSheet(
            f"color:{TEXT3};font-size:14px;padding:10px;"
            f"background:{BG};border-radius:8px;"
        )
        cl.addWidget(self.disp_status_lbl)

        lo.addWidget(card)
        lo.addStretch()
        return w

    def _disp_edit_changed(self, text):
        """Display-tab edit field changed — sync to main event_name."""
        self.event_name = text
        self.event_edit.blockSignals(True)
        self.event_edit.setText(text)
        self.event_edit.blockSignals(False)
        self._update_char_count()

    def _update_char_count(self):
        if not hasattr(self, "disp_char_lbl"):
            return
        limit = 64 if self.event_scroll_mode else 16
        used  = len(self.event_name)
        self.disp_char_lbl.setText(
            f"{used} / {limit} characters  "
            f"({'scroll mode — no display limit' if self.event_scroll_mode else 'static mode — fits 6-panel display'})"
        )

    # ── Tab 3: Debug ───────────────────────────────────────────────────────
    def _make_debug_tab(self):
        w  = QWidget()
        lo = QVBoxLayout(w)
        lo.setContentsMargins(12, 12, 12, 12)

        t = QLabel("🔧  Debug Console")
        t.setStyleSheet(f"font-size:20px;font-weight:bold;color:{BLUE};padding:6px 0;")
        lo.addWidget(t)

        cards = QHBoxLayout()
        cards.setSpacing(10)

        c1 = self._card()
        self.dbg_serial = QLabel("❌  Not Connected")
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

        br = QHBoxLayout()
        br.setSpacing(8)
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
        ref = QPushButton("🔄")
        ref.setFixedSize(46, 46)
        ref.clicked.connect(self._refresh_ports)
        ph = QHBoxLayout()
        ph.addWidget(self.port_combo, 1)
        ph.addWidget(ref)
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
        lo.addWidget(sg)

        # Game
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

        # Font sizes
        fg = QGroupBox(
            "LED Display Font Sizes  [ 1 = Small   2 = Medium   3 = Large ]")
        ff = QFormLayout(fg)
        ff.setSpacing(10)
        for label, attr, wname in [
            ("Score  (Rows 3-4, cols 1-2 & 5-6):",  "font_score", "fs_score"),
            ("Game Clock  (Rows 3-4, cols 3-4):",   "font_clock", "fs_clock"),
            ("Fouls  (Rows 5-6, cols 1-2 & 5-6):",  "font_foul",  "fs_foul"),
            ("Shot Clock  (Rows 5-6, cols 3-4):",   "font_shot",  "fs_shot"),
        ]:
            sp = QSpinBox()
            sp.setRange(1, 3)
            sp.setValue(getattr(self, attr))
            sp.valueChanged.connect(lambda v, a=attr: self._set_font(a, v))
            setattr(self, wname, sp)
            ff.addRow(label, sp)
        send_f = QPushButton("✓  Send Font Update to Display Now")
        send_f.setFixedHeight(50)
        send_f.setStyleSheet(f"background:#1e40af;color:#bfdbfe;font-weight:bold;"
                             f"border-radius:8px;border:none;font-size:14px;")
        send_f.clicked.connect(self._send_score)
        ff.addRow("", send_f)
        lo.addWidget(fg)

        # Info
        ig = QGroupBox("System Information")
        il = QVBoxLayout(ig)
        info = (
            f"<span style='color:{TEXT};font-size:14px;'>"
            f"<b>GPIO:</b> {'✓ Active' if GPIO_OK else '✗ Inactive'} &nbsp;&nbsp; "
            f"<b>Python:</b> {sys.version.split()[0]} &nbsp;&nbsp; "
            f"<b>Serial Ports:</b> {len(serial.tools.list_ports.comports())}"
            f"</span>"
        )
        il_lbl = QLabel(info)
        il_lbl.setStyleSheet(
            f"padding:12px;background:{CARD};border-radius:8px;"
        )
        il.addWidget(il_lbl)
        lo.addWidget(ig)
        lo.addStretch()
        return outer

    # ─────────────────────────────────────────────────────────────────────
    # Keyboard / Dialog openers
    # ─────────────────────────────────────────────────────────────────────
    def _set_event_mode(self, scroll: bool):
        self.event_scroll_mode = scroll
        max_len = 64 if scroll else 16
        self.event_edit.setMaxLength(max_len)
        if not scroll and len(self.event_name) > 16:
            self.event_name = self.event_name[:16]
            self.event_edit.setText(self.event_name)
        on_top  = (f"background:#166534;color:#bbf7d0;font-weight:bold;font-size:13px;"
                   f"border-radius:8px;border:none;min-width:90px;")
        off_top = (f"background:{BTN_BG};color:{TEXT2};font-weight:bold;font-size:13px;"
                   f"border-radius:8px;border:1px solid {BORDER};min-width:90px;")
        self.ev_static_btn.setStyleSheet(off_top if scroll else on_top)
        self.ev_scroll_btn.setStyleSheet(on_top  if scroll else off_top)
        # Keep Display tab buttons in sync
        on_tab  = (f"background:#166534;color:#bbf7d0;font-weight:bold;font-size:15px;"
                   f"border-radius:8px;border:none;min-width:130px;min-height:50px;")
        off_tab = (f"background:{BTN_BG};color:{TEXT2};font-weight:bold;font-size:15px;"
                   f"border-radius:8px;border:1px solid {BORDER};min-width:130px;min-height:50px;")
        if hasattr(self, "disp_static_btn"):
            self.disp_static_btn.setStyleSheet(off_tab if scroll else on_tab)
            self.disp_scroll_btn.setStyleSheet(on_tab  if scroll else off_tab)
            if hasattr(self, "disp_event_edit"):
                self.disp_event_edit.setMaxLength(max_len)
                self._update_char_count()
        self._log(f"Event display mode: {'SCROLL' if scroll else 'STATIC'}")
        # No auto-send — user must press SEND

    def _open_kb_event(self):
        max_len = 64 if self.event_scroll_mode else 16
        text, ok = VirtualKeyboard.getText(
            self, "Enter Event Title", self.event_name, max_len)
        if ok:
            self.event_name = text
            self.event_edit.setText(text)
            if hasattr(self, "disp_event_edit"):
                self.disp_event_edit.blockSignals(True)
                self.disp_event_edit.setText(text)
                self.disp_event_edit.blockSignals(False)
                self._update_char_count()
            # No auto-send — user must press SEND in the Display tab

    def _open_kb_event_disp(self):
        """Keyboard opener from the Display tab."""
        max_len = 64 if self.event_scroll_mode else 16
        text, ok = VirtualKeyboard.getText(
            self, "Enter Event Title", self.event_name, max_len)
        if ok:
            self.event_name = text
            self.event_edit.setText(text)
            self.disp_event_edit.blockSignals(True)
            self.disp_event_edit.setText(text)
            self.disp_event_edit.blockSignals(False)
            self._update_char_count()

    def _open_kb_team(self, side):
        current = self.team_a if side == "A" else self.team_b
        text, ok = VirtualKeyboard.getText(
            self, f"Enter Team {side} Name", current, 15)
        if ok:
            setattr(self, f"team_{side.lower()}", text)
            edit = self.team_a_edit if side == "A" else self.team_b_edit
            edit.setText(text)
            self._send_score()

    def _open_clock_dialog(self):
        dlg = GameClockDialog(self.clock_secs, self)
        if dlg.exec_() == QDialog.Accepted:
            self._stop_clock()
            self.clock_secs   = dlg.result_secs()
            self.clock_tenths = 0
            self.clock_lbl.setText(self._fmt_clock())
            self._log(f"Clock set to {self._fmt_clock()}")
            self._send_score()

    def _open_shot_dialog(self):
        dlg = ShotClockDialog(self.shot_secs, self)
        if dlg.exec_() == QDialog.Accepted:
            self._stop_shot()
            self.shot_secs    = dlg.result_secs()
            self.shot_tenths  = 0
            self.shot_lbl.setText(self._fmt_shot())
            self._log(f"Shot clock set to {self.shot_secs}s")
            self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # Helper widgets
    # ─────────────────────────────────────────────────────────────────────
    @staticmethod
    def _card():
        f = QFrame()
        f.setStyleSheet(f"""
            QFrame {{
                background:{CARD};
                border:1px solid {BORDER};
                border-radius:12px;
            }}
        """)
        QVBoxLayout(f)
        return f

    @staticmethod
    def _hr():
        f = QFrame()
        f.setFrameShape(QFrame.HLine)
        f.setStyleSheet(f"color:{BORDER};margin:2px 0;")
        return f

    @staticmethod
    def _kb_btn():
        b = QPushButton("⌨")
        b.setFixedSize(46, 46)
        b.setStyleSheet(
            f"background:#1e3a8a;color:#93c5fd;font-size:20px;"
            f"border-radius:8px;border:none;min-height:46px;"
        )
        return b

    @staticmethod
    def _ok_btn():
        """Green OK / Send button — placed next to each keyboard button."""
        b = QPushButton("✓  OK")
        b.setFixedHeight(46)
        b.setMinimumWidth(72)
        b.setStyleSheet(
            f"background:#166534;color:#bbf7d0;font-size:14px;font-weight:bold;"
            f"border-radius:8px;border:none;"
        )
        return b

    @staticmethod
    def _section_lbl(text):
        l = QLabel(text)
        l.setStyleSheet(
            f"color:{TEXT3};font-size:11px;font-weight:bold;"
            f"letter-spacing:2px;margin-top:2px;"
        )
        return l

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
            self._log(f"Team {side} timeout used — {getattr(self, attr)} remaining")
            self._send_score()

    def _add_timeout(self, side):
        attr = f"timeouts_{side.lower()}"
        if getattr(self, attr) < MAX_TIMEOUTS:
            setattr(self, attr, getattr(self, attr) + 1)
            self._refresh_to_ui(side)
            self._send_score()

    def _reset_timeouts(self, side):
        setattr(self, f"timeouts_{side.lower()}", MAX_TIMEOUTS)
        self._refresh_to_ui(side)
        self._send_score()

    def _refresh_to_ui(self, side):
        count = getattr(self, f"timeouts_{side.lower()}")
        getattr(self, f"to_{side.lower()}_lbl").setText(str(count))
        for i, dot in enumerate(getattr(self, f"to_{side.lower()}_dots")):
            dot.setStyleSheet(
                f"font-size:22px;color:{AMBER};" if i < count
                else f"font-size:22px;color:{BORDER};"
            )

    def _set_quarter(self, v):
        self.quarter = v
        self.qtr_lbl.setText(str(v) if v <= 4 else "OT")
        self._log(f"Quarter: {self.qtr_lbl.text()}")
        self._send_score()

    def _set_poss(self, v):
        self.possession = v
        self.poss_a_btn.setChecked(v == "A")
        self.poss_none_btn.setChecked(v == "N")
        self.poss_b_btn.setChecked(v == "B")
        self._send_score()

    def _next_quarter(self):
        if self.quarter < 5:
            self._stop_clock()
            self._stop_shot()
            self._set_quarter(self.quarter + 1)
            self.clock_secs   = self.qtr_mins * 60
            self.clock_tenths = 0
            self.clock_lbl.setText(self._fmt_clock())
            self._log(f"New quarter {self.qtr_lbl.text()} — clock reset")
            self._send_score()

    def _set_font(self, attr, v):
        setattr(self, attr, v)
        self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # Clocks
    # ─────────────────────────────────────────────────────────────────────
    def _start_clock(self):
        if not self.clock_running and (self.clock_secs > 0 or self.clock_tenths > 0):
            self.clock_running = True
            self._log("Game clock STARTED")
            self._send_score()

    def _stop_clock(self):
        if self.clock_running:
            self.clock_running = False
            self._log("Game clock STOPPED")
            self._send_score()

    def _reset_clock(self):
        self.clock_running = False
        self.clock_secs    = self.qtr_mins * 60
        self.clock_tenths  = 0
        self.clock_lbl.setText(self._fmt_clock())
        self._send_score()

    def _start_shot(self):
        if not self.shot_running and (self.shot_secs > 0 or self.shot_tenths > 0):
            self.shot_running = True
            self._log("Shot clock STARTED")
            self._send_score()

    def _stop_shot(self):
        if self.shot_running:
            self.shot_running = False
            self._log("Shot clock STOPPED")
            self._send_score()

    def _reset_shot(self, secs):
        self.shot_secs    = secs
        self.shot_tenths  = 0
        self.shot_running = False
        self.shot_lbl.setText(self._fmt_shot())
        self._log(f"Shot clock reset to {secs}s")
        self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # 100 ms tick
    # ─────────────────────────────────────────────────────────────────────
    def _tick(self):
        changed = False

        if self.clock_running and (self.clock_secs > 0 or self.clock_tenths > 0):
            self.clock_tenths -= 1
            if self.clock_tenths < 0:
                self.clock_tenths = 9
                self.clock_secs  -= 1
            if self.clock_secs <= 0 and self.clock_tenths <= 0:
                self.clock_secs   = 0
                self.clock_tenths = 0
                self.clock_running = False
                self._log("⏰ Game clock EXPIRED")
            self.clock_lbl.setText(self._fmt_clock())
            self._update_clock_debug()
            changed = True

        if self.shot_running and (self.shot_secs > 0 or self.shot_tenths > 0):
            self.shot_tenths -= 1
            if self.shot_tenths < 0:
                self.shot_tenths = 9
                self.shot_secs  -= 1
            if self.shot_secs <= 0 and self.shot_tenths <= 0:
                self.shot_secs    = 0
                self.shot_tenths  = 0
                self.shot_running = False
                self._log("⏰ Shot clock EXPIRED")
            self.shot_lbl.setText(self._fmt_shot())
            self._update_shot_debug()
            changed = True

        if changed:
            self._send_score()   # never sends N — Slave 1 display won't be interrupted

    def _update_clock_debug(self):
        if not hasattr(self, "dbg_clock"):
            return
        running = self.clock_running
        self.dbg_clock.setText("⏱  Running" if running else "⏱  Stopped")
        self.dbg_clock.setStyleSheet(
            f"font-size:14px;font-weight:bold;"
            f"color:{'#4ade80' if running else ORANGE};"
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
    # Serial
    # ─────────────────────────────────────────────────────────────────────
    def _refresh_ports(self):
        self.port_combo.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo.addItems(ports or ["No ports found"])

    def _apply_serial(self):
        self.port = self.port_combo.currentText()
        self._connect_serial()

    def _connect_serial(self):
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if not ports:
            self._set_serial_ui(False, "No ports found")
            self._log("❌ No serial ports found", "error")
            return
        target = self.port if self.port in ports else ports[0]
        try:
            baud = int(self.baud_combo.currentText()) if hasattr(self, "baud_combo") else 115200
            self.serial_port = serial.Serial(target, baud, timeout=0.1)
            self.port = target
            self._set_serial_ui(True, target)
            self._log(f"✅ Connected: {target} @ {baud}", "success")
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
            self.dbg_serial.setStyleSheet(
                f"font-size:14px;font-weight:bold;color:{color};"
            )
            self.dbg_port_lbl.setText(f"Port: {detail}")

    # ─────────────────────────────────────────────────────────────────────
    # Packet TX  (split so _tick never sends the N packet)
    # ─────────────────────────────────────────────────────────────────────
    def _send_name(self):
        """Send only the N packet (event title + teams + scroll mode).
        Called manually via the Send-to-Display tab SEND button."""
        n_pkt = (f"N,{self.event_name},{self.team_a},{self.team_b},"
                 f"{1 if self.event_scroll_mode else 0}\n")
        if not self.serial_port or not self.serial_port.is_open:
            self._log("⚠ Serial not connected — N packet not sent", "warn")
            return
        try:
            self.serial_port.write(n_pkt.encode())
            self.serial_port.flush()
            mode_str = "SCROLL" if self.event_scroll_mode else "STATIC"
            self._log(f"TX N → {n_pkt.strip()}", "success")
            if hasattr(self, "disp_status_lbl"):
                self.disp_status_lbl.setText(
                    f'✓  Sent: "{self.event_name}"  [{mode_str}]')
                self.disp_status_lbl.setStyleSheet(
                    f"color:{GREEN};font-size:14px;font-weight:bold;"
                    f"padding:10px;background:{CARD};border-radius:8px;")
        except Exception as e:
            self._log(f"❌ Write error: {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    def _send_score(self):
        """Send only the S packet (scores, clocks, fouls …).
        Safe to call at high frequency — does NOT touch Slave 1 display."""
        poss  = self.possession if self.possession in ("A", "B") else "N"
        s_pkt = (
            f"S,"
            f"{self.score_a},{self.score_b},"
            f"{self.clock_secs},{self.clock_tenths},"
            f"{self.quarter},{poss},"
            f"{self.fouls_a},{self.fouls_b},"
            f"{self.timeouts_a},{self.timeouts_b},"
            f"{1 if self.clock_running else 0},"
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
            self._log(f"TX S → {s_pkt.strip()}", "packet")
        except Exception as e:
            self._log(f"❌ Write error: {e}", "error")
            self._set_serial_ui(False, "Write error")
            self.serial_port = None

    def _send(self):
        """Send both N and S packets — used on initial connect only."""
        self._send_name()
        self._send_score()

    # ─────────────────────────────────────────────────────────────────────
    # Debug console
    # ─────────────────────────────────────────────────────────────────────
    def _log(self, msg, level="info"):
        if not hasattr(self, "console"):
            return
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        colors = {
            "info":    "#58a6ff",
            "success": "#3fb950",
            "warn":    "#d29922",
            "error":   "#f85149",
            "packet":  "#6b7280",
        }
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
            cur.removeSelectedText()
            cur.deleteChar()

    def closeEvent(self, event):
        if GPIO_OK:
            try:
                GPIO.cleanup()
            except Exception:
                pass
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
        event.accept()


# ─────────────────────────────────────────────────────────────────────────────
def main():
    app = QApplication(sys.argv)
    app.setFont(QFont("Segoe UI", 11))
    win = ScoreboardApp()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
