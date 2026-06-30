#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")}"
DIST_DIR="$ROOT_DIR/dist"
PACKAGE_NAME="MacFanController-$VERSION"
RELEASE_DIR="$DIST_DIR/$PACKAGE_NAME-Installer"
ZIP_PATH="$DIST_DIR/$PACKAGE_NAME-Installer.zip"

"$ROOT_DIR/scripts/build.sh"

rm -rf "$RELEASE_DIR" "$ZIP_PATH"
mkdir -p "$RELEASE_DIR"

cp "$DIST_DIR/$PACKAGE_NAME.pkg" "$RELEASE_DIR/"
cp "$ROOT_DIR/docs/INSTALL.md" "$RELEASE_DIR/"
cp "$ROOT_DIR/docs/MODEL_SUPPORT.md" "$RELEASE_DIR/"
cp "$ROOT_DIR/RELEASE_NOTES.md" "$RELEASE_DIR/"
cp "$ROOT_DIR/LICENSE" "$RELEASE_DIR/"

(
    cd "$RELEASE_DIR"
    LC_ALL=C LANG=C shasum -a 256 "$PACKAGE_NAME.pkg" > SHA256SUMS.txt
)

(
    cd "$DIST_DIR"
    /usr/bin/zip -qry -X "$ZIP_PATH" "$PACKAGE_NAME-Installer"
)

echo "Built release package:"
echo "  $ZIP_PATH"
echo "Contents:"
find "$RELEASE_DIR" -maxdepth 1 -type f -print | sort
