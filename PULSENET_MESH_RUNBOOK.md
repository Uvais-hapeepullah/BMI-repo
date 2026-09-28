# PulseNet-Mesh HIL Simulation — Step-by-Step Runbook

> **What this does:** Your laptop runs a Python script that generates fake patient vitals,
> encrypts them, and feeds them over USB to a **Gateway ESP32**. That ESP32 re-broadcasts
> them wirelessly over **ESP-NOW**. One or more **Relay ESP32s** (on power banks) catch the
> signal, decrypt it, increment a hop counter, and re-broadcast it further — extending the
> network mesh range automatically.

---

## 📦 What You Need

| Item | Quantity | Notes |
|---|---|---|
| ESP32 Dev Board | 2+ | Any variant (ESP32-WROOM, ESP32-S, NodeMCU-32, etc.) |
| USB-A to Micro-USB (or USB-C) cables | 2+ | One per board |
| USB Power Banks | 1+ | For the relay node(s) — no laptop needed |
| This laptop | 1 | Runs the Python injector + powers the Gateway ESP32 |

---

## 🧰 Part 1 — One-Time Software Setup

### Step 1 — Install Arduino IDE 2

1. Download from **https://www.arduino.cc/en/software** (choose the Windows installer).
2. Install it with default settings.

---

### Step 2 — Add the ESP32 Board Package

1. Open Arduino IDE → **File → Preferences**.
2. In the **"Additional boards manager URLs"** field, paste:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Click **OK**.
4. Go to **Tools → Board → Boards Manager…**
5. Search for **`esp32`** → find **"esp32 by Espressif Systems"** → click **Install**.
6. Wait for the download to finish (~200 MB).

---

### Step 3 — Install the ArduinoJson Library

> Required only by `receiver_relay_node.ino`. The gateway doesn't need it.

1. In Arduino IDE → **Sketch → Include Library → Manage Libraries…**
2. Search for **`ArduinoJson`** → find **"ArduinoJson by Benoit Blanchon"**.
3. Install the latest **v6.x or v7.x** version.

---

### Step 4 — Verify Python Dependencies

All packages are already installed. Confirm by running:

```powershell
pip install -r d:\simulator\requirements.txt
```

Expected output: all lines say `Requirement already satisfied`.

---

## 🔌 Part 2 — Flash the Gateway ESP32 (Laptop-Attached)

> This is the board plugged into **your laptop via USB**. It receives hex strings from Python
> and broadcasts them over the air.

### Step 5 — Connect the Gateway ESP32

1. Plug the ESP32 into your laptop with a USB cable.
2. Windows will install drivers automatically (CP2102 or CH340 chip).
   - If it doesn't, download the driver:
     - **CP2102**: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers
     - **CH340**: https://sparks.gogo.co.nz/ch340.html

---

### Step 6 — Find Your COM Port

1. Right-click the **Start menu** → **Device Manager**.
2. Expand **"Ports (COM & LPT)"**.
3. Look for an entry like `Silicon Labs CP210x USB to UART Bridge (COM4)` or `USB-SERIAL CH340 (COM5)`.
4. **Note the COM number** — you'll need it in Step 10.

Or run this in PowerShell to list all active serial ports:
```powershell
[System.IO.Ports.SerialPort]::getportnames()
```

---

### Step 7 — Open the Gateway Firmware

1. In Arduino IDE → **File → Open…**
2. Navigate to `d:\simulator\gateway_node.ino` and open it.

---

### Step 8 — Select the Right Board & Port

1. **Tools → Board → esp32 → "ESP32 Dev Module"**
   *(Or pick your specific variant if you know it)*
2. **Tools → Port → COM_X** *(the port you found in Step 6)*
3. **Tools → Upload Speed → 921600**

---

### Step 9 — Upload the Firmware

1. Click the **→ Upload** button (or press `Ctrl+U`).
2. Wait for `Done uploading.` to appear at the bottom.
3. If you see `Connecting...........____` stuck — hold the **BOOT button** on the ESP32 while it connects, then release.

> **Verify it's working:** Open **Tools → Serial Monitor**, set baud to **115200**.  
> You should see:
> ```
> ╔══════════════════════════════════════╗
> ║  PulseNet-Mesh  —  Gateway Node 0   ║
> ╚══════════════════════════════════════╝
> [GW] Wi-Fi STA  MAC : XX:XX:XX:XX:XX:XX
> [GW] Ready — waiting for hex packets on Serial …
> ```

---

## 📡 Part 3 — Flash the Relay ESP32(s) (Power Bank)

> Do this for **every relay node** you want in the mesh. Each one extends range by one hop.

### Step 10 — Open the Relay Firmware

1. **Unplug the Gateway ESP32** (or use a second USB port).
2. Plug in a **Relay ESP32** via USB.
3. In Arduino IDE → **File → Open…** → open `d:\simulator\receiver_relay_node.ino`.

---

### Step 11 — Select Board & Port

1. **Tools → Board → ESP32 Dev Module** (same as before).
2. **Tools → Port** → select the port that appeared when you plugged in this relay board.

---

### Step 12 — Upload the Relay Firmware

1. Click **→ Upload** and wait for `Done uploading.`
2. Open **Serial Monitor** at 115200 baud to confirm:
   ```
   ╔══════════════════════════════════════════╗
   ║  PulseNet-Mesh  —  Receiver/Relay Node  ║
   ╚══════════════════════════════════════════╝
   [RR] Listening for ESP-NOW broadcasts …
   ```
3. Unplug and connect this ESP32 to a **USB power bank** — it runs completely standalone.
4. **Repeat Steps 10–12 for each additional relay node.**

---

## 🚀 Part 4 — Run the Simulation

### Step 13 — Plug in the Gateway ESP32

Re-attach the **Gateway ESP32** to your laptop. Confirm its COM port in Device Manager again (Step 6).

---

### Step 14 — Start the Python Backend and Open the UI

The Python injector has been upgraded to a FastAPI web server that provides a Hardware-in-the-Loop (HIL) graphical user interface.

Open a PowerShell terminal in `d:\simulator` and run:

```powershell
# Replace COM4 with YOUR actual port number from Step 6
python simulated_patient_injector.py --port COM4
```

**What you'll see:**
```
[PulseNet] Opening serial port COM4 @ 115200 ...
[PulseNet] Starting web server on http://127.0.0.1:8000
INFO:     Started server process [12345]
INFO:     Waiting for application startup.
INFO:     Application startup complete.
INFO:     Uvicorn running on http://127.0.0.1:8000 (Press CTRL+C to quit)
```

Now, open your web browser and navigate to **`http://127.0.0.1:8000`**. 

You will see the **PulseNet-Mesh HIL Simulator UI**, where you can:
- **Start/Stop** the telemetry injection to your ESP32 hardware network.
- **Inject Faults:** Corrupt data packets, drop packets, or poison CRCs to test the mesh network's robustness.
- **Add Randomness:** Adjust the data latency sizes, signal noise, and jitter.
- **Watch Traversal:** Visually trace packets traversing the network from the injector to the gateway and over the relays.

---

### Step 15 — Monitor the Gateway (Optional)

Open the Arduino IDE **Serial Monitor** on the Gateway's port while the injector is running:

```
[GW] Received hex line (64 chars)
[GW] Decoded 32 bytes — broadcasting via ESP-NOW …
[GW] TX #1  OK
```

---

### Step 16 — Monitor the Relay Nodes (Optional)

Connect any relay ESP32 back to USB temporarily while keeping it receiving. Open its Serial Monitor:

```
[RR] Packet received  len=32  from FF:FF:FF:FF:FF:FF
[RR] Plaintext (47 bytes): {"node_id":2,"seq":5,"hr":108,"spo2":94,"hops":0}
[RR] ACCEPT: node=2 seq=5 hops=0 HR=108 SpO2=94
[RR] Modified JSON (48 bytes): {"node_id":2,"seq":5,"hr":108,"spo2":94,"hops":1}
[RR] Relayed 48 bytes (hops now 1)
```

---

## 🛑 Stopping the Simulation

Press **`Ctrl+C`** in the PowerShell window running the injector.

```
[PulseNet-Mesh] Injection stopped by user.
```

---

## 🔧 Troubleshooting

| Error | Cause | Fix |
|---|---|---|
| `Cannot open port 'COM3': FileNotFoundError` | Wrong port or ESP32 not connected | Run `[System.IO.Ports.SerialPort]::getportnames()` and pass the correct port with `--port COMX` |
| `Connecting...____` stuck in Arduino IDE | ESP32 not in flash mode | Hold **BOOT** button while uploading |
| `esp_now_init() failed` | Another sketch using Wi-Fi in AP mode | Re-upload the firmware cleanly |
| Relay receives but doesn't re-send | Hop limit (4) already reached | Normal — the packet has traversed 4 nodes |
| `DUP : node=X seq=Y` in relay log | Same packet received twice via different paths | Normal — deduplication is working correctly |
| Node 2 never shows `[!CRITICAL]` | Simulation hasn't run 50+ ticks yet | Wait ~100 seconds for the deterioration to progress |
| `ArduinoJson.h: No such file or directory` | Library not installed | Repeat Step 3 |

---

## 🗺️ System Architecture

```
┌──────────────────────────────┐
│  Laptop                      │
│  simulated_patient_injector  │  ← generates Node 1/2/3 vitals
│  (Python, AES encrypt, USB)  │
└──────────────┬───────────────┘
               │ USB Serial (115200 baud)
               │ hex-encoded encrypted packets + \n
               ▼
┌──────────────────────────────┐
│  Gateway ESP32  (Node 0)     │  gateway_node.ino
│  hex decode → esp_now_send  │
└──────────────┬───────────────┘
               │ ESP-NOW broadcast  (Channel 6, 802.11)
               ▼
┌──────────────────────────────┐
│  Relay ESP32  (power bank)   │  receiver_relay_node.ino
│  AES decrypt → dedup check  │
│  hops++ → AES re-encrypt    │
│  → esp_now_send (if <4 hops)│
└──────────────┬───────────────┘
               │ ESP-NOW re-broadcast
               ▼
          (next relay node or final sink)
```

---

## 📁 Project File Reference

| File | Purpose |
|---|---|
| [`requirements.txt`](file:///d:/simulator/requirements.txt) | Python dependencies |
| [`simulated_patient_injector.py`](file:///d:/simulator/simulated_patient_injector.py) | Runs on laptop — generates & sends telemetry |
| [`gateway_node.ino`](file:///d:/simulator/gateway_node.ino) | Firmware for the USB-attached Gateway ESP32 |
| [`receiver_relay_node.ino`](file:///d:/simulator/receiver_relay_node.ino) | Firmware for standalone power-bank Relay ESP32s |
