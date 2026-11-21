#!/usr/bin/env bash
# ==============================================================================
# Zephium Browser - WSL / Ubuntu / Debian One-Click Dependency Setup
# ==============================================================================
set -e

echo "=== [Zephium WSL Setup] Installing required system libraries ==="

if command -v apt-get &> /dev/null; then
    sudo apt-get update
    # Install WebKitGTK (prefers 4.1 for Ubuntu 22.04/24.04, falls back to 4.0 if needed)
    if apt-cache show libwebkit2gtk-4.1-dev &> /dev/null; then
        WEBKIT_PKG="libwebkit2gtk-4.1-dev"
    else
        WEBKIT_PKG="libwebkit2gtk-4.0-dev"
    fi

    echo "Installing: ${WEBKIT_PKG}, GTK3, SQLite3, build-essential, CMake, GStreamer, and Fonts..."
    sudo apt-get install -y \
        build-essential \
        cmake \
        pkg-config \
        "${WEBKIT_PKG}" \
        libgtk-3-dev \
        libsqlite3-dev \
        libpcre2-dev \
        libcurl4-openssl-dev \
        libsodium-dev \
        fonts-liberation \
        fonts-dejavu-core \
        fonts-noto-color-emoji \
        fontconfig \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-libav \
        gstreamer1.0-gl

    # Remove buggy VA-API plugin on WSL if present (causes segfault during video playback on WSLg)
    sudo apt-get remove -y gstreamer1.0-vaapi 2>/dev/null || true

    # Refresh font cache
    echo "Updating font cache..."
    fc-cache -fv
elif command -v dnf &> /dev/null; then
    sudo dnf install -y \
        gcc gcc-c++ make cmake pkgconf \
        webkit2gtk4.1-devel gtk3-devel sqlite-devel \
        pcre2-devel libcurl-devel libsodium-devel \
        liberation-fonts dejavu-sans-fonts fontconfig \
        gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free
elif command -v pacman &> /dev/null; then
    sudo pacman -Sy --noconfirm \
        base-devel cmake pkgconf \
        webkit2gtk-4.1 gtk3 sqlite pcre2 curl libsodium \
        ttf-liberation ttf-dejavu fontconfig \
        gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav
else
    echo "Notice: Package manager not recognized. Please install webkit2gtk-4.1, gtk3, sqlite3, and cmake manually."
fi

echo "=== [Zephium WSL Setup] Dependencies installed successfully! ==="
echo "You can now run: ./run_wsl.sh"
