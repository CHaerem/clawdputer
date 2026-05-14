#include "ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#include "core/event_bus.h"

namespace {

// Nordic UART Service — buddy protocol (Anthropic spec)
constexpr const char* NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// clawd-bridge service — see protocol/WIRE.md
constexpr const char* BRIDGE_SERVICE = "c1aedb01-1d0c-4adc-9b1a-c1aedb010000";
constexpr const char* BRIDGE_RX      = "c1aedb01-1d0c-4adc-9b1a-c1aedb010001";
constexpr const char* BRIDGE_TX      = "c1aedb01-1d0c-4adc-9b1a-c1aedb010002";

struct Link {
    EventSource           source;
    const char*           name;
    NimBLECharacteristic* tx;
    std::string           rxBuf;
    bool                  connected;

    Link(EventSource s, const char* n)
        : source(s), name(n), tx(nullptr), connected(false) {}
};

Link        g_nus(EventSource::NusLink,    "nus");
Link        g_bridge(EventSource::BridgeLink, "bridge");
std::string g_deviceName = "Claude-Cardputer";

Link* linkForChar(NimBLECharacteristic* c) {
    if (c == g_nus.tx)    return &g_nus;
    if (c == g_bridge.tx) return &g_bridge;
    return nullptr;
}

class RxCallback : public NimBLECharacteristicCallbacks {
public:
    explicit RxCallback(Link* link) : link_(link) {}

    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string val = c->getValue();
        for (char ch : val) {
            if (ch == '\n') {
                events::publish({EventType::LinkLine, link_->source, link_->rxBuf});
                link_->rxBuf.clear();
            } else if (link_->rxBuf.size() < 4096) {
                link_->rxBuf.push_back(ch);
            } else {
                link_->rxBuf.clear();
            }
        }
    }

private:
    Link* link_;
};

class TxCallback : public NimBLECharacteristicCallbacks {
public:
    explicit TxCallback(Link* link) : link_(link) {}

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
        bool wantConnected = subValue != 0;
        if (wantConnected == link_->connected) return;
        link_->connected = wantConnected;
        Serial.printf("[ble:%s] %s\n", link_->name,
                      wantConnected ? "subscribed" : "unsubscribed");
        events::publish({
            wantConnected ? EventType::LinkConnected : EventType::LinkDisconnected,
            link_->source,
            {},
        });
    }

private:
    Link* link_;
};

class SrvCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        Serial.println("[ble] peer connected");
        // Keep advertising so a second central (buddy + bridge in parallel,
        // or bridge while Claude Desktop is already paired) can still find
        // us. NimBLE stops advertising by default after the first connect.
        NimBLEDevice::startAdvertising();
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        Serial.printf("[ble] peer disconnected (reason=%d)\n", reason);
        NimBLEDevice::startAdvertising();
    }
};

void buildService(NimBLEServer* server,
                  Link*         link,
                  const char*   serviceUuid,
                  const char*   rxUuid,
                  const char*   txUuid) {
    auto svc    = server->createService(serviceUuid);
    auto rxChar = svc->createCharacteristic(
        rxUuid,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
    rxChar->setCallbacks(new RxCallback(link));

    link->tx = svc->createCharacteristic(
        txUuid,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
    link->tx->setCallbacks(new TxCallback(link));
    // NimBLE 2.x starts services automatically when the server starts; no
    // explicit svc->start() call needed.
}

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

    buildService(server, &g_nus,    NUS_SERVICE,    NUS_RX,    NUS_TX);
    buildService(server, &g_bridge, BRIDGE_SERVICE, BRIDGE_RX, BRIDGE_TX);

    // Advertising packet only carries NUS so Claude Desktop's scan filter
    // matches us. The bridge service is discovered after connect.
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

void sendLine(EventSource source, const std::string& line) {
    Link* link = (source == EventSource::NusLink) ? &g_nus : &g_bridge;
    if (!link->tx || !link->connected) return;
    std::string s = line + "\n";
    link->tx->setValue(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    link->tx->notify();
}

bool isConnected(EventSource source) {
    return (source == EventSource::NusLink) ? g_nus.connected : g_bridge.connected;
}

std::string deviceName() { return g_deviceName; }

void setDeviceName(const std::string& name) { g_deviceName = name; }

void clearBonds() { NimBLEDevice::deleteAllBonds(); }

}  // namespace ble
