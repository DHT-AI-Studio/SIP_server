# WebSocket SIP 通話客戶端

## 簡介

這是一個 Python 實現的 WebSocket 客戶端，用於連接到 SIP WebSocket 服務器，發起電話呼叫並顯示接收到的 RTP 數據包內容。此客戶端可以解析 RTP 頭部並顯示詳細的 RTP 數據包信息。

## 功能特點

- 自動連接到 WebSocket 服務器
- 發起 SIP 呼叫請求
- 解析並顯示每個 RTP 數據包的詳細信息：
  - RTP 頭部字段（版本、填充、擴展、CSRC 計數）
  - 標記位、負載類型、序列號
  - 時間戳和 SSRC
  - 負載大小和前 16 個字節的負載數據（十六進制格式）
- 每 50 個數據包顯示統計信息
- 可以通過 Ctrl+C 優雅退出

## 依賴庫

- Python 3.6+
- websockets 庫

## 安裝依賴

```bash
pip install websockets
```

## 使用方法

1. 確保 WebSocket 服務器正在運行（`ws_demo_server` 執行檔）

2. 運行 Python 客戶端：
   ```bash
   python ws_client.py [選項]
   ```

3. 可用命令行選項：
   - `--server ADDRESS`：服務器地址（默認：localhost）
   - `--port PORT`：服務器端口（默認：8080）
   - `--number PHONE`：要撥打的電話號碼（默認：0938220136）

4. 示例：
   ```bash
   python ws_client.py --server 192.168.1.100 --port 8080 --number 0938220136
   ```

## 工作原理

1. 客戶端建立與服務器的 WebSocket 連接
2. 發送 "CALL:{phone_number}" 消息以發起 SIP 呼叫
3. 然後監聽傳入消息，特別是尋找帶有 "RTP:" 前綴的消息
4. 當收到 RTP 數據包時：
   - 將十六進制表示轉換為二進制
   - 解析 RTP 頭部（前 12 個字節）
   - 提取並顯示負載
   - 打印有關數據包結構的詳細信息

客戶端將繼續接收和顯示 RTP 數據包，直到您按下 Ctrl+C 退出。

## 完整代碼

```python
#!/usr/bin/env python3
import asyncio
import websockets
import binascii
import argparse
import signal
import sys

# Global variables
rtp_packet_count = 0
connected = False
force_exit = False

async def ws_client(server_address, server_port, phone_number):
    global connected, rtp_packet_count, force_exit
    
    uri = f"ws://{server_address}:{server_port}"
    print(f"[INFO] Connecting to {uri}...")
    
    try:
        async with websockets.connect(uri) as websocket:
            print(f"[INFO] Connection established with server")
            connected = True
            
            # Send call request
            call_request = f"CALL:{phone_number}"
            await websocket.send(call_request)
            print(f"[INFO] Call request sent to {phone_number}")
            
            # Receive messages until force_exit
            while not force_exit:
                try:
                    message = await asyncio.wait_for(websocket.recv(), timeout=1.0)
                    
                    # Check if it's an RTP packet
                    if message.startswith("RTP:"):
                        rtp_packet_count += 1
                        hex_data = message[4:]  # Skip "RTP:" prefix
                        
                        # Convert hex to binary
                        try:
                            binary_data = binascii.unhexlify(hex_data)
                            
                            # Extract RTP header (first 12 bytes)
                            if len(binary_data) >= 12:
                                header = binary_data[:12]
                                payload = binary_data[12:]
                                
                                # Parse RTP header
                                version = (header[0] >> 6) & 0x03
                                padding = (header[0] >> 5) & 0x01
                                extension = (header[0] >> 4) & 0x01
                                csrc_count = header[0] & 0x0F
                                marker = (header[1] >> 7) & 0x01
                                payload_type = header[1] & 0x7F
                                seq_num = (header[2] << 8) | header[3]
                                timestamp = int.from_bytes(header[4:8], byteorder='big')
                                ssrc = int.from_bytes(header[8:12], byteorder='big')
                                
                                # Print RTP packet information
                                print(f"\n--- RTP Packet #{rtp_packet_count} ---")
                                print(f"Header: {header.hex()}")
                                print(f"Version: {version}, Padding: {padding}, Extension: {extension}, CSRC Count: {csrc_count}")
                                print(f"Marker: {marker}, Payload Type: {payload_type}, Sequence Number: {seq_num}")
                                print(f"Timestamp: {timestamp}, SSRC: {ssrc}")
                                print(f"Payload Size: {len(payload)} bytes")
                                print(f"Payload (first 16 bytes): {payload[:16].hex()}")
                                
                                # Print statistics every 50 packets
                                if rtp_packet_count % 50 == 0:
                                    print(f"\n[INFO] Received {rtp_packet_count} RTP packets so far")
                            else:
                                print(f"[WARNING] Received incomplete RTP packet (size: {len(binary_data)})")
                        except binascii.Error:
                            print(f"[ERROR] Invalid hex data received: {hex_data[:30]}...")
                    else:
                        # Regular server message
                        print(f"[SERVER] {message}")
                
                except asyncio.TimeoutError:
                    # Timeout is expected, just continue
                    continue
                except websockets.exceptions.ConnectionClosed:
                    print("[INFO] Connection closed by server")
                    break
    
    except Exception as e:
        print(f"[ERROR] {str(e)}")
    finally:
        connected = False
        print("[INFO] Disconnected from server")

def signal_handler(sig, frame):
    global force_exit
    print("\n[INFO] Interrupt received, exiting...")
    force_exit = True

def main():
    # Parse command line arguments
    parser = argparse.ArgumentParser(description="WebSocket SIP Demo Client")
    parser.add_argument("--server", default="localhost", help="Server address (default: localhost)")
    parser.add_argument("--port", type=int, default=8080, help="Server port (default: 8080)")
    parser.add_argument("--number", default="0938220136", help="Phone number to call (default: 0938220136)")
    args = parser.parse_args()
    
    # Register signal handler
    signal.signal(signal.SIGINT, signal_handler)
    
    print("=== WebSocket SIP Demo Python Client ===")
    print(f"Server: {args.server}:{args.port}")
    print(f"Target phone number: {args.number}")
    print("Press Ctrl+C to exit")
    
    # Start client
    try:
        asyncio.run(ws_client(args.server, args.port, args.number))
    except KeyboardInterrupt:
        print("\n[INFO] Program terminated by user")
    
    # Print final statistics
    print(f"\n[SUMMARY] Total RTP packets received: {rtp_packet_count}")

if __name__ == "__main__":
    main()
```

## 輸出示例

```
=== WebSocket SIP Demo Python Client ===
Server: localhost:8080
Target phone number: 0938220136
Press Ctrl+C to exit
[INFO] Connecting to ws://localhost:8080...
[INFO] Connection established with server
[INFO] Call request sent to 0938220136
[SERVER] SIP 呼叫成功建立

--- RTP Packet #1 ---
Header: 80000000000000000000000b
Version: 2, Padding: 0, Extension: 0, CSRC Count: 0
Marker: 1, Payload Type: 0, Sequence Number: 0
Timestamp: 0, SSRC: 11
Payload Size: 160 bytes
Payload (first 16 bytes): ffffffff7f7f7f7f7f7fff7f7f7f7f

--- RTP Packet #2 ---
Header: 80000001000003e00000000b
Version: 2, Padding: 0, Extension: 0, CSRC Count: 0
Marker: 1, Payload Type: 0, Sequence Number: 1
Timestamp: 992, SSRC: 11
Payload Size: 160 bytes
Payload (first 16 bytes): 7f7f7fff7f7f7f7f7fff7f7f7f7fff
```

## 注意事項

- 此客戶端僅支持文本模式的 WebSocket 通訊
- 服務器發送的 RTP 數據必須使用 "RTP:" 前綴並以十六進制格式編碼
- RTP 解析假設標準的 12 字節 RTP 頭部格式 