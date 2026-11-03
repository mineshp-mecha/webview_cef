#ifndef WEBVIEW_CEF_ELINUX_WEBVIEW_SUBSURFACE_H_
#define WEBVIEW_CEF_ELINUX_WEBVIEW_SUBSURFACE_H_

#include <flutter_elinux.h>

#include "include/cef_render_handler.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

struct wl_buffer;
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
  // on the CEF UI thread; |info| is valid only during the call.
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

  explicit WebviewSubsurface(std::shared_ptr<WaylandSubsurfaceContext> context);

  static void OnBufferRelease(void* data, wl_buffer* buffer);
  static const wl_buffer_listener kBufferListener;

  // All of these require |context_->mutex|, which also serialises the release
  // listener: it only runs from dispatches made under that lock.
  void InitLayer(Layer& layer, wl_surface* parent);
  void DestroyLayer(Layer& layer);
  // Wraps the frame in a wl_buffer and makes it |layer|'s current one.
  bool SetFrame(Layer& layer,
                const CefAcceleratedPaintInfo& info,
                int32_t width,
                int32_t height);
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
