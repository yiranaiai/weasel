#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

#include <stdio.h>

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

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
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

/* Composition anchor policy.
 *
 * TSF hosts report the caret through ITfContextView::GetTextExt and for almost
 * every host that answer is the truth: the candidate window is placed on it and
 * nothing here second-guesses it.
 *
 * Two hosts cannot be served that way:
 *
 *   - WPS Writer paints its own caret and answers with a corner of the document
 *     window. Its automation model does know the caret the user sees, so the
 *     server reads it and the client asks for it over IPC
 *     (WEASEL_IPC_GET_CARET_ANCHOR) before reporting a position.
 *   - WPS Presentation only recomputes its rect when an edit commits, so it is
 *     anchored on the click that placed the caret instead (see below).
 *
 * A host that answers nothing - UWP/XAML islands such as Task Manager - must
 * not be handed a made-up rect: reporting the all-zero answer parked the
 * candidate window in the screen corner on every keystroke. When GetTextExt
 * produces nothing usable, no position is reported at all, which is what stock
 * Weasel did and what those hosts expect.
 *
 * All rects are screen coordinates in physical pixels: Weasel is per-monitor
 * DPI aware and every API used here already reports physical pixels, so no
 * manual scaling is applied.
 */
namespace {

/* WPS Presentation (class PP12FrameClass; the slide view itself runs in a
 * separate wpp.exe but the frame window still carries that class) answers
 * GetTextExt with a rectangle that is only recomputed when an edit commits.
 * It reports a corner of the slide area for a fresh composition, and after the
 * first commit keeps reporting the caret as it was at that moment: later caret
 * moves are never reflected. Every composition that follows a caret move then
 * landed on the previous caret, which reads as the candidate window being one
 * step behind.
 *
 * The click that placed the caret is the one live signal the host leaves us:
 * the user clicks the line to edit and starts typing, so the mouse sits on the
 * spot being typed into. Typing again without moving the mouse keeps that
 * anchor. Nothing else takes this path - every other host, including WPS
 * Writer, still uses the rect TSF reports. */
bool _IsPresentationFrame(HWND hwnd) {
  if (hwnd == nullptr)
    return false;
  HWND root = ::GetAncestor(hwnd, GA_ROOT);
  if (root == nullptr)
    root = hwnd;
  wchar_t name[64] = {0};
  if (!::GetClassNameW(root, name, _countof(name)))
    return false;
  return wcscmp(name, L"PP12FrameClass") == 0;
}

/* Line height is only used to keep the rect looking like a caret; the UI places
 * the candidate window under rc.bottom either way. */
const int kPresentationCaretHeight = 20;

/* Anchor for a composition the host cannot locate: the mouse position when it
 * is over the slide view, else the view origin, which at least keeps the
 * candidate window inside the document instead of on a screen corner. */
void _PresentationAnchor(ITfContextView* view, RECT* out) {
  RECT bounds = {0, 0, 0, 0};
  const bool have_bounds =
      view != nullptr && SUCCEEDED(view->GetScreenExt(&bounds)) &&
      bounds.right > bounds.left && bounds.bottom > bounds.top;
  POINT pt = {0, 0};
  if (have_bounds && ::GetCursorPos(&pt) && ::PtInRect(&bounds, pt)) {
    out->left = pt.x;
    out->right = pt.x;
    out->top = pt.y - kPresentationCaretHeight;
    out->bottom = pt.y;
    return;
  }
  if (have_bounds) {
    out->left = bounds.left + 4;
    out->top = bounds.top + 4;
    out->right = out->left;
    out->bottom = out->top + kPresentationCaretHeight;
    return;
  }
  out->left = out->right = 0;
  out->top = 0;
  out->bottom = kPresentationCaretHeight;
}

/* WPS Writer is the only host whose caret comes from its own automation model.
 * The server asks KWps.Application for Selection.Range and keeps the answer
 * cached, because Writer does not reply to that query while it is waiting for
 * an IME answer. Only Writer frames take this path: the class check keeps every
 * other host off the IPC round trip. */
bool _IsWriterHost(HWND hwnd) {
  if (hwnd == nullptr)
    return false;
  wchar_t name[64] = {0};
  HWND root = ::GetAncestor(hwnd, GA_ROOT);
  if (root == nullptr)
    root = hwnd;
  if (::GetClassNameW(root, name, _countof(name)) &&
      wcscmp(name, L"OpusApp") == 0)
    return true;
  if (::GetClassNameW(hwnd, name, _countof(name)) && wcscmp(name, L"_WwG") == 0)
    return true;
  return false;
}

/* Bounded trace of the rect a composition settled on: a handful of lines per
 * process, enough to explain a bug report and invisible otherwise.
 * %TEMP%\weasel-comp-rect.log */
const int kDiagMaxLines = 24;

void _DiagCompositionRect(HWND hwnd,
                          HRESULT hr,
                          const RECT& rc,
                          const char* source) {
  static LONG s_lines = 0;
  if (::InterlockedIncrement(&s_lines) > kDiagMaxLines)
    return;
  wchar_t temp[MAX_PATH] = {0};
  if (!::GetTempPathW(MAX_PATH, temp))
    return;
  wchar_t path[MAX_PATH] = {0};
  ::swprintf_s(path, L"%sweasel-comp-rect.log", temp);
  FILE* fp = nullptr;
  if (::_wfopen_s(&fp, path, L"a") != 0 || fp == nullptr)
    return;
  wchar_t cls[64] = {0};
  HWND root = hwnd != nullptr ? ::GetAncestor(hwnd, GA_ROOT) : nullptr;
  if (root != nullptr)
    ::GetClassNameW(root, cls, _countof(cls));
  ::fprintf(fp, "hr=0x%08lx src=%s root=%ls rc=(%ld,%ld,%ld,%ld)\n", (long)hr,
            source, cls, (long)rc.left, (long)rc.top, (long)rc.right,
            (long)rc.bottom);
  ::fclose(fp);
}

}  // namespace

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
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

  /* The presentation host never answers GetTextExt with the current caret, so
   * asking it is pointless: anchor on the click that placed the caret instead
   * and return before the rect below is built. */
  if (_IsPresentationFrame(hwnd_host)) {
    RECT presentation_rc = {};
    _PresentationAnchor(_pContextView, &presentation_rc);
    _pTextService->_SetCompositionPosition(presentation_rc);
    return S_OK;
  }

  HRESULT hr_ext = E_FAIL;
  if (_pContextView != nullptr)
    hr_ext = _pContextView->GetTextExt(ec, pRange, &rc, &fClipped);

  /* WPS Writer is the one host whose real caret lives outside TSF. Ask the
   * server for it before reporting anything: it answers from its cache, and the
   * class check keeps every other host off that IPC round trip. */
  if (_IsWriterHost(hwnd_host)) {
    RECT anchored = {};
    if (_pTextService->_CaretAnchor(&anchored)) {
      _DiagCompositionRect(hwnd_host, hr_ext, anchored, "writer-anchor");
      _pTextService->_SetCompositionPosition(anchored);
      return S_OK;
    }
  }

  /* Below is stock Weasel, unchanged: a rect is reported only when the host
   * produced one, and the enhanced correction still measures against the
   * foreground window. Hosts that answer nothing (UWP/XAML islands such as
   * Task Manager) therefore see exactly what they saw before this branch
   * existed - no position reported at all. */
  if (hr_ext == S_OK && (rc.left != 0 || rc.top != 0)) {
    _DiagCompositionRect(hwnd_host, hr_ext, rc, "host");

    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd;
      RECT rcForegroundWindow;
      hwnd = ::GetForegroundWindow();
      ::GetWindowRect(hwnd, &rcForegroundWindow);

      if (rc.left < rcForegroundWindow.left ||
          rc.left > rcForegroundWindow.right ||
          rc.top < rcForegroundWindow.top ||
          rc.top > rcForegroundWindow.bottom) {
        POINT pt;
        bool hasCaret = ::GetCaretPos(&pt);
        int offsetx = rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
        int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
        rc.left += offsetx;
        rc.right += offsetx;
        rc.top += offsety;
        rc.bottom += offsety;
      }
    }
    _pTextService->_SetCompositionPosition(rc);
  } else {
    _DiagCompositionRect(hwnd_host, hr_ext, rc, "none");
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  /* WPS hosts only: their text store answers GetTextExt from a read/write
   * session (and refuses the stock read-only one), so those two classes get the
   * wider request with the stock request as fallback. Every other host takes
   * the stock request unchanged - one call, read-only, exactly as upstream. */
  HWND hwnd_host = nullptr;
  pContextView->GetWnd(&hwnd_host);
  if (_IsPresentationFrame(hwnd_host) || _IsWriterHost(hwnd_host)) {
    hr = E_FAIL;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    if (FAILED(hr))
      pContext->RequestEditSession(_tfClientId, pEditSession,
                                   TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  } else {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  }
  return SUCCEEDED(hr);
}

bool WeaselTSF::_CaretAnchor(RECT* anchor) {
  return weasel::UnpackInputPosition(m_client.GetCaretAnchor(), anchor);
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
