#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")}"
APP_PATH="$ROOT_DIR/build/Mac Fan Controller.app"
APP_BIN="$APP_PATH/Contents/MacOS/Mac Fan Controller"
HELPER_PATH="$ROOT_DIR/build/macfanctl"
PKG_PATH="$ROOT_DIR/dist/MacFanController-$VERSION.pkg"
ZIP_PATH="$ROOT_DIR/dist/MacFanController-$VERSION-Installer.zip"

"$ROOT_DIR/scripts/package_release.sh"

plutil -lint "$APP_PATH/Contents/Info.plist"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

echo "App architectures: $(lipo -archs "$APP_BIN" 2>/dev/null || true)"
echo "Helper architectures: $(lipo -archs "$HELPER_PATH" 2>/dev/null || true)"

STATUS_FILE="$(mktemp)"
cleanup_status_file() {
    rm -f "$STATUS_FILE"
}
trap cleanup_status_file EXIT
if "$HELPER_PATH" status > "$STATUS_FILE"; then
    HELPER_EXIT=0
else
    HELPER_EXIT=$?
fi

/usr/bin/python3 - "$STATUS_FILE" "$HELPER_EXIT" <<'PY'
import json
import sys

path = sys.argv[1]
helper_exit = int(sys.argv[2])

try:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
except Exception as exc:
    raise SystemExit(f"helper did not emit valid JSON: {exc}")

if "ok" not in data or not isinstance(data["ok"], bool):
    raise SystemExit("helper JSON is missing boolean ok")
if not isinstance(data.get("fans", []), list):
    raise SystemExit("helper JSON fans must be an array")
if not isinstance(data.get("parameters", []), list):
    raise SystemExit("helper JSON parameters must be an array")
if "system" not in data or not isinstance(data["system"], dict):
    raise SystemExit("helper JSON is missing system object")

profile = data.get("hardwareProfile")
strategy = profile.get("controlStrategy") if isinstance(profile, dict) else "unknown"
print("helper ok:", data["ok"], "fans:", len(data.get("fans", [])), "strategy:", strategy)

if helper_exit != 0 and data["ok"]:
    raise SystemExit(f"helper exited {helper_exit} but reported ok")

if not data["ok"]:
    message = data.get("message") or data.get("error") or "diagnostic status"
    print(f"helper reported non-controllable hardware/environment: {message}")
PY

cleanup_status_file
trap - EXIT

pkgutil --payload-files "$PKG_PATH" | grep -F "Applications/Mac Fan Controller.app/Contents/MacOS/Mac Fan Controller" >/dev/null
pkgutil --payload-files "$PKG_PATH" | grep -F "Library/PrivilegedHelperTools/com.codex.macfanctl" >/dev/null

unzip -tq "$ZIP_PATH" >/dev/null
unzip -l "$ZIP_PATH" | grep -F "INSTALL.md" >/dev/null
unzip -l "$ZIP_PATH" | grep -F "MODEL_SUPPORT.md" >/dev/null

(
    cd "$ROOT_DIR/dist/MacFanController-$VERSION-Installer"
    LC_ALL=C LANG=C shasum -a 256 -c SHA256SUMS.txt
)

echo "Verification passed."
