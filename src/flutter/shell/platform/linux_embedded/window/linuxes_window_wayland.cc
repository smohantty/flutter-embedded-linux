// Copyright 2021 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/linuxes_window_wayland.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <cassert>
#include <unordered_map>

#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/surface/context_egl_wayland.h"

static constexpr char kWlCompositor[] = "wl_compositor";
static constexpr char kWlShell[] = "wl_shell";
static constexpr char kWlSeat[] = "wl_seat";
static constexpr char kWlOutput[] = "wl_output";
static constexpr char kWlShm[] = "wl_shm";
static constexpr char kWestonDesktopShell[] = "weston_desktop_shell";
static constexpr char kZwpTextInputManagerV1[] = "zwp_text_input_manager_v1";
static constexpr char kWlDataDeviceManager[] = "wl_data_device_manager";

static constexpr char kWlCursorThemeBottomLeftCorner[] = "bottom_left_corner";
static constexpr char kWlCursorThemeBottomRightCorner[] = "bottom_right_corner";
static constexpr char kWlCursorThemeBottomSide[] = "bottom_side";
static constexpr char kWlCursorThemeGrabbing[] = "grabbing";
static constexpr char kWlCursorThemeLeftPtr[] = "left_ptr";
static constexpr char kWlCursorThemeLeftSide[] = "left_side";
static constexpr char kWlCursorThemeRightSide[] = "right_side";
static constexpr char kWlCursorThemeTopLeftCorner[] = "top_left_corner";
static constexpr char kWlCursorThemeTopRightCorner[] = "top_right_corner";
static constexpr char kWlCursorThemeTopSide[] = "top_side";
static constexpr char kWlCursorThemeXterm[] = "xterm";
static constexpr char kWlCursorThemeHand1[] = "hand1";
static constexpr char kWlCursorThemeWatch[] = "watch";
static constexpr char kCursorNameNone[] = "none";

static constexpr char kClipboardMimeTypeText[] = "text/plain";

namespace flutter {

const wl_registry_listener LinuxesWindowWayland::kWlRegistryListener = {
    .global =
        [](void* data, wl_registry* wl_registry, uint32_t name,
           const char* interface, uint32_t version) {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          self->WlRegistryHandler(wl_registry, name, interface, version);
        },
    .global_remove =
        [](void* data, wl_registry* wl_registry, uint32_t name) {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          self->WlUnRegistryHandler(wl_registry, name);
        },
};

const wl_shell_surface_listener LinuxesWindowWayland::kWlShellSurfaceListener =
    {
        .ping = [](void* data, struct wl_shell_surface* wl_shell_surface,
                   uint32_t serial) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          wl_shell_surface_pong(self->wl_shell_surface_, serial);
        },
        .configure = [](void* data, struct wl_shell_surface* wl_shell_surface,
                        uint32_t edges, int32_t width, int32_t height) -> void {
          LINUXES_LOG(ERROR) << "todo: Not supprpoted now.";
        },
        .popup_done = [](void* data,
                         struct wl_shell_surface* wl_shell_surface) -> void {},
};

const wl_seat_listener LinuxesWindowWayland::kWlSeatListener = {
    .capabilities = [](void* data, wl_seat* seat, uint32_t caps) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);

      if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->wl_pointer_) {
        self->wl_pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self->wl_pointer_, &kWlPointerListener, self);
      } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && self->wl_pointer_) {
        wl_pointer_destroy(self->wl_pointer_);
        self->wl_pointer_ = nullptr;
      }

      if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !self->wl_touch_) {
        self->wl_touch_ = wl_seat_get_touch(seat);
        wl_touch_add_listener(self->wl_touch_, &kWlTouchListener, self);
      } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && self->wl_touch_) {
        wl_touch_destroy(self->wl_touch_);
        self->wl_touch_ = nullptr;
      }

      if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->wl_keyboard_) {
        self->wl_keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(self->wl_keyboard_, &kWlKeyboardListener,
                                 self);
      } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && self->wl_keyboard_) {
        wl_keyboard_destroy(self->wl_keyboard_);
        self->wl_keyboard_ = nullptr;
      }
    },
};

const wl_pointer_listener LinuxesWindowWayland::kWlPointerListener = {
    .enter = [](void* data, struct wl_pointer* wl_pointer, uint32_t serial,
                struct wl_surface* surface, wl_fixed_t surface_x,
                wl_fixed_t surface_y) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->show_cursor_) {
        self->cursor_info_.wl_pointer = wl_pointer;
        self->cursor_info_.serial = serial;
      }

      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnPointerMove(x, y);
        self->pointer_x_ = x;
        self->pointer_y_ = y;
      }
    },
    .leave = [](void* data, struct wl_pointer* pointer, uint32_t serial,
                wl_surface* surface) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnPointerLeave();
        self->pointer_x_ = -1;
        self->pointer_y_ = -1;
      }
    },
    .motion = [](void* data, struct wl_pointer* pointer, uint32_t time,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnPointerMove(x, y);
        self->pointer_x_ = x;
        self->pointer_y_ = y;
      }
    },
    .button = [](void* data, struct wl_pointer* pointer, uint32_t serial,
                 uint32_t time, uint32_t button, uint32_t status) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        FlutterPointerMouseButtons flutter_button;
        switch (button) {
          case BTN_LEFT:
            flutter_button = kFlutterPointerButtonMousePrimary;
            break;
          case BTN_RIGHT:
            flutter_button = kFlutterPointerButtonMouseSecondary;
            break;
          case BTN_MIDDLE:
            flutter_button = kFlutterPointerButtonMouseMiddle;
            break;
          case BTN_BACK:
            flutter_button = kFlutterPointerButtonMouseBack;
            break;
          case BTN_FORWARD:
            flutter_button = kFlutterPointerButtonMouseForward;
            break;
          default:
            LINUXES_LOG(ERROR) << "Not expected button input: " << button;
            return;
        }

        if (status == WL_POINTER_BUTTON_STATE_PRESSED) {
          self->binding_handler_delegate_->OnPointerDown(
              self->pointer_x_, self->pointer_y_, flutter_button);
        } else {
          self->binding_handler_delegate_->OnPointerUp(
              self->pointer_x_, self->pointer_y_, flutter_button);
        }
      }
    },
    .axis = [](void* data, struct wl_pointer* wl_pointer, uint32_t time,
               uint32_t axis, wl_fixed_t value) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double delta = wl_fixed_to_double(value);
        constexpr int32_t kScrollOffsetMultiplier = 20;
        self->binding_handler_delegate_->OnScroll(
            self->pointer_x_, self->pointer_y_,
            axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : delta,
            axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? delta : 0,
            kScrollOffsetMultiplier);
      }
    },
};  // namespace flutter

const wl_touch_listener LinuxesWindowWayland::kWlTouchListener = {
    .down = [](void* data, struct wl_touch* wl_touch, uint32_t serial,
               uint32_t time, struct wl_surface* surface, int32_t id,
               wl_fixed_t surface_x, wl_fixed_t surface_y) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnTouchDown(time, id, x, y);
      }
    },
    .up = [](void* data, struct wl_touch* wl_touch, uint32_t serial,
             uint32_t time, int32_t id) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnTouchUp(time, id);
      }
    },
    .motion = [](void* data, struct wl_touch* wl_touch, uint32_t time,
                 int32_t id, wl_fixed_t surface_x,
                 wl_fixed_t surface_y) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnTouchMotion(time, id, x, y);
      }
    },
    .frame = [](void* data, struct wl_touch* wl_touch) -> void {},
    .cancel = [](void* data, struct wl_touch* wl_touch) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnTouchCancel();
      }
    },
};

const wl_keyboard_listener LinuxesWindowWayland::kWlKeyboardListener = {
    .keymap = [](void* data, wl_keyboard* wl_keyboard, uint32_t format, int fd,
                 uint32_t size) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKeyMap(format, fd, size);
      }
    },
    .enter = [](void* data, wl_keyboard* wl_keyboard, uint32_t serial,
                wl_surface* surface, wl_array* keys) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
    },
    .leave = [](void* data, wl_keyboard* wl_keyboard, uint32_t serial,
                wl_surface* surface) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
    },
    .key = [](void* data, wl_keyboard* wl_keyboard, uint32_t serial,
              uint32_t time, uint32_t key, uint32_t state) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKey(
            key, (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                     ? FLUTTER_LINUXES_BUTTON_DOWN
                     : FLUTTER_LINUXES_BUTTON_UP);
      }
    },
    .modifiers = [](void* data, wl_keyboard* wl_keyboard, uint32_t serial,
                    uint32_t mods_depressed, uint32_t mods_latched,
                    uint32_t mods_locked, uint32_t group) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKeyModifiers(
            mods_depressed, mods_latched, mods_locked, group);
      }
    },
    .repeat_info = [](void* data, wl_keyboard* wl_keyboard, int rate,
                      int delay) -> void {},
};

const wl_output_listener LinuxesWindowWayland::kWlOutputListener = {
    .geometry = [](void* data, struct wl_output* wl_output, int32_t x,
                   int32_t y, int32_t physical_width, int32_t physical_height,
                   int32_t subpixel, const char* make, const char* model,
                   int32_t output_transform) -> void {},
    .mode = [](void* data, struct wl_output* wl_output, uint32_t flags,
               int32_t width, int32_t height, int32_t refresh) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (flags & WL_OUTPUT_MODE_CURRENT) {
        LINUXES_LOG(INFO) << "Display output resolution: " << width << "x"
                          << height;
        if (self->window_mode_ == FlutterWindowMode::kFullscreen) {
          self->current_width_ = width;
          self->current_height_ = height;
          if (self->binding_handler_delegate_) {
            self->binding_handler_delegate_->OnWindowSizeChanged(width, height);
          }
        }
      }
    },
    .done = [](void* data, struct wl_output* wl_output) -> void {},
    .scale = [](void* data, struct wl_output* wl_output,
                int32_t scale) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      LINUXES_LOG(INFO) << "Display output scale: " << scale;
      self->current_scale_ = scale;
    },
};

const zwp_text_input_v1_listener LinuxesWindowWayland::kZwpTextInputV1Listener =
    {
        .enter = [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
                    struct wl_surface* surface) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .leave = [](void* data,
                    struct zwp_text_input_v1* zwp_text_input_v1) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_hide_input_panel(self->zwp_text_input_v1_);
          }
        },
        .modifiers_map = [](void* data,
                            struct zwp_text_input_v1* zwp_text_input_v1,
                            struct wl_array* map) -> void {},
        .input_panel_state = [](void* data,
                                struct zwp_text_input_v1* zwp_text_input_v1,
                                uint32_t state) -> void {},
        .preedit_string =
            [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
               uint32_t serial, const char* text, const char* commit) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          if (self->binding_handler_delegate_ && strlen(text)) {
            self->binding_handler_delegate_->OnVirtualKey(text[0]);
          }
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_reset(self->zwp_text_input_v1_);
            // If there is no input data, the backspace key cannot be used,
            // so set dummy data.
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .preedit_styling =
            [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
               uint32_t index, uint32_t length, uint32_t style) -> void {},
        .preedit_cursor = [](void* data,
                             struct zwp_text_input_v1* zwp_text_input_v1,
                             int32_t index) -> void {},
        .commit_string = [](void* data,
                            struct zwp_text_input_v1* zwp_text_input_v1,
                            uint32_t serial, const char* text) -> void {
          // commit_string is notified only when the space key is pressed.
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          if (self->binding_handler_delegate_ && strlen(text)) {
            self->binding_handler_delegate_->OnVirtualKey(text[0]);
          }
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .cursor_position = [](void* data,
                              struct zwp_text_input_v1* zwp_text_input_v1,
                              int32_t index, int32_t anchor) -> void {},
        .delete_surrounding_text =
            [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
               int32_t index, uint32_t length) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          if (self->binding_handler_delegate_) {
            self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_BACKSPACE);
          }
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .keysym = [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
                     uint32_t serial, uint32_t time, uint32_t sym,
                     uint32_t state, uint32_t modifiers) -> void {
          auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
          if ((state == WL_KEYBOARD_KEY_STATE_PRESSED) &&
              (self->binding_handler_delegate_)) {
            switch (sym) {
              case XKB_KEY_Left:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_LEFT);
                break;
              case XKB_KEY_Right:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_RIGHT);
                break;
              case XKB_KEY_Up:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_UP);
                break;
              case XKB_KEY_Down:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_DOWN);
                break;
              case XKB_KEY_Tab:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_TAB);
                break;
              case XKB_KEY_Return:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_ENTER);
                break;
              default:
                break;
            }
          }
        },
        .language = [](void* data, struct zwp_text_input_v1* zwp_text_input_v1,
                       uint32_t serial, const char* language) -> void {},
        .text_direction = [](void* data,
                             struct zwp_text_input_v1* zwp_text_input_v1,
                             uint32_t serial, uint32_t direction) -> void {},
};

const wl_data_device_listener LinuxesWindowWayland::kWlDataDeviceListener = {
    .data_offer = [](void* data, struct wl_data_device* wl_data_device,
                     struct wl_data_offer* offer) -> void {},
    .enter = [](void* data, struct wl_data_device* wl_data_device,
                uint32_t serial, struct wl_surface* surface, wl_fixed_t x,
                wl_fixed_t y, struct wl_data_offer* offer) -> void {},
    .leave = [](void* data, struct wl_data_device* wl_data_device) -> void {},
    .motion = [](void* data, struct wl_data_device* wl_data_device,
                 uint32_t time, wl_fixed_t x, wl_fixed_t y) -> void {},
    .drop = [](void* data, struct wl_data_device* wl_data_device) -> void {},
    .selection = [](void* data, struct wl_data_device* wl_data_device,
                    struct wl_data_offer* offer) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      if (self->wl_data_offer_) {
        wl_data_offer_destroy(self->wl_data_offer_);
      }
      self->wl_data_offer_ = offer;
    },
};

const wl_data_source_listener LinuxesWindowWayland::kWlDataSourceListener = {
    .target = [](void* data, struct wl_data_source* wl_data_source,
                 const char* mime_type) -> void {},
    .send = [](void* data, struct wl_data_source* wl_data_source,
               const char* mime_type, int32_t fd) -> void {
      if (strcmp(mime_type, kClipboardMimeTypeText)) {
        LINUXES_LOG(ERROR) << "Not expected mime_type: " << mime_type;
        return;
      }
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      // Write the copied data to the clipboard.
      write(fd, self->clipboard_data_.c_str(),
            strlen(self->clipboard_data_.c_str()));
      close(fd);
    },
    .cancelled = [](void* data, struct wl_data_source* wl_data_source) -> void {
      auto self = reinterpret_cast<LinuxesWindowWayland*>(data);
      self->clipboard_data_ = "";
      if (self->wl_data_source_) {
        wl_data_source_destroy(self->wl_data_source_);
        self->wl_data_source_ = nullptr;
      }
    },
    .dnd_drop_performed = [](void* data,
                             struct wl_data_source* wl_data_source) -> void {},
    .dnd_finished = [](void* data,
                       struct wl_data_source* wl_data_source) -> void {},
    .action = [](void* data, struct wl_data_source* wl_data_source,
                 uint32_t dnd_action) -> void {},
};

LinuxesWindowWayland::LinuxesWindowWayland(FlutterWindowMode window_mode,
                                           int32_t width, int32_t height,
                                           bool show_cursor)
    : cursor_info_({nullptr, 0, ""}),
      display_valid_(false),
      wl_pointer_(nullptr),
      wl_touch_(nullptr),
      wl_keyboard_(nullptr),
      zwp_text_input_manager_v1_(nullptr),
      zwp_text_input_v1_(nullptr),
      wl_shm_(nullptr),
      wl_cursor_theme_(nullptr),
      wl_data_device_manager_(nullptr),
      wl_data_device_(nullptr),
      wl_data_offer_(nullptr),
      wl_data_source_(nullptr),
      serial_(0) {
  window_mode_ = window_mode;
  current_width_ = width;
  current_height_ = height;
  show_cursor_ = show_cursor;

  wl_display_ = wl_display_connect(nullptr);
  if (!wl_display_) {
    LINUXES_LOG(ERROR) << "Failed to connect to the Wayland display.";
    return;
  }

  wl_registry_ = wl_display_get_registry(wl_display_);
  if (!wl_registry_) {
    LINUXES_LOG(ERROR) << "Failed to get the wayland registry.";
    return;
  }

  wl_registry_add_listener(wl_registry_, &kWlRegistryListener, this);
  wl_display_dispatch(wl_display_);
  wl_display_roundtrip(wl_display_);

  if (wl_data_device_manager_ && wl_seat_) {
    wl_data_device_ = wl_data_device_manager_get_data_device(
        wl_data_device_manager_, wl_seat_);
    wl_data_device_add_listener(wl_data_device_, &kWlDataDeviceListener, this);
  }

  if (weston_desktop_shell_) {
    weston_desktop_shell_desktop_ready(weston_desktop_shell_);
  }

  display_valid_ = true;
}

LinuxesWindowWayland::~LinuxesWindowWayland() {
  display_valid_ = false;

  if (weston_desktop_shell_) {
    weston_desktop_shell_destroy(weston_desktop_shell_);
    weston_desktop_shell_ = nullptr;
  }

  if (wl_cursor_theme_) {
    wl_cursor_theme_destroy(wl_cursor_theme_);
    wl_cursor_theme_ = nullptr;
  }

  if (zwp_text_input_v1_) {
    zwp_text_input_v1_destroy(zwp_text_input_v1_);
    zwp_text_input_v1_ = nullptr;
  }

  if (zwp_text_input_manager_v1_) {
    zwp_text_input_manager_v1_destroy(zwp_text_input_manager_v1_);
    zwp_text_input_manager_v1_ = nullptr;
  }

  if (wl_data_offer_) {
    wl_data_offer_destroy(wl_data_offer_);
    wl_data_offer_ = nullptr;
  }

  if (wl_data_source_) {
    wl_data_source_destroy(wl_data_source_);
    wl_data_source_ = nullptr;
  }

  if (wl_data_device_) {
    if (wl_data_device_manager_version_ >=
        WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(wl_data_device_);
    } else {
      wl_data_device_destroy(wl_data_device_);
    }
    wl_data_device_ = nullptr;
  }

  if (wl_data_device_manager_) {
    wl_data_device_manager_destroy(wl_data_device_manager_);
    wl_data_device_manager_ = nullptr;
  }

  if (wl_shm_) {
    wl_shm_destroy(wl_shm_);
    wl_shm_ = nullptr;
  }

  if (wl_shell_) {
    wl_shell_destroy(wl_shell_);
    wl_shell_ = nullptr;
  }

  if (wl_compositor_) {
    wl_compositor_destroy(wl_compositor_);
    wl_compositor_ = nullptr;
  }

  if (wl_registry_) {
    wl_registry_destroy(wl_registry_);
    wl_registry_ = nullptr;
  }

  if (wl_display_) {
    wl_display_flush(wl_display_);
    wl_display_disconnect(wl_display_);
    wl_display_ = nullptr;
  }
}

void LinuxesWindowWayland::SetView(WindowBindingHandlerDelegate* window) {
  binding_handler_delegate_ = window;
}

LinuxesRenderSurfaceTarget* LinuxesWindowWayland::GetRenderSurfaceTarget()
    const {
  return render_surface_.get();
}

double LinuxesWindowWayland::GetDpiScale() { return current_scale_; }

PhysicalWindowBounds LinuxesWindowWayland::GetPhysicalWindowBounds() {
  return {GetCurrentWidth(), GetCurrentHeight()};
}

bool LinuxesWindowWayland::DispatchEvent() {
  if (!IsValid()) {
    LINUXES_LOG(ERROR) << "Wayland display is invalid.";
    return false;
  }

  // If Wayland compositor (Weston) terminates, -1 is returned.
  return (wl_display_dispatch(wl_display_) != -1);
}

bool LinuxesWindowWayland::CreateRenderSurface(int32_t width, int32_t height) {
  if (!display_valid_ || !wl_compositor_ || !wl_shell_) {
    LINUXES_LOG(ERROR) << "Invalid compositor and shell.";
    return false;
  }

  if (window_mode_ == FlutterWindowMode::kFullscreen) {
    width = current_width_;
    height = current_height_;
  }

  LINUXES_LOG(TRACE) << "Created the Wyalnad surface: " << width << "x"
                     << height;
  if (show_cursor_) {
    wl_cursor_surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!wl_cursor_surface_) {
      LINUXES_LOG(ERROR)
          << "Failed to create the compositor surface for cursor.";
      return false;
    }
  }

  native_window_ =
      std::make_unique<NativeWindowWayland>(wl_compositor_, width, height);

  wl_shell_surface_ =
      wl_shell_get_shell_surface(wl_shell_, native_window_->Surface());
  if (!wl_shell_surface_) {
    LINUXES_LOG(ERROR) << "Failed to get the shell surface.";
    return false;
  }
  wl_shell_surface_add_listener(wl_shell_surface_, &kWlShellSurfaceListener,
                                this);
  wl_shell_surface_set_title(wl_shell_surface_, "Flutter");
  wl_shell_surface_set_toplevel(wl_shell_surface_);

  render_surface_ =
      std::make_unique<SurfaceGlWayland>(std::make_unique<ContextEglWayland>(
          std::make_unique<EnvironmentEgl<wl_display>>(wl_display_)));
  render_surface_->SetNativeWindow(native_window_.get());

  // The offscreen (resource) surface will not be mapped, but needs to be a
  // wl_surface because ONLY window EGL surfaces are supported on Wayland.
  render_surface_->SetNativeWindowResource(
      std::make_unique<NativeWindowWayland>(wl_compositor_, 1, 1));

  return true;
}

void LinuxesWindowWayland::DestroyRenderSurface() {
  // destroy the main surface before destroying the client window on Wayland.
  {
    render_surface_ = nullptr;
    native_window_ = nullptr;
  }

  if (wl_shell_surface_) {
    wl_shell_surface_destroy(wl_shell_surface_);
    wl_shell_surface_ = nullptr;
  }

  if (wl_cursor_surface_) {
    wl_surface_destroy(wl_cursor_surface_);
    wl_cursor_surface_ = nullptr;
  }
}

void LinuxesWindowWayland::UpdateVirtualKeyboardStatus(const bool show) {
  // Not supported virtual keyboard.
  if (!zwp_text_input_v1_ || !wl_seat_) {
    return;
  }

  if (show) {
    if (native_window_) {
      zwp_text_input_v1_show_input_panel(zwp_text_input_v1_);
      zwp_text_input_v1_activate(zwp_text_input_v1_, wl_seat_,
                                 native_window_->Surface());
    }
  } else {
    zwp_text_input_v1_deactivate(zwp_text_input_v1_, wl_seat_);
  }
}

void LinuxesWindowWayland::UpdateFlutterCursor(const std::string& cursor_name) {
  if (show_cursor_) {
    if (cursor_name.compare(cursor_info_.cursor_name) == 0) {
      return;
    }
    cursor_info_.cursor_name = cursor_name;

    if (cursor_name.compare(kCursorNameNone) == 0) {
      // Turn off the cursor.
      wl_pointer_set_cursor(cursor_info_.wl_pointer, cursor_info_.serial,
                            wl_cursor_surface_, 0, 0);
      wl_surface_attach(wl_cursor_surface_, nullptr, 0, 0);
      wl_surface_damage(wl_cursor_surface_, 0, 0, 0, 0);
      wl_surface_commit(wl_cursor_surface_);
      return;
    }

    auto wl_cursor = GetWlCursor(cursor_name);
    if (!wl_cursor) {
      return;
    }
    auto image = wl_cursor->images[0];
    auto buffer = wl_cursor_image_get_buffer(image);
    if (buffer) {
      wl_pointer_set_cursor(cursor_info_.wl_pointer, cursor_info_.serial,
                            wl_cursor_surface_, image->hotspot_x,
                            image->hotspot_y);
      wl_surface_attach(wl_cursor_surface_, buffer, 0, 0);
      wl_surface_damage(wl_cursor_surface_, 0, 0, image->width, image->height);
      wl_surface_commit(wl_cursor_surface_);
    }
  }
}

std::string LinuxesWindowWayland::GetClipboardData() {
  std::string str = "";

  if (wl_data_offer_) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) == -1) {
      return str;
    }

    wl_data_offer_receive(wl_data_offer_, kClipboardMimeTypeText, fd[1]);
    close(fd[1]);
    wl_display_dispatch(wl_display_);

    char buf[256];
    int len;
    // Read data form the clipboard.
    while ((len = read(fd[0], buf, sizeof(buf))) > 0) {
      str.append(buf, len);
    }
    close(fd[0]);
    return str;
  }

  return str;
}

void LinuxesWindowWayland::SetClipboardData(const std::string& data) {
  clipboard_data_ = data;
  if (wl_data_device_manager_) {
    if (wl_data_source_) {
      wl_data_source_destroy(wl_data_source_);
      wl_data_source_ = nullptr;
    }

    wl_data_source_ =
        wl_data_device_manager_create_data_source(wl_data_device_manager_);
    if (!wl_data_source_) {
      return;
    }

    wl_data_source_offer(wl_data_source_, kClipboardMimeTypeText);
    wl_data_source_add_listener(wl_data_source_, &kWlDataSourceListener, this);
    wl_data_device_set_selection(wl_data_device_, wl_data_source_, serial_);
  }
}

bool LinuxesWindowWayland::IsValid() const {
  if (!display_valid_ || !native_window_ || !render_surface_ ||
      !native_window_->IsValid() || !render_surface_->IsValid()) {
    return false;
  }
  return true;
}

void LinuxesWindowWayland::WlRegistryHandler(wl_registry* wl_registry,
                                             uint32_t name,
                                             const char* interface,
                                             uint32_t version) {
  if (!strcmp(interface, kWlCompositor)) {
    wl_compositor_ = static_cast<decltype(wl_compositor_)>(
        wl_registry_bind(wl_registry, name, &wl_compositor_interface, 1));
    return;
  }

  if (!strcmp(interface, kWlShell)) {
    wl_shell_ = static_cast<decltype(wl_shell_)>(
        wl_registry_bind(wl_registry, name, &wl_shell_interface, 1));
    return;
  }

  if (!strcmp(interface, kWlSeat)) {
    wl_seat_ = static_cast<decltype(wl_seat_)>(
        wl_registry_bind(wl_registry, name, &wl_seat_interface, 1));
    wl_seat_add_listener(wl_seat_, &kWlSeatListener, this);
    return;
  }

#ifdef DESKTOP_SHELL
  if (!strcmp(interface, kWestonDesktopShell)) {
    weston_desktop_shell_ =
        static_cast<decltype(weston_desktop_shell_)>(wl_registry_bind(
            wl_registry, name, &weston_desktop_shell_interface, 1));
    return;
  }
#else
  weston_desktop_shell_ = nullptr;
#endif

  if (!strcmp(interface, kWlOutput)) {
    wl_output_ = static_cast<decltype(wl_output_)>(
        wl_registry_bind(wl_registry, name, &wl_output_interface, 1));
    wl_output_add_listener(wl_output_, &kWlOutputListener, this);
  }

  if (!strcmp(interface, kWlShm)) {
    if (show_cursor_) {
      wl_shm_ = static_cast<decltype(wl_shm_)>(
          wl_registry_bind(wl_registry, name, &wl_shm_interface, 1));
      wl_cursor_theme_ = wl_cursor_theme_load(nullptr, 32, wl_shm_);
      if (!wl_cursor_theme_) {
        LINUXES_LOG(ERROR) << "Failed to load cursor theme.";
        return;
      }
      CreateSupportedWlCursorList();
    }
    return;
  }

#ifdef USE_VIRTUAL_KEYBOARD
  if (!strcmp(interface, kZwpTextInputManagerV1)) {
    zwp_text_input_manager_v1_ =
        static_cast<decltype(zwp_text_input_manager_v1_)>(wl_registry_bind(
            wl_registry, name, &zwp_text_input_manager_v1_interface, 1));
    zwp_text_input_v1_ =
        zwp_text_input_manager_v1_create_text_input(zwp_text_input_manager_v1_);
    if (!zwp_text_input_v1_) {
      LINUXES_LOG(ERROR) << "Failed to create text input manager.";
      return;
    }
    zwp_text_input_v1_add_listener(zwp_text_input_v1_, &kZwpTextInputV1Listener,
                                   this);
    return;
  }
#endif

  if (!strcmp(interface, kWlDataDeviceManager)) {
    // Save the version of wl_data_device_manager because the release method of
    // wl_data_device differs depending on it. Since wl_data_device_manager has
    // been released up to version 3, set the upper limit to 3.
    constexpr uint32_t kMaxVersion = 3;
    wl_data_device_manager_version_ = std::min(kMaxVersion, version);
    wl_data_device_manager_ = static_cast<decltype(wl_data_device_manager_)>(
        wl_registry_bind(wl_registry, name, &wl_data_device_manager_interface,
                         wl_data_device_manager_version_));
  }
}

void LinuxesWindowWayland::WlUnRegistryHandler(wl_registry* wl_registry,
                                               uint32_t name) {}

void LinuxesWindowWayland::CreateSupportedWlCursorList() {
  std::vector<std::string> wl_cursor_themes{
      kWlCursorThemeLeftPtr,
      kWlCursorThemeBottomLeftCorner,
      kWlCursorThemeBottomRightCorner,
      kWlCursorThemeBottomSide,
      kWlCursorThemeGrabbing,
      kWlCursorThemeLeftSide,
      kWlCursorThemeRightSide,
      kWlCursorThemeTopLeftCorner,
      kWlCursorThemeTopRightCorner,
      kWlCursorThemeTopSide,
      kWlCursorThemeXterm,
      kWlCursorThemeHand1,
      kWlCursorThemeWatch,
  };

  for (const auto& theme : wl_cursor_themes) {
    auto wl_cursor =
        wl_cursor_theme_get_cursor(wl_cursor_theme_, theme.c_str());
    if (!wl_cursor) {
      LINUXES_LOG(ERROR) << "Unsupported cursor theme: " << theme.c_str();
      continue;
    }
    supported_wl_cursor_list_[theme] = wl_cursor;
  }
}

wl_cursor* LinuxesWindowWayland::GetWlCursor(const std::string& cursor_name) {
  // Convert the cursor theme name from Flutter's cursor value to Wayland's one.
  // However, Wayland has not all cursor themes corresponding to Flutter.
  // If there is no Wayland's cursor theme corresponding to the Flutter's cursor
  // name, it is defined as empty.
  // If empty, the default cursor theme(left_ptr) will be displayed.
  static const std::unordered_map<std::string, std::string>
      flutter_to_wayland_cursor_map = {
          {"alias", ""},
          {"allScroll", ""},
          {"basic", kWlCursorThemeLeftPtr},
          {"cell", ""},
          {"click", kWlCursorThemeHand1},
          {"contextMenu", ""},
          {"copy", ""},
          {"forbidden", ""},
          {"grab", ""},
          {"grabbing", kWlCursorThemeGrabbing},
          {"help", ""},
          {"move", ""},
          {"noDrop", ""},
          {"precise", ""},
          {"progress", ""},
          {"text", kWlCursorThemeXterm},
          {"resizeColumn", ""},
          {"resizeDown", kWlCursorThemeBottomSide},
          {"resizeDownLeft", kWlCursorThemeBottomLeftCorner},
          {"resizeDownRight", kWlCursorThemeBottomRightCorner},
          {"resizeLeft", kWlCursorThemeLeftSide},
          {"resizeLeftRight", ""},
          {"resizeRight", kWlCursorThemeRightSide},
          {"resizeRow", ""},
          {"resizeUp", kWlCursorThemeTopSide},
          {"resizeUpDown", ""},
          {"resizeUpLeft", kWlCursorThemeTopLeftCorner},
          {"resizeUpRight", kWlCursorThemeTopRightCorner},
          {"resizeUpLeftDownRight", ""},
          {"resizeUpRightDownLeft", ""},
          {"verticalText", ""},
          {"wait", kWlCursorThemeWatch},
          {"zoomIn", ""},
          {"zoomOut", ""},
      };

  if (flutter_to_wayland_cursor_map.find(cursor_name) !=
      flutter_to_wayland_cursor_map.end()) {
    auto theme = flutter_to_wayland_cursor_map.at(cursor_name);
    if (!theme.empty() && supported_wl_cursor_list_.find(theme) !=
                              supported_wl_cursor_list_.end()) {
      return supported_wl_cursor_list_[theme];
    }
  }

  LINUXES_LOG(ERROR) << "Unsupported cursor: " << cursor_name.c_str();
  return supported_wl_cursor_list_[kWlCursorThemeLeftPtr];
}

}  // namespace flutter