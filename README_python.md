# WebSocket SIP 音頻系統 - Python 客戶端使用指南

## 概述

本項目提供了一個完整的 WebSocket SIP 音頻系統，包含 C 語言服務器端和 Python 客戶端。系統支持 SIP 通話、音頻文件上傳/播放、RTP 數據接收和處理等功能。

## 系統架構

```
┌─────────────────┐    WebSocket    ┌─────────────────┐    SIP/RTP    ┌─────────────────┐
│  Python 客戶端  │ ──────────────→ │   C 服務器端    │ ──────────────→ │   SIP 服務器    │
│                 │ ←────────────── │                 │ ←────────────── │                 │
└─────────────────┘                 └─────────────────┘                 └─────────────────┘
```

## Python 版本要求

- **Python 3.7+** （支持 asyncio）
- **推薦 Python 3.8+** （更好的 asyncio 支持）

## 依賴包安裝

### 方法1：使用 pip 安裝

```bash
# 安裝必要的依賴包
pip install websockets asyncio
```

### 方法2：創建虛擬環境（推薦）

```bash
# 創建虛擬環境
python3 -m venv venv

# 激活虛擬環境
source venv/bin/activate  # Linux/Mac
# 或
venv\Scripts\activate     # Windows

# 安裝依賴包
pip install websockets asyncio
```

### 方法3：使用 requirements.txt

創建 `requirements.txt` 文件：

```text
websockets>=10.0
asyncio-mqtt>=0.11.0
```

然後安裝：

```bash
pip install -r requirements.txt
```



## 快速開始


### 運行 Python 客戶端

#### 基本使用

```bash
python3 ws_audio_client.py
```

#### 指定服務器地址

```bash
python3 ws_audio_client.py --server 192.168.1.100 --port 8080
```

#### 命令行參數

```bash
python3 ws_audio_client.py --help
```

## 詳細使用說明

### 客戶端功能菜單

連接成功後，客戶端會顯示以下菜單：

```
=== WebSocket SIP 音頻 Demo 客戶端 (Python版) ===
1. 撥打電話 (預設號碼: 0938220136)
2. 撥打電話 (自定義號碼)
3. 掛斷電話
4. 顯示統計
5. 上傳 WAV 檔案
6. 播放指定 WAV 檔案
7. 將接收的 RTP 封包保存為 WAV 文件
8. 退出
請選擇 (1-8):
```

### 功能詳解

#### 1. 撥打電話（選項 1 & 2）

- **選項 1**：撥打預設號碼 `0938220136`
- **選項 2**：撥打自定義號碼

```bash
請選擇 (1-8): 2
請輸入電話號碼: 0912345678
```

#### 2. 掛斷電話（選項 3）

發送掛斷信號給服務器，結束當前通話。

#### 3. 顯示統計（選項 4）

顯示接收的 RTP 封包數量和緩存的音頻數據大小。

```
統計信息：已接收 1250 個 RTP 封包，已緩存 45678 字節音頻數據
```

#### 4. 上傳 WAV 檔案（選項 5）

上傳本地 WAV 文件到服務器：

```bash
請選擇 (1-8): 5
請輸入要上傳的 WAV 檔案路徑: /path/to/your/audio.wav
```

**支持的格式**：
- WAV 格式
- 最大文件大小：1MB
- 推薦：8000Hz, 單聲道, μ-law 編碼

#### 5. 播放指定 WAV 檔案（選項 6）

播放服務器上已上傳的 WAV 文件：

```bash
請選擇 (1-8): 6
請輸入要播放的 WAV 檔案名稱: audio.wav
```

#### 6. 保存 RTP 數據為 WAV（選項 7）

將接收到的 RTP 數據保存為本地 WAV 文件：

```bash
請選擇 (1-8): 7
正在將接收到的 RTP 數據保存為 WAV 文件: rtp_capture_20231215_143022.wav
WAV 文件保存成功！
```

## 完整 Python 源碼

<details>
<summary>點擊展開查看完整的 Python 源碼 (ws_audio_client.py)</summary>

```python
#!/usr/bin/env python3
"""
WebSocket SIP 音頻客戶端 - Python 版本
功能包括撥打電話、上傳WAV文件、播放音頻、接收RTP數據等
"""

#!/usr/bin/env python3
"""
WebSocket SIP 音頻客戶端 - Python 版本
功能包括撥打電話、上傳WAV文件、播放音頻、接收RTP數據等
"""

import asyncio
import websockets
import json
import base64
import struct
import wave
import time
import os
import sys
import argparse
from datetime import datetime
from pathlib import Path
import threading
import signal

# 配置常數
DEFAULT_SERVER_ADDRESS = "0.0.0.0"
DEFAULT_SERVER_PORT = 8080
MAX_FILE_SIZE = 1024 * 1024  # 1MB
WAV_HEADER_SIZE = 64

class WebSocketAudioClient:
    def __init__(self, server_address=DEFAULT_SERVER_ADDRESS, server_port=DEFAULT_SERVER_PORT):
        self.server_address = server_address
        self.server_port = server_port
        self.websocket = None
        self.connected = False
        self.force_exit = False
        self.rtp_packet_count = 0
        self.rtp_buffer = bytearray()
        
    def log_with_timestamp(self, message):
        """帶時間戳的日誌輸出"""
        timestamp = datetime.now().strftime("[%Y-%m-%d %H:%M:%S]")
        print(f"{timestamp} {message}")
        
    def generate_wav_header(self, data_size):
        """生成 WAV 文件頭 (PCMU/G.711 μ-law 格式, 8000Hz, 單聲道)"""
        header = bytearray(WAV_HEADER_SIZE)
        
        # RIFF 頭部
        header[0:4] = b"RIFF"
        filesize = data_size + WAV_HEADER_SIZE - 8
        header[4:8] = struct.pack("<I", filesize)
        
        # WAVE 標識
        header[8:12] = b"WAVE"
        
        # fmt 區塊
        header[12:16] = b"fmt "
        header[16:20] = struct.pack("<I", 18)  # fmt 區塊大小
        header[20:22] = struct.pack("<H", 7)   # 音頻格式: 7 = MULAW
        header[22:24] = struct.pack("<H", 1)   # 通道數: 1 = 單聲道
        header[24:28] = struct.pack("<I", 8000)  # 採樣率: 8000Hz
        header[28:32] = struct.pack("<I", 8000)  # 字節率: 8000 字節/秒
        header[32:34] = struct.pack("<H", 1)     # 數據塊對齊: 1 字節
        header[34:36] = struct.pack("<H", 8)     # 位寬: 8 位
        header[36:38] = struct.pack("<H", 0)     # 額外參數大小: 0
        
        # fact 區塊
        header[38:42] = b"fact"
        header[42:46] = struct.pack("<I", 4)     # fact 區塊大小
        header[46:50] = struct.pack("<I", data_size)  # 樣本數
        
        # data 區塊
        header[50:54] = b"data"
        header[54:58] = struct.pack("<I", data_size)  # data 區塊大小
        
        return header
        
    def hex_to_bin(self, hex_str):
        """將十六進制字符串轉換為二進制數據"""
        return bytes.fromhex(hex_str)
        
    def save_rtp_to_wav(self, filename):
        """將緩存的 RTP 數據保存為 WAV 文件"""
        if not self.rtp_buffer:
            self.log_with_timestamp("錯誤: 沒有 RTP 數據可保存")
            return False
            
        try:
            # 生成 WAV 頭部
            header = self.generate_wav_header(len(self.rtp_buffer))
            
            # 寫入文件
            with open(filename, 'wb') as f:
                f.write(header)
                f.write(self.rtp_buffer)
                
            self.log_with_timestamp(f"成功保存 {len(self.rtp_buffer)} 字節的 RTP 數據到 {filename}")
            return True
        except Exception as e:
            self.log_with_timestamp(f"保存 WAV 文件失敗: {e}")
            return False
            
    def prepare_wav_upload(self, filename):
        """讀取 WAV 檔案並準備上傳"""
        try:
            # 檢查檔案是否存在
            if not os.path.exists(filename):
                self.log_with_timestamp(f"錯誤: 檔案 {filename} 不存在")
                return None
                
            # 檢查檔案大小
            file_size = os.path.getsize(filename)
            if file_size > MAX_FILE_SIZE:
                self.log_with_timestamp(f"錯誤: 檔案 {filename} 太大 ({file_size} 字節)，最大允許 {MAX_FILE_SIZE} 字節")
                return None
                
            # 讀取檔案
            with open(filename, 'rb') as f:
                file_data = f.read()
                
            self.log_with_timestamp(f"準備上傳 WAV 檔案: {filename} ({len(file_data)} 字節)")
            return file_data
        except Exception as e:
            self.log_with_timestamp(f"讀取檔案失敗: {e}")
            return None
            
    async def send_message(self, message):
        """發送消息到服務器"""
        if self.websocket and self.connected:
            try:
                await self.websocket.send(message)
                self.log_with_timestamp(f"發送消息: {message}")
            except Exception as e:
                self.log_with_timestamp(f"發送消息失敗: {e}")
        else:
            self.log_with_timestamp("WebSocket 未連接，無法發送消息")
            
    async def upload_wav_file(self, filename):
        """上傳 WAV 檔案到服務器"""
        if not self.websocket or not self.connected:
            self.log_with_timestamp("WebSocket 未連接，無法上傳檔案")
            return False
            
        # 準備檔案數據
        file_data = self.prepare_wav_upload(filename)
        if file_data is None:
            return False
            
        # Base64 編碼
        encoded_data = base64.b64encode(file_data).decode('utf-8')
        
        # 準備上傳消息
        upload_msg = f"WAV_UPLOAD:{os.path.basename(filename)}:{encoded_data}"
        
        self.log_with_timestamp(f"開始上傳 WAV 檔案 {filename} (編碼後大小: {len(encoded_data)} 字節)")
        
        # 發送上傳消息
        await self.send_message(upload_msg)
        return True
        
    async def handle_server_message(self, message):
        """處理服務器消息"""
        try:
            if message.startswith("RTP:"):
                self.rtp_packet_count += 1
                
                # 解析十六進制數據
                hex_data = message[4:]
                if hex_data:
                    try:
                        bin_data = self.hex_to_bin(hex_data)
                        rtp_data_len = len(bin_data)
                        
                        # 只在前幾個封包或每100個封包顯示詳細資訊
                        if self.rtp_packet_count <= 5 or self.rtp_packet_count % 100 == 0:
                            self.log_with_timestamp(f"收到 RTP 封包 #{self.rtp_packet_count}，原始大小: {rtp_data_len} 字節")
                        
                        # 只在前3個封包顯示十六進制數據
                        if self.rtp_packet_count <= 3:
                            hex_sample = hex_data[:32] + ("..." if len(hex_data) > 32 else "")
                            self.log_with_timestamp(f"RTP 數據樣本 (前16字節): {hex_sample}")
                            
                            # 提取 RTP 有效載荷 (跳過 RTP 頭部 12 字節)
                            if len(bin_data) > 12:
                                payload = bin_data[12:]  # 跳過 RTP 頭部
                                self.rtp_buffer.extend(payload)
                        
                        # 每 100 個封包顯示一次統計
                        if self.rtp_packet_count % 100 == 0:
                            self.log_with_timestamp(f"已接收 {self.rtp_packet_count} 個 RTP 封包，已緩存 {len(self.rtp_buffer)} 字節音頻數據")
                            
                    except ValueError as e:
                        self.log_with_timestamp(f"解析 RTP 數據失敗: {e}")
                        
            elif message.startswith("WAV_ACK:"):
                ack_msg = message[8:]
                self.log_with_timestamp(f"收到 WAV 上傳確認: {ack_msg}")
            else:
                self.log_with_timestamp(f"收到服務器消息: {message}")
                
        except Exception as e:
            self.log_with_timestamp(f"處理服務器消息失敗: {e}")
            
    async def connect_to_server(self):
        """連接到 WebSocket 服務器"""
        uri = f"ws://{self.server_address}:{self.server_port}"
        self.log_with_timestamp(f"正在連接到 {uri}")
        
        try:
            self.websocket = await websockets.connect(
                uri,
                subprotocols=["sip-audio-protocol"],
                max_size=1024*1024,  # 1MB max message size
                ping_interval=20,
                ping_timeout=10
            )
            self.connected = True
            self.log_with_timestamp("WebSocket 客戶端連接建立")
            
            # 監聽服務器消息
            async for message in self.websocket:
                if self.force_exit:
                    break
                await self.handle_server_message(message)
                
        except websockets.exceptions.ConnectionClosed:
            self.log_with_timestamp("WebSocket 連接關閉")
        except Exception as e:
            self.log_with_timestamp(f"WebSocket 連接錯誤: {e}")
        finally:
            self.connected = False
            
    def show_menu(self):
        """顯示用戶菜單"""
        print("\n=== WebSocket SIP 音頻 Demo 客戶端 (Python版) ===")
        print("1. 撥打電話 (預設號碼: 0938220136)")
        print("2. 撥打電話 (自定義號碼)")
        print("3. 掛斷電話")
        print("4. 顯示統計")
        print("5. 上傳 WAV 檔案")
        print("6. 播放指定 WAV 檔案")
        print("7. 將接收的 RTP 封包保存為 WAV 文件")
        print("8. 退出")
        print("請選擇 (1-8): ", end="", flush=True)
        
    async def handle_user_input(self):
        """處理用戶輸入"""
        # 等待連接建立
        while not self.connected and not self.force_exit:
            await asyncio.sleep(0.1)
            
        if self.force_exit:
            return
            
        while not self.force_exit and self.connected:
            try:
                self.show_menu()
                choice = await asyncio.get_event_loop().run_in_executor(None, input)
                choice = choice.strip()
                
                if choice == "1":
                    await self.send_message("CALL:0938220136")
                    self.log_with_timestamp("發起撥打預設號碼的請求")
                    
                elif choice == "2":
                    print("請輸入電話號碼: ", end="", flush=True)
                    phone_number = await asyncio.get_event_loop().run_in_executor(None, input)
                    phone_number = phone_number.strip()
                    
                    if phone_number.isdigit() and len(phone_number) >= 3:
                        await self.send_message(f"CALL:{phone_number}")
                        self.log_with_timestamp(f"發起撥打 {phone_number} 的請求")
                    else:
                        self.log_with_timestamp("無效的電話號碼格式，請使用純數字")
                        
                elif choice == "3":
                    await self.send_message("HANGUP")
                    self.log_with_timestamp("發送掛斷請求")
                    
                elif choice == "4":
                    self.log_with_timestamp(f"統計信息：已接收 {self.rtp_packet_count} 個 RTP 封包，已緩存 {len(self.rtp_buffer)} 字節音頻數據")
                    
                elif choice == "5":
                    print("請輸入要上傳的 WAV 檔案路徑: ", end="", flush=True)
                    filename = await asyncio.get_event_loop().run_in_executor(None, input)
                    filename = filename.strip()
                    
                    if filename:
                        await self.upload_wav_file(filename)
                    else:
                        self.log_with_timestamp("無效的檔案路徑")
                        
                elif choice == "6":
                    print("請輸入要播放的 WAV 檔案名稱: ", end="", flush=True)
                    filename = await asyncio.get_event_loop().run_in_executor(None, input)
                    filename = filename.strip()
                    
                    if filename:
                        await self.send_message(f"PLAY_WAV:{filename}")
                        self.log_with_timestamp(f"發送播放 WAV 檔案請求: {filename}")
                    else:
                        self.log_with_timestamp("無效的檔案名稱")
                        
                elif choice == "7":
                    # 生成帶時間戳的文件名
                    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                    wav_filename = f"rtp_capture_{timestamp}.wav"
                    
                    self.log_with_timestamp(f"正在將接收到的 RTP 數據保存為 WAV 文件: {wav_filename}")
                    if self.save_rtp_to_wav(wav_filename):
                        self.log_with_timestamp("WAV 文件保存成功！")
                    else:
                        self.log_with_timestamp("保存 WAV 文件失敗")
                        
                elif choice == "8":
                    self.log_with_timestamp("用戶選擇退出")
                    self.force_exit = True
                    return
                    
                else:
                    print("無效選擇，請重新輸入")
                    
                # 給 WebSocket 處理一些時間
                await asyncio.sleep(0.1)
                
            except KeyboardInterrupt:
                self.log_with_timestamp("收到中斷信號，正在關閉客戶端...")
                self.force_exit = True
                return
            except Exception as e:
                self.log_with_timestamp(f"處理用戶輸入錯誤: {e}")
                
    async def run(self):
        """運行客戶端"""
        self.log_with_timestamp("WebSocket SIP 音頻 Demo 客戶端啟動")
        self.log_with_timestamp(f"服務器地址: {self.server_address}:{self.server_port}")
        
        try:
            # 先啟動連接任務
            connection_task = asyncio.create_task(self.connect_to_server())
            
            # 等待一下讓連接建立
            await asyncio.sleep(0.5)
            
            # 然後啟動用戶輸入處理
            input_task = asyncio.create_task(self.handle_user_input())
            
            # 等待任一任務完成
            done, pending = await asyncio.wait(
                [connection_task, input_task], 
                return_when=asyncio.FIRST_COMPLETED
            )
            
            # 取消未完成的任務
            for task in pending:
                task.cancel()
                
        except KeyboardInterrupt:
            self.log_with_timestamp("收到中斷信號，正在關閉客戶端...")
        finally:
            self.force_exit = True
            if self.websocket:
                await self.websocket.close()
            self.log_with_timestamp("WebSocket 音頻客戶端已關閉")
            self.log_with_timestamp(f"總計接收了 {self.rtp_packet_count} 個 RTP 封包")

def signal_handler(signum, frame):
    """信號處理器"""
    print("\n收到中斷信號，正在關閉...")
    sys.exit(0)

def main():
    """主函數"""
    parser = argparse.ArgumentParser(description="WebSocket SIP 音頻客戶端 - Python版")
    parser.add_argument("-s", "--server", default=DEFAULT_SERVER_ADDRESS,
                       help=f"服務器地址 (默認: {DEFAULT_SERVER_ADDRESS})")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_SERVER_PORT,
                       help=f"服務器端口 (默認: {DEFAULT_SERVER_PORT})")
    
    args = parser.parse_args()
    
    # 設置信號處理器
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # 創建並運行客戶端
    client = WebSocketAudioClient(args.server, args.port)
    
    try:
        asyncio.run(client.run())
    except KeyboardInterrupt:
        print("客戶端已關閉")

if __name__ == "__main__":
    main() 
```

</details>

## 協議說明

### WebSocket 消息格式

#### 客戶端發送的消息

| 消息類型 | 格式 | 說明 |
|---------|------|------|
| 撥打電話 | `CALL:<號碼>` | 發起 SIP 通話 |
| 掛斷電話 | `HANGUP` | 結束當前通話 |
| 上傳文件 | `WAV_UPLOAD:<文件名>:<Base64數據>` | 上傳 WAV 文件 |
| 播放文件 | `PLAY_WAV:<文件名>` | 播放指定 WAV 文件 |

#### 服務器發送的消息

| 消息類型 | 格式 | 說明 |
|---------|------|------|
| RTP 數據 | `RTP:<十六進制數據>` | 實時音頻數據 |
| 上傳確認 | `WAV_ACK:<確認信息>` | 文件上傳成功確認 |
| 狀態消息 | `<狀態信息>` | 系統狀態更新 |







**最後更新**：2025年6月7日 