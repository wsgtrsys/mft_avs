// mediaengine-probe.cpp - ask the Media Foundation Media Engine what it makes of a file.
//
// Why this exists: "the system media player" on Windows 10 (????? / Movies & TV) does
// NOT go through MFCreateSourceReaderFromURL, and it is not MFPlay either.  It drives the
// Media Engine (the same component Internet Explorer and Edge used for <video>), which has
// its own format acceptance step before any decoder MFT is consulted.  The user-visible
// symptom of that step refusing a file is "?????? unknown ???????".
// bin/typedump.exe measures the SourceReader path; this measures the player path.
//
// Read the output like this:
//   * run the control file (samples/control_h264.mp4) in the same invocation.  If the
//     control reaches CANPLAY here, the harness itself is capable and a failure on another
//     file is that file being refused.  Without the control, a failure proves nothing:
//     no video renderer is attached to this engine.
//   * error 4 = MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED (the format step refused it);
//     error 3 = DECODE (it accepted the container and failed later).
// Set AVS_MFT_TRACE=1 to see whether our byte stream handler / decoder are entered at all:
// no [avsmft]/[avssrc] lines means we were never asked.
//
// usage: mediaengine-probe <file> [<file> ...]

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>

static volatile LONG g_done = 0;
static volatile LONG g_saw_error = 0;

static LONG Peek(volatile LONG* p) { return *p; }

static const wchar_t* EventName(DWORD e)
{
    switch (e) {
    case MF_MEDIA_ENGINE_EVENT_LOADSTART:       return L"LOADSTART";
    case MF_MEDIA_ENGINE_EVENT_PROGRESS:        return L"PROGRESS";
    case MF_MEDIA_ENGINE_EVENT_SUSPEND:       return L"SUSPENDED";
    case MF_MEDIA_ENGINE_EVENT_ABORT:           return L"ABORT";
    case MF_MEDIA_ENGINE_EVENT_ERROR:           return L"ERROR";
    case MF_MEDIA_ENGINE_EVENT_STALLED:         return L"STALLED";
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:  return L"LOADEDMETADATA";
    case MF_MEDIA_ENGINE_EVENT_CANPLAY:         return L"CANPLAY";
    case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH:  return L"CANPLAYTHROUGH";
    case MF_MEDIA_ENGINE_EVENT_ENDED:           return L"ENDED";
    default:                                    return L"(other)";
    }
}

struct Notify : public IMFMediaEngineNotify
{
    LONG m_refs;
    Notify() : m_refs(1) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refs); }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_refs);
        if (!r) delete this;
        return r;
    }

    IFACEMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        wprintf(L"    event %2u %-16s\n", event, EventName(event));
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            InterlockedExchange(&g_saw_error, 1);
            InterlockedExchange(&g_done, 1);
        } else if (event == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH ||
                   event == MF_MEDIA_ENGINE_EVENT_ENDED) {
            InterlockedExchange(&g_done, 1);
        }
        // param1 carries the event attributes in newer builds; nothing here needs them -
        // the engine's own GetError() is the authority on the reason.
        (void)param1; (void)param2;
        return S_OK;
    }
};

static HRESULT MakeUrl(const wchar_t* path, wchar_t* url, size_t cap)
{
    wchar_t full[_MAX_PATH];
    // The count GetFullPathNameW wants is in CHARACTERS, not bytes: passing sizeof(full)
    // (520 bytes for a 260-element wchar_t buffer) lets it write past the end and the run
    // dies with a stack buffer overrun (0xC0000409) before this function prints anything.
    if (!GetFullPathNameW(path, (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    // A file path is a valid IRI for MediaEngine as long as the separators are slashes.
    size_t n = wcslen(full);
    if (n + 10 > cap) return E_INVALIDARG;
    wcscpy_s(url, cap, L"file:///");
    size_t o = 8;
    for (size_t i = 0; i < n; i++)
        url[o + i] = (full[i] == L'\\') ? L'/' : full[i];
    url[o + n] = 0;
    return S_OK;
}

static int Probe(const wchar_t* path)
{
    wchar_t url[4096];
    HRESULT hr = MakeUrl(path, url, sizeof(url) / sizeof(url[0]));
    if (FAILED(hr)) { wprintf(L"  bad path: 0x%08X\n", hr); return 2; }

    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_saw_error, 0);

    Notify* notify = new Notify();
    IMFAttributes* attrs = nullptr;
    IMFMediaEngineClassFactory* factory = nullptr;
    IMFMediaEngine* engine = nullptr;
    int rc = 1;

    wprintf(L"  file: %s\n", path);
    hr = MFCreateAttributes(&attrs, 1);
    if (SUCCEEDED(hr)) hr = attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        // 0: the same flags a player uses for a normal file.  The engine is created without
        // a render target on purpose - we only ask what it decides about the format.
        hr = factory->CreateInstance(0, attrs, &engine);
    }
    if (SUCCEEDED(hr)) wprintf(L"    engine created\n");
    if (SUCCEEDED(hr)) hr = engine->SetMuted(TRUE);   // no audio device needed
    if (SUCCEEDED(hr)) hr = engine->SetSource(url);
    if (SUCCEEDED(hr)) hr = engine->Load();
    if (SUCCEEDED(hr)) {
        for (int i = 0; i < 150 && !Peek(&g_done); i++) Sleep(100);
        if (!Peek(&g_done)) wprintf(L"    (no terminal event within 15 s)\n");
    } else {
        wprintf(L"    SetSource/Load -> 0x%08X\n", hr);
    }

    IMFMediaError* err = nullptr;
    HRESULT hr2 = engine ? engine->GetError(&err) : E_POINTER;
    int errCode = -1;
    HRESULT extended = 0;
    if (SUCCEEDED(hr2) && err) {
        errCode = (int)err->GetErrorCode();
        extended = err->GetExtendedErrorCode();
    }
    double dur = engine ? engine->GetDuration() : 0.0;
    ULONG w = 0, h = 0;
    HRESULT hrSize = engine ? engine->GetNativeVideoSize(&w, &h) : E_FAIL;
    unsigned short ready = engine ? engine->GetReadyState() : 0;

    wprintf(L"  readyState=%u  duration=%.3f s  native video=%lux%lu (%s)\n",
            ready, dur, w, h, SUCCEEDED(hrSize) ? L"read ok" : L"not available");
    wprintf(L"  engine error=%d  extended=0x%08X\n", errCode, (unsigned)extended);
    if (errCode == 4)
        wprintf(L"  => the Media Engine refused the SOURCE (its format step).  No decoder MFT "
                L"of ours was involved.\n");
    else if (errCode == 3)
        wprintf(L"  => the Media Engine accepted the file and failed while playing it.\n");
    rc = (errCode == -1 || errCode == 0) && (SUCCEEDED(hrSize) && w && h);
    wprintf(L"  => %s\n", rc ? L"the Media Engine took this file"
                             : L"the Media Engine did NOT take this file");

    if (err) err->Release();
    if (attrs) attrs->Release();
    if (factory) factory->Release();
    if (engine) engine->Release();
    notify->Release();
    return rc ? 0 : 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: mediaengine-probe <file> [<file> ...]\n");
        return 2;
    }
    // The process can fast-fail inside the Media Engine; with a buffered stdout the progress
    // lines are lost and the tool looks as if it had printed nothing at all.
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        int r = 1;
        try { r = Probe(argv[i]); }
        catch (...) { wprintf(L"  => a C++ exception escaped the Media Engine" L"\n"); }
        if (r) bad++;
    }
    MFShutdown();
    CoUninitialize();
    return bad ? 1 : 0;
}
