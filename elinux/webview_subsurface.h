#ifndef WEBVIEW_CEF_ELINUX_WEBVIEW_SUBSURFACE_H_
#define WEBVIEW_CEF_ELINUX_WEBVIEW_SUBSURFACE_H_

#include <flutter_elinux.h>

#include "include/cef_render_handler.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct wl_buffer;
struct wl_callback;
struct wl_callback_listener;
struct wl_buffer_listener;
struct wl_display;
struct wl_event_queue;
struct wl_surface;
struct wl_subsurface;
struct wp_viewport;

namespace webview_cef {

class WaylandSubsurfaceContext;

// Presents CEF's DMA-BUF frames on a Wayland subsurface placed below the
// Flutter surface, so webview updates are damaged with CEF's dirty rects and
// Flutter frames over the webview never re-damage it. Flutter must leave the
// webview's rect transparent.
//
// The browser's popup widget (an open <select> list) gets its own subsurface,
// a child of the webview's, so it moves and hides with the webview and its
// position applies on our own commits rather than Flutter's.
class WebviewSubsurface {
 public:
  // Returns null when the view is not on Wayland or the compositor lacks
  // wl_subcompositor, zwp_linux_dmabuf_v1 or wp_viewporter.
  static std::unique_ptr<WebviewSubsurface> Create(FlutterDesktopViewRef view);

  ~WebviewSubsurface();

  // Places the webview in the parent surface's coordinates (Flutter logical
  // pixels). A hidden subsurface keeps no buffer attached.
  void SetRect(int32_t x, int32_t y, int32_t width, int32_t height,
               bool visible);

  // Attaches the frame and damages only |dirty_rects| (buffer pixels). Called
  // on the CEF UI thread; |info| is valid only during the call. Paced by the
  // compositor's frame callbacks: while the previous frame is not done, only
  // the newest frame is kept and committed when the callback arrives.
  void Present(const CefAcceleratedPaintInfo& info,
               const CefRenderHandler::RectList& dirty_rects,
               int32_t width,
               int32_t height);

  // Shows the popup at |rect| (webview logical pixels) or hides it.
  void SetPopup(bool visible, const CefRect& rect);

  // Like Present, for the popup's frames.
  void PresentPopup(const CefAcceleratedPaintInfo& info,
                    const CefRenderHandler::RectList& dirty_rects,
                    int32_t width,
                    int32_t height);

 private:
  // One wl_surface fed with CEF frames.
  struct Layer {
    wl_surface* surface = nullptr;
    wl_subsurface* subsurface = nullptr;
    wp_viewport* viewport = nullptr;
    // Live buffers, mapped to whether the compositor still holds them.
    std::unordered_map<wl_buffer*, bool> buffers;
    // The latest CEF frame.
    wl_buffer* current = nullptr;
    // Whether the compositor has a buffer attached.
    bool mapped = false;
    // The compositor holds nothing for a surface that was hidden, so its next
    // frame is damaged whole rather than with CEF's incremental dirty rects.
    bool needs_full_damage = true;
  };

  // A CEF frame waiting for the compositor's frame callback. Holds its own
  // dup of the DMA-BUF fd so the buffer outlives CEF's paint callback.
  struct PendingFrame {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint64_t modifier = 0;
    cef_color_type_t format = CEF_COLOR_TYPE_BGRA_8888;
    int32_t width = 0;
    int32_t height = 0;
    // Union of the dirty rects of every frame replaced while waiting.
    CefRenderHandler::RectList damage;
    bool full_damage = false;
  };

  explicit WebviewSubsurface(std::shared_ptr<WaylandSubsurfaceContext> context);

  static void OnBufferRelease(void* data, wl_buffer* buffer);
  static const wl_buffer_listener kBufferListener;
  static void OnFrameDone(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener kFrameListener;

  // All of these require the subsurface mutex, which the listeners (run on
  // the context's dispatch thread) take too.
  void InitLayer(Layer& layer, wl_surface* parent);
  void DestroyLayer(Layer& layer);
  // Wraps the frame in a wl_buffer and makes it |layer|'s current one.
  bool SetFrame(Layer& layer,
                int fd,
                uint32_t offset,
                uint32_t stride,
                uint64_t modifier,
                cef_color_type_t format,
                int32_t width,
                int32_t height);
  // Commits a view frame and asks for the compositor's frame callback.
  // Null |dirty_rects| damages the whole buffer.
  void CommitViewFrame(int fd,
                       uint32_t offset,
                       uint32_t stride,
                       uint64_t modifier,
                       cef_color_type_t format,
                       int32_t width,
                       int32_t height,
                       const CefRenderHandler::RectList* dirty_rects);
  void CommitPendingFrame();
  void DropPendingFrame();
  // Forgets an outstanding frame callback, e.g. once the surface is hidden
  // and the compositor may never send it.
  void CancelFrameCallback();
  // Attaches |layer.current| scaled to |width| x |height| logical pixels.
  // Null |dirty_rects| damages the whole buffer.
  void Show(Layer& layer,
            int32_t width,
            int32_t height,
            const CefRenderHandler::RectList* dirty_rects);
  void Hide(Layer& layer);
  bool PopupShown() const;

  std::shared_ptr<WaylandSubsurfaceContext> context_;
  Layer view_;
  Layer popup_;
  // The view's outstanding frame callback, and when it was requested.
  wl_callback* frame_callback_ = nullptr;
  int64_t frame_requested_ms_ = 0;
  PendingFrame pending_;
  bool has_pending_ = false;

  int32_t x_ = 0;
  int32_t y_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  bool visible_ = false;
  bool popup_visible_ = false;
  CefRect popup_rect_;
};

}  // namespace webview_cef

#endif  // WEBVIEW_CEF_ELINUX_WEBVIEW_SUBSURFACE_H_
