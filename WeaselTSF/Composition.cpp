#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

#include <imm.h>
#include <stdio.h>

/* Deferred probe state, shared by the composition helpers and the edit sessions
 * below. See _ArmProbeWrite for what the two flags mean. */
static std::atomic<bool> g_probe_armed{false};
static std::atomic<bool> g_probe_disabled{false};

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled)
      : CEditSession(pTextService, pContext) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
};

STDMETHODIMP CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* set selection */
    TF_SELECTION tfSelection;
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);

    // The old composition's range is still visible while its asynchronous
    // end session is pending. Position only after the new composition has
    // actually been created, not from the response handler's stale range.
    _pTextService->_UpdateCompositionWindow(_pContext);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(
      new CStartCompositionEditSession(this, pContext, fCUASWorkaroundEnabled));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE)
      : CEditSession(pTextService, pContext), _clear(clear) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  /* Clear the dummy text we set before, if any. A probe filler goes even on a
   * commit, where the caller asked not to clear: the filler is ours, and
   * leaving it would put an invisible character into the document. */
  const bool probe_pending = g_probe_armed.exchange(false);
  com_ptr<ITfRange> pCompositionRange;
  if ((_clear || probe_pending) &&
      _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  // Drop ownership before EndComposition(). Some applications notify
  // OnCompositionTerminated synchronously while the old composition ends.
  // Keeping it as the current composition makes that normal notification
  // look like an external abort and can clear a new Rime composition during
  // auto-commit.
  if (_pTextService && _pTextService->_IsCurrentComposition(_pComposition))
    _pTextService->_FinalizeComposition();
  _pComposition->EndComposition(ec);
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                BOOL endUI) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;
  com_ptr<ITfComposition> pComposition = _pComposition;

  if (endUI)
    _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, pComposition, clear)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Composition anchor helpers.
 *
 * TSF hosts are expected to report the caret rectangle through
 * ITfContextView::GetTextExt. WPS Writer/Presentation draw their own caret and
 * never answer that query, so the candidate window used to keep whatever
 * default position the UI had, which lands in the bottom-right corner. The
 * helpers below build a fallback chain so a usable screen-space anchor is
 * nearly always available:
 *
 *   1. GetTextExt, accepted only when it overlaps the host window
 *   2. GUITHREADINFO::rcCaret of the host UI thread
 *   3. GetTextExt on a neighbour range, for hosts that refuse a collapsed one
 *   4. GetTextExt on our composition after a temporary invisible filler, for
 *      hosts that report no extent unless the range carries a character
 *   5. GetCaretPos() of the calling thread
 *   6. the IMM32 composition window position
 *   7. the last anchor that was actually reported
 *   8. the host window origin, which always exists
 *
 * All rects are screen coordinates in physical pixels: Weasel is per-monitor
 * DPI aware and every API used here already reports physical pixels, so no
 * manual scaling is applied.
 */
namespace {

RECT _VirtualScreenRect() {
  RECT rc;
  rc.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
  rc.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
  rc.right = rc.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
  rc.bottom = rc.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
  return rc;
}

/* Caret rects have no width: a caret is a vertical line, so hosts report
 * (x, top, x, bottom). IntersectRect() answers FALSE for such a rect because
 * the intersection has no area, so it cannot be used to test containment.
 * Compare edges instead - touching counts as overlapping. */
bool _RectTouches(const RECT& a, const RECT& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom &&
         b.top <= a.bottom;
}

bool _IsOnVirtualScreen(const RECT& rc) {
  const RECT screen = _VirtualScreenRect();
  if (screen.right <= screen.left || screen.bottom <= screen.top)
    return true;  // metrics unavailable, do not second-guess the host
  return _RectTouches(rc, screen);
}

/* A host that cannot answer GetTextExt tends to return a degenerate point or
 * coordinates glued to the virtual screen corner. Reject those, and reject
 * anything that does not overlap the host window: a caret is always drawn
 * somewhere inside the window it belongs to.
 *
 * Zero width must stay acceptable, WPS Writer answers (x, top, x, bottom) for
 * a real caret. Zero height must not: no caret is a horizontal line, and both
 * the corner sentinel and the enhanced-position repair below produce exactly
 * that shape - (0, 150, 1, 150) for a full screen WPS window. */
bool _IsAnchorUsable(const RECT& rc, HWND hwnd_host) {
  if (rc.right < rc.left || rc.bottom < rc.top)
    return false;
  if (rc.bottom == rc.top)
    return false;
  if (!_IsOnVirtualScreen(rc))
    return false;
  if (hwnd_host != nullptr) {
    RECT window;
    if (::GetWindowRect(hwnd_host, &window) && window.right > window.left &&
        window.bottom > window.top) {
      // A clipped caret may sit a few pixels outside the client area.
      ::InflateRect(&window, 16, 16);
      if (!_RectTouches(rc, window))
        return false;
    }
  }
  return true;
}

/* imm32.lib is not linked into WeaselTSF (see WeaselTSF.vcxproj and
 * WeaselTSF/xmake.lua), so the entry points below are resolved at run time
 * instead of adding a new link dependency. */
using PfnImmGetContext = HIMC(WINAPI*)(HWND);
using PfnImmReleaseContext = BOOL(WINAPI*)(HWND, HIMC);
using PfnImmGetCompositionWindow = BOOL(WINAPI*)(HIMC, LPCOMPOSITIONFORM);

struct Imm32Api {
  PfnImmGetContext get_context = nullptr;
  PfnImmReleaseContext release_context = nullptr;
  PfnImmGetCompositionWindow get_composition_window = nullptr;

  bool usable() const {
    return get_context != nullptr && release_context != nullptr &&
           get_composition_window != nullptr;
  }
};

const Imm32Api& _Imm32() {
  static const Imm32Api api = []() {
    Imm32Api loaded;
    HMODULE module = ::LoadLibraryW(L"imm32.dll");
    if (module != nullptr) {
      loaded.get_context = reinterpret_cast<PfnImmGetContext>(
          ::GetProcAddress(module, "ImmGetContext"));
      loaded.release_context = reinterpret_cast<PfnImmReleaseContext>(
          ::GetProcAddress(module, "ImmReleaseContext"));
      loaded.get_composition_window =
          reinterpret_cast<PfnImmGetCompositionWindow>(
              ::GetProcAddress(module, "ImmGetCompositionWindow"));
    }
    return loaded;
  }();
  return api;
}

/* Window that owns the caret. The context view window is authoritative; the
 * focused window and the foreground window are fallbacks for hosts that do not
 * hand one out. */
HWND _CaretHostWindow(HWND hwnd_host) {
  if (hwnd_host != nullptr)
    return hwnd_host;
  HWND hwnd = ::GetFocus();
  if (hwnd != nullptr)
    return hwnd;
  return ::GetForegroundWindow();
}

bool _AnchorFromCaret(HWND hwnd_host, RECT* anchor) {
  HWND hwnd = _CaretHostWindow(hwnd_host);
  if (hwnd == nullptr)
    return false;
  const DWORD thread_id = ::GetWindowThreadProcessId(hwnd, nullptr);
  if (thread_id == 0)
    return false;

  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetGUIThreadInfo(thread_id, &info) || info.hwndCaret == nullptr)
    return false;

  POINT top_left = {info.rcCaret.left, info.rcCaret.top};
  POINT bottom_right = {info.rcCaret.right, info.rcCaret.bottom};
  if (!::ClientToScreen(info.hwndCaret, &top_left) ||
      !::ClientToScreen(info.hwndCaret, &bottom_right))
    return false;

  const RECT rc = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  if (rc.bottom <= rc.top)
    return false;
  if (!_IsAnchorUsable(rc, hwnd_host))
    return false;
  *anchor = rc;
  return true;
}

bool _AnchorFromImm32(HWND hwnd_host, RECT* anchor) {
  const Imm32Api& api = _Imm32();
  if (!api.usable())
    return false;
  HWND hwnd = _CaretHostWindow(hwnd_host);
  if (hwnd == nullptr)
    return false;

  HIMC himc = api.get_context(hwnd);
  if (himc == nullptr)
    return false;

  bool found = false;
  COMPOSITIONFORM form = {};
  // Only CFS_POINT and CFS_FORCE_POSITION put a caret position in
  // ptCurrentPos (client coordinates); CFS_RECT only fills rcArea.
  if (api.get_composition_window(himc, &form) &&
      (form.dwStyle & (CFS_POINT | CFS_FORCE_POSITION))) {
    POINT pt = form.ptCurrentPos;
    if (::ClientToScreen(hwnd, &pt)) {
      const RECT rc = {pt.x, pt.y, pt.x, pt.y + 1};
      if (_IsAnchorUsable(rc, hwnd_host)) {
        *anchor = rc;
        found = true;
      }
    }
  }
  api.release_context(hwnd, himc);
  return found;
}

/* Caret position of the calling thread. GetCaretPos() works for hosts that
 * keep a thread caret but never fill GUITHREADINFO::hwndCaret, which is common
 * when the app draws the caret itself. */
bool _AnchorFromCaretPos(HWND hwnd_host, RECT* anchor) {
  HWND hwnd = _CaretHostWindow(hwnd_host);
  if (hwnd == nullptr)
    return false;

  POINT caret = {};
  if (!::GetCaretPos(&caret))
    return false;
  // Hosts that never set a caret still report success with (0, 0), which would
  // anchor the window at the client origin. Treat it as "no caret" instead: the
  // terminal fallback produces the same point when the caret really is there.
  if (caret.x == 0 && caret.y == 0)
    return false;
  // The thread caret lives in client coordinates of the thread window.
  POINT top_left = caret;
  if (!::ClientToScreen(hwnd, &top_left))
    return false;

  const RECT rc = {top_left.x, top_left.y, top_left.x, top_left.y + 1};
  if (!_IsAnchorUsable(rc, hwnd_host))
    return false;
  *anchor = rc;
  return true;
}

/* Last resort, and it must always succeed.
 *
 * While a composition is active the candidate window is published from
 * _SetCompositionPosition(), so returning without an anchor does not mean
 * "keep the old position" - it means the window is never shown at all. A
 * rough anchor that is guaranteed to be on screen beats a well validated
 * one that does not exist. */
bool _AnchorFromHostWindow(HWND hwnd_host, RECT* anchor) {
  HWND hwnd = _CaretHostWindow(hwnd_host);
  if (hwnd != nullptr) {
    POINT origin = {0, 0};
    if (::ClientToScreen(hwnd, &origin)) {
      *anchor = {origin.x, origin.y, origin.x + 1, origin.y + 1};
      return true;
    }
    RECT window;
    if (::GetWindowRect(hwnd, &window) && window.right > window.left &&
        window.bottom > window.top) {
      *anchor = {window.left, window.top, window.left + 1, window.top + 1};
      return true;
    }
  }
  POINT cursor = {};
  if (::GetCursorPos(&cursor)) {
    *anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    return true;
  }
  return false;
}

/* Diagnostic sink for the anchor chain. Only active while the Weasel log level
 * is raised (HKCU\Software\Rime\Weasel\LogLevel > 0) so the normal path keeps
 * away from the file system. It records which fallback produced the anchor,
 * which is the only way to tell whether a host exposes a caret at all. */
/* Trace sink. Only active while the Weasel log level is raised
 * (HKCU\Software\Rime\Weasel\LogLevel > 0) so the normal path stays away from
 * the file system. It records every source the anchor chain consults, because
 * for a self drawn host such as WPS Writer the only way to tell what it does
 * expose is to write the numbers down. */
FILE* _OpenTraceFile() {
  if (!IsWeaselLogEnabled())
    return nullptr;
  wchar_t path[MAX_PATH] = {};
  const DWORD len = ::GetTempPathW(MAX_PATH, path);
  constexpr size_t suffix_len = 18;  // "weasel-anchor.log" plus the NUL
  if (len == 0 || len + suffix_len > MAX_PATH)
    return nullptr;
  ::lstrcatW(path, L"weasel-anchor.log");
  FILE* fp = nullptr;
  if (::_wfopen_s(&fp, path, L"a") != 0)
    return nullptr;
  return fp;
}

void _TraceDiag(HRESULT hr_ext,
                const RECT& ext_rc,
                BOOL ext_clipped,
                HRESULT hr_sel,
                const RECT& sel_rc,
                ITfContextView* view,
                HWND hwnd_host,
                const char* source,
                const RECT& final_rc) {
  FILE* fp = _OpenTraceFile();
  if (fp == nullptr)
    return;

  RECT view_rect = {};
  HRESULT hr_view = E_FAIL;
  if (view != nullptr)
    hr_view = view->GetScreenExt(&view_rect);

  HWND host = _CaretHostWindow(hwnd_host);
  HWND caret_hwnd = nullptr;
  RECT caret_rect = {};
  if (host != nullptr) {
    const DWORD tid = ::GetWindowThreadProcessId(host, nullptr);
    GUITHREADINFO info = {};
    info.cbSize = sizeof(info);
    if (tid != 0 && ::GetGUIThreadInfo(tid, &info)) {
      caret_hwnd = info.hwndCaret;
      caret_rect = info.rcCaret;
    }
  }

  POINT caret_pos = {};
  const BOOL has_caret_pos = ::GetCaretPos(&caret_pos);

  COMPOSITIONFORM form = {};
  BOOL has_form = FALSE;
  const Imm32Api& api = _Imm32();
  if (api.usable() && host != nullptr) {
    HIMC himc = api.get_context(host);
    if (himc != nullptr) {
      has_form = api.get_composition_window(himc, &form);
      api.release_context(host, himc);
    }
  }

  RECT host_rect = {};
  if (host != nullptr)
    ::GetWindowRect(host, &host_rect);

  std::ostringstream line;
  line << "text_ext hr=0x" << std::hex << (long)hr_ext << std::dec << " rc=("
       << ext_rc.left << "," << ext_rc.top << "," << ext_rc.right << ","
       << ext_rc.bottom << ") clipped=" << (ext_clipped ? 1 : 0)
       << " | sel_ext hr=0x" << std::hex << (long)hr_sel << std::dec << " rc=("
       << sel_rc.left << "," << sel_rc.top << "," << sel_rc.right << ","
       << sel_rc.bottom << ")"
       << " | screen_ext hr=0x" << std::hex << (long)hr_view << std::dec
       << " rc=(" << view_rect.left << "," << view_rect.top << ","
       << view_rect.right << "," << view_rect.bottom << ")"
       << " | hwnd=0x" << std::hex << (unsigned long long)host << " win=("
       << std::dec << host_rect.left << "," << host_rect.top << ","
       << host_rect.right << "," << host_rect.bottom << ")"
       << " | gui caret=0x" << std::hex << (unsigned long long)caret_hwnd
       << " rc=(" << std::dec << caret_rect.left << "," << caret_rect.top << ","
       << caret_rect.right << "," << caret_rect.bottom << ")"
       << " | caretpos ok=" << (has_caret_pos ? 1 : 0) << " pt=(" << caret_pos.x
       << "," << caret_pos.y << ")"
       << " | imm has=" << (has_form ? 1 : 0) << " style=0x" << std::hex
       << form.dwStyle << std::dec << " pt=(" << form.ptCurrentPos.x << ","
       << form.ptCurrentPos.y << ")"
       << " | source=" << source << " final=(" << final_rc.left << ","
       << final_rc.top << "," << final_rc.right << "," << final_rc.bottom
       << ")";

  const std::string now = current_time();
  ::fprintf(fp, "%s %s\n", now.c_str(), line.str().c_str());
  ::fclose(fp);
}

/* Ask GetTextExt for a range that actually contains text.
 *
 * Some hosts refuse to map a collapsed range to a layout but answer as soon as
 * the range covers a character. WPS Writer behaves this way: with inline
 * preedit the composition carries the preedit string and the candidate window
 * follows the caret, while an empty composition leaves it parked. Widen the
 * range by one character and take the edge that faces the caret. */
bool _AnchorFromNeighbourRange(TfEditCookie ec,
                               ITfContextView* view,
                               ITfRange* range,
                               HWND hwnd_host,
                               const char* label,
                               RECT* anchor) {
  bool found = false;
  bool forward = true;
  RECT rc = {};
  HRESULT hr_clone = E_FAIL;
  HRESULT hr_fwd = E_FAIL;
  HRESULT hr_bwd = E_FAIL;
  HRESULT hr_ext = E_FAIL;
  LONG cch_fwd = 0;
  LONG cch_bwd = 0;

  do {
    if (view == nullptr || range == nullptr)
      break;

    com_ptr<ITfRange> probe;
    hr_clone = range->Clone(&probe);
    if (hr_clone != S_OK || probe == nullptr)
      break;

    hr_fwd = probe->ShiftEnd(ec, 1, &cch_fwd, nullptr);
    if (hr_fwd != S_OK || cch_fwd <= 0) {
      forward = false;
      hr_bwd = probe->ShiftStart(ec, -1, &cch_bwd, nullptr);
      if (hr_bwd != S_OK || cch_bwd == 0)
        break;
    }

    BOOL clipped = FALSE;
    hr_ext = view->GetTextExt(ec, probe, &rc, &clipped);
    if (hr_ext != S_OK)
      break;

    const int height = rc.bottom > rc.top ? rc.bottom - rc.top : 1;
    RECT candidate = {};
    if (forward)
      candidate = {rc.left, rc.top, rc.left, rc.top + height};
    else
      candidate = {rc.right, rc.top, rc.right, rc.top + height};
    if (_IsAnchorUsable(candidate, hwnd_host)) {
      *anchor = candidate;
      found = true;
    }
  } while (false);

  FILE* fp = _OpenTraceFile();
  if (fp != nullptr) {
    const std::string now = current_time();
    ::fprintf(fp,
              "%s neigh %s dir=%s clone=0x%lx fwd=0x%lx/%ld bwd=0x%lx/%ld "
              "ext=0x%lx rc=(%ld,%ld,%ld,%ld) anchored=%d\n",
              now.c_str(), label, forward ? "fwd" : "bwd", (long)hr_clone,
              (long)hr_fwd, cch_fwd, (long)hr_bwd, cch_bwd, (long)hr_ext,
              rc.left, rc.top, rc.right, rc.bottom, found ? 1 : 0);
    ::fclose(fp);
  }

  return found;
}

/* Some hosts keep no document text in their TSF store: they only report a text
 * extent for a range that carries characters. WPS Writer is one of them - an
 * empty composition gets the work area corner back, while a composition with
 * text gets the real caret. Borrow the mechanism: put one zero width space
 * into our own composition, read the extent, then take it back out. Both
 * writes happen inside a single edit session, so the application observes only
 * the final empty composition.
 *
 * The filler is read back every time. If it cannot be removed the probe
 * disables itself for the process rather than risk leaving invisible
 * characters in a document. */
/* Number of characters currently held by a composition, or -1 when the query
 * fails. Only the zero/non-zero distinction matters here. */
/* 1 when the composition holds no character, 0 when it holds text, -1 when the
 * query failed. Uses CompareStart against the range itself, the same call the
 * text edit sink already relies on. */
int _CompositionEmpty(TfEditCookie ec, ITfComposition* composition) {
  if (composition == nullptr)
    return -1;
  com_ptr<ITfRange> range;
  if (composition->GetRange(&range) != S_OK || range == nullptr)
    return -1;
  LONG cmp = 0;
  if (range->CompareStart(ec, range, TF_ANCHOR_END, &cmp) != S_OK)
    return -1;
  return cmp == 0 ? 1 : 0;
}

/* Probe, phase one: put an invisible filler into the composition and leave it
 * there.
 *
 * Some hosts answer GetTextExt only for text they have already laid out, and
 * the layout pass runs after this edit session returns: a filler read back
 * inside the same session always comes back as the corner sentinel, which is
 * exactly what WPS Writer and Presentation do. So arm the probe here and let
 * the next update read the extent in its own session (see
 * CProbeMeasureEditSession). A zero width space is invisible, so the extra
 * character cannot be seen while it waits.
 *
 * A composition that holds text is never overwritten: under
 * preedit_type = inline that text is the preedit of this very service. A
 * composition that holds text here is either our own stale filler from an
 * update that never measured it - removed - or a preedit, which is left alone.
 */
bool _ArmProbeWrite(TfEditCookie ec,
                    WeaselTSF* text_service,
                    ITfContext* context,
                    ITfComposition* composition) {
  if (g_probe_disabled.load() || composition == nullptr ||
      text_service == nullptr)
    return false;

  const int empty = _CompositionEmpty(ec, composition);
  if (empty == 0) {
    com_ptr<ITfRange> stale;
    if (composition->GetRange(&stale) == S_OK && stale != nullptr)
      stale->SetText(ec, 0, L"", 0);
    g_probe_armed.store(false);
    return false;
  }
  if (empty != 1)
    return false;  // cannot tell, do not write

  com_ptr<ITfRange> range;
  if (composition->GetRange(&range) != S_OK || range == nullptr)
    return false;
  if (range->SetText(ec, 0, L"\u200B", 1) != S_OK)
    return false;

  /* Present the filler exactly like an inline preedit, because that is the only
   * shape WPS maps to a layout: it carries the composing display attribute and
   * the selection sits inside the composition. A bare SetText, with or without
   * a real character, is ignored and answered with the corner sentinel. The
   * filler itself stays invisible, so mimicking the inline presentation shows
   * nothing. */
  text_service->_SetCompositionDisplayAttributes(ec, context, range);

  range->Collapse(ec, TF_ANCHOR_END);
  TF_SELECTION selection;
  selection.range = range;
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  context->SetSelection(ec, 1, &selection);

  g_probe_armed.store(true);
  FILE* fp = _OpenTraceFile();
  if (fp != nullptr) {
    const std::string now = current_time();
    ::fprintf(fp, "%s probe armed\n", now.c_str());
    ::fclose(fp);
  }
  return true;
}

/* Last anchor that was reported to the UI. Deliberately per process: a stale
 * caret position is still a better guess than the bottom-right default. */
struct AnchorStore {
  CRITICAL_SECTION lock;
  bool has_anchor = false;
  RECT anchor = {};

  AnchorStore() { ::InitializeCriticalSection(&lock); }
};

AnchorStore& _AnchorState() {
  static AnchorStore store;
  return store;
}

void _RememberAnchor(const RECT& rc) {
  AnchorStore& store = _AnchorState();
  ::EnterCriticalSection(&store.lock);
  store.anchor = rc;
  store.has_anchor = true;
  ::LeaveCriticalSection(&store.lock);
}

bool _LastAnchor(RECT* anchor) {
  AnchorStore& store = _AnchorState();
  bool found = false;
  ::EnterCriticalSection(&store.lock);
  if (store.has_anchor) {
    *anchor = store.anchor;
    found = true;
  }
  ::LeaveCriticalSection(&store.lock);
  return found;
}

/* Keep the anchor inside the virtual desktop so a host that reports a caret on
 * a detached monitor cannot push the candidate window off screen. */
void _ClampAnchor(RECT& rc) {
  const RECT screen = _VirtualScreenRect();
  if (screen.right <= screen.left || screen.bottom <= screen.top)
    return;
  const int max_x = screen.right - 1;
  const int max_y = screen.bottom - 1;
  int dx = 0;
  int dy = 0;
  if (rc.left < screen.left)
    dx = screen.left - rc.left;
  else if (rc.left > max_x)
    dx = max_x - rc.left;
  if (rc.top < screen.top)
    dy = screen.top - rc.top;
  else if (rc.top > max_y)
    dy = max_y - rc.top;
  if (dx != 0 || dy != 0)
    ::OffsetRect(&rc, dx, dy);
}

}  // namespace

/* Probe, phase two: read the extent of the filler armed by the previous update,
 * then take it back out. Runs in its own edit session, i.e. after the host has
 * laid the filler out - the step WPS refuses to do inside the session that
 * inserted it. */
class CProbeMeasureEditSession : public CEditSession {
 public:
  CProbeMeasureEditSession(com_ptr<WeaselTSF> pTextService,
                           com_ptr<ITfContext> pContext,
                           com_ptr<ITfComposition> pComposition)
      : CEditSession(pTextService, pContext), _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CProbeMeasureEditSession::DoEditSession(TfEditCookie ec) {
  /* The composition may have ended between the two sessions - a commit on the
   * same key, a focus change - and then there is nothing to measure. */
  if (!g_probe_armed.load() || _pComposition == nullptr ||
      _pTextService == nullptr ||
      !_pTextService->_IsCurrentComposition(_pComposition))
    return S_OK;

  HWND hwnd_host = nullptr;
  com_ptr<ITfContextView> view;
  if (_pContext != nullptr && _pContext->GetActiveView(&view) == S_OK &&
      view != nullptr)
    view->GetWnd(&hwnd_host);

  HRESULT hr_ext = E_FAIL;
  RECT rc = {};
  if (view != nullptr && _pComposition != nullptr) {
    com_ptr<ITfRange> range;
    if (_pComposition->GetRange(&range) == S_OK && range != nullptr) {
      BOOL clipped = FALSE;
      hr_ext = view->GetTextExt(ec, range, &rc, &clipped);
    }
  }
  const RECT ext_rc = rc;

  /* Take the filler out whatever the extent said, then verify against the
   * composition itself rather than the range kept above. */
  int empty_after = -1;
  if (_pComposition != nullptr) {
    _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

    com_ptr<ITfRange> clean;
    if (_pComposition->GetRange(&clean) == S_OK && clean != nullptr)
      clean->SetText(ec, 0, L"", 0);
    empty_after = _CompositionEmpty(ec, _pComposition);
    if (empty_after == 0) {
      // cch = 0 left it behind; exactly one character was inserted.
      com_ptr<ITfRange> clean2;
      if (_pComposition->GetRange(&clean2) == S_OK && clean2 != nullptr) {
        clean2->SetText(ec, 1, L"", 0);
        empty_after = _CompositionEmpty(ec, _pComposition);
      }
    }
    if (empty_after == 0)
      g_probe_disabled.store(true);  // never probe this process again

    /* The probe moved the caret past the filler; put it back at the start of
     * the composition, where it was before arming. */
    com_ptr<ITfRange> restore;
    if (_pComposition->GetRange(&restore) == S_OK && restore != nullptr) {
      restore->Collapse(ec, TF_ANCHOR_START);
      TF_SELECTION selection;
      selection.range = restore;
      selection.style.ase = TF_AE_NONE;
      selection.style.fInterimChar = FALSE;
      _pContext->SetSelection(ec, 1, &selection);
    }
  }
  g_probe_armed.store(false);

  bool found = false;
  if (hr_ext == S_OK) {
    const int height = rc.bottom > rc.top ? rc.bottom - rc.top : 1;
    RECT candidate = {rc.left, rc.top, rc.left, rc.top + height};
    if (_IsAnchorUsable(candidate, hwnd_host)) {
      rc = candidate;
      found = true;
    }
  }
  /* The window is only published from a position, so always publish one even
   * when the measurement failed. */
  if (!found && !_LastAnchor(&rc))
    _AnchorFromHostWindow(hwnd_host, &rc);
  _ClampAnchor(rc);
  _RememberAnchor(rc);

  FILE* fp = _OpenTraceFile();
  if (fp != nullptr) {
    const std::string now = current_time();
    ::fprintf(fp,
              "%s probe2 ext=0x%lx rc=(%ld,%ld,%ld,%ld) empty=%d anchored=%d "
              "final=(%ld,%ld,%ld,%ld)\n",
              now.c_str(), (long)hr_ext, ext_rc.left, ext_rc.top, ext_rc.right,
              ext_rc.bottom, empty_after, found ? 1 : 0, rc.left, rc.top,
              rc.right, rc.bottom);
    ::fclose(fp);
  }

  _pTextService->_SetCompositionPosition(rc);
  return S_OK;
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition,
                            TfClientId client_id)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
    _client_id = client_id;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
  TfClientId _client_id;
};

STDMETHODIMP CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc = {0, 0, 1, 1};
  BOOL fClipped = FALSE;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  HWND hwnd_host = nullptr;
  if (_pContextView != nullptr)
    _pContextView->GetWnd(&hwnd_host);

  HRESULT hr_ext = E_FAIL;
  BOOL ext_clipped = FALSE;
  RECT ext_rc = {0, 0, 1, 1};
  if (_pContextView != nullptr)
    hr_ext = _pContextView->GetTextExt(ec, pRange, &rc, &fClipped);
  ext_rc = rc;
  ext_clipped = fClipped;

  bool anchored = false;
  if (hr_ext == S_OK && (rc.left != 0 || rc.top != 0)) {
    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd = hwnd_host != nullptr ? hwnd_host : ::GetForegroundWindow();
      RECT rcForegroundWindow;
      if (hwnd != nullptr && ::GetWindowRect(hwnd, &rcForegroundWindow)) {
        if (rc.left < rcForegroundWindow.left ||
            rc.left > rcForegroundWindow.right ||
            rc.top < rcForegroundWindow.top ||
            rc.top > rcForegroundWindow.bottom) {
          POINT pt;
          bool hasCaret = ::GetCaretPos(&pt);
          int offsetx =
              rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
          int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
          rc.left += offsetx;
          rc.right += offsetx;
          rc.top += offsety;
          rc.bottom += offsety;
        }
      }
    }
    anchored = _IsAnchorUsable(rc, hwnd_host);
  }

  /* Diagnostic only, never used for positioning: does this host answer for the
   * selection range even when it refuses the composition range? */
  HRESULT hr_sel = E_FAIL;
  RECT sel_rc = {0, 0, 1, 1};
  if (IsWeaselLogEnabled() && _pContextView != nullptr &&
      selection.range != nullptr && selection.range != pRange) {
    BOOL sel_clipped = FALSE;
    hr_sel =
        _pContextView->GetTextExt(ec, selection.range, &sel_rc, &sel_clipped);
  }

  /* WPS Writer/Presentation never answer GetTextExt for the empty composition
   * that preedit_type = composition creates, and some hosts return a
   * degenerate rect. Fall back to the caret exposed by the host UI thread,
   * then to a neighbour range that carries text, the thread caret, the IMM32
   * composition window, the previous anchor, and finally a position that is
   * always available. */
  const char* source = "text_ext";
  if (!anchored && _AnchorFromCaret(hwnd_host, &rc)) {
    anchored = true;
    source = "gui_thread_info";
  }
  if (!anchored && _AnchorFromNeighbourRange(ec, _pContextView, pRange,
                                             hwnd_host, "compose", &rc)) {
    anchored = true;
    source = "neighbour_compose";
  }
  /* The composition range may be clamped to the composition extent, so try the
   * selection range as well: it always points into the document text. */
  if (!anchored && selection.range != pRange &&
      _AnchorFromNeighbourRange(ec, _pContextView, selection.range, hwnd_host,
                                "select", &rc)) {
    anchored = true;
    source = "neighbour_select";
  }
  /* Hosts that expose no text at all still answer once their layout has seen a
   * character, so arm the deferred probe: this session inserts an invisible
   * filler, the next update measures it. */
  if (!anchored && _pComposition != nullptr)
    _ArmProbeWrite(ec, _pTextService, _pContext, _pComposition);
  if (!anchored && _AnchorFromCaretPos(hwnd_host, &rc)) {
    anchored = true;
    source = "caret_pos";
  }
  if (!anchored && _AnchorFromImm32(hwnd_host, &rc)) {
    anchored = true;
    source = "imm32";
  }
  if (!anchored && _LastAnchor(&rc)) {
    anchored = true;
    source = "last_anchor";
  }
  if (!anchored && _AnchorFromHostWindow(hwnd_host, &rc)) {
    anchored = true;
    source = "host_window";
  }

  /* Always report a position: while a composition is active the candidate
   * window is only published from here. Skipping the call leaves the window
   * invisible instead of merely misplaced. */
  _ClampAnchor(rc);
  _RememberAnchor(rc);
  _TraceDiag(hr_ext, ext_rc, ext_clipped, hr_sel, sel_rc, _pContextView,
             hwnd_host, source, rc);
  _pTextService->_SetCompositionPosition(rc);

  /* The filler is in place. Ask for the measurement session from inside this
   * one so the host runs it next, with the layout of the filler already done -
   * the whole point of splitting the probe. */
  if (g_probe_armed.load() && _pComposition != nullptr) {
    com_ptr<CProbeMeasureEditSession> pMeasure;
    pMeasure.Attach(
        new CProbeMeasureEditSession(_pTextService, _pContext, _pComposition));
    HRESULT hr_probe = E_FAIL;
    if (pMeasure != nullptr) {
      _pContext->RequestEditSession(_client_id, pMeasure,
                                    TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                    &hr_probe);
    }
    if (FAILED(hr_probe))
      g_probe_armed.store(false);  // the filler would never be measured
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;

  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(new CGetTextExtentEditSession(
      this, pContext, pContextView, _pComposition,
      _cand->style().enhanced_position, _tfClientId));
  if (pEditSession == NULL) {
    return FALSE;
  }
  /* Read and write: the extent session may need to put a filler character into
   * the composition to obtain a position from hosts that do not report one for
   * an empty range. A document that refuses writes still gets positioned, just
   * without that probe. */
  HRESULT hr = E_FAIL;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  if (FAILED(hr)) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  }
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
  _FlushPendingUI();
}

void WeaselTSF::_FlushPendingUI() {
  if (!_hasPendingUI || !_pendingUIContext)
    return;
  auto context = std::move(_pendingUIContext);
  auto status = _pendingUIStatus;
  _hasPendingUI = false;
  _UpdateUI(*context, status);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDMETHODIMP CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
}

/* Composition State */
STDMETHODIMP WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                                ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  // EndComposition() may generate this callback for the composition we just
  // closed. Only an active, matching composition is an external termination.
  if (!_IsCurrentComposition(pComposition))
    return S_OK;

  // A host may terminate the empty TSF composition used for a non-inline
  // preedit. Keep Rime's composing state; the next key will create a fresh
  // TSF composition. Only an inactive Rime session should be aborted here.
  if (_status.composing) {
    _FinalizeComposition();
    return S_OK;
  }

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  } else {
    // The host may terminate the TSF composition before focus-loss cleanup.
    // End the candidate UI even when the composition pointer is already gone.
    _cand->EndUI();
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}

BOOL WeaselTSF::_IsCurrentComposition(ITfComposition* pComposition) {
  return _pComposition != nullptr && _pComposition == pComposition;
}
