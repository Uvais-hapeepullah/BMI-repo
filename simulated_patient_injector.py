"""
PulseNet-Mesh HIL Simulation — Backend Server
=============================================
Runs a FastAPI server that serves the UI and bridges WebSocket messages
to the Gateway ESP32 over serial.

Usage
-----
  python simulated_patient_injector.py [--port COM3] [--host 127.0.0.1] [--web-port 8000]
"""

import argparse
import json
import sys
import uvicorn
import serial
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from Crypto.Cipher import AES

AES_KEY = b"PulseNetMeshKey!"
BAD_AES_KEY = b"WrongKey1234567!"
BLOCK_SIZE = 16

app = FastAPI()
ser = None

def pkcs7_pad(data: bytes) -> bytes:
    pad_len = BLOCK_SIZE - (len(data) % BLOCK_SIZE)
    return data + bytes([pad_len] * pad_len)

def aes_ecb_encrypt(plaintext: str, bad_key: bool = False) -> str:
    key = BAD_AES_KEY if bad_key else AES_KEY
    cipher = AES.new(key, AES.MODE_ECB)
    padded = pkcs7_pad(plaintext.encode("utf-8"))
    return cipher.encrypt(padded).hex().upper()

@app.get("/")
def get_ui():
    try:
        with open("index.html", "r", encoding="utf-8") as f:
            return HTMLResponse(f.read())
    except FileNotFoundError:
        return HTMLResponse("<h1>index.html not found in current directory.</h1>", status_code=404)

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_json()
            if data.get("type") == "inject" and ser is not None and ser.is_open:
                payload = data.get("payload", {})
                faults = data.get("faults", {})

                if faults.get("drop"):
                    continue

                json_str = json.dumps(payload, separators=(",", ":"))
                hex_str = aes_ecb_encrypt(json_str, bad_key=faults.get("aesErr"))

                if faults.get("corruptHex"):
                    # Mutate some characters to simulate noise/burst/crc
                    l = list(hex_str)
                    if len(l) > 4:
                        l[2] = 'F'
                        l[3] = 'F'
                    hex_str = "".join(l)

                if faults.get("truncate"):
                    hex_str = hex_str[:max(8, int(len(hex_str) * 0.3))]

                line = hex_str + "\n"
                ser.write(line.encode("ascii"))
                ser.flush()
                print(f"[TX] Node {payload.get('node_id')} | seq={payload.get('seq')} | len={len(hex_str)}")
    except WebSocketDisconnect:
        pass
    except Exception as e:
        print(f"WS error: {e}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM3", help="Serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--web-port", type=int, default=8000)
    args = parser.parse_args()

    global ser
    try:
        print(f"[PulseNet] Opening serial port {args.port} @ {args.baud} ...")
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        print(f"[ERROR] Cannot open {args.port}: {exc}", file=sys.stderr)
        print("WARNING: Running without serial connection. UI will work but hardware won't receive data.")

    print(f"[PulseNet] Starting web server on http://{args.host}:{args.web_port}")
    uvicorn.run(app, host=args.host, port=args.web_port, log_level="warning")

if __name__ == "__main__":
    main()
