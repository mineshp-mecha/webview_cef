#ifndef WEBVIEW_CEF_TEXTURE_H_
#define WEBVIEW_CEF_TEXTURE_H_

#include <flutter/texture_registrar.h>
#include <flutter_texture_registrar.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#ifdef WEBVIEW_CEF_GPU_TEXTURE
#include "include/cef_render_handler.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241
#endif
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif

struct WebviewCefTexture
{
  int64_t textureId = 0;
  int32_t width = 0;
  int32_t height = 0;
  uint64_t frame_serial = 0;
  uint64_t uploaded_serial = 0;
  EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
  FlutterDesktopEGLImage egl_image_descriptor = {};
  int plane_count = 0;
  int fds[kAcceleratedPaintMaxPlanes] = {-1, -1, -1, -1};
  uint32_t strides[kAcceleratedPaintMaxPlanes] = {};
  uint64_t offsets[kAcceleratedPaintMaxPlanes] = {};
  uint64_t modifier = 0;
  cef_color_type_t format = CEF_COLOR_TYPE_RGBA_8888;
  std::vector<int> retired_fds;
  std::unique_ptr<flutter::TextureVariant> texture_variant;
  std::mutex mutex;

  WebviewCefTexture()
  {
    texture_variant = std::make_unique<flutter::TextureVariant>(
        flutter::EGLImageTexture(
            [this](size_t width, size_t height, void *display, void *context)
            {
              (void)context;
              return GetEGLImage(width, height, static_cast<EGLDisplay>(display));
            }));
  }

  ~WebviewCefTexture()
  {
    std::lock_guard<std::mutex> lock(mutex);
    CloseFds();
    DestroyImage(eglGetCurrentDisplay());
  }

  void CloseFds()
  {
    for (int index = 0; index < plane_count; ++index)
    {
      if (fds[index] >= 0)
      {
        close(fds[index]);
        fds[index] = -1;
      }
    }
    plane_count = 0;
    for (int fd : retired_fds)
    {
      if (fd >= 0)
        close(fd);
    }
    retired_fds.clear();
  }

  void DestroyImage(EGLDisplay display)
  {
    if (egl_image == EGL_NO_IMAGE_KHR)
      return;
    auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    if (destroy_image != nullptr && display != EGL_NO_DISPLAY)
    {
      destroy_image(display, egl_image);
    }
    egl_image = EGL_NO_IMAGE_KHR;
  }

  const FlutterDesktopEGLImage *GetEGLImage(size_t requested_width,
                                            size_t requested_height,
                                            EGLDisplay display)
  {
    (void)requested_width;
    (void)requested_height;
    std::lock_guard<std::mutex> lock(mutex);
    if (plane_count != 1 || fds[0] < 0 || frame_serial == 0 ||
        display == EGL_NO_DISPLAY)
    {
      return nullptr;
    }
    if (uploaded_serial != frame_serial)
    {
      auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
          eglGetProcAddress("eglCreateImageKHR"));
      if (create_image == nullptr)
        return nullptr;
      const uint32_t drm_format = format == CEF_COLOR_TYPE_BGRA_8888
                                      ? DRM_FORMAT_ARGB8888
                                      : DRM_FORMAT_ABGR8888;
      std::vector<EGLint> attributes = {
          EGL_WIDTH,
          static_cast<EGLint>(width),
          EGL_HEIGHT,
          static_cast<EGLint>(height),
          EGL_LINUX_DRM_FOURCC_EXT,
          static_cast<EGLint>(drm_format),
          EGL_DMA_BUF_PLANE0_FD_EXT,
          fds[0],
          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
          static_cast<EGLint>(offsets[0]),
          EGL_DMA_BUF_PLANE0_PITCH_EXT,
          static_cast<EGLint>(strides[0]),
      };
      if (modifier != 0)
      {
        attributes.insert(attributes.end(), {
                                                EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                                static_cast<EGLint>(modifier & 0xffffffffu),
                                                EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                                static_cast<EGLint>(modifier >> 32),
                                            });
      }
      attributes.push_back(EGL_NONE);
      EGLImageKHR new_image = create_image(display, EGL_NO_CONTEXT,
                                           EGL_LINUX_DMA_BUF_EXT, nullptr,
                                           attributes.data());
      if (new_image == EGL_NO_IMAGE_KHR)
        return nullptr;
      DestroyImage(display);
      for (int fd : retired_fds)
      {
        if (fd >= 0)
          close(fd);
      }
      retired_fds.clear();
      egl_image = new_image;
      uploaded_serial = frame_serial;
      egl_image_descriptor.egl_image = egl_image;
      egl_image_descriptor.width = static_cast<size_t>(width);
      egl_image_descriptor.height = static_cast<size_t>(height);
      egl_image_descriptor.release_callback = nullptr;
      egl_image_descriptor.release_context = nullptr;
    }
    return &egl_image_descriptor;
  }
};

#else
struct WebviewCefTexture {
  uint8_t* buffer = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t textureId = 0;

  FlutterDesktopPixelBuffer pixel_buffer = {};

  std::unique_ptr<flutter::TextureVariant> texture_variant;
  std::mutex mutex;

  WebviewCefTexture() {
    texture_variant = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture(
            [this](size_t width, size_t height)
            {
              (void)width;
              (void)height;
              std::lock_guard<std::mutex> lock(mutex);
              pixel_buffer.buffer = buffer;
              pixel_buffer.width = this->width;
              pixel_buffer.height = this->height;
              return &pixel_buffer;
            }));
  }

  ~WebviewCefTexture() {
    delete[] buffer;
  }
};
#endif

#endif  // WEBVIEW_CEF_TEXTURE_H_
