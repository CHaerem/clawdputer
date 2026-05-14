#pragma once

#include <stddef.h>

#include "app.h"

namespace registry {

void           add(const App* app);
size_t         count();
const App*     at(size_t i);
const App*     find(const char* id);

}  // namespace registry
