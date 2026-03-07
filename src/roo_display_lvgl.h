/// @file roo_display_lvgl.h
/// @brief Public API surface for roo_display_lvgl display, touch, and drawing
/// utilities.
#pragma once

#include "lvgl.h"
#include "roo_display.h"
#include "roo_display/core/device.h"
#include "roo_display/core/streamable.h"
#include "roo_display/products/combo_device.h"
#include "roo_display/touch/calibration.h"
#include "roo_display_lvgl/font.h"

namespace roo_display {

/// @brief Wrapper providing calibrated touch input for a display device.
class LvglTouchDisplay {
public:
  /// Constructs a touch display wrapper.
  LvglTouchDisplay(DisplayDevice &display_device, TouchDevice &touch_device,
                   TouchCalibration touch_calibration = TouchCalibration())
      : display_device_(display_device), touch_device_(touch_device),
        touch_calibration_(touch_calibration) {}

  /// Initializes the touch device.
  void init() { touch_device_.initTouch(); }

  /// Returns calibrated touch points in display coordinates.
  TouchResult getTouch(TouchPoint *points, int max_points);

  /// Returns raw touch points in absolute 0-4095 coordinates.
  TouchResult getRawTouch(TouchPoint *points, int max_points) {
    return touch_device_.getTouch(points, max_points);
  }

  /// Sets the touch calibration mapping.
  void setCalibration(TouchCalibration touch_calibration) {
    touch_calibration_ = touch_calibration;
  }

  /// Returns the current touch calibration.
  const TouchCalibration &calibration() const { return touch_calibration_; }

private:
  DisplayDevice &display_device_;
  TouchDevice &touch_device_;
  TouchCalibration touch_calibration_;

  int16_t raw_touch_x_;
  int16_t raw_touch_y_;
  int16_t raw_touch_z_;
};

/// @brief Display facade that owns a display device and optional touch input.
class LvglDisplay {
public:
  /// Constructs a display without touch support.
  LvglDisplay(DisplayDevice &display_device)
      : LvglDisplay(display_device, nullptr, TouchCalibration()) {}

  /// Constructs a display with touch support and optional calibration.
  LvglDisplay(DisplayDevice &display_device, TouchDevice &touch_device,
              TouchCalibration touch_calibration = TouchCalibration())
      : LvglDisplay(display_device, &touch_device, touch_calibration) {}

  /// Constructs a display from a combo device.
  LvglDisplay(ComboDevice &device)
      : LvglDisplay(device.display(), device.touch(),
                    device.touch_calibration()) {}

  /// Returns the total pixel area of the raw device.
  int32_t area() const {
    return display_device_.raw_width() * display_device_.raw_height();
  }

  /// Returns the current extents used by this display.
  const Box &extents() const { return extents_; }

  /// Returns the width in pixels of the current extents.
  int16_t width() const { return extents_.width(); }

  /// Returns the height in pixels of the current extents.
  int16_t height() const { return extents_.height(); }

  /// Initializes the devices, and the LVGL library.
  void init();

  /// Sets the display orientation.
  void setOrientation(Orientation orientation);

  /// Returns the current orientation.
  Orientation orientation() const { return orientation_; }

  /// Returns the touch calibration for the display.
  const TouchCalibration &touchCalibration() const {
    return touch_.calibration();
  }

  /// Returns mutable access to the display output.
  DisplayOutput &output() { return *output_; }
  /// Returns const access to the display output.
  const DisplayOutput &output() const { return *output_; }

  /// Returns calibrated touch points in display coordinates.
  ///
  /// If no touch has been registered, returns {.touch_points = 0} and does not
  /// modify `points`. If k touch points have been registered, sets up to
  /// max_points entries in `points`, and returns {.touch_points = k}. In both
  /// cases, the returned timestamp specifies the detection time.
  TouchResult getTouch(TouchPoint *points, int max_points) {
    return touch_.getTouch(points, max_points);
  }

  /// Returns raw touch points in absolute coordinates (0-4095).
  TouchResult getRawTouch(TouchPoint *points, int max_points) {
    return touch_.getRawTouch(points, max_points);
  }

  /// Returns true and sets (x, y) if touched; otherwise returns false.
  bool getTouch(int16_t &x, int16_t &y);

  /// Sets the touch calibration mapping.
  void setTouchCalibration(TouchCalibration touch_calibration) {
    touch_.setCalibration(touch_calibration);
  }

  void enableTurbo();

  void disableTurbo();

  bool isTurboEnabled() const { return turbo_ != nullptr; }

private:
  LvglDisplay(DisplayDevice &display_device, TouchDevice *touch_device,
              TouchCalibration touch_calibration);

  friend void flushAreaCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
  friend void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data);

  void flush(const lv_area_t *area, uint8_t *px_map);

  DisplayDevice &display_device_;
  std::unique_ptr<BackgroundFillOptimizer::FrameBuffer> turbo_frame_buffer_;
  std::unique_ptr<BackgroundFillOptimizer> turbo_;
  // Set to either the display_device_ or the turbo wrapper.
  DisplayOutput *output_;
  TouchDisplay touch_;
  Orientation orientation_;

  Box extents_;

  size_t framebuffer_size_;
  std::unique_ptr<roo::byte[]> framebuffer_;
  lv_display_t *lv_display_;
};

} // namespace roo_display
