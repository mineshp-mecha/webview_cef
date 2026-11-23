// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "webview_handler.h"
#include "fps_debug.h"  // FPSDBG

#include <sstream>
#include <string>
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <cstdint>
#include <png.h>
#include <zlib.h>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

// std::to_string fails for ints on Ubuntu 24.04:
// webview_handler.cc:86:86: error: no matching function for call to 'to_string'
// webview_handler.cc:567:24: error: no matching function for call to 'to_string'
namespace stringpatch
{
    template < typename T > std::string to_string( const T& n )
    {
        std::ostringstream stm ;
        stm << n ;
        return stm.str() ;
    }
}

#include "webview_js_handler.h"

namespace {
// The only browser that currently get focused
CefRefPtr<CefBrowser> current_focused_browser_ = nullptr;

}  // namespace

WebviewHandler::WebviewHandler() {

}

WebviewHandler::~WebviewHandler() {
    browser_map_.clear();
    js_callbacks_.clear();
}

namespace {
// HiDPI: CEF's accelerated OSR path ignores CefScreenInfo::device_scale_factor
// on Linux and always renders at view size. So the view is made |dpi| times
// larger than the Flutter widget (physical pixels) and the page zoomed by
// |dpi| to keep its CSS layout. Coordinates entering CEF are scaled up by
// ToView; view coordinates leaving it are scaled down by ToLogical.
int ToView(int logical, float dpi) {
    return static_cast<int>(std::lround(logical * dpi));
}

int ToLogical(int view, float dpi) {
    return dpi > 0 ? static_cast<int>(std::lround(view / dpi)) : view;
}

CefRect ToLogical(const CefRect &rect, float dpi) {
    return CefRect(ToLogical(rect.x, dpi), ToLogical(rect.y, dpi),
                   ToLogical(rect.width, dpi), ToLogical(rect.height, dpi));
}

// Chromium zoom factor is 1.2^level.
double ZoomLevelFor(float dpi) {
    return dpi > 0 ? std::log(dpi) / std::log(1.2) : 0.0;
}

// The scale CEF renders at: |dpi|, capped at kDefaultMaxRenderScale or
// WEBVIEW_CEF_MAX_RENDER_SCALE (0 removes the cap). Raster cost grows with the
// square of the scale, so a cap trades sharpness for speed on weak GPUs; the
// compositor scales the frame up to fit.
constexpr float kDefaultMaxRenderScale = 2.0f;

float RenderScale(float dpi) {
    static const float cap = [] {
        const char* value = std::getenv("WEBVIEW_CEF_MAX_RENDER_SCALE");
        return value ? std::strtof(value, nullptr) : kDefaultMaxRenderScale;
    }();
    return cap >= 1.0f ? std::min(dpi, cap) : dpi;
}
}  // namespace

bool WebviewHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
     CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
	std::string message_name = message->GetName();
    if (message_name == kFocusedNodeChangedMessage)
    {
        current_focused_browser_ = browser;
        bool editable = message->GetArgumentList()->GetBool(0);
        if (onFocusedNodeChangeMessage) {
            onFocusedNodeChangeMessage(browser->GetIdentifier(), editable);
        }
        if (editable && onImeCompositionRangeChangedMessage) {
            auto bit = browser_map_.find(browser->GetIdentifier());
            const float dpi = bit != browser_map_.end() ? bit->second.dpi : 1.0f;
            auto args = message->GetArgumentList();
            onImeCompositionRangeChangedMessage(browser->GetIdentifier(), ToLogical(args->GetInt(1), dpi), ToLogical(args->GetInt(2), dpi), ToLogical(args->GetInt(3), dpi));
        }
    }
    else if(message_name == kJSCallCppFunctionMessage)
    {
        CefString fun_name = message->GetArgumentList()->GetString(0);
		CefString param = message->GetArgumentList()->GetString(1);
		int js_callback_id = message->GetArgumentList()->GetInt(2);

        if (fun_name.empty() || !(browser.get())) {
		    return false;
	    }

        onJavaScriptChannelMessage(
            fun_name,param,stringpatch::to_string(js_callback_id), browser->GetIdentifier(), frame->GetIdentifier().ToString());
    }
    else if(message_name == kEvaluateCallbackMessage){
        CefString callbackId = message->GetArgumentList()->GetString(0);
        CefRefPtr<CefValue> param = message->GetArgumentList()->GetValue(1);

        if(!callbackId.empty()){
            auto it = js_callbacks_.find(callbackId.ToString());
            if(it != js_callbacks_.end()){
                it->second(param);
                js_callbacks_.erase(it);
            }
        }
    }
    return false;
}

void WebviewHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
    //todo: title change
    if(onTitleChangedEvent) {
        onTitleChangedEvent(browser->GetIdentifier(), title);
    }
}

void WebviewHandler::OnAddressChange(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                     const CefString& url) {
    if(onUrlChangedEvent) {
        onUrlChangedEvent(browser->GetIdentifier(), url);
    }
}

bool WebviewHandler::OnCursorChange(CefRefPtr<CefBrowser> browser,
                            CefCursorHandle cursor,
                            cef_cursor_type_t type,
                            const CefCursorInfo& custom_cursor_info){
    if(onCursorChangedEvent) {
        onCursorChangedEvent(browser->GetIdentifier(), type);
        return true;
    }
    return false;
}

bool WebviewHandler::OnTooltip(CefRefPtr<CefBrowser> browser, CefString& text) {
    if(onTooltipEvent) {
        onTooltipEvent(browser->GetIdentifier(), text);
        return true;
    }
    return false;
}

bool WebviewHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                      cef_log_severity_t level,
                                      const CefString& message,
                                      const CefString& source,
                                      int line){
    if(onConsoleMessageEvent){
        onConsoleMessageEvent(browser->GetIdentifier(), level, message, source, line);
    }
    return false;
}

void WebviewHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    if (!browser->IsPopup()) {
        browser_map_.emplace(browser->GetIdentifier(), browser_info());
        browser_map_[browser->GetIdentifier()].browser = browser;
    }
}

bool WebviewHandler::DoClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();    
    // Allow the close. For windowed browsers this will result in the OS close
    // event being sent.
    return false;
}

void WebviewHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
}

bool WebviewHandler::OnBeforePopup(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  int popup_id,
                                  const CefString& target_url,
                                  const CefString& target_frame_name,
                                  WindowOpenDisposition target_disposition,
                                  bool user_gesture,
                                  const CefPopupFeatures& popupFeatures,
                                  CefWindowInfo& windowInfo,
                                  CefRefPtr<CefClient>& client,
                                  CefBrowserSettings& settings,
                                  CefRefPtr<CefDictionaryValue>& extra_info,
                                  bool* no_javascript_access) {
    loadUrl(browser->GetIdentifier(), target_url);
    return true;
}

void WebviewHandler::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next)
{
    executeJavaScript(browser->GetIdentifier(), "document.activeElement.blur()");
}

bool WebviewHandler::OnSetFocus(CefRefPtr<CefBrowser> browser, FocusSource source)
{
    return false;
}

void WebviewHandler::OnGotFocus(CefRefPtr<CefBrowser> browser)
{
}

void WebviewHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                ErrorCode errorCode,
                                const CefString& errorText,
                                const CefString& failedUrl) {
    CEF_REQUIRE_UI_THREAD();

    // Don't display an error for downloaded files.
    if (errorCode == ERR_ABORTED)
        return;

    if (onLoadErrorEvent) {
        onLoadErrorEvent(
            browser->GetIdentifier(),
            errorCode,
            errorText.ToString(),
            failedUrl.ToString(),
            frame->IsMain()
        );
    }
}

void WebviewHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                 CefLoadHandler::TransitionType transition_type) {
    if (frame->IsMain()) {
        auto it = browser_map_.find(browser->GetIdentifier());
        if (it != browser_map_.end()) {
            browser->GetHost()->SetZoomLevel(ZoomLevelFor(it->second.dpi));
        }
    }
    if(onLoadStart){
        onLoadStart(browser->GetIdentifier(), frame->GetURL());
    }
    return;
}

void WebviewHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                               int httpStatusCode) {
    if(onLoadEnd){
        onLoadEnd(browser->GetIdentifier(), frame->GetURL());
    }
    return;
}

bool WebviewHandler::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefDownloadItem> download_item,
                                      const CefString& suggested_name,
                                      CefRefPtr<CefBeforeDownloadCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!download_item || !download_item->IsValid()) {
        return false;
    }
    uint32_t download_id = download_item->GetId();
    download_before_callbacks_[download_id] = callback;

    if (onBeforeDownloadEvent) {
        onBeforeDownloadEvent(
            browser->GetIdentifier(),
            download_id,
            download_item->GetURL().ToString(),
            suggested_name.ToString(),
            download_item->GetContentDisposition().ToString(),
            download_item->GetMimeType().ToString(),
            download_item->GetTotalBytes()
        );
    }
    return true;
}

void WebviewHandler::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefDownloadItem> download_item,
                                       CefRefPtr<CefDownloadItemCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    if (!download_item || !download_item->IsValid()) {
        return;
    }
    uint32_t download_id = download_item->GetId();
    download_item_callbacks_[download_id] = callback;

    if (onDownloadUpdatedEvent) {
        onDownloadUpdatedEvent(
            browser->GetIdentifier(),
            download_id,
            download_item->GetURL().ToString(),
            download_item->GetFullPath().ToString(),
            download_item->GetReceivedBytes(),
            download_item->GetTotalBytes(),
            download_item->GetCurrentSpeed(),
            download_item->GetPercentComplete(),
            download_item->IsInProgress(),
            download_item->IsComplete(),
            download_item->IsCanceled(),
            download_item->IsInterrupted(),
            static_cast<int>(download_item->GetInterruptReason())
        );
    }

    if (!download_item->IsInProgress()) {
        download_item_callbacks_.erase(download_id);
        download_before_callbacks_.erase(download_id);
    }
}

void WebviewHandler::continueDownload(uint32_t downloadId, const std::string& downloadPath, bool showDialog) {
    auto it = download_before_callbacks_.find(downloadId);
    if (it != download_before_callbacks_.end()) {
        if (it->second) {
            it->second->Continue(CefString(downloadPath), showDialog);
        }
        download_before_callbacks_.erase(it);
    }
}

void WebviewHandler::cancelDownload(uint32_t downloadId) {
    auto it = download_item_callbacks_.find(downloadId);
    if (it != download_item_callbacks_.end()) {
        if (it->second) {
            it->second->Cancel();
        }
    }
}

void WebviewHandler::pauseDownload(uint32_t downloadId) {
    auto it = download_item_callbacks_.find(downloadId);
    if (it != download_item_callbacks_.end()) {
        if (it->second) {
            it->second->Pause();
        }
    }
}

void WebviewHandler::resumeDownload(uint32_t downloadId) {
    auto it = download_item_callbacks_.find(downloadId);
    if (it != download_item_callbacks_.end()) {
        if (it->second) {
            it->second->Resume();
        }
    }
}

void WebviewHandler::CloseAllBrowsers(bool force_close) {
    if (browser_map_.empty()){
        return;
    }
    
    for (auto& it : browser_map_){
        it.second.browser->GetHost()->CloseBrowser(force_close);
        it.second.browser = nullptr;
    }
    browser_map_.clear();
}

void WebviewHandler::closeBrowser(int browserId)
{
    auto it = browser_map_.find(browserId);
    if(it != browser_map_.end()){
        it->second.browser->GetHost()->CloseBrowser(true);
        it->second.browser = nullptr;
        browser_map_.erase(it);
    }
}

void WebviewHandler::createBrowser(std::string url, bool isPrivate, std::function<void(int)> callback)
{
#ifndef OS_MAC
    if(!CefCurrentlyOn(TID_UI)) {
		CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::createBrowser, this, url, isPrivate, callback));
		return;
	}
#endif
    CefBrowserSettings browser_settings ;
    // Capped at 60 by CEF; ignored entirely when external begin frame drives the
    // frames (GPU path below). Kept for the software fallback path.
    browser_settings.windowless_frame_rate = 60;
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
#ifdef WEBVIEW_CEF_GPU_TEXTURE
    // Enable GPU shared-texture rendering: CEF delivers frames via
    // OnAcceleratedPaint (a D3D11 shared texture on Windows, an IOSurface on
    // macOS) instead of the software OnPaint CPU buffer. Requires the GPU to be
    // left enabled (see WebviewApp).
    window_info.shared_texture_enabled = true;
#if defined(_WIN32) || defined(OS_MAC)
    // Drive frame production ourselves so the rate can follow the display's
    // refresh (>60 Hz) instead of being capped at windowless_frame_rate. The
    // platform layer ticks SendExternalBeginFrame on each vblank — Windows from
    // an IDXGIOutput vblank wait, macOS from a CVDisplayLink.
    window_info.external_begin_frame_enabled = true;
#else
    window_info.external_begin_frame_enabled = false;
#endif
#endif

    // Create the Request Context
    CefRefPtr<CefRequestContext> request_context = nullptr;
    if (isPrivate) {
        CefRequestContextSettings context_settings;
        // Leaving context_settings.cache_path empty makes it "Incognito"
        request_context = CefRequestContext::CreateContext(context_settings, nullptr);
    }

    auto browser = CefBrowserHost::CreateBrowserSync(window_info, this, url, browser_settings, nullptr, request_context);
    if (!browser) {
        callback(-1);
        return;
    }
    callback(browser->GetIdentifier());
#ifdef WEBVIEW_CEF_GPU_TEXTURE
    // The GPU shared-texture path is the only render path on this build (no
    // OnPaint fallback). If no accelerated frame arrives shortly, the GPU
    // compositor likely can't export a shared texture — warn so a black webview
    // isn't silent.
    CefPostDelayedTask(TID_UI, base::BindOnce(&WebviewHandler::warnIfNoAcceleratedFrame, this), 5000);
#endif
}

void WebviewHandler::warnIfNoAcceleratedFrame() {
#ifdef WEBVIEW_CEF_GPU_TEXTURE
    if (!received_accelerated_frame_ && !gpu_warning_logged_) {
        gpu_warning_logged_ = true;
        fprintf(stderr,
                "[webview_cef] WARNING: no GPU accelerated-paint frame after 5s. "
                "This build renders the webview only via the GPU shared-texture "
                "path; if it appears black, the GPU compositor may be unavailable "
                "(headless, VM, software GL, or a crashed GPU process).\n");
        fflush(stderr);
    }
#endif
}

void WebviewHandler::sendExternalBeginFrame() {
#ifdef WEBVIEW_CEF_GPU_TEXTURE
    if (!CefCurrentlyOn(TID_UI)) {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::sendExternalBeginFrame, this));
        return;
    }
    for (auto& kv : browser_map_) {
        if (kv.second.browser && kv.second.browser->GetHost()) {
            kv.second.browser->GetHost()->SendExternalBeginFrame();
        }
    }
#endif
}

void WebviewHandler::sendScrollEvent(int browserId, int x, int y, int deltaX, int deltaY)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        const float dpi = it->second.dpi;
        CefMouseEvent ev;
        ev.x = ToView(x, dpi);
        ev.y = ToView(y, dpi);
        // Deltas are view pixels too; scaling keeps the CSS scroll distance.
        deltaX = ToView(deltaX, dpi);
        deltaY = ToView(deltaY, dpi);

#ifndef __APPLE__
        // The scrolling direction on Windows and Linux is different from MacOS
        deltaY = -deltaY;
#endif
        it->second.browser->GetHost()->SendMouseWheelEvent(ev, deltaX, deltaY);
    }
}

void WebviewHandler::sendTouchEvent(int browserId, const CefTouchEvent &ev)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end())
    {
        const float dpi = it->second.dpi;
        CefTouchEvent view_ev = ev;
        view_ev.x = ev.x * dpi;
        view_ev.y = ev.y * dpi;
        view_ev.radius_x = ev.radius_x * dpi;
        view_ev.radius_y = ev.radius_y * dpi;
        it->second.browser->GetHost()->SendTouchEvent(view_ev);
    }
}

void WebviewHandler::changeSize(int browserId, float a_dpi, int w, int h)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        // |dpi| holds the render scale from here on; every view <-> logical
        // conversion uses it, so a capped scale stays consistent.
        const float scale = RenderScale(a_dpi);
        const bool dpi_changed = it->second.dpi != scale;
        std::cerr << "webview_cef: browser " << browserId << " view " << w << "x"
                  << h << " @" << a_dpi << "x render @" << scale << "x -> CEF "
                  << ToView(w, scale) << "x" << ToView(h, scale) << std::endl;
        it->second.dpi = scale;
        it->second.width = w;
        it->second.height = h;
        it->second.browser->GetHost()->WasResized();
        if (dpi_changed) {
            it->second.browser->GetHost()->SetZoomLevel(ZoomLevelFor(scale));
        }
    }
}

void WebviewHandler::cursorClick(int browserId, int x, int y, bool up)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        CefMouseEvent ev;
        ev.x = ToView(x, it->second.dpi);
        ev.y = ToView(y, it->second.dpi);
        ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        if(up && it->second.is_dragging) {
            it->second.browser->GetHost()->DragTargetDrop(ev);
            it->second.browser->GetHost()->DragSourceSystemDragEnded();
            it->second.is_dragging = false;
        } else {
            it->second.browser->GetHost()->SendMouseClickEvent(ev, CefBrowserHost::MouseButtonType::MBT_LEFT, up, 1);
        }
    }
}

void WebviewHandler::cursorMove(int browserId, int x , int y, bool dragging)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        CefMouseEvent ev;
        ev.x = ToView(x, it->second.dpi);
        ev.y = ToView(y, it->second.dpi);
        if(dragging) {
            ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        }
        if(it->second.is_dragging && dragging) {
            it->second.browser->GetHost()->DragTargetDragOver(ev, DRAG_OPERATION_EVERY);
        } else {
            it->second.browser->GetHost()->SendMouseMoveEvent(ev, false);
        }
    }
}

bool WebviewHandler::StartDragging(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefDragData> drag_data,
                                  DragOperationsMask allowed_ops,
                                  int x,
                                  int y){
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end() && it->second.browser->IsSame(browser)) {
        CefMouseEvent ev;
        ev.x = x;
        ev.y = y;
        ev.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
        it->second.browser->GetHost()->DragTargetDragEnter(drag_data, ev, DRAG_OPERATION_EVERY);
        it->second.is_dragging = true;
    }
    return true;
}

void WebviewHandler::OnImeCompositionRangeChanged(CefRefPtr<CefBrowser> browser, const CefRange &selection_range, const CefRenderHandler::RectList &character_bounds)
{
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if(it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup()) {
        return;
    }
    if (!character_bounds.empty()) {
        if (it->second.is_ime_commit) {
            auto lastCharacter = character_bounds.back();
            it->second.prev_ime_position = lastCharacter;
            const float dpi = it->second.dpi;
            onImeCompositionRangeChangedMessage(browser->GetIdentifier(), ToLogical(lastCharacter.x + lastCharacter.width, dpi), ToLogical(lastCharacter.y + lastCharacter.height, dpi), ToLogical(lastCharacter.height, dpi));
            it->second.is_ime_commit = false;
        }
        else
        {
            auto firstCharacter = character_bounds.front();
            if (firstCharacter != it->second.prev_ime_position) {
                it->second.prev_ime_position = firstCharacter;
                const float dpi = it->second.dpi;
                onImeCompositionRangeChangedMessage(browser->GetIdentifier(), ToLogical(firstCharacter.x, dpi), ToLogical(firstCharacter.y + firstCharacter.height, dpi), ToLogical(firstCharacter.height, dpi));
            }
        }
    }
}

void WebviewHandler::sendKeyEvent(CefKeyEvent& ev)
{
    auto browser = current_focused_browser_;
    if (!browser.get()) {
        return;
    }
    browser->GetHost()->SendKeyEvent(ev);
}

void WebviewHandler::loadUrl(int browserId, std::string url)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        it->second.browser->GetMainFrame()->LoadURL(url);
    }
}

void WebviewHandler::goForward(int browserId) {
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        it->second.browser->GetMainFrame()->GetBrowser()->GoForward();
    }
}

void WebviewHandler::goBack(int browserId) {
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        it->second.browser->GetMainFrame()->GetBrowser()->GoBack();
    }
}

void WebviewHandler::reload(int browserId) {
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        it->second.browser->GetMainFrame()->GetBrowser()->Reload();
    }
}

void WebviewHandler::wasHidden(int browserId, bool hidden)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
    {
        return;
    }
    it->second.browser->GetHost()->WasHidden(hidden);
}
bool WebviewHandler::canGoForward(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        return it->second.browser->CanGoForward();
    }
    return false;
}

bool WebviewHandler::canGoBack(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get())
    {
        return it->second.browser->CanGoBack();
    }
    return false;
}

void WebviewHandler::openDevTools(int browserId) {
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end()) {
        CefWindowInfo windowInfo;
#ifdef OS_WIN
        windowInfo.SetAsPopup(nullptr, "DevTools");
#endif
        it->second.browser->GetHost()->ShowDevTools(windowInfo, this, CefBrowserSettings(), CefPoint());
    }
}

void WebviewHandler::imeSetComposition(int browserId, std::string text)
{
    auto it = browser_map_.find(browserId);
    if (it==browser_map_.end() || !it->second.browser.get()) {
        return;
    }

    CefString cTextStr = CefString(text);

    std::vector<CefCompositionUnderline> underlines;
    cef_composition_underline_t underline = {};
    underline.range.from = 0;
    underline.range.to = static_cast<int>(0 + cTextStr.length());
    underline.color = CefColorSetARGB(255, 0, 0, 0);
    underline.background_color = CefColorSetARGB(0, 0, 0, 0);
    underline.thick = 0;
    underline.style = CEF_CUS_DOT;
    underlines.push_back(underline);

    // Keeps the caret at the end of the composition
    auto selection_range_end = static_cast<int>(0 + cTextStr.length());
    // Keep the caret at the end of the composition (matches cefclient).
    CefRange selection_range = CefRange(selection_range_end, selection_range_end);
    it->second.browser->GetHost()->ImeSetComposition(cTextStr, underlines, CefRange(UINT32_MAX, UINT32_MAX), selection_range);
}

void WebviewHandler::imeCommitText(int browserId, std::string text)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end() || !it->second.browser.get())
    {
        return;
    }

    CefString cTextStr = CefString(text);
    if (cTextStr.empty())
    {
        return;
    }
    it->second.is_ime_commit = true;

    // Commit the text directly to the focused web input element
    it->second.browser->GetHost()->ImeCommitText(cTextStr, CefRange(UINT32_MAX, UINT32_MAX), 0);
}

void WebviewHandler::setClientFocus(int browserId, bool focus)
{
    auto it = browser_map_.find(browserId);
    if (it==browser_map_.end() || !it->second.browser.get()) {
        return;
    }
    it->second.wants_focus = focus;
    if (focus) {
        // Re-arm the first-frame re-assert (handles a SetFocus that lands before
        // the browser is render/input-ready under external_begin_frame).
        it->second.focus_reasserted = false;
    }
    it->second.browser->GetHost()->SetFocus(focus);
}

CefRefPtr<CefBrowser> WebviewHandler::getFocusedBrowser()
{
    return current_focused_browser_;
}

void WebviewHandler::imeSetCompositionNative(const std::wstring& text, int cursor)
{
    if (!CefCurrentlyOn(TID_UI)) {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::imeSetCompositionNative, this, text, cursor));
        return;
    }
    auto browser = current_focused_browser_;
    if (!browser.get()) {
        return;
    }
    CefString cTextStr;
    cTextStr.FromWString(text);

    std::vector<CefCompositionUnderline> underlines;
    cef_composition_underline_t underline = {};
    underline.range.from = 0;
    underline.range.to = static_cast<int>(cTextStr.length());
    underline.color = CefColorSetARGB(255, 0, 0, 0);
    underline.background_color = CefColorSetARGB(0, 0, 0, 0);
    underline.thick = 0;
    underline.style = CEF_CUS_DOT;
    underlines.push_back(underline);

    const int len = static_cast<int>(cTextStr.length());
    const int caret = (cursor < 0 || cursor > len) ? len : cursor;
    CefRange selection_range(caret, caret);
    browser->GetHost()->ImeSetComposition(cTextStr, underlines,
        CefRange(UINT32_MAX, UINT32_MAX), selection_range);
}

void WebviewHandler::imeCommitTextNative(const std::wstring& text)
{
    if (!CefCurrentlyOn(TID_UI)) {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::imeCommitTextNative, this, text));
        return;
    }
    auto browser = current_focused_browser_;
    if (!browser.get()) {
        return;
    }
    CefString cTextStr;
    cTextStr.FromWString(text);
    browser->GetHost()->ImeCommitText(cTextStr, CefRange(UINT32_MAX, UINT32_MAX), 0);
}

void WebviewHandler::imeFinishComposition()
{
    if (!CefCurrentlyOn(TID_UI)) {
        CefPostTask(TID_UI, base::BindOnce(&WebviewHandler::imeFinishComposition, this));
        return;
    }
    auto browser = current_focused_browser_;
    if (!browser.get()) {
        return;
    }
    browser->GetHost()->ImeFinishComposingText(false);
}

void WebviewHandler::setCookie(const std::string& domain, const std::string& key, const std::string& value){
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if(manager){
        CefCookie cookie;
		CefString(&cookie.path).FromASCII("/");
		CefString(&cookie.name).FromString(key.c_str());
		CefString(&cookie.value).FromString(value.c_str());

		if (!domain.empty()) {
			CefString(&cookie.domain).FromString(domain.c_str());
		}

		cookie.httponly = true;
		cookie.secure = false;
		std::string httpDomain = "https://" + domain + "/cookiestorage";
		manager->SetCookie(httpDomain, cookie, nullptr);
    }
}

void WebviewHandler::deleteCookie(const std::string& domain, const std::string& key)
{
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager) {
        std::string httpDomain = "https://" + domain + "/cookiestorage";
        manager->DeleteCookies(httpDomain, key, nullptr);
    }
}

void WebviewHandler::visitAllCookies(std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback){
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
	{
		return;
	}

    CefRefPtr<WebviewCookieVisitor> cookieVisitor = new WebviewCookieVisitor();
    cookieVisitor->setOnVisitComplete(callback);

    manager->VisitAllCookies(cookieVisitor);
}

void WebviewHandler::visitUrlCookies(const std::string& domain, const bool& isHttpOnly, std::function<void(std::map<std::string, std::map<std::string, std::string>>)> callback){
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (!manager)
	{
		return;
	}

    CefRefPtr<WebviewCookieVisitor> cookieVisitor = new WebviewCookieVisitor();
    cookieVisitor->setOnVisitComplete(callback);

    std::string httpDomain = "https://" + domain + "/cookiestorage";

    manager->VisitUrlCookies(httpDomain, isHttpOnly, cookieVisitor);
}

void WebviewHandler::setJavaScriptChannels(int browserId, const std::vector<std::string> channels)
{
    std::string extensionCode = "try{";
    for(auto& channel : channels)
    {
        extensionCode += channel;
        extensionCode += " = (e,r) => {external.JavaScriptChannel('";
        extensionCode += channel;
        extensionCode += "',e,r)};";
    }
    extensionCode += "}catch(e){console.log(e);}";
    executeJavaScript(browserId, extensionCode);
}

void WebviewHandler::sendJavaScriptChannelCallBack(const bool error, const std::string result, const std::string callbackId, const int browserId, const std::string frameId)
{
    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kExecuteJsCallbackMessage);
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    args->SetInt(0, atoi(callbackId.c_str()));
    args->SetBool(1, error);
    args->SetString(2, result);
    auto bit = browser_map_.find(browserId);
    if(bit != browser_map_.end()){
        int64_t frameIdInt = atoll(frameId.c_str());

        CefRefPtr<CefFrame> frame = bit->second.browser->GetMainFrame();

        // CefFrame::GetIdentifier() returns a string identifier in current CEF
        // (since CEF 122) on every platform.
        bool identifierMatch = std::stoll(frame->GetIdentifier().ToString()) == frameIdInt;
        if (identifierMatch)
        {
            frame->SendProcessMessage(PID_RENDERER, message);
        }
    }
}

static std::string GetCallbackId()
{
    auto time = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
	time_t timestamp = time.time_since_epoch().count();
    return std::to_string(timestamp);
} 

void WebviewHandler::executeJavaScript(int browserId, const std::string code, std::function<void(CefRefPtr<CefValue>)> callback)
{
    if(!code.empty())
    {
        auto bit = browser_map_.find(browserId);
        if(bit != browser_map_.end() && bit->second.browser.get()){
            CefRefPtr<CefFrame> frame = bit->second.browser->GetMainFrame();
            if (frame)
            {
                std::string finalCode = code;
                if(callback != nullptr){
                    std::string callbackId = GetCallbackId();

                    finalCode = "external.EvaluateCallback('";
                    finalCode += callbackId;
                    finalCode += "',(function(){return ";
                    finalCode += code;
                    finalCode += "})());";
                    js_callbacks_[callbackId] = callback;
                }
			    frame->ExecuteJavaScript(finalCode, frame->GetURL(), 0);
            }
        }
    }
}

void WebviewHandler::captureScreenshot(int browserId)
{
    auto it = browser_map_.find(browserId);
    if (it != browser_map_.end() && it->second.browser.get()) {
        it->second.capture_requested = true;
        
        // --- PROACTIVE FIX: Check if we already have a buffer to capture from.
        // OnPaint is only called when something changes. If the page is static,
        // it might not be called for a while.
        it->second.browser->GetHost()->Invalidate(PET_VIEW);
    }
}

void WebviewHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) {
    CEF_REQUIRE_UI_THREAD();
    auto it = browser_map_.find(browser->GetIdentifier());
    if(it == browser_map_.end() || !it->second.browser.get() || browser->IsPopup()) {
        return;
    }
    rect.x = rect.y = 0;
    
    rect.width = std::max(1, ToView(it->second.width, it->second.dpi));
    rect.height = std::max(1, ToView(it->second.height, it->second.dpi));
}

bool WebviewHandler::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    // HiDPI is done by view size + page zoom (see ToView); the accelerated OSR
    // path ignores device_scale_factor, so CEF keeps its default of 1.
    return false;
}

void write_to_vector(png_structp png_ptr, png_bytep data, png_size_t length) {
    auto* buf = static_cast<std::vector<unsigned char>*>(png_get_io_ptr(png_ptr));
    buf->insert(buf->end(), data, data + length);
}

void WebviewHandler::OnPaint(CefRefPtr<CefBrowser> browser, CefRenderHandler::PaintElementType type,
                             const CefRenderHandler::RectList &dirtyRects, const void *buffer, int w, int h)
{
#ifdef WEBVIEW_CEF_GPU_TEXTURE
    fprintf(stderr,
            "[webview_cef] WARNING: no GPU accelerated-paint frame. \n");
    fflush(stderr);
#endif
    if (!browser->IsPopup())
    {
        int browserId = browser->GetIdentifier();
        auto it = browser_map_.find(browserId);
        if (it != browser_map_.end())
        {
            if (it->second.capture_requested)
            {
                it->second.capture_requested = false;

                // --- Resize + BGRA->RGB pack happens HERE, on the UI/paint thread,
                // directly from the live `buffer` — no full-frame copy first.
                int target_h = h;
                int target_w = w;
                int scale = 1;
                if (h > 480)
                {
                    scale = (h + 479) / 480;
                    target_h = h / scale;
                    target_w = w / scale;
                }

                auto small = std::make_shared<std::vector<unsigned char>>(
                    (size_t)target_w * target_h * 3);

                const unsigned char *src = static_cast<const unsigned char *>(buffer);
                unsigned char *dst = small->data();

                for (int y = 0; y < target_h; y++)
                {
                    int src_y = y * scale;
                    for (int x = 0; x < target_w; x++)
                    {
                        int src_x = x * scale;
                        int srcIdx = (src_y * w + src_x) * 4; // BGRA
                        int dstIdx = (y * target_w + x) * 3;
                        dst[dstIdx + 0] = src[srcIdx + 2]; // R
                        dst[dstIdx + 1] = src[srcIdx + 1]; // G
                        dst[dstIdx + 2] = src[srcIdx + 0]; // B
                    }
                }

                // Background thread encodes to memory and invokes the Dart callback
                std::thread([small, browserId, target_w, target_h, cb = onCaptureCompleteCallback]()
                            {
                    std::vector<unsigned char> pngBuf;
                    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                    if (!png_ptr) {
                        if (cb) cb(browserId, false, nullptr, 0);
                        return;
                    }

                    png_infop info_ptr = png_create_info_struct(png_ptr);
                    if (!info_ptr) {
                        png_destroy_write_struct(&png_ptr, NULL);
                        if (cb) cb(browserId, false, nullptr, 0);
                        return;
                    }

                    if (setjmp(png_jmpbuf(png_ptr))) {
                        png_destroy_write_struct(&png_ptr, &info_ptr);
                        if (cb) cb(browserId, false, nullptr, 0);
                        return;
                    }

                    png_set_write_fn(png_ptr, &pngBuf, write_to_vector, NULL);
                    png_set_IHDR(png_ptr, info_ptr, target_w, target_h,
                                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

                    png_set_compression_level(png_ptr, 6);
                    png_set_compression_strategy(png_ptr, Z_RLE);
                    png_set_filter(png_ptr, 0, PNG_FILTER_SUB);

                    png_write_info(png_ptr, info_ptr);

                    const unsigned char* px = small->data();
                    for (int y = 0; y < target_h; y++) {
                        png_write_row(png_ptr, const_cast<png_bytep>(px + (size_t)y * target_w * 3));
                    }

                    png_write_end(png_ptr, NULL);
                    
                    if (cb) {
                        cb(browserId, true, pngBuf.data(), pngBuf.size());
                    }

                    png_destroy_write_struct(&png_ptr, &info_ptr);
                }).detach();
            }
        }
        
        if (onPaintCallback != nullptr) {
            onPaintCallback(browser->GetIdentifier(), buffer, w, h);
        }
    }
}

void WebviewHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
{
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end()) {
        return;
    }
    it->second.popup_visible = show;
    if (!show) {
        it->second.popup_rect = CefRect();
        // The popup area was never part of the view frame, so nothing needs
        // repainting; the host just stops drawing the popup.
    }
    if (onPopupChanged) {
        onPopupChanged(browser->GetIdentifier(), show,
                       ToLogical(it->second.popup_rect, it->second.dpi));
    }
}

void WebviewHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end()) {
        return;
    }
    it->second.popup_rect = rect;
    if (it->second.popup_visible && onPopupChanged) {
        onPopupChanged(browser->GetIdentifier(), true,
                       ToLogical(rect, it->second.dpi));
    }
}

void WebviewHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, CefRenderHandler::PaintElementType type,
                                        const CefRenderHandler::RectList &dirtyRects, const CefAcceleratedPaintInfo &info)
{
    if (type == PET_VIEW) {  // FPSDBG
        static fpsdbg::Counter counter("cef paints");
        counter.Tick();
    }
#ifdef WEBVIEW_CEF_GPU_TEXTURE
#if defined(OS_LINUX)
    if (type == PET_POPUP)
    {
        auto it = browser_map_.find(browser->GetIdentifier());
        if (it != browser_map_.end() && it->second.popup_visible &&
            onAcceleratedPopupPaintCallback != nullptr)
        {
            // Popup frames are sized like the popup rect, in view pixels.
            const CefRect &rect = it->second.popup_rect;
            onAcceleratedPopupPaintCallback(browser->GetIdentifier(), info,
                                            dirtyRects, rect.width, rect.height);
        }
        return;
    }
#else
    if (type == PET_POPUP)
    {
        return;
    }
#endif
    if (!browser->IsPopup() && onAcceleratedPaintCallback != nullptr)
    {
        received_accelerated_frame_ = true;
        int w = 0, h = 0;
        auto it = browser_map_.find(browser->GetIdentifier());
        if (it != browser_map_.end()) {
            w = ToView(it->second.width, it->second.dpi);
            h = ToView(it->second.height, it->second.dpi);
            // A produced frame means the browser is render/input-ready. With
            // external_begin_frame a SetFocus issued right after creation can be
            // dropped (the browser wasn't ready yet), which left the webview
            // unable to receive keyboard input until the window was re-focused.
            // Re-apply the requested focus once, now that frames are flowing.
            if (it->second.wants_focus && !it->second.focus_reasserted) {
                it->second.focus_reasserted = true;
                it->second.browser->GetHost()->SetFocus(true);
            }
        }
        // The shared texture is pool-owned and only valid for the duration of
        // this callback. On Windows it is a HANDLE (open with
        // ID3D11Device1::OpenSharedResource1); on macOS it is an IOSurfaceRef.
        // The platform renderer wraps/copies it before returning.
#ifdef __APPLE__
        const void *sharedTexture = reinterpret_cast<const void *>(info.shared_texture_io_surface);
#elif defined(OS_LINUX)
        const void *sharedTexture = nullptr;
#else
        const void *sharedTexture = reinterpret_cast<const void *>(info.shared_texture_handle);
#endif
        onAcceleratedPaintCallback(browser->GetIdentifier(), sharedTexture,
                                   w, h, static_cast<int>(info.format));
#if defined(OS_LINUX)
        if (onAcceleratedPaintInfoCallback != nullptr)
        {
            onAcceleratedPaintInfoCallback(browser->GetIdentifier(), info, dirtyRects, w, h);
        }
#endif
    }
#endif
}
// ---------------------------------------------------------------------------
// Touch selection (long press). selects the text, tracks the handles
// and decides which edit actions are allowed; we only forward that state to
// Dart, which draws the handles and the cut/copy/paste toolbar.
// ---------------------------------------------------------------------------

// Size Chromium uses to hit-test the touch handles; the Dart side draws them
// at the same size.
static constexpr int kTouchHandleSize = 32;

// Actions from Dart (quickMenuAction). Keep in sync with webview.dart.
enum QuickMenuAction {
    kQuickMenuDismiss = 0,
    kQuickMenuCut = 1,
    kQuickMenuCopy = 2,
    kQuickMenuPaste = 3,
    kQuickMenuSelectAll = 4,
};

bool WebviewHandler::RunContextMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback)
{
    // No context menu: a long press is handled as touch selection instead.
    callback->Cancel();
    return true;
}

bool WebviewHandler::RunQuickMenu(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefPoint& location, const CefSize& size, QuickMenuEditStateFlags edit_state_flags, CefRefPtr<CefRunQuickMenuCallback> callback)
{
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it == browser_map_.end()) {
        return false;
    }
    if (it->second.quick_menu_callback) {
        it->second.quick_menu_callback->Cancel();
    }
    it->second.quick_menu_callback = callback;
    if (onQuickMenuEvent) {
        onQuickMenuEvent(browser->GetIdentifier(), true, location.x, location.y, size.width, size.height, edit_state_flags);
    }
    return true;
}

void WebviewHandler::OnQuickMenuDismissed(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
    auto it = browser_map_.find(browser->GetIdentifier());
    if (it != browser_map_.end()) {
        it->second.quick_menu_callback = nullptr;
    }
    if (onQuickMenuEvent) {
        onQuickMenuEvent(browser->GetIdentifier(), false, 0, 0, 0, 0, 0);
    }
}

void WebviewHandler::GetTouchHandleSize(CefRefPtr<CefBrowser> browser, cef_horizontal_alignment_t orientation, CefSize& size)
{
    size = CefSize(kTouchHandleSize, kTouchHandleSize);
}

void WebviewHandler::OnTouchHandleStateChanged(CefRefPtr<CefBrowser> browser, const CefTouchHandleState& state)
{
    if (onTouchHandleEvent) {
        onTouchHandleEvent(browser->GetIdentifier(), state.touch_handle_id, state.flags, state.enabled,
                           state.orientation, state.origin.x, state.origin.y, state.alpha);
    }
}

void WebviewHandler::quickMenuAction(int browserId, int action)
{
    auto it = browser_map_.find(browserId);
    if (it == browser_map_.end()) {
        return;
    }
    if (action == kQuickMenuSelectAll) {
        // Not a quick menu command: select everything and let Chromium show a
        // new quick menu for the new selection.
        it->second.browser->GetFocusedFrame()->SelectAll();
        return;
    }
    CefRefPtr<CefRunQuickMenuCallback> callback = it->second.quick_menu_callback;
    if (!callback) {
        return;
    }
    it->second.quick_menu_callback = nullptr;
    switch (action) {
        case kQuickMenuCut: callback->Continue(QM_EDITFLAG_CAN_CUT, EVENTFLAG_NONE); break;
        case kQuickMenuCopy: callback->Continue(QM_EDITFLAG_CAN_COPY, EVENTFLAG_NONE); break;
        case kQuickMenuPaste: callback->Continue(QM_EDITFLAG_CAN_PASTE, EVENTFLAG_NONE); break;
        default: callback->Cancel(); break;
    }
}
