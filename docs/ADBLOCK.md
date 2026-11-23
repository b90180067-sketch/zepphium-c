# Chặn quảng cáo gốc (native ad blocking)

Tài liệu này mô tả kiến trúc, cách build, cấu hình và cách kiểm chứng tính năng
chặn quảng cáo **ở tầng mạng** của Zephium: không extension, không content
script, không phụ thuộc API extension của engine hệ thống.

- **Windows (WebView2)** → engine [adblock-rust](https://github.com/brave/adblock-rust)
  (engine của Brave) gọi qua C ABI trong sự kiện `WebResourceRequested`.
- **Linux/WSL (WebKitGTK)** → `WebKitUserContentFilterStore` + bộ lọc JSON đã
  chuyển đổi sẵn cho WebKit (EasyList/EasyPrivacy/uBO), lấy từ
  [bnema/ublock-webkit-filters](https://github.com/bnema/ublock-webkit-filters).

---

## 1. Vì sao bỏ extension, chuyển xuống tầng mạng

Bản thử nghiệm trước đây nạp uBOL (uBlock Origin Lite) như một unpacked
extension vào WebView2 (thư mục `windows/extensions/`, commit `604b275`).
Cách đó thất bại vì:

- build extension cần Node/esbuild + submodule + cache riêng; chạy trên WSL lại
  càng dễ lệch môi trường (path, cache, phiên bản Chromium của WebView2 runtime);
- extension sống trong sandbox của engine, chịu giới hạn API extension (MV2/MV3)
  và **phụ thuộc phiên bản WebView2 runtime** — runtime cũ là mất tính năng;
- extension có thể bị trang web phát hiện/né (nó là JS/CSS trong page context),
  và mọi rule đều phải đi qua engine JS của extension.

Tích hợp ở **network layer** giải quyết cả ba: request bị quyết định **trước khi
rời khỏi engine mạng**, cách ly hoàn toàn với JavaScript của trang, không có
extension process, không tốn thêm bộ nhớ cho một profile extension.

| | Windows (WebView2) | Linux/WSL (WebKitGTK) |
|---|---|---|
| Điểm chặn | `WebResourceRequested` (mỗi tab) | WebKit content blocker (biên dịch sẵn trong UI process) |
| Dữ liệu lọc | EasyList/EasyPrivacy/uBO dạng văn bản ABP | JSON content blocker (cùng bộ lọc, đã chuyển đổi) |
| Engine | adblock-rust `0.13.6` (staticlib Rust, C ABI) | WebKit Content Extensions (`WebKitUserContentFilterStore`) |
| Cache khởi động | `engine.bin` (flatbuffer + header riêng) | filter đã biên dịch trong store của WebKit, đặt tên theo version |
| Cập nhật | tải lại danh sách mỗi 24h (worker thread) | hỏi `manifest.json` mỗi 24h, tải bản mới khi có |
| Fallback | không chặn gì (chỉ log), browser vẫn chạy | 4 rule built-in của module C17 `blocker` |

---

## 2. Phần A — adblock-rust + WebView2 (Windows)

### 2.1 Crate `blocker/rust` (Rust → C ABI)

```
blocker/
├── adblock_ffi.h      # hợp đồng C ABI (đóng băng), dùng bởi cả shell và selftest
└── rust/
    ├── Cargo.toml     # crate-type = ["staticlib"], adblock = "0.13.6"
    ├── Cargo.lock     # ghim phiên bản để build lặp lại được
    └── src/lib.rs     # engine + cache + kiểm tra request + unit test
```

Build:

```powershell
# một lần, xem windows/fetch_rust.ps1
rustup toolchain install stable-x86_64-pc-windows-gnu
cargo build --release --manifest-path blocker/rust/Cargo.toml
# -> blocker/rust/target/release/libzephium_adblock.a (~9 MB)
```

Vì sao là **staticlib** chứ không phải DLL:

- `zephium_win.exe` vẫn chỉ cần `WebView2Loader.dll` (có sẵn từ Microsoft)
  bên cạnh là chạy; không có thêm DLL của dự án để quên copy hay lệch phiên bản;
- toolchain MinGW của shell là `x86_64-pc-windows-gnu` (msvcrt), đúng target mặc
  định của Rust trên Windows, nên ABI khớp trực tiếp;
- build lại engine = build lại exe, không có "DLL cũ nằm cạnh exe mới".

Link (đã kiểm chứng bằng chương trình C thật, không chỉ bằng compile):

```text
g++ ... libzephium_adblock.a -lsynchronization -lws2_32 -luserenv -lntdll \
    -ladvapi32 -lbcrypt -lole32 -lshell32 -luser32 -lgdi32
```

`-lsynchronization` là bắt buộc: `std` của Rust dùng `WaitOnAddress` /
`WakeByAddressSingle` qua futex, và MinGW export các hàm đó từ
`libsynchronization` chứ không phải `libkernel32`.

### 2.2 An toàn luồng: single-thread + thread-affinity

`adblock-rust` được build với feature mặc định `single-thread` — chế độ nhanh
nhất nhưng `Engine` **không** `Send`/`Sync`. Thay vì bọc Mutex (vẫn không
hợp lệ với `!Send`) hay `unsafe impl Send`, FFI ghi lại `ThreadId` lúc tạo
handle và **kiểm tra ở mọi entry point**:

- gọi từ sai thread → trả `ZEPHIUM_ADBLOCK_ERR_THREAD`, không chạm vào engine;
- `free` từ sai thread → leak đúng một handle thay vì UB;
- `check` từ sai thread → **fail open** (ALLOW), không chặn oan.

Vì shell chỉ dùng engine trên UI thread của WebView2 (nơi phát sự kiện
`WebResourceRequested`), hot path không có lock nào.

### 2.3 Chống panic qua FFI (0xc0000005)

Mọi hàm `extern "C"` đều nằm trong `catch_unwind(AssertUnwindSafe(...))`.
`Cargo.toml` khai báo `panic = "unwind"` cho `[profile.release]` — nếu ai đó
build với `panic = "abort"`, panic sẽ xuyên qua biên C và giết tiến trình (đúng
kiểu exit code `0xc0000005` mà panic unwind qua FFI tạo ra).

Khi bắt được panic:

1. handle bị **poison** (`panics` +1, ghi `last_error`);
2. mọi `check` sau đó trả `ALLOW` và vẫn tăng bộ đếm `checks` (minh bạch);
3. browser tiếp tục chạy, chỉ mất tính năng chặn.

Có hook test riêng (`zephium_adblock_selftest_panic`,
`zephium_adblock_engine_selftest_panic`) để unit test và `adblock_selftest.exe`
chứng minh hành vi này, chứ không chỉ tin vào code đọc.

### 2.4 Cache nhị phân (warm start)

Định dạng `engine.bin` do crate này sở hữu:

| Offset | Nội dung |
|---|---|
| 0..8 | magic `ZEPHADB1` |
| 8..12 | ABI version (LE u32) |
| 12..16 | reserved |
| 16..24 | rule count (LE u64) |
| 24..32 | list bytes (LE u64) |
| 32.. | flatbuffer engine do `adblock-rust::Engine::serialize()` sinh ra |

Header riêng cho phép **từ chối cache lạ/khác version trước khi** đưa dữ liệu vào
bộ deserialize của thư viện, và giữ được `rule_count` sau khi load nóng (thư
viện không expose số rule). Ghi file kiểu atomic (`.tmp` + rename) nên không bao
giờ có cache dở dang.

### 2.5 Luồng xử lý một request

1. Mỗi tab, sau khi controller sẵn sàng:
   `AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL)`
   rồi `add_WebResourceRequested` với một handler riêng của tab.
2. Handler lấy `Uri`, `Method`, resource context và header `Referer`.
3. **Phân loại**: context → `ZEPHIUM_ADBLOCK_RES_*`. WebView2 báo cả navigation
   trong iframe là `DOCUMENT`, nên shell giữ một **tập URI iframe** của tab (từ
   `ICoreWebView2_4::add_FrameCreated` +
   `ICoreWebView2Frame::add_NavigationStarting`, FIFO 64 entry): request
   `DOCUMENT` mà URL nằm trong tập đó là `SUBDOCUMENT`, còn lại là document
   thật. Nhờ vậy rule `$document` không chặn oan iframe và ngược lại.
4. **source_url** (cho `$third-party`, `$domain=`): document request dùng chính
   URL của nó; request khác dùng header `Referer`, fallback URL document hiện tại
   của tab.
5. Gọi `zephium_adblock_check()` và hành động:
   - `BLOCK` → trả response **204 rỗng** (không có byte nào ra mạng);
   - `REDIRECT` (`$redirect`) → trả **200** với body đã decode từ data-URI,
     `Content-Type` theo `redirect_mime`, kèm `Access-Control-Allow-Origin: *`;
   - `REWRITE` (`$removeparam`) → đếm + log rồi cho qua (xem §2.8);
   - `ALLOW` → không đụng vào args, request đi tiếp bình thường.

### 2.6 Bộ lọc: nguồn, cache, cập nhật mỗi 24h

Danh sách mặc định (raw ABP/uBO text, tải bằng WinHTTP trên worker thread):

| id | URL | max |
|---|---|---|
| easylist | `https://easylist.to/easylist/easylist.txt` | 8 MB |
| easyprivacy | `https://easylist.to/easylist/easyprivacy.txt` | 8 MB |
| ublock-filters | `https://ublockorigin.github.io/uAssets/filters/filters.txt` | 4 MB |
| ublock-badware | `https://ublockorigin.github.io/uAssets/filters/badware.txt` | 2 MB |
| ublock-quick-fixes | `https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt` | 1 MB |
| ublock-unbreak | `https://ublockorigin.github.io/uAssets/filters/unbreak.txt` | 2 MB |
| ublock-privacy | `https://ublockorigin.github.io/uAssets/filters/privacy.txt` | 2 MB |
| peter-lowe | `https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&mimetype=plaintext` | 1 MB |

Thư mục dữ liệu: `%LOCALAPPDATA%\Zephium\adblock\` (không dùng thư mục exe vì
bản cài trong `Program Files` là read-only), hoặc `$ZEPHIUM_ADBLOCK_DIR`.

Nếu thư mục đó không tạo được (profile bị khoá quyền, hoặc tiến trình chạy trong môi
trường chỉ cho ghi trong workspace), shell tự chuyển sang `<thư mục exe>\zephium_adblock`
và log cả hai đường dẫn kèm mã lỗi Win32; chỉ khi cả hai đều không ghi được thì ad blocking
chạy không có danh sách (cache `engine.bin` có sẵn vẫn load được).

```
%LOCALAPPDATA%\Zephium\adblock\
├── lists\easylist.txt ...      # bản tải về, ghi atomic
└── engine.bin                  # cache engine (~6.8 MB)
```

Luồng khởi động (engine **chỉ** được tạo/load/check/free trên UI thread):

1. `load_cache(engine.bin)` → nếu OK thì chặn được ngay sau ~8 ms;
2. nếu không, load các `.txt` đã có trên đĩa → build engine → `save_cache`;
3. nếu chưa có danh sách nào: bắt đầu tải ở worker thread, **không chặn** việc
   duyệt web; tải xong thì `PostMessage(WM_APP)` về UI thread để build;
4. mỗi 24h (hoặc khi file danh sách đổi hash) làm lại bước 3; lỗi giữ engine cũ.

### 2.7 Hiệu năng đo được (máy test: Windows, MinGW gcc 16.2, 8 danh sách thật)

Số liệu lấy từ harness C link trực tiếp staticlib và chạy trên chính máy build:

| Chỉ số | Giá trị |
|---|---|
| Danh sách | 8 file, **4.937.321 byte** → **153.705 rule** |
| Build engine (lạnh) | **~119 ms** |
| Cache `engine.bin` | **6.8 MB** |
| Ghi cache | ~11 ms |
| Load cache (warm start) | **~8 ms** |
| Thời gian engine / request (100.000 check) | **avg 3.07 µs, p50 2.5 µs, p99 11.3 µs** |

`p99 11 µs` bao gồm cả lần đầu gặp host mới (adblock-rust biên dịch regex lazy);
sau đó phần lớn request ở mức 1–4 µs. Tổng overhead của `WebResourceRequested`
(COM call + UTF-16→UTF-8) nhỏ hơn nhiều so với một vòng tải network.

### 2.8 Giới hạn đã biết (Part A)

- **WebSocket**: WebView2 **không** phát `WebResourceRequested` cho WebSocket
  handshake, nên rule `$websocket` không có tác dụng trên Windows. Engine vẫn hỗ
  trợ kiểm tra loại này (`ZEPHIUM_ADBLOCK_RES_WEBSOCKET`) cho caller khác. Hướng
  bổ sung nếu cần: CDP `Network.setBlockedURLs`/`Fetch` qua
  `ICoreWebView2::CallDevToolsProtocolMethod` — chưa triển khai.
- **`$removeparam`**: không rewrite được URL request từ `WebResourceRequested`;
  shell đếm/log rồi cho request đi.
- **`$csp`**: không thêm header cho request đi tiếp được; bỏ qua.
- **`$popup`**: popup do handler `NewWindowRequested` xử lý theo allow-list của
  module `navigation`; rule popup của bộ lọc không được áp dụng riêng.
- **Cosmetic filtering** (ẩn phần tử): ABI có sẵn `zephium_adblock_cosmetic_css()`
  nhưng shell Windows chưa inject CSS — phần mở rộng có chủ ý (network-level là mục
  tiêu chính, GTK đã có element hiding qua content filter JSON).
- **Third-party chính xác tuyệt đối**: WebView2 không cho biết *frame* phát ra
  request; ta dùng `Referer` + document URL. Với request không có Referer
  (Referrer-Policy chặt), third-party tính theo document URL của tab.

---

## 3. Phần B — WebKitUserContentFilterStore (Linux/WSL)

### 3.1 Nguồn và định dạng

WebKit GTK chỉ nhận **content blocker JSON** (`trigger`/`action`), không nhận
EasyList text. Dự án [bnema/ublock-webkit-filters](https://github.com/bnema/ublock-webkit-filters)
chuyển đổi sẵn EasyList/EasyPrivacy/uAssets và publish release hằng ngày:

| File | Kích thước | Rule |
|---|---|---|
| `combined-part1.json` | ~24 MB | ~150k |
| `combined-part2.json` | ~16 MB | phần còn lại |
| `easylist.json` / `easyprivacy.json` | ~20/17 MB | 124k / 104k |
| `manifest.json` | ~2 KB | version + rule count |

WebKit giới hạn **150.000 rule cho một filter**, nên dùng các file
`combined-part*` và mỗi part là một filter riêng trong manager.

### 3.2 Luồng

1. Khởi động: thử `webkit_user_content_filter_store_load()` cho từng identifier
   `zephium-adblock-<part>-<version>` (**warm start**, ~0.5 ms/filter) → gắn ngay
   vào user-content-manager của mọi tab.
2. Nếu store miss: `webkit_user_content_filter_store_save_from_file()` trên JSON
   đã cache (WebKit tự biên dịch ở worker thread của nó, UI không bị block) → gắn.
3. Nếu chưa có JSON: dùng 4 rule built-in của module C17 (luôn có, offline được),
   đồng thời tải `manifest.json` + các part ở background (bắt đầu 3 s sau khi cửa
   sổ hiện, để không tranh băng thông với trang đầu tiên).
4. Nếu version trong manifest trùng stamp và cache đủ → **"cache hit (fresh)"**:
   không tải, không biên dịch. Nếu khác → tải bản mới (stream 64 KiB/lần, ghi
   atomic `.part` + rename), validate, biên dịch 2 part **song song**, commit
   manifest+stamp, gắn filter vào mọi tab hiện có **và** các tab tạo sau, rồi
   `webkit_user_content_filter_store_remove()` identifier cũ + xoá
   `v/<version-cũ>/` (dùng `remove_finish`: gọi không callback từng bị WebKit
   bỏ qua âm thầm).
5. Mọi lỗi (mạng, JSON hỏng, filter > 150k rule, WebKit không biên dịch được) →
   log `[WARN] [ADBLOCK_FAILED]` kèm lý do, **giữ bộ lọc đang dùng** và **tiếp tục
   duyệt web**.

### 3.3 Module portable `blocker/filters.c`

Quyết định "dùng cache nào / có nên tải lại / file JSON có hợp lệ" không phụ thuộc
GTK, nên được tách thành module C17 thuần:

- parse `manifest.json` không cần JSON library (bounded, không cấp phát);
- stamp `key=value` + chính sách 24h;
- validate stream + đếm rule của mảng JSON (24 MB không cần nằm hết trong RAM);
- tạo identifier versioned, sanitize tên file/version (dữ liệu từ mạng!);
- ghép path theo layout `<cache>/v/<version>/<file>`.

Module này có unit test `blocker/harness/filters_test.c` chạy trong ctest trên mọi
nền tảng.

### 3.4 Số đo (WSL, WebKitGTK 2.52.6, libsoup 3.6.6, 12 core)

| Chỉ số | Giá trị |
|---|---|
| `combined-part1.json` (24.5 MB) | **150.000 rule**, biên dịch **10.7–19.1 s**, ~340–405 MB RSS |
| `combined-part2.json` (16.1 MB) | 96.880 rule, ~8.5 s, ~291 MB |
| Mặc định (2 part) | `filters=2 rules=246880`, log `compile_ms=16822` |
| Peak RSS của tiến trình zephium lúc biên dịch lại | **~550 MB** |
| Warm start (store đã có filter đã biên dịch) | **~0.5 ms/filter**, `loaded_from_store=2` |
| Kích thước store | ~105 MB (2 part) / ~81 MB (`MODE=lists`, 4 filter) |

`ZEPHIUM_ADBLOCK_MODE=lists` dùng các list riêng (easylist + easyprivacy +
peter-lowe + ublock-badware = 243.989 rule, 4 filter) khi máy ít RAM hoặc muốn
fingerprint nhỏ hơn; đây cũng là fallback tự động nếu một part vượt giới hạn 150k rule.

### 3.5 Cấu hình

| Biến / cờ | Ý nghĩa |
|---|---|
| `ZEPHIUM_ADBLOCK=0` | tắt cho phiên chạy |
| `--no-adblock` | tắt cho phiên chạy (CLI) |
| `ZEPHIUM_ADBLOCK_DIR` | đổi thư mục cache (mặc định `$XDG_CACHE_HOME/zephium/adblock`) |
| `ZEPHIUM_ADBLOCK_MODE=lists` | dùng các list riêng thay vì 2 combined part |
| `ZEPHIUM_ADBLOCK_URL_BASE` | đổi base URL release (test/mirror) |
| `--adblock-status` | in trạng thái cache rồi thoát (0 = cache dùng được, 2 = chưa có) |

Filter đã biên dịch nằm trong store `$XDG_CACHE_HOME/zephium/content-filters`
(chung với filter của bản cũ); dữ liệu tải về nằm ở
`$XDG_CACHE_HOME/zephium/adblock/{manifest.json,manifest.stamp,v/<version>/*.json}`.

---

## 4. Build

### 4.1 Windows (native)

Yêu cầu: Windows 10/11 x64, WebView2 Runtime, MinGW-w64, **Rust** (GNU toolchain)
cho engine.

```powershell
cd windows
powershell -ExecutionPolicy Bypass -File fetch_sdk.ps1    # WebView2 SDK (1 lần)
powershell -ExecutionPolicy Bypass -File fetch_rust.ps1    # Rust gnu toolchain (1 lần)
mingw32-make                                               # build staticlib + exe
mingw32-make adblock-test                                  # unit + benchmark của engine
```

`fetch_rust.ps1` cài `rustup` + toolchain `stable-x86_64-pc-windows-gnu` vào
`%USERPROFILE%\.cargo` (không sửa PATH hệ thống). `windows/Makefile` tự tìm
`cargo` trên PATH rồi fallback `%USERPROFILE%\.cargo\bin\cargo.exe`.

### 4.2 Linux / WSL (WebKitGTK)

```bash
sudo apt-get install -y libwebkit2gtk-4.1-dev libsoup-3.0-dev libsqlite3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DZEPHIUM_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure     # gồm zephium_filters_test
./build/zephium --adblock-status               # version/rule/cache hiện tại
./build/zephium https://example.com            # log: [ADBLOCK] ...
```

Configure in ra `Found libsoup-3.0 ...: enabling adblock filter downloads`; nếu không có
libsoup-3.0 thì shell vẫn biên dịch (chỉ dùng được JSON đã cache, không tự cập nhật).
Lần đầu chạy (cache trống) hãy chờ ~20–40 s để tải + biên dịch xong ở background.

### 4.3 Kiểm thử Rust FFI

```powershell
cargo test --manifest-path blocker/rust/Cargo.toml
```

8 test: block/allow theo rule, `$redirect` (bytes thật + MIME), URL không hợp lệ
không bị chặn, cache roundtrip + cache hỏng bị từ chối, thread-affinity, panic
containment (fail open), cosmetic CSS, debug rule info.

---

## 5. Kiểm chứng

### 5.1 Đã kiểm chứng trên máy build

| Hạng mục | Cách chứng minh |
|---|---|
| C ABI + link MinGW | chương trình C gọi `zephium_adblock_*`, link staticlib, chạy PASS |
| Block/allow/redirect trên bộ lọc thật | bench harness: 153.705 rule, quyết định đúng cho doubleclick/gtag/… |
| Cache | save 6.8 MB / 11 ms, load 8 ms, rule count giữ nguyên |
| Panic containment | hook test → `ERR_PANIC` rồi check kế tiếp = ALLOW, tiến trình không chết |
| Thread-affinity | gọi từ thread khác → `ERR_THREAD`, engine vẫn dùng được ở thread gốc |
| Windows shell build + selftest | `mingw32-make` + `adblock_selftest.exe` (xem `windows/README.md`) |
| Windows shell build (clean) | `mingw32-make clean && mingw32-make` → exe 6.5 MB (engine link tĩnh) |
| Windows selftest | `adblock_selftest.exe`: **91/91 PASS**, 280.516 rule, avg 4.68 µs, p99 10.3 µs |
| Rust unit test | `cargo test`: 8/8 PASS |
| Portable filters module | `gcc -Werror blocker/filters.c + harness` → **263/263 check PASS** (cũng chạy trong ctest) |
| WebKitGTK build + test | clean configure/build `-DZEPHIUM_WERROR=ON`: **0 warning**, ctest **2/2**, `--batch` PASS |
| WebKitGTK E2E lạnh | cache trống → fallback built-in → tải 24.568.412 + 16.161.479 B → `filters=2 rules=246880 compile_ms=16822` → screenshot OK |
| WebKitGTK E2E nóng | `cache hit ... (warm start, no download)`, `loaded_from_store=2`; store chỉ còn 2 identifier hiện tại |
| WebKitGTK đổi version | release mới được tải/biên dịch/commit, identifier + thư mục version cũ bị xoá |
| WebKitGTK lỗi | connection refused / JSON hỏng / 150.001 rule → `ADBLOCK_FAILED`, giữ bộ lọc cũ, trang vẫn tải |
| Không có WebKit/libsoup | `-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE` → build headless `-Werror` sạch, ctest PASS |

### 5.2 Chưa kiểm chứng (cần desktop thật)

- **Lớp COM `WebResourceRequested` trên shell Windows**: toàn bộ logic quyết định đã
  được chạy (unit + benchmark), nhưng môi trường build hiện tại không mở cửa sổ WebView2,
  nên chưa có bằng chứng "trang thật không còn quảng cáo"/"`$redirect` hiển thị đúng".
  Phần GTK thì đã chạy E2E thật (tải + biên dịch + gắn filter + screenshot).
- Máy rất yếu/RAM thấp: 150k rule ≈ 340–405 MB RSS mỗi filter lúc biên dịch và ~550 MB
  peak cả tiến trình; nên dùng `ZEPHIUM_ADBLOCK_MODE=lists` nếu máy < 2 GB RAM trống.

### 5.3 Cách kiểm chứng thủ công

Windows:

```powershell
.\windows\zephium_win.exe https://example.com
Get-Content windows\zephium_win.log | Select-String "adblock"
# adblock: engine adblock-rust 0.13, 153705 rules, ...
# adblock: checks=... blocked=... redirected=... avg=...ns
```

Linux/WSL:

```bash
./build/zephium --screenshot /tmp/page.png --screenshot-delay 15 https://example.com
python3 read_log.py    # mục [ADBLOCK]
```

---

## 6. Bảo mật, quyền riêng tư, giấy phép

- Danh sách lọc tải qua **HTTPS** từ nguồn cố định; tên file lấy từ manifest được
  sanitize trước khi dùng làm path (chống path traversal), mỗi file có giới hạn kích
  thước và tổng dung lượng.
- Không có telemetry: số liệu chỉ ghi vào log cục bộ
  (`windows\zephium_win.log`, `/tmp/zephium_debug.log`); URL trong log đi qua
  `RedactUrl`/`zephium_nav_redact_url` như các log khác của dự án.
- Cache engine + danh sách lọc là dữ liệu công khai, không chứa lịch sử duyệt web;
  profile `zephium_profile.db` vẫn giữ DACL quyền riêng như trước.
- **Giấy phép**: adblock-rust là MPL-2.0 → shim trong `blocker/rust` cũng MPL-2.0
  (source có trong repo). Bộ lọc EasyList/uBO **không** được vendor vào repo (chỉ tải
  lúc chạy); công cụ chuyển đổi của bnema là GPL-3.0 nhưng dữ liệu JSON là đầu ra,
  không phải code được nhúng.

---

## 7. Sự cố thường gặp

| Hiện tượng | Xử lý |
|---|---|
| Windows: link báo thiếu `WaitOnAddress` | thiếu `-lsynchronization` trong `windows/Makefile` |
| Windows: `cargo` not found | chạy `windows\fetch_rust.ps1` hoặc thêm `%USERPROFILE%\.cargo\bin` vào PATH |
| Windows: log `adblock: engine unavailable` | chưa tải được danh sách (mạng/proxy); kiểm tra `%LOCALAPPDATA%\Zephium\adblock\lists` |
| Windows: mọi check trả ALLOW sau một panic | handle đã poison (cố ý, fail open); khởi động lại để có engine mới |
| GTK: `[WARN] [ADBLOCK_FAILED]` | WebKit không biên dịch được JSON (file hỏng/WebKit cũ); xoá `$XDG_CACHE_HOME/zephium/adblock` + `.../content-filters` để tải lại |
| GTK: chặn quá mạnh (site hỏng) | `ZEPHIUM_ADBLOCK=0 ./run_wsl.sh <url>` để tắt; hoặc `ZEPHIUM_ADBLOCK_MODE=lists`; hoặc xoá cache |
| GTK: lần đầu chậm / RAM tăng | Bình thường: ~40 MB tải + biên dịch 246.880 rule ở background (peak ~550 MB). Dùng `--adblock-status` để xem tiến độ cache |
| GTK: không có libsoup-3.0-dev | shell vẫn build/chạy, chỉ không tự cập nhật danh sách (log `download disabled`) |

---

## 8. Hướng phát triển

1. **WebSocket**: dùng CDP `Network.setBlockedURLs` (chỉ pattern URL) hoặc
   `Fetch.enable` để có điểm chặn thật cho ws/wss.
2. **Cosmetic filtering trên Windows**: dùng `zephium_adblock_cosmetic_css()` (đã
   có trong ABI) ở `NavigationCompleted`, inject `<style>`.
3. **`$removeparam`**: chặn rồi tự fetch URL đã rewrite qua WinHTTP và trả response
   — đúng ngữ nghĩa nhưng tốn thêm round-trip.
4. **`$csp`**: trả response có header `Content-Security-Policy` (cần fetch lại body).
5. **Đa luồng**: nếu cần kiểm tra request ngoài UI thread, build crate với
   `--no-default-features --features full-regex-handling,embedded-domain-resolver`
   (Engine trở thành Send+Sync) rồi bọc Mutex; hiện tại chưa cần.