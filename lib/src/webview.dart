import 'dart:async';
import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'webview_events_listener.dart';
import 'webview_javascript.dart';
import 'webview_manager.dart';
import 'webview_textinput.dart';
import 'webview_tooltip.dart';

// CEF key event types
const int keyEventRawKeyDown = 0;
const int keyEventKeyDown = 1;
const int keyEventKeyUp = 2;
const int keyEventChar = 3;

// CEF event flags
const int eventFlagNone = 0;
const int eventFlagCapsLockOn = 1 << 0;
const int eventFlagShiftDown = 1 << 1;
const int eventFlagControlDown = 1 << 2;
const int eventFlagAltDown = 1 << 3;
const int eventFlagLeftMouseButton = 1 << 4;
const int eventFlagMiddleMouseButton = 1 << 5;
const int eventFlagRightMouseButton = 1 << 6;
const int eventFlagCommandDown = 1 << 7;
const int eventFlagNumLockOn = 1 << 8;
const int eventFlagIsKeyPad = 1 << 9;
const int eventFlagIsLeft = 1 << 10;
const int eventFlagIsRight = 1 << 11;

class WebViewController extends ValueNotifier<bool> {
  WebViewController(this._pluginChannel, this._index, {Widget? loading})
      : super(false) {
    _loadingWidget = loading;
  }
  final MethodChannel _pluginChannel;
  Widget? _loadingWidget;

  late WebView _webviewWidget;
  Widget get webviewWidget => _webviewWidget;
  Widget get loadingWidget => _loadingWidget ?? const Text("loading...");

  Completer<void>? _creatingCompleter;
  Future<void> get ready => _creatingCompleter?.future ?? Future<void>.value();
  bool _isDisposed = false;
  bool _focusEditable = false;

  final int _index;
  late int _browserId;
  late int _textureId;
  int get textureId => _textureId;
  final Map<String, JavascriptChannel> _javascriptChannels =
      <String, JavascriptChannel>{};
  Map<String, JavascriptChannel> get javascriptChannels => _javascriptChannels;
  WebviewEventsListener? _listener;
  WebviewEventsListener? get listener => _listener;

  get onJavascriptChannelMessage => (final String channelName,
          final String message, final String callbackId, final String frameId) {
        if (_javascriptChannels.containsKey(channelName)) {
          _javascriptChannels[channelName]!.onMessageReceived(
              JavascriptMessage(message, callbackId, frameId));
        } else {
          debugPrint('Channel "$channelName" is not exists');
        }
      };

  get onToolTip => _onToolTip;
  get onCursorChanged => _onCursorChanged;
  get onFocusedNodeChangeMessage => _onFocusedNodeChangeMessage;
  get onImeCompositionRangeChangedMessage =>
      _onImeCompositionRangeChangedMessage;

  /// Initializes the underlying platform view.
  Future<void> initialize(String url, {bool isPrivate = false}) async {
    if (_isDisposed) {
      return Future<void>.value();
    }
    final existingCompleter = _creatingCompleter;
    if (existingCompleter != null) {
      return existingCompleter.future;
    }

    final creatingCompleter = Completer<void>();
    _creatingCompleter = creatingCompleter;
    try {
      await WebviewManager().initialize();
      List args = await _pluginChannel.invokeMethod('create', [url, isPrivate]);
      _browserId = args[0] as int;
      _textureId = args[1] as int;
      WebviewManager().onBrowserCreated(_index, _browserId);
      await Future.delayed(const Duration(milliseconds: 50));
      _webviewWidget = WebView(this);
      value = true;
      creatingCompleter.complete();
    } on PlatformException catch (e) {
      creatingCompleter.completeError(e);
      _creatingCompleter = null;
    }
    return creatingCompleter.future;
  }

  setWebviewListener(WebviewEventsListener listener) {
    _listener = listener;
  }

  @override
  Future<void> dispose() async {
    await _creatingCompleter?.future;
    if (!_isDisposed) {
      _isDisposed = true;
      WebviewManager().removeWebView(_browserId);
      await _pluginChannel.invokeMethod('close', _browserId);
    }
    super.dispose();
  }

  /// Loads the given [url].
  Future<void> loadUrl(String url) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('loadUrl', [_browserId, url]);
  }

  /// Reloads the current document.
  Future<void> reload() async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('reload', _browserId);
  }

  Future<void> wasHidden(bool hidden) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('wasHidden', [_browserId, hidden]);
  }

  /// Captures a screenshot of the current webview content.
  Future<Uint8List?> captureScreenshot() async {
    if (_isDisposed) {
      return null;
    }
    assert(value);
    return WebviewManager().captureScreenshot(_browserId);
  }

  Future<void> goForward() async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('goForward', _browserId);
  }

  Future<void> goBack() async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('goBack', _browserId);
  }

  Future<bool> canGoForward() async {
    if (_isDisposed) {
      return false;
    }
    assert(value);
    final bool? canGo =
        await _pluginChannel.invokeMethod<bool>('canGoForward', _browserId);
    return canGo ?? false;
  }

  Future<bool> canGoBack() async {
    if (_isDisposed) {
      return false;
    }
    assert(value);
    final bool? canGo =
        await _pluginChannel.invokeMethod<bool>('canGoBack', _browserId);
    return canGo ?? false;
  }

  Future<void> openDevTools() async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('openDevTools', _browserId);
  }

  Future<void> imeSetComposition(String composingText) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel
        .invokeMethod('imeSetComposition', [_browserId, composingText]);
  }

  Future<void> imeCommitText(String composingText) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel
        .invokeMethod('imeCommitText', [_browserId, composingText]);
  }

  Future<void> setClientFocus(bool focus) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('setClientFocus', [_browserId, focus]);
  }

  /// Sends a key event to CEF. Used on platforms without native key support (eLinux).
  Future<void> sendKeyEvent(int type, int keyCode, int modifiers, int character,
      int unmodifiedCharacter) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('sendKeyEvent',
        [_browserId, type, keyCode, modifiers, character, unmodifiedCharacter]);
  }

  Future<void> setJavaScriptChannels(Set<JavascriptChannel> channels) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    _assertJavascriptChannelNamesAreUnique(channels);

    for (var channel in channels) {
      _javascriptChannels[channel.name] = channel;
    }

    return _pluginChannel.invokeMethod('setJavaScriptChannels',
        [_browserId, _extractJavascriptChannelNames(channels).toList()]);
  }

  Future<void> sendJavaScriptChannelCallBack(
      bool error, String result, String callbackId, String frameId) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('sendJavaScriptChannelCallBack',
        [error, result, callbackId, _browserId, frameId]);
  }

  Future<void> executeJavaScript(String code) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('executeJavaScript', [_browserId, code]);
  }

  Future<dynamic> evaluateJavascript(String code) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel
        .invokeMethod('evaluateJavascript', [_browserId, code]);
  }

  Future<void> continueDownload(int downloadId, String downloadPath,
      {bool showDialog = false}) async {
    if (_isDisposed) return;
    return WebviewManager()
        .continueDownload(downloadId, downloadPath, showDialog: showDialog);
  }

  Future<void> cancelDownload(int downloadId) async {
    if (_isDisposed) return;
    return WebviewManager().cancelDownload(downloadId);
  }

  Future<void> pauseDownload(int downloadId) async {
    if (_isDisposed) return;
    return WebviewManager().pauseDownload(downloadId);
  }

  Future<void> resumeDownload(int downloadId) async {
    if (_isDisposed) return;
    return WebviewManager().resumeDownload(downloadId);
  }

  /// Moves the virtual cursor to [position].
  Future<void> _cursorMove(Offset position) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod(
        'cursorMove', [_browserId, position.dx.round(), position.dy.round()]);
  }

  Future<void> _cursorDragging(Offset position) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('cursorDragging',
        [_browserId, position.dx.round(), position.dy.round()]);
  }

  Future<void> _cursorClickDown(Offset position) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('cursorClickDown',
        [_browserId, position.dx.round(), position.dy.round()]);
  }

  Future<void> _cursorClickUp(Offset position) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('cursorClickUp',
        [_browserId, position.dx.round(), position.dy.round()]);
  }

  /// Sets the horizontal and vertical scroll delta.
  Future<void> _setScrollDelta(Offset position, int dx, int dy) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('setScrollDelta',
        [_browserId, position.dx.round(), position.dy.round(), dx, dy]);
  }

  /// Sends a native touch event to CEF.
  Future<void> _sendTouchEvent(
      int touchId, int type, Offset position, int modifiers) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel.invokeMethod('sendTouchEvent',
        [_browserId, type, touchId, position.dx, position.dy, modifiers]);
  }

  /// Sets the surface size to the provided [size].
  Future<void> _setSize(double dpi, Size size) async {
    if (_isDisposed) {
      return;
    }
    assert(value);
    return _pluginChannel
        .invokeMethod('setSize', [_browserId, dpi, size.width, size.height]);
  }

  Set<String> _extractJavascriptChannelNames(Set<JavascriptChannel> channels) {
    final Set<String> channelNames =
        channels.map((JavascriptChannel channel) => channel.name).toSet();
    return channelNames;
  }

  void _assertJavascriptChannelNamesAreUnique(
      final Set<JavascriptChannel>? channels) {
    if (channels == null || channels.isEmpty) {
      return;
    }
    assert(_extractJavascriptChannelNames(channels).length == channels.length);
  }

  Function(String)? _onToolTip;
  Function(int)? _onCursorChanged;
  Function(bool editable)? _onFocusedNodeChangeMessage;
  Function(int, int, int)? _onImeCompositionRangeChangedMessage;
}

enum _TouchGestureType { undecided, horizontal, vertical }

class WebView extends StatefulWidget {
  final WebViewController controller;

  static double get mouseScrollSensitivity => Platform.isMacOS ? 1.0 : 10.0;
  static const double trackpadScrollSensitivity = 1.0;
  static const double touchScrollSensitivity = 1.0;

  const WebView(this.controller, {super.key});

  @override
  WebViewState createState() => WebViewState();
}

class WebViewState extends State<WebView> with WebeViewTextInput {
  final GlobalKey _key = GlobalKey();
  String _composingText = '';
  late final _focusNode = FocusNode();
  bool isPrimaryFocus = false;
  WebviewTooltip? _tooltip;
  MouseCursor _mouseType = SystemMouseCursors.basic;
  bool? _hasNativeKeySupport;
  _TouchGestureType _gestureType = _TouchGestureType.undecided;
  Offset? _startPosition;
  bool _touchCancelled = false;
  int? _primaryPointerId;

  WebViewController get _controller => widget.controller;

  @override
  updateEditingValueWithDeltas(List<TextEditingDelta> textEditingDeltas) {
    /// Handles IME composition and virtual keyboard input
    for (var d in textEditingDeltas) {
      if (d is TextEditingDeltaInsertion) {
        if (d.composing.isValid) {
          _composingText += d.textInserted;
          _controller.imeSetComposition(_composingText);
        } else {
          if (d.textInserted == '\n' ||
              d.textInserted == '\r' ||
              d.textInserted == '\r\n') {
            _sendVirtualKey(0x0D, character: 0x0D);
          } else {
            _controller.imeCommitText(d.textInserted);
          }
        }
      } else if (d is TextEditingDeltaDeletion) {
        if (d.composing.isValid && _composingText.isNotEmpty) {
          if (_composingText == d.textDeleted) {
            _composingText = "";
          } else if (_composingText.length >= d.textDeleted.length) {
            _composingText = _composingText.substring(
                0, _composingText.length - d.textDeleted.length);
          } else {
            _composingText = "";
          }
          _controller.imeSetComposition(_composingText);
        }
      } else if (d is TextEditingDeltaReplacement) {
        if (d.composing.isValid) {
          _composingText = d.replacementText;
          _controller.imeSetComposition(_composingText);
        } else {
          _controller.imeCommitText(d.replacementText);
          _composingText = '';
        }
      } else if (d is TextEditingDeltaNonTextUpdate) {
        if (_composingText.isNotEmpty) {
          _controller.imeCommitText(_composingText);
          _composingText = '';
        }
      }
    }

    // Update the current value so onVirtualEditingValue has the correct oldText next time
    // For OSR webview we don't have the full text state, but we can maintain it from deltas
    // However, since we're using a fake TextEditingValue just to track diffs in onVirtualEditingValue,
    // we can construct it from the deltas' resulting state if they provide it.
    if (textEditingDeltas.isNotEmpty) {
      currentTextEditingValue = textEditingDeltas.last.apply(currentTextEditingValue ?? const TextEditingValue());
    }
  }

  void _sendVirtualKey(int keyCode, {int character = 0}) {
    _controller.sendKeyEvent(
      keyEventRawKeyDown,
      keyCode,
      eventFlagNone,
      character,
      character,
    );
    if (character != 0) {
      _controller.sendKeyEvent(
        keyEventChar,
        keyCode,
        eventFlagNone,
        character,
        character,
      );
    }
    _controller.sendKeyEvent(
      keyEventKeyUp,
      keyCode,
      eventFlagNone,
      character,
      character,
    );
  }

  @override
  void onVirtualAction(TextInputAction action) {
    _sendVirtualKey(0x0D, character: 0x0D);
  }

  @override
  void onVirtualEditingValue(TextEditingValue value) {
    final oldText = currentTextEditingValue?.text ?? '';
    final newText = value.text;
    currentTextEditingValue = value;

    if (newText.length < oldText.length) {
      final deleteCount = oldText.length - newText.length;
      for (int i = 0; i < deleteCount; i++) {
        _sendVirtualKey(0x08, character: 0x08);
      }
    } else if (newText.length > oldText.length) {
      final inserted = newText.substring(oldText.length);
      if (inserted == '\n' || inserted == '\r' || inserted == '\r\n') {
        _sendVirtualKey(0x0D, character: 0x0D);
      } else {
        _controller.imeCommitText(inserted);
      }
    }
  }

  @override
  void initState() {
    super.initState();
    _controller._onFocusedNodeChangeMessage = (editable) {
      _composingText = '';
      currentTextEditingValue = const TextEditingValue();
      if (editable) {
        attachTextInputClient();
        _controller.setClientFocus(true);
      } else {
        detachTextInputClient();
      }
      _controller._focusEditable = editable;
    };

    _controller._onImeCompositionRangeChangedMessage = (x, y, height) {
      final box = _key.currentContext!.findRenderObject() as RenderBox;
      updateIMEComposionPosition(x.toDouble(), y.toDouble(), height.toDouble(),
          box.localToGlobal(Offset.zero));
    };

    _controller._onToolTip = (final String text) {
      _tooltip ??= WebviewTooltip(_key.currentContext!);
      _tooltip?.showToolTip(text);
    };

    _controller._onCursorChanged = (int type) {
      switch (type) {
        case 0:
          _mouseType = SystemMouseCursors.basic;
          break;
        case 1:
          _mouseType = SystemMouseCursors.precise;
          break;
        case 2:
          _mouseType = SystemMouseCursors.click;
          break;
        case 3:
          _mouseType = SystemMouseCursors.text;
          break;
        case 4:
          _mouseType = SystemMouseCursors.wait;
          break;
        default:
          _mouseType = SystemMouseCursors.basic;
          break;
      }
      setState(() {});
    };

    // Check if platform has native key support (e.g., GTK on desktop Linux)
    WebviewManager().hasNativeKeySupport.then((value) {
      _hasNativeKeySupport = value;
    });

    // Report initial surface size
    WidgetsBinding.instance
        .addPostFrameCallback((_) => _reportSurfaceSize(context));
  }

  KeyEventResult _onKeyEvent(FocusNode node, KeyEvent event) {
    // Only handle keys on platforms without native key support (eLinux)
    // Treat null as "don't handle yet" to prevent double-delivery during async gap
    if (_hasNativeKeySupport != false) {
      return KeyEventResult.ignored;
    }

    // Map Flutter key event to CEF key event
    final logicalKey = event.logicalKey;
    final character = event.character;

    // 1. If the key event has a character, let the TextInputClient handle it.
    // This prevents doubling because updateEditingValueWithDeltas/onVirtualEditingValue
    // will be responsible for sending the text to CEF.
    if (character != null && character.isNotEmpty) {
      return KeyEventResult.ignored;
    }

    // Convert logical key to Windows keycode
    int keyCode = _logicalKeyToWindowsKeyCode(logicalKey);

    // Build modifiers
    int modifiers = 0;
    if (HardwareKeyboard.instance.isShiftPressed) {
      modifiers |= eventFlagShiftDown;
    }
    if (HardwareKeyboard.instance.isControlPressed) {
      modifiers |= eventFlagControlDown;
    }
    if (HardwareKeyboard.instance.isAltPressed) {
      modifiers |= eventFlagAltDown;
    }

    // Determine event type
    int type;
    if (event is KeyDownEvent) {
      type = keyEventRawKeyDown;
    } else if (event is KeyUpEvent) {
      type = keyEventKeyUp;
    } else {
      return KeyEventResult.ignored;
    }

    // Send key event to CEF
    _controller.sendKeyEvent(
      type,
      keyCode,
      modifiers,
      0, // No character here as they are ignored above
      0,
    );

    return KeyEventResult.handled;
  }

  int _logicalKeyToWindowsKeyCode(LogicalKeyboardKey key) {
    // Map Flutter logical keys to Windows key codes
    // This is a simplified mapping - may need to be expanded
    if (key == LogicalKeyboardKey.f12) return 0x7B;
    if (key == LogicalKeyboardKey.f1) return 0x70;
    if (key == LogicalKeyboardKey.f2) return 0x71;
    if (key == LogicalKeyboardKey.f3) return 0x72;
    if (key == LogicalKeyboardKey.f4) return 0x73;
    if (key == LogicalKeyboardKey.f5) return 0x74;
    if (key == LogicalKeyboardKey.f6) return 0x75;
    if (key == LogicalKeyboardKey.f7) return 0x76;
    if (key == LogicalKeyboardKey.f8) return 0x77;
    if (key == LogicalKeyboardKey.f9) return 0x78;
    if (key == LogicalKeyboardKey.f10) return 0x79;
    if (key == LogicalKeyboardKey.f11) return 0x7A;
    if (key == LogicalKeyboardKey.enter) return 0x0D;
    if (key == LogicalKeyboardKey.escape) return 0x1B;
    if (key == LogicalKeyboardKey.tab) return 0x09;
    if (key == LogicalKeyboardKey.backspace) return 0x08;
    if (key == LogicalKeyboardKey.delete) return 0x2E;
    if (key == LogicalKeyboardKey.insert) return 0x2D;
    if (key == LogicalKeyboardKey.home) return 0x24;
    if (key == LogicalKeyboardKey.end) return 0x23;
    if (key == LogicalKeyboardKey.pageUp) return 0x21;
    if (key == LogicalKeyboardKey.pageDown) return 0x22;
    if (key == LogicalKeyboardKey.arrowUp) return 0x26;
    if (key == LogicalKeyboardKey.arrowDown) return 0x28;
    if (key == LogicalKeyboardKey.arrowLeft) return 0x25;
    if (key == LogicalKeyboardKey.arrowRight) return 0x27;

    // For alphanumeric keys, use the key label
    final keyLabel = key.keyLabel;
    if (keyLabel.length == 1) {
      final charCode = keyLabel.codeUnitAt(0);
      if (charCode >= 0x41 && charCode <= 0x5A) {
        // A-Z
        return charCode;
      }
      if (charCode >= 0x30 && charCode <= 0x39) {
        // 0-9
        return charCode;
      }
    }

    // Default fallback
    return 0;
  }

  @override
  Widget build(BuildContext context) {
    return Focus(
      autofocus: true,
      focusNode: _focusNode,
      canRequestFocus: true,
      debugLabel: "webview_cef",
      onFocusChange: (focused) {
        _composingText = '';
        currentTextEditingValue = const TextEditingValue();
        if (focused) {
          _controller.setClientFocus(true);
          if (_controller._focusEditable) {
            attachTextInputClient();
          }
        } else {
          _controller.setClientFocus(false);
          if (_controller._focusEditable) {
            detachTextInputClient();
          }
        }
      },
      onKeyEvent: _onKeyEvent,
      child: SizedBox.expand(key: _key, child: _buildInner()),
    );
  }

  Widget _buildInner() {
    return NotificationListener<SizeChangedLayoutNotification>(
      onNotification: (notification) {
        _reportSurfaceSize(context);
        return true;
      },
      child: SizeChangedLayoutNotifier(
        child: Listener(
          onPointerHover: (ev) {
            _controller._cursorMove(ev.localPosition);
            _tooltip?.cursorOffset = ev.position;
          },
          onPointerDown: (ev) {
            if (!_focusNode.hasFocus) {
              _controller._onImeCompositionRangeChangedMessage?.call(0, 0, 0);
              _focusNode.requestFocus();
              Future.delayed(const Duration(milliseconds: 50), () {
                if (!_focusNode.hasFocus) {
                  _focusNode.requestFocus();
                }
              });
            }
            if (ev.kind == PointerDeviceKind.touch) {
              if (_primaryPointerId == null) {
                _primaryPointerId = ev.pointer;
                _startPosition = ev.localPosition;
                _gestureType = _TouchGestureType.undecided;
                _touchCancelled = false;
              }
              _sendTouchEvent(ev, 1); // pressed (CEF_TET_PRESSED = 1)
            } else {
              _controller._cursorClickDown(ev.localPosition);
            }
          },
          onPointerUp: (ev) {
            if (ev.kind == PointerDeviceKind.touch) {
              if (!_touchCancelled) {
                _sendTouchEvent(ev, 0); // released (CEF_TET_RELEASED = 0)
              }
              if (ev.pointer == _primaryPointerId) {
                _primaryPointerId = null;
                _startPosition = null;
                _touchCancelled = false;
              }
            } else {
              _controller._cursorClickUp(ev.localPosition);
            }
          },
          onPointerMove: (ev) {
            if (ev.kind == PointerDeviceKind.touch) {
              if (ev.pointer == _primaryPointerId &&
                  _gestureType == _TouchGestureType.undecided &&
                  _startPosition != null) {
                final dx = ev.localPosition.dx - _startPosition!.dx;
                final dy = ev.localPosition.dy - _startPosition!.dy;
                if (dx.abs() > 10 || dy.abs() > 10) {
                  if (dx.abs() > dy.abs()) {
                    _gestureType = _TouchGestureType.horizontal;
                    _sendTouchEvent(ev, 3); // cancelled (CEF_TET_CANCELLED = 3)
                    _touchCancelled = true;
                  } else {
                    _gestureType = _TouchGestureType.vertical;
                  }
                }
              }

              if (!_touchCancelled) {
                _sendTouchEvent(ev, 2); // moved (CEF_TET_MOVED = 2)
              }
            } else {
              _controller._cursorDragging(ev.localPosition);
            }
          },
          onPointerCancel: (ev) {
            if (ev.kind == PointerDeviceKind.touch) {
              if (!_touchCancelled) {
                _sendTouchEvent(ev, 3); // cancelled (CEF_TET_CANCELLED = 3)
              }
              if (ev.pointer == _primaryPointerId) {
                _primaryPointerId = null;
                _startPosition = null;
                _touchCancelled = false;
              }
            }
          },
          onPointerSignal: (signal) {
            if (signal is PointerScrollEvent) {
              double multiplier = 1.0;
              if (signal.kind == PointerDeviceKind.mouse) {
                multiplier = WebView.mouseScrollSensitivity;
              } else if (signal.kind == PointerDeviceKind.trackpad) {
                multiplier = WebView.trackpadScrollSensitivity;
              } else if (signal.kind == PointerDeviceKind.touch) {
                multiplier = WebView.touchScrollSensitivity;
              }
              _controller._setScrollDelta(
                signal.localPosition,
                (signal.scrollDelta.dx * multiplier).round(),
                (signal.scrollDelta.dy * multiplier).round(),
              );
            }
          },
          onPointerPanZoomUpdate: (event) {
            double multiplier = WebView.trackpadScrollSensitivity;
            _controller._setScrollDelta(
              event.localPosition,
              (event.panDelta.dx * multiplier).round(),
              (event.panDelta.dy * multiplier).round(),
            );
          },
          child: MouseRegion(
            cursor: _mouseType,
            child: Builder(builder: (context) {
              return Texture(textureId: _controller._textureId);
            }),
          ),
        ),
      ),
    );
  }

  Future<void> _sendTouchEvent(PointerEvent ev, int type) {
    int modifiers = 0;
    if (HardwareKeyboard.instance.isShiftPressed) {
      modifiers |= eventFlagShiftDown;
    }
    if (HardwareKeyboard.instance.isControlPressed) {
      modifiers |= eventFlagControlDown;
    }
    if (HardwareKeyboard.instance.isAltPressed) {
      modifiers |= eventFlagAltDown;
    }

    return _controller._sendTouchEvent(
      ev.pointer,
      type,
      ev.localPosition,
      modifiers,
    );
  }

  void _reportSurfaceSize(BuildContext context) async {
    double dpi = MediaQuery.of(context).devicePixelRatio;
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box != null) {
      final size = Size(box.size.width, box.size.height);
      if (_lastReportedSize == size && _lastReportedDpi == dpi) {
        return;
      }
      _lastReportedSize = size;
      _lastReportedDpi = dpi;
      await _controller.ready;
      unawaited(_controller._setSize(dpi, size));
    }
  }

  Size? _lastReportedSize;
  double? _lastReportedDpi;
}

class StaticWebView extends StatelessWidget {
  final WebViewController controller;

  const StaticWebView(this.controller, {super.key});

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<bool>(
      valueListenable: controller,
      builder: (context, initialized, child) {
        if (!initialized) {
          return controller.loadingWidget;
        }
        return Texture(textureId: controller.textureId);
      },
    );
  }
}
