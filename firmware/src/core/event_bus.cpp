#include "event_bus.h"

#include <vector>

namespace {
struct Slot {
    int               token;
    events::Handler   handler;
};

std::vector<Slot>& slots() {
    static std::vector<Slot> v;
    return v;
}

int& nextToken() {
    static int t = 1;
    return t;
}
}  // namespace

namespace events {

int subscribe(Handler h) {
    int t = nextToken()++;
    slots().push_back({t, std::move(h)});
    return t;
}

void unsubscribe(int token) {
    auto& v = slots();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->token == token) {
            v.erase(it);
            return;
        }
    }
}

void publish(const Event& e) {
    // Copy snapshot so a handler that unsubscribes mid-dispatch doesn't
    // invalidate our iterator.
    auto snapshot = slots();
    for (auto& s : snapshot) s.handler(e);
}

}  // namespace events
