import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/physics.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import 'webview_manager.dart';
import 'webview_events_listener.dart';
import 'webview_javascript.dart';
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

  late Completer<void> _creatingCompleter;
  Future<void> get ready => _creatingCompleter.future;
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
    _creatingCompleter = Completer<void>();
    try {
      await WebviewManager().ready;
      List args = await _pluginChannel.invokeMethod('create', [url, isPrivate]);
      _browserId = args[0] as int;
      _textureId = args[1] as int;
      WebviewManager().onBrowserCreated(_index, _browserId);
      await Future.delayed(const Duration(milliseconds: 50));
      _webviewWidget = WebView(this);
      value = true;
      _creatingCompleter.complete();
    } on PlatformException catch (e) {
      _creatingCompleter.completeError(e);
    }
    return _creatingCompleter.future;
  }

  setWebviewListener(WebviewEventsListener listener) {
    _listener = listener;
  }

  @override
  Future<void> dispose() async {
    await _creatingCompleter.future;
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

class WebView extends StatefulWidget {
  final WebViewController controller;

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

  Offset? _lastTouchPosition;
  Offset? _inertiaStartTouchPosition;
  bool _isTouchMoving = false;
  AnimationController? _inertiaController;
  // The single active listener on _inertiaController. Only one flick's
  // listener may be attached at a time -- without this, each new flick
  // stacked another listener on the reused controller and old + new flicks
  // fought over the same setScrollDelta stream (the direction-flip / jump
  // bug seen in the logs).
  VoidCallback? _inertiaListener;
  final TickerProvider _tickerProvider = const _WebViewTickerProvider();

  // Velocity tracking for touch-release inertia, captured straight off the
  // raw pointer stream (no separate GestureDetector arena / delay).
  final List<_TrackedPoint> _recentPoints = [];

  // Per-frame scroll coalescing so we don't flood the platform channel with
  // a setScrollDelta call on every single pointer move (important on
  // high refresh-rate screens where pointer events can outpace frames).
  Offset _pendingScrollDelta = Offset.zero;
  Offset? _pendingScrollPosition;
  Timer? _scrollFlushTimer;

  WebViewController get _controller => widget.controller;

  @override
  updateEditingValueWithDeltas(List<TextEditingDelta> textEditingDeltas) {
    /// Handles IME composition only
    for (var d in textEditingDeltas) {
      if (d is TextEditingDeltaInsertion) {
        // composing text
        if (d.composing.isValid) {
          _composingText += d.textInserted;
          _controller.imeSetComposition(_composingText);
        } else {
          // Directly committed text (e.g. English typing, or a commit delivered
          // as a plain insertion). Must run on every platform, including Windows.
          _controller.imeCommitText(d.textInserted);
        }
      } else if (d is TextEditingDeltaDeletion) {
        if (d.composing.isValid) {
          if (_composingText == d.textDeleted) {
            _composingText = "";
          }
          _controller.imeSetComposition(_composingText);
        }
      } else if (d is TextEditingDeltaReplacement) {
        if (d.composing.isValid) {
          // Composition is still ongoing (preedit revised).
          _composingText = d.replacementText;
          _controller.imeSetComposition(_composingText);
        } else {
          // Composition finished (a candidate was selected): commit the final
          // text. Without this the selected text was dropped and never shown.
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
  }

  @override
  void dispose() {
    if (_inertiaListener != null) {
      _inertiaController?.removeListener(_inertiaListener!);
      _inertiaListener = null;
    }
    _inertiaController?.dispose();
    _scrollFlushTimer?.cancel();
    super.dispose();
  }

  void _stopInertia() {
    final wasAnimating = _inertiaController?.isAnimating ?? false;
    if (wasAnimating) {
      _inertiaController?.stop();
      _isTouchMoving = true; // Mark as moving to prevent click on next up
    } else {
      _isTouchMoving = false;
    }

    // Detach the previous flick's listener so it can't keep firing once a
    // new touch/flick starts.
    if (_inertiaListener != null) {
      _inertiaController?.removeListener(_inertiaListener!);
      _inertiaListener = null;
    }

    // Prevent leftover inertia deltas from bleeding into the next drag.
    _scrollFlushTimer?.cancel();
    _scrollFlushTimer = null;
    _pendingScrollDelta = Offset.zero;
    _pendingScrollPosition = null;
  }

  void _performInertia(Offset velocity) {
    debugPrint("webview_cef: _performInertia velocity=$velocity");
    _inertiaController ??=
        AnimationController.unbounded(vsync: _tickerProvider);

    // Remove any listener left over from a previous flick before adding a
    // new one -- addListener is additive, so without this old flicks kept
    // firing alongside new ones and their deltas got interleaved.
    if (_inertiaListener != null) {
      _inertiaController!.removeListener(_inertiaListener!);
      _inertiaListener = null;
    }

    final double devicePixelRatio = MediaQuery.of(context).devicePixelRatio;
    final Offset scaledVelocity = velocity / devicePixelRatio;
    if (scaledVelocity.distance < 0.1) {
      _isTouchMoving = false;
      return;
    }

    final direction = scaledVelocity / scaledVelocity.distance;
    final startTouchPosition =
        _inertiaStartTouchPosition ?? _lastTouchPosition ?? Offset.zero;

    // Cap the maximum velocity to prevent extreme jumps, but allow fast
    // flicks to actually feel fast (was 3500).
    const double maxVelocity = 2200.0;
    final effectiveVelocity = scaledVelocity.distance.clamp(0.0, maxVelocity);

    // Lower friction so momentum coasts smoothly instead of stopping almost
    // immediately.
    final simulation = FrictionSimulation(0.25, 0.0, effectiveVelocity);

    double lastDistance = 0.0;

    _inertiaListener = () {
      if (!mounted ||
          _inertiaController == null ||
          !_inertiaController!.isAnimating) return;

      final double currentDistance = _inertiaController!.value;
      final double deltaDistance = currentDistance - lastDistance;
      if (deltaDistance.abs() < 0.1) return;

      final Offset delta = direction * deltaDistance;

      // If startTouchPosition is zero, it might cause jumps to the top-left.
      // We should use a reasonably safe position if it's missing.
      Offset scrollPosition = startTouchPosition;
      if (scrollPosition == Offset.zero) {
        final box = context.findRenderObject() as RenderBox?;
        if (box != null) {
          scrollPosition = Offset(box.size.width / 2, box.size.height / 2);
        }
      }

      _queueScroll(scrollPosition, Offset(-delta.dx, -delta.dy));
      lastDistance = currentDistance;
    };
    _inertiaController!.addListener(_inertiaListener!);

    _inertiaController!.animateWith(simulation).then((_) {
      _isTouchMoving = false;
    });
  }

  /// Batches scroll deltas and flushes at most once per frame, so fast
  /// pointer/inertia updates don't flood the platform channel.
  void _queueScroll(Offset position, Offset delta) {
    debugPrint("webview_cef: _queueScroll position=$position, delta=$delta");
    _pendingScrollPosition = position;
    _pendingScrollDelta += delta;
    if (_scrollFlushTimer != null) return;
    _scrollFlushTimer = Timer(const Duration(milliseconds: 10), () {
      _scrollFlushTimer = null;
      final pos = _pendingScrollPosition;
      if (pos == null) return;
      var d = _pendingScrollDelta;
      _pendingScrollDelta = Offset.zero;

      // Clamp so a burst of queued deltas never lands as one big jump.
      const double maxStep = 80.0;
      if (d.distance > maxStep) {
        d = d / d.distance * maxStep;
      }
      _controller._setScrollDelta(pos, d.dx.round(), d.dy.round());
    });
  }

  @override
  void initState() {
    super.initState();
    _controller._onFocusedNodeChangeMessage = (editable) {
      _composingText = '';
      editable ? attachTextInputClient() : detachTextInputClient();
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
      character?.codeUnitAt(0) ?? 0,
      character?.codeUnitAt(0) ?? 0,
    );

    // Send CHAR event after RAWKEYDOWN when character is present (required for text entry)
    if (event is KeyDownEvent && character != null) {
      _controller.sendKeyEvent(
        keyEventChar,
        keyCode,
        modifiers,
        character.codeUnitAt(0),
        character.codeUnitAt(0),
      );
    }
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
            _stopInertia();
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
              _lastTouchPosition = ev.localPosition;
              _inertiaStartTouchPosition = ev.localPosition;
              _isTouchMoving = false;
              // Start a fresh velocity sample window for this touch
              // sequence, fed straight from the raw pointer stream (no
              // separate GestureDetector arena, so no extra latency).
              _recentPoints
                ..clear()
                ..add(_TrackedPoint(ev.timeStamp, ev.localPosition));
            } else {
              _controller._cursorClickDown(ev.localPosition);
            }
          },
          onPointerUp: (ev) {
            if (ev.kind == PointerDeviceKind.touch) {
              if (!_isTouchMoving) {
                // If it was a short tap (no movement), we should still trigger a click
                _controller._cursorClickDown(ev.localPosition);
                _controller._cursorClickUp(ev.localPosition);
              } else {
                // Release: hand off to inertia using the velocity derived
                // from just the last ~60ms of motion (more robust than a
                // whole-gesture least-squares fit on fast/curved swipes).
                if (_recentPoints.length >= 2) {
                  final first = _recentPoints.first;
                  final last = _recentPoints.last;
                  final dt = (last.time - first.time).inMicroseconds / 1e6;
                  if (dt > 0) {
                    final v = (last.position - first.position) / dt; // px/sec
                    if (v.distance > 0) {
                      _performInertia(v * 0.4);
                    }
                  }
                }
              }
              _lastTouchPosition = null;
              _recentPoints.clear();
            } else {
              _controller._cursorClickUp(ev.localPosition);
            }
          },
          onPointerCancel: (ev) {
            _lastTouchPosition = null;
            _recentPoints.clear();
            _isTouchMoving = false;
          },
          onPointerMove: (ev) {
            if (ev.kind == PointerDeviceKind.touch) {
              debugPrint(
                  "webview_cef: onPointerMove (touch) localPosition=${ev.localPosition}");
              _recentPoints.add(_TrackedPoint(ev.timeStamp, ev.localPosition));
              final cutoff = ev.timeStamp - const Duration(milliseconds: 60);
              _recentPoints.removeWhere((p) => p.time < cutoff);
              if (_lastTouchPosition != null) {
                final delta = ev.localPosition - _lastTouchPosition!;
                if (delta.distance > 12) {
                  _isTouchMoving = true;
                }
                _queueScroll(ev.localPosition,
                    Offset(-delta.dx * 0.25, -delta.dy * 0.35));
                _lastTouchPosition = ev.localPosition;
                _inertiaStartTouchPosition = ev.localPosition;
              }
            } else {
              _controller._cursorDragging(ev.localPosition);
            }
          },
          onPointerSignal: (signal) {
            if (signal is PointerScrollEvent) {
              debugPrint(
                  "webview_cef: onPointerSignal (scroll) localPosition=${signal.localPosition}, scrollDelta=${signal.scrollDelta}");
              _queueScroll(
                  signal.localPosition,
                  Offset(signal.scrollDelta.dx * 0.25,
                      signal.scrollDelta.dy * 0.25));
            }
          },
          onPointerPanZoomUpdate: (event) {
            debugPrint(
                "webview_cef: onPointerPanZoomUpdate panDelta=${event.panDelta}");
            _queueScroll(event.localPosition,
                Offset(event.panDelta.dx * 0.25, event.panDelta.dy * 0.25));
          },
          child: MouseRegion(
            cursor: _mouseType,
            child: Texture(textureId: _controller._textureId),
          ),
        ),
      ),
    );
  }

  void _reportSurfaceSize(BuildContext context) async {
    double dpi = MediaQuery.of(context).devicePixelRatio;
    final box = _key.currentContext?.findRenderObject() as RenderBox?;
    if (box != null) {
      await _controller.ready;
      unawaited(
          _controller._setSize(dpi, Size(box.size.width, box.size.height)));
    }
  }
}

class _TrackedPoint {
  final Duration time;
  final Offset position;
  _TrackedPoint(this.time, this.position);
}

class _WebViewTickerProvider implements TickerProvider {
  const _WebViewTickerProvider();

  @override
  Ticker createTicker(TickerCallback onTick) => Ticker(onTick);
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
