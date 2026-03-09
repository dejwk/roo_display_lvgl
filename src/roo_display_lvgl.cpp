#include "roo_display_lvgl.h"

#include "roo_collections.h"
#include "roo_collections/flat_small_hash_map.h"

// #include <cmath>

// #include "roo_display/filter/color_filter.h"

namespace roo_display {

namespace {

roo_collections::FlatSmallHashMap<lv_display_t *, LvglDisplay *> display_map;
roo_collections::FlatSmallHashMap<lv_indev_t *, LvglDisplay *> indev_map;

static inline uint32_t tick(void) { return roo_time::Uptime::Now().inMillis(); }

}  // namespace

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
      output_(&display_device_),
      touch_(display_device,
             touch_device == nullptr ? dummy_touch : *touch_device,
             touch_calibration),
      orientation_(display_device.orientation()),
      extents_(0, 0, display_device.effective_width() - 1,
               display_device.effective_height() - 1),
      framebuffer_size_(display_device.raw_width() *
                        display_device.raw_height() * 2 / 10),
      framebuffer_(nullptr),
      lv_display_(nullptr) {}

void LvglDisplay::setOrientation(Orientation orientation) {
  if (orientation_ == orientation) return;
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
  turbo_frame_buffer_ = std::make_unique<BackgroundFillOptimizer::FrameBuffer>(
      display_device_.effective_width(), display_device_.effective_height());
  turbo_ = std::make_unique<BackgroundFillOptimizer>(display_device_,
                                                     *turbo_frame_buffer_);
  output_ = turbo_.get();
}

void LvglDisplay::disableTurbo() {
  output_ = &display_device_;
  turbo_.reset();
  turbo_frame_buffer_.reset();
}

void LvglDisplay::init() {
  display_device_.init();
  extents_ = Box(0, 0, display_device_.effective_width() - 1,
                 display_device_.effective_height() - 1);
  touch_.init();
  lv_init();
  lv_tick_set_cb(tick);
  lv_display_ = lv_display_create(display_device_.effective_width(),
                                  display_device_.effective_height());
  display_map.insert({lv_display_, this});
  LOG(INFO) << "Allocating framebuffer of size " << framebuffer_size_
            << " bytes";
  framebuffer_.reset(new roo::byte[framebuffer_size_]);
  LOG(INFO) << "Framebuffer allocated at "
            << static_cast<void *>(framebuffer_.get());
  lv_display_set_buffers(lv_display_, framebuffer_.get(), nullptr,
                         framebuffer_size_, LV_DISPLAY_RENDER_MODE_PARTIAL);

  LOG(INFO) << "Buffers set";
  lv_display_set_flush_cb(lv_display_, flushAreaCb);
  LOG(INFO) << "Flush callback set";

  lv_indev_t *indev = lv_indev_create();           /* Create input device */
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER); /* Set the device type */
  indev_map.insert({indev, this});
  lv_indev_set_read_cb(indev, touchReadCb); /* Set the read callback */
  LOG(INFO) << "Input device set";
}

void LvglDisplay::flush(const lv_area_t *area, uint8_t *px_map) {
  lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
  display_device_.begin();
  // device.display().fillRect(10, 10, 50, 50, roo_display::color::Red);
  DCHECK_GE(area->x1, 0);
  DCHECK_LT(area->x1 + (area->x2 - area->x1),
            display_device_.effective_width());
  DCHECK_GE(area->y1, 0);
  DCHECK_LT(area->y1 + (area->y2 - area->y1),
            display_device_.effective_height());

  output_->drawDirectRect(
      (const roo::byte *)px_map, 2 * (area->x2 - area->x1 + 1), 0, 0,
      area->x2 - area->x1, area->y2 - area->y1, area->x1, area->y1);
  display_device_.end();

  lv_display_flush_ready(lv_display_);
}

}  // namespace roo_display
