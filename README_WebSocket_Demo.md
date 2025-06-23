# WebSocket SIP Demo

這個 demo 展示了如何使用 WebSocket 來控制 SIP 通話，並實時傳輸 RTP 數據。

## 功能概述

- **WebSocket 服務端** (`ws_demo_server.c`): 接收客戶端的通話請求，發起 SIP 通話，播放音檔，並將接收到的 RTP 封包轉發給 WebSocket 客戶端
- **WebSocket 客戶端** (`ws_demo_client.c`): 向服務端發起打電話請求，接收 RTP 封包並記錄到終端
- **RTP 包裝器** (`ws_rtp_wrapper.c/.h`): 擴展現有 RTP 功能，支持自定義回調函數

## 系統要求

- Linux 系統
- libwebsockets 開發庫
- OpenSSL 開發庫
- pthread 支持

## 安裝依賴

在 Ubuntu/Debian 系統上：

```bash
sudo apt-get update
sudo apt-get install libwebsockets-dev libssl-dev build-essential
```

或使用 Makefile：

```bash
make -f Makefile_ws install-deps
```

## 編譯

使用專用的 Makefile 編譯：

```bash
make -f Makefile_ws
```

這將會：
1. 編譯 SIP 庫文件
2. 編譯 WebSocket 包裝器
3. 編譯服務端和客戶端程序
4. 創建示例音檔 `sample.wav`

## 使用步驟

### 1. 啟動 WebSocket 服務端

```bash
./ws_demo_server
```

服務端將在端口 8080 上監聽 WebSocket 連接。

### 2. 啟動 WebSocket 客戶端

在另一個終端中：

```bash
./ws_demo_client
```

客戶端將自動連接到 localhost:8080。

### 3. 使用客戶端界面

客戶端提供以下選項：

1. **撥打電話 (預設號碼)** - 使用預設號碼 0938220136
2. **撥打電話 (自定義號碼)** - 輸入自定義電話號碼
3. **掛斷電話** - 結束當前通話
4. **顯示統計** - 查看接收到的 RTP 封包數量
5. **退出** - 關閉客戶端

### 4. 觀察輸出

- **服務端** 將顯示 SIP 通話過程和 RTP 接收情況
- **客戶端** 將實時顯示接收到的 RTP 封包信息

## 文件結構

```
sip/
├── ws_demo_server.c       # WebSocket 服務端
├── ws_demo_client.c       # WebSocket 客戶端
├── ws_rtp_wrapper.h       # RTP 包裝器頭文件
├── ws_rtp_wrapper.c       # RTP 包裝器實現
├── create_sample_wav.c    # 示例音檔生成器
├── Makefile_ws           # 專用編譯文件
├── sample.wav            # 生成的示例音檔
└── lib/                  # 現有的 SIP 庫文件
    ├── sip_client.h
    ├── sip_client.c
    ├── rtp.c
    └── ...
```

## 工作流程

1. 客戶端通過 WebSocket 發送 `CALL:電話號碼` 消息
2. 服務端接收到請求後啟動 SIP 線程
3. SIP 線程發起呼叫並等待 200 OK 響應
4. 呼叫建立後，服務端開始播放音檔（RTP 發送）
5. 同時啟動 RTP 接收器監聽對方的音頻
6. 接收到的 RTP 數據通過回調函數轉發給 WebSocket 客戶端
7. 客戶端接收並記錄 RTP 封包信息到終端
8. 通話持續 30 秒後自動結束，或收到 HANGUP 消息時結束

## 配置參數

可以在代碼中修改以下參數：

- **SIP 服務器**: `SIP_SERVER` (默認: 192.168.1.170)
- **WebSocket 端口**: `WS_PORT` (默認: 8080)
- **RTP 端口**: `LOCAL_RTP_PORT` (默認: 32002)
- **通話持續時間**: 在 `sip_call_thread` 函數中修改

## 故障排除

### 編譯錯誤
- 確保安裝了所有依賴庫
- 檢查 libwebsockets 版本是否兼容

### 連接問題
- 確保 WebSocket 端口 8080 未被佔用
- 檢查防火牆設置

### SIP 通話失敗
- 驗證 SIP 服務器地址和認證信息
- 檢查網絡連接和 SIP 端口

### RTP 數據接收問題
- 確保 RTP 端口未被佔用
- 檢查 NAT/防火牆配置

## 清理

清理編譯文件：

```bash
make -f Makefile_ws clean
```

## 注意事項

1. 此 demo 避免修改現有的 lib/ 文件，通過包裝器擴展功能
2. RTP 監控使用獨立的端口避免與現有接收器衝突
3. 所有操作都有詳細的日誌輸出便於調試
4. 程序支持 Ctrl+C 安全退出 