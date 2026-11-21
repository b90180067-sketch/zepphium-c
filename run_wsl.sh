#!/usr/bin/env bash
# ==============================================================================
# Zephium Browser - WSL / WSLg Optimized Launcher & Builder
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

echo "============================================================"
echo "  Zephium Standalone Browser - WSL Launcher"
echo "============================================================"

# 1. WSLg Display Diagnostics
if [ -z "${DISPLAY}" ] && [ -z "${WAYLAND_DISPLAY}" ]; then
    if [ -S "/tmp/.X11-unix/X0" ] || [ -d "/mnt/wslg" ]; then
        echo "[WSL Launcher] Exporting default WSLg display: DISPLAY=:0"
        export DISPLAY=:0
    else
        echo "[WSL Launcher] Warning: No DISPLAY or WAYLAND_DISPLAY found."
        echo "If running on WSL without WSLg, start an X-Server (VcXsrv/X410) or pass --headless."
    fi
fi

# 2. Audio Pipeline Mapping for WSLg PulseAudio
if [ -S "/mnt/wslg/runtime-dir/pulse/native" ]; then
    export PULSE_SERVER="unix:/mnt/wslg/runtime-dir/pulse/native"
fi

# 3. Force X11 backend: X11 avoids Wayland DMA-BUF & subsurface glitches on WSLg
export GDK_BACKEND="x11"

# 4. WebKitGTK WSL Sandboxing, DMA-BUF & Compositing
# Disable bubblewrap sandbox in WSL2 kernel due to unprivileged mount restrictions
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1

# Do NOT export WEBKIT_DISABLE_COMPOSITING_MODE / WEBKIT_DISABLE_DMABUF_RENDERER:
# WebKitGTK >= 2.4x presents decoded video frames through the accelerated
# compositor. Disabling them makes the UI process segfault a few seconds after a
# YouTube MSE player starts, and the <video> element never shows a picture.
unset WEBKIT_DISABLE_COMPOSITING_MODE
unset WEBKIT_DISABLE_DMABUF_RENDERER
unset WEBKIT_DMABUF_RENDERER_FORCE_SHM

# Force software GL (llvmpipe) while keeping compositing enabled: avoids the WSLg
# Mesa D3D12/DRI3 bugs (missing glyphs, DRI3 errors) without breaking video.
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe

# Redirect WebKit cache/data to native Linux tmpfs (/tmp) to avoid NTFS DrvFs fallocate crashes
export XDG_CACHE_HOME="/tmp/zephium-cache"
export XDG_DATA_HOME="/tmp/zephium-data"
mkdir -p /tmp/zephium-cache /tmp/zephium-data

# 5. Force software video decoding on WSLg.
# VA-API decoders crash here, and NVDEC cannot create a CUDA/GL context (Mesa runs
# on llvmpipe), so nvav1dec/nvvp9dec drop every frame and YouTube stays black.
export GST_PLUGIN_FEATURE_RANK="vaapi:NONE,vaapidecode:NONE,vaapisink:NONE,vaapipostproc:NONE,vah264dec:NONE,vah265dec:NONE,vavp8dec:NONE,vavp9dec:NONE,vaav1dec:NONE,nvdec:NONE,nvenc:NONE,nvav1dec:NONE,nvh264dec:NONE,nvh265dec:NONE,nvvp8dec:NONE,nvvp9dec:NONE,nvjpegdec:NONE,nvmpeg2videodec:NONE,nvmpeg4videodec:NONE"
export LIBVA_DRIVER_NAME=none
export WEBKIT_GST_HOLE_PUNCH=0

# 6. Check and install GStreamer codecs & fakevideosink for YouTube
NEED_CODECS=0
if command -v gst-inspect-1.0 &> /dev/null; then
    if ! gst-inspect-1.0 fakevideosink &> /dev/null; then
        NEED_CODECS=1
    fi
else
    NEED_CODECS=1
fi

if ! dpkg -s gstreamer1.0-libav fonts-liberation &> /dev/null 2>&1; then
    NEED_CODECS=1
fi

if [ "${NEED_CODECS}" -eq 1 ]; then
    echo "[WSL Launcher] Installing required GStreamer codecs (fakevideosink, libav) for YouTube..."
    sudo apt-get update && sudo apt-get install -y \
        gstreamer1.0-tools \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-libav \
        gstreamer1.0-gl \
        fonts-liberation \
        fonts-dejavu-core \
        fontconfig || true
    fc-cache -fv || true
fi

# 7. GStreamer logging for troubleshooting.
# Default level 1 (errors only): level 2 also logs every decoder warning (e.g. a QoS
# drop per frame) straight into the debug file, which is pure overhead during playback.
export GST_DEBUG="1,webkit*:4,media*:4"
export GST_DEBUG_FILE="/tmp/zephium_gst.log"

# 8. Build or update binary with CMake
echo "[WSL Launcher] Compiling/updating Zephium binary with CMake..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "[WSL Launcher] Launching Zephium..."
echo "[WSL Launcher] To inspect runtime logs, run: python3 read_log.py"
exec ./build/zephium "$@"
