#pragma once

#include <cstddef>

namespace roo_display {

void* dma_alloc(size_t size);
void dma_free(void* ptr);

}  // namespace roo_display
