#include "webview_subsurface.h"

#include "../common/fps_debug.h"  // FPSDBG

#include <drm_fourcc.h>
#include <wayland-client.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"  // FPSDBG

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
  wp_presentation* presentation = nullptr;  // FPSDBG

  // Dispatches |queue| as events arrive, so buffer releases and frame
  // callbacks are handled even while CEF sends no frames.
  std::thread dispatch_thread;
  std::atomic<bool> stopping{false};

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

// One lock for all subsurface state. It serialises the platform and CEF UI
// threads with the listeners, which run on the context's dispatch thread.
std::mutex& SubsurfaceMutex() {
  static std::mutex mutex;
  return mutex;
}

// Objects listeners may point at. A listener that runs after its object was
// destroyed (it was already being dispatched) finds it gone and does nothing.
std::unordered_set<const void*>& LiveObjects() {
  static std::unordered_set<const void*> objects;
  return objects;
}

bool IsLive(const void* object) {
  return LiveObjects().count(object) != 0;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Commit anyway when the compositor sends no frame callback for this long,
// so a quirk there can't freeze the page.
constexpr int64_t kFrameCallbackTimeoutMs = 100;

// Past this many rects, damaging the whole buffer is simpler and as cheap.
constexpr size_t kMaxDamageRects = 16;

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
  } else if (!strcmp(interface, wp_presentation_interface.name)) {  // FPSDBG
    self->presentation = static_cast<wp_presentation*>(
        wl_registry_bind(registry, name, &wp_presentation_interface, 1));
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
  // The embedder reads the display with prepare_read/read_events, so a second
  // thread can block on our queue alongside it.
  dispatch_thread = std::thread([this] {
    while (!stopping) {
      if (wl_display_dispatch_queue(display, queue) < 0) {
        break;
      }
    }
  });
  return true;
}

WaylandSubsurfaceContext::~WaylandSubsurfaceContext() {
  if (dispatch_thread.joinable()) {
    // Wake the dispatch thread with an event on our queue, then let it exit.
    stopping = true;
    wl_display* display_wrapper = WrapOnQueue(display, queue);
    wl_callback* wake = wl_display_sync(display_wrapper);
    wl_proxy_wrapper_destroy(display_wrapper);
    wl_display_flush(display);
    dispatch_thread.join();
    wl_callback_destroy(wake);
  }
  if (dmabuf) {
    zwp_linux_dmabuf_v1_destroy(dmabuf);
  }
  if (viewporter) {
    wp_viewporter_destroy(viewporter);
  }
  if (presentation) {  // FPSDBG
    wp_presentation_destroy(presentation);
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

// FPSDBG: wp_presentation feedback for the view's frames. The compositor
// reports each committed frame as presented (with the time it reached the
// screen) or discarded (replaced before it was shown). Printed about once a
// second while frames come in.
namespace presentation_dbg {

struct Stats {
  int presented = 0;
  int discarded = 0;
  uint64_t window_start_ns = 0;  // presentation clock
  uint64_t last_presented_ns = 0;
  uint64_t max_gap_ns = 0;
};
Stats stats;

void MaybePrint(uint64_t now_ns) {
  if (stats.window_start_ns == 0) {
    stats.window_start_ns = now_ns;
    return;
  }
  const uint64_t elapsed = now_ns - stats.window_start_ns;
  if (elapsed > 2500000000ull) {  // idle gap: start a new window
    stats = Stats{0, 0, now_ns, stats.last_presented_ns, 0};
    return;
  }
  if (elapsed < 1000000000ull) {
    return;
  }
  std::fprintf(stderr,
               "[FPSDBG] screen presented   %5.1f/s  discarded %4.1f/s  "
               "max gap %.0fms\n",
               stats.presented * 1e9 / elapsed,
               stats.discarded * 1e9 / elapsed, stats.max_gap_ns / 1e6);
  stats = Stats{0, 0, now_ns, stats.last_presented_ns, 0};
}

void OnSyncOutput(void*, struct wp_presentation_feedback*, wl_output*) {}

void OnPresented(void*,
                 struct wp_presentation_feedback* feedback,
                 uint32_t tv_sec_hi,
                 uint32_t tv_sec_lo,
                 uint32_t tv_nsec,
                 uint32_t,
                 uint32_t,
                 uint32_t,
                 uint32_t) {
  const uint64_t ns =
      ((static_cast<uint64_t>(tv_sec_hi) << 32) | tv_sec_lo) * 1000000000ull +
      tv_nsec;
  ++stats.presented;
  // Gaps longer than 2s are idle time, not a dropped frame.
  if (stats.last_presented_ns && ns > stats.last_presented_ns &&
      ns - stats.last_presented_ns < 2000000000ull) {
    stats.max_gap_ns = std::max(stats.max_gap_ns, ns - stats.last_presented_ns);
  }
  stats.last_presented_ns = ns;
  MaybePrint(ns);
  wp_presentation_feedback_destroy(feedback);
}

void OnDiscarded(void*, struct wp_presentation_feedback* feedback) {
  ++stats.discarded;
  wp_presentation_feedback_destroy(feedback);
}

constexpr wp_presentation_feedback_listener kListener = {
    OnSyncOutput, OnPresented, OnDiscarded};

}  // namespace presentation_dbg

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
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  LiveObjects().insert(this);
  LiveObjects().insert(&view_);
  LiveObjects().insert(&popup_);
  InitLayer(view_, context_->parent);
  wl_subsurface_place_below(view_.subsurface, context_->parent);
  // A new subsurface stacks above its parent, so the popup sits over the page.
  InitLayer(popup_, view_.surface);
  wl_surface_commit(popup_.surface);
  wl_surface_commit(view_.surface);
  wl_display_flush(context_->display);
}

WebviewSubsurface::~WebviewSubsurface() {
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  LiveObjects().erase(this);
  LiveObjects().erase(&view_);
  LiveObjects().erase(&popup_);
  CancelFrameCallback();
  DropPendingFrame();
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
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  if (!IsLive(data)) {
    return;
  }
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
                                 int fd,
                                 uint32_t offset,
                                 uint32_t stride,
                                 uint64_t modifier,
                                 cef_color_type_t format,
                                 int32_t width,
                                 int32_t height) {
  // The webview is opaque; X formats let the compositor skip blending.
  const uint32_t drm_format = format == CEF_COLOR_TYPE_BGRA_8888
                                  ? DRM_FORMAT_XRGB8888
                                  : DRM_FORMAT_XBGR8888;
  zwp_linux_buffer_params_v1* params =
      zwp_linux_dmabuf_v1_create_params(context_->dmabuf);
  // libwayland dups |fd| while marshalling, so the caller may close it after.
  zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride, modifier >> 32,
                                 modifier & 0xffffffff);
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
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
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
    // Unmapping the webview surface unmaps the popup with it. A hidden
    // surface may never get its frame callback, so stop waiting for it.
    Hide(view_);
    CancelFrameCallback();
    DropPendingFrame();
  }
  wl_display_flush(context_->display);
}

void WebviewSubsurface::Present(const CefAcceleratedPaintInfo& info,
                                const CefRenderHandler::RectList& dirty_rects,
                                int32_t width,
                                int32_t height) {
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  const auto& plane = info.planes[0];
  if (frame_callback_ &&
      NowMs() - frame_requested_ms_ < kFrameCallbackTimeoutMs) {
    // The compositor hasn't finished the previous frame: keep only the
    // newest one, with the damage of every frame it replaces.
    const int fd = dup(plane.fd);
    if (fd < 0) {
      return;
    }
    if (has_pending_) {
      close(pending_.fd);
    } else {
      pending_.damage.clear();
      pending_.full_damage = false;
    }
    pending_.fd = fd;
    pending_.offset = plane.offset;
    pending_.stride = plane.stride;
    pending_.modifier = info.modifier;
    pending_.format = info.format;
    pending_.width = width;
    pending_.height = height;
    pending_.full_damage = pending_.full_damage || dirty_rects.empty() ||
                           pending_.damage.size() + dirty_rects.size() >
                               kMaxDamageRects;
    if (!pending_.full_damage) {
      pending_.damage.insert(pending_.damage.end(), dirty_rects.begin(),
                             dirty_rects.end());
    }
    has_pending_ = true;
    static fpsdbg::Counter skipped("subsurface skipped");  // FPSDBG
    skipped.Tick();
    return;
  }
  // Nothing outstanding (or the callback timed out): commit right away.
  CancelFrameCallback();
  DropPendingFrame();
  CommitViewFrame(plane.fd, plane.offset, plane.stride, info.modifier,
                  info.format, width, height, &dirty_rects);
}

void WebviewSubsurface::CommitViewFrame(
    int fd,
    uint32_t offset,
    uint32_t stride,
    uint64_t modifier,
    cef_color_type_t format,
    int32_t width,
    int32_t height,
    const CefRenderHandler::RectList* dirty_rects) {
  if (!SetFrame(view_, fd, offset, stride, modifier, format, width, height) ||
      !visible_) {
    return;
  }
  frame_callback_ = wl_surface_frame(view_.surface);
  wl_callback_add_listener(frame_callback_, &kFrameListener, this);
  frame_requested_ms_ = NowMs();
  if (context_->presentation) {  // FPSDBG: feedback for the next commit
    wp_presentation_feedback_add_listener(
        wp_presentation_feedback(context_->presentation, view_.surface),
        &presentation_dbg::kListener, nullptr);
  }
  Show(view_, width_, height_, dirty_rects);
  wl_display_flush(context_->display);
  static fpsdbg::Counter counter("subsurface shown");  // FPSDBG
  counter.Tick();
}

void WebviewSubsurface::CommitPendingFrame() {
  if (!has_pending_) {
    return;
  }
  PendingFrame frame = std::move(pending_);
  pending_ = PendingFrame();
  has_pending_ = false;
  CommitViewFrame(frame.fd, frame.offset, frame.stride, frame.modifier,
                  frame.format, frame.width, frame.height,
                  frame.full_damage ? nullptr : &frame.damage);
  close(frame.fd);
}

void WebviewSubsurface::DropPendingFrame() {
  if (has_pending_) {
    close(pending_.fd);
    pending_ = PendingFrame();
    has_pending_ = false;
  }
}

void WebviewSubsurface::CancelFrameCallback() {
  if (frame_callback_) {
    wl_callback_destroy(frame_callback_);
    frame_callback_ = nullptr;
  }
}

const wl_callback_listener WebviewSubsurface::kFrameListener = {
    &WebviewSubsurface::OnFrameDone,
};

void WebviewSubsurface::OnFrameDone(void* data,
                                    wl_callback* callback,
                                    uint32_t time) {
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  if (!IsLive(data)) {
    return;
  }
  auto* self = static_cast<WebviewSubsurface*>(data);
  if (callback != self->frame_callback_) {
    return;  // A callback we already gave up on.
  }
  wl_callback_destroy(callback);
  self->frame_callback_ = nullptr;
  // The compositor is ready for the next frame: send the newest one.
  self->CommitPendingFrame();
}

void WebviewSubsurface::SetPopup(bool visible, const CefRect& rect) {
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
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
  std::lock_guard<std::mutex> lock(SubsurfaceMutex());
  const auto& plane = info.planes[0];
  if (!SetFrame(popup_, plane.fd, plane.offset, plane.stride, info.modifier,
                info.format, width, height) ||
      !PopupShown()) {
    return;
  }
  Show(popup_, popup_rect_.width, popup_rect_.height, &dirty_rects);
  wl_display_flush(context_->display);
}

}  // namespace webview_cef
