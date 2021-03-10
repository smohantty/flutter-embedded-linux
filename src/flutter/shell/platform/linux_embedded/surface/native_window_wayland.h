// Copyright 2021 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_NATIVE_WINDOW_WAYLAND_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_NATIVE_WINDOW_WAYLAND_H_

#include <wayland-egl.h>

#include "flutter/shell/platform/linux_embedded/surface/native_window.h"

namespace flutter {

class NativeWindowWayland : public NativeWindow<wl_egl_window, wl_surface> {
 public:
  NativeWindowWayland(wl_compositor* compositor, const size_t width,
                      const size_t height);
  ~NativeWindowWayland();

  // |NativeWindow|
  bool Resize(const size_t width, const size_t height) const override;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_SURFACE_NATIVE_WINDOW_WAYLAND_H_