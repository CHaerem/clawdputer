#pragma once

#include <stdint.h>

#include <functional>
#include <string>

// Which BLE link produced the event. Apps subscribe to the same bus and
// filter by source so buddy doesn't see bridge traffic and vice versa.
enum class EventSource : uint8_t {
    NusLink,     // Claude Desktop buddy
    BridgeLink,  // clawd-bridge daemon
    WifiLink,    // wifi service (network up/down)
};

enum class EventType : uint8_t {
    LinkConnected,
    LinkDisconnected,
    LinkLine,
};

struct Event {
    EventType   type;
    EventSource source;
    std::string data;  // populated for LinkLine; empty otherwise
};

namespace events {

using Handler = std::function<void(const Event&)>;

int  subscribe(Handler h);
void unsubscribe(int token);
void publish(const Event& e);

}  // namespace events
