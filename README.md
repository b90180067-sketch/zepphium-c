# Zephium Browser (C17 Standalone Engine & WebKitGTK Shell)

Bản port hoàn chỉnh sang ngôn ngữ **C (chuẩn ISO C17)** của **Zephium Browser**, loại bỏ các thành phần "work" / agentic thừa, giữ lại toàn bộ tính năng duyệt web cốt lõi với độ chính xác bit-for-bit so với bản gốc Rust.

---

## 1. Cấu trúc thư mục

```
zephium-c/
├── blocker/            # WebKit Content Filter (Adblock/Tracker blocker) + PCRE2 regex
├── browser_core/       # Core browser state (Tabs, Omnibox, Zoom, SQLite store)
├── geometry/           # Point, Size, Rect tính toán giao diện
├── ids/                # 128-bit ULID Crockford-32 định danh Tabs/Windows
├── navigation/         # URL Classification, Search fallback, SSRF & Local host protection
├── store/              # SQLite3 WAL store (Bookmarks, History, Downloads, Preferences)
├── zoom/               # Điều khiển zoom cấp số nhân rời rạc
├── main.c              # Entry point: Linux WebKitGTK 4.1 Shell + Cross-platform CLI REPL
├── CMakeLists.txt      # Cấu hình build đa nền tảng (CMake 3.16+)
├── Makefile            # Makefile cho MinGW / GCC / Clang
├── setup_wsl.sh        # Script tự động cài dependencies trên WSL
├── run_wsl.sh          # Script tối ưu chạy trên WSLg
├── zephium_gcc.exe     # Binary prebuilt (GCC 14+)
└── zephium_clang.exe   # Binary prebuilt (Clang 19+)
```

---

## 2. Kiểm thử trên WSL (Windows Subsystem for Linux)

Đường dẫn thư mục này từ trong WSL:
```bash
cd /mnt/c/Users/offli/zephium-c
```

### Bước 1: Cài đặt dependencies (chạy 1 lần duy nhất)
```bash
chmod +x setup_wsl.sh run_wsl.sh
./setup_wsl.sh
```

### Bước 2: Khởi chạy trình duyệt GUI WebKitGTK trên WSLg
```bash
./run_wsl.sh https://duckduckgo.com
```

### Các tùy chọn nâng cao trên WSL:
- **Chế độ Software Rendering (khi GPU ảo hóa WSLg không ổn định)**:
  ```bash
  ./build/zephium --disable-gpu https://github.com
  ```
- **Chế độ ép chạy X11 backend**:
  ```bash
  ./build/zephium --x11 https://gnu.org
  ```
- **Chế độ Headless / CLI REPL (khi kết nối qua SSH hoặc terminal thuần)**:
  ```bash
  ./build/zephium --headless
  ```

### Sự cố đã biết: video YouTube không phát được trên WSLg

Triệu chứng: trang `watch?v=...` mở được, log có `MEDIA_CHUNK_LOADED` nhưng khung hình
đen và tiến trình có thể thoát với `Segmentation fault` vài giây sau khi player MSE khởi tạo.

Nguyên nhân: WebKitGTK 2.4x trở lên đưa frame video **đã giải mã** ra accelerated
compositor. Khi bật `WEBKIT_DISABLE_COMPOSITING_MODE` hoặc
`WEBKIT_DISABLE_DMABUF_RENDERER` (các "safeguard" trước đây của `run_wsl.sh`), player
không còn đường hiển thị frame nên `<video>` đứng ở `readyState = 1` và UI process
crash. Ngoài ra VA-API/NVDEC không tạo được CUDA/GL context trên WSLg (Mesa chạy
llvmpipe), nên decoder phần cứng còn drop toàn bộ frame (`Dropping frame due to QoS`).

Cách xử lý (đã áp dụng trong source):

- **Không** tắt accelerated compositing và DMA-BUF renderer nữa; chỉ ép GL phần mềm
  (`LIBGL_ALWAYS_SOFTWARE=1`, `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`).
- Hạ rank (demote) các decoder phần cứng VA-API/NVDEC để GStreamer dùng decoder CPU.
- Log được ghi mới cho mỗi lần chạy (`/tmp/zephium_debug.log`) để `read_log.py`
  chỉ phân tích phiên hiện tại.

Kiểm tra lại:

```bash
./run_wsl.sh "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
python3 read_log.py    # xem mục [RENDER] và [MEDIA]
```

## 3. Hiệu năng trên WSL (software rendering)

Số đo trên máy test: WSL2, **không có `/dev/dri`** → Mesa chỉ dùng được llvmpipe
(rasterize bằng CPU), dù `/usr/lib/wsl/lib/libd3d12.so` có mặt.

| Kịch bản | CPU WebProcess | FPS |
|---|---|---|
| Trang động đơn giản (compositing bật) | ~136% | ~55-60 |
| Trang động đơn giản (compositing tắt) | ~12% | ~55-60 |
| YouTube trang chủ (không phát video) | ~145% | – |
| YouTube watch 480p VP9 (decode phần mềm) | ~250-260% | ~10-25 |
| Video local 640x360 | ~124% | ~28 |

Kết luận: chi phí nằm ở **composite/rasterize bằng CPU**, không phải ở code app
(UI process chỉ ~13%, phần còn lại là WebProcess của WebKit). Video bắt buộc phải có
accelerated compositing nên không thể tắt để lấy lại 12% CPU như trước.

Muốn FPS cao hơn thì phải cấp GPU cho WSL:

- `ls /dev/dri` — nếu không có `renderD128` thì mọi thứ đều chạy bằng CPU.
- Khi đã có GPU, có thể bỏ `LIBGL_ALWAYS_SOFTWARE=1` và `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe`
  trong `run_wsl.sh` để Mesa dùng driver d3d12.
- Cửa sổ càng lớn càng tốn CPU (1280x800 so với 900x600 chênh ~10% FPS).
- Ép YouTube sang H.264 không có tác dụng: WebKit vẫn báo hỗ trợ VP9 và YouTube vẫn chọn VP9.

Tối ưu đã áp dụng trong code:

- SQLite: `journal_mode=WAL` + `synchronous=NORMAL` (commit lịch sử ~26ms → ~8ms trên DrvFs).
- Ghi lịch sử một lần cho mỗi lần điều hướng (trước đây mỗi lần `URI` đổi đều ghi), có dedupe.
- Không cập nhật ô địa chỉ/tiêu đề nếu giá trị không đổi (tránh repaint toolbar và reset con trỏ).
- Log media rút gọn URL (`/tmp/zephium_debug.log` 35KB → ~10KB) và `GST_DEBUG` mặc định chỉ còn ERROR.

---

## 4. Kiểm thử trên Windows (MinGW-w64)

### Chạy binary có sẵn:
```powershell
.\zephium_gcc.exe --headless
# hoặc
.\zephium_clang.exe --headless
```

### Chạy automated regression test:
```powershell
.\zephium_gcc.exe --batch
```

### Chạy toàn bộ 7 verification gates (diff đối chiếu Rust, UBSan, Bench, Fuzz):
```powershell
mingw32-make gate
```
