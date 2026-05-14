#pragma once

#include <functional>
#include <string>

enum class EventType {
    BleConnected,
    BleDisconnected,
    BleLine,
};

struct Event {
    EventType   type;
    std::string data;  // populated for BleLine; empty otherwise
};

namespace events {

using Handler = std::function<void(const Event&)>;

// Subscribe to events. Returns a token usable with unsubscribe(). Subscriptions
// are intended to be made from app onEnter and released in onExit.
int  subscribe(Handler h);
void unsubscribe(int token);

// Publish an event to all current subscribers. Called from service threads
// (e.g. BLE callbacks); handlers run inline on the publisher's thread.
void publish(const Event& e);

}  // namespace events
