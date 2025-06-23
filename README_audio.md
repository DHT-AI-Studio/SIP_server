# WebSocket SIP 音頻系統

這是一個基於 WebSocket 的 SIP 音頻系統，允許客戶端上傳 WAV 檔案並透過 WebSocket 控制服務器播放音頻。

## 新功能特色

1. **WAV 檔案上傳**: 客戶端可以上傳 WAV 檔案到服務器
2. **遠端播放控制**: 客戶端可以透過 WebSocket 指令控制服務器播放特定音檔
3. **Base64 編碼傳輸**: 支援透過 WebSocket 安全傳輸二進制 WAV 檔案
4. **實時 RTP 串流**: 服務器將上傳的音檔轉換為 RTP 封包並傳送
5. **雙向音頻**: 同時支援 RTP 發送和接收

## 檔案說明

- `ws_audio_server.c` - 音頻 WebSocket 服務器
- `ws_audio_client.c` - 音頻 WebSocket 客戶端 
- `Makefile_audio` - 編譯設定檔

## 編譯指令

```bash
# 編譯所有程序
make -f Makefile_audio all

# 只編譯服務器
make -f Makefile_audio ws_audio_server

# 只編譯客戶端
make -f Makefile_audio ws_audio_client

# 查看所有可用指令
make -f Makefile_audio help
```

## 使用方式

### 1. 啟動服務器

```bash
# 啟動音頻服務器（自動創建測試 WAV 檔和上傳目錄）
make -f Makefile_audio audio-server
```

服務器會：
- 監聽端口 8080
- 創建 `uploaded_wavs/` 目錄存放上傳檔案
- 生成測試 WAV 檔案 `sample.wav`

### 2. 啟動客戶端

```bash
# 連接到本地服務器
make -f Makefile_audio audio-client

# 連接到指定 IP 的服務器
make -f Makefile_audio audio-client-ip
```

### 3. 操作流程

1. **撥打電話**: 在客戶端選擇選項 1 或 2 撥打電話
2. **上傳 WAV 檔**: 選擇選項 5，輸入檔案路徑（例如 `sample.wav`）
3. **播放音檔**: 選擇選項 6，輸入檔案名稱（例如 `sample.wav`）
4. **接收 RTP**: 客戶端會自動接收並記錄 RTP 封包
5. **保存錄音**: 選擇選項 7 將接收的 RTP 保存為 WAV 檔案

## WebSocket 協議

### 客戶端發送的訊息

- `CALL:電話號碼` - 撥打電話
- `HANGUP` - 掛斷電話
- `WAV_UPLOAD:檔案名稱:Base64編碼資料` - 上傳 WAV 檔案
- `PLAY_WAV:檔案名稱` - 播放指定檔案

### 服務器發送的訊息

- `RTP:十六進制資料` - RTP 封包資料
- `WAV_ACK:確認訊息` - 操作確認訊息

## 技術特點

### 移除的功能（相對於原版）
- 移除了自動 10 秒後播放音檔的功能
- 改為完全透過 WebSocket 指令控制

### 新增的功能
- Base64 編碼/解碼 WAV 檔案傳輸
- 檔案上傳管理系統
- 遠端播放控制
- 上傳目錄管理 (`uploaded_wavs/`)

### 音頻處理
- 支援 G.711 μ-law 格式
- 8000Hz 採樣率，單聲道
- RTP 封包大小：160 字節有效載荷
- 20ms 封包間隔

## 故障排除

### 編譯問題
```bash
# 清理並重新編譯
make -f Makefile_audio clean
make -f Makefile_audio all
```

### 連接問題
- 確認服務器正在運行
- 檢查防火牆設定
- 確認 IP 地址和端口正確

### 音頻問題
- 確認 WAV 檔案格式正確（8000Hz, μ-law）
- 檢查檔案大小（最大 1MB）
- 確認通話已建立再播放音頻

## 範例操作

1. 在終端 1 執行：`make -f Makefile_audio audio-server`
2. 在終端 2 執行：`make -f Makefile_audio audio-client`
3. 在客戶端選擇 1 撥打預設號碼
4. 等待通話建立
5. 選擇 5 上傳 `sample.wav`
6. 選擇 6 播放 `sample.wav`
7. 對方應該會聽到音頻
8. 選擇 7 保存接收到的 RTP 為 WAV 檔案

## 與原版的差異

| 功能 | 原版 (ws_demo_*) | 新版 (ws_audio_*) |
|------|------------------|-------------------|
| 自動播放 | ✓ 10秒後自動播放 | ✗ 移除自動播放 |
| 檔案上傳 | ✗ | ✓ Base64 上傳 |
| 遠端控制 | ✗ | ✓ WebSocket 指令 |
| 檔案管理 | ✗ | ✓ 上傳目錄管理 |
| 協議名稱 | sip-demo-protocol | sip-audio-protocol |

這個新系統提供了更靈活的音頻管理功能，完全透過 WebSocket 進行控制。 