/**
 * PulseNet-Mesh — Gateway Node Firmware (Node 0)
 * ===============================================
 * Board  : ESP32 (any variant)
 * Role   : Serial-bridge → ESP-NOW broadcaster
 *
 * The laptop-side Python injector writes AES-encrypted hex strings
 * (terminated with '\n') to this device's USB-serial port at 115200 baud.
 * This firmware:
 *   1. Reads one line at a time from Serial.
 *   2. Trims whitespace / CR.
 *   3. Decodes the hex string into a raw byte array.
 *   4. Broadcasts the raw bytes over ESP-NOW (broadcast MAC, Channel 6).
 *
 * Upload settings (Arduino IDE)
 * ─────────────────────────────
 *   Board   : "ESP32 Dev Module"  (or your specific variant)
 *   Upload  : 921600 baud
 *   Flash   : 4 MB, QIO
 *
 * Required libraries (install via Library Manager)
 * ──────────────────────────────────────────────────
 *   - WiFi       (bundled with esp32 Arduino core)
 *   - esp_now    (bundled with esp32 Arduino core)
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ── Constants ──────────────────────────────────────────────────────────────────

static const uint32_t SERIAL_BAUD      = 115200;
static const uint8_t  ESPNOW_CHANNEL   = 6;
static const size_t   MAX_PAYLOAD_SIZE = 250;   // ESP-NOW hard ceiling

// Broadcast address — reaches every ESP32 on the same channel
static uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Global state ───────────────────────────────────────────────────────────────

static bool    espNowReady = false;
static uint32_t txCount   = 0;
static uint32_t txErrors  = 0;

// ── ESP-NOW send callback ──────────────────────────────────────────────────────

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        txCount++;
        Serial.printf("[GW] TX #%u  OK\n", txCount);
    } else {
        txErrors++;
        Serial.printf("[GW] TX #%u  FAIL (total errors: %u)\n", txCount + txErrors, txErrors);
    }
}

// ── Hex decode helper ──────────────────────────────────────────────────────────

/**
 * Decode a NUL-terminated uppercase/lowercase hex string into *buf*.
 * Returns the number of bytes written, or 0 on error.
 */
static size_t hexDecode(const char *hexStr, uint8_t *buf, size_t bufLen) {
    size_t hexLen = strlen(hexStr);
    if (hexLen % 2 != 0) {
        Serial.println("[GW] ERROR: odd hex length — discarding packet.");
        return 0;
    }
    size_t byteCount = hexLen / 2;
    if (byteCount > bufLen) {
        Serial.printf("[GW] ERROR: decoded size %u > buffer %u — discarding.\n",
                      byteCount, bufLen);
        return 0;
    }

    for (size_t i = 0; i < byteCount; i++) {
        char highNibble = hexStr[i * 2];
        char lowNibble  = hexStr[i * 2 + 1];

        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };

        int hi = nibble(highNibble);
        int lo = nibble(lowNibble);
        if (hi < 0 || lo < 0) {
            Serial.printf("[GW] ERROR: invalid hex char at pos %u — discarding.\n", i * 2);
            return 0;
        }
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return byteCount;
}

// ── Setup ──────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);  // give the host a moment to open the port

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║  PulseNet-Mesh  —  Gateway Node 0   ║");
    Serial.println("╚══════════════════════════════════════╝");

    // ── Wi-Fi: Station mode on Channel 6 (no AP association)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[GW] Wi-Fi STA  MAC : %s\n", WiFi.macAddress().c_str());
    Serial.printf("[GW] ESP-NOW channel : %u\n", ESPNOW_CHANNEL);

    // ── ESP-NOW initialisation
    if (esp_now_init() != ESP_OK) {
        Serial.println("[GW] FATAL: esp_now_init() failed — halting.");
        while (true) { delay(1000); }
    }
    esp_now_register_send_cb(onDataSent);

    // ── Register broadcast peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, BROADCAST_ADDR, 6);
    peerInfo.channel  = ESPNOW_CHANNEL;
    peerInfo.encrypt  = false;   // encryption handled at application layer

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[GW] FATAL: esp_now_add_peer() failed — halting.");
        while (true) { delay(1000); }
    }

    espNowReady = true;
    Serial.println("[GW] Ready — waiting for hex packets on Serial …\n");
}

// ── Main Loop ──────────────────────────────────────────────────────────────────

void loop() {
    if (!espNowReady) return;

    // Block until a complete '\n'-terminated line arrives
    if (!Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();   // remove CR, trailing spaces, etc.

    if (line.length() == 0) return;   // ignore blank lines

    Serial.printf("[GW] Received hex line (%u chars)\n", line.length());

    // ── Decode hex → raw bytes ──────────────────────────────────────────────
    uint8_t payload[MAX_PAYLOAD_SIZE];
    size_t  payloadLen = hexDecode(line.c_str(), payload, MAX_PAYLOAD_SIZE);

    if (payloadLen == 0) return;   // decode error already logged

    // ── Safety ceiling check ────────────────────────────────────────────────
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        Serial.printf("[GW] WARN: payload %u bytes exceeds %u-byte ceiling — dropping.\n",
                      payloadLen, MAX_PAYLOAD_SIZE);
        return;
    }

    Serial.printf("[GW] Decoded %u bytes — broadcasting via ESP-NOW …\n", payloadLen);

    // ── Broadcast over the air ──────────────────────────────────────────────
    esp_err_t result = esp_now_send(BROADCAST_ADDR, payload, payloadLen);
    if (result != ESP_OK) {
        Serial.printf("[GW] esp_now_send() error: 0x%X\n", result);
    }
}
