#include "roo_display_lvgl.h"

#include "roo_collections.h"
#include "roo_collections/flat_small_hash_map.h"
#include "roo_display_lvgl/hal/dma.h"

namespace roo_display {

namespace {

roo_collections::FlatSmallHashMap<lv_display_t *, LvglDisplay *> display_map;
roo_collections::FlatSmallHashMap<lv_indev_t *, LvglDisplay *> indev_map;

static inline uint32_t tick(void) { return roo_time::Uptime::Now().inMillis(); }

// Default roo_threads FreeRTOS priority is 1. Rendering often needs higher
// scheduling precedence to avoid visible scanout jitter.
constexpr uint16_t kRenderThreadPriority = 3;
constexpr uint32_t kRenderThreadStackSize = 4096;

static roo_time::Uptime last_flush_start = roo_time::Uptime::Start();
static roo_time::Uptime last_flush_end = roo_time::Uptime::Start();

}  // namespace

static void my_rounder_cb(lv_event_t *e) {
  lv_area_t *area = lv_event_get_invalidated_area(e);
  if (area->x2 - area->x1 < 32) {
    // Don't round small areas, to avoid excessive overdraw.
    return;
  }
  // Align memory regions for DMA eligibility.
  area->x1 = (area->x1 & ~0xF);
  area->x2 = (area->x2 | 0xF);
}

void flushAreaCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  auto it = display_map.find(disp);
  assert(it != display_map.end());
  LvglDisplay *display = it->second;
  display->flush(area, px_map);
}

void touchReadCb(lv_indev_t *indev, lv_indev_data_t *data) {
  auto it = indev_map.find(indev);
  assert(it != indev_map.end());
  LvglDisplay *display = it->second;
  int16_t touchpad_x, touchpad_y;
  if (display->getTouch(touchpad_x, touchpad_y)) {
    data->point.x = touchpad_x;
    data->point.y = touchpad_y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

TouchResult LvglTouchDisplay::getTouch(TouchPoint *points, int max_points) {
  TouchResult result = getRawTouch(points, max_points);
  if (result.touch_points > 0) {
    Orientation orientation = display_device_.orientation();
    for (int i = 0; i < result.touch_points && i < max_points; ++i) {
      TouchPoint &p = points[i];
      touch_calibration_.Calibrate(p);
      if (orientation.isRightToLeft()) {
        p.x = 4095 - p.x;
        p.vx = -p.vx;
      }
      if (orientation.isBottomToTop()) {
        p.y = 4095 - p.y;
        p.vy = -p.vy;
      }
      if (orientation.isXYswapped()) {
        std::swap(p.x, p.y);
        std::swap(p.vx, p.vy);
      }
      p.x = ((int32_t)p.x * (display_device_.effective_width() - 1)) / 4095;
      p.y = ((int32_t)p.y * (display_device_.effective_height() - 1)) / 4095;
      p.vx = ((int32_t)p.vx * (display_device_.effective_width() - 1)) / 4095;
      p.vy = ((int32_t)p.vy * (display_device_.effective_height() - 1)) / 4095;
    }
  }
  return result;
}

bool LvglDisplay::getTouch(int16_t &x, int16_t &y) {
  TouchPoint point;
  TouchResult result = touch_.getTouch(&point, 1);
  if (result.touch_points == 0) {
    return false;
  }
  x = point.x;
  y = point.y;
  return true;
}

class DummyTouch : public TouchDevice {
 public:
  void initTouch() override {}

  TouchResult getTouch(TouchPoint *points, int max_points) override {
    return TouchResult(roo_time::Uptime::Now(), 0);
  }
};

static DummyTouch dummy_touch;

LvglDisplay::LvglDisplay(DisplayDevice &display_device,
                         TouchDevice *touch_device,
                         TouchCalibration touch_calibration)
    : display_device_(display_device),
      byteswap_(false),  // Set in init().
      output_(&display_device_),
      touch_(display_device,
             touch_device == nullptr ? dummy_touch : *touch_device,
             touch_calibration),
      orientation_(display_device.orientation()),
      extents_(0, 0, display_device.effective_width() - 1,
               display_device.effective_height() - 1),
      framebuffer_(nullptr),
      lv_display_(nullptr),
      lv_indev_(nullptr),
      render_stop_(false),
      render_busy_(false),
      pending_render_{{0, 0, 0, 0}, nullptr, 0, false} {}

LvglDisplay::~LvglDisplay() {
  stopRenderWorker();
  if (lv_display_ != nullptr) {
    display_map.erase(lv_display_);
    lv_display_ = nullptr;
  }
  if (lv_indev_ != nullptr) {
    indev_map.erase(lv_indev_);
    lv_indev_ = nullptr;
  }
  if (framebuffer_ != nullptr) {
    dma_free(framebuffer_);
    framebuffer_ = nullptr;
  }
}

void LvglDisplay::waitForRenderIdle() {
  roo::unique_lock<roo::mutex> lock(render_mutex_);
  render_cv_.wait(lock,
                  [this]() { return !pending_render_.valid && !render_busy_; });
}

void LvglDisplay::stopRenderWorker() {
  {
    roo::lock_guard<roo::mutex> lock(render_mutex_);
    render_stop_ = true;
  }
  render_cv_.notify_all();
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
}

void LvglDisplay::startRenderWorker() {
#if defined(ROO_THREADS_USE_FREERTOS)
  roo::thread::attributes attrs;
  attrs.set_name("lvgl-rnd");
  attrs.set_priority(kRenderThreadPriority);
  attrs.set_stack_size(kRenderThreadStackSize);
  render_thread_ = roo::thread(attrs, [this]() { renderLoop(); });
#else
  render_thread_ = roo::thread([this]() { renderLoop(); });
#endif
}

void LvglDisplay::renderLoop() {
  while (true) {
    RenderRequest req;
    {
      roo::unique_lock<roo::mutex> lock(render_mutex_);
      render_cv_.wait(
          lock, [this]() { return render_stop_ || pending_render_.valid; });
      if (render_stop_ && !pending_render_.valid) {
        break;
      }
      req = pending_render_;
      pending_render_.valid = false;
      render_busy_ = true;
    }

    display_device_.begin();
    output_->drawDirectRect((const roo::byte *)req.px_map, req.row_width_bytes,
                            0, 0, req.area.x2 - req.area.x1,
                            req.area.y2 - req.area.y1, req.area.x1,
                            req.area.y1);
    display_device_.end();
    // #if LV_USE_OS != LV_OS_NONE
    lv_display_flush_ready(lv_display_);
    // #endif

    last_flush_end = roo_time::Uptime::Now();
    {
      roo::lock_guard<roo::mutex> lock(render_mutex_);
      render_busy_ = false;
    }
    render_cv_.notify_all();
  }
}

void LvglDisplay::setOrientation(Orientation orientation) {
  if (orientation_ == orientation) return;

  waitForRenderIdle();
  orientation_ = orientation;
  display_device_.begin();
  display_device_.setOrientation(orientation);
  display_device_.end();
  extents_ = Box(0, 0, display_device_.effective_width() - 1,
                 display_device_.effective_height() - 1);
  if (turbo_frame_buffer_ != nullptr) {
    turbo_frame_buffer_->setSwapXY(orientation_.isXYswapped());
    turbo_frame_buffer_->invalidate();
  }
}

void LvglDisplay::enableTurbo() {
  if (turbo_ != nullptr) return;

  waitForRenderIdle();
  turbo_frame_buffer_ = std::make_unique<BackgroundFillOptimizer::FrameBuffer>(
      display_device_.effective_width(), display_device_.effective_height());
  turbo_ = std::make_unique<BackgroundFillOptimizer>(display_device_,
                                                     *turbo_frame_buffer_);
  output_ = turbo_.get();
}

void LvglDisplay::disableTurbo() {
  waitForRenderIdle();
  output_ = &display_device_;
  turbo_.reset();
  turbo_frame_buffer_.reset();
}

void LvglDisplay::init() {
  stopRenderWorker();
  if (lv_display_ != nullptr) {
    display_map.erase(lv_display_);
    lv_display_ = nullptr;
  }
  if (lv_indev_ != nullptr) {
    indev_map.erase(lv_indev_);
    lv_indev_ = nullptr;
  }

  display_device_.init();
  extents_ = Box(0, 0, display_device_.effective_width() - 1,
                 display_device_.effective_height() - 1);
  touch_.init();
  lv_init();
  lv_tick_set_cb(tick);
  lv_display_ = lv_display_create(display_device_.effective_width(),
                                  display_device_.effective_height());
  const roo_display::DisplayDevice::ColorFormat &device_format =
      display_device_.getColorFormat();
  int bytes_per_pixel = 0;
  switch (device_format.mode()) {
    case DisplayOutput::ColorFormat::kModeRgb565: {
      if (device_format.byte_order() == roo_io::ByteOrder::kNativeEndian) {
        lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_RGB565);
      } else if (LV_DRAW_SW_SUPPORT_RGB565_SWAPPED) {
        lv_display_set_color_format(lv_display_,
                                    LV_COLOR_FORMAT_RGB565_SWAPPED);
      } else {
        LOG(WARNING)
            << "Display color format byte order is not native, but RGB565 byte "
               "swapping is not enabled. Software byte swapping will be "
               "performed. To enable RGB565 native byte swapping, define "
               "LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=1.";
        lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_RGB565);
        byteswap_ = true;
      }
      bytes_per_pixel = 2;
      break;
    }
    case DisplayOutput::ColorFormat::kModeRgb888: {
      lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_RGB888);
      bytes_per_pixel = 3;
      break;
    }
    case DisplayOutput::ColorFormat::kModeGrayscale8: {
      lv_display_set_color_format(lv_display_, LV_COLOR_FORMAT_I8);
      bytes_per_pixel = 1;
      break;
    }
    default: {
      LOG(FATAL) << "Unsupported display color format";
    }
  }
  display_map.insert({lv_display_, this});
  if (framebuffer_ != nullptr) {
    dma_free(framebuffer_);
  }
  size_t framebuffer_size = display_device_.raw_width() *
                            display_device_.raw_height() * bytes_per_pixel / 10;

  framebuffer_ =
      CHECK_NOTNULL(static_cast<roo::byte *>(dma_alloc(framebuffer_size)));

  lv_display_set_buffers(lv_display_, framebuffer_, nullptr, framebuffer_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_display_set_flush_cb(lv_display_, flushAreaCb);
  lv_display_add_event_cb(lv_display_, my_rounder_cb, LV_EVENT_INVALIDATE_AREA,
                          lv_display_);

  lv_indev_ = lv_indev_create();                       /* Create input device */
  lv_indev_set_type(lv_indev_, LV_INDEV_TYPE_POINTER); /* Set the device type */
  indev_map.insert({lv_indev_, this});
  lv_indev_set_read_cb(lv_indev_, touchReadCb); /* Set the read callback */

  {
    roo::lock_guard<roo::mutex> lock(render_mutex_);
    render_stop_ = false;
    render_busy_ = false;
    pending_render_.valid = false;
    pending_render_.px_map = nullptr;
    pending_render_.row_width_bytes = 0;
  }
  // #if LV_USE_OS != LV_OS_NONE
  startRenderWorker();
  // #endif
}

void LvglDisplay::flush(const lv_area_t *area, uint8_t *px_map) {
  if (byteswap_) {
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
  }
  DCHECK_GE(area->x1, 0);
  DCHECK_LT(area->x1 + (area->x2 - area->x1),
            display_device_.effective_width());
  DCHECK_GE(area->y1, 0);
  DCHECK_LT(area->y1 + (area->y2 - area->y1),
            display_device_.effective_height());

  // LOG(INFO) << "Last flush: " << (last_flush_end -
  // last_flush_start).inMicros();
  last_flush_start = roo_time::Uptime::Now();
  // LOG(INFO) << "Gap:        " << (last_flush_start -
  // last_flush_end).inMicros();

  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t row_width_bytes = 2 * width;

  {
    roo::unique_lock<roo::mutex> lock(render_mutex_);
    render_cv_.wait(lock, [this]() {
      return render_stop_ || (!pending_render_.valid && !render_busy_);
    });
    // if (render_stop_) {
    //   lv_display_flush_ready(lv_display_);
    //   return;
    // }
    pending_render_.area = *area;
    pending_render_.px_map = px_map;
    pending_render_.row_width_bytes = row_width_bytes;
    pending_render_.valid = true;
    render_cv_.notify_all();
  }

  // display_device_.begin();

  // output_->drawDirectRectAsync(
  //     (const roo::byte *)px_map, 2 * (area->x2 - area->x1 + 1), 0, 0,
  //     area->x2 - area->x1, area->y2 - area->y1, area->x1, area->y1, [this]()
  //     {
  //       display_device_.end();
  //       last_flush_end = roo_time::Uptime::Now();
  //       lv_display_flush_ready(lv_display_);
  //     });
}

}  // namespace roo_display
