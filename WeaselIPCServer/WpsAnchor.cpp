#include "stdafx.h"

#include <WpsAnchor.h>

#include <oleauto.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace weasel {
namespace wps_anchor {
namespace {

const wchar_t kWriterWindowClass[] = L"OpusApp";
const wchar_t kDocumentWindowClass[] = L"_WwG";
const int kQueryTimeoutMs = 8;
const int kPollIntervalMs = 300;

std::atomic<bool>& EnabledFlag() {
  static std::atomic<bool> flag(true);
  return flag;
}

bool _ClassIs(HWND hwnd, const wchar_t* name) {
  if (!hwnd)
    return false;
  wchar_t text[128] = {0};
  if (!::GetClassNameW(hwnd, text, _countof(text)))
    return false;
  return wcscmp(text, name) == 0;
}

struct FindState {
  HWND first = nullptr;
  int count = 0;
};

BOOL CALLBACK _FindDocWindowProc(HWND hwnd, LPARAM param) {
  if (_ClassIs(hwnd, kDocumentWindowClass)) {
    FindState* state = reinterpret_cast<FindState*>(param);
    if (!state->first)
      state->first = hwnd;
    state->count++;
  }
  return TRUE;
}

// Writer keeps several document views alive at once (every open document has
// one, and they are all "visible"), so the view that is actually being edited
// has to be told apart: it is the one holding keyboard focus.
HWND _FocusedDocWindow(HWND top_level) {
  const DWORD thread_id = ::GetWindowThreadProcessId(top_level, nullptr);
  if (!thread_id)
    return nullptr;
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetGUIThreadInfo(thread_id, &info) || !info.hwndFocus)
    return nullptr;
  HWND view = info.hwndFocus;
  for (int depth = 0; depth < 8 && view; ++depth) {
    if (_ClassIs(view, kDocumentWindowClass)) {
      // A view from another frame must not be used: that would anchor the
      // candidate window to a document the user is not editing.
      return ::GetAncestor(view, GA_ROOT) == top_level ? view : nullptr;
    }
    view = ::GetParent(view);
  }
  return nullptr;
}

// The caret is drawn into the document view, a child of the window the
// automation model reports as Window.Hwnd. Only the foreground top level
// window is considered, so a background Writer window never becomes the anchor.
HWND _FindDocWindow() {
  HWND foreground = ::GetForegroundWindow();
  if (!foreground)
    return nullptr;
  HWND top_level = ::GetAncestor(foreground, GA_ROOT);
  if (!top_level || !_ClassIs(top_level, kWriterWindowClass))
    return nullptr;
  HWND focused = _FocusedDocWindow(top_level);
  if (focused)
    return focused;
  // No focus information: only a single view can be taken for granted, never
  // guess between several documents.
  FindState state = {};
  ::EnumChildWindows(top_level, _FindDocWindowProc,
                     reinterpret_cast<LPARAM>(&state));
  return state.count == 1 ? state.first : nullptr;
}

// A tiny owning wrapper so early returns cannot leak the VARIANT payload.
struct ScopedVariant {
  VARIANT value;

  ScopedVariant() { ::VariantInit(&value); }
  ~ScopedVariant() { ::VariantClear(&value); }
  ScopedVariant(const ScopedVariant&) = delete;
  ScopedVariant& operator=(const ScopedVariant&) = delete;

  IDispatch* dispatch() const {
    return value.vt == VT_DISPATCH ? value.pdispVal : nullptr;
  }
};

CLSID _WriterClsid() {
  static std::mutex mutex;
  static CLSID clsid = {};
  static bool resolved = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (!resolved) {
    // KWps.Application is the ProgID Writer registers under HKCU; the other
    // spellings cover installations that expose only one of them.
    const wchar_t* const kProgIds[] = {L"KWps.Application", L"KWPS.Application",
                                       L"Kwps.Application"};
    for (const wchar_t* prog_id : kProgIds) {
      CLSID candidate = {};
      if (SUCCEEDED(::CLSIDFromProgID(prog_id, &candidate))) {
        clsid = candidate;
        resolved = true;
        break;
      }
    }
  }
  return clsid;
}

// GetActiveObject reaches the instance the user is looking at;
// CoCreateInstance would silently start a second, invisible one.
IDispatch* _ActiveWriter() {
  CLSID clsid = _WriterClsid();
  if (IsEqualGUID(clsid, CLSID_NULL))
    return nullptr;
  IUnknown* unknown = nullptr;
  if (FAILED(::GetActiveObject(clsid, nullptr, &unknown)) || !unknown)
    return nullptr;
  IDispatch* app = nullptr;
  if (FAILED(unknown->QueryInterface(IID_IDispatch,
                                     reinterpret_cast<void**>(&app))))
    app = nullptr;
  unknown->Release();
  return app;
}

HRESULT _PropGet(IDispatch* object, const wchar_t* name, VARIANT* out) {
  if (!object || !name || !out)
    return E_INVALIDARG;
  DISPID id = DISPID_UNKNOWN;
  LPOLESTR name_ptr = const_cast<LPOLESTR>(name);
  HRESULT hr =
      object->GetIDsOfNames(IID_NULL, &name_ptr, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED(hr))
    return hr;
  DISPPARAMS params = {nullptr, nullptr, 0, 0};
  return object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                        &params, out, nullptr, nullptr);
}

HRESULT _Call(IDispatch* object,
              const wchar_t* name,
              VARIANT* out,
              VARIANT* args,
              UINT count) {
  if (!object || !name)
    return E_INVALIDARG;
  DISPID id = DISPID_UNKNOWN;
  LPOLESTR name_ptr = const_cast<LPOLESTR>(name);
  HRESULT hr =
      object->GetIDsOfNames(IID_NULL, &name_ptr, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED(hr))
    return hr;
  DISPPARAMS params = {args, nullptr, count, 0};
  return object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                        &params, out, nullptr, nullptr);
}

bool _AsLong(const VARIANT& value, long* out) {
  if (!out)
    return false;
  switch (value.vt) {
    case VT_I1:
      *out = value.cVal;
      return true;
    case VT_I2:
      *out = value.iVal;
      return true;
    case VT_I4:
    case VT_INT:
      *out = value.lVal;
      return true;
    case VT_I8:
      *out = static_cast<long>(value.llVal);
      return true;
    case VT_R4:
      *out = static_cast<long>(value.fltVal);
      return true;
    case VT_R8:
      *out = static_cast<long>(value.dblVal);
      return true;
    default:
      break;
  }
  VARIANT converted;
  ::VariantInit(&converted);
  if (SUCCEEDED(::VariantChangeType(&converted, const_cast<VARIANT*>(&value), 0,
                                    VT_I4))) {
    *out = converted.lVal;
    ::VariantClear(&converted);
    return true;
  }
  ::VariantClear(&converted);
  return false;
}

// The automation answer has to look like a caret: line-high, overlapping the
// document view. Anything else is a bogus reading and we fall back.
bool _SaneRect(const RECT& rect, HWND doc_window) {
  const long width = rect.right - rect.left;
  const long height = rect.bottom - rect.top;
  if (height < 6 || height > 400)
    return false;
  if (width < 0 || width > 4096)
    return false;
  RECT view = {};
  if (!::GetWindowRect(doc_window, &view))
    return false;
  ::InflateRect(&view, 24, 24);
  return rect.left <= view.right && rect.right >= view.left &&
         rect.top <= view.bottom && rect.bottom >= view.top;
}

// Saves the cost of walking Application.Windows on every lookup: once the
// window that owns the document view is known, only its caret has to be asked
// for. Only the automation thread touches this.
struct Cache {
  IDispatch* window = nullptr;
  HWND doc_parent = nullptr;

  void Reset() {
    if (window)
      window->Release();
    window = nullptr;
    doc_parent = nullptr;
  }
};

bool _CaretRectOfWindow(IDispatch* window, RECT* out) {
  ScopedVariant selection;
  if (FAILED(_PropGet(window, L"Selection", &selection.value)) ||
      !selection.dispatch())
    return false;
  ScopedVariant range;
  if (FAILED(_PropGet(selection.dispatch(), L"Range", &range.value)) ||
      !range.dispatch())
    return false;

  long left = 0, top = 0, width = 0, height = 0;
  // Window.GetPoint takes the output parameters by reference and expects the
  // range last, i.e. the argument array is built right to left.
  VARIANT args[5];
  ::VariantInit(&args[0]);
  ::VariantInit(&args[1]);
  ::VariantInit(&args[2]);
  ::VariantInit(&args[3]);
  ::VariantInit(&args[4]);
  args[0].vt = VT_DISPATCH;
  args[0].pdispVal = range.dispatch();
  args[1].vt = VT_BYREF | VT_I4;
  args[1].plVal = &height;
  args[2].vt = VT_BYREF | VT_I4;
  args[2].plVal = &width;
  args[3].vt = VT_BYREF | VT_I4;
  args[3].plVal = &top;
  args[4].vt = VT_BYREF | VT_I4;
  args[4].plVal = &left;
  ScopedVariant result;
  if (FAILED(_Call(window, L"GetPoint", &result.value, args, 5)))
    return false;
  if (height <= 0)
    return false;
  out->left = left;
  out->top = top;
  out->right = left + (width > 0 ? width : 0);
  out->bottom = top + height;
  return true;
}

// Walks the document collection and picks the window whose Hwnd is the parent
// of the document view we are drawing into. Titles are not usable: an edited
// document gets an asterisk and the same title can be open twice.
bool _QueryCaretRect(IDispatch* app,
                     HWND doc_window,
                     RECT* out,
                     std::wstring* detail,
                     Cache* cache) {
  ScopedVariant windows;
  if (FAILED(_PropGet(app, L"Windows", &windows.value)) ||
      !windows.dispatch()) {
    if (detail)
      *detail += L" windows=err";
    return false;
  }
  long count = 0;
  {
    ScopedVariant value;
    if (SUCCEEDED(_PropGet(windows.dispatch(), L"Count", &value.value)))
      _AsLong(value.value, &count);
  }
  if (detail)
    *detail += L" count=" + std::to_wstring(count);
  if (count <= 0)
    return false;

  const HWND doc_parent = ::GetParent(doc_window);
  for (long index = 1; index <= count && index <= 32; ++index) {
    ScopedVariant item;
    {
      VARIANT arg;
      ::VariantInit(&arg);
      arg.vt = VT_I4;
      arg.lVal = index;
      if (FAILED(_Call(windows.dispatch(), L"Item", &item.value, &arg, 1)) ||
          !item.dispatch())
        continue;
    }
    long hwnd_value = 0;
    {
      ScopedVariant value;
      if (!SUCCEEDED(_PropGet(item.dispatch(), L"Hwnd", &value.value)))
        continue;
      if (!_AsLong(value.value, &hwnd_value))
        continue;
    }
    const HWND window_hwnd =
        reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd_value));
    const bool matched =
        window_hwnd == doc_window || window_hwnd == doc_parent || count == 1;
    if (detail)
      *detail += L" hwnd[" + std::to_wstring(index) + L"]=" +
                 std::to_wstring(hwnd_value) + (matched ? L"*" : L"");
    if (!matched)
      continue;
    if (!_CaretRectOfWindow(item.dispatch(), out))
      continue;
    if (!_SaneRect(*out, doc_window)) {
      if (detail)
        *detail += L" insane";
      continue;
    }
    if (cache) {
      cache->Reset();
      cache->window = item.dispatch();
      cache->window->AddRef();
      cache->doc_parent = doc_parent;
    }
    if (detail)
      *detail += L" match=" + std::to_wstring(index);
    return true;
  }
  return false;
}

// Fast path: reuse the window matched last time, so a lookup costs
// Selection + Range + GetPoint instead of a full document walk. The Hwnd check
// keeps a document switch, or a restarted Writer, from being answered with
// the wrong window's caret.
bool _QueryThroughCache(HWND doc_window, RECT* out, Cache* cache) {
  const HWND doc_parent = ::GetParent(doc_window);
  if (cache->window && cache->doc_parent == doc_parent) {
    ScopedVariant hwnd;
    long hwnd_value = 0;
    if (SUCCEEDED(_PropGet(cache->window, L"Hwnd", &hwnd.value)) &&
        _AsLong(hwnd.value, &hwnd_value) &&
        reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd_value)) ==
            doc_parent &&
        _CaretRectOfWindow(cache->window, out) && _SaneRect(*out, doc_window))
      return true;
    cache->Reset();
  }
  IDispatch* app = _ActiveWriter();
  if (!app)
    return false;
  RECT rect = {};
  const bool ok = _QueryCaretRect(app, doc_window, &rect, nullptr, cache);
  app->Release();
  if (!ok)
    return false;
  *out = rect;
  return true;
}

// Owns a single-threaded apartment dedicated to the WPS automation calls, plus
// the last anchor it managed to read. The input path only ever reads that
// cache: Writer does not pump messages while it waits for an IME reply, so a
// synchronous call from the IPC thread could stall for the whole timeout on
// every keystroke.
class Worker {
 public:
  static Worker& Instance() {
    static Worker* worker = new Worker();  // process lifetime, thread outlives
    return *worker;
  }

  bool Query(HWND doc_window, RECT* out, int timeout_ms);
  bool Latest(HWND doc_window, RECT* out, int max_age_ms);
  void WarmUp();
  void Stop();

 private:
  Worker() = default;

  struct Anchor {
    RECT rect = {};
    HWND doc_parent = nullptr;
    ULONGLONG tick = 0;
  };

  void _EnsureThread();
  void _ThreadMain();
  void _Store(HWND doc_window, const RECT& rect);

  std::mutex mutex_;
  std::condition_variable wake_;
  bool started_ = false;
  bool ready_ = false;
  bool stop_ = false;
  bool has_request_ = false;
  unsigned request_seq_ = 0;
  unsigned done_seq_ = 0;
  bool result_ok_ = false;
  HWND request_window_ = nullptr;
  RECT result_ = {};
  Cache cache_;
  std::thread thread_;

  std::mutex anchor_mutex_;
  // One anchor per document view, so switching between open documents picks
  // up where that document was instead of losing the anchor.
  std::map<HWND, Anchor> anchors_;
};

void Worker::_EnsureThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_)
    return;
  started_ = true;
  thread_ = std::thread([this] { _ThreadMain(); });
}

bool Worker::Query(HWND doc_window, RECT* out, int timeout_ms) {
  if (!doc_window || !out)
    return false;
  _EnsureThread();
  std::unique_lock<std::mutex> lock(mutex_);
  // One request in flight is enough: a second one would only queue up behind
  // an answer that is already stale.
  if (has_request_)
    return false;
  has_request_ = true;
  request_window_ = doc_window;
  const unsigned seq = ++request_seq_;
  wake_.notify_all();
  const bool finished = wake_.wait_for(
      lock,
      std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : kQueryTimeoutMs),
      [this, seq] { return done_seq_ >= seq || stop_; });
  if (!finished || done_seq_ < seq || !result_ok_)
    return false;
  *out = result_;
  return true;
}

bool Worker::Latest(HWND doc_window, RECT* out, int max_age_ms) {
  if (!doc_window || !out)
    return false;
  _EnsureThread();
  {
    // Ask for a refresh for the next call, but never wait for it here.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_request_) {
      has_request_ = true;
      request_window_ = doc_window;
      ++request_seq_;
    }
  }
  wake_.notify_all();

  const HWND doc_parent = ::GetParent(doc_window);
  std::lock_guard<std::mutex> lock(anchor_mutex_);
  auto it = anchors_.find(doc_parent);
  if (!doc_parent || it == anchors_.end())
    return false;
  const ULONGLONG age = ::GetTickCount64() - it->second.tick;
  if (age > static_cast<ULONGLONG>(max_age_ms >= 0 ? max_age_ms : 1500))
    return false;
  *out = it->second.rect;
  return true;
}

void Worker::_Store(HWND doc_window, const RECT& rect) {
  const HWND doc_parent = ::GetParent(doc_window);
  if (!doc_parent)
    return;
  std::lock_guard<std::mutex> lock(anchor_mutex_);
  Anchor& anchor = anchors_[doc_parent];
  anchor.rect = rect;
  anchor.doc_parent = doc_parent;
  anchor.tick = ::GetTickCount64();
  // Open documents are few; drop the least recently seen ones if a long
  // session somehow exceeds the limit.
  while (anchors_.size() > 8) {
    auto oldest = anchors_.begin();
    for (auto it = anchors_.begin(); it != anchors_.end(); ++it) {
      if (it->second.tick < oldest->second.tick)
        oldest = it;
    }
    anchors_.erase(oldest);
  }
}

void Worker::WarmUp() {
  _EnsureThread();
  std::unique_lock<std::mutex> lock(mutex_);
  wake_.wait_for(lock, std::chrono::milliseconds(50),
                 [this] { return ready_ || stop_; });
}

void Worker::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void Worker::_ThreadMain() {
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // Touch the activation path once here: a cold GetActiveObject costs more
  // than the whole per-keystroke budget, and this thread is not on it.
  {
    IDispatch* app = _ActiveWriter();
    if (app)
      app->Release();
  }
  // Fill the anchor once before anyone asks, so the first composition after a
  // server start is not waiting for the poll interval.
  {
    HWND doc_window = nullptr;
    if (Enabled() && TargetIsWriter(&doc_window)) {
      RECT rect = {};
      if (_QueryThroughCache(doc_window, &rect, &cache_))
        _Store(doc_window, rect);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
  }
  wake_.notify_all();
  for (;;) {
    HWND doc_window = nullptr;
    unsigned seq = 0;
    bool requested = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool signalled =
          wake_.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                         [this] { return stop_ || has_request_; });
      if (stop_)
        break;
      if (signalled && has_request_) {
        doc_window = request_window_;
        seq = request_seq_;
        has_request_ = false;
        requested = true;
      }
    }
    if (!requested) {
      // Idle: keep an anchor ready so the first candidate window of the next
      // composition does not wait for a lookup.
      if (!Enabled() || !TargetIsWriter(&doc_window))
        continue;
    }
    RECT rect = {};
    const bool ok = _QueryThroughCache(doc_window, &rect, &cache_);
    if (ok)
      _Store(doc_window, rect);
    if (requested) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (seq > done_seq_)
          done_seq_ = seq;
        result_ok_ = ok;
        if (ok)
          result_ = rect;
      }
      wake_.notify_all();
    }
    // Pump what queued up while we were inside the automation call, so a
    // pending incoming call is never rejected for lack of a message loop.
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
    }
  }
  cache_.Reset();
  ::CoUninitialize();
}

std::string _ToAnsi(const std::wstring& text) {
  if (text.empty())
    return std::string();
  const int need = ::WideCharToMultiByte(CP_ACP, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(need > 0 ? need : 0, '\0');
  if (need > 0)
    ::WideCharToMultiByte(CP_ACP, 0, text.c_str(),
                          static_cast<int>(text.size()), &out[0], need, nullptr,
                          nullptr);
  return out;
}

std::wstring _DescribeProbe(HWND doc_window) {
  std::wstring detail = L"app=";
  IDispatch* app = _ActiveWriter();
  if (!app)
    return detail + L"none";
  detail += L"ok";
  RECT rect = {};
  const bool ok = _QueryCaretRect(app, doc_window, &rect, &detail, nullptr);
  detail += ok ? L" ok" : L" fail";
  app->Release();
  return detail;
}

}  // namespace

void SetEnabled(bool enabled) {
  EnabledFlag().store(enabled);
}

bool Enabled() {
  return EnabledFlag().load();
}

bool TargetIsWriter(HWND* doc_window) {
  HWND found = _FindDocWindow();
  if (doc_window)
    *doc_window = found;
  return found != nullptr;
}

bool Query(HWND doc_window, RECT* out, int timeout_ms) {
  if (!Enabled() || !doc_window || !out)
    return false;
  return Worker::Instance().Query(doc_window, out, timeout_ms);
}

bool LatestAnchor(HWND doc_window, RECT* out, int max_age_ms) {
  if (!Enabled() || !doc_window || !out)
    return false;
  return Worker::Instance().Latest(doc_window, out, max_age_ms);
}

void WarmUp() {
  if (!Enabled())
    return;
  Worker::Instance().WarmUp();
}

void RunProbe(int seconds) {
  wchar_t temp[MAX_PATH] = {0};
  if (!::GetEnvironmentVariableW(L"TEMP", temp, _countof(temp)))
    wcscpy_s(temp, _countof(temp), L"C:\\Windows\\Temp");
  wchar_t path[MAX_PATH] = {0};
  _snwprintf_s(path, _countof(path), _TRUNCATE,
               L"%s\\weasel-wps-anchor-probe.log", temp);
  FILE* file = _wfopen(path, L"w");
  if (!file)
    return;
  fprintf(file, "# probe %d s\n", seconds);
  fflush(file);
  const ULONGLONG start = ::GetTickCount64();
  while (::GetTickCount64() - start <
         static_cast<ULONGLONG>(seconds > 0 ? seconds : 30) * 1000) {
    HWND foreground = ::GetForegroundWindow();
    wchar_t foreground_class[128] = {0};
    if (foreground)
      ::GetClassNameW(foreground, foreground_class, _countof(foreground_class));
    HWND doc_window = nullptr;
    const bool is_writer = TargetIsWriter(&doc_window);
    RECT sync_rect = {};
    const bool sync_ok = is_writer && Query(doc_window, &sync_rect);
    RECT latest_rect = {};
    const bool latest_ok = is_writer && LatestAnchor(doc_window, &latest_rect);
    fprintf(file,
            "fg=%p/%s writer=%d doc=%p sync=%d(%ld,%ld,h=%ld) latest=%d(%ld,%"
            "ld,h=%ld)\n",
            reinterpret_cast<void*>(foreground),
            _ToAnsi(foreground_class).c_str(), is_writer ? 1 : 0,
            reinterpret_cast<void*>(doc_window), sync_ok ? 1 : 0,
            sync_rect.left, sync_rect.top, sync_rect.bottom - sync_rect.top,
            latest_ok ? 1 : 0, latest_rect.left, latest_rect.top,
            latest_rect.bottom - latest_rect.top);
    if (is_writer && !sync_ok)
      fprintf(file, "  detail: %s\n",
              _ToAnsi(_DescribeProbe(doc_window)).c_str());
    fflush(file);
    ::Sleep(120);
  }
  fclose(file);
  Worker::Instance().Stop();
}

}  // namespace wps_anchor
}  // namespace weasel
