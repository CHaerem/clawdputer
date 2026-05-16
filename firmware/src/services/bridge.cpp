#include "bridge.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "ble.h"
#include "wifi.h"

namespace bridge {

namespace {

WiFiClient   g_tcp;
bool         g_tcpUp        = false;
std::string  g_tcpHost;
int          g_tcpPort       = 0;
uint32_t     g_lastDiscovery = 0;
std::string  g_tcpRxBuf;
int          g_eventSub      = 0;
bool         g_paused       = false;

constexpr uint32_t DISCOVERY_INTERVAL_MS = 30000;

void tryDiscover() {
    if (!wifi::isConnected()) return;
    if (g_tcpUp) return;
    uint32_t now = millis();
    if (g_lastDiscovery && now - g_lastDiscovery < DISCOVERY_INTERVAL_MS) return;
    g_lastDiscovery = now;

    if (!MDNS.begin("clawdputer-client")) {
        Serial.println("[bridge] mDNS begin failed");
        return;
    }
    int n = MDNS.queryService("clawd-bridge", "tcp");
    if (n <= 0) {
        Serial.println("[bridge] mDNS: no clawd-bridge advertised");
        return;
    }
    IPAddress ip = MDNS.IP(0);
    int       port = MDNS.port(0);
    Serial.printf("[bridge] discovered %s:%d via mDNS\n", ip.toString().c_str(), port);

    if (g_tcp.connect(ip, port, 3000)) {
        g_tcpUp   = true;
        g_tcpHost = ip.toString().c_str();
        g_tcpPort = port;
        g_tcp.setNoDelay(true);
        Serial.println("[bridge] TCP connected");
        events::publish({EventType::LinkConnected, EventSource::BridgeLink, {}});
    } else {
        Serial.println("[bridge] TCP connect failed");
    }
}

void pumpTcp() {
    if (!g_tcpUp) return;
    if (!g_tcp.connected()) {
        Serial.println("[bridge] TCP peer dropped");
        g_tcp.stop();
        g_tcpUp = false;
        g_tcpRxBuf.clear();
        events::publish({EventType::LinkDisconnected, EventSource::BridgeLink, {}});
        return;
    }
    while (g_tcp.available() > 0) {
        int c = g_tcp.read();
        if (c < 0) break;
        if (c == '\n') {
            events::publish({EventType::LinkLine, EventSource::BridgeLink, g_tcpRxBuf});
            g_tcpRxBuf.clear();
        } else if (g_tcpRxBuf.size() < 4096) {
            g_tcpRxBuf.push_back((char)c);
        } else {
            g_tcpRxBuf.clear();
        }
    }
}

void onTickHook(const Event&) {}  // unused, kept for symmetry

}  // namespace

void begin() {
    // BLE service is initialised by ble::begin(); we just need to start
    // polling TCP when WiFi is up. We subscribe to the bus so we can pump
    // the TCP socket on each WifiLink event tick — but pumpTcp() is also
    // called from a periodic task installed by an FreeRTOS-style timer
    // below. Simpler: piggyback on the main loop's tick driver.
    (void)events::subscribe([](const Event&){});  // placeholder; tick() below drives polling
}

bool isConnected() {
    return ble::isConnected(EventSource::BridgeLink) || g_tcpUp;
}

void sendLine(const std::string& line) {
    if (ble::isConnected(EventSource::BridgeLink)) {
        ble::sendLine(EventSource::BridgeLink, line);
        return;
    }
    if (g_tcpUp && g_tcp.connected()) {
        std::string s = line + "\n";
        g_tcp.write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
}

Transport activeTransport() {
    if (ble::isConnected(EventSource::BridgeLink)) return Transport::Ble;
    if (g_tcpUp) return Transport::Tcp;
    return Transport::None;
}

void pause() {
    // Setting the flag isn't enough — the TCP socket and its kernel-side
    // buffers stay allocated and fragment the heap. Close it so mbedTLS
    // gets a contiguous block for its handshake.
    g_paused = true;
    if (g_tcpUp) {
        g_tcp.stop();
        g_tcpUp = false;
        g_tcpRxBuf.clear();
        std::string().swap(g_tcpRxBuf);  // release capacity, not just size
        events::publish({EventType::LinkDisconnected, EventSource::BridgeLink, {}});
        Serial.println("[bridge] paused (TCP closed)");
    }
}

void resume() {
    g_paused = false;
    // Reset the discovery throttle so we reconnect on the next tick rather
    // than waiting up to 30 s.
    g_lastDiscovery = 0;
}

// Called from the main loop tick driver in main.cpp.
void tick() {
    if (g_paused) return;
    pumpTcp();
    tryDiscover();
}

}  // namespace bridge

// main.cpp calls this; declared extern to avoid including <Arduino.h> in
// the public header.
extern "C" void clawd_bridge_tick() { bridge::tick(); }
