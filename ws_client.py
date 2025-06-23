#ws_client.py
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