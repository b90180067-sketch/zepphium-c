# Zephium for Windows (Win32 + WebView2)

WebKitGTK only exists on Linux, so the native Windows build embeds **WebView2**
(the Edge/Chromium engine that ships with Windows) instead, and reuses the same
portable C17 core (`zoom`, `navigation`, `store`, `browser_core`).

```
windows/
├── zephium_win.cpp     # Win32 window + toolbar + WebView2 host (C++17)
├── Makefile            # mingw32-make build
├── fetch_sdk.ps1       # downloads the WebView2 SDK into windows/sdk
├── sdk/                # WebView2 SDK (not committed, fetched by the script)
├── zephium_win.exe     # build output
└── WebView2Loader.dll  # build output (copied from the SDK)
```

## Yêu cầu

- Windows 10/11 x64, đã có **Microsoft Edge WebView2 Runtime**
  (Windows 11 có sẵn; Windows 10 cài từ https://developer.microsoft.com/microsoft-edge/webview2/).
  Kiểm tra: `reg query "HKLM\\SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"`
- MinGW-w64 tại `C:/mingw64` với `gcc`, `g++`, `mingw32-make` và SQLite3 tĩnh ở
  `C:/mingw64/opt/lib/libsqlite3.a` (đúng layout mà `Makefile` gốc của project đang dùng).

## Build

```powershell
cd C:\Users\offli\zephium-c\windows
powershell -ExecutionPolicy Bypass -File fetch_sdk.ps1   # chỉ lần đầu
mingw32-make
```

Kết quả: `zephium_win.exe` + `WebView2Loader.dll` trong `windows/`.

## Chạy

```powershell
.\zephium_win.exe                       # mở duckduckgo
.\zephium_win.exe https://grokipedia.com
```

Phím tắt: `Enter` trong ô địa chỉ để điều hướng, `Esc` trả focus về trang.

## Tính năng

- **Multi-tab**: thanh tab tự vẽ (GDI) với nút `×` từng tab và nút `+`; tab đang mở
  có nền sáng, tab đang ngủ đông có tiền tố `⏾`. Mỗi tab sở hữu một
  `ICoreWebView2Controller` riêng trên cùng HWND.
- **Tab ngủ đông (hibernation)**: tab không active quá `--tab-sleep` phút sẽ bị huỷ
  webview (giữ URL/tiêu đề/zoom), khi bấm lại sẽ được tạo lại và nạp lại URL cũ.
  Không bao giờ ngủ tab đang active và tab đang phát audio
  (`ICoreWebView2_8::get_IsDocumentPlayingAudio`).
- Toolbar: back / forward / reload, ô địa chỉ, zoom -/+ (zoom theo từng tab), tiêu đề
  cửa sổ theo trang.
- Đồng bộ URL + trạng thái back/forward mỗi 400 ms (không cần COM event handler).
- Lịch sử duyệt web ghi vào SQLite (`zephium_profile.db`) qua core C17 dùng chung,
  URL được phân loại bởi module `navigation` (search fallback, SSRF guard) — kể cả
  khi điều hướng bằng link trong trang.
- Autoplay được bật (`--autoplay-policy=no-user-gesture-required`).
- Log chẩn đoán: `windows/zephium_win.log`.

### Tuỳ chọn dòng lệnh

```
zephium_win.exe [URL] [--tab-sleep <phút>] [--tab-sleep-seconds <giây>]
```

- `--tab-sleep <phút>`: thời gian nhàn rỗi trước khi tab ngủ đông (mặc định **10** phút,
  `0` = tắt).
- `--tab-sleep-seconds <giây>`: như trên nhưng theo giây (dùng để test).

### Phím tắt

| Phím | Hành động |
|---|---|
| `Ctrl+T` | Tab mới |
| `Ctrl+W` | Đóng tab hiện tại |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Tab kế / tab trước |
| `Ctrl+L` | Focus ô địa chỉ |
| `F5` | Tải lại |
| `Ctrl++` / `Ctrl+-` | Zoom tab hiện tại |

### Đo hiệu quả ngủ đông (4 tab nhẹ: example.com/org/net + Wikipedia)

| Cấu hình | Process | Private working set |
|---|---|---|
| Tắt ngủ đông | 9 | **196.3 MB** |
| Ngủ đông sau 8s | 6 | **119.3 MB** |

→ tiết kiệm ~39% RAM cho 4 tab nhẹ; với tab nặng (grokipedia, video) phần renderer
được giải phóng ngay, còn bộ nhớ GPU của WebGL có thể được Chromium thu hồi chậm hơn.

Lưu ý: tab ngủ đông khi đánh thức sẽ **nạp lại trang** (mất vị trí cuộn và trạng thái
form); đây là đánh đổi giống "sleeping tabs" của Edge/Chrome.

## Giới hạn (MVP)

- Một cửa sổ (nhiều tab trong cửa sổ đó); chưa có session restore sau khi tắt app.
- Chưa tích hợp adblock và download manager.
- `zephium_win.cpp` dùng C++17 vì WebView2 là COM/C++; phần core vẫn là C17 thuần.

## Bộ nhớ (RAM)

Số đo trên máy test (Windows 11 26300, WebView2 157.0.4341.60). Cột "MB" là
**private working set** — đúng đại lượng Task Manager hiển thị ở cột Memory, cộng cho
toàn bộ process của app:

| Trang | Cửa sổ | Tổng | Ghi chú |
|---|---|---|---|
| example.com (tĩnh) | 1280x800 | **~94 MB** | ngang với bản Rust (~91 MB) |
| YouTube watch (video đang phát) | 1280x800 | **~620 MB** | renderer ~295 + GPU ~260 |
| YouTube watch (video đang phát) | 720x480 | **~460 MB** | giảm theo diện tích video |

Kết luận: phần "nặng" là **giải mã video + frame buffer của GPU process**, không phải
shell. Cùng một engine thì chi phí mỗi trang là như nhau; khác biệt lớn nhất đến từ
kích thước video đang hiển thị và trang đó có đang phát hay không.

Bản Rust (Tauri/wry) trong Task Manager hiện 2 nhóm: nhóm "WebView2: Zephium" ~91 MB là
**cửa sổ chrome UI** (5 process, không có renderer của trang), còn nhóm có tab YouTube
(~382 MB, renderer + GPU) mới là trang thật — nên so sánh phải cùng loại nhóm.

Cờ tiết kiệm RAM đang dùng (học từ bản Rust):

- `--disable-features=msWebOOUI,msPdfOOUI,SpareRendererForSitePerProcess,
  msEdgeSidebarV2,msEdgeAutofill,msEdgeDiscover,msEdgeShoppingAssistant,
  msEdgeWebCapture,msEdgeCollections`
  → không có spare renderer dự phòng (~40 MB), không nạp service Office/PDF/Sidebar/Discover.
- `--disable-background-networking --disable-component-update`
  → không tải component/telemetry chạy nền.
- `--autoplay-policy=no-user-gesture-required`: giữ để YouTube tự phát; nếu ưu tiên RAM
  thì bỏ cờ này (đo được ~611 MB → ~546 MB cho một trang video).

Muốn giảm RAM khi xem video: thu nhỏ cửa sổ (623 → 463 MB) và tránh mở nhiều tab video
cùng lúc.

## Ghi chú kỹ thuật

- `ICoreWebView2EnvironmentOptions` được tự cài đặt vì helper của SDK cần WRL
  (`<wrl/implements.h>`) mà MinGW không có.
- `TargetCompatibleBrowserVersion` **phải** là chuỗi version hợp lệ, nếu để rỗng
  thì `CreateCoreWebView2EnvironmentWithOptions` trả `E_INVALIDARG` (0x80070057).
  Giá trị đang dùng khớp với `CORE_WEBVIEW_TARGET_PRODUCT_VERSION` của SDK 1.0.4370.26.
