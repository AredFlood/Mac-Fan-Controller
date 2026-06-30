#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$ROOT_DIR/dist"
APP_NAME="Mac Fan Controller"
APP_DIR="$BUILD_DIR/$APP_NAME.app"
CONTENTS_DIR="$APP_DIR/Contents"
PKGROOT_DIR="$BUILD_DIR/pkgroot"
INTERMEDIATE_DIR="$BUILD_DIR/intermediate"
VERSION="${VERSION:-$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")}"
ARCHS="${ARCHS:-arm64 x86_64}"

MACOS_MIN_VERSION="${MACOS_MIN_VERSION:-13.0}"

export COPYFILE_DISABLE=1

rm -rf "$BUILD_DIR" "$DIST_DIR"
mkdir -p "$CONTENTS_DIR/MacOS" "$CONTENTS_DIR/Resources" "$PKGROOT_DIR/Applications" "$PKGROOT_DIR/Library/PrivilegedHelperTools" "$DIST_DIR" "$INTERMEDIATE_DIR"

echo "Building version $VERSION for architectures: $ARCHS"

HELPER_SLICES=()
APP_SLICES=()

for ARCH in $ARCHS; do
    TARGET="$ARCH-apple-macosx$MACOS_MIN_VERSION"
    HELPER_SLICE="$INTERMEDIATE_DIR/macfanctl-$ARCH"
    APP_SLICE="$INTERMEDIATE_DIR/$APP_NAME-$ARCH"

    echo "Building macfanctl helper for $TARGET"
    clang \
        -O2 \
        -Wall \
        -Wextra \
        -target "$TARGET" \
        -mmacosx-version-min="$MACOS_MIN_VERSION" \
        "$ROOT_DIR/Sources/MacFanCtl/main.c" \
        -framework IOKit \
        -framework CoreFoundation \
        -lm \
        -o "$HELPER_SLICE"

    echo "Building SwiftUI app for $TARGET"
    swiftc \
        -O \
        -parse-as-library \
        -target "$TARGET" \
        "$ROOT_DIR/Sources/MacFanApp/MacFanApp.swift" \
        -framework SwiftUI \
        -framework AppKit \
        -o "$APP_SLICE"

    HELPER_SLICES+=("$HELPER_SLICE")
    APP_SLICES+=("$APP_SLICE")
done

if [ "${#HELPER_SLICES[@]}" -eq 1 ]; then
    cp "${HELPER_SLICES[0]}" "$BUILD_DIR/macfanctl"
    cp "${APP_SLICES[0]}" "$CONTENTS_DIR/MacOS/$APP_NAME"
else
    lipo -create "${HELPER_SLICES[@]}" -output "$BUILD_DIR/macfanctl"
    lipo -create "${APP_SLICES[@]}" -output "$CONTENTS_DIR/MacOS/$APP_NAME"
fi

cp "$ROOT_DIR/Info.plist" "$CONTENTS_DIR/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$CONTENTS_DIR/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$CONTENTS_DIR/Info.plist"
printf "APPL????" > "$CONTENTS_DIR/PkgInfo"
cp "$BUILD_DIR/macfanctl" "$CONTENTS_DIR/Resources/macfanctl"
chmod 755 "$CONTENTS_DIR/MacOS/$APP_NAME" "$CONTENTS_DIR/Resources/macfanctl"
plutil -lint "$CONTENTS_DIR/Info.plist"

if command -v codesign >/dev/null 2>&1; then
    codesign --force --sign - "$BUILD_DIR/macfanctl" >/dev/null
    codesign --force --deep --sign - "$APP_DIR" >/dev/null
fi

ditto --noextattr --norsrc "$APP_DIR" "$PKGROOT_DIR/Applications/$APP_NAME.app"
install -m 755 "$BUILD_DIR/macfanctl" "$PKGROOT_DIR/Library/PrivilegedHelperTools/com.codex.macfanctl"

if command -v xattr >/dev/null 2>&1; then
    xattr -cr "$PKGROOT_DIR"
fi

chmod +x "$ROOT_DIR/packaging/scripts/postinstall"
chmod +x "$ROOT_DIR/packaging/scripts/preinstall"

echo "Building pkg"
pkgbuild \
    --root "$PKGROOT_DIR" \
    --component-plist "$ROOT_DIR/packaging/component.plist" \
    --scripts "$ROOT_DIR/packaging/scripts" \
    --filter '\.DS_Store$' \
    --filter '(^|/)\.svn($|/)' \
    --filter '(^|/)CVS($|/)' \
    --filter '(^|/)\._' \
    --filter '(^|/)\.__' \
    --identifier "com.codex.macfancontroller" \
    --version "$VERSION" \
    --install-location "/" \
    "$DIST_DIR/MacFanController-$VERSION.pkg"

echo "Built:"
echo "  $APP_DIR"
echo "  $DIST_DIR/MacFanController-$VERSION.pkg"
echo "Architectures:"
lipo -archs "$CONTENTS_DIR/MacOS/$APP_NAME" 2>/dev/null || file "$CONTENTS_DIR/MacOS/$APP_NAME"
