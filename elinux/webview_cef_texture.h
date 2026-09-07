#ifndef WEBVIEW_CEF_TEXTURE_H_
#define WEBVIEW_CEF_TEXTURE_H_

#include <flutter/texture_registrar.h>
#include <flutter_texture_registrar.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#ifdef WEBVIEW_CEF_GPU_TEXTURE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

struct WebviewGpuTexture {
  int64_t textureId = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  void* egl_image = nullptr;
  std::mutex mutex;

  FlutterDesktopEGLImage egl_image_buffer = {};

  std::unique_ptr<flutter::TextureVariant> texture_variant;

  WebviewGpuTexture(FlutterDesktopTextureRegistrarRef registrar) {
    FlutterDesktopEGLImageTextureConfig config = {
        .callback = [](size_t w, size_t h, void* egl_display, void* egl_context,
                       void* user_data) -> const FlutterDesktopEGLImage* {
          auto* self = static_cast<WebviewGpuTexture*>(user_data);
          std::lock_guard<std::mutex> lock(self->mutex);
          if (!self->egl_image) {
            return nullptr;
          }
          self->egl_image_buffer.egl_image = self->egl_image;
          self->egl_image_buffer.width = self->width;
          self->egl_image_buffer.height = self->height;
          return &self->egl_image_buffer;
        },
        .user_data = this,
    };

    FlutterDesktopTextureInfo info = {
        .type = kFlutterDesktopEGLImageTexture,
        .egl_image_config = config,
    };

    textureId = FlutterDesktopTextureRegistrarRegisterExternalTexture(registrar, &info);
  }

  ~WebviewGpuTexture() {
    if (egl_image) {
      EGLDisplay display = eglGetCurrentDisplay();
      if (display != EGL_NO_DISPLAY) {
        auto eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        if (eglDestroyImageKHR) {
          eglDestroyImageKHR(display, (EGLImageKHR)egl_image);
        }
      }
    }
  }
};
#endif

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
            [this](size_t w, size_t h) -> const FlutterDesktopPixelBuffer* {
              std::lock_guard<std::mutex> lock(mutex);
              pixel_buffer.buffer = buffer;
              pixel_buffer.width = width;
              pixel_buffer.height = height;
              return &pixel_buffer;
            }));
  }

  ~WebviewCefTexture() {
    delete[] buffer;
  }
};

#endif  // WEBVIEW_CEF_TEXTURE_H_
