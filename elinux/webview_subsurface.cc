#include "webview_subsurface.h"

#include <drm_fourcc.h>
#include <wayland-client.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

namespace webview_cef {

// Wayland objects shared by every webview of one Flutter view. All proxies
// live on a private event queue so the embedder's dispatch of the default
// queue never runs our listeners, and ours never runs the embedder's.
class WaylandSubsurfaceContext {
 public:
  static std::shared_ptr<WaylandSubsurfaceContext> Get(
      FlutterDesktopViewRef view);

  ~WaylandSubsurfaceContext();

  wl_display* display = nullptr;
  wl_surface* parent = nullptr;
  wl_event_queue* queue = nullptr;
  wl_compositor* compositor = nullptr;
  wl_subcompositor* subcompositor = nullptr;
  zwp_linux_dmabuf_v1* dmabuf = nullptr;
  wp_viewporter* viewporter = nullptr;

  // Serialises request streams from the platform and CEF UI threads.
  std::mutex mutex;

  // wl_registry_listener callbacks.
  static void OnGlobal(void* data,
                       wl_registry* registry,
                       uint32_t name,
                       const char* interface,
                       uint32_t version);
  static void OnGlobalRemove(void* data, wl_registry* registry, uint32_t name) {}

 private:
  bool Init(const FlutterDesktopWaylandHandles& handles);

  wl_registry* registry_ = nullptr;
};

namespace {

constexpr wl_registry_listener kRegistryListener = {
    &WaylandSubsurfaceContext::OnGlobal,
    &WaylandSubsurfaceContext::OnGlobalRemove,
};

// Returns a wrapper of |proxy| whose new objects are created on |queue|.
template <typename T>
T* WrapOnQueue(T* proxy, wl_event_queue* queue) {
  auto* wrapper = static_cast<T*>(
      wl_proxy_create_wrapper(reinterpret_cast<wl_proxy*>(proxy)));
  if (wrapper) {
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  }
  return wrapper;
}

template <typename T>
void DestroyWrapper(T*& wrapper) {
  if (wrapper) {
    wl_proxy_wrapper_destroy(wrapper);
    wrapper = nullptr;
  }
}

}  // namespace

void WaylandSubsurfaceContext::OnGlobal(void* data,
                                        wl_registry* registry,
                                        uint32_t name,
                                        const char* interface,
                                        uint32_t version) {
  auto* self = static_cast<WaylandSubsurfaceContext*>(data);
  if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 2) {
    self->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(wl_registry_bind(
        registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u)));
  } else if (!strcmp(interface, wp_viewporter_interface.name)) {
    self->viewporter = static_cast<wp_viewporter*>(
        wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
  }
}

std::shared_ptr<WaylandSubsurfaceContext> WaylandSubsurfaceContext::Get(
    FlutterDesktopViewRef view) {
  static std::mutex cache_mutex;
  static std::unordered_map<FlutterDesktopViewRef,
                            std::weak_ptr<WaylandSubsurfaceContext>>
      cache;
  std::lock_guard<std::mutex> lock(cache_mutex);
  if (auto existing = cache[view].lock()) {
    return existing;
  }

  FlutterDesktopWaylandHandles handles = {};
  if (!FlutterDesktopViewGetWaylandHandles(view, &handles) ||
      !handles.subcompositor) {
    return nullptr;
  }
  auto context = std::make_shared<WaylandSubsurfaceContext>();
  if (!context->Init(handles)) {
    return nullptr;
  }
  cache[view] = context;
  return context;
}

bool WaylandSubsurfaceContext::Init(
    const FlutterDesktopWaylandHandles& handles) {
  display = static_cast<wl_display*>(handles.display);
  parent = static_cast<wl_surface*>(handles.surface);
  queue = wl_display_create_queue(display);
  if (!queue) {
    return false;
  }
  compositor =
      WrapOnQueue(static_cast<wl_compositor*>(handles.compositor), queue);
  subcompositor =
      WrapOnQueue(static_cast<wl_subcompositor*>(handles.subcompositor), queue);

  wl_display* display_wrapper = WrapOnQueue(display, queue);
  if (!display_wrapper) {
    return false;
  }
  registry_ = wl_display_get_registry(display_wrapper);
  wl_proxy_wrapper_destroy(display_wrapper);
  wl_registry_add_listener(registry_, &kRegistryListener, this);
  wl_display_roundtrip_queue(display, queue);

  if (!compositor || !subcompositor || !dmabuf || !viewporter) {
    std::cerr << "webview_cef: subsurface unavailable (dmabuf=" << !!dmabuf
              << " viewporter=" << !!viewporter << ")" << std::endl;
    return false;
  }
  return true;
}

WaylandSubsurfaceContext::~WaylandSubsurfaceContext() {
  if (dmabuf) {
    zwp_linux_dmabuf_v1_destroy(dmabuf);
  }
  if (viewporter) {
    wp_viewporter_destroy(viewporter);
  }
  if (registry_) {
    wl_registry_destroy(registry_);
  }
  DestroyWrapper(compositor);
  DestroyWrapper(subcompositor);
  if (queue) {
    wl_display_flush(display);
    wl_event_queue_destroy(queue);
  }
}

// ── WebviewSubsurface ───────────────────────────────────────────────────────

std::unique_ptr<WebviewSubsurface> WebviewSubsurface::Create(
    FlutterDesktopViewRef view) {
  auto context = WaylandSubsurfaceContext::Get(view);
  if (!context) {
    return nullptr;
  }
  auto subsurface =
      std::unique_ptr<WebviewSubsurface>(new WebviewSubsurface(context));
  if (!subsurface->view_.viewport || !subsurface->popup_.viewport) {
    return nullptr;
  }
  return subsurface;
}

WebviewSubsurface::WebviewSubsurface(
    std::shared_ptr<WaylandSubsurfaceContext> context)
    : context_(std::move(context)) {
  std::lock_guard<std::mutex> lock(context_->mutex);
  InitLayer(view_, context_->parent);
  wl_subsurface_place_below(view_.subsurface, context_->parent);
  // A new subsurface stacks above its parent, so the popup sits over the page.
  InitLayer(popup_, view_.surface);
  wl_surface_commit(popup_.surface);
  wl_surface_commit(view_.surface);
  wl_display_flush(context_->display);
}

WebviewSubsurface::~WebviewSubsurface() {
  std::lock_guard<std::mutex> lock(context_->mutex);
  DestroyLayer(popup_);
  DestroyLayer(view_);
  wl_display_flush(context_->display);
}

void WebviewSubsurface::InitLayer(Layer& layer, wl_surface* parent) {
  layer.surface = wl_compositor_create_surface(context_->compositor);
  layer.subsurface = wl_subcompositor_get_subsurface(
      context_->subcompositor, layer.surface, parent);
  // Flutter keeps all input: an empty input region lets pointer events over
  // the hole fall through to the parent surface.
  wl_region* empty = wl_compositor_create_region(context_->compositor);
  wl_surface_set_input_region(layer.surface, empty);
  wl_region_destroy(empty);
  // Desync so CEF frames reach the screen without waiting for the parent to
  // commit; position changes still apply on the parent's next commit.
  wl_subsurface_set_desync(layer.subsurface);
  layer.viewport = wp_viewporter_get_viewport(context_->viewporter,
                                              layer.surface);
}

void WebviewSubsurface::DestroyLayer(Layer& layer) {
  if (layer.viewport) {
    wp_viewport_destroy(layer.viewport);
  }
  if (layer.subsurface) {
    wl_subsurface_destroy(layer.subsurface);
  }
  if (layer.surface) {
    wl_surface_destroy(layer.surface);
  }
  for (auto& [buffer, held] : layer.buffers) {
    wl_buffer_destroy(buffer);
  }
  layer = Layer();
}

const wl_buffer_listener WebviewSubsurface::kBufferListener = {
    &WebviewSubsurface::OnBufferRelease,
};

void WebviewSubsurface::OnBufferRelease(void* data, wl_buffer* buffer) {
  auto* layer = static_cast<Layer*>(data);
  auto it = layer->buffers.find(buffer);
  if (it == layer->buffers.end()) {
    return;
  }
  it->second = false;
  // The latest frame stays alive so a hidden surface can be shown again
  // without waiting for CEF to repaint.
  if (buffer != layer->current) {
    wl_buffer_destroy(buffer);
    layer->buffers.erase(it);
  }
}

bool WebviewSubsurface::SetFrame(Layer& layer,
                                 const CefAcceleratedPaintInfo& info,
                                 int32_t width,
                                 int32_t height) {
  const auto& plane = info.planes[0];
  // The webview is opaque; X formats let the compositor skip blending.
  const uint32_t drm_format = info.format == CEF_COLOR_TYPE_BGRA_8888
                                  ? DRM_FORMAT_XRGB8888
                                  : DRM_FORMAT_XBGR8888;
  zwp_linux_buffer_params_v1* params =
      zwp_linux_dmabuf_v1_create_params(context_->dmabuf);
  zwp_linux_buffer_params_v1_add(params, plane.fd, 0, plane.offset,
                                 plane.stride, info.modifier >> 32,
                                 info.modifier & 0xffffffff);
  wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
      params, width, height, drm_format, 0);
  zwp_linux_buffer_params_v1_destroy(params);
  if (!buffer) {
    return false;
  }
  wl_buffer_add_listener(buffer, &kBufferListener, &layer);
  layer.buffers[buffer] = false;

  // CEF exports a fresh DMA-BUF for every paint, so each frame gets its own
  // wl_buffer; the previous one goes now, or on release if still on screen.
  wl_buffer* previous = layer.current;
  layer.current = buffer;
  if (previous) {
    auto it = layer.buffers.find(previous);
    if (it != layer.buffers.end() && !it->second) {
      wl_buffer_destroy(previous);
      layer.buffers.erase(it);
    }
  }
  return true;
}

void WebviewSubsurface::Show(Layer& layer,
                             int32_t width,
                             int32_t height,
                             const CefRenderHandler::RectList* dirty_rects) {
  if (!layer.current) {
    return;
  }
  wl_surface_attach(layer.surface, layer.current, 0, 0);
  layer.buffers[layer.current] = true;
  // CEF's dirty rects are relative to the previous frame it painted, which is
  // what the compositor shows, so damaging only them is correct.
  if (!dirty_rects || dirty_rects->empty() || layer.needs_full_damage) {
    wl_surface_damage_buffer(layer.surface, 0, 0, INT32_MAX, INT32_MAX);
  } else {
    for (const auto& r : *dirty_rects) {
      wl_surface_damage_buffer(layer.surface, r.x, r.y, r.width, r.height);
    }
  }
  layer.needs_full_damage = false;
  layer.mapped = true;
  wp_viewport_set_destination(layer.viewport, width, height);
  wl_surface_commit(layer.surface);
}

void WebviewSubsurface::Hide(Layer& layer) {
  if (layer.mapped) {
    wl_surface_attach(layer.surface, nullptr, 0, 0);
    wl_surface_commit(layer.surface);
    layer.mapped = false;
  }
  layer.needs_full_damage = true;
}

bool WebviewSubsurface::PopupShown() const {
  return popup_visible_ && popup_rect_.width > 0 && popup_rect_.height > 0;
}

void WebviewSubsurface::SetRect(int32_t x,
                                int32_t y,
                                int32_t width,
                                int32_t height,
                                bool visible) {
  std::lock_guard<std::mutex> lock(context_->mutex);
  wl_display_dispatch_queue_pending(context_->display, context_->queue);
  visible = visible && width > 0 && height > 0;
  if (x == x_ && y == y_ && width == width_ && height == height_ &&
      visible == visible_) {
    return;
  }
  if (x != x_ || y != y_) {
    wl_subsurface_set_position(view_.subsurface, x, y);
  }
  x_ = x;
  y_ = y;
  width_ = width;
  height_ = height;
  visible_ = visible;
  if (visible_) {
    // CEF repaints at the new size shortly; until then the viewport scales
    // the previous frame into the new rect.
    Show(view_, width_, height_, nullptr);
  } else {
    // Unmapping the webview surface unmaps the popup with it.
    Hide(view_);
  }
  wl_display_flush(context_->display);
}

void WebviewSubsurface::Present(const CefAcceleratedPaintInfo& info,
                                const CefRenderHandler::RectList& dirty_rects,
                                int32_t width,
                                int32_t height) {
  std::lock_guard<std::mutex> lock(context_->mutex);
  // Runs OnBufferRelease for buffers the compositor is done with.
  wl_display_dispatch_queue_pending(context_->display, context_->queue);
  if (!SetFrame(view_, info, width, height) || !visible_) {
    return;
  }
  Show(view_, width_, height_, &dirty_rects);
  wl_display_flush(context_->display);
}

void WebviewSubsurface::SetPopup(bool visible, const CefRect& rect) {
  std::lock_guard<std::mutex> lock(context_->mutex);
  wl_display_dispatch_queue_pending(context_->display, context_->queue);
  const bool moved = rect.x != popup_rect_.x || rect.y != popup_rect_.y;
  popup_visible_ = visible;
  popup_rect_ = rect;
  if (!PopupShown()) {
    Hide(popup_);
    wl_display_flush(context_->display);
    return;
  }
  if (moved) {
    wl_subsurface_set_position(popup_.subsurface, rect.x, rect.y);
    // The popup's position is applied by its parent's commit, which is ours.
    wl_surface_commit(view_.surface);
  }
  // CEF may not repaint a popup reopened with unchanged content, so show the
  // last frame right away; fresh frames replace it as they arrive.
  Show(popup_, rect.width, rect.height, nullptr);
  wl_display_flush(context_->display);
}

void WebviewSubsurface::PresentPopup(
    const CefAcceleratedPaintInfo& info,
    const CefRenderHandler::RectList& dirty_rects,
    int32_t width,
    int32_t height) {
  std::lock_guard<std::mutex> lock(context_->mutex);
  wl_display_dispatch_queue_pending(context_->display, context_->queue);
  if (!SetFrame(popup_, info, width, height) || !PopupShown()) {
    return;
  }
  Show(popup_, popup_rect_.width, popup_rect_.height, &dirty_rects);
  wl_display_flush(context_->display);
}

}  // namespace webview_cef
