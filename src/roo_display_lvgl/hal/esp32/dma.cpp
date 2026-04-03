#include "roo_display_lvgl/hal/esp32/dma.h"

#if defined(ESP_PLATFORM)

#include "esp_heap_caps.h"

namespace roo_display {

void* dma_alloc(size_t size) {
#ifdef ROO_TESTING
  return malloc(size);
#else
  return heap_caps_aligned_calloc(
      32, 1, size,
      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_CACHE_ALIGNED);
#endif
}

void dma_free(void* ptr) { heap_caps_free(ptr); }

}  // namespace roo_display

#endif
