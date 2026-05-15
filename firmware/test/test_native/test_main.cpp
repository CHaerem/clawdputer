// Host-side unit tests for the hardware-agnostic core modules. Runs
// under PlatformIO's `native` env on the developer's Mac (or CI) — no
// ESP32 toolchain or device required.
//
//   pio test -e native -d firmware

#include <unity.h>

#include "core/app.h"
#include "core/registry.h"
#include "core/event_bus.h"

// PIO test-build doesn't honour build_src_filter for the native env in the
// way we'd hope, so the .cpp implementations are pulled in directly here.
// They're plain STL underneath, so they compile cleanly on the host.
#include "../../src/core/registry.cpp"
#include "../../src/core/event_bus.cpp"

namespace {

// Dummy apps for registry tests.
void noop() {}
void noop_key(char) {}
void noop_evt(const Event&) {}

App g_appA = { "alpha", "Alpha", "first",  0,
               noop, noop, noop, noop_key, noop, noop_evt };
App g_appB = { "beta",  "Beta",  "second", 0,
               noop, noop, noop, noop_key, noop, noop_evt };

}  // namespace

void test_registry_starts_empty_then_grows() {
    TEST_ASSERT_EQUAL_size_t(0, registry::count());
    registry::add(&g_appA);
    TEST_ASSERT_EQUAL_size_t(1, registry::count());
    registry::add(&g_appB);
    TEST_ASSERT_EQUAL_size_t(2, registry::count());
}

void test_registry_at_returns_in_order() {
    TEST_ASSERT_EQUAL_STRING("alpha", registry::at(0)->id);
    TEST_ASSERT_EQUAL_STRING("beta",  registry::at(1)->id);
    TEST_ASSERT_NULL(registry::at(99));
}

void test_registry_find_by_id() {
    TEST_ASSERT_EQUAL_STRING("alpha", registry::find("alpha")->id);
    TEST_ASSERT_EQUAL_STRING("beta",  registry::find("beta")->id);
    TEST_ASSERT_NULL(registry::find("missing"));
}

void test_event_bus_subscriber_receives_publish() {
    int hits = 0;
    EventSource lastSource = EventSource::NusLink;
    int token = events::subscribe([&](const Event& e) {
        hits++;
        lastSource = e.source;
    });

    events::publish({EventType::LinkConnected, EventSource::BridgeLink, ""});
    TEST_ASSERT_EQUAL(1, hits);
    TEST_ASSERT_EQUAL(EventSource::BridgeLink, lastSource);

    events::unsubscribe(token);
    events::publish({EventType::LinkLine, EventSource::WifiLink, "ignored"});
    TEST_ASSERT_EQUAL(1, hits);  // unchanged after unsubscribe
}

void test_event_bus_multiple_subscribers() {
    int a = 0, b = 0;
    int tokA = events::subscribe([&](const Event&) { a++; });
    int tokB = events::subscribe([&](const Event&) { b++; });

    events::publish({EventType::LinkLine, EventSource::NusLink, "hi"});
    TEST_ASSERT_EQUAL(1, a);
    TEST_ASSERT_EQUAL(1, b);

    events::unsubscribe(tokA);
    events::publish({EventType::LinkLine, EventSource::NusLink, "again"});
    TEST_ASSERT_EQUAL(1, a);
    TEST_ASSERT_EQUAL(2, b);

    events::unsubscribe(tokB);
}

void test_event_bus_handler_carries_data() {
    std::string payload;
    int tok = events::subscribe([&](const Event& e) { payload = e.data; });

    events::publish({EventType::LinkLine, EventSource::BridgeLink,
                     "hello-world"});
    TEST_ASSERT_EQUAL_STRING("hello-world", payload.c_str());

    events::unsubscribe(tok);
}

void setUp()    {}
void tearDown() {}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_registry_starts_empty_then_grows);
    RUN_TEST(test_registry_at_returns_in_order);
    RUN_TEST(test_registry_find_by_id);
    RUN_TEST(test_event_bus_subscriber_receives_publish);
    RUN_TEST(test_event_bus_multiple_subscribers);
    RUN_TEST(test_event_bus_handler_carries_data);
    return UNITY_END();
}
