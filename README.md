# WebSocket SIP 音頻服務器 (ws_audio_server.c) 文檔

## 項目概述

`ws_audio_server.c` 是一個整合了 **WebSocket 通信**、**SIP 協議**和 **RTP 音頻處理** 的多線程音頻服務器。它實現了完整的 VoIP 通話功能，支持實時音頻傳輸、WAV 檔案上傳播放，以及通過 WebSocket 與客戶端進行交互控制。

### 核心功能
- **WebSocket 服務端**：接收客戶端指令（撥號、掛斷、上傳音檔、播放音檔）
- **SIP 客戶端**：與 SIP 服務器建立語音通話
- **RTP 音頻處理**：雙向實時音頻數據傳輸
- **檔案管理**：WAV 音檔的上傳、保存和播放
- **音頻轉發**：將接收到的 RTP 音頻轉發給 WebSocket 客戶端

## 系統架構

### 1. 多線程架構圖

```
主線程 (WebSocket事件循環)
├── SIP線程 (sip_call_thread)
│   ├── RTP接收線程 (receive_rtp_thread) [高優先級]
│   └── RTP發送子進程 (fork創建) [低優先級]
└── 音頻處理線程 (rtp_audio_thread)
```

### 2. 網絡通信層次

```
應用層: WebSocket (端口8080) + SIP (端口5060)
傳輸層: TCP (WebSocket) + UDP (SIP + RTP)
RTP通信: 本地32000 (接收) ↔ 對方動態端口 (發送)
```

### 3. 數據流向圖

```
WebSocket客戶端 ←→ WebSocket服務器 ←→ SIP服務器
                        ↓
                   RTP接收線程 (32000)
                        ↓
                   音頻數據處理
                        ↓
                ├── WAV文件保存
                └── WebSocket轉發
                        
WAV檔案 → RTP發送子進程 → 對方RTP端口
```

## RTP 端口分配機制詳解

### 1. 端口配置策略

**本地端口（相對固定）：**
```c
#define LOCAL_RTP_PORT 32000        // 接收端口
#define LOCAL_RTP_SEND_PORT 32001   // 發送端口（備用）
```

**對方端口（動態協商）：**
- 通過 SIP SDP 協商獲得
- 在 200 OK 回應中解析 `m=audio` 行
- 存儲在 `session.remote_rtp_port` 中

### 2. SIP 協商過程中的端口解析

#### 第一步：發送 INVITE 時宣告我方端口
```c
// 在 SDP 中宣告我們監聽的端口
snprintf(sdp, BUF_SIZE,
    "v=0\r\n"
    "o=- 0 0 IN IP4 " LOCAL_IP "\r\n"
    "s=Custom SIP Client\r\n"
    "c=IN IP4 " LOCAL_IP "\r\n"
    "t=0 0\r\n"
    "m=audio %d RTP/AVP 0 8 101\r\n"  // 宣告監聽 32000
    // ... 其他 SDP 內容
    LOCAL_RTP_PORT  // 32000
);
```

#### 第二步：解析對方回應中的 RTP 端口
```c
// 收到 200 OK 回應時，解析對方的 SDP
if (status_code == 200) {
    const char *m_line = strstr(sdp_start, "m=audio ");
    if (m_line) {
        sscanf(m_line, "m=audio %d", &session->remote_rtp_port);
        log_with_timestamp("解析到對方 RTP 端口: %d\n", session->remote_rtp_port);
    }
}
```

### 3. 實際網絡通信流程

```
步驟1: 我方 → SIP服務器
   INVITE sip:0938220136@192.168.1.170
   SDP: m=audio 32000 RTP/AVP 0 8 101

步驟2: SIP服務器 → 我方  
   200 OK
   SDP: m=audio 15432 RTP/AVP 0 101    ← 動態分配的端口

步驟3: RTP 通信建立
   我方監聽: 192.168.157.126:32000     ← 固定
   對方監聽: 192.168.1.170:15432       ← 動態解析
   
步驟4: 音頻數據流
   我方發送 → 192.168.1.170:15432
   我方接收 ← 192.168.1.170:任意端口
```

## RTP 發送和接收線程互動機制

### 1. RTP 接收線程 (receive_rtp_thread)

#### 生命週期和職責
```c
void* receive_rtp_thread(void *arg) {
    // 1. 設置高優先級，確保及時接收
    struct sched_param param;
    param.sched_priority = 10;  // 高優先級
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    
    // 2. 設置 socket 超時，確保可以響應停止信號
    struct timeval tv = {1, 0};  // 1秒超時
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // 3. 持續接收循環
    while (running) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
                        (struct sockaddr *)&sender_addr, &sender_len);
        
        if (n > 0) {
            process_rtp_packet(buffer, n);  // 處理 RTP 包
        }
    }
}
```

#### RTP 包處理流程
```c
// 接收到 RTP 包後的三重處理
if (n > 0) {
    rtp_hdr = (rtp_header_t *)buffer;           // 1. 解析 RTP 頭部
    payload = buffer + sizeof(rtp_header_t);     // 2. 提取音頻負載
    
    // 3a. 統計更新
    received_packet_count++;
    total_bytes_received += payload_size;
    
    // 3b. 回調處理（轉發到 WebSocket）
    if (global_rtp_callback) {
        global_rtp_callback((unsigned char*)buffer, n);
    }
    
    // 3c. WAV 文件保存
    if (output_file && payload_size > 0) {
        fwrite(payload, 1, payload_size, output_file);
        fflush(output_file);
    }
}
```

### 2. RTP 發送子進程

#### 進程分離策略
```c
// 使用 fork() 創建子進程，避免線程競爭
pid_t audio_pid = fork();

if (audio_pid == 0) {
    // 子進程：專門處理 RTP 發送
    send_audio_data(shared_rtp_sockfd, wav_file, dest_addr);
    exit(0);  // 子進程結束
} else if (audio_pid > 0) {
    // 父進程：繼續接收 RTP
    waitpid(audio_pid, &status, 0);  // 等待子進程完成
}
```

#### 音頻發送流程
```c
// 子進程中的音頻發送邏輯
while ((bytes_read = fread(payload, 1, 160, wav_fp)) > 0) {
    // 1. 構造 RTP 頭部
    init_rtp_header(rtp_hdr, 0, seq_num, timestamp, ssrc);
    
    // 2. 組裝 RTP 包
    int packet_size = sizeof(rtp_header_t) + bytes_read;
    
    // 3. 發送到對方 RTP 端口
    sendto(rtp_sockfd, rtp_packet, packet_size, 0,
           (struct sockaddr *)dest_addr, sizeof(*dest_addr));
    
    // 4. 更新序列號和時間戳
    seq_num++;
    timestamp += bytes_read;
    
    // 5. 維持 G.711 標準間隔（20ms）
    usleep(20000);
}
```

### 3. Socket 共享機制

#### Socket 創建和共享
```c
// 在 start_rtp_receiver() 中創建全局 socket
static int rtp_sockfd = -1;
rtp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);

// 綁定到本地接收端口
struct sockaddr_in local_addr;
local_addr.sin_port = htons(LOCAL_RTP_PORT);  // 32000
bind(rtp_sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr));

// 通過 get_rtp_sockfd() 共享給發送進程
int get_rtp_sockfd() {
    return rtp_sockfd;  // 返回全局 socket
}
```

#### 雙向通信實現
```
同一個 UDP Socket (rtp_sockfd)
├── 接收線程：recvfrom() 監聽端口 32000
└── 發送子進程：sendto() 發送到對方端口

優勢：
- 真正的全雙工通信
- 避免端口衝突
- 簡化網絡配置
```

### 4. 線程間協調機制

#### 優先級設置
```c
// 接收線程：高優先級（確保及時接收）
param.sched_priority = 10;
pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);

// 發送線程：低優先級（讓出資源給接收）
param.sched_priority = 1;
pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
```

#### 同步機制
```c
// 全局狀態變量（volatile 確保線程可見性）
static volatile int running = 0;           // 接收線程運行標誌
static volatile int sip_call_active = 0;   // 通話活躍標誌
static volatile int rtp_packets_received = 0;  // 統計計數

// 回調機制
typedef void (*rtp_data_callback_t)(const unsigned char *rtp_data, size_t data_size);
static rtp_data_callback_t global_rtp_callback = NULL;
```

## 編譯和部署

### 1. 系統需求

#### 依賴庫
```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install libwebsockets-dev libssl-dev build-essential

# CentOS/RHEL
sudo yum install libwebsockets-devel openssl-devel gcc make
```

#### 編譯器要求
- GCC 4.8+ 或 Clang 3.5+
- 支持 C99 標準
- pthread 支持

### 2. 編譯方法

#### 使用 Makefile
```bash
cd sip
make -f Makefile_audio ws_audio_server
```

#### 手動編譯
```bash
gcc -Wall -I. -o ws_audio_server ws_audio_server.c \
    lib/sip_client.c lib/sip_call.c lib/sip_message.c lib/rtp.c \
    -lpthread -lwebsockets -lssl -lcrypto -lm
```

### 3. 配置文件

#### 修改 SIP 配置
編輯 `lib/sip_client.h`：
```c
#define SIP_SERVER "192.168.1.170"    // SIP 服務器 IP
#define LOCAL_IP "192.168.157.126"    // 本機 IP
#define USERNAME "voip"               // SIP 用戶名
#define PASSWORD "qwER12#$"           // SIP 密碼
#define CALLER "0921367101"           // 主叫號碼
```

#### 網絡配置
- 確保防火牆開放端口：8080 (WebSocket)、5060 (SIP)、32000-32001 (RTP)
- 如果在 NAT 環境，需要配置端口轉發
- 確保與 SIP 服務器的網絡連通性

## 使用方法

### 1. 啟動服務器

#### 方法一：直接運行
```bash
./ws_audio_server
```

#### 方法二：使用 Makefile（推薦）
```bash
make -f Makefile_audio audio-server
```

#### 啟動日誌示例
```
[2024-01-15 10:30:25] WebSocket 音頻服務器啟動
[2024-01-15 10:30:25] 創建上傳目錄: uploaded_wavs
[2024-01-15 10:30:25] WebSocket 音頻服務器監聽所有網路介面上的端口 8080
[2024-01-15 10:30:25] 上傳目錄: uploaded_wavs
```

### 2. WebSocket 客戶端交互

#### 連接到服務器
```javascript
const ws = new WebSocket('ws://192.168.157.126:8080');
```

#### 支持的消息格式

**撥打電話：**
```
CALL:0938220136
```

**掛斷電話：**
```
HANGUP
```

**上傳 WAV 檔案：**
```
WAV_UPLOAD:filename.wav:base64_encoded_data
```

**播放 WAV 檔案：**
```
PLAY_WAV:filename.wav
```

#### 服務器回應格式
```
WAV_ACK:檔案上傳成功 (12345 字節)
WAV_ACK:開始播放檔案 filename.wav
RTP:hexadecimal_rtp_data
```

### 3. 完整使用流程

```
1. 啟動服務器
   ./ws_audio_server

2. 客戶端連接
   WebSocket 連接到 ws://server_ip:8080

3. 撥打電話
   發送: CALL:目標號碼

4. 等待通話建立
   服務器日誌顯示 "SIP 呼叫成功建立"

5. 上傳音檔
   發送: WAV_UPLOAD:test.wav:base64_data

6. 播放音檔
   發送: PLAY_WAV:test.wav

7. 接收音頻
   客戶端接收 RTP: 消息

8. 結束通話
   發送: HANGUP 或等待超時
```

## 調試和監控

### 1. 日誌系統

#### 日誌級別
- **INFO**：正常操作信息
- **WARNING**：警告信息
- **ERROR**：錯誤信息

#### 關鍵日誌示例
```
[2024-01-15 10:30:30] 收到打電話請求，目標號碼: 0938220136
[2024-01-15 10:30:31] SIP 呼叫成功建立
[2024-01-15 10:30:32] **正確配置**: 我方監聽端口 32000，對方監聽端口 15432
[2024-01-15 10:30:32] 啟動 RTP 接收器...
[2024-01-15 10:30:33] 接收RTP包 #1：來源=192.168.1.170:15432, 序號=12345, 時間戳=160000, 大小=160
```

### 2. 網絡診斷

#### 抓包分析
```bash
# 監控 SIP 信令
sudo tcpdump -i any -n port 5060 -w sip.pcap

# 監控 RTP 音頻
sudo tcpdump -i any -n port 32000 -w rtp.pcap

# 監控 WebSocket
sudo tcpdump -i any -n port 8080 -w websocket.pcap
```

#### 端口檢查
```bash
# 檢查端口占用
netstat -tulpn | grep -E "(8080|5060|32000)"

# 測試端口連通性
telnet 192.168.1.170 5060
nc -u 192.168.157.126 32000
```

### 3. 性能監控

#### 系統資源
```bash
# CPU 使用率
top -p $(pgrep ws_audio_server)

# 記憶體使用
ps aux | grep ws_audio_server

# 網絡統計
ss -u -n | grep 32000
```

#### 音頻質量指標
- **RTP 包接收率**：正常應 > 95%
- **音頻延遲**：< 150ms
- **丟包率**：< 1%

## 故障排除

### 1. 常見問題

#### WebSocket 連接失敗
```
錯誤：Connection refused
解決：檢查防火牆設置，確保端口 8080 開放
命令：sudo ufw allow 8080
```

#### SIP 註冊失敗
```
錯誤：401 Unauthorized
解決：檢查 SIP 用戶名和密碼配置
位置：lib/sip_client.h 中的 USERNAME 和 PASSWORD
```

#### RTP 音頻無聲
```
錯誤：接收到 RTP 包但無音頻
解決：檢查 WAV 文件格式，確保為 G.711 μ-law
命令：file received_from_server.wav
```

#### 端口衝突
```
錯誤：bind: Address already in use
解決：檢查端口占用，終止衝突進程
命令：sudo lsof -i :32000
```

### 2. 調試技巧

#### 啟用詳細日誌
```c
// 在編譯時添加調試標誌
gcc -DDEBUG -Wall -I. -o ws_audio_server ...
```

#### 檢查 RTP 數據
```bash
# 檢查生成的原始 RTP 數據
hexdump -C rtp_raw_data.bin | head -20

# 檢查 WAV 文件頭
hexdump -C received_from_server.wav | head -5
```

#### 驗證音頻質量
```bash
# 播放接收到的音頻
aplay received_from_server.wav

# 轉換格式進行測試
sox received_from_server.wav -t wav test_output.wav
```



## 性能優化

### 1. 記憶體優化
- 使用記憶體池減少頻繁分配
- 優化緩衝區大小
- 及時釋放不需要的資源

### 2. 網絡優化
- 調整 socket 緩衝區大小
- 使用 epoll 替代 select
- 實現 RTP 包聚合

### 3. 線程優化
- 使用線程池
- 優化線程優先級
- 減少上下文切換

## 安全考慮

### 1. 網絡安全
- 實施 IP 白名單
- 添加速率限制
- 使用 TLS 加密 WebSocket

### 2. 音頻安全
- 實施 SRTP 加密
- 添加音頻水印
- 檢測異常音頻模式

### 3. 系統安全
- 以非 root 用戶運行
- 限制文件系統訪問
- 實施資源限制





**最後更新：2025年6月23日**
