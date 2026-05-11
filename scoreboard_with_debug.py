#!/usr/bin/env python3
"""
Basketball Scoreboard with Debug Panel
Complete system with built-in diagnostics
Requirements: pip3 install PyQt5 pyserial --break-system-packages
"""

import sys
import json
import serial
import serial.tools.list_ports
from datetime import datetime
from PyQt5.QtWidgets import *
from PyQt5.QtCore import QTimer, Qt, pyqtSignal, QObject
from PyQt5.QtGui import QFont, QTextCursor

# GPIO pins
PIN_GCC_START = 17
PIN_GCC_STOP = 27
PIN_SCC_24 = 22
PIN_SCC_START = 23
PIN_SCC_STOP = 24
PIN_SCC_14 = 25

DEFAULT_QTR_MIN = 10
DEFAULT_SHOT = 24

# GPIO Signals
class GPIOSignals(QObject):
    gcc_start = pyqtSignal()
    gcc_stop = pyqtSignal()
    scc_24 = pyqtSignal()
    scc_start = pyqtSignal()
    scc_stop = pyqtSignal()
    scc_14 = pyqtSignal()

GPIO_OK = False
gpio_sig = GPIOSignals()

try:
    import RPi.GPIO as GPIO
    GPIO.setmode(GPIO.BCM)
    GPIO.setwarnings(False)
    for p in [PIN_GCC_START, PIN_GCC_STOP, PIN_SCC_24, PIN_SCC_START, PIN_SCC_STOP, PIN_SCC_14]:
        GPIO.setup(p, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    
    GPIO.add_event_detect(PIN_GCC_START, GPIO.FALLING, callback=lambda _: gpio_sig.gcc_start.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_GCC_STOP, GPIO.FALLING, callback=lambda _: gpio_sig.gcc_stop.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_24, GPIO.FALLING, callback=lambda _: gpio_sig.scc_24.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_START, GPIO.FALLING, callback=lambda _: gpio_sig.scc_start.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_STOP, GPIO.FALLING, callback=lambda _: gpio_sig.scc_stop.emit(), bouncetime=200)
    GPIO.add_event_detect(PIN_SCC_14, GPIO.FALLING, callback=lambda _: gpio_sig.scc_14.emit(), bouncetime=200)
    GPIO_OK = True
    print("✓ GPIO OK")
except:
    print("✗ GPIO unavailable")

# Main Window with Tabs
class ScoreboardWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        
        # State
        self.event_name = "BASKETBALL"
        self.team_a = "TEAM A"
        self.team_b = "TEAM B"
        self.score_a = 0
        self.score_b = 0
        self.fouls_a = 0
        self.fouls_b = 0
        self.timeouts_a = 5
        self.timeouts_b = 5
        self.quarter = 1
        self.possession = " "
        self.clock_secs = DEFAULT_QTR_MIN * 60
        self.clock_tenths = 0
        self.clock_running = False
        self.shot_secs = DEFAULT_SHOT
        self.shot_tenths = 0
        self.shot_running = False
        self.screen_mask = 63
        self.qtr_mins = DEFAULT_QTR_MIN
        
        self.serial_port = None
        self.port = "/dev/ttyUSB0"
        self.packets_sent = 0
        self.last_packet = ""
        
        self.tick_timer = QTimer()
        self.tick_timer.setInterval(100)
        self.tick_timer.timeout.connect(self._tick)
        self.tick_timer.start()
        
        self._build_ui()
        self._connect_gpio()
        self.setWindowTitle("Basketball Scoreboard with Debug")
        self.showMaximized()
        
        # Auto-connect serial
        self._connect_serial()
    
    def _connect_gpio(self):
        if not GPIO_OK:
            return
        gpio_sig.gcc_start.connect(self._start_clock)
        gpio_sig.gcc_stop.connect(self._stop_clock)
        gpio_sig.scc_24.connect(lambda: self._reset_shot(24))
        gpio_sig.scc_start.connect(self._start_shot)
        gpio_sig.scc_stop.connect(self._stop_shot)
        gpio_sig.scc_14.connect(lambda: self._reset_shot(14))
    
    def _build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        main = QVBoxLayout(root)
        main.setContentsMargins(0, 0, 0, 0)
        main.setSpacing(0)
        
        # Create tab widget
        self.tabs = QTabWidget()
        self.tabs.setStyleSheet("""
            QTabWidget::pane {
                border: 1px solid #2a2a2a;
                background: #0a0a0a;
            }
            QTabBar::tab {
                background: #1a1a1a;
                color: #ffffff;
                padding: 12px 24px;
                margin-right: 2px;
                border: 1px solid #2a2a2a;
                border-bottom: none;
                border-top-left-radius: 6px;
                border-top-right-radius: 6px;
            }
            QTabBar::tab:selected {
                background: #0a0a0a;
                border-bottom: 2px solid #3b82f6;
            }
            QTabBar::tab:hover {
                background: #2a2a2a;
            }
        """)
        
        # Tab 1: Scoreboard
        scoreboard_tab = self._create_scoreboard_tab()
        self.tabs.addTab(scoreboard_tab, "🏀 Scoreboard")
        
        # Tab 2: Debug Console
        debug_tab = self._create_debug_tab()
        self.tabs.addTab(debug_tab, "🔧 Debug Console")
        
        # Tab 3: Connection Settings
        settings_tab = self._create_settings_tab()
        self.tabs.addTab(settings_tab, "⚙ Settings")
        
        main.addWidget(self.tabs)
        
        self.setStyleSheet("""
            QMainWindow { background: #0a0a0a; }
            QWidget { color: #fff; font-size: 13px; }
            QPushButton { 
                background: #2a2a2a; 
                border: 1px solid #404040;
                padding: 8px; 
                border-radius: 4px;
                color: #ffffff;
            }
            QPushButton:hover { background: #3b82f6; }
            QPushButton:pressed { background: #2563eb; }
            QLineEdit {
                background: #1a1a1a;
                border: 1px solid #333;
                padding: 6px;
                border-radius: 4px;
                color: #ffffff;
            }
            QLabel { color: #ffffff; }
            QGroupBox {
                border: 1px solid #2a2a2a;
                border-radius: 6px;
                margin-top: 12px;
                padding-top: 12px;
                color: #90caf9;
                font-weight: bold;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 10px;
                padding: 0 5px;
            }
        """)
    
    def _create_scoreboard_tab(self):
        """Main scoreboard interface"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setSpacing(10)
        layout.setContentsMargins(10, 10, 10, 10)
        
        # Top bar
        top = QHBoxLayout()
        top.addWidget(QLabel("Event:"))
        self.event_edit = QLineEdit(self.event_name)
        self.event_edit.textChanged.connect(lambda t: setattr(self, "event_name", t))
        top.addWidget(self.event_edit, 1)
        
        self.serial_btn = QPushButton("📡 Connect Serial")
        self.serial_btn.clicked.connect(self._connect_serial)
        top.addWidget(self.serial_btn)
        
        self.serial_lbl = QLabel("⚠ Not Connected")
        self.serial_lbl.setStyleSheet("color: #ef4444; font-weight: bold; padding: 8px;")
        top.addWidget(self.serial_lbl)
        layout.addLayout(top)
        
        # Scoreboard
        score_row = QHBoxLayout()
        score_row.addWidget(self._create_team("A"), 1)
        score_row.addWidget(self._create_center(), 1)
        score_row.addWidget(self._create_team("B"), 1)
        layout.addLayout(score_row, 1)
        
        # Bottom stats
        stats = QHBoxLayout()
        self.packets_lbl = QLabel("Packets Sent: 0")
        self.packets_lbl.setStyleSheet("color: #10b981; font-weight: bold;")
        stats.addWidget(self.packets_lbl)
        stats.addStretch()
        
        gpio_status = QLabel("GPIO: " + ("✓ Active" if GPIO_OK else "✗ Inactive"))
        gpio_status.setStyleSheet(f"color: {'#10b981' if GPIO_OK else '#ef4444'}; font-weight: bold;")
        stats.addWidget(gpio_status)
        layout.addLayout(stats)
        
        return widget
    
    def _create_debug_tab(self):
        """Debug console with live monitoring"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(10, 10, 10, 10)
        
        # Title
        title = QLabel("🔧 Debug Console")
        title.setStyleSheet("font-size: 18px; font-weight: bold; color: #3b82f6; padding: 10px;")
        layout.addWidget(title)
        
        # Status cards
        cards = QHBoxLayout()
        
        # Serial status card
        serial_card = QFrame()
        serial_card.setStyleSheet("""
            QFrame {
                background: #1a1a1a;
                border: 1px solid #2a2a2a;
                border-radius: 8px;
                padding: 15px;
            }
        """)
        serial_lo = QVBoxLayout(serial_card)
        serial_lo.addWidget(QLabel("Serial Connection"))
        self.debug_serial_status = QLabel("❌ Not Connected")
        self.debug_serial_status.setStyleSheet("font-size: 14px; font-weight: bold; color: #ef4444;")
        serial_lo.addWidget(self.debug_serial_status)
        self.debug_port_lbl = QLabel("Port: -")
        self.debug_port_lbl.setStyleSheet("color: #808080; font-size: 11px;")
        serial_lo.addWidget(self.debug_port_lbl)
        cards.addWidget(serial_card)
        
        # Packet stats card
        packet_card = QFrame()
        packet_card.setStyleSheet("""
            QFrame {
                background: #1a1a1a;
                border: 1px solid #2a2a2a;
                border-radius: 8px;
                padding: 15px;
            }
        """)
        packet_lo = QVBoxLayout(packet_card)
        packet_lo.addWidget(QLabel("Packet Statistics"))
        self.debug_packet_count = QLabel("Sent: 0")
        self.debug_packet_count.setStyleSheet("font-size: 14px; font-weight: bold; color: #10b981;")
        packet_lo.addWidget(self.debug_packet_count)
        self.debug_packet_rate = QLabel("Rate: 0/sec")
        self.debug_packet_rate.setStyleSheet("color: #808080; font-size: 11px;")
        packet_lo.addWidget(self.debug_packet_rate)
        cards.addWidget(packet_card)
        
        # Clock status card
        clock_card = QFrame()
        clock_card.setStyleSheet("""
            QFrame {
                background: #1a1a1a;
                border: 1px solid #2a2a2a;
                border-radius: 8px;
                padding: 15px;
            }
        """)
        clock_lo = QVBoxLayout(clock_card)
        clock_lo.addWidget(QLabel("System Status"))
        self.debug_clock_status = QLabel("⏱️ Clock: Stopped")
        self.debug_clock_status.setStyleSheet("font-size: 14px; font-weight: bold; color: #f97316;")
        clock_lo.addWidget(self.debug_clock_status)
        self.debug_shot_status = QLabel("🔴 Shot: Stopped")
        self.debug_shot_status.setStyleSheet("color: #808080; font-size: 11px;")
        clock_lo.addWidget(self.debug_shot_status)
        cards.addWidget(clock_card)
        
        layout.addLayout(cards)
        
        # Live console
        console_label = QLabel("📊 Live Packet Monitor")
        console_label.setStyleSheet("font-weight: bold; margin-top: 10px;")
        layout.addWidget(console_label)
        
        self.console = QTextEdit()
        self.console.setReadOnly(True)
        self.console.setStyleSheet("""
            QTextEdit {
                background: #0d1117;
                border: 1px solid #2a2a2a;
                border-radius: 6px;
                color: #c9d1d9;
                font-family: 'Courier New', monospace;
                font-size: 12px;
                padding: 10px;
            }
        """)
        layout.addWidget(self.console, 1)
        
        # Control buttons
        btn_row = QHBoxLayout()
        
        clear_btn = QPushButton("🗑️ Clear Console")
        clear_btn.clicked.connect(lambda: self.console.clear())
        btn_row.addWidget(clear_btn)
        
        test_btn = QPushButton("📤 Send Test Packet")
        test_btn.clicked.connect(self._send_test_packet)
        btn_row.addWidget(test_btn)
        
        reconnect_btn = QPushButton("🔄 Reconnect Serial")
        reconnect_btn.clicked.connect(self._connect_serial)
        btn_row.addWidget(reconnect_btn)
        
        btn_row.addStretch()
        layout.addLayout(btn_row)
        
        return widget
    
    def _create_settings_tab(self):
        """Connection and system settings"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setContentsMargins(20, 20, 20, 20)
        layout.setSpacing(15)
        
        title = QLabel("⚙ System Settings")
        title.setStyleSheet("font-size: 18px; font-weight: bold; color: #3b82f6; padding: 10px;")
        layout.addWidget(title)
        
        # Serial port selection
        serial_grp = QGroupBox("Serial Port Configuration")
        serial_lo = QVBoxLayout(serial_grp)
        
        port_row = QHBoxLayout()
        port_row.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumHeight(35)
        self._refresh_ports()
        port_row.addWidget(self.port_combo, 1)
        
        refresh_btn = QPushButton("🔄 Refresh")
        refresh_btn.clicked.connect(self._refresh_ports)
        port_row.addWidget(refresh_btn)
        serial_lo.addLayout(port_row)
        
        baud_row = QHBoxLayout()
        baud_row.addWidget(QLabel("Baud Rate:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["9600", "19200", "38400", "57600", "115200"])
        self.baud_combo.setCurrentText("115200")
        self.baud_combo.setMinimumHeight(35)
        baud_row.addWidget(self.baud_combo, 1)
        serial_lo.addLayout(baud_row)
        
        apply_btn = QPushButton("✓ Apply and Connect")
        apply_btn.setStyleSheet("background: #10b981; font-weight: bold; padding: 12px;")
        apply_btn.clicked.connect(self._apply_settings)
        serial_lo.addWidget(apply_btn)
        
        layout.addWidget(serial_grp)
        
        # Game settings
        game_grp = QGroupBox("Game Settings")
        game_lo = QVBoxLayout(game_grp)
        
        qtr_row = QHBoxLayout()
        qtr_row.addWidget(QLabel("Quarter Duration:"))
        self.qtr_spin = QSpinBox()
        self.qtr_spin.setRange(1, 30)
        self.qtr_spin.setValue(self.qtr_mins)
        self.qtr_spin.setSuffix(" minutes")
        self.qtr_spin.setMinimumHeight(35)
        qtr_row.addWidget(self.qtr_spin, 1)
        game_lo.addLayout(qtr_row)
        
        layout.addWidget(game_grp)
        
        # System info
        info_grp = QGroupBox("System Information")
        info_lo = QVBoxLayout(info_grp)
        
        info_text = f"""
        <b>GPIO Status:</b> {'✓ Active' if GPIO_OK else '✗ Inactive'}<br>
        <b>Python Version:</b> {sys.version.split()[0]}<br>
        <b>PyQt5:</b> Installed<br>
        <b>Available Ports:</b> {len(serial.tools.list_ports.comports())}
        """
        info_lbl = QLabel(info_text)
        info_lbl.setStyleSheet("padding: 10px; background: #1a1a1a; border-radius: 6px;")
        info_lo.addWidget(info_lbl)
        
        layout.addWidget(info_grp)
        
        layout.addStretch()
        
        return widget
    
    def _create_team(self, side):
        grp = QGroupBox(f"Team {side}")
        lo = QVBoxLayout(grp)
        
        name = QLineEdit(self.team_a if side == "A" else self.team_b)
        name.setMaxLength(15)
        name.setAlignment(Qt.AlignCenter)
        if side == "A":
            self.team_a_edit = name
            name.textChanged.connect(lambda t: setattr(self, "team_a", t))
        else:
            self.team_b_edit = name
            name.textChanged.connect(lambda t: setattr(self, "team_b", t))
        lo.addWidget(name)
        
        score = QLabel("0")
        score.setAlignment(Qt.AlignCenter)
        score.setStyleSheet("font-size: 72px; font-weight: bold; color: #fbbf24;")
        if side == "A":
            self.score_a_lbl = score
        else:
            self.score_b_lbl = score
        lo.addWidget(score)
        
        btns = QHBoxLayout()
        for pts in [3, 2, 1]:
            b = QPushButton(f"+{pts}")
            b.setStyleSheet("background: #1565c0; font-weight: bold;")
            b.clicked.connect(lambda _, p=pts, s=side: self._add_score(s, p))
            btns.addWidget(b)
        lo.addLayout(btns)
        
        ctrl = QHBoxLayout()
        m1 = QPushButton("-1")
        m1.setStyleSheet("background: #dc2626;")
        m1.clicked.connect(lambda _, s=side: self._add_score(s, -1))
        rst = QPushButton("RST")
        rst.clicked.connect(lambda _, s=side: self._reset_score(s))
        ctrl.addWidget(m1)
        ctrl.addWidget(rst)
        lo.addLayout(ctrl)
        
        foul_row = QHBoxLayout()
        foul_row.addWidget(QLabel("Fouls:"))
        foul_lbl = QLabel("0")
        foul_lbl.setStyleSheet("font-size: 18px; font-weight: bold;")
        if side == "A":
            self.fouls_a_lbl = foul_lbl
        else:
            self.fouls_b_lbl = foul_lbl
        foul_row.addWidget(foul_lbl)
        fp = QPushButton("+")
        fp.setFixedWidth(30)
        fp.clicked.connect(lambda _, s=side: self._delta_fouls(s, 1))
        fm = QPushButton("-")
        fm.setFixedWidth(30)
        fm.clicked.connect(lambda _, s=side: self._delta_fouls(s, -1))
        foul_row.addWidget(fp)
        foul_row.addWidget(fm)
        lo.addLayout(foul_row)
        
        return grp
    
    def _create_center(self):
        grp = QGroupBox("Control")
        lo = QVBoxLayout(grp)
        
        # Clock
        lo.addWidget(QLabel("GAME CLOCK"))
        self.clock_lbl = QLabel("10:00")
        self.clock_lbl.setAlignment(Qt.AlignCenter)
        self.clock_lbl.setStyleSheet("font-size: 48px; font-weight: bold; color: #10b981;")
        lo.addWidget(self.clock_lbl)
        
        clock_btns = QHBoxLayout()
        self.clk_start = QPushButton("▶ START")
        self.clk_start.setStyleSheet("background: #10b981; font-weight: bold;")
        self.clk_start.clicked.connect(self._start_clock)
        self.clk_stop = QPushButton("■ STOP")
        self.clk_stop.setStyleSheet("background: #dc2626; font-weight: bold;")
        self.clk_stop.clicked.connect(self._stop_clock)
        clock_btns.addWidget(self.clk_start)
        clock_btns.addWidget(self.clk_stop)
        lo.addLayout(clock_btns)
        
        # Shot
        lo.addWidget(QLabel("SHOT CLOCK"))
        self.shot_lbl = QLabel("24")
        self.shot_lbl.setAlignment(Qt.AlignCenter)
        self.shot_lbl.setStyleSheet("font-size: 36px; font-weight: bold; color: #f97316;")
        lo.addWidget(self.shot_lbl)
        
        shot_btns = QHBoxLayout()
        b24 = QPushButton("24")
        b24.setStyleSheet("background: #f97316;")
        b24.clicked.connect(lambda: self._reset_shot(24))
        b14 = QPushButton("14")
        b14.setStyleSheet("background: #ea580c;")
        b14.clicked.connect(lambda: self._reset_shot(14))
        shot_btns.addWidget(b24)
        shot_btns.addWidget(b14)
        lo.addLayout(shot_btns)
        
        shot_ctrl = QHBoxLayout()
        self.shot_start = QPushButton("▶")
        self.shot_start.setStyleSheet("background: #10b981;")
        self.shot_start.clicked.connect(self._start_shot)
        self.shot_stop = QPushButton("■")
        self.shot_stop.setStyleSheet("background: #dc2626;")
        self.shot_stop.clicked.connect(self._stop_shot)
        shot_ctrl.addWidget(self.shot_start)
        shot_ctrl.addWidget(self.shot_stop)
        lo.addLayout(shot_ctrl)
        
        # Quarter
        qtr_row = QHBoxLayout()
        qtr_row.addWidget(QLabel("QTR:"))
        self.qtr_lbl = QLabel("1")
        self.qtr_lbl.setStyleSheet("font-size: 24px; font-weight: bold;")
        qtr_row.addWidget(self.qtr_lbl)
        for lbl, v in [("Q1", 1), ("Q2", 2), ("Q3", 3), ("Q4", 4), ("OT", 5)]:
            b = QPushButton(lbl)
            b.setFixedWidth(40)
            b.clicked.connect(lambda _, x=v: self._set_quarter(x))
            qtr_row.addWidget(b)
        lo.addLayout(qtr_row)
        
        # Possession
        poss_row = QHBoxLayout()
        poss_row.addWidget(QLabel("POSS:"))
        self.poss_a = QPushButton("A")
        self.poss_b = QPushButton("B")
        self.poss_non = QPushButton("—")
        for b in [self.poss_a, self.poss_b, self.poss_non]:
            b.setCheckable(True)
            poss_row.addWidget(b)
        self.poss_non.setChecked(True)
        self.poss_a.clicked.connect(lambda: self._set_poss("A"))
        self.poss_b.clicked.connect(lambda: self._set_poss("B"))
        self.poss_non.clicked.connect(lambda: self._set_poss(" "))
        lo.addLayout(poss_row)
        
        return grp
    
    # Control methods
    def _start_clock(self):
        self.clock_running = True
        self._log("🟢 Game clock started")
        self._send()
    
    def _stop_clock(self):
        self.clock_running = False
        self._log("🔴 Game clock stopped")
        self._send()
    
    def _start_shot(self):
        self.shot_running = True
        self._log("🟢 Shot clock started")
        self._send()
    
    def _stop_shot(self):
        self.shot_running = False
        self._log("🔴 Shot clock stopped")
        self._send()
    
    def _reset_shot(self, secs):
        self.shot_secs = secs
        self.shot_tenths = 0
        self._refresh_shot()
        self._log(f"🔄 Shot clock reset to {secs}")
        self._send()
    
    def _add_score(self, s, p):
        if s == "A":
            self.score_a = max(0, self.score_a + p)
            self.score_a_lbl.setText(str(self.score_a))
            self._log(f"⚡ Team A score: {self.score_a} ({p:+d})")
        else:
            self.score_b = max(0, self.score_b + p)
            self.score_b_lbl.setText(str(self.score_b))
            self._log(f"⚡ Team B score: {self.score_b} ({p:+d})")
        self._send()
    
    def _reset_score(self, s):
        if s == "A":
            self.score_a = 0
            self.score_a_lbl.setText("0")
            self._log("🔄 Team A score reset")
        else:
            self.score_b = 0
            self.score_b_lbl.setText("0")
            self._log("🔄 Team B score reset")
        self._send()
    
    def _delta_fouls(self, s, d):
        if s == "A":
            self.fouls_a = max(0, self.fouls_a + d)
            self.fouls_a_lbl.setText(str(self.fouls_a))
        else:
            self.fouls_b = max(0, self.fouls_b + d)
            self.fouls_b_lbl.setText(str(self.fouls_b))
        self._send()
    
    def _set_quarter(self, v):
        self.quarter = v
        self.qtr_lbl.setText(str(v) if v <= 4 else "OT")
        self._log(f"📊 Quarter changed to {self.qtr_lbl.text()}")
        self._send()
    
    def _set_poss(self, v):
        self.possession = v
        self.poss_a.setChecked(v == "A")
        self.poss_b.setChecked(v == "B")
        self.poss_non.setChecked(v == " ")
        self._send()
    
    def _refresh_clock(self):
        s, t = self.clock_secs, self.clock_tenths
        text = f"{s//60}:{s%60:02d}" if s >= 60 else f"{s}.{t}"
        self.clock_lbl.setText(text)
        
        # Update debug status
        if hasattr(self, 'debug_clock_status'):
            status = "⏱️ Clock: Running" if self.clock_running else "⏱️ Clock: Stopped"
            color = "#10b981" if self.clock_running else "#f97316"
            self.debug_clock_status.setText(status)
            self.debug_clock_status.setStyleSheet(f"font-size: 14px; font-weight: bold; color: {color};")
    
    def _refresh_shot(self):
        s, t = self.shot_secs, self.shot_tenths
        text = f"{s}.{t}" if s < 10 else str(s)
        self.shot_lbl.setText(text)
        
        # Update debug status
        if hasattr(self, 'debug_shot_status'):
            status = "🔴 Shot: Running" if self.shot_running else "🔴 Shot: Stopped"
            self.debug_shot_status.setText(status)
    
    def _tick(self):
        changed = False
        
        if self.clock_running and (self.clock_secs > 0 or self.clock_tenths > 0):
            self.clock_tenths -= 1
            if self.clock_tenths < 0:
                self.clock_tenths = 9
                self.clock_secs -= 1
            if self.clock_secs <= 0 and self.clock_tenths <= 0:
                self.clock_secs = 0
                self.clock_tenths = 0
                self.clock_running = False
                self._log("⏰ Game clock expired")
            self._refresh_clock()
            changed = True
        
        if self.shot_running and (self.shot_secs > 0 or self.shot_tenths > 0):
            self.shot_tenths -= 1
            if self.shot_tenths < 0:
                self.shot_tenths = 9
                self.shot_secs -= 1
            if self.shot_secs <= 0 and self.shot_tenths <= 0:
                self.shot_secs = 0
                self.shot_tenths = 0
                self.shot_running = False
                self._log("⏰ Shot clock expired")
            self._refresh_shot()
            changed = True
        
        if changed:
            self._send()
    
    def _refresh_ports(self):
        """Refresh available serial ports"""
        self.port_combo.clear()
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            self.port_combo.addItems(ports)
            self._log(f"📡 Found {len(ports)} serial port(s)")
        else:
            self.port_combo.addItem("No ports found")
            self._log("⚠️ No serial ports detected")
    
    def _apply_settings(self):
        """Apply settings and reconnect"""
        self.port = self.port_combo.currentText()
        self._connect_serial()
    
    def _connect_serial(self):
        """Connect to ESP32 Master via serial"""
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
            self._log("🔌 Disconnected from serial port")
        
        ports = [p.device for p in serial.tools.list_ports.comports()]
        
        if not ports:
            self._log("❌ No serial ports found!")
            self.serial_lbl.setText("⚠ No Ports Found")
            self.serial_lbl.setStyleSheet("color: #ef4444; font-weight: bold; padding: 8px;")
            if hasattr(self, 'debug_serial_status'):
                self.debug_serial_status.setText("❌ No Ports")
                self.debug_serial_status.setStyleSheet("color: #ef4444; font-weight: bold;")
            return
        
        for port in ports:
            try:
                self._log(f"🔍 Trying port: {port}")
                self.serial_port = serial.Serial(port, 115200, timeout=0.1)
                self.port = port
                
                self.serial_lbl.setText(f"✓ Connected: {port}")
                self.serial_lbl.setStyleSheet("color: #10b981; font-weight: bold; padding: 8px;")
                
                if hasattr(self, 'debug_serial_status'):
                    self.debug_serial_status.setText("✅ Connected")
                    self.debug_serial_status.setStyleSheet("color: #10b981; font-weight: bold;")
                    self.debug_port_lbl.setText(f"Port: {port}")
                
                self._log(f"✅ Serial connected: {port} @ 115200 baud")
                self._send()  # Send initial state
                return
            except Exception as e:
                self._log(f"❌ Failed {port}: {str(e)}")
        
        self._log("❌ Could not connect to any port")
        self.serial_lbl.setText("⚠ Connection Failed")
        self.serial_lbl.setStyleSheet("color: #ef4444; font-weight: bold; padding: 8px;")
    
    def _send(self):
        """Send packet to ESP32 Master"""
        if not self.serial_port or not self.serial_port.is_open:
            self._log("⚠️ Cannot send - serial not connected", level="warn")
            return
        
        # Send names packet first (only when changed)
        names_packet = f"N,{self.event_name},{self.team_a},{self.team_b}\n"
        
        # Send score packet
        poss = self.possession if self.possession in ("A", "B") else "N"
        packet = (f"S,{self.score_a},{self.score_b},"
                 f"{self.clock_secs},{self.clock_tenths},"
                 f"{self.quarter},{poss},"
                 f"{self.fouls_a},{self.fouls_b},"
                 f"{self.screen_mask},"
                 f"{1 if self.clock_running else 0},"
                 f"{self.shot_secs},{self.shot_tenths}\n")
        
        try:
            # Send names first
            self.serial_port.write(names_packet.encode())
            self.serial_port.flush()
            
            # Then send scores
            self.serial_port.write(packet.encode())
            self.serial_port.flush()
            
            self.packets_sent += 1
            self.last_packet = packet.strip()
            
            # Update UI
            self.packets_lbl.setText(f"Packets Sent: {self.packets_sent}")
            if hasattr(self, 'debug_packet_count'):
                self.debug_packet_count.setText(f"Sent: {self.packets_sent}")
            
            self._log(f"📤 Names: {names_packet.strip()}", level="packet")
            self._log(f"📤 Scores: {packet.strip()}", level="packet")
            
        except Exception as e:
            self._log(f"❌ Serial write error: {str(e)}", level="error")
    
    def _send_test_packet(self):
        """Send a test packet"""
        self._log("🧪 Sending test packet...")
        self._send()
    
    def _log(self, message, level="info"):
        """Add message to debug console"""
        if not hasattr(self, 'console'):
            return
        
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        
        # Color coding
        colors = {
            "info": "#58a6ff",
            "success": "#3fb950",
            "warn": "#d29922",
            "error": "#f85149",
            "packet": "#8b949e"
        }
        color = colors.get(level, "#c9d1d9")
        
        formatted = f'<span style="color: #8b949e;">[{timestamp}]</span> <span style="color: {color};">{message}</span>'
        self.console.append(formatted)
        
        # Auto-scroll
        self.console.moveCursor(QTextCursor.End)
        
        # Limit console to 1000 lines
        doc = self.console.document()
        if doc.blockCount() > 1000:
            cursor = QTextCursor(doc.findBlockByNumber(0))
            cursor.select(QTextCursor.BlockUnderCursor)
            cursor.removeSelectedText()
            cursor.deleteChar()
    
    def closeEvent(self, event):
        if GPIO_OK:
            try:
                GPIO.cleanup()
            except:
                pass
        if self.serial_port:
            self.serial_port.close()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setFont(QFont("Arial", 10))
    win = ScoreboardWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
