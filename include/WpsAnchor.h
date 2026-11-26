#pragma once
#include <windows.h>

namespace weasel {
namespace wps_anchor {

// Gate: is the foreground window a WPS Writer document window?
// WPS draws the carets of an empty composition itself and refuses to map them
// to text, so the caret rect TSF hands us can be a corner of the document
// window instead of the caret. Its automation model still knows the truth.
bool TargetIsWriter(HWND* doc_window);

// Ask WPS Writer for the caret rect of |doc_window| and wait for the answer.
// Only for diagnostics: the input path must not block on Writer.
// The rect is in physical screen pixels, width 0 and height of the caret line,
// i.e. the same shape and unit the IPC input position uses.
bool Query(HWND doc_window, RECT* out, int timeout_ms = 8);

// The input path entry point. Asks for a refresh of |doc_window|'s anchor and
// returns the newest one already known for that document, without waiting:
// Writer does not pump messages while it waits for an IME reply, so a
// synchronous call at keystroke time would regularly time out.
bool LatestAnchor(HWND doc_window, RECT* out, int max_age_ms = 1500);

// Starts the automation thread early, so the first lookup inside a document
// does not pay for thread creation and COM start-up.
void WarmUp();

// Switches the provider off without a rebuild (also used by the probe).
void SetEnabled(bool enabled);
bool Enabled();

// Diagnostic entry point for "WeaselServer.exe /wps-anchor-probe": samples the
// anchor for |seconds| and writes one line per sample to a file in %TEMP%,
// because the server is a GUI subsystem program without a console.
void RunProbe(int seconds);

}  // namespace wps_anchor
}  // namespace weasel
