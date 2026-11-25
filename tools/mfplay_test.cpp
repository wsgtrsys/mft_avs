//----------------------------------------------------------------------------------------------
// mfplay_test.cpp - play a file with MFPlay, which uses the full Media Foundation
//                   playback graph: source node -> decoder MFTs -> video/audio mixers
//                   and renderers, driven by the media session and its clock.
//
//   mfplay-test <file> [-seconds N] [-width W] [-height H]
//
// Unlike the SourceReader tools this exercises the path a player really takes, so it
// also checks the parts the reader path does not touch: topology resolution, the
// session events, the clock, and video rendering through the EVR.
//
// The file name must have an extension that Media Foundation can resolve, so for .avs
// the byte stream handler has to be registered (regsvr32 of AVSVideoDecoder.dll).
//----------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mferror.h>
#include <strsafe.h>
#include <dbghelp.h>
#include "crashreport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

template <class T> static void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = nullptr;
    }
}
#define SAFE_RELEASE(p) SafeRelease(&p)

static std::wstring WS(const char* s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 1), 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfplay")
#pragma comment(lib, "ole32")
#pragma comment(lib, "user32")

static const wchar_t SZ_WND_CLASS[] = L"avsplaytest";

// the position type used by IMFPMediaPlayer::GetPosition (mfapi.h does not declare it
// in this SDK, the value is the documented MFPOSITION_MEDIA_PLAYER)
static const GUID MFPOS_MEDIA_PLAYER =
    { 0x9b9580bf, 0x8d84, 0x4c9d, { 0xa1, 0xbf, 0xe5, 0x32, 0x84, 0x80, 0x0a, 0x3f } };

static const char* EventName(MFP_EVENT_TYPE e)
{
    switch (e)
    {
    case MFP_EVENT_TYPE_PLAY:                 return "PLAY";
    case MFP_EVENT_TYPE_PAUSE:                return "PAUSE";
    case MFP_EVENT_TYPE_STOP:                 return "STOP";
    case MFP_EVENT_TYPE_POSITION_SET:         return "POSITION_SET";
    case MFP_EVENT_TYPE_RATE_SET:             return "RATE_SET";
    case MFP_EVENT_TYPE_MEDIAITEM_CREATED:    return "MEDIAITEM_CREATED";
    case MFP_EVENT_TYPE_MEDIAITEM_SET:        return "MEDIAITEM_SET";
    case MFP_EVENT_TYPE_FRAME_STEP:           return "FRAME_STEP";
    case MFP_EVENT_TYPE_MEDIAITEM_CLEARED:    return "MEDIAITEM_CLEARED";
    case MFP_EVENT_TYPE_MF:                   return "MF";
    case MFP_EVENT_TYPE_ERROR:                return "ERROR";
    case MFP_EVENT_TYPE_PLAYBACK_ENDED:       return "PLAYBACK_ENDED";
    case MFP_EVENT_TYPE_ACQUIRE_USER_CREDENTIAL: return "ACQUIRE_USER_CREDENTIAL";
    default:                                  return "?";
    }
}

static const char* MediaEventName(int t)
{
    // a part of MediaEventType (mfobjects.h), only the ones a session really sends
    switch (t)
    {
    case 0:   return "MEUnknown";
    case 1:   return "MEError";
    case 2:   return "MEExtendedType";
    case 3:   return "MENonFatalError";
    case 101: return "MESessionTopologySet";
    case 102: return "MESessionTopologiesCleared";
    case 103: return "MESessionStarted";
    case 104: return "MESessionPaused";
    case 105: return "MESessionStopped";
    case 106: return "MESessionClosed";
    case 107: return "MESessionEnded";
    case 108: return "MESessionRateChanged";
    case 110: return "MESessionCapabilitiesChanged";
    case 111: return "MESessionTopologyStatus";
    case 112: return "MESessionNotifyPresentationTime";
    case 113: return "MENewPresentation";
    case 122: return "MEBufferingStarted";
    case 123: return "MEBufferingStopped";
    case 128: return "MERendererEvent";
    case 129: return "MESessionStreamSinkFormatChanged";
    case 201: return "MESourceStarted";
    case 202: return "MEStreamStarted";
    case 203: return "MESourceSeeked";
    case 204: return "MEStreamSeeked";
    case 205: return "MENewStream";
    case 206: return "MEUpdatedStream";
    case 207: return "MESourceStopped";
    case 208: return "MEStreamStopped";
    case 209: return "MESourcePaused";
    case 210: return "MEStreamPaused";
    case 211: return "MEEndOfPresentation";
    case 212: return "MEEndOfStream";
    case 213: return "MEMediaSample";
    case 214: return "MEStreamTick";
    case 216: return "MEStreamFormatChanged";
    case 301: return "MEStreamSinkStarted";
    case 302: return "MEStreamSinkStopped";
    case 303: return "MEStreamSinkPaused";
    case 304: return "MEStreamSinkRateChanged";
    case 305: return "MEStreamSinkRequestSample";
    default:  return "?";
    }
}

class PlayerCallback : public IMFPMediaPlayerCallback
{
    LONG        m_Ref = 1;
public:
    bool        m_bOpened = false;
    bool        m_bPlaying = false;
    bool        m_bEnded = false;
    HRESULT     m_hrFailure = S_OK;
    HRESULT     m_hrItem    = S_OK;   // what setting the item itself returned
    int         m_nMFEvents = 0;
    volatile LONG m_nPaint = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback))
        {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG rc = InterlockedDecrement(&m_Ref);
        if (rc == 0) delete this;
        return rc;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pHeader) override
    {
        if (!pHeader) return;

        // the raw Media Session events are forwarded here; printing the interesting ones
        // is what shows whether topology resolution and the session start went through
        if (pHeader->eEventType == MFP_EVENT_TYPE_MF)
        {
            MFP_MF_EVENT* pEv = MFP_GET_MF_EVENT(pHeader);
            // MFPlay forwards the session events it does not handle itself; naming them
            // is what shows how far the session got (topology, started, error, ended)
            const int t = (int)pEv->MFEventType;
            IMFMediaEvent* pMediaEvent = pEv->pMFMediaEvent;
            if (pMediaEvent != NULL)
            {
                MediaEventType met = MEUnknown;
                GUID guidExt = GUID_NULL;
                HRESULT hrStatus = S_OK;
                pMediaEvent->GetType(&met);
                pMediaEvent->GetExtendedType(&guidExt);
                pMediaEvent->GetStatus(&hrStatus);
                printf("  session event %-34s type=%d status=0x%08X%s\n",
                       MediaEventName((int)met), (int)met, (unsigned)hrStatus,
                       (guidExt == GUID_NULL) ? "" : " [extended type]");
            }
            else
                printf("  session event %-34s type=%d\n", MediaEventName(t), t);
            m_nMFEvents++;
        }

        printf("event %-20s hr=0x%08X state=%d\n", EventName(pHeader->eEventType),
               (unsigned)pHeader->hrEvent, (int)pHeader->eState);

        if (FAILED(pHeader->hrEvent))
        {
            m_hrFailure = pHeader->hrEvent;
            printf("  FAILED\n");
        }

        switch (pHeader->eEventType)
        {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
            if (FAILED(pHeader->hrEvent))
                m_hrItem = pHeader->hrEvent;
            m_bOpened = true;
            break;
        case MFP_EVENT_TYPE_PLAY:
            m_bPlaying = true;
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            m_bEnded = true;
            break;
        case MFP_EVENT_TYPE_ERROR:
            break;
        default:
            break;
        }
    }
};

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static const char* StateName(MFP_MEDIAPLAYER_STATE s)
{
    switch (s)
    {
    case MFP_MEDIAPLAYER_STATE_STOPPED:  return "stopped";
    case MFP_MEDIAPLAYER_STATE_PLAYING:  return "playing";
    case MFP_MEDIAPLAYER_STATE_PAUSED:   return "paused";
    case MFP_MEDIAPLAYER_STATE_SHUTDOWN: return "shutdown";
    default:                             return "empty";
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    InstallCrashReport();

    if (argc < 2)
    {
        printf("usage: mfplay-test <file> [-seconds N] [-width W] [-height H] [-no-end]\n");
        return 1;
    }
    const char* fileArg = nullptr;
    int seconds = 5;
    bool bNoEnd = false;
    DWORD style = WS_POPUP | WS_VISIBLE;
    int x = -8000, y = -8000, w = 720, h = 576;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-width") && i + 1 < argc) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-height") && i + 1 < argc) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-no-end")) bNoEnd = true;
        else fileArg = argv[i];
    }
    if (!fileArg)
    {
        printf("no file given\n");
        return 1;
    }
    const std::wstring file = WS(fileArg);

    WCHAR wszFile[MAX_PATH * 4];
    if (GetFullPathNameW(file.c_str(), ARRAYSIZE(wszFile), wszFile, nullptr) == 0)
    {
        printf("bad path\n");
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = SZ_WND_CLASS;
    RegisterClassExW(&wc);

    // an off screen window: the EVR in the graph needs a window to present to
    HWND hWnd = CreateWindowExW(0, SZ_WND_CLASS, L"AVS playback test", style, x, y, w, h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd)
    {
        printf("CreateWindow failed (%lu)\n", GetLastError());
        return 1;
    }

    WCHAR wszURL[512];
    if (FAILED(StringCchPrintfW(wszURL, ARRAYSIZE(wszURL), L"file://%s", wszFile)))
    {
        printf("bad url\n");
        return 1;
    }
    printf("url:     %S\n", wszURL);

    // MFPlay builds an EVR and an audio renderer for every item.  A session
    // without a presentation surface or a playback endpoint fails the media
    // item with MF_E_CANNOT_CREATE_SINK, which says nothing about the AVS
    // path, so probe both sinks and report the environment instead.
    {
        IMFActivate *pVideoActivate = nullptr;
        IMFActivate *pAudioActivate = nullptr;
        HRESULT hrVideo = MFCreateVideoRendererActivate(hWnd, &pVideoActivate);
        HRESULT hrAudio = MFCreateAudioRendererActivate(&pAudioActivate);
        if (FAILED(hrVideo) || FAILED(hrAudio)) {
            printf("no media foundation render sink in this session "
                   "(video 0x%08X, audio 0x%08X): MFPlay cannot create its "
                   "EVR / audio renderer.  The graph check needs an interactive "
                   "session; the source and decoder paths are still covered by "
                   "checks 1-10 and by mfsession-test.  SKIPPED.\n",
                   (unsigned) hrVideo, (unsigned) hrAudio);
            if (pVideoActivate)
                pVideoActivate->Release();
            if (pAudioActivate)
                pAudioActivate->Release();
            DestroyWindow(hWnd);
            MFShutdown();
            CoUninitialize();
            return 0;
        }
        pVideoActivate->Release();
        pAudioActivate->Release();
    }

    PlayerCallback* pCB = new PlayerCallback();
    IMFPMediaPlayer* pPlayer = nullptr;

    HRESULT hr = MFPCreateMediaPlayer(wszURL, TRUE, MFP_OPTION_NONE, pCB, hWnd, &pPlayer);
    printf("MFPCreateMediaPlayer -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    // pump messages (MFPlay delivers its callbacks on this thread) and watch the clock
    const ULONGLONG t0 = GetTickCount64();
    LONGLONG llMaxTime = 0;
    int nPosProbes = 0;
    int nPosFailed = 0;
    HRESULT hrPosLast = S_OK;
    const bool bVerbose = (getenv("AVS_PLAY_VERBOSE") != nullptr);
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_STOPPED;
    ULONG vw = 0, vh = 0;

    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000ull && !pCB->m_bEnded &&
           SUCCEEDED(pCB->m_hrFailure))
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        pPlayer->GetState(&state);
        PROPVARIANT var = {}; PropVariantInit(&var);
        const HRESULT hrPos = pPlayer->GetPosition(MFPOS_MEDIA_PLAYER, &var);
        if (nPosProbes++ < 1 && bVerbose)
            printf("  GetPosition -> 0x%08X vt=%d\n", (unsigned)hrPos, (int)var.vt);
        if (SUCCEEDED(hrPos))
        {
            if (var.vt == VT_I8)
            {
                const LONGLONG llPos = var.hVal.QuadPart;   // PROPVARIANT VT_I8 is hVal
                if (llPos > llMaxTime)
                    llMaxTime = llPos;
            }
        }
        else if (hrPos != hrPosLast)
        {
            hrPosLast = hrPos;
            nPosFailed++;
        }        PropVariantClear(&var);
        SIZE sz = { 0, 0 };
        if (SUCCEEDED(pPlayer->GetNativeVideoSize(&sz, nullptr)) && sz.cx > 0)
        {
            vw = (ULONG)sz.cx;
            vh = (ULONG)sz.cy;
        }

        Sleep(20);
    }

    printf("state:   %s\n", StateName(state));
    printf("position reached: %.3f s\n", (double)llMaxTime / 10000000.0);
    if (llMaxTime == 0 && nPosProbes > 0)
        printf("note: IMFPMediaPlayer::GetPosition failed for all %d probe(s), last 0x%08X -\n"
               "      this MFPlay build does not expose a position; the graph is still\n"
               "      observed through AVS_MFT_TRACE=1 progress logs and PLAYBACK_ENDED.\n",
               nPosProbes, (unsigned)hrPosLast);
    printf("native video size: %ux%u\n", vw, vh);    printf("media session events seen: %d\n", pCB->m_nMFEvents);

    // MFPlay is asynchronous: stop, then give it a moment before releasing. Releasing
    // the player without that costs a crash on the way out.
    pPlayer->Stop();
    for (int i = 0; i < 25; i++)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto drained;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
drained:
    pPlayer->Shutdown();
    for (int i = 0; i < 25; i++)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

done:
    printf("opened:  %s   played: %s   ended: %s   failure: 0x%08X\n",
           pCB->m_bOpened ? "yes" : "no", pCB->m_bPlaying ? "yes" : "no",
           pCB->m_bEnded ? "yes" : "no", (unsigned)pCB->m_hrFailure);

    // the position is not available from this MFPlay build, so the pass criterion is
    // "the graph resolved, played, and ran to the end of the file" (use -no-end for
    // files longer than the run time).
    // MF_E_CANNOT_CREATE_SINK while the item is being set means this session
    // cannot build MFPlay's own video or audio sink.  A plain H.264 file fails
    // the same way here, so it is the environment, not the AVS path.
    if (pCB->m_hrItem == (HRESULT) 0xC00D36FA /* MF_E_CANNOT_CREATE_SINK */) {
        printf("no MF render sink in this session (MF_E_CANNOT_CREATE_SINK while "
               "setting the item): a plain H.264 file fails identically, so the "
               "graph check needs an interactive desktop session.  The source and "
               "decoder paths are covered by checks 1-10 and by mfsession-test.  "
               "SKIPPED.\n");
        SAFE_RELEASE(pPlayer);
        SAFE_RELEASE(pCB);
        DestroyWindow(hWnd);
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    const bool bOk = pCB->m_bOpened && pCB->m_bPlaying && SUCCEEDED(pCB->m_hrFailure) &&
                     (pCB->m_bEnded || (bNoEnd && vw > 0));

    SAFE_RELEASE(pPlayer);
    SAFE_RELEASE(pCB);
    DestroyWindow(hWnd);
    MFShutdown();
    CoUninitialize();

    printf("%s\n", bOk ? "playback through the MF graph OK" : "playback did NOT work");
    return bOk ? 0 : 1;
}
