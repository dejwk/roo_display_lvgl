#pragma once

#include "roo_display/hal/config.h"

#if defined(ESP_PLATFORM)

#include "roo_display_lvgl/hal/esp32/dma.h"

#else

#include "roo_display_lvgl/hal/default/dma.h"

#endif
