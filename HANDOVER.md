# Space Clock（M5Stack Core2）專案交接文件

> 版本：韌體 3.0.44（本機最新；GitHub 上公開的 OTA 版本是 3.0.35）
> 倉庫：`houtacheng/m5stack-core2-space-clock`（分支 `main`）
> 這份文件給接手的人看：先讀第 1～4 節就能建置、燒錄、更新；第 6～8 節講架構與各功能；第 10～11 節是目前的問題與踩過的坑。

---

## 1. 專案是什麼

在 M5Stack Core2 上跑的原生 C++ 韌體（Arduino 框架），是一個「太空時鐘」，主要功能：

| 功能 | 說明 |
|---|---|
| 時鐘 | 三種畫面（Space / Flip / Matrix）、NTP 校時、22 個城市時區並自動切換日光節約時間 |
| 鬧鐘 | 20 組、星期重複、音效、底部 LED 燈效 |
| 靜心計時 | 倒數、背景白噪音、燈光；倒數中不進待機 |
| 小夜燈 | 底部 10 顆 LED；螢幕關閉時可亮（電源鍵關閉與待機自動關閉各自獨立設定） |
| 訊息 | Signal + Microsoft Teams，經過 NAS 上的「橋接服務」轉進來，可回覆、已讀同步；未讀時 LED 亮淺藍呼吸燈（亮度可在後台調） |
| Companion | Bitfocus Companion 的衛星面板（3×2 按鍵、4 頁）；支援區網 TCP 與網際網路 WSS |
| 情緒觀察 | 多頁表單，資料送到 PocketBase API，離線時存本機佇列再補傳；「我的紀錄」統計頁 |
| Home Assistant | Assist 語音（按住說話）、MQTT 狀態發布與設定 |
| 聽法模式 | 左滑進入的 SD 卡 MP3 播放器：資料夾、播放清單、速度 0.5–2X（保持音高）、藍牙耳機或內建喇叭、網頁上的 SD 檔案管理 |
| 遠端除錯 | 類似 adb：`tools/clockctl.py` 經網路看狀態、記錄、截圖、執行測試指令、更新韌體 |
| OTA 更新 | 網頁後台上傳、GitHub 上的 `firmware/manifest.json` 檢查更新 |

---

## 2. 倉庫結構

```
SpaceClockNative/            ← 韌體（Arduino sketch 資料夾，名稱不能改）
  SpaceClockNative.ino       ← 主程式，約 11,000 行：畫面、網頁後台、迴圈、所有功能
  config.h                   ← 版本號 SPACE_CLOCK_VERSION、OTA manifest 網址
  listen_player.h            ← 聽法模式的播放引擎（解碼、時間伸縮、SD、播放清單）
  listen_bt.h                ← 藍牙 A2DP 傳送（環形緩衝、淡入淡出、按鍵、重連）
  wsola.h                    ← 變速不變調演算法（WSOLA）
  adb_serial.h               ← Serial 外包：同時把輸出存進環形緩衝，供 /adb/log 讀取
  emotion_tls.h              ← 情緒 API 的根憑證
  mqtt_guide.h               ← 網頁後台的 MQTT 說明頁
  assets.h / art/ / art-source/ ← 圖片資產（由 tools/generate_assets.sh 產生）
  generated/                 ← 字型與音效（C 陣列），由工具產生，不要手改
  listen_chars.txt           ← 23 px 字型要收錄的字（SD 檔名常用字）
  wifi_secrets.example.h     ← 本機專用 Wi‑Fi 預設值範本（wifi_secrets.h 不進 git）
  README.md / NOTICE.md      ← 使用說明（功能歷史很長）、第三方授權
bridge/                      ← NAS 上的橋接服務（Python + Docker）：Signal、Teams
firmware/                    ← 公開的 OTA 映像與 manifest.json（發布時才更新）
tools/                       ← 建置、字型、發布、遠端除錯工具
HANDOVER.md                  ← 本文件
```

**不要提交**：`SpaceClockNative/wifi_secrets.h`、`.arduino-build/`、`.arduino-cache/`（`.gitignore` 已排除）。

---

## 3. 硬體與資源限制（最重要的背景知識）

- **晶片**：ESP32‑D0WDQ6，雙核 240 MHz，**4 MB PSRAM**，16 MB flash。
- **內部 RAM 非常緊**：一般模式可用約 **35～45 KB**（`/adb/info` 的 `memory.internal_free`）。TLS、藍牙、每個 FreeRTOS 工作的堆疊都吃這塊。新增工作或大陣列前先看這個數字。
- **flash**：app 分割區 6.5 MB（`default` 16 MB 配置，有兩個 app 槽做 OTA）。目前映像約 5.99 MB（91%）。
- **DROM（唯讀資料）約 3.5 MiB 為上限**，字型都在這裡；`tools/validate_public_build.py` 會檢查。
- **螢幕與 SD 卡共用同一條 SPI 匯流排**：不要在別的執行緒寫 SD 的同時讓主執行緒畫圖（3.0.6–3.0.23 曾因此弄壞 SD；現在網頁上傳的處理在主迴圈裡一小段一小段做）。
- **SD 卡必須是 MBR 分割表 + FAT32**。GPT（GUID）分割表讀不到——Mac 磁碟工具要選「主開機記錄」。
- **USB 序列埠只是 UART 橋**：不能把 SD 卡掛成磁碟；開啟序列埠會重置板子（DTR）。
- 內建喇叭與藍牙不能同時使用；聽法模式時為了釋放 RAM，**整個網路（Wi‑Fi）是關的**。

---

## 4. 建置、燒錄、發布

### 4.1 需求
- `arduino-cli`（或 Arduino IDE）
- 板子套件：**M5Stack ESP32 套件 3.2.5**，FQBN `m5stack:esp32:m5stack_core2`，選項 `PSRAM=enabled,PartitionScheme=default`
- 函式庫（`~/Documents/Arduino/libraries`）：M5Unified/M5GFX、ArduinoJson（v7 的 `JsonDocument`）、WiFiManager、PubSubClient、WebSockets、Adafruit_NeoPixel、**ESP32-A2DP 1.8.11（從 GitHub 安裝）**、Helix MP3 解碼（`mp3dec.h`，來自 ESP8266Audio 的 libhelix-mp3）
- Python 3（工具腳本）

### 4.2 正式建置（會做所有檢查）
```bash
bash tools/build_public.sh
```
它會依序：驗證字型 → 用 `tools/make_bt_sdk.py` 建立「修補過的 SDK 副本」（`.arduino-build/sdk-esp32`）→ 以 `-O2`、`-DSPACE_CLOCK_PUBLIC_BUILD` 編譯到 `.arduino-build/release-<版本>/` → 驗證映像（旗標、版本字串、字型點陣、不含私人 Wi‑Fi 設定、大小）。

### 4.3 手動編譯（開發用，約 1～3 分鐘）
```bash
SDK="$PWD/.arduino-build/sdk-esp32"
arduino-cli compile \
  --fqbn m5stack:esp32:m5stack_core2:PSRAM=enabled,PartitionScheme=default \
  --build-property compiler.optimization_flags=-O2 \
  --build-property compiler.cpp.extra_flags=-DSPACE_CLOCK_PUBLIC_BUILD \
  --build-property "compiler.sdk.path=$SDK" \
  --build-path .arduino-build/release-X SpaceClockNative
```
**三個一定要記得的地方：**
1. **不要覆寫 `build.extra_flags`**（會丟掉 PSRAM 初始化，Matrix 畫面會空白；`config.h` 有 `#error` 保護）。
2. **一定要用修補過的 SDK**（`compiler.sdk.path`）。標準 SDK 在開 PSRAM + 藍牙時 IRAM 不夠（差約 9 KB）。先跑一次 `python3 tools/make_bt_sdk.py`。
3. 用 **`-O2`**：Helix 解碼快約 20%，2 倍速播放需要。

### 4.4 字型檢查（改了任何中文字串之後務必跑）
```bash
python3 tools/validate_ui_fonts.py
```
UI 字型是點陣字型，**每個出現在 .ino 的中文字都必須在字型裡**，否則會顯示空白，而且會擋住正式建置。`source_han_sans_8.h`（16 px）涵蓋全部 CJK；`source_han_sans_14.h`（23 px，約 1,000 字，稀疏）只含常用字。缺字時：換個詞，或用
`tools/extend_gfx_font.py --index 5 --pixels 23 --from-source SpaceClockNative.ino SpaceClockNative/listen_chars.txt` 加字（需要 Source Han Sans 原始字型，並留意 DROM 上限）。

### 4.5 燒錄到時鐘
- **OTA（優先）**：`python3 tools/clockctl.py <IP> --token <權杖> ota <bin>`（見第 9 節）。速度約 1 分鐘。傳之前它會先開 `/sd` 頁，關掉 Wi‑Fi 省電，否則在訊號差時容易逾時。OTA 回應後時鐘還在寫入、重開，**約 30～60 秒內不要碰序列埠**（開序列埠會重置，可能中斷更新）。
- **USB**：`arduino-cli upload -p /dev/cu.usbserial-* --fqbn …,UploadSpeed=115200 --input-dir .arduino-build/release-X`。成功的標準是輸出裡有 **4 次 `Hash of data verified`**。這條 USB 很容易中斷（「chip stopped responding / TX path down」）：寫迴圈重試、每次加 `timeout 400`、換線。**不要同時在同一個 build 資料夾重新編譯**。
- 虛擬機（Parallels）會搶走 Core2 的 USB，序列埠就消失。

### 4.6 發布到 GitHub（OTA 供其他裝置自動更新）
1. 改 `config.h` 版本 → `bash tools/build_public.sh`
2. `cp .arduino-build/release-<版本>/SpaceClockNative.ino.bin firmware/SpaceClockNative-OTA.bin`
3. 更新 `firmware/manifest.json`（`version`、`sha256`、`size`、`notes`；sha256 用 `shasum -a 256`）
4. commit、`git push origin main`（偶爾 GitHub 回 500，重試即可）
5. 驗證：`gh api repos/houtacheng/m5stack-core2-space-clock/contents/firmware/manifest.json` 與下載後的 sha256 一致。`raw.githubusercontent.com` 有幾分鐘快取，會比 API 慢。

---

## 5. 設定與機密資料

- 一般設定存在 NVS（`Preferences` 命名空間 `spaceclock`）與 SPIFFS；OTA 更新**不會**清掉這些。
- **Wi‑Fi**：最多 10 組設定檔（網頁後台輸入），**成功連上的網路會自動記入空的欄位**（3.0.18 起）；另有「設定熱點」（SSID `SpaceClock-Setup`）存單一網路。
- `wifi_secrets.h`（本機預設 Wi‑Fi，不進 git）只在非公開建置使用；公開建置會用 `-DSPACE_CLOCK_PUBLIC_BUILD` 排除。
- 每台時鐘有自己的**遠端除錯權杖**（NVS 鍵 `adbTok`，首次開機隨機產生），顯示在網頁後台「韌體」頁，也在 `/adb` 路由使用。
- 橋接服務的存取權杖在橋接後台頁面（只能從區網開啟）。
- **安全模型**：網頁後台本身沒有密碼，假設區網可信。請勿把時鐘的 80 與 8081 埠直接暴露到網際網路。

---

## 6. 韌體架構

### 6.1 執行緒（FreeRTOS 工作）

| 工作 | 堆疊 | 核心 | 作用 |
|---|---|---|---|
| `loopTask`（Arduino `loop()`） | 8 KB | 1 | 觸控、繪圖、網頁伺服器、鬧鐘、MQTT、HASS、畫面狀態 |
| `net`（`netTask`） | 16 KB | 1 | 背景 HTTPS：Signal 輪詢、行事曆、**情緒 API（離線佇列上傳、統計、連線檢查）**、首頁網路檢查 |
| `companion`（`companionRxTask`） | 5 KB | 1 | 區網 TCP 的 Companion：讀行、立刻回 PONG、定時 PING（只在有設區網主機時才建立） |
| `companionWs`（`companionWsTask`） | 6 KB | 1 | 網際網路 WSS 的 Companion：連線（TLS 握手約 1.2 秒）、2 秒一次 PING |
| `listen`（`decodeTask`） | 5 KB | 1 | MP3 解碼 → 喇叭或藍牙環形緩衝 |
| `btstart` / BT 堆疊 | — | 0 | 藍牙啟動與 Bluedroid |
| `loopWatchdog` | 3 KB | 0 | 主迴圈 30 秒沒動作就重啟並記錄卡在哪一段 |

### 6.2 主迴圈原則
- **`loop()` 不可以做長時間阻塞**。曾經的症狀：畫面凍結、觸控沒反應、Companion 被伺服器踢掉。長工作要放 `netTask`，或拆成小步驟（例如 `sdRawPump()` 每次最多 25 ms）。
- 迴圈裡的區塊用 `SLOW_SECTION("名稱", 程式)` 包起來：超過 300 ms 會印 `[slow] 名稱 took N ms rssi X`，同時把名稱記給看門狗用。
- 工作之間的資料交換都用**佇列＋互斥鎖**：`NetLock`（一次一個 HTTPS）、`CompLock`（Companion 的 `WiFiClient`）、`EmoQueueLock`（情緒離線佇列檔）、`listen::SdLock`（所有 SD 存取）。**畫面繪製只能在 `loop()` 執行緒**；背景工作只設旗標或丟訊息給主迴圈處理。

### 6.3 畫面（`enum class Screen`）
`Clock, Menu, Faces, Companion, Alarms, Settings, Meditation, MeditationSettings, EmotionObservation, EmotionRecords, EmotionSettings, EmotionReminder, HassAssist, FirmwareUpdate, About, NightLight, Calendar, Messages, MessageDetail, MessageReply, MessageFull, MessageHub, WifiSwitch, Listen`。每個畫面有 `drawXxx()`，觸控在 `handleTouch()` 分派。聽法模式內還有子視圖 `ListenView { Browse, Now, Pick, Confirm, Bluetooth, Setting }`。

### 6.4 電源與時間
- 外接電源（VBUS）或充電中 = 不省電；只靠電池時進「省電模式」（螢幕關閉時 CPU 降到 80 MHz）。Wi‑Fi 省電（modem sleep）在外接電源、網頁上傳、Companion 畫面時關閉（省電會讓每個請求慢 100–800 ms）。
- 聽法模式把 CPU 固定在 240 MHz——**執行中改頻率會讓藍牙連線斷掉**（HCI reason 0x108，連線監督逾時）。
- 時間：系統時鐘 + POSIX 時區規則（`TIME_ZONES[]`，含日光節約時間）。開機時用 RTC（存當地時間）設定系統時鐘，之後不論有沒有 Wi‑Fi 都自動切換日光節約時間。已實測：2026‑11‑01 秋季 `01:59:55 EDT → 01:00:07 EST`，2027‑03‑14 春季 `01:59:55 EST → 03:00:07 EDT`。

---

## 7. 各功能模組筆記

### 7.1 訊息（Signal / Teams）與橋接服務
- 時鐘每 5 秒（省電時更久）向橋接服務 `GET /api/messages?since=<id>` 輪詢；先試區網網址，失敗改用 Cloudflare Tunnel 外網網址。回傳含新訊息、`unread`（未讀 id 清單，跨裝置同步已讀）、`teams.state`（`off`/`login`/`ok`）。
- 回覆：`POST /api/reply`。已讀：`/api/read`。立即同步：`/api/sync`。
- 訊息陣列在 PSRAM，**500 則 × 每則約 3.4 KB**（`text[3200]`）。滿了以後一次移除 64 則最舊的已讀訊息。
- **Teams 用裝置代碼登入**（公司帳號）。橋接服務存 refresh token；Microsoft 拒絕續期時橋接會清掉它，必須人工到橋接後台「登入 Teams」重新登入（這一步不能由程式代勞）。登出時訊息中心的 Teams 圖示會顯示橘色「需重新登入」（3.0.42，需橋接服務也更新）。
- 橋接服務（`bridge/`）：Python，與 `signal-cli-rest-api`（json-rpc 模式）一起用 Docker 跑在 NAS。部署方式請看 `bridge/README.md` 與 `docker-compose.yml`；改 `bridge.py` 後要在 NAS 上重新啟動容器（倉庫裡沒有自動部署）。

### 7.2 Companion（衛星）
- Companion 的衛星 API：連上後伺服器送 `BEGIN`，時鐘回 `ADD-DEVICE`，伺服器回 `ADD-DEVICE OK` 並推 `KEY-STATE`（按鍵圖 PNG base64）。**必須定期送 `PING`**，伺服器約 5 秒沒收到任何資料就關連線（3.0.30 之前網際網路模式沒送，每 5.5 秒掉線一次）。
- 四頁 = 四個衛星裝置（ID `<晶片>-p1…p4`）。如果四頁都用同一個網際網路網址且沒有區網主機，就用**一條共用連線同時登錄四個衛星**（`companionSharedLink()`），換頁不重連、連線在背景保持。否則退回舊做法：一次一頁，離開頁面就斷線。
- 每頁的按鍵資料各自存在記憶體（`companionLabelsAll/ColorsAll/ImagesAll[頁][鍵]`）。連線還沒就緒時按下的按鍵會排隊 2.5 秒內補送。

### 7.3 情緒觀察
- 表單資料先存本機佇列（SPIFFS `/emotion_queue.json`），背景（`maintainEmotionBackground`）逐筆上傳，失敗時以 15 秒起、最長 5 分鐘退避。送出時不再等網路。
- 統計頁先顯示快取，背景抓到新資料後自動更新。
- 每個 HTTPS 連線（TLS 握手）約要 2 秒，所以不要在 `loop()` 裡呼叫。

### 7.4 聽法模式（最複雜的部分）
- 進入：時鐘首頁左滑。進入時先 `pauseNetwork()`（關 Wi‑Fi、停網頁與背景工作），再啟動藍牙；離開時反向。
- 播放引擎：Helix 解碼 → 單聲道 → （變速時）WSOLA（取樣率 > 32 kHz 先降頻）→ 喇叭（M5.Speaker 頻道 6）或藍牙環形緩衝（44.1 kHz 立體聲，`RING_FRAMES=32768`，以 `fillCap` 控制領先量）。16 KB 預讀、`SLOT=6144`。速度設定、播放模式存在 SD 的 `/.listen.txt`；播放清單在 `/.playlists/`。
- 藍牙：`LinkSource`（繼承 `BluetoothA2DPSource`）覆寫了啟動事件，**取消函式庫內寫死的 10 秒延遲**並立刻連線；記住的耳機存在 NVS 命名空間 `listenbt`；「喇叭模式」（`lbt::speakerMode`）只是不使用藍牙，不會刪掉記住的耳機。時鐘為「可連線但不可被搜尋」，耳機可以自己連進來。
- 暫停時真的把 A2DP 串流 suspend/start；淡入淡出避免爆音；`pausedByLink` = 耳機斷線時暫停等它回來（不會自動切到喇叭）。
- 長按「速度／倒退／前進」進設定頁（預設選項框 + 滑軌）。
- 網頁 `/sd`：SD 檔案管理（上傳、刪除、搬移、改名、新增資料夾）。上傳走 8081 埠的「快速上傳」（`sdRawPump`），約 225 KB/s（Wi‑Fi 為瓶頸，SD 本身約 870 KB/s）。上傳中與開啟 `/sd` 後 10 分鐘內，時鐘不進待機。
- 本機有改過的函式庫：`ESP32-A2DP/src/BluetoothA2DPSource.cpp` 加了一行 ACL 斷線原因的 `ESP_LOGW`（診斷用，非必要）。

### 7.5 LED 優先順序（`updateAlarmBaseLights`）
鬧鐘 > 小夜燈畫面 > 手動小夜燈 > 靜心 > 螢幕關閉時的小夜燈 > **未讀訊息淺藍呼吸燈**（聽法模式中不亮）。每次亮起原因變化會印 `[led] <原因> (color …, strength …%)`。**尚未解決**：有使用者看到過「底部 LED 亮白燈」，原因還沒抓到（靜心暫停會常亮暖白色 20%、預設小夜燈是暖白色——先查這兩個）。

---

## 8. 版本與分支狀況

- 公開（GitHub `main` 與 OTA）：**3.0.35**。
- 本機已 commit 但**尚未推送／發布**：3.0.39～3.0.44（遠端除錯、離線 DST、Teams 需重新登入提示、看門狗、Wi‑Fi 省電策略、訊息清單批次修剪、MQTT 降頻）。發布前請照 4.6 做，並實機確認。
- 版本號慣例：每次功能或修正就加 patch（3.0.x），`config.h` 的 `SPACE_CLOCK_VERSION`。

---

## 9. 遠端除錯與測試

### 9.1 `tools/clockctl.py`（只用 Python 標準函式庫）
```bash
python3 tools/clockctl.py <時鐘IP> --token <權杖> info          # 狀態 JSON
python3 tools/clockctl.py <IP> --token <權杖> log -f            # 持續追蹤序列埠記錄
python3 tools/clockctl.py <IP> --token <權杖> cmd "t:pwr"       # 執行除錯指令並回傳輸出
python3 tools/clockctl.py <IP> --token <權杖> shell             # 互動式
python3 tools/clockctl.py <IP> --token <權杖> shot out.png      # 螢幕截圖
python3 tools/clockctl.py <IP> --token <權杖> reboot
python3 tools/clockctl.py <IP> --token <權杖> ota file.bin
```
權杖在網頁後台「韌體」頁；也可放環境變數 `CLOCK_TOKEN` 或 `~/.clockctl`。對應的 HTTP 路由：`/adb/info`、`/adb/log?from=`、`/adb/cmd`（POST）、`/adb/shot`、`/adb/reboot`（POST），都需 `?token=`。

`info` 內容：版本、運作時間、重開原因、**`previous_boot_problem`（看門狗記錄的「上次卡在哪」）**、內部／PSRAM 記憶體、各工作堆疊剩餘、Wi‑Fi（SSID/RSSI/IP）、電池、畫面狀態、本地時間與時區、聽法／SD／藍牙、MQTT／Companion／HASS 連線、未讀數。

### 9.2 `t:` 測試指令（序列埠或 `clockctl cmd`）
`t:cal` `t:emo` `t:sig` `t:net` `t:pwr` `t:touch` `t:time [epoch]` `t:teams 1|0` `t:unread` `t:markall` `t:enter` `t:exitlisten` `t:listen <路徑>` `t:play <檔>` `t:speed N` `t:seek f` `t:lstat` `t:lstop` `t:lui` `t:bench <檔> <碼> <秒>` `t:sdw <MB>` `t:rename` `t:glyph <字>` `t:bt` `t:btscan` `t:btlist` `t:btconnect N` `t:btoff` `t:thr N` `t:sleep` `t:wake` `t:key play|pause|next|prev` `t:nosaved`。
（指令定義都在 `runDebugCommand()`。）

### 9.3 序列埠
115200 baud。自己寫小腳本用 pyserial，開啟前設 `dtr=False, rts=False`（仍會重置一次板子）。**不要在使用者正在用時鐘時開序列埠。**

### 9.4 看門狗
`loop()` 超過 30 秒沒跑就重啟，並把卡住的區塊名稱、運作時間、剩餘記憶體寫進 RTC 記憶體；重開後 `info.previous_boot_problem` 與開機記錄 `[boot] reset reason: …` 會顯示。長時間且刻意的阻塞（Wi‑Fi 設定熱點、韌體下載／上傳）會自己餵狗；新增長阻塞工作時要在迴圈裡更新 `loopHeartbeatMs` 或使用 `loopWatchdogHold`。

---

## 10. 已知問題與建議優化（依優先順序）

1. **藍牙連線慢（10～60 秒）**：時鐘一直呼叫 AirPods，每次逾時 5.1 秒；已知 AirPods 在連著 iPhone 時常不回應。3.0.21 改成「可連線」讓耳機能連進來，**尚未實測是否有效**。可試：縮短 page timeout、進入聽法模式前先啟動藍牙、搭配 `[bt +ms]` 時間記錄分析。
2. **網路不好時整機卡頓／當機**：曾在 RSSI ‑81 dBm 時 MQTT 發布卡 1.8 秒、訊息抓取卡 3.5 秒，甚至主迴圈停住。看門狗只是「自動恢復」；根本解法是把 `maintainMqtt()`、`maintainHassAssist()`、`pollSignalMessages()` 的阻塞部分都搬到背景工作（現在只有 Signal/行事曆/情緒 API 在 `netTask`）。
3. **主迴圈仍有數百毫秒的停頓**：`[slow] mqtt` 每次 300–700 ms（即使 Wi‑Fi 不省電）、`[slow] signal` 300–2000 ms。3.0.44 已降低 MQTT 頻率、改善訊息清單修剪，但 MQTT 的 JSON 組裝與發布仍在主執行緒。訊息結構每則 3.4 KB，建議改成環形緩衝或把 `text` 改為指標。
4. **白色 LED 原因未查出**（見 7.5）。
5. **2 倍速播放餘裕小**：48 kHz 立體聲 256 kbps 檔案解碼負載約 60%。已加大預讀與緩衝；若還有爆音，考慮降低解碼成本或在 PSRAM 之外給解碼更多快取。
6. **SD 卡相容性**：只認 MBR + FAT32。大容量 SDXC（128 GB）在 SPI 模式的相容性沒有實測；遇到讀不到先換小容量卡。
7. **3.0.44 對 10.41.10.195 這台更新時失聯**（更新到一半逾時後一直連不上），原因不明；若該裝置異常需 USB 重燒。
8. **Companion 共用連線**（四頁一條）只做了連線穩定性測試，換頁與按鍵的實機體驗還沒有使用者回報。
9. **Teams 登入有時效**：公司設定可能讓 refresh token 數天到數週失效。若要根治需要和公司 IT 確認條件式存取原則，或改用其他授權方式。
10. **時區清單**沒有大西洋時間（Halifax 等）；要加就在 `TIME_ZONES[]` 補 POSIX 規則（例如 `AST4ADT,M3.2.0/2,M11.1.0/2`）。
11. **程式結構**：`SpaceClockNative.ino` 11,000 行單檔，沒有自動化測試。建議逐步拆檔（網頁後台、訊息、情緒、Companion 各一個 .h/.cpp），並為 WSOLA、JSON 解析、時區規則補單元測試。
12. 網頁後台沒有密碼（見第 5 節）。

---

## 11. 踩過的坑（接手前先看）

- **Arduino 自動產生函式原型**：在檔案前段（型別定義之前）定義函式，會讓後面用到型別的原型出錯（"X does not name a type"）。要放在前面的小東西改用 `#define` 或變數。
- **新增中文字串前先想字型**：缺字會被 `validate_ui_fonts.py` 擋下；網頁後台的中文也算在內。
- **zsh 不會自動切字**：`set -- $var` 在 zsh 不拆字；寫 shell 腳本時用函式與引號。
- **TLS 要吃很多內部 RAM**：新增工作的堆疊會讓 `internal_free` 掉幾 KB，TLS 就可能失敗（「TLS 記憶體不足」）。改完看 `info`。
- **ESP32 的 `WiFiClient`/`WebSocketsClient` 不是執行緒安全**：只在一個工作裡用，其他執行緒用佇列或旗標溝通。
- **OTA 時不要碰序列埠**；USB 燒錄的 3 個區塊驗證 ≠ 成功，要 4 個。
- **聽法模式**：記憶體、CPU 頻率、網路狀態彼此牽動，改動前先讀 `enterListenMode()` / `leaveListenMode()` / `pauseNetwork()` / `resumeNetwork()`。
- **Companion 要求每幾秒有資料往來**；心跳設太緊或沒送，都會被判定離線。
- **GitHub 推送偶爾 500**、`raw.githubusercontent.com` 有快取：發布後用 API 驗證，不要只看 raw。

---

## 12. 發布前檢查清單

- [ ] `config.h` 版本已更新，`bash tools/build_public.sh` 通過（字型、旗標、映像大小、無私人 Wi‑Fi）
- [ ] 至少一台實機用 OTA 更新、重開後 `clockctl info` 確認版本與 `previous_boot_problem` 為空
- [ ] `internal_free` 在一般模式不低於約 25 KB；`stack_bytes_never_used` 沒有接近 0
- [ ] 聽法模式：進入／退出、播放、藍牙（有耳機時）、2 倍速各試一次
- [ ] Companion、訊息、情緒觀察送出各試一次
- [ ] 更新 `firmware/SpaceClockNative-OTA.bin` 與 `manifest.json`，推送後用 API 驗證 sha256
- [ ] 若 `bridge/bridge.py` 有改，通知負責 NAS 的人重新部署
