import 'package:flutter/rendering.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

/// Reports where the webview's native subsurface should be shown, in global
/// logical pixels. [visible] is false while the hole is not part of the
/// painted scene (e.g. an inactive IndexedStack child) or is disposed.
typedef SubsurfaceRectCallback = void Function(Rect rect, bool visible);

/// The holes showing one webview. A webview has a single subsurface but may
/// be built in several places at once (e.g. the page and a tab-switcher card),
/// so the largest visible hole owns it; the others paint nothing.
class SubsurfaceHoleGroup {
  SubsurfaceHoleGroup(this.onRectChanged);

  final SubsurfaceRectCallback onRectChanged;

  final Map<_RenderSurfaceHole, Rect> _visible = {};
  _RenderSurfaceHole? _owner;
  Rect _sentRect = Rect.zero;
  bool _sentVisible = false;

  void _update(_RenderSurfaceHole hole, Rect rect, bool visible) {
    if (visible) {
      _visible[hole] = rect;
    } else {
      _visible.remove(hole);
    }

    _RenderSurfaceHole? owner;
    double ownerArea = -1;
    _visible.forEach((candidate, candidateRect) {
      final area = candidateRect.width * candidateRect.height;
      if (area > ownerArea) {
        owner = candidate;
        ownerArea = area;
      }
    });
    if (owner != _owner) {
      final previous = _owner;
      _owner = owner;
      // A hole leaving the scene (or being detached) has nothing to repaint.
      if (previous != null &&
          previous.attached &&
          !(previous == hole && !visible)) {
        previous.markNeedsPaint();
      }
      owner?.markNeedsPaint();
    }

    final ownerRect = owner == null ? _sentRect : _visible[owner]!;
    final ownerVisible = owner != null;
    if (ownerRect != _sentRect || ownerVisible != _sentVisible) {
      _sentRect = ownerRect;
      _sentVisible = ownerVisible;
      onRectChanged(ownerRect, ownerVisible);
    }
  }
}

/// Paints a transparent hole through which a Wayland subsurface placed below
/// the Flutter surface shows.
///
/// The hole clears the pixels painted under it, so it only works when no
/// ancestor wraps it in a saveLayer (Opacity, ShaderMask, BackdropFilter...):
/// there the clear only empties the offscreen layer.
class WebviewSurfaceHole extends LeafRenderObjectWidget {
  const WebviewSurfaceHole({super.key, required this.group});

  final SubsurfaceHoleGroup group;

  @override
  RenderObject createRenderObject(BuildContext context) =>
      _RenderSurfaceHole(group);

  @override
  void updateRenderObject(
      BuildContext context, RenderObject renderObject) {
    (renderObject as _RenderSurfaceHole).group = group;
  }
}

class _RenderSurfaceHole extends RenderBox {
  _RenderSurfaceHole(this._group);

  SubsurfaceHoleGroup _group;
  set group(SubsurfaceHoleGroup value) {
    if (value == _group) {
      return;
    }
    _group._update(this, _reportedRect ?? Rect.zero, false);
    _group = value;
    _reportedRect = null;
    _reportedVisible = false;
    markNeedsPaint();
  }

  Rect? _reportedRect;
  bool _reportedVisible = false;
  bool _frameCallbackScheduled = false;

  // Its own layer lets the per-frame check tell whether the hole is still in
  // the scene: a parent that stops painting this child drops the layer.
  @override
  bool get isRepaintBoundary => true;

  @override
  bool get sizedByParent => true;

  @override
  Size computeDryLayout(BoxConstraints constraints) => constraints.biggest;

  @override
  bool hitTestSelf(Offset position) => true;

  @override
  void attach(PipelineOwner owner) {
    super.attach(owner);
    _scheduleCheck();
  }

  @override
  void detach() {
    _report(_reportedRect ?? Rect.zero, false);
    super.detach();
  }

  @override
  void paint(PaintingContext context, Offset offset) {
    // Only the owner reveals the subsurface; another hole clearing its rect
    // would just punch a see-through window into whatever surrounds it.
    if (_group._owner == this) {
      context.canvas.drawRect(
        offset & size,
        Paint()..blendMode = BlendMode.clear,
      );
    }
    _scheduleCheck();
  }

  // Re-armed every frame while attached, so position and visibility are
  // tracked without the hole itself requesting frames.
  void _scheduleCheck() {
    if (_frameCallbackScheduled) {
      return;
    }
    _frameCallbackScheduled = true;
    SchedulerBinding.instance.addPostFrameCallback((_) {
      _frameCallbackScheduled = false;
      if (!attached) {
        return;
      }
      final inScene = layer?.attached ?? false;
      if (inScene && hasSize) {
        _report(localToGlobal(Offset.zero) & size, true);
      } else {
        _report(_reportedRect ?? Rect.zero, false);
      }
      _scheduleCheck();
    });
  }

  void _report(Rect rect, bool visible) {
    if (rect == _reportedRect && visible == _reportedVisible) {
      return;
    }
    _reportedRect = rect;
    _reportedVisible = visible;
    _group._update(this, rect, visible);
  }
}
