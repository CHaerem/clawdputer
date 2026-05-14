#include "ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#include "core/event_bus.h"

namespace {

constexpr const char* NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

NimBLECharacteristic* g_txChar    = nullptr;
bool                  g_connected = false;
std::string           g_rxBuf;
std::string           g_deviceName = "Claude-Cardputer";

class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string val = c->getValue();
        for (char ch : val) {
            if (ch == '\n') {
                events::publish({EventType::BleLine, g_rxBuf});
                g_rxBuf.clear();
            } else if (g_rxBuf.size() < 4096) {
                g_rxBuf.push_back(ch);
            } else {
                g_rxBuf.clear();
            }
        }
    }
};

class SrvCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        Serial.println("[ble] connected");
        g_connected = true;
        events::publish({EventType::BleConnected, {}});
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[ble] disconnected (reason=%d)\n", reason);
        g_connected = false;
        events::publish({EventType::BleDisconnected, {}});
        NimBLEDevice::startAdvertising();
    }
};

}  // namespace

namespace ble {

void begin() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char nm[40];
    snprintf(nm, sizeof(nm), "Claude-Cardputer-%02X%02X", mac[4], mac[5]);
    g_deviceName = nm;
    Serial.printf("[ble] name=%s\n", g_deviceName.c_str());

    NimBLEDevice::init(g_deviceName.c_str());
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setPower(9);
    Serial.println("[ble] NimBLE init ok");

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

    svc->start();

    // Split adv across the primary packet and scan response so neither overflows
    // 31 bytes. Primary packet carries flags + 128-bit service UUID; scan
    // response carries the complete local name.
    auto adv = NimBLEDevice::getAdvertising();

    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.setCompleteServices(NimBLEUUID(NUS_SERVICE));
    adv->setAdvertisementData(advData);

    NimBLEAdvertisementData scanData;
    scanData.setName(g_deviceName);
    adv->setScanResponseData(scanData);

    adv->enableScanResponse(true);
    if (adv->start()) {
        Serial.println("[ble] advertising started");
    } else {
        Serial.println("[ble] ADVERTISING START FAILED");
    }
}

void sendLine(const std::string& line) {
    if (!g_txChar || !g_connected) return;
    std::string s = line + "\n";
    g_txChar->setValue(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    g_txChar->notify();
}

bool        isConnected() { return g_connected; }
std::string deviceName() { return g_deviceName; }

void setDeviceName(const std::string& name) {
    g_deviceName = name;
    // NimBLE doesn't expose a clean rename-while-advertising path; the new
    // name takes effect on next boot. Persisting to NVS is a separate concern.
}

void clearBonds() { NimBLEDevice::deleteAllBonds(); }

}  // namespace ble
