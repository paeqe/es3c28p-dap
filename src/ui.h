#pragma once
#include <lvgl.h>

namespace ui {

void begin();       // builds both screens, shows the browser
void tick();        // refresh dynamic labels; call from loop()

} // namespace ui
