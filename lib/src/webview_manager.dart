import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:webview_cef/src/webview_inject_user_script.dart';

import 'webview.dart';

import 'dart:ffi';
import 'package:ffi/ffi.dart';

typedef CaptureCompleteCallbackNative = Void Function(
    Int32 browserId, Bool success, Pointer<Uint8> data, IntPtr size);
typedef CaptureCompleteCallbackDart = void Function(
    int browserId, bool success, Pointer<Uint8> data, int size);

class WebviewManager extends ValueNotifier<bool> {
  static final WebviewManager _instance = WebviewManager._internal();

  factory WebviewManager() => _instance;

  late Completer<void> _creatingCompleter;

  final MethodChannel pluginChannel = const MethodChannel("webview_cef");

  final _webViews = <int, WebViewController>{};
  final _injectUserScripts = <int, InjectUserScripts?>{};

  final _tempWebViews = <int, WebViewController>{};
  final _tempInjectUserScripts = <int, InjectUserScripts?>{};

  int nextIndex = 1;

  bool? _hasNativeKeySupport;

  final Map<int, Completer<Uint8List?>> _pendingCaptures = {};

  late final _captureCallback = NativeCallable<CaptureCompleteCallbackNative>.listener(_onCaptureComplete);

  void _onCaptureComplete(int browserId, bool success, Pointer<Uint8> data, int size) {
    final completer = _pendingCaptures.remove(browserId);
    if (completer == null) return;
    completer.complete(success ? data.asTypedList(size) : null);
  }

  void _registerCaptureCallback() {
    try {
      String libName = 'libwebview_cef_plugin.so';
      if (Platform.isWindows) libName = 'webview_cef_plugin.dll';
      if (Platform.isMacOS) libName = 'webview_cef_plugin.framework/webview_cef_plugin';

      final DynamicLibrary lib = DynamicLibrary.open(libName);
      final void Function(Pointer<NativeFunction<CaptureCompleteCallbackNative>>)
          setCallback = lib.lookupFunction<
                  Void Function(
                      Pointer<NativeFunction<CaptureCompleteCallbackNative>>),
                  void Function(
                      Pointer<NativeFunction<CaptureCompleteCallbackNative>>)>('webview_cef_set_capture_complete_callback');
      setCallback(_captureCallback.nativeFunction);
    } catch (e) {
      debugPrint('Failed to register capture callback: $e');
    }
  }

  Future<Uint8List?> captureScreenshot(int browserId) async {
    final completer = Completer<Uint8List?>();
    _pendingCaptures[browserId] = completer;

    await pluginChannel.invokeMethod('captureScreenshot', [browserId, '']);

    return completer.future.timeout(
      const Duration(seconds: 5),
      onTimeout: () {
        _pendingCaptures.remove(browserId);
        return null;
      },
    );
  }

  get ready => _creatingCompleter.future;

  /// Returns true if the platform has native key event handling (e.g., GTK on desktop Linux).
  /// When false, Dart-side key handling should be used (e.g., eLinux).
  Future<bool> get hasNativeKeySupport async {
    _hasNativeKeySupport ??= await pluginChannel.invokeMethod<bool>('hasNativeKeySupport') ?? false;
    return _hasNativeKeySupport!;
  }

  WebViewController createWebView({
    Widget? loading,
    InjectUserScripts? injectUserScripts,
  }) {
    int browserIndex = nextIndex++;
    final controller =
        WebViewController(pluginChannel, browserIndex, loading: loading);
    _tempWebViews[browserIndex] = controller;
    _tempInjectUserScripts[browserIndex] = injectUserScripts;

    return controller;
  }

  void removeWebView(int browserId) {
    if (browserId > 0) {
      _webViews.remove(browserId);
    }
  }

  WebviewManager._internal() : super(false);

  Future<void> initialize({String? userAgent}) async {
    _creatingCompleter = Completer<void>();
    try {
      if (userAgent != null && userAgent.isNotEmpty) {
        await pluginChannel.invokeMethod('init', userAgent);
      } else {
        await pluginChannel.invokeMethod('init');
      }
      pluginChannel.setMethodCallHandler(methodCallhandler);
      _registerCaptureCallback();
      // Wait for the platform to complete initialization.
      await Future.delayed(const Duration(milliseconds: 300));
      _creatingCompleter.complete();
      value = true;
    } on PlatformException catch (e) {
      _creatingCompleter.completeError(e);
    }
    return _creatingCompleter.future;
  }

  @override
  Future<void> dispose() async {
    super.dispose();
    pluginChannel.setMethodCallHandler(null);
    _webViews.clear();
  }

  void onBrowserCreated(int browserIndex, int browserId) {
    _webViews[browserId] = _tempWebViews[browserIndex]!;
    _injectUserScripts[browserId] = _tempInjectUserScripts[browserIndex];

    _tempWebViews.remove(browserIndex);
    _tempInjectUserScripts.remove(browserIndex);
  }

  Future<void> methodCallhandler(MethodCall call) async {
    switch (call.method) {
      case "urlChanged":
        int browserId = call.arguments["browserId"] as int;
        _webViews[browserId]
            ?.listener
            ?.onUrlChanged
            ?.call(call.arguments["url"] as String);
        return;
      case "titleChanged":
        int browserId = call.arguments["browserId"] as int;
        _webViews[browserId]
            ?.listener
            ?.onTitleChanged
            ?.call(call.arguments["title"] as String);
        return;
      case "onConsoleMessage":
        int browserId = call.arguments["browserId"] as int;
        _webViews[browserId]?.listener?.onConsoleMessage?.call(
            call.arguments["level"] as int,
            call.arguments["message"] as String,
            call.arguments["source"] as String,
            call.arguments["line"] as int);
        return;
      case 'javascriptChannelMessage':
        int browserId = call.arguments['browserId'] as int;
        _webViews[browserId]?.onJavascriptChannelMessage?.call(
            call.arguments['channel'] as String,
            call.arguments['message'] as String,
            call.arguments['callbackId'] as String,
            call.arguments['frameId'] as String);
        return;
      case 'onTooltip':
        int browserId = call.arguments['browserId'] as int;
        _webViews[browserId]?.onToolTip?.call(call.arguments['text'] as String);
        return;
      case 'onCursorChanged':
        int browserId = call.arguments['browserId'] as int;
        _webViews[browserId]
            ?.onCursorChanged
            ?.call(call.arguments['type'] as int);
        return;
      case 'onFocusedNodeChangeMessage':
        int browserId = call.arguments['browserId'] as int;
        bool editable = call.arguments['editable'] as bool;
        _webViews[browserId]?.onFocusedNodeChangeMessage(editable);
        return;
      case 'onImeCompositionRangeChangedMessage':
        int browserId = call.arguments['browserId'] as int;
        _webViews[browserId]?.onImeCompositionRangeChangedMessage?.call(
            call.arguments['x'] as int,
            call.arguments['y'] as int,
            call.arguments['height'] as int);
        return;
      case 'onLoadStart':
        int browserId = call.arguments["browserId"] as int;
        String urlId = call.arguments["urlId"] as String;

        await _injectUserScriptIfNeeds(browserId, _injectUserScripts[browserId]?.retrieveLoadStartInjectScripts() ?? []);

        WebViewController controller =
        _webViews[browserId] as WebViewController;
        _webViews[browserId]?.listener?.onLoadStart?.call(controller, urlId);
        return;
      case 'onLoadEnd':
        int browserId = call.arguments["browserId"] as int;
        String urlId = call.arguments["urlId"] as String;

        await _injectUserScriptIfNeeds(browserId, _injectUserScripts[browserId]?.retrieveLoadEndInjectScripts() ?? []);

        WebViewController controller =
        _webViews[browserId] as WebViewController;
        _webViews[browserId]?.listener?.onLoadEnd?.call(controller, urlId);
        return;
      case 'onLoadError':
        int browserId = call.arguments['browserId'] as int;
        WebViewController? controller = _webViews[browserId];
        if (controller != null) {
          controller.listener?.onLoadError?.call(
            controller,
            call.arguments['errorCode'] as int? ?? 0,
            call.arguments['errorText'] as String? ?? '',
            call.arguments['failedUrl'] as String? ?? '',
            call.arguments['isMainFrame'] as bool? ?? true,
          );
        }
        return;
      case 'onBeforeDownload':
        int browserId = call.arguments['browserId'] as int;
        WebViewController? controller = _webViews[browserId];
        if (controller != null) {
          controller.listener?.onBeforeDownload?.call(
            controller,
            call.arguments['downloadId'] as int,
            call.arguments['url'] as String,
            call.arguments['suggestedName'] as String,
            call.arguments['contentDisposition'] as String,
            call.arguments['mimeType'] as String,
            call.arguments['totalBytes'] as int,
          );
        }
        return;
      case 'onDownloadUpdated':
        int browserId = call.arguments['browserId'] as int;
        WebViewController? controller = _webViews[browserId];
        if (controller != null) {
          controller.listener?.onDownloadUpdated?.call(
            controller,
            call.arguments['downloadId'] as int,
            call.arguments['url'] as String,
            call.arguments['fullPath'] as String,
            call.arguments['receivedBytes'] as int,
            call.arguments['totalBytes'] as int,
            call.arguments['currentSpeed'] as int,
            call.arguments['percentComplete'] as int,
            call.arguments['isInProgress'] as bool,
            call.arguments['isComplete'] as bool,
            call.arguments['isCanceled'] as bool,
            call.arguments['isInterrupted'] as bool,
            call.arguments['interruptReason'] as int,
          );
        }
        return;
      default:
    }
  }

  Future<void> continueDownload(int downloadId, String downloadPath,
      {bool showDialog = false}) async {
    assert(value);
    return pluginChannel.invokeMethod(
        'continueDownload', [downloadId, downloadPath, showDialog]);
  }

  Future<void> cancelDownload(int downloadId) async {
    assert(value);
    return pluginChannel.invokeMethod('cancelDownload', downloadId);
  }

  Future<void> pauseDownload(int downloadId) async {
    assert(value);
    return pluginChannel.invokeMethod('pauseDownload', downloadId);
  }

  Future<void> resumeDownload(int downloadId) async {
    assert(value);
    return pluginChannel.invokeMethod('resumeDownload', downloadId);
  }

  Future<void> _injectUserScriptIfNeeds(
      int browserId, List<UserScript> scripts) async {
    if (scripts.isEmpty) return;

    await _webViews[browserId]?.ready;

    for (final script in scripts) {
      await _webViews[browserId]?.executeJavaScript(script.script);
    }
  }

  Future<void> setCookie(String domain, String key, String val) async {
    assert(value);
    return pluginChannel.invokeMethod('setCookie', [domain, key, val]);
  }

  Future<void> deleteCookie(String domain, String key) async {
    assert(value);
    return pluginChannel.invokeMethod('deleteCookie', [domain, key]);
  }

  Future<dynamic> visitAllCookies() async {
    assert(value);
    return pluginChannel.invokeMethod('visitAllCookies');
  }

  Future<dynamic> visitUrlCookies(String domain, bool isHttpOnly) async {
    assert(value);
    return pluginChannel.invokeMethod('visitUrlCookies', [domain, isHttpOnly]);
  }

  Future<void> quit() async {
    //only call this method when you want to quit the app
    assert(value);
    return pluginChannel.invokeMethod('quit');
  }
}
