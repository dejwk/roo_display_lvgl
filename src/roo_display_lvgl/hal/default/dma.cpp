#include "roo_display_lvgl/hal/default/dma.h"

#include <cstdlib>

#include "roo_display/hal/config.h"

#if !defined(ESP_PLATFORM)

namespace roo_display {

void* dma_alloc(size_t size) { return std::malloc(size); }

void dma_free(void* ptr) { std::free(ptr); }

}  // namespace roo_display

#endif
