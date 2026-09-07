#!/usr/bin/env bash
set -euo pipefail

# Parse optional arguments
CLEAN_BUILD=false
INSTALL_DEPS=false

for arg in "$@"; do
  case $arg in
    --clean) CLEAN_BUILD=true ;;
    --deps)  INSTALL_DEPS=true ;;
  esac
done

# ==============================================================================
# CONFIGURATION & PATHS
# ==============================================================================
WINE_SRC_DIR="$(cd "./" && pwd)"
BUILD_DIR="${WINE_SRC_DIR}/wine"
INSTALL_PREFIX="/tmp/wine_build"
WINEVER="10.17"
OUTPUT_WCP="/mnt/d/winlator/wine-${WINEVER}-custom.wcp"

echo "================================================================="
echo "==> Starting Wine WOW64 Build Pipeline for Winlator..."
echo "================================================================="

# 1. Clear Conflict Environment Variables
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR

# 2. Install Build Dependencies (Only when --deps is passed)
if [ "${INSTALL_DEPS}" = true ]; then
  echo "==> Installing Wine host & cross-compiler dependencies..."
  sudo apt-get update && sudo apt-get install -y \
    build-essential bison flex pkg-config \
    gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 mingw-w64 \
    libfreetype-dev libfontconfig1-dev libgl1-mesa-dev libglu1-mesa-dev \
    libvulkan-dev libsdl2-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev libgstreamer-plugins-bad1.0-dev \
    libasound2-dev libpulse-dev libgnutls28-dev libmpg123-dev \
    libopenal-dev libpng-dev libjpeg-dev libtiff-dev libwebp-dev liblcms2-dev \
    libxml2-dev libxslt1-dev libx11-dev libxcursor-dev libxi-dev \
    libxrandr-dev libxrender-dev tar xz-utils zstd libkrb5-dev libgssapi-krb5-2 krb5-config
fi

# 3. Setup Directories
if [ "${CLEAN_BUILD}" = true ]; then
  echo "==> [--clean passed] Cleaning build and install directories..."
  rm -rf "${BUILD_DIR}" "${INSTALL_PREFIX}"
fi

mkdir -p "${BUILD_DIR}" "${INSTALL_PREFIX}"
cd "${BUILD_DIR}"

# 4. Run ./configure ONLY if Makefile does not exist
if [ ! -f "Makefile" ]; then
  echo "==> Running Wine ./configure..."
  ../configure --prefix="${INSTALL_PREFIX}" \
    --enable-archs=i386,x86_64 \
    --enable-win64 \
    --with-mingw \
    --with-freetype \
    --with-fontconfig \
    --with-gstreamer \
    --with-vulkan \
    --with-opengl \
    --with-sdl \
    --with-gnutls \
    --with-xrandr \
    --with-xrender \
    --with-gssapi \
    --with-krb5 \
    --enable-tools \
    --disable-tests \
    --disable-win16 \
    --without-unwind \
    --without-dbus \
    --without-inotify \
    --without-netapi \
    --without-xshape \
    --without-xxf86vm \
    --without-xshm \
    --without-xcomposite \
    --without-xfixes \
    --without-xinerama \
    --without-capi \
    --without-coreaudio \
    --without-cups \
    --without-gphoto \
    --without-oss \
    --without-pcap \
    --without-pcsclite \
    --without-sane \
    --without-udev \
    --without-usb \
    --without-v4l2 \
    --without-wayland \
    --without-ffmpeg \
    --without-opencl \
    --without-vosk \
    --without-gettext \
    --with-gettextpo=no
fi

# 5. Incremental Compilation Across All CPU Cores
NPROC=$(nproc)
echo "==> Compiling Wine using ${NPROC} threads..."
make -j"${NPROC}"

# 6. Install Binaries
echo "==> Installing binaries to ${INSTALL_PREFIX}..."
make install STRIP=true

# 7. Aggressive Pruning & Stripping (Reduces package to ~100-130MB)
echo "==> Pruning static libraries, def files, and manuals..."
find "${INSTALL_PREFIX}" -type f \( -name "*.a" -o -name "*.def" \) -delete
rm -rf "${INSTALL_PREFIX}/share/man"
rm -rf "${INSTALL_PREFIX}/share/doc"

echo "==> Stripping Linux host binaries..."
find "${INSTALL_PREFIX}" -name "*.so*" -exec strip --strip-unneeded {} + 2>/dev/null || true

echo "==> Stripping Windows MinGW PE binaries..."
find "${INSTALL_PREFIX}" -name "*.dll" -exec x86_64-w64-mingw32-strip --strip-unneeded {} + 2>/dev/null || true
find "${INSTALL_PREFIX}" -name "*.exe" -exec x86_64-w64-mingw32-strip --strip-unneeded {} + 2>/dev/null || true
find "${INSTALL_PREFIX}" -name "*.dll" -exec i686-w64-mingw32-strip --strip-unneeded {} + 2>/dev/null || true
find "${INSTALL_PREFIX}" -name "*.exe" -exec i686-w64-mingw32-strip --strip-unneeded {} + 2>/dev/null || true

# 8. Verification Steps
echo "==> Verifying PE architecture output..."
if [ -f "${INSTALL_PREFIX}/lib/wine/i386-windows/ntdll.dll" ] && [ -f "${INSTALL_PREFIX}/lib/wine/x86_64-windows/ntdll.dll" ]; then
  echo "SUCCESS: Both i386-windows and x86_64-windows PE modules verified!"
else
  echo "ERROR: Missing required PE ntdll.dll binaries."
  exit 1
fi

# 9. Inject Extra Package Assets
echo "==> Injecting profile.json and prefixPack.tzst..."

# Create profile.json dynamically with variable expansion
cat > "${WINE_SRC_DIR}/profile.json" <<EOF
{
    "type": "Wine",
    "versionName": "${WINEVER}-x86_64",
    "versionCode": 0,
    "description": "Wine ${WINEVER} x86_64 - Windows compatibility layer with improved gaming support",
    "files": [],
    "wine": {
        "binPath": "bin",
        "libPath": "lib",
        "prefixPack": "prefixPack.tzst"
    }
}
EOF


cp -v "${WINE_SRC_DIR}/profile.json" "${INSTALL_PREFIX}/"
[ -f "${WINE_SRC_DIR}/prefixPack.tzst" ] && cp -v "${WINE_SRC_DIR}/prefixPack.tzst" "${INSTALL_PREFIX}/"

# 10. Compress Package into .wcp
echo "==> Packaging into Winlator Container Package (.wcp)..."
cd "${INSTALL_PREFIX}"
mkdir -p "$(dirname "${OUTPUT_WCP}")"
tar --exclude='include' --use-compress-program="zstd -T0 --ultra -16" -cf "${OUTPUT_WCP}" .

echo "================================================================="
echo "BUILD COMPLETE!"
echo "Package saved to: ${OUTPUT_WCP}"
echo "================================================================="
