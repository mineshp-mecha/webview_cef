import 'package:webview_cef/src/webview.dart';

typedef TitleChangeCb = void Function(String title);
typedef UrlChangeCb = void Function(String url);
/* Log severity levels. from CEF include/internal/cef_types.h
  0:default logging (currently info logging)
  1:verbose logging or debug logging
  2:info logging
  3:warning logging
  4:error logging
  5:fatal logging
  99:disable logging to file for all messages, and to stderr for messages with severity less than fatal
 */
typedef LoadStartCb = void Function(WebViewController controller, String url);
typedef LoadStopCb = void Function(WebViewController controller, String url);

typedef OnConsoleMessage = void Function(
    int level, String message, String source, int line);

typedef OnBeforeDownloadCb = void Function(
  WebViewController controller,
  int downloadId,
  String url,
  String suggestedName,
  String contentDisposition,
  String mimeType,
  int totalBytes,
);

typedef OnDownloadUpdatedCb = void Function(
  WebViewController controller,
  int downloadId,
  String url,
  String fullPath,
  int receivedBytes,
  int totalBytes,
  int currentSpeed,
  int percentComplete,
  bool isInProgress,
  bool isComplete,
  bool isCanceled,
  bool isInterrupted,
  int interruptReason,
);

class WebviewEventsListener {
  TitleChangeCb? onTitleChanged;
  UrlChangeCb? onUrlChanged;
  OnConsoleMessage? onConsoleMessage;
  LoadStartCb? onLoadStart;
  LoadStopCb? onLoadEnd;
  OnBeforeDownloadCb? onBeforeDownload;
  OnDownloadUpdatedCb? onDownloadUpdated;

  WebviewEventsListener({
    this.onTitleChanged,
    this.onUrlChanged,
    this.onConsoleMessage,
    this.onLoadStart,
    this.onLoadEnd,
    this.onBeforeDownload,
    this.onDownloadUpdated,
  });
}
