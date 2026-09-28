/**
 * PulseNet-Mesh — Receiver / Relay Node Firmware
 * ===============================================
 * Board  : ESP32 (any variant), powered from a USB power bank
 * Role   : ESP-NOW receiver → AES decrypt → hop relay → re-encrypt → ESP-NOW re-broadcast
 *
 * Behaviour
 * ─────────
 *  1. Receives raw ESP-NOW broadcast packets on Channel 6.
 *  2. Decrypts the payload with AES-128-ECB (key: "PulseNetMeshKey!").
 *  3. Strips PKCS#7 padding and parses the JSON string.
 *  4. Checks a (node_id, seq) deduplication table — drops duplicates.
 *  5. Increments the "hops" field in the JSON.
 *  6. If hops < MAX_HOPS (4), re-encrypts and re-broadcasts once.
 *
 * Required libraries
 * ──────────────────
 *  - WiFi         (bundled with esp32 Arduino core)
 *  - esp_now      (bundled with esp32 Arduino core)
 *  - ArduinoJson  (install via Library Manager — v6.x or v7.x)
 *  - AESLib       (install via Library Manager — "AESLib by kakopappa")
 *    OR use the ESP32 hardware AES via mbedTLS (shown below, no extra lib needed)
 *
 * NOTE: This firmware uses mbedTLS (built into the ESP32 Arduino core) for
 *       AES-128-ECB, which requires no additional library installation.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "mbedtls/aes.h"

// ── Constants ──────────────────────────────────────────────────────────────────

static const uint8_t  ESPNOW_CHANNEL   = 6;
static const size_t   MAX_PAYLOAD_SIZE = 250;
static const int      MAX_HOPS         = 4;
static const uint8_t  AES_KEY[16]      = {
    'P','u','l','s','e','N','e','t','M','e','s','h','K','e','y','!'
};

// Broadcast address — re-floods the packet to all peers on the channel
static uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ── Deduplication table ────────────────────────────────────────────────────────
// Stores the highest received sequence number per node_id (nodes 0-255).
// Packets arriving with seq <= last_seen are discarded as duplicates.

static const uint8_t  MAX_NODES      = 16;
static int32_t        lastSeq[MAX_NODES];   // -1 = never seen

// ── Helpers: Hex encode / decode ───────────────────────────────────────────────

static const char HEX_CHARS[] = "0123456789ABCDEF";

static String bytesToHex(const uint8_t *data, size_t len) {
    String out;
    out.reserve(len * 2 + 1);
    for (size_t i = 0; i < len; i++) {
        out += HEX_CHARS[(data[i] >> 4) & 0x0F];
        out += HEX_CHARS[data[i] & 0x0F];
    }
    return out;
}

static size_t hexToBytes(const char *hexStr, uint8_t *buf, size_t bufLen) {
    size_t hexLen = strlen(hexStr);
    if (hexLen % 2 != 0) return 0;
    size_t byteCount = hexLen / 2;
    if (byteCount > bufLen) return 0;

    for (size_t i = 0; i < byteCount; i++) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int hi = nibble(hexStr[i * 2]);
        int lo = nibble(hexStr[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        buf[i] = (uint8_t)((hi << 4) | lo);
    }
    return byteCount;
}

// ── AES-128-ECB via mbedTLS ────────────────────────────────────────────────────

/**
 * Decrypt *inLen* bytes from *in* into *out* using AES-128-ECB.
 * *out* must be at least *inLen* bytes.
 * Returns true on success.
 */
static bool aesDecrypt(const uint8_t *in, size_t inLen, uint8_t *out) {
    if (inLen == 0 || inLen % 16 != 0) return false;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, AES_KEY, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }

    for (size_t offset = 0; offset < inLen; offset += 16) {
        if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT,
                                   in + offset, out + offset) != 0) {
            mbedtls_aes_free(&ctx);
            return false;
        }
    }
    mbedtls_aes_free(&ctx);
    return true;
}

/**
 * Encrypt *inLen* bytes from *in* into *out* using AES-128-ECB + PKCS#7.
 * *outLen* is set to the padded ciphertext length.
 * *out* must be at least ((*inLen / 16) + 1) * 16 bytes.
 * Returns true on success.
 */
static bool aesEncrypt(const uint8_t *in, size_t inLen,
                       uint8_t *out, size_t &outLen) {
    uint8_t padLen = (uint8_t)(16 - (inLen % 16));
    outLen = inLen + padLen;

    // Build padded plaintext
    uint8_t padded[MAX_PAYLOAD_SIZE + 16] = {};
    if (outLen > sizeof(padded)) return false;
    memcpy(padded, in, inLen);
    memset(padded + inLen, padLen, padLen);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, AES_KEY, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    for (size_t offset = 0; offset < outLen; offset += 16) {
        if (mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT,
                                   padded + offset, out + offset) != 0) {
            mbedtls_aes_free(&ctx);
            return false;
        }
    }
    mbedtls_aes_free(&ctx);
    return true;
}

// ── PKCS#7 unpad ──────────────────────────────────────────────────────────────

/**
 * Validate and strip PKCS#7 padding in-place.
 * Returns the unpadded length, or 0 if padding is invalid.
 */
static size_t pkcs7Unpad(uint8_t *buf, size_t len) {
    if (len == 0 || len % 16 != 0) return 0;
    uint8_t padLen = buf[len - 1];
    if (padLen == 0 || padLen > 16) return 0;
    for (size_t i = len - padLen; i < len; i++) {
        if (buf[i] != padLen) return 0;
    }
    return len - padLen;
}

// ── ESP-NOW receive callback ───────────────────────────────────────────────────

void onDataReceived(const uint8_t *mac, const uint8_t *data, int dataLen) {
    Serial.printf("\n[RR] Packet received  len=%d  from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  dataLen,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (dataLen <= 0 || (size_t)dataLen > MAX_PAYLOAD_SIZE) {
        Serial.println("[RR] WARN: packet size out of range — dropping.");
        return;
    }

    // ── Step 1: AES-128-ECB decrypt ─────────────────────────────────────────
    uint8_t decrypted[MAX_PAYLOAD_SIZE] = {};
    if (!aesDecrypt(data, (size_t)dataLen, decrypted)) {
        Serial.println("[RR] ERROR: AES decrypt failed — dropping.");
        return;
    }

    // ── Step 2: Strip PKCS#7 padding ────────────────────────────────────────
    size_t plainLen = pkcs7Unpad(decrypted, (size_t)dataLen);
    if (plainLen == 0) {
        Serial.println("[RR] ERROR: invalid PKCS#7 padding — dropping.");
        return;
    }
    decrypted[plainLen] = '\0';   // null-terminate for JSON parsing
    Serial.printf("[RR] Plaintext (%u bytes): %s\n", plainLen, (char *)decrypted);

    // ── Step 3: Parse JSON ───────────────────────────────────────────────────
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, (char *)decrypted);
    if (err) {
        Serial.printf("[RR] ERROR: JSON parse failed (%s) — dropping.\n", err.c_str());
        return;
    }

    int nodeId = doc["node_id"] | -1;
    int seq    = doc["seq"]     | -1;
    int hops   = doc["hops"]   | 0;

    if (nodeId < 0 || seq < 0) {
        Serial.println("[RR] ERROR: missing node_id or seq — dropping.");
        return;
    }

    // ── Step 4: Deduplication ────────────────────────────────────────────────
    if ((size_t)nodeId >= MAX_NODES) {
        Serial.printf("[RR] WARN: node_id %d >= MAX_NODES — dropping.\n", nodeId);
        return;
    }
    if (seq <= lastSeq[nodeId]) {
        Serial.printf("[RR] DUP : node=%d seq=%d (last=%d) — dropping.\n",
                      nodeId, seq, lastSeq[nodeId]);
        return;
    }
    lastSeq[nodeId] = seq;

    Serial.printf("[RR] ACCEPT: node=%d seq=%d hops=%d HR=%d SpO2=%d\n",
                  nodeId, seq, hops,
                  (int)(doc["hr"] | 0),
                  (int)(doc["spo2"] | 0));

    // ── Step 5: Hop-limit check ──────────────────────────────────────────────
    if (hops >= MAX_HOPS) {
        Serial.printf("[RR] HOP LIMIT reached (%d/%d) — not relaying.\n", hops, MAX_HOPS);
        return;
    }

    // ── Step 6: Increment hops ───────────────────────────────────────────────
    doc["hops"] = hops + 1;

    // ── Step 7: Re-serialise JSON ────────────────────────────────────────────
    char modifiedJson[256] = {};
    size_t jsonLen = serializeJson(doc, modifiedJson, sizeof(modifiedJson));
    Serial.printf("[RR] Modified JSON (%u bytes): %s\n", jsonLen, modifiedJson);

    // ── Step 8: Re-encrypt ───────────────────────────────────────────────────
    uint8_t ciphertext[MAX_PAYLOAD_SIZE + 16] = {};
    size_t  cipherLen = 0;
    if (!aesEncrypt((const uint8_t *)modifiedJson, jsonLen,
                    ciphertext, cipherLen)) {
        Serial.println("[RR] ERROR: AES re-encrypt failed — not relaying.");
        return;
    }

    // ── Step 9: Safety ceiling check ────────────────────────────────────────
    if (cipherLen > MAX_PAYLOAD_SIZE) {
        Serial.printf("[RR] WARN: ciphertext %u bytes > ceiling — not relaying.\n", cipherLen);
        return;
    }

    // ── Step 10: Re-broadcast ────────────────────────────────────────────────
    esp_err_t result = esp_now_send(BROADCAST_ADDR, ciphertext, cipherLen);
    if (result == ESP_OK) {
        Serial.printf("[RR] Relayed %u bytes (hops now %d)\n", cipherLen, hops + 1);
    } else {
        Serial.printf("[RR] esp_now_send() error: 0x%X\n", result);
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  PulseNet-Mesh  —  Receiver/Relay Node  ║");
    Serial.println("╚══════════════════════════════════════════╝");

    // Initialise dedup table
    for (uint8_t i = 0; i < MAX_NODES; i++) lastSeq[i] = -1;

    // ── Wi-Fi: STA mode, Channel 6 (no association)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[RR] MAC Address : %s\n", WiFi.macAddress().c_str());
    Serial.printf("[RR] Channel     : %u\n", ESPNOW_CHANNEL);

    // ── ESP-NOW init
    if (esp_now_init() != ESP_OK) {
        Serial.println("[RR] FATAL: esp_now_init() failed — halting.");
        while (true) { delay(1000); }
    }

    // Register broadcast peer for re-transmission
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, BROADCAST_ADDR, 6);
    peerInfo.channel = ESPNOW_CHANNEL;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("[RR] FATAL: esp_now_add_peer() failed — halting.");
        while (true) { delay(1000); }
    }

    // Register receive callback
    esp_now_register_recv_cb(onDataReceived);

    Serial.println("[RR] Listening for ESP-NOW broadcasts …\n");
}

// ── Main Loop ──────────────────────────────────────────────────────────────────

void loop() {
    // All work is done in the ESP-NOW receive callback.
    // Yield to the Wi-Fi/ESP-NOW task to avoid watchdog resets.
    delay(10);
}
