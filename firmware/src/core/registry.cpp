#include "registry.h"

#include <string.h>

#include <vector>

namespace {
std::vector<const App*>& apps() {
    static std::vector<const App*> v;
    return v;
}
}  // namespace

void registerApp(const App* app) { registry::add(app); }

namespace registry {

void add(const App* app) { apps().push_back(app); }

size_t count() { return apps().size(); }

const App* at(size_t i) { return i < apps().size() ? apps()[i] : nullptr; }

const App* find(const char* id) {
    for (auto* a : apps()) {
        if (strcmp(a->id, id) == 0) return a;
    }
    return nullptr;
}

}  // namespace registry
