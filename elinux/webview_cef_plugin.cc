#include "include/webview_cef/webview_cef_plugin.h"
#include "include/cef_browser.h"

#include <flutter_elinux.h>
#include <flutter/plugin_registrar.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <cstring>
#include <memory>
#include <unordered_map>

#include <webview_plugin.h>
#include "webview_cef_texture.h"

#ifdef WEBVIEW_CEF_GPU_TEXTURE
#include <include/internal/cef_types_linux.h>
#include <libdrm/drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

class WebviewGpuTextureRenderer : public webview_cef::WebviewTexture {
 public:
  WebviewGpuTextureRenderer(FlutterDesktopTextureRegistrarRef registrar_ref)
      : registrar_ref_(registrar_ref) {
    texture_ = std::make_unique<WebviewGpuTexture>(registrar_ref_);
    textureId = texture_->textureId;
  }

  ~WebviewGpuTextureRenderer() override {
    FlutterDesktopTextureRegistrarUnregisterExternalTexture(registrar_ref_, texture_->textureId, nullptr, nullptr);
  }

  void onAcceleratedFrame(const void* sharedHandle, int width, int height, int format) override {
    if (!sharedHandle) return;
    auto* info = static_cast<const cef_accelerated_paint_info_t*>(sharedHandle);
    if (!info || info->plane_count == 0) return;

    std::cout << "[WebviewCEF-BUILD-7] onAcceleratedFrame: " << width << "x" << height 
              << " FD: " << info->planes[0].fd << std::endl;

    std::lock_guard<std::mutex> lock(texture_->mutex);
    
    // For eLinux on i.MX8MP, we expect DMA-BUF.
    // We need to create an EGLImage from the DMA-BUF FD.
    
    EGLDisplay display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) return;

    // Cleanup old image
    if (texture_->egl_image) {
      auto eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
      if (eglDestroyImageKHR) {
        eglDestroyImageKHR(display, (EGLImageKHR)texture_->egl_image);
      }
      texture_->egl_image = nullptr;
    }

    EGLint attribs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)((info->format == CEF_COLOR_TYPE_RGBA_8888) ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_ARGB8888),
        EGL_DMA_BUF_PLANE0_FD_EXT, (EGLint)info->planes[0].fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)info->planes[0].offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)info->planes[0].stride,
        EGL_NONE
    };

    std::cout << "[WebviewCEF-BUILD-7] Importing FD: " << info->planes[0].fd 
              << " Stride: " << info->planes[0].stride 
              << " Offset: " << info->planes[0].offset 
              << " Size: " << width << "x" << height << std::endl;

    auto eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    if (!eglCreateImageKHR) return;

    EGLImageKHR image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DRM_FOURCC_EXT, NULL, attribs);
    if (image != EGL_NO_IMAGE_KHR) {
      texture_->egl_image = image;
      texture_->width = width;
      texture_->height = height;
      FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(registrar_ref_, texture_->textureId);
    } else {
      EGLint err = eglGetError();
      std::cerr << "[WebviewCEF-BUILD-7] eglCreateImageKHR failed: 0x" << std::hex << err << std::dec << std::endl;
    }
  }

 private:
  FlutterDesktopTextureRegistrarRef registrar_ref_;
  std::unique_ptr<WebviewGpuTexture> texture_;
};
#endif

std::unordered_map<int64_t, std::shared_ptr<webview_cef::WebviewPlugin>> webviewPlugins;

class WebviewTextureRenderer : public webview_cef::WebviewTexture {
 public:
  WebviewTextureRenderer(flutter::TextureRegistrar* registrar)
      : registrar_(registrar) {
    texture_ = std::make_unique<WebviewCefTexture>();
    texture_->textureId =
        registrar_->RegisterTexture(texture_->texture_variant.get());
    textureId = texture_->textureId;
  }

  ~WebviewTextureRenderer() override {
    registrar_->UnregisterTexture(texture_->textureId);
  }

  void onFrame(const void* buffer, int32_t width, int32_t height) override {
    std::lock_guard<std::mutex> lock(texture_->mutex);
    if (texture_->width != (uint32_t)width ||
        texture_->height != (uint32_t)height) {
      texture_->width = width;
      texture_->height = height;
      delete[] texture_->buffer;
      texture_->buffer = new uint8_t[width * height * 4];
    }
    webview_cef::SwapBufferFromBgraToRgba(
        (void*)texture_->buffer, buffer, width, height);
    registrar_->MarkTextureFrameAvailable(texture_->textureId);
  }

 private:
  flutter::TextureRegistrar* registrar_;
  std::unique_ptr<WebviewCefTexture> texture_;
};

// ── Value conversion helpers (unchanged) ────────────────────────────────────

static flutter::EncodableValue encode_wvalue(WValue* args) {
  WValueType type = webview_value_get_type(args);
  switch (type) {
    case Webview_Value_Type_Bool:
      return flutter::EncodableValue(webview_value_get_bool(args));
    case Webview_Value_Type_Int:
      return flutter::EncodableValue((int64_t)webview_value_get_int(args));
    case Webview_Value_Type_Float:
      return flutter::EncodableValue(webview_value_get_float(args));
    case Webview_Value_Type_Double:
      return flutter::EncodableValue(webview_value_get_double(args));
    case Webview_Value_Type_String:
      return flutter::EncodableValue(
          std::string(webview_value_get_string(args)));
    case Webview_Value_Type_List: {
      flutter::EncodableList list;
      size_t len = webview_value_get_len(args);
      for (size_t i = 0; i < len; i++)
        list.push_back(encode_wvalue(webview_value_get_list_value(args, i)));
      return flutter::EncodableValue(list);
    }
    case Webview_Value_Type_Map: {
      flutter::EncodableMap map;
      size_t len = webview_value_get_len(args);
      for (size_t i = 0; i < len; i++) {
        auto key = encode_wvalue(webview_value_get_key(args, i));
        auto val = encode_wvalue(webview_value_get_value(args, i));
        map[key] = val;
      }
      return flutter::EncodableValue(map);
    }
    default:
      return flutter::EncodableValue();
  }
}

static WValue* decode_to_wvalue(const flutter::EncodableValue& val) {
  if (std::holds_alternative<bool>(val))
    return webview_value_new_bool(std::get<bool>(val));
  if (std::holds_alternative<int32_t>(val))
    return webview_value_new_int(std::get<int32_t>(val));
  if (std::holds_alternative<int64_t>(val))
    return webview_value_new_int(std::get<int64_t>(val));
  if (std::holds_alternative<double>(val))
    return webview_value_new_double(std::get<double>(val));
  if (std::holds_alternative<std::string>(val))
    return webview_value_new_string(std::get<std::string>(val).c_str());
  if (std::holds_alternative<flutter::EncodableList>(val)) {
    WValue* list = webview_value_new_list();
    for (auto& item : std::get<flutter::EncodableList>(val)) {
      WValue* witem = decode_to_wvalue(item);
      webview_value_append(list, witem);
      webview_value_unref(witem);
    }
    return list;
  }
  if (std::holds_alternative<flutter::EncodableMap>(val)) {
    WValue* map = webview_value_new_map();
    for (auto& [k, v] : std::get<flutter::EncodableMap>(val)) {
      WValue* wk = decode_to_wvalue(k);
      WValue* wv = decode_to_wvalue(v);
      webview_value_set(map, wk, wv);
      webview_value_unref(wk);
      webview_value_unref(wv);
    }
    return map;
  }
  return webview_value_new_null();
}

// ── Plugin class ─────────────────────────────────────────────────────────────

class WebviewCefPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar, FlutterDesktopTextureRegistrarRef registrar_ref, int64_t window_id) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "webview_cef",
        &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<WebviewCefPlugin>(
        registrar->texture_registrar(), registrar_ref, std::move(channel));

    // Populate the map and set the window_id_
    plugin->SetWindowId(window_id);
    registrar->AddPlugin(std::move(plugin));
  }

  WebviewCefPlugin(flutter::TextureRegistrar* texture_registrar,
                  FlutterDesktopTextureRegistrarRef registrar_ref,
                  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel)
#ifndef WEBVIEW_CEF_GPU_TEXTURE
    : texture_registrar_(texture_registrar),
#else
    :
#endif
      registrar_ref_(registrar_ref),
      channel_(std::move(channel)),
      plugin_(std::make_shared<webview_cef::WebviewPlugin>()) {
        
    channel_->SetMethodCallHandler(
        [this](const flutter::MethodCall<flutter::EncodableValue>& call,
               std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
          HandleMethodCall(call, std::move(result));
        });

    plugin_->setInvokeMethodFunc([this](std::string method, WValue* arguments) {
      flutter::EncodableValue args = encode_wvalue(arguments);
      channel_->InvokeMethod(method,
          std::make_unique<flutter::EncodableValue>(args));
    });

    plugin_->setCreateTextureFunc([this]() {
#ifdef WEBVIEW_CEF_GPU_TEXTURE
      auto renderer = std::make_shared<WebviewGpuTextureRenderer>(registrar_ref_);
#else
      auto renderer = std::make_shared<WebviewTextureRenderer>(texture_registrar_);
#endif
      return std::dynamic_pointer_cast<webview_cef::WebviewTexture>(renderer);
    });
  }

  ~WebviewCefPlugin() override {
    webviewPlugins.erase(window_id_);
    plugin_ = nullptr;
    if (webviewPlugins.empty()) {
      webview_cef::stopCEF();
    }
  }

  void SetWindowId(int64_t id) {
    window_id_ = id;
    webviewPlugins.emplace(id, plugin_);
  }

 private:
 void HandleMethodCall(
         const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

       const std::string& method = call.method_name();
       WValue* args = decode_to_wvalue(
           call.arguments() ? *call.arguments() : flutter::EncodableValue());

       auto result_ptr = result.release();
       plugin_->HandleMethodCall(
           method.c_str(), args,
           [result_ptr, method, args](int ret, WValue* responseArgs) {
             if (ret > 0) {
               auto val = encode_wvalue(responseArgs);
               result_ptr->Success(val);
             } else if (ret < 0) {
               result_ptr->Error("error", "error");
             } else {
               result_ptr->NotImplemented();
             }
             delete result_ptr;
             webview_value_unref(args);
           });
     }

#ifndef WEBVIEW_CEF_GPU_TEXTURE
  flutter::TextureRegistrar* texture_registrar_;
#endif
FlutterDesktopTextureRegistrarRef registrar_ref_;
std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
std::shared_ptr<webview_cef::WebviewPlugin> plugin_;
int64_t window_id_ = 0;
};

// ── C entry points ───────────────────────────────────────────────────────────

void webview_cef_plugin_register_with_registrar(
    FlutterDesktopPluginRegistrarRef registrar) {
      
  // Get the view handle to use as a unique window ID
  int64_t window_id = reinterpret_cast<int64_t>(FlutterDesktopPluginRegistrarGetView(registrar));

  WebviewCefPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrar>(registrar),
      FlutterDesktopRegistrarGetTextureRegistrar(registrar),
      window_id);
}

FLUTTER_PLUGIN_EXPORT int initCEFProcesses(int argc, char** argv) {
  CefMainArgs main_args(argc, argv);
  return webview_cef::initCEFProcesses(main_args);
}

// Alias expected by generated_plugin_registrant.cc
FLUTTER_PLUGIN_EXPORT void WebviewCefPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  webview_cef_plugin_register_with_registrar(registrar);
}
