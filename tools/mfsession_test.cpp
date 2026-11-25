//----------------------------------------------------------------------------------------------
// mfsession_test.cpp - drive a raw IMFMediaSession by hand and report what the session does at
//                      the end of a presentation.
//
//   usage: mfsession-test <file> [-seconds N] [-start HNS] [-noevr] [-grabber] [-dec <decoder dll>] [-ts] [-dump <file>]
//
//   -dump <file>  with -grabber: write the pictures the graph presented, in presentation
//                 order (NV12).  Counting samples cannot show whether the right pictures
//                 arrived in the right order; a dump can be compared with a decode.
//
//   Why a hand-built topology: MFCreateSourceReaderFromURL hides it.  The question this tool
//   answers - the source reached the end and the decoder handed the last picture to the graph, so
//   does the session report MESessionEnded? - is only observable when the topology, the clock
//   polling and the event queue are under our control.
//
//   -dec <dll>  load a decoder MFT out of that DLL and put it in the topology as an explicit
//               transform node.  Without it the topology loader picks the registered MFT, which is
//               the production path; with it a measurement can hold one MFT instance under a
//               microscope (that is how the H.264-through-our-own-MFT control cell is built).
//   -ts         print an event timeline in milliseconds from Start() and dump the media types of
//               the source stream descriptors and of the topology nodes (AvsTypeDump.h).
//   -noevr      no video output node; used to tell "the graph has no sink in this session" apart
//               from "the graph has a sink that does not end".
//   -grabber    put a sample grabber sink (MFCreateSampleGrabberSinkActivate) on the video stream
//               instead of the EVR.  This is the only way this tree can count what reaches a sink:
//               the callback receives every delivered sample with its presentation time, and needs no
//               desktop session.  It is opt-in and never automatic: measured here, the H.264 control
//               that reaches MESessionEnded with the EVR delivers nothing to a grabber
//               (MF_E_UNSUPPORTED_FORMAT), so end-semantics runs must keep the EVR to stay comparable
//               with the pinned state.
//
//   Output contract.  scripts/session-matrix.sh, scripts/attribution-matrix.sh, scripts/eos-table.py,
//   scripts/type-diff.py, scripts/type-contract-test.py and run-tests items 11c/11d/11i parse these
//   lines, so a format change here has to be made together with them:
//       started: yes|no   ended: yes|no   failure: 0x........
//       clock reached: N.NNN s
//         event <name> status=0x........            [" @+N ms"] [" topology=<state>(<n>)"]
//       media session runs (clock advances)   /   media session does NOT advance
//       ... SKIPPED.                                when this session cannot create a render sink
//   Exit status: 0 a session ran (started and the clock advanced) or the run was skipped because
//   no render sink exists here; 127 the file could not be opened as media; 1 otherwise.
//
//   The clock line reports what the calls returned, not an interpretation: gettime is the result of
//   IMFPresentationClock::GetTime, state is IMFClock::GetState and chars is
//   GetClockCharacteristics.  The clock is taken from IMFMediaSession::GetClock, which is what this
//   SDK declares; this SDK has no MF_SERVICE_* GUIDs and no IMFClockState type.
//
//   History, because it changes how these numbers should be read: this file was truncated to zero
//   bytes by a patch script that opened it for writing before checking its own arguments, and this
//   tree has no version control.  The output contract above was recovered from the compiled object
//   (tmp/recover/obj_strings.txt), from that binary's import table (tmp/imports.txt: it uses
//   MFCreateAudioRendererActivate, MFCreateVideoRendererActivate, MFCreateTopology,
//   MFCreateTopologyNode and no clock or service imports) and from the logs the tool had produced
//   (tmp/eos/, tmp/types/).  The rewrite was accepted only after
//       python scripts/eos-table.py --check
//   reproduced the pinned three-cell state and the run-tests session items still passed.
//----------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <evr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#include "crashreport.h"
#include "../MediaFoundationTransform/AVSVideoDecoder/AvsTypeDump.h"

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")       // CommandLineToArgvW
#pragma comment(lib, "gdi32")

template <class T> static void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}
#define SAFE_RELEASE(p) if ((p)) { SafeRelease(&(p)); }

static void GuidToString(const GUID& g, char* out /* 39 bytes */)
{
    sprintf_s(out, 39, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              (unsigned)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
              g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
              g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool GuidIsFourcc(const GUID& g)
{
    return g.Data2 == 0 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 && g.Data4[7] == 0x71;
}

static const char* FourccName(const GUID& g)
{
    static char buf[8];
    if (!GuidIsFourcc(g))
        return nullptr;
    buf[0] = (char)((g.Data1 >> 0) & 0xFF);
    buf[1] = (char)((g.Data1 >> 8) & 0xFF);
    buf[2] = (char)((g.Data1 >> 16) & 0xFF);
    buf[3] = (char)((g.Data1 >> 24) & 0xFF);
    buf[4] = 0;
    if (buf[0] >= ' ' && buf[0] < 0x7F && buf[1] >= ' ' && buf[1] < 0x7F &&
        buf[2] >= ' ' && buf[2] < 0x7F && buf[3] >= ' ' && buf[3] < 0x7F)
        return buf;
    return nullptr;
}

// Event names.  This SDK declares only part of MediaEventType: mfobjects.h has the presentation,
// source, stream, sink and transform ranges, while the session range (MEError,
// MESessionTopologyStatus, MESessionEnded, MESessionCapabilitiesChanged, ...) is missing entirely,
// and so are the GUID_MExxxx extended types.  Two sources of names are used and both are checked:
//   - names the SDK itself declares, referenced through the enum identifiers so the compiler
//     verifies the value;
//   - the session range, whose numbers were recovered by running this tool against the previous
//     build's logs (tmp/types/h264b.log and tmp/eos/*.log show the same event order and the same
//     MF_TOPOSTATUS payloads); the recovered numbers are named below and nothing else is named.
// An event whose number is not in either list prints as ME#<number> with its extended type GUID.
struct EventNameEntry { ULONG value; const char* name; };

// Every name below is referenced through the SDK's own MediaEventType identifiers, so the compiler
// checks the numbers.  This SDK does declare the whole range (mfobjects.h, the anonymous enum that
// starts at MEUnknown = 0): the session range is 100..129, sources 200..222, sinks 300..327, trust
// 400..403, transforms 600..604.  An earlier rewrite of this file could not find that enum with
// grep and started inventing numbers; inventing them here would silently rename the events in
// scripts/eos-table.py, scripts/attribution-matrix.sh and the pinned 11d/11i state, which is the
// failure mode this reconstruction has to avoid.
static const EventNameEntry g_eventNames[] = {
    { MEUnknown,                             "MEUnknown" },
    { MEError,                               "MEError" },
    { MEExtendedType,                        "MEExtendedType" },
    { MENonFatalError,                       "MENonFatalError" },

    { MESessionTopologySet,                  "MESessionTopologySet" },
    { MESessionTopologiesCleared,            "MESessionTopologiesCleared" },
    { MESessionStarted,                      "MESessionStarted" },
    { MESessionPaused,                       "MESessionPaused" },
    { MESessionStopped,                      "MESessionStopped" },
    { MESessionClosed,                       "MESessionClosed" },
    { MESessionEnded,                        "MESessionEnded" },
    { MESessionRateChanged,                  "MESessionRateChanged" },
    { MESessionScrubSampleComplete,          "MESessionScrubSampleComplete" },
    { MESessionCapabilitiesChanged,          "MESessionCapabilitiesChanged" },
    { MESessionTopologyStatus,               "MESessionTopologyStatus" },
    { MESessionNotifyPresentationTime,       "MESessionNotifyPresentationTime" },
    { MESessionStreamSinkFormatChanged,      "MESessionStreamSinkFormatChanged" },

    { MENewPresentation,                     "MENewPresentation" },
    { MEBufferingStarted,                    "MEBufferingStarted" },
    { MEBufferingStopped,                    "MEBufferingStopped" },
    { MEConnectStart,                        "MEConnectStart" },
    { MEConnectEnd,                          "MEConnectEnd" },
    { MEReconnectStart,                      "MEReconnectStart" },
    { MEReconnectEnd,                        "MEReconnectEnd" },
    { MERendererEvent,                       "MERendererEvent" },

    { MESourceUnknown,                       "MESourceUnknown" },
    { MESourceStarted,                       "MESourceStarted" },
    { MEStreamStarted,                       "MEStreamStarted" },
    { MESourceSeeked,                        "MESourceSeeked" },
    { MEStreamSeeked,                        "MEStreamSeeked" },
    { MENewStream,                           "MENewStream" },
    { MEUpdatedStream,                       "MEUpdatedStream" },
    { MESourceStopped,                       "MESourceStopped" },
    { MEStreamStopped,                       "MEStreamStopped" },
    { MESourcePaused,                        "MESourcePaused" },
    { MEStreamPaused,                        "MEStreamPaused" },
    { MEEndOfPresentation,                   "MEEndOfPresentation" },
    { MEEndOfStream,                         "MEEndOfStream" },
    { MEMediaSample,                         "MEMediaSample" },
    { MEStreamTick,                          "MEStreamTick" },
    { MEStreamThinMode,                      "MEStreamThinMode" },
    { MEStreamFormatChanged,                 "MEStreamFormatChanged" },
    { MESourceRateChanged,                   "MESourceRateChanged" },
    { MEEndOfPresentationSegment,            "MEEndOfPresentationSegment" },
    { MESourceCharacteristicsChanged,        "MESourceCharacteristicsChanged" },
    { MESourceRateChangeRequested,           "MESourceRateChangeRequested" },
    { MESourceMetadataChanged,               "MESourceMetadataChanged" },

    { MESinkUnknown,                         "MESinkUnknown" },
    { MEStreamSinkStarted,                   "MEStreamSinkStarted" },
    { MEStreamSinkStopped,                   "MEStreamSinkStopped" },
    { MEStreamSinkPaused,                    "MEStreamSinkPaused" },
    { MEStreamSinkRateChanged,               "MEStreamSinkRateChanged" },
    { MEStreamSinkRequestSample,             "MEStreamSinkRequestSample" },
    { MEStreamSinkMarker,                    "MEStreamSinkMarker" },
    { MEStreamSinkPrerolled,                 "MEStreamSinkPrerolled" },
    { MEStreamSinkScrubSampleComplete,       "MEStreamSinkScrubSampleComplete" },
    { MEStreamSinkFormatChanged,             "MEStreamSinkFormatChanged" },
    { MEStreamSinkDeviceChanged,             "MEStreamSinkDeviceChanged" },
    { MEQualityNotify,                       "MEQualityNotify" },
    { MESinkInvalidated,                     "MESinkInvalidated" },

    { MEPolicyChanged,                       "MEPolicyChanged" },
    { MEContentProtectionMessage,            "MEContentProtectionMessage" },

    { METransformNeedInput,                  "METransformNeedInput" },
    { METransformHaveOutput,                 "METransformHaveOutput" },
    { METransformDrainComplete,              "METransformDrainComplete" },
    { METransformMarker,                     "METransformMarker" },
    { METransformInputStreamStateChanged,    "METransformInputStreamStateChanged" },
};

static const char* EventName(ULONG value)
{
    for (size_t i = 0; i < sizeof(g_eventNames) / sizeof(g_eventNames[0]); i++)
        if (g_eventNames[i].value == value)
            return g_eventNames[i].name;
    return nullptr;
}

static const char* TopoStatusName(LONG st)
{
    switch (st)
    {
    case MF_TOPOSTATUS_INVALID:         return "invalid";
    case MF_TOPOSTATUS_READY:           return "ready";
    case MF_TOPOSTATUS_STARTED_SOURCE:  return "started-source";
    case MF_TOPOSTATUS_DYNAMIC_CHANGED: return "dynamic-changed";
    case MF_TOPOSTATUS_SINK_SWITCHED:   return "sink-switched";
    case MF_TOPOSTATUS_ENDED:           return "ended";
    default:                            return "unknown";
    }
}

static const char* ClockStateName(MFCLOCK_STATE st)
{
    switch (st)
    {
    case MFCLOCK_STATE_INVALID:     return "invalid";
    case MFCLOCK_STATE_RUNNING:     return "running";
    case MFCLOCK_STATE_STOPPED:     return "stopped";
    case MFCLOCK_STATE_PAUSED:      return "paused";
    default:                        return "unknown";
    }
}

// The topology status of a MESessionTopologyStatus event is not the event value (measured: GetValue
// returns an interface there) and this SDK's IMFMediaEvent does not declare any extended-information
// accessor at all.  What works is the attribute store the event object itself carries: ask the event
// for IMFAttributes and read MF_EVENT_TOPOLOGY_STATUS from it.  The other route was tried and is
// false here: extending IMFMediaEvent by the two slots MF is documented to have after GetValue and
// calling GetExtendedInformation through that returns a meaningless 0x735CEEB0, i.e. the slot
// assumption is wrong on this build.
//----------------------------------------------------------------------------------------------
// what one run accumulates
struct RunState
{
    bool    bStarted;
    bool    bEnded;
    bool    bEndedByNumber;   // set from the MediaEventType number the tool names MESessionEnded
    bool    bTopoEnded;
    bool    bReady;
    HRESULT hrFailure;
    int     nEvents;
    double  dClockMax;
    // KR14.1 measurement: who stops the presentation clock, and when does the graph say it is over.
    // The distinction matters because "no MESessionEnded" is only a defect if the host has no other
    // way to learn that playback finished; these fields record the alternatives with their ordering.
    double  dDuration;              // MF_PD_DURATION in seconds, -1 when the source states none
    bool    bFirstStopped;          // a poll saw MFCLOCK_STATE_STOPPED while we had not called Stop()
    ULONGLONG llFirstStoppedMs;     // wall ms of that first poll, zero of the column is Start()
    ULONGLONG llHostStopMs;         // wall ms at which THIS process calls IMFMediaSession::Stop()
    bool    bTraceTime;
    LARGE_INTEGER liStart;    // wall clock at Start(), the zero of the @+N ms column
    std::vector<std::pair<ULONGLONG, std::string> > timeline;

    RunState() : bStarted(false), bEnded(false), bEndedByNumber(false), bTopoEnded(false),
                 bReady(false), hrFailure(S_OK), nEvents(0), dClockMax(0.0), dDuration(-1.0),
                 bFirstStopped(false), llFirstStoppedMs(0), llHostStopMs(0), bTraceTime(false)
    {
        liStart.QuadPart = 0;
    }
    ULONGLONG NowMs() const
    {
        if (liStart.QuadPart == 0)
            return 0;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return (ULONGLONG)(((LONGLONG)(now.QuadPart - liStart.QuadPart) * 1000) / freq.QuadPart);
    }
};

static std::vector<std::string> g_attrLines;    // topology dumps, printed at the end

//----------------------------------------------------------------------------------------------
// The session's event pump is synchronous: GetEvent(MF_EVENT_FLAG_NO_WAIT) in the same loop that
// polls the clock.  An asynchronous BeginGetEvent + IMFAsyncCallback pump was tried first and
// measured wrong - with GetParameters reporting queue length 0 only the license-acquisition event
// ever arrived, which would have made every cell look dead for a reason that has nothing to do
// with the pipeline.  This SDK declares IMFMediaSession : IMFMediaEventGenerator, so the session
// object itself is polled directly; no MF_SERVICE_* GUID (none exist here) is involved.
//----------------------------------------------------------------------------------------------
// a window for the EVR; MF needs a window to present into, and an off-screen one keeps this a
// command line tool
// What actually reaches a sink.  Until this option existed, nothing in this tree had a number for
// "how many pictures were delivered": the tool counted events, not samples.  The sample grabber is
// an ordinary IMFStreamSink node (MFCreateSampleGrabberSinkActivate lives in mf.dll, which this
// file already links), and in this SDK its callback interface IMFSampleGrabberSinkCallback
// (8C7B80BF-EE42-4b59-B1DF-55668E1BDCA8) hands over llSampleTime, so both the count and the first
// and last presentation time are measurable.  Unlike the EVR it needs no desktop session.
struct GrabCallback : public IMFSampleGrabberSinkCallback
{
    ULONG refs;
    long  samples;
    long long firstTime, lastTime;
    unsigned long long bytes;
    long width, height;
    // the clock callbacks are part of this interface, and whether they fire is itself evidence:
    // a sink that never sees OnClockStart was never told to present anything.
    long clockStarts, clockStops, clockPauses, clockRestarts, clockRates;
    long long clockStartOffset;
    // -dump <file>: write the pictures in the order the graph presented them.  "how many
    // pictures arrived" says nothing about whether they are the right ones in the right
    // order; this makes that comparable with an independent decode of the same file.
    FILE*    fDump;

    GrabCallback() : refs(1), samples(0), firstTime(0), lastTime(0), bytes(0),
                     width(0), height(0), clockStarts(0), clockStops(0), clockPauses(0),
                     clockRestarts(0), clockRates(0), clockStartOffset(0), fDump(nullptr) {}
    virtual ~GrabCallback() {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSampleGrabberSinkCallback) ||
            riid == __uuidof(IMFClockStateSink))
        {
            *ppv = static_cast<IMFSampleGrabberSinkCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return (ULONG)InterlockedIncrement((LONG*)&refs); }
    STDMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement((LONG*)&refs);
                                     if (r == 0) delete this;
                                     return (ULONG)r; }

    // IMFClockStateSink, all five members as this SDK declares them (mfidl.h:6083)
    STDMETHODIMP OnClockStart(MFTIME hnsSystemTime, LONGLONG llStartOffset)
    {
        clockStarts++;
        clockStartOffset = llStartOffset;
        printf("  [grab] OnClockStart(system=%lld offset=%lld)\n",
               (long long)hnsSystemTime, llStartOffset);
        fflush(stdout);
        return S_OK;
    }
    STDMETHODIMP OnClockStop(MFTIME)    { clockStops++;   return S_OK; }
    STDMETHODIMP OnClockPause(MFTIME)   { clockPauses++;  return S_OK; }
    STDMETHODIMP OnClockRestart(MFTIME) { clockRestarts++; return S_OK; }
    STDMETHODIMP OnClockSetRate(MFTIME, float) { clockRates++; return S_OK; }
    STDMETHODIMP OnSetPresentationClock(IMFPresentationClock*) { return S_OK; }

    STDMETHODIMP OnProcessSample(REFGUID, DWORD dwFlags, LONGLONG llTime, LONGLONG llDuration,
                                 const BYTE* pbData, DWORD cb)
    {
        if (samples == 0)
            firstTime = llTime;
        samples++;
        lastTime = llTime;
        bytes += cb;
        if (fDump && pbData)
            fwrite(pbData, 1, cb, fDump);
        if (samples <= 3 || samples % 25 == 0)
            printf("  [grab] #%ld time=%lld dur=%lld size=%lu flags=0x%lX\n",
                   samples, llTime, llDuration, (unsigned long)cb, (unsigned long)dwFlags);
        fflush(stdout);
        return S_OK;
    }
    STDMETHODIMP OnShutdown() { return S_OK; }
};

static std::wstring g_wDumpPath;         // -dump <file>, first video stream only
static std::vector<GrabCallback*> g_grabs;     // one per video stream, reported at the end

// The grabber wants an explicit uncompressed output type; the graph puts the decoder in front of it.
// Width and height are taken from what the source publishes.  If a stream states no size, no grabber
// is created and the run says so - a measurement tool must not invent a frame size.
static IMFActivate* CreateGrabberSink(IMFStreamDescriptor* pSD)
{
    IMFMediaTypeHandler* pHandler = nullptr;
    IMFMediaType* pSrcType = nullptr;
    if (FAILED(pSD->GetMediaTypeHandler(&pHandler)) || pHandler == nullptr)
        return nullptr;
    pHandler->GetMediaTypeByIndex(0, &pSrcType);
    SAFE_RELEASE(pHandler);
    if (pSrcType == nullptr)
        return nullptr;

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(pSrcType, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
    {
        printf("  no grabber sink for this stream: it publishes no frame size, and the grabber "
               "cannot be created without one\n");
        SAFE_RELEASE(pSrcType);
        return nullptr;
    }
    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(pSrcType, MF_MT_FRAME_RATE, &num, &den)) || num == 0 || den == 0)
    {
        num = 25;
        den = 1;
        printf("  this stream states no frame rate; the grabber needs one, so 25/1 is used and "
               "said out loud (it changes pacing, not what is delivered)\n");
    }

    IMFMediaType* pType = nullptr;
    IMFActivate* pActivate = nullptr;
    GrabCallback* pCb = new GrabCallback();
    if (!g_wDumpPath.empty() && g_grabs.empty())
    {
        pCb->fDump = _wfopen(g_wDumpPath.c_str(), L"wb");
        printf("  [grab] pictures are written to %ls (NV12, in presentation order)\n",
               g_wDumpPath.c_str());
    }
    pCb->width = (long)w;
    pCb->height = (long)h;
    if (SUCCEEDED(MFCreateMediaType(&pType)) && pType != nullptr)
    {
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, num, den);
        HRESULT hr = MFCreateSampleGrabberSinkActivate(pType, pCb, &pActivate);
        printf("  sample grabber sink requested: %ux%u at %u/%u -> 0x%08X\n",
               w, h, num, den, (unsigned)hr);
        if (SUCCEEDED(hr) && pActivate != nullptr)
            g_grabs.push_back(pCb);       // the sink holds its own reference; ours is for the report
        else
            SAFE_RELEASE(pCb);
    }
    SAFE_RELEASE(pType);
    SAFE_RELEASE(pSrcType);
    return pActivate;
}

static HWND g_hwnd = nullptr;

static bool EnsureWindow()
{
    if (g_hwnd)
        return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"avssessiontest";
    if (!GetClassInfoExW(wc.hInstance, wc.lpszClassName, &wc))
    {
        if (!RegisterClassExW(&wc))
            return false;
    }
    // WS_POPUP | WS_VISIBLE, exactly as the working MFPlay harness creates it: an invisible window
    // was measured here to be enough to break topology resolution (the session's SetTopology came
    // back with MESessionTopologySet status 0x80004002 E_NOINTERFACE for a plain H.264 file with a
    // system decoder, i.e. a failure with nothing to do with AVS).  Still off screen, so this stays
    // a command line tool.
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"AVS session test", WS_POPUP | WS_VISIBLE,
                             -8000, -8000, 720, 576, nullptr, nullptr, wc.hInstance, nullptr);
    return g_hwnd != nullptr;
}

// What the topology loader decided.  MF_TOPOLOGY_RESOLUTION_STATUS on the topology and
// MF_TOPONODE_ERRORCODE / MF_TOPONODE_ERROR_MAJORTYPE / MF_TOPONODE_ERROR_SUBTYPE on each node are
// the SDK's own report of why a topology did not resolve; reading them beats guessing from the
// session's MESessionTopologySet status.
static void DumpTopologyResolution(IMFTopology* pTopology, const char* when)
{
    WORD cNodes = 0;
    if (FAILED(pTopology->GetNodeCount(&cNodes)))
        return;
    UINT32 uRes = 0xFFFFFFFF;
    pTopology->GetUINT32(MF_TOPOLOGY_RESOLUTION_STATUS, &uRes);
    printf("[topo-res] when=%s nodes=%u resolution_status=0x%08X (%s)\n", when,
           (unsigned)cNodes, (unsigned)uRes,
           uRes == MF_TOPOLOGY_RESOLUTION_SUCCEEDED ? "succeeded" : "not succeeded");
    for (WORD i = 0; i < cNodes; i++)
    {
        IMFTopologyNode* pNode = nullptr;
        if (FAILED(pTopology->GetNode(i, &pNode)) || pNode == nullptr)
            continue;
        MF_TOPOLOGY_TYPE nodeType = MF_TOPOLOGY_OUTPUT_NODE;
        pNode->GetNodeType(&nodeType);
        // IMFTopologyNode's attribute methods are not declared in this SDK's header, but the object
        // answers to IMFAttributes, which is what carries the loader's error attributes.
        IMFAttributes* pAttr = nullptr;
        HRESULT hrAttr = pNode->QueryInterface(__uuidof(IMFAttributes), (void**)&pAttr);
        BOOL bHasErr = FALSE;
        HRESULT hrErr = S_OK;
        GUID mj = GUID_NULL, sub = GUID_NULL;
        UINT32 connectMethod = 0xFFFFFFFF;
        if (SUCCEEDED(hrAttr) && pAttr != nullptr)
        {
            // this SDK's IMFAttributes has no GetError; MF_TOPONODE_ERRORCODE holds an MF_ATTRIBUTE_...
            // error value, read it through GetItem and report the PROPVARIANT type we actually get
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pAttr->GetItem(MF_TOPONODE_ERRORCODE, &pv)))
            {
                bHasErr = TRUE;
                hrErr = (pv.vt == VT_ERROR) ? pv.scode : S_OK;
            }
            PropVariantClear(&pv);
            pAttr->GetGUID(MF_TOPONODE_ERROR_MAJORTYPE, &mj);
            pAttr->GetGUID(MF_TOPONODE_ERROR_SUBTYPE, &sub);
            pAttr->GetUINT32(MF_TOPONODE_CONNECT_METHOD, &connectMethod);
            pAttr->Release();
        }
        char mjBuf[39], subBuf[39];
        GuidToString(mj, mjBuf);
        GuidToString(sub, subBuf);
        printf("[topo-res] node %u type=%s(%d) error=%s 0x%08X major={%s} subtype={%s} "
               "connect_method=0x%X\n",
               (unsigned)i,
               nodeType == MF_TOPOLOGY_SOURCESTREAM_NODE ? "source-stream" :
               nodeType == MF_TOPOLOGY_TRANSFORM_NODE ? "transform" :
               nodeType == MF_TOPOLOGY_OUTPUT_NODE ? "output" : "other",
               (int)nodeType, bHasErr ? "set" : "unset", (unsigned)hrErr, mjBuf, subBuf,
               (unsigned)connectMethod);
        // the chain as the loader left it: which outputs are actually linked, and to what
        DWORD cOut = 0;
        if (SUCCEEDED(pNode->GetOutputCount(&cOut)))
        {
            for (DWORD j = 0; j < cOut; j++)
            {
                IMFTopologyNode* pDown = nullptr;
                DWORD idxDown = 0;
                HRESULT hrLink = pNode->GetOutput(j, &pDown, &idxDown);
                if (SUCCEEDED(hrLink) && pDown != nullptr)
                {
                    MF_TOPOLOGY_TYPE tDown = MF_TOPOLOGY_OUTPUT_NODE;
                    pDown->GetNodeType(&tDown);
                    WORD downIdx = 0xFFFF;
                    for (WORD k = 0; k < cNodes; k++)
                    {
                        IMFTopologyNode* pCand = nullptr;
                        if (SUCCEEDED(pTopology->GetNode(k, &pCand)) && pCand == pDown)
                        {
                            downIdx = k;
                            SAFE_RELEASE(pCand);
                            break;
                        }
                        SAFE_RELEASE(pCand);
                    }
                    printf("[topo-res]   node %u output %u -> node %u input %u type=%s(%d)\n",
                           (unsigned)i, j, (unsigned)downIdx, (unsigned)idxDown,
                           tDown == MF_TOPOLOGY_SOURCESTREAM_NODE ? "source-stream" :
                           tDown == MF_TOPOLOGY_TRANSFORM_NODE ? "transform" :
                           tDown == MF_TOPOLOGY_OUTPUT_NODE ? "output" : "other", (int)tDown);
                    pDown->Release();
                }
                else
                    printf("[topo-res]   node %u output %u not linked -> 0x%08X\n",
                           (unsigned)i, j, (unsigned)hrLink);
            }
        }
        pNode->Release();
    }
    fflush(stdout);
}

// Ask the session's own topology loader to resolve the topology and report what it says.
// IMFGetService and IMFTopoLoader are declared by this SDK, and MF_SESSION_TOPOLOADER is the
// service the session uses for SetTopology, so this is the same code path - only synchronous, and
// with the loader's HRESULT visible instead of inferred from an event's status field.
static void ProbeTopologyLoader(IMFMediaSession* pSession, IMFTopology* pTopology)
{
    IMFGetService* pSvc = nullptr;
    HRESULT hr = pSession->QueryInterface(__uuidof(IMFGetService), (void**)&pSvc);
    if (FAILED(hr) || pSvc == nullptr)
    {
        printf("[topo-loader] session has no IMFGetService -> 0x%08X\n", (unsigned)hr);
        return;
    }
    IMFTopoLoader* pLoader = nullptr;
    hr = pSvc->GetService(MF_SESSION_TOPOLOADER, IID_IMFTopoLoader, (void**)&pLoader);
    pSvc->Release();
    if (FAILED(hr) || pLoader == nullptr)
    {
        printf("[topo-loader] GetService(MF_SESSION_TOPOLOADER) -> 0x%08X\n", (unsigned)hr);
        return;
    }
    IMFTopology* pResolved = nullptr;
    HRESULT hrLoad = pLoader->Load(pTopology, &pResolved, nullptr);
    printf("[topo-loader] Load -> 0x%08X\n", (unsigned)hrLoad);
    if (pResolved != nullptr)
        DumpTopologyResolution(pResolved, "loader-resolved");
    SAFE_RELEASE(pResolved);
    pLoader->Release();
}

//----------------------------------------------------------------------------------------------
// dump every media type a stream descriptor offers, under -ts
static void DumpStreamTypes(IMFStreamDescriptor* pSD, unsigned stream, bool bSelected)
{
    if (!AvsTypeDumpOn())
        return;
    IMFMediaTypeHandler* pHandler = nullptr;
    if (FAILED(pSD->GetMediaTypeHandler(&pHandler)) || pHandler == nullptr)
        return;
    DWORD c = 0;
    pHandler->GetMediaTypeCount(&c);
    char tag[64];
    sprintf_s(tag, sizeof(tag), "src-sd%u-%s", stream, bSelected ? "sel" : "unsel");
    for (DWORD j = 0; j < c; j++)
    {
        IMFMediaType* pMT = nullptr;
        if (SUCCEEDED(pHandler->GetMediaTypeByIndex(j, &pMT)) && pMT != nullptr)
        {
            char full[80];
            sprintf_s(full, sizeof(full), "%s.t%u", tag, j);
            AvsDumpType(full, pMT);
            pMT->Release();
        }
    }
    pHandler->Release();
}

// What the topology ended up containing, under -ts.  Note what this SDK does and does not expose:
// IMFTopologyNode inherits IMFAttributes, so node attributes are readable, and the node's object
// (the renderer for an output node) is reachable through GetObject; the loader does not fill
// GetOutputPrefType / GetInputPrefType here, and that is printed as "no preferred type" instead of
// being guessed.
static void DumpTopologyTypes(IMFTopology* pTopology, const char* when)
{
    if (!AvsTypeDumpOn())
        return;
    WORD cNodes = 0;
    if (FAILED(pTopology->GetNodeCount(&cNodes)))
        return;
    g_attrLines.push_back("[attrdump] topology nodes=" + std::to_string((unsigned)cNodes) +
                          " when=" + when);
    for (WORD i = 0; i < cNodes; i++)
    {
        IMFTopologyNode* pNode = nullptr;
        if (FAILED(pTopology->GetNode(i, &pNode)) || pNode == nullptr)
            continue;
        MF_TOPOLOGY_TYPE nodeType = MF_TOPOLOGY_OUTPUT_NODE;
        pNode->GetNodeType(&nodeType);
        IUnknown* pObj = nullptr;
        pNode->GetObject(&pObj);
        IMFAttributes* pAttr = nullptr;
        HRESULT hrAttr = pNode->QueryInterface(__uuidof(IMFAttributes), (void**)&pAttr);
        IUnknown* pSD = nullptr;
        if (SUCCEEDED(hrAttr) && pAttr)
            pAttr->GetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, IID_IUnknown, (void**)&pSD);
        char line[256];
        sprintf_s(line, sizeof(line),
                  "[attrdump] topo-node%u type=%s(%d) stream_desc=%s node_object=%s",
                  (unsigned)i,
                  nodeType == MF_TOPOLOGY_SOURCESTREAM_NODE ? "source-stream" :
                  nodeType == MF_TOPOLOGY_TRANSFORM_NODE ? "transform" :
                  nodeType == MF_TOPOLOGY_OUTPUT_NODE ? "output" : "other",
                  (int)nodeType, pSD ? "yes" : "no", pObj ? "yes" : "no");
        g_attrLines.push_back(line);
        SAFE_RELEASE(pSD);
        SAFE_RELEASE(pObj);

        for (DWORD j = 0; j < 1; j++)
        {
            char tag[64];
            IMFMediaType* pMT = nullptr;
            sprintf_s(tag, sizeof(tag), "topo-n%u-out%u", (unsigned)i, j);
            if (SUCCEEDED(pNode->GetOutputPrefType(j, &pMT)) && pMT != nullptr)
            {
                AvsDumpType(tag, pMT);
                pMT->Release();
            }
            else
                g_attrLines.push_back(std::string("[attrdump] ") + tag + " no preferred type");
            pMT = nullptr;
            sprintf_s(tag, sizeof(tag), "topo-n%u-in%u", (unsigned)i, j);
            if (SUCCEEDED(pNode->GetInputPrefType(j, &pMT)) && pMT != nullptr)
            {
                AvsDumpType(tag, pMT);
                pMT->Release();
            }
            else
                g_attrLines.push_back(std::string("[attrdump] ") + tag + " no preferred type");
        }
        SAFE_RELEASE(pAttr);
        pNode->Release();
    }
}

//----------------------------------------------------------------------------------------------
static void HandleEvent(IMFMediaEvent* pEvent, RunState& run)
{
    MediaEventType met = (MediaEventType)0;
    pEvent->GetType(&met);
    GUID extType = GUID_NULL;
    pEvent->GetExtendedType(&extType);
    HRESULT hrStatus = S_OK;
    pEvent->GetStatus(&hrStatus);

    ULONG value = (ULONG)met;
    const char* named = EventName(value);

    char nameBuf[80];
    char extBuf[39];
    GuidToString(extType, extBuf);
    if (named != nullptr)
        sprintf_s(nameBuf, sizeof(nameBuf), "%s", named);
    else
        sprintf_s(nameBuf, sizeof(nameBuf), "ME#%u", value);

    // What the event carries, through the two channels MF actually uses - reporting both is what
    // makes the table readable:
    //   GetValue(): MESessionTopologySet carries the new topology (VT_UNKNOWN); other events carry a
    //     number or nothing.  The topology *status* is not in the value (measured: it comes back as
    //     VT_UNKNOWN there too).
    //   GetExtendedInformation(MF_EVENT_TOPOLOGY_STATUS): the MF_TOPOSTATUS number of a
    //     MESessionTopologyStatus event, through the slot declared in IMFMediaEventExtended above.
    std::string extra;

    if (named != nullptr && strcmp(named, "MESessionTopologyStatus") == 0)
    {
        LONG v = 0;
        HRESULT hrExt = E_FAIL;
        IMFAttributes* pEvtAttrs = nullptr;
        if (SUCCEEDED(pEvent->QueryInterface(__uuidof(IMFAttributes),
                                            (void**)&pEvtAttrs)) && pEvtAttrs != nullptr)
        {
            UINT32 st = 0;
            // IMFAttributes::GetUINT32 rather than MFGetAttributeUINT32: this SDK's helper takes the
            // default by value and returns the attribute, so it cannot report "not present".
            hrExt = pEvtAttrs->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &st);
            if (SUCCEEDED(hrExt))
                v = (LONG)st;
            pEvtAttrs->Release();
        }
        else
        {
            hrExt = E_NOINTERFACE;
        }
        if (SUCCEEDED(hrExt))
        {
            char b[64];
            sprintf_s(b, sizeof(b), " topology=%s(%ld)", TopoStatusName(v), v);
            extra = b;
            if (v == MF_TOPOSTATUS_READY)
                run.bReady = true;
            else if (v == MF_TOPOSTATUS_ENDED)
                run.bTopoEnded = true;
        }
        else
        {
            // no name claimed, no number read: say so instead of printing a bare event line
            char b[64];
            sprintf_s(b, sizeof(b), " topo-status-attribute-get=0x%08X", (unsigned)hrExt);
            extra = b;
        }
    }

    if (extra.empty())
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        HRESULT hrValue = pEvent->GetValue(&var);
        if (FAILED(hrValue))
        {
            char b[48];
            sprintf_s(b, sizeof(b), " value-get-failed=0x%08X", (unsigned)hrValue);
            extra = b;
        }
        else if (var.vt == VT_UI4 || var.vt == VT_I4)
        {
            char b[32];
            sprintf_s(b, sizeof(b), " value=%ld", (LONG)var.ulVal);
            extra = b;
        }
        else if (var.vt == VT_I8)
        {
            char b[48];
            sprintf_s(b, sizeof(b), " value=%lld", (long long)var.hVal.QuadPart);
            extra = b;
        }
        else if (var.vt == VT_UNKNOWN || var.vt == VT_DISPATCH)
        {
            // the old build of this tool printed "<interface>" here; keep the shape of the line
            extra = " value=<interface>";
        }
        else
        {
            char b[48];
            sprintf_s(b, sizeof(b), " value-type=0x%hX", (unsigned short)var.vt);
            extra = b;
        }
        PropVariantClear(&var);
    }

    printf("  event %-34s status=0x%08X", nameBuf, (unsigned)hrStatus);
    if (run.bTraceTime)
        printf(" @+%llu ms", (unsigned long long)run.NowMs());
    printf("%s", extra.c_str());
    if (named == nullptr)
        printf(" extended={%s}", extBuf);
    printf("\n");
    fflush(stdout);

    run.nEvents++;
    run.timeline.push_back(std::make_pair(run.NowMs(), std::string(nameBuf) + extra));

    if (named && strcmp(named, "MESessionEnded") == 0)
    {
        run.bEnded = true;
        run.bEndedByNumber = true;
    }
    if (FAILED(hrStatus) && SUCCEEDED(run.hrFailure))
        run.hrFailure = hrStatus;
}

//----------------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
    InstallCrashReport();

    // the tools take UTF-8 command lines from bash and wide ones from cmd; convert through
    // CommandLineToArgvW so paths with non-ASCII characters work either way
    int cw = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &cw);
    const wchar_t* wszFile = nullptr;
    const wchar_t* wszDll = nullptr;
    int seconds = 5;
    LONGLONG llStartPos = 0;
    bool bEvr = true;
    bool bGrabber = false;
    const char* szSinkKind = "EVR";
    RunState run;

    for (int i = 1; i < cw; i++)
    {
        if (!wcscmp(wargv[i], L"-seconds") && i + 1 < cw)
            seconds = _wtoi(wargv[++i]);
        else if (!wcscmp(wargv[i], L"-start") && i + 1 < cw)
            llStartPos = _wtoi64(wargv[++i]);
        else if (!wcscmp(wargv[i], L"-noevr"))
            bEvr = false;
        else if (!wcscmp(wargv[i], L"-grabber"))
        {
            bGrabber = true;
            szSinkKind = "grabber";
        }
        else if (!wcscmp(wargv[i], L"-dec") && i + 1 < cw)
            wszDll = wargv[++i];
        else if (!wcscmp(wargv[i], L"-dump") && i + 1 < cw)
            g_wDumpPath = wargv[++i];
        else if (!wcscmp(wargv[i], L"-ts"))
            run.bTraceTime = true;
        else if (!wszFile)
            wszFile = wargv[i];
    }
    // wargv stays alive for the whole run: wszFile and wszDll point into it
    if (wszFile == nullptr)
    {
        printf("usage: mfsession-test <file> [-seconds N] [-start HNS] [-noevr] [-grabber] "
               "[-dec <decoder dll>] [-ts]\n");
        return 1;
    }

    WCHAR wszUrl[4096];
    {
        WCHAR wszFull[MAX_PATH * 2];
        if (GetFullPathNameW(wszFile, (DWORD)(MAX_PATH * 2), wszFull, nullptr) == 0)
        {
            printf("bad path\n");
            return 1;
        }
        swprintf_s(wszUrl, L"file://%s", wszFull);
    }
    printf("url:     %S\n", wszUrl);
    fflush(stdout);

    // apartment model: measured, not assumed.  With COINIT_APARTMENTTHREADED this process never
    // pumps messages while it waits, and even a plain H.264 file with the system decoder and the
    // EVR failed to resolve its topology (MESessionTopologySet status 0x80004002).  A hand-built
    // session receives COM callbacks from Media Foundation's own work queue threads, so MTA is the
    // model this harness should run under.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        printf("CoInitializeEx -> 0x%08X\n", (unsigned)hr);
        return 1;
    }
    // MFStartup(MF_VERSION) already asks for the full startup: this SDK declares
    // MFStartup(ULONG Version, DWORD dwFlags = MFSTARTUP_FULL), MFSTARTUP_LITE is the second
    // argument, and MFSTARTUP_FULL itself is 0.  Passing MFSTARTUP_FULL as the version, as an
    // earlier rewrite of this file did, changes nothing; checked so the next reader does not try it.
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        printf("MFStartup -> 0x%08X\n", (unsigned)hr);
        CoUninitialize();
        return 127;
    }

    IMFSourceResolver* pResolver = nullptr;
    IMFMediaSource* pSource = nullptr;
    IMFPresentationDescriptor* pPD = nullptr;
    IMFTopology* pTopology = nullptr;
    IMFMediaSession* pSession = nullptr;
    IMFClock* pClock = nullptr;
    IMFPresentationClock* pPresClock = nullptr;
    IMFActivate* pAudioActivate = nullptr;
    IMFActivate* pVideoActivate = nullptr;
    std::vector<IUnknown*> g_renderersOwned;    // renderer objects, released at the end
    HMODULE hDll = nullptr;
    IMFTransform* pMFT = nullptr;
    IMFTransform* pAMFT = nullptr;
    bool bSkipped = false;
    // set when the session cannot create its render sink at all (see the output-node block below)
    bool bSinkUnavailable = false;
    int nExit = 1;

    // the decoder classes of this DLL; the values are the CLSIDs the DLL itself exports
    // (AVSVideoDecoder/classGuids.h), spelled out here so the tool does not need the DLL headers
    static const CLSID CLSID_AVSVideoDecoder_local =
        { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x11 } };
    static const CLSID CLSID_AVSAudioDecoder_local =
        { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x12 } };

    typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, LPVOID*);

    if (wszDll != nullptr)
    {
        // LOAD_WITH_ALTERED_SEARCH_PATH needs an absolute path; the scripts pass a relative one
        static WCHAR wszDllFull[MAX_PATH * 2];
        if (GetFullPathNameW(wszDll, (DWORD)(MAX_PATH * 2), wszDllFull, nullptr) != 0)
            wszDll = wszDllFull;
        hDll = LoadLibraryExW(wszDll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hDll == nullptr)
        {
            printf("LoadLibrary %S -> last error %lu\n", wszDll, GetLastError());
            goto done;
        }
        GetClassObjectFn fn = (GetClassObjectFn)GetProcAddress(hDll, "DllGetClassObject");
        if (fn == nullptr)
        {
            printf("%S has no DllGetClassObject\n", wszDll);
            goto done;
        }
        IClassFactory* pCF = nullptr;
        hr = fn(CLSID_AVSVideoDecoder_local, IID_IClassFactory, (void**)&pCF);
        if (FAILED(hr) || pCF == nullptr)
        {
            printf("DllGetClassObject(video decoder) -> 0x%08X\n", (unsigned)hr);
            goto done;
        }
        hr = pCF->CreateInstance(nullptr, IID_IMFTransform, (void**)&pMFT);
        pCF->Release();
        if (FAILED(hr) || pMFT == nullptr)
        {
            printf("CreateInstance(IMFTransform) -> 0x%08X\n", (unsigned)hr);
            goto done;
        }
        pCF = nullptr;
        if (SUCCEEDED(fn(CLSID_AVSAudioDecoder_local, IID_IClassFactory, (void**)&pCF)) && pCF)
        {
            pCF->CreateInstance(nullptr, IID_IMFTransform, (void**)&pAMFT);
            pCF->Release();
        }
        printf("decoder MFT from %S: video=%s audio=%s\n", wszDll,
               pMFT ? "created" : "no", pAMFT ? "created" : "no");
        fflush(stdout);
    }

    hr = MFCreateSourceResolver(&pResolver);
    if (FAILED(hr))
    {
        printf("MFCreateSourceResolver -> 0x%08X\n", (unsigned)hr);
        goto done;
    }
    {
        // How the source is resolved is a variable of its own, so it is named rather than baked
        // in.  "loose" lets Media Foundation offer the file to other registered handlers as well;
        // "strict" resolves by extension only, which is what a player does for a registered
        // extension.  The two modes are not equivalent: with the loose flag the H.264 control
        // through our own MFT ended the session, and the same cell without it did not, so any
        // statement about "AVS does not end" has to name the mode.  AVS_TEST_RESOLVE_STRICT=1
        // selects strict; scripts/ab-resolve.sh re-measures both.
        const BOOL bStrict = GetEnvironmentVariableW(L"AVS_TEST_RESOLVE_STRICT", nullptr, 0) > 0;
        const DWORD resolveFlags = bStrict
            ? MF_RESOLUTION_MEDIASOURCE
            : (MF_RESOLUTION_MEDIASOURCE |
               MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE);
        printf("source resolution mode: %s\n",
               bStrict ? "strict (by extension)" : "loose (content need not match the extension)");
        fflush(stdout);

        MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
        IUnknown* pUnk = nullptr;
        hr = pResolver->CreateObjectFromURL(wszUrl, resolveFlags, nullptr, &objType, &pUnk);
        printf("CreateObjectFromURL -> 0x%08X\n", (unsigned)hr);
        if (FAILED(hr) || pUnk == nullptr)
        {
            SAFE_RELEASE(pUnk);
            nExit = 127;
            goto done;
        }
        hr = pUnk->QueryInterface(__uuidof(IMFMediaSource), (void**)&pSource);
        SAFE_RELEASE(pUnk);
        if (FAILED(hr))
        {
            printf("no IMFMediaSource -> 0x%08X\n", (unsigned)hr);
            nExit = 127;
            goto done;
        }
    }

    hr = pSource->CreatePresentationDescriptor(&pPD);
    printf("CreatePresentationDescriptor -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr))
    {
        nExit = 127;
        goto done;
    }
    {
        UINT64 ullDur = 0;
        if (SUCCEEDED(pPD->GetUINT64(MF_PD_DURATION, &ullDur)))
        {
            run.dDuration = (double)ullDur / 10000000.0;
            printf("presentation descriptor duration: %llu (%.3f s)\n",
                   (unsigned long long)ullDur, (double)ullDur / 10000000.0);
        }
        else
            printf("presentation descriptor duration: not set\n");
        // This SDK has no MF_SD_DURATION, so no GUID is invented for it: where a stream ends is
        // measured from the decoder trace instead (AVS_MFT_TRACE "ProcessInput ... time=" and
        // "handed to graph ... time=").
    }

    if (FAILED(MFCreateAudioRendererActivate(&pAudioActivate)) || pAudioActivate == nullptr)
        pAudioActivate = nullptr;
    if (pAudioActivate == nullptr)
        printf("no audio playback device in this session (MF_E_NO_AUDIO_PLAYBACK_DEVICE): "
               "audio streams are left out of the topology\n");
    if (bEvr)
    {
        if (!EnsureWindow())
        {
            printf("CreateWindow failed (%lu)\n", GetLastError());
            goto done;
        }
        if (!bGrabber)
        {
            // Measure, do not assume.  This shell answers E_ACCESSDENIED (0x80070005) for the EVR's
            // ActivateObject while an interactive desktop session does not; that is a property of the
            // shell, not of the pipeline under test.  So the availability of a compositor is probed
            // here, and when it is missing the run continues against the sample grabber instead of
            // reporting nothing.
            IMFActivate* pProbe = nullptr;
            IUnknown* pProbeObj = nullptr;
            HRESULT hrProbe = E_FAIL;
            if (SUCCEEDED(MFCreateVideoRendererActivate(g_hwnd, &pProbe)) && pProbe != nullptr)
                hrProbe = pProbe->ActivateObject(__uuidof(IUnknown), (void**)&pProbeObj);
            printf("EVR availability probe: ActivateObject -> 0x%08X\n", (unsigned)hrProbe);
            SAFE_RELEASE(pProbeObj);
            SAFE_RELEASE(pProbe);
            if (FAILED(hrProbe))
            {
                // Measured, not assumed: the grabber is a real IMFStreamSink and it does measure
                // delivery, but it is not equivalent to a compositor.  Under it the H.264 control
                // that reaches MESessionEnded with the EVR delivers zero samples and fails with
                // MF_E_UNSUPPORTED_FORMAT (0xC00D3E98).  The pinned cells (regression 11d/11i) are
                // about end semantics, so they must keep the sink they were pinned with: the
                // fallback is opt-in with -grabber, and a default run still says "no sink here".
                printf("no compositor sink in this session: this run cannot measure end semantics. "
                       "Add -grabber to measure delivery against a sample grabber instead.\n");
            }
        }
        if (!bGrabber &&
            (FAILED(MFCreateVideoRendererActivate(g_hwnd, &pVideoActivate)) ||
             pVideoActivate == nullptr))
        {
            pVideoActivate = nullptr;
            printf("no EVR renderer activate object in this session: video streams are left out\n");
        }
    }

    {
        DWORD cStreams = 0;
        pPD->GetStreamDescriptorCount(&cStreams);
        printf("streams in the presentation descriptor: %u\n", (unsigned)cStreams);
        fflush(stdout);

        hr = MFCreateTopology(&pTopology);
        if (FAILED(hr))
        {
            printf("MFCreateTopology -> 0x%08X\n", (unsigned)hr);
            goto done;
        }

        for (DWORD i = 0; i < cStreams; i++)
        {
            BOOL bSelected = FALSE;
            IMFStreamDescriptor* pSD = nullptr;
            if (FAILED(pPD->GetStreamDescriptorByIndex(i, &bSelected, &pSD)) || pSD == nullptr)
                continue;

            GUID major = GUID_NULL, sub = GUID_NULL;
            IMFMediaType* pCur = nullptr;
            IMFMediaTypeHandler* pHandler = nullptr;
            if (SUCCEEDED(pSD->GetMediaTypeHandler(&pHandler)) && pHandler != nullptr)
            {
                pHandler->GetMediaTypeByIndex(0, &pCur);   // the type the source publishes here
                if (pCur != nullptr)
                {
                    pCur->GetGUID(MF_MT_MAJOR_TYPE, &major);
                    pCur->GetGUID(MF_MT_SUBTYPE, &sub);
                }
                SAFE_RELEASE(pHandler);
            }
            DumpStreamTypes(pSD, i, bSelected != FALSE);

            char subBuf[39];
            GuidToString(sub, subBuf);
            const char* pMajor = FourccName(major);
            char majorBuf[39];
            if (pMajor == nullptr)
            {
                GuidToString(major, majorBuf);
                pMajor = majorBuf;
            }

            const bool bVideo = (major == MFMediaType_Video);
            const bool bAudio = (major == MFMediaType_Audio);
            printf("stream %u: selected=%d major: '%s'\n",
                   (unsigned)i, (bSelected != FALSE) ? 1 : 0, pMajor);
            printf("         subtype: {%s}\n", subBuf);
            fflush(stdout);

            if (bSelected == FALSE)
            {
                printf("  stream %u is deselected in the presentation descriptor, left out\n",
                       (unsigned)i);
                SAFE_RELEASE(pSD);
                continue;
            }
            if (!bVideo && !bAudio)
            {
                printf("  stream %u is neither video nor audio, left out\n", (unsigned)i);
                SAFE_RELEASE(pSD);
                continue;
            }

            {
                // A source node needs its stream descriptor attached, or the topology resolver
                // cannot tell which of the source's streams the node means (measured: without it
                // the node is created and the topology then fails to resolve).
                IMFTopologyNode* pSrcNode = nullptr;
                IMFTopologyNode* pDecNode = nullptr;
                IMFTopologyNode* pOutNode = nullptr;
                IMFActivate*     pAct = nullptr;
                IMFMediaSink*    pSink = nullptr;
                IMFStreamSink*   pStreamSink = nullptr;
                IUnknown*        pNodeObj = nullptr;
                const char*      szKind = bVideo ? (bGrabber ? "grabber" : "EVR") : "audio";
                bool             bOwnActivate = false;

                if (SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSrcNode)) &&
                    pSrcNode != nullptr)
                {
                    pSrcNode->SetUnknown(MF_TOPONODE_SOURCE, pSource);
                    // Both descriptors, not just the stream one: a source-stream node that carries
                    // only MF_TOPONODE_STREAM_DESCRIPTOR makes SetTopology answer
                    // MF_E_TOPO_MISSING_PRESENTATION_DESCRIPTOR (0xC00D5217).  Measured.
                    pSrcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
                    pSrcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);
                    hr = pTopology->AddNode(pSrcNode);
                    printf("  source node for stream %u added\n", (unsigned)i);
                }

                // The decoder is put in by hand.  Letting the topology loader insert it was
                // measured to answer MF_E_TOPO_CODEC_NOT_FOUND for our own subtypes, because the
                // loader only finds a codec through a registered class; -dec names the DLL here.
                IMFTransform* pDec = bVideo ? pMFT : pAMFT;
                if (pDec != nullptr && SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_TRANSFORM_NODE,
                                                                     &pDecNode)) &&
                    pDecNode != nullptr)
                {
                    hr = pDecNode->SetObject(pDec);
                    printf("  explicit decoder node for stream %u (SetObject) -> 0x%08X\n",
                           (unsigned)i, (unsigned)hr);
                    pTopology->AddNode(pDecNode);
                    hr = pSrcNode->ConnectOutput(0, pDecNode, 0);
                    printf("  source -> decoder connected -> 0x%08X\n", (unsigned)hr);
                }
                else
                {
                    printf("  no decoder object for stream %u: connecting the source directly\n",
                           (unsigned)i);
                }

                if (bVideo)
                {
                    if (bGrabber)
                    {
                        pAct = CreateGrabberSink(pSD);
                        bOwnActivate = true;
                    }
                    else
                        pAct = pVideoActivate;
                    if (pAct == nullptr)
                    {
                        // no compositor in this shell: the run must say so and be a skip, not a
                        // failure (see regression 11l L4)
                        bSinkUnavailable = true;
                        printf("  no MF render sink in this session: video stream %u left out\n",
                               (unsigned)i);
                    }
                }
                else
                    pAct = pAudioActivate;

                if (pAct != nullptr &&
                    SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutNode)) &&
                    pOutNode != nullptr)
                {
                    // The node object has to be the sink's IMFStreamSink, not the object the
                    // activate handed back: that one answers IMFMediaSink but E_NOINTERFACE for
                    // IMFMediaStream, and the topology then fails with E_NOINTERFACE (0x80004002).
                    HRESULT hrAct = pAct->ActivateObject(__uuidof(IMFMediaSink), (void**)&pSink);
                    printf("  %s renderer ActivateObject -> 0x%08X\n", szKind, (unsigned)hrAct);
                    if (SUCCEEDED(hrAct) && pSink != nullptr)
                    {
                        DWORD nSS = 0;
                        pSink->GetStreamSinkCount(&nSS);
                        HRESULT hrSS = pSink->GetStreamSinkByIndex(0, &pStreamSink);
                        printf("  %s sink has %u stream sink(s), GetStreamSinkByIndex(0) -> 0x%08X\n",
                               szKind, (unsigned)nSS, (unsigned)hrSS);
                        if (SUCCEEDED(hrSS) && pStreamSink != nullptr)
                        {
                            pNodeObj = pStreamSink;
                            printf("  output node object: the sink's IMFStreamSink\n");
                        }
                    }
                    if (pNodeObj == nullptr)
                        pNodeObj = pAct;      // kept to show the error, not as a working choice

                    if (pNodeObj == pAct)
                    {
                        // The sink was never activated, so there is no IMFStreamSink to put on the
                        // node.  Adding the node anyway leaves an output node whose object is an
                        // IMFActivate, and the whole topology is then refused: one stream without a
                        // usable sink must not take the working streams down with it.
                        printf("  %s output node for stream %u not added: the sink was never "
                               "activated\n", szKind, (unsigned)i);
                        // A video stream with no sink at all means this shell cannot measure end
                        // semantics; that is a skip, not a product failure (regression 11c/11d and
                        // the L4 contract of grabber-delivery-test depend on it being said out
                        // loud).  Audio failing - no audio device - must not skip a run that has a
                        // working video sink.
                        if (bVideo)
                            bSinkUnavailable = true;
                        SAFE_RELEASE(pOutNode);
                        pOutNode = nullptr;
                    }
                    else
                    {
                        hr = pOutNode->SetObject(pNodeObj);
                        pTopology->AddNode(pOutNode);
                        IMFTopologyNode* pFrom = (pDecNode != nullptr) ? pDecNode : pSrcNode;
                        hr = pFrom->ConnectOutput(0, pOutNode, 0);
                        if (SUCCEEDED(hr))
                            printf("  %s output node for stream %u connected (via %s node -> "
                                   "0x%08X)\n",
                                   szKind, (unsigned)i,
                                   (pDecNode != nullptr) ? "decoder" : "source", (unsigned)hr);
                        else
                            printf("  %s output node for stream %u -> 0x%08X\n",
                                   szKind, (unsigned)i, (unsigned)hr);
                        // the sink must outlive the topology: the graph presents through it.  This
                        // reference is taken on top of the local one, which is released below.
                        if (pStreamSink != nullptr)
                        {
                            pStreamSink->AddRef();
                            g_renderersOwned.push_back(pStreamSink);
                        }
                    }
                }

                SAFE_RELEASE(pStreamSink);
                SAFE_RELEASE(pSink);
                if (bOwnActivate)
                    SAFE_RELEASE(pAct);
                SAFE_RELEASE(pOutNode);
                SAFE_RELEASE(pDecNode);
                SAFE_RELEASE(pSrcNode);
            }
            SAFE_RELEASE(pSD);
        }
    }

    if (pTopology == nullptr)
    {
        printf("no topology was built\n");
        goto done;
    }

    {
        // The session is created with the default attributes: the loader settings that this SDK
        // declares (MF_SESSION_TOPOLOADER) are probed separately below instead of being assumed.
        hr = MFCreateMediaSession(nullptr, &pSession);
        printf("MFCreateMediaSession -> 0x%08X\n", (unsigned)hr);
        if (FAILED(hr) || pSession == nullptr)
        {
            nExit = 127;
            goto done;
        }
        hr = pSession->SetTopology(0 /* MFSESSION_SETTOPOLOGY_FLAGS: the initial topology */,
                                   pTopology);
        printf("SetTopology -> 0x%08X\n", (unsigned)hr);
        if (FAILED(hr))
        {
            // Not a reason to stop measuring: the reason a topology does not resolve is reported by
            // the session as MESessionTopologySet / a node error attribute, and those are exactly
            // what this tool is for.  A topology that the session refuses is still dumped below.
            if (SUCCEEDED(run.hrFailure))
                run.hrFailure = hr;
        }
        fflush(stdout);
        DumpTopologyResolution(pTopology, "after-settopology");
        ProbeTopologyLoader(pSession, pTopology);
        DumpTopologyTypes(pTopology, "mft-in");
    }

    if (pSession == nullptr)
    {
        nExit = 127;
        goto done;
    }

    // start, then pump events and poll the clock in one loop.  Both are measured here: the event
    // list is what the session reports, the clock position is what the graph really reached.  With
    // no sink at all nothing advances, which is a property of the shell.
    {
        printf("event polling: GetEvent(MF_EVENT_FLAG_NO_WAIT)\n");
        fflush(stdout);

        // The clock is the session's own: this SDK declares no MF_SERVICE_* GUID and the
        // presentation clock it declares (IMFPresentationClock::SetTimeSource) is not the one the
        // running session uses, so the clock is taken through IMFMediaSession::GetClock and polled
        // through IMFClock.  Nothing here invents a clock of its own.
        hr = pSession->GetClock(&pClock);
        if (FAILED(hr) || pClock == nullptr)
        {
            printf("GetClock -> 0x%08X: the clock position cannot be polled\n", (unsigned)hr);
        }
        else
        {
            // IMFClock (this SDK) has no GetTime: the position comes from the presentation clock,
            // the state and the characteristics from IMFClock.  Both are the session's own clock.
            pClock->QueryInterface(__uuidof(IMFPresentationClock), (void**)&pPresClock);
        }

        // Start takes a time format and a position (this SDK's IMFMediaSession::Start); GUID_NULL
        // asks for the session's default, and the position is in 100ns, which is what the printed
        // line says.
        {
            PROPVARIANT varStart;
            PropVariantInit(&varStart);
            varStart.vt = VT_I8;
            varStart.hVal.QuadPart = (LONGLONG)llStartPos;
            GUID guidTimeFormat = GUID_NULL;
            hr = pSession->Start(&guidTimeFormat, &varStart);
            printf("Start(%lld 100ns) -> 0x%08X\n", (long long)llStartPos, (unsigned)hr);
            fflush(stdout);
            PropVariantClear(&varStart);
            if (SUCCEEDED(hr))
                run.bStarted = true;
        }
        QueryPerformanceCounter(&run.liStart);

        LONGLONG llLastPos = 0;
        ULONGLONG llDeadline = GetTickCount64() + (ULONGLONG)seconds * 1000ULL;
        while (GetTickCount64() < llDeadline)
        {
            for (;;)
            {
                IMFMediaEvent* pEvent = nullptr;
                HRESULT hrEv = pSession->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent);
                if (FAILED(hrEv))
                {
                    SAFE_RELEASE(pEvent);
                    break;
                }
                HandleEvent(pEvent, run);
                SAFE_RELEASE(pEvent);
            }

            MFTIME rt = 0;
            HRESULT hrTime = (pPresClock != nullptr) ? pPresClock->GetTime(&rt) : E_NOINTERFACE;
            MFCLOCK_STATE st = MFCLOCK_STATE_INVALID;
            HRESULT hrState = (pClock != nullptr) ? pClock->GetState(0, &st) : E_NOINTERFACE;
            DWORD dwChars = 0;
            if (pClock != nullptr)
                pClock->GetClockCharacteristics(&dwChars);
            if (SUCCEEDED(hrTime))
            {
                double dPos = (double)rt / 10000000.0;
                if (dPos > run.dClockMax)
                    run.dClockMax = dPos;
            }
            double dDelta = (double)(rt - llLastPos) / 10000000.0;
            llLastPos = rt;
            printf("  clock: gettime=0x%08X pos=%.3f s delta=%.3f s state=%s chars=0x%X\n",
                   (unsigned)hrTime, (double)rt / 10000000.0, dDelta,
                   (SUCCEEDED(hrState) ? ClockStateName(st) : "getstate-failed"),
                   (unsigned)dwChars);
            fflush(stdout);
            if (SUCCEEDED(hrState) && st == MFCLOCK_STATE_STOPPED && !run.bFirstStopped &&
                run.llHostStopMs == 0)
            {
                run.bFirstStopped = true;
                run.llFirstStoppedMs = run.NowMs();
                printf("  clock: entered state=stopped at @+%llu ms without the host calling "
                       "Stop() - the graph stopped the presentation clock by itself\n",
                       (unsigned long long)run.llFirstStoppedMs);
                fflush(stdout);
            }
            Sleep(20);
        }

        run.llHostStopMs = run.NowMs();
        printf("[host] this process calls IMFMediaSession::Stop() at @+%llu ms\n",
               (unsigned long long)run.llHostStopMs);
        pSession->Stop();
        pSession->Shutdown();
    }

    // Where the graph said it was over, as a line the scripts can read.  The two end-of-presentation
    // markers are taken from the event timeline this process recorded; -1 means the event never came.
    {
        long long llEop = -1, llTopoEnded = -1, llGraphStopped = -1;
        for (size_t k = 0; k < run.timeline.size(); k++)
        {
            const std::string& sEvent = run.timeline[k].second;
            if (llEop < 0 && sEvent.find("MEEndOfPresentation") != std::string::npos)
                llEop = (long long)run.timeline[k].first;
            if (llTopoEnded < 0 && sEvent.find("topology=ended") != std::string::npos)
                llTopoEnded = (long long)run.timeline[k].first;
        }
        if (run.bFirstStopped)
            llGraphStopped = (long long)run.llFirstStoppedMs;
        printf("eos-signals: end-of-presentation=%lld ms topology-ended=%lld ms "
               "clock-stopped-by-graph=%lld ms host-stop=%llu ms clock-max=%.3f s "
               "duration=%s session-ended=%s\n",
               llEop, llTopoEnded, llGraphStopped, (unsigned long long)run.llHostStopMs,
               run.dClockMax, (run.dDuration >= 0.0) ? "stated" : "not stated",
               run.bEnded ? "yes" : "no");
    }

    printf("session events seen: %d\n", run.nEvents);
    printf("started: %s   ended: %s   failure: 0x%08X\n",
           run.bStarted ? "yes" : "no", run.bEnded ? "yes" : "no", (unsigned)run.hrFailure);
    printf("clock reached: %.3f s\n", run.dClockMax);
    printf("event timeline (ms from Start):\n");
    for (size_t k = 0; k < run.timeline.size(); k++)
        printf("  @+%llu ms  %s\n", (unsigned long long)run.timeline[k].first,
               run.timeline[k].second.c_str());
    if (run.dClockMax > 0.1)
    {
        printf("media session runs (clock advances)\n");
        nExit = 0;
    }
    else
        printf("media session does NOT advance\n");

    if (!g_grabs.empty())
    {
        printf("sink delivery (one callback line per sample handed to the grabber):\n");
        for (size_t k = 0; k < g_grabs.size(); k++)
        {
            GrabCallback* pCb = g_grabs[k];
            double dSpan = (double)(pCb->lastTime - pCb->firstTime) / 10000000.0;
            /* Round 27 Check: the pts below are Media Foundation 100 ns ticks, while every
             * other number on this line (span, clock-max, eop/topo/host-stop) is seconds or
             * milliseconds.  Comparing raw ticks against clock-max produced a
             * "the clock never reached the end" conclusion that did not exist:
             * 30400000 is 3.04 s, not 30.4 s.  Print the seconds in the same line. */
            printf("  grabber %d (%ldx%ld): samples delivered = %ld, first pts = %lld (%.3f s), "
                   "last pts = %lld (%.3f s), span = %.3f s, bytes = %llu\n",
                   (int)k, pCb->width, pCb->height, pCb->samples, pCb->firstTime,
                   (double)pCb->firstTime / 10000000.0, pCb->lastTime,
                   (double)pCb->lastTime / 10000000.0,
                   dSpan, (unsigned long long)pCb->bytes);
            printf("           clock: start=%ld (offset=%lld) stop=%ld pause=%ld restart=%ld "
                   "setrate=%ld\n",
                   pCb->clockStarts, pCb->clockStartOffset, pCb->clockStops, pCb->clockPauses,
                   pCb->clockRestarts, pCb->clockRates);
            if (pCb->fDump != nullptr)
            {
                fclose(pCb->fDump);
                pCb->fDump = nullptr;
            }
        }
    }
    else if (bSinkUnavailable)
    {
        // A shell without a compositor cannot measure end semantics; that is reported as a skip so
        // the regression suite does not read it as a product failure.
        printf("no MF render sink in this session: the graph has no sink to present to, so end "
               "semantics cannot be measured here.\n");
        printf("SKIPPED.\n");
        nExit = 0;
    }

done:
    if (!g_attrLines.empty())
    {
        for (size_t k = 0; k < g_attrLines.size(); k++)
            printf("%s\n", g_attrLines[k].c_str());
    }
    // Order matters: the session holds a reference to the topology, and the topology holds a
    // reference to the MFT whose code lives in the DLL we loaded.  Freeing the library before those
    // are gone unloads code that the graph still has pointers into (measured: access violation
    // inside MFCORE.DLL on the way out).
    SAFE_RELEASE(pPresClock);
    SAFE_RELEASE(pClock);
    SAFE_RELEASE(pSession);
    SAFE_RELEASE(pTopology);
    SAFE_RELEASE(pPD);
    SAFE_RELEASE(pSource);
    SAFE_RELEASE(pResolver);
    for (size_t k = 0; k < g_renderersOwned.size(); k++)
        SAFE_RELEASE(g_renderersOwned[k]);
    g_renderersOwned.clear();
    for (size_t k = 0; k < g_grabs.size(); k++)
        SAFE_RELEASE(g_grabs[k]);
    g_grabs.clear();
    SAFE_RELEASE(pMFT);
    SAFE_RELEASE(pAMFT);
    if (hDll != nullptr)
        FreeLibrary(hDll);
    SAFE_RELEASE(pAudioActivate);
    SAFE_RELEASE(pVideoActivate);
    if (g_hwnd != nullptr)
        DestroyWindow(g_hwnd);
    MFShutdown();
    CoUninitialize();
    return nExit;
}
