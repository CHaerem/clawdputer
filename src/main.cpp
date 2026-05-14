// clawdputer — Cardputer port of anthropics/claude-desktop-buddy
//
// Talks to Claude Desktop over BLE Nordic UART Service. Receives
// newline-delimited JSON heartbeats/commands, displays session state, and lets
// the user approve or deny pending tool-permission prompts from the keyboard.
//
// Wire protocol: https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md

#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <esp_mac.h>

static constexpr const char* NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr const char* NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static constexpr const char* NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static NimBLECharacteristic* g_txChar = nullptr;
static bool   g_connected = false;
static String g_rxBuf;
static String g_deviceName = "Claude-Cardputer";
static String g_owner = "";

static String   g_msg;
static int      g_total = 0, g_running = 0, g_waiting = 0;
static uint32_t g_tokensToday = 0;
static uint32_t g_lastHeartbeatMs = 0;

static String g_promptId, g_promptTool, g_promptHint;

static uint32_t g_bootMs = 0;
static uint32_t g_approvals = 0, g_denials = 0;
static bool     g_screenDirty = true;

static void render() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);

    d.setTextSize(2);
    d.setTextColor(WHITE);
    d.setCursor(6, 4);
    d.print("clawdputer");

    d.setTextSize(1);
    d.setCursor(6, 28);
    if (!g_connected) {
        d.setTextColor(0x7BEF);
        d.print("BLE advertising as");
        d.setCursor(6, 40);
        d.setTextColor(WHITE);
        d.print(g_deviceName);
        d.setCursor(6, 60);
        d.setTextColor(0x7BEF);
        d.print("Claude > Developer >");
        d.setCursor(6, 72);
        d.print("Open Hardware Buddy");
        return;
    }

    d.setTextColor(0x07E0);
    d.print("connected");
    if (g_owner.length()) {
        d.setTextColor(WHITE);
        d.print("  ");
        d.print(g_owner);
    }

    if (g_promptId.length()) {
        d.fillRect(0, 44, 240, 90, 0x4800);
        d.setTextColor(WHITE);
        d.setTextSize(2);
        d.setCursor(6, 48);
        d.print("APPROVE?");
        d.setTextSize(1);
        d.setCursor(6, 70);
        d.setTextColor(0xFFE0);
        d.print(g_promptTool);
        d.setCursor(6, 82);
        d.setTextColor(WHITE);
        String h = g_promptHint;
        if (h.length() > 38) h = h.substring(0, 35) + "...";
        d.print(h);
        d.setCursor(6, 102);
        d.setTextColor(0x07E0);
        d.print("[Y] approve   ");
        d.setTextColor(0xF800);
        d.print("[N] deny");
        return;
    }

    d.setTextColor(WHITE);
    d.setCursor(6, 44);
    d.printf("sessions:%d  run:%d  wait:%d", g_total, g_running, g_waiting);
    d.setCursor(6, 56);
    d.printf("tokens today: %u", (unsigned)g_tokensToday);

    if (g_msg.length()) {
        d.setCursor(6, 76);
        d.setTextColor(0xFFE0);
        String m = g_msg;
        if (m.length() > 40) m = m.substring(0, 37) + "...";
        d.print(m);
    }

    d.setCursor(6, 122);
    d.setTextColor(0x7BEF);
    d.printf("appr:%u  deny:%u", (unsigned)g_approvals, (unsigned)g_denials);
}

static void sendLine(const String& line) {
    if (!g_txChar || !g_connected) return;
    String s = line + "\n";
    g_txChar->setValue((uint8_t*)s.c_str(), s.length());
    g_txChar->notify();
}

static void sendPermission(const String& id, const char* decision) {
    JsonDocument doc;
    doc["cmd"] = "permission";
    doc["id"] = id;
    doc["decision"] = decision;
    String out;
    serializeJson(doc, out);
    sendLine(out);
}

static void handleSnapshot(JsonDocument& doc) {
    g_total       = doc["total"]        | 0;
    g_running     = doc["running"]      | 0;
    g_waiting     = doc["waiting"]      | 0;
    g_tokensToday = doc["tokens_today"] | 0;
    if (doc["msg"].is<const char*>()) g_msg = (const char*)doc["msg"];

    if (doc["prompt"].is<JsonObject>()) {
        JsonObject p = doc["prompt"];
        g_promptId   = (const char*)(p["id"]   | "");
        g_promptTool = (const char*)(p["tool"] | "");
        g_promptHint = (const char*)(p["hint"] | "");
    } else {
        g_promptId = ""; g_promptTool = ""; g_promptHint = "";
    }
    g_lastHeartbeatMs = millis();
    g_screenDirty = true;
}

static void handleCommand(JsonDocument& doc) {
    String c = (const char*)(doc["cmd"] | "");
    if (c.length() == 0) return;

    if (c == "owner") {
        if (doc["name"].is<const char*>()) g_owner = (const char*)doc["name"];
        sendLine("{\"ack\":\"owner\",\"ok\":true,\"n\":0}");
        g_screenDirty = true;
    } else if (c == "name") {
        if (doc["name"].is<const char*>()) g_deviceName = (const char*)doc["name"];
        sendLine("{\"ack\":\"name\",\"ok\":true,\"n\":0}");
    } else if (c == "status") {
        JsonDocument resp;
        resp["ack"] = "status";
        resp["ok"]  = true;
        resp["n"]   = 0;
        auto data = resp["data"].to<JsonObject>();
        data["name"] = g_deviceName;
        data["sec"]  = true;
        auto sys = data["sys"].to<JsonObject>();
        sys["up"]   = (millis() - g_bootMs) / 1000;
        sys["heap"] = (uint32_t)ESP.getFreeHeap();
        auto stats = data["stats"].to<JsonObject>();
        stats["appr"] = g_approvals;
        stats["deny"] = g_denials;
        String out;
        serializeJson(resp, out);
        sendLine(out);
    } else if (c == "unpair") {
        sendLine("{\"ack\":\"unpair\",\"ok\":true,\"n\":0}");
        NimBLEDevice::deleteAllBonds();
    }
    // Other commands (char_begin/file/chunk/...): intentionally no ack.
    // Per protocol, declining a folder push is signaled by NOT acking.
}

static void handleLine(const String& line) {
    if (line.length() == 0) return;
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;

    if (doc["cmd"].is<const char*>()) {
        handleCommand(doc);
        return;
    }
    if (!doc["total"].isNull() || doc["msg"].is<const char*>() || doc["prompt"].is<JsonObject>()) {
        handleSnapshot(doc);
    }
}

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        std::string val = c->getValue();
        for (char ch : val) {
            if (ch == '\n') {
                handleLine(g_rxBuf);
                g_rxBuf = "";
            } else if (g_rxBuf.length() < 4096) {
                g_rxBuf += ch;
            } else {
                g_rxBuf = "";
            }
        }
    }
};

class SrvCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        g_connected = true;
        g_screenDirty = true;
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        g_connected = false;
        g_msg = "";
        g_promptId = ""; g_promptTool = ""; g_promptHint = "";
        g_lastHeartbeatMs = 0;
        g_screenDirty = true;
        NimBLEDevice::startAdvertising();
    }
};

static void handleKeys() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    if (g_promptId.length() == 0) return;

    auto state = M5Cardputer.Keyboard.keysState();
    for (char ch : state.word) {
        if (ch == 'y' || ch == 'Y') {
            sendPermission(g_promptId, "once");
            g_approvals++;
            g_promptId = ""; g_promptTool = ""; g_promptHint = "";
            g_screenDirty = true;
            break;
        }
        if (ch == 'n' || ch == 'N') {
            sendPermission(g_promptId, "deny");
            g_denials++;
            g_promptId = ""; g_promptTool = ""; g_promptHint = "";
            g_screenDirty = true;
            break;
        }
    }
}

void setup() {
    g_bootMs = millis();
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char nm[40];
    snprintf(nm, sizeof(nm), "Claude-Cardputer-%02X%02X", mac[4], mac[5]);
    g_deviceName = nm;

    render();

    NimBLEDevice::init(g_deviceName.c_str());
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    auto server = NimBLEDevice::createServer();
    server->setCallbacks(new SrvCallback());

    auto svc = server->createService(NUS_SERVICE);

    auto rxChar = svc->createCharacteristic(
        NUS_RX,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    rxChar->setCallbacks(new RxCallback());

    g_txChar = svc->createCharacteristic(
        NUS_TX,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);

    auto adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE);
    adv->setName(std::string(g_deviceName.c_str()));
    adv->enableScanResponse(true);
    adv->start();
}

void loop() {
    M5Cardputer.update();
    handleKeys();

    if (g_connected && g_lastHeartbeatMs && (millis() - g_lastHeartbeatMs > 30000)) {
        g_msg = "(no heartbeat)";
        g_screenDirty = true;
        g_lastHeartbeatMs = 0;
    }

    if (g_screenDirty) {
        render();
        g_screenDirty = false;
    }
    delay(20);
}
