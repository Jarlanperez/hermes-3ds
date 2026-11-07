#!/bin/sh
# Keep the Hermes 3DS relay running on macOS (starts at login, restarts if it stops).
# Usage: sh relay/install-macos.sh      Uninstall: sh relay/install-macos.sh --uninstall
set -e
LABEL=com.hermes3ds.relay
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/$LABEL.log"
RELAY="$(cd "$(dirname "$0")" && pwd)/relay.py"
PY=$(command -v python3)

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
if [ "$1" = "--uninstall" ]; then rm -f "$PLIST"; echo "Removed."; exit 0; fi

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$PY</string><string>$RELAY</string></array>
  <key>EnvironmentVariables</key><dict><key>HOME</key><string>$HOME</string></dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>15</integer>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
EOF
launchctl bootstrap "gui/$(id -u)" "$PLIST"
sleep 3
tail -3 "$LOG"
