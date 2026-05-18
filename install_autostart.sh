#!/bin/bash
# Run this ONCE on the Raspberry Pi to set up autostart.
# Usage:  bash install_autostart.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AUTOSTART_DIR="$HOME/.config/autostart"

mkdir -p "$AUTOSTART_DIR"

cat > "$AUTOSTART_DIR/scoreboard.desktop" << EOF
[Desktop Entry]
Type=Application
Name=PointiQ
Exec=bash -c "sleep 6 && python3 $SCRIPT_DIR/scoreboard_v5.py"
X-GNOME-Autostart-enabled=true
EOF

echo "Autostart installed → $AUTOSTART_DIR/scoreboard.desktop"
echo "Reboot to test: sudo reboot"
