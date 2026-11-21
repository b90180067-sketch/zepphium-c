#!/usr/bin/env python3
"""
Zephium Browser - Diagnostic Log Reader & Bug Analyzer
Parses and analyzes:
  - Zephium runtime logs (/tmp/zephium_debug.log or zephium_debug.log)
  - GStreamer media pipeline logs (/tmp/zephium_gst.log)
  - WebProcess termination signals, resource load failures and media decode stalls
"""

import sys
import os
import re
import time

LOG_FILES = [
    "/tmp/zephium_debug.log",
    "zephium_debug.log",
    os.path.expanduser("~/.cache/zephium_debug.log")
]

GST_LOG_FILES = [
    "/tmp/zephium_gst.log",
    "zephium_gst.log"
]

# ANSI color formatting
RED    = "\033[1;31m"
YELLOW = "\033[1;33m"
GREEN  = "\033[1;32m"
CYAN   = "\033[1;36m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RESET  = "\033[0m"

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# GStreamer emits plenty of harmless WARN lines. Treating them as pipeline
# errors buries the real cause, so they are filtered out explicitly.
BENIGN_GST = (
    "Failed to remove temporary file",          # downloadbuffer temp-file cleanup
    "qtdemux_tags.c",                           # unmapped iTunes metadata tags
    "av1_caps_get_mime_codec",                  # unknown AV1 tier (defaults to M)
    "gst_cuda_nvrtc_load_library_once",         # optional CUDA runtime probe
    "Unknown ebml id",                          # matroska cluster peeking
    "chroma-site only makes sense",
    "invalid chroma-site, using default",
    "validate_chroma_site",
)

# Element instance names carry a numeric suffix ("nvav1dec1"), so no \b at the end.
HW_VIDEO_DECODER_RE = re.compile(
    r"(nvav1dec|nvh264dec|nvh265dec|nvvp8dec|nvvp9dec|nvjpegdec|nvdec"
    r"|vah264dec|vah265dec|vavp8dec|vavp9dec|vaav1dec)\d*"
)

QOS_DROP_MARKER = "Dropping frame due to QoS"


def find_first_existing(candidates):
    for path in candidates:
        if os.path.isfile(path) and os.path.getsize(path) > 0:
            return path
    return None


def strip_ansi(line):
    return ANSI_RE.sub("", line)


def truncate(text, limit=160):
    return text if len(text) <= limit else text[:limit] + "..."


def short_uri(uri, limit=120):
    """Keep media URLs readable: scheme://host/path?first-params"""
    match = re.match(r"^(https?://[^/?]+)([^?]*)(\?[^&]*)?", uri)
    if not match:
        return truncate(uri, limit)
    host, path, first_param = match.group(1), match.group(2), match.group(3) or ""
    path = path if len(path) <= 20 else path[:17] + "..."
    return truncate(host + path + first_param, limit)


def analyze_logs(follow=False):
    log_path = find_first_existing(LOG_FILES)
    gst_path = find_first_existing(GST_LOG_FILES)

    print(f"{BOLD}============================================================{RESET}")
    print(f"  {CYAN}Zephium Diagnostic Log Reader & Bug Analyzer{RESET}")
    print(f"{BOLD}============================================================{RESET}")

    if not log_path and not gst_path:
        print(f"{YELLOW}No log files found yet.{RESET}")
        print("Please launch Zephium first via: ./run_wsl.sh <url>")
        return

    if log_path:
        print(f"[*] Reading Zephium Runtime Log : {BOLD}{log_path}{RESET}")
    if gst_path:
        print(f"[*] Reading GStreamer Pipeline : {BOLD}{gst_path}{RESET}")
    print("------------------------------------------------------------\n")

    failed_resources = []
    crashes = []
    blocked_security = []
    media_fetches = []
    warnings = []
    render_env = []

    if log_path:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = strip_ansi(raw).strip()
                if "RESOURCE_FAILED" in line:
                    # Redirect/subresource cancellations were logged as ERROR by
                    # older builds; they are not real failures.
                    if "code: 302" in line:
                        continue
                    failed_resources.append(line)
                elif "WEB_PROCESS_TERMINATED" in line or "CRASHED" in line:
                    crashes.append(line)
                elif "SECURITY_BLOCKED" in line:
                    blocked_security.append(line)
                elif "MEDIA_CHUNK_LOADED" in line or "videoplayback" in line:
                    media_fetches.append(line)
                elif "RENDER_ENV" in line:
                    render_env.append(line)
                elif "[WARN]" in line:
                    warnings.append(line)

    # ---- GStreamer log analysis -------------------------------------------------
    gst_errors = []
    gst_ignored = 0
    player_created = 0
    player_playing = 0
    qos_drops = 0
    qos_drop_sample = ""
    hw_decoders = {}
    codecs = []

    if gst_path:
        with open(gst_path, "r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = strip_ansi(raw).rstrip("\n")
                if not line.strip():
                    continue

                if "createGSTPlayBin" in line:
                    player_created += 1
                if "State changed" in line and "new: PLAYING" in line:
                    player_playing += 1
                if QOS_DROP_MARKER in line:
                    qos_drops += 1
                    if not qos_drop_sample:
                        qos_drop_sample = line
                for match in HW_VIDEO_DECODER_RE.finditer(line):
                    name = match.group(1)
                    hw_decoders[name] = hw_decoders.get(name, 0) + 1
                codec_match = re.search(r"Setting codec for stream (\d+) to ([A-Za-z0-9\.\-]+)", line)
                if codec_match:
                    entry = f"stream {codec_match.group(1)}: {codec_match.group(2)}"
                    if entry not in codecs:
                        codecs.append(entry)

                is_error = ("ERROR" in line or "CRITICAL" in line
                            or "not-negotiated" in line or "Internal data stream error" in line
                            or "Could not decode" in line)
                if not is_error:
                    continue
                if any(benign in line for benign in BENIGN_GST):
                    gst_ignored += 1
                    continue
                gst_errors.append(line.strip())

    # 1. Print WebProcess Crashes
    if crashes:
        print(f"{RED}{BOLD}[CRITICAL] WebProcess Termination Events ({len(crashes)}):{RESET}")
        for c in crashes[-5:]:
            print(f"  {RED}x {truncate(c, 180)}{RESET}")
        print()

    # 2. Print Failed Resources
    if failed_resources:
        print(f"{RED}{BOLD}[ERROR] Failed Network / Subresource Loads ({len(failed_resources)}):{RESET}")
        for r in failed_resources[-10:]:
            print(f"  {RED}x {truncate(r, 180)}{RESET}")
        print()

    # 3. Print Security Blocked items
    if blocked_security:
        print(f"{YELLOW}{BOLD}[WARN] Navigation Blocked by Engine Policy ({len(blocked_security)}):{RESET}")
        for b in blocked_security[-5:]:
            print(f"  {YELLOW}! {truncate(b, 180)}{RESET}")
        print()

    # 4. Print Media Fetches
    if media_fetches:
        print(f"{GREEN}{BOLD}[INFO] Active Media Streams & Chunks ({len(media_fetches)}):{RESET}")
        for m in media_fetches[-5:]:
            print(f"  {GREEN}+ {short_uri(m)}{RESET}")
        print()

    # 5. Render/compositor configuration
    if render_env:
        print(f"{CYAN}{BOLD}[RENDER] Display & Compositing:{RESET}")
        for entry in render_env[-2:]:
            print(f"  {entry}")
        print()

    # 6. Media pipeline summary (the part that explains black / frozen video)
    if player_created or codecs or qos_drops or hw_decoders:
        print(f"{CYAN}{BOLD}[MEDIA] GStreamer Playback Pipeline:{RESET}")
        print(f"  players created : {player_created}")
        print(f"  transitions to PLAYING : {player_playing}")
        if codecs:
            print(f"  selected codecs : {', '.join(codecs)}")
        if hw_decoders:
            hw_list = ", ".join(f"{name} x{count}" for name, count in sorted(hw_decoders.items()))
            print(f"  {YELLOW}hardware decoders in use : {hw_list}{RESET}")
        else:
            print("  hardware decoders in use : none (software decode)")
        if qos_drops:
            print(f"  {RED}frames dropped by QoS     : {qos_drops}{RESET}")
        print()

    if gst_errors:
        print(f"{RED}{BOLD}[GSTREAMER] Pipeline Errors Detected ({len(gst_errors)}):{RESET}")
        for g in gst_errors[-5:]:
            print(f"  {RED}x {truncate(g, 180)}{RESET}")
        print()
    if gst_ignored:
        print(f"{DIM}  ({gst_ignored} harmless GStreamer warnings ignored: temp-file cleanup, "
              f"container tags, optional CUDA probes){RESET}\n")

    # ---- Automated root cause analysis -----------------------------------------
    print(f"{BOLD}============================================================{RESET}")
    print(f"  {CYAN}Automated Diagnosis & Recommended Fixes:{RESET}")
    print(f"{BOLD}============================================================{RESET}")

    diagnosed = False

    compositing_disabled = any("compositing=disabled" in line for line in render_env)
    dmabuf_disabled = any("dmabuf=disabled" in line for line in render_env)

    if compositing_disabled or dmabuf_disabled:
        diagnosed = True
        parts = []
        if compositing_disabled:
            parts.append("WEBKIT_DISABLE_COMPOSITING_MODE")
        if dmabuf_disabled:
            parts.append("WEBKIT_DISABLE_DMABUF_RENDERER")
        print(f"{RED}> Cause: {' and '.join(parts)} is set.{RESET}")
        print("  WebKitGTK presents decoded video frames through the accelerated compositor.")
        print("  With that disabled, YouTube's <video> stays black and the UI process can")
        print("  segfault a few seconds after the MSE player starts.")
        print("  Fix: unset those variables (Zephium 1.0.5+ no longer sets them), then relaunch:")
        print("    unset WEBKIT_DISABLE_COMPOSITING_MODE WEBKIT_DISABLE_DMABUF_RENDERER")
        print("    export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=llvmpipe\n")

    if hw_decoders and qos_drops:
        diagnosed = True
        names = ", ".join(sorted(hw_decoders))
        print(f"{YELLOW}> Cause: hardware video decoder ({names}) was picked but cannot keep up, "
              f"so every frame is dropped (QoS) and the <video> element stays black/frozen.{RESET}")
        print("  On WSLg the NVIDIA/CUDA decode stack is incomplete (libnvrtc is missing) and the")
        print("  renderer is software (llvmpipe), so NVDEC/VA-API frames can never be presented.")
        print("  Fix: demote the hardware decoders so GStreamer falls back to CPU decoding:")
        print("    export GST_PLUGIN_FEATURE_RANK=vaapi:NONE,nvav1dec:NONE,nvh264dec:NONE,nvvp9dec:NONE,nvdec:NONE")
        print("  Zephium 1.0.6+ sets this automatically on WSL (see apply_wsl_optimizations).\n")

    if hw_decoders and not qos_drops:
        diagnosed = True
        print(f"{YELLOW}> Cause: media is decoded by a hardware decoder ({', '.join(sorted(hw_decoders))}).{RESET}")
        print("  If the picture is black or frozen while audio plays, force software decoding:")
        print("    export GST_PLUGIN_FEATURE_RANK=vaapi:NONE,nvav1dec:NONE,nvh264dec:NONE,nvvp9dec:NONE,nvdec:NONE\n")

    if any("fakevideosink" in e for e in gst_errors + failed_resources):
        diagnosed = True
        print(f"{YELLOW}> Cause: GStreamer 'fakevideosink' element is missing.{RESET}")
        print("  Fix: sudo apt-get install -y gstreamer1.0-plugins-good\n")

    if any("vaapi" in e.lower() for e in gst_errors + crashes):
        diagnosed = True
        print(f"{YELLOW}> Cause: VA-API hardware decoding is crashing on WSLg.{RESET}")
        print("  Fix: sudo apt-get remove -y gstreamer1.0-vaapi\n")

    if any("code: 403" in e for e in failed_resources):
        diagnosed = True
        print(f"{YELLOW}> Cause: YouTube SABR/Token rejection (HTTP 403 Forbidden).{RESET}")
        print("  Fix: the User-Agent or Cookies must match the client that the media URL was")
        print("       issued for, otherwise googlevideo rejects the range requests.\n")

    if (any("videoplayback" in m for m in media_fetches) and player_playing
            and (compositing_disabled or dmabuf_disabled)):
        print(f"{DIM}  Media chunks arrive and the pipeline reaches PLAYING. If the picture is still")
        print(f"  black, the frames are not reaching the compositor: check the WSL launcher")
        print(f"  (WEBKIT_DISABLE_COMPOSITING_MODE / WEBKIT_DISABLE_DMABUF_RENDERER).{RESET}\n")

    if not diagnosed and not crashes and not failed_resources:
        print(f"{GREEN}+ No fatal crash, failed resource load or decode stall recorded in the active log.{RESET}\n")
    elif not diagnosed:
        print(f"{GREEN}+ No media decoder stall detected in the active log.{RESET}\n")

    if follow and log_path:
        print(f"{CYAN}[*] Following log in real-time (Press Ctrl+C to stop)...{RESET}")
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            f.seek(0, os.SEEK_END)
            try:
                while True:
                    line = f.readline()
                    if line:
                        print(line, end="")
                    else:
                        time.sleep(0.5)
            except KeyboardInterrupt:
                print(f"\n{CYAN}Stopped log follower.{RESET}")


if __name__ == "__main__":
    follow_mode = ("--follow" in sys.argv or "-f" in sys.argv)
    analyze_logs(follow=follow_mode)
