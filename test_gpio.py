#!/usr/bin/env python3
"""
GPIO Button Test — run this from terminal on the Pi:
    python3 test_gpio.py

Connect your button: one leg to GND (e.g. pin 9), other leg to GPIO 17 (pin 11).
Press the button — you should see "PRESSED" in the terminal.
Press Ctrl+C to quit.
"""

print("=" * 50)
print("  GPIO Button Test")
print("=" * 50)

# ── Test 1: can we import gpiozero? ──────────────────
try:
    from gpiozero import Button
    print("[OK ] gpiozero imported successfully")
except ImportError as e:
    print(f"[FAIL] gpiozero import failed: {e}")
    print("       Fix: sudo apt install python3-gpiozero")
    exit(1)

# ── Test 2: can we open GPIO 17? ─────────────────────
PIN = 17
try:
    btn = Button(PIN, pull_up=True, bounce_time=0.05)
    print(f"[OK ] GPIO {PIN} (physical pin 11) opened — pull-up ON")
except Exception as e:
    print(f"[FAIL] Could not open GPIO {PIN}: {e}")
    print("       Fix: sudo usermod -a -G gpio $USER  then reboot")
    exit(1)

# ── Test 3: live press detection ─────────────────────
from signal import pause

press_count = [0]

def on_press():
    press_count[0] += 1
    print(f"  >>> BUTTON PRESSED (count: {press_count[0]}) <<<")

def on_release():
    print(f"      button released")

btn.when_pressed  = on_press
btn.when_released = on_release

print()
print(f"Watching GPIO {PIN} — press your button now. Ctrl+C to quit.")
print("-" * 50)

try:
    pause()
except KeyboardInterrupt:
    btn.close()
    print(f"\nDone. Total presses detected: {press_count[0]}")
