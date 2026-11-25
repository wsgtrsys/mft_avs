//----------------------------------------------------------------------------------------------
// AVSMediaSource.cpp - libavformat based IMFMediaSource for AVS video (+ DRA audio).
//
// Windows cannot open our files by itself: the MPEG-2 TS byte stream handler does not
// know stream_type 0x42 (AVS video) nor the 'DRA1' registration descriptor, and a raw
// .avs elementary stream has no handler at all.  This source demuxes with libavformat -
// the same library the decoder MFTs use - and publishes the elementary streams with the
// MFVideoFormat_AVS / MFAudioFormat_DRA sub-types that AVSVideoDecoderMFT and
// AVSAudioDecoderMFT accept, so that the normal MF pipeline (SourceReader or Media
// Session) reaches them.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "classGuids.h"
#include "AvsDemuxHint.h"
#include "AvsChannelMask.h"
#include "AvsColor.h"
#include "AVSMediaSource.h"

#include <new>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>

#define AVS_IO_BUFFER_SIZE      (128 * 1024)
#define AVS_MAX_BUFFERED_EVENTS 256
#define AVS_MAX_PENDING_SAMPLES 4096    // memory guard for the per stream backlog

// trace only (AVS_MFT_TRACE): how often a consumer asked each stream for a sample
static LONG g_avsRequestCount[8] = { 0 };
static LONG g_avsSampleCount[8] = { 0 };      // trace only: samples handed to the graph
#define AVS_PREROLL_PACKETS     256
#define HNS_PER_SECOND          10000000LL
#define AVS_NOPTS               ((int64_t)0x8000000000000000LL)

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
// Registration and byte stream handler problems are invisible from the outside: MF
// turns a failing handler into MF_E_UNSUPPORTED_BYTESTREAM_TYPE.  Set
// AVS_MFT_TRACE=1 in the environment of the player to see what is called.
static bool AVS_TraceOn(void)
{
    static int on = -1;
    if (on < 0)
    {
        WCHAR v[4] = { 0 };
        on = (GetEnvironmentVariableW(L"AVS_MFT_TRACE", v, 3) > 0) ? 1 : 0;
    }
    return on != 0;
}

static void AVS_Trace(const char* what, HRESULT hr)
{
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] %s -> 0x%08X\n", what, (unsigned)hr);
}

static std::string Narrow(const WCHAR* pw)
{
    if (pw == NULL)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, pw, -1, NULL, 0, NULL, NULL);
    std::string s(n > 1 ? n - 1 : 0, 0);
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, pw, -1, &s[0], n, NULL, NULL);
    return s;
}

static HRESULT FromHnsProp(const PROPVARIANT* pVar, LONGLONG* phns)
{
    if (pVar == NULL || phns == NULL)
        return E_POINTER;
    if (pVar->vt == VT_I8)
    {
        *phns = pVar->hVal.QuadPart;
        return S_OK;
    }
    if (pVar->vt == VT_UI4)
    {
        *phns = (LONGLONG)pVar->ulVal * HNS_PER_SECOND;
        return S_OK;
    }
    if (pVar->vt == VT_EMPTY || pVar->vt == VT_ERROR)
    {
        *phns = 0;
        return S_OK;
    }
    return MF_E_INVALID_TIMESTAMP;
}

// FFmpeg demuxer name for a file extension, used because a raw AVS elementary stream
// cannot be recognized by content probing (its picture start code is the MPEG-1/2 one).
// The lower-case extension of a URL, empty when the name has none.
static std::string ExtensionFromUrl(LPCWSTR pwszURL)
{
    if (pwszURL == NULL)
        return std::string();
    std::wstring url(pwszURL);
    size_t dot = url.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return std::string();
    std::string ext = Narrow(url.c_str() + dot);
    for (size_t i = 0; i < ext.size(); i++)
        if (ext[i] >= 'A' && ext[i] <= 'Z')
            ext[i] = (char)(ext[i] - 'A' + 'a');
    return ext;
}

static std::string DemuxHintFromExtension(LPCWSTR pwszURL)
{
    return AvsDemuxHintForExt(ExtensionFromUrl(pwszURL));
}

// Streams whose codec we do not know are skipped, so the presentation only ever
// contains streams that some decoder MFT can be expected to handle.
static BOOL StdVideoSubtype(enum AVCodecID id, GUID* pSub)
{
    switch (id)
    {
    case AV_CODEC_ID_H264:      *pSub = MFVideoFormat_H264;  return TRUE;
    case AV_CODEC_ID_HEVC:      *pSub = MFVideoFormat_HEVC;  return TRUE;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO: *pSub = MFVideoFormat_MPEG2; return TRUE;
    default:                    return FALSE;
    }
}

static BOOL StdAudioSubtype(enum AVCodecID id, GUID* pSub)
{
    switch (id)
    {
    case AV_CODEC_ID_AAC:       *pSub = MFAudioFormat_AAC;  return TRUE;
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MP2:       *pSub = MFAudioFormat_MP3;  return TRUE;
    // video/dfws.ts is an AVS+ broadcast stream whose audio is E-AC-3.  Dropping such a stream is
    // worse than describing it: the presentation descriptor then hides the fact that the file has
    // audio at all, and a machine that does have a Dolby decoder is given nothing to select.
    // The AC-3 family stays outside our own decoder by design - it is not an AVS-family codec, so a
    // system or third-party decoder owns it.  bin/mftenum.exe shows this machine registers 18 audio
    // decoders and none of them is a Dolby one, so here the stream is described, not decoded.
    case AV_CODEC_ID_AC3:       *pSub = MFAudioFormat_Dolby_AC3;    return TRUE;
    case AV_CODEC_ID_EAC3:      *pSub = MFAudioFormat_Dolby_DDPlus; return TRUE;
    default:                    return FALSE;
    }
}

// ===========================================================================
// CAVSMediaSource
// ===========================================================================
CAVSMediaSource::CAVSMediaSource()
    : m_RefCount(1)
    , m_pEventQueue(NULL)
    , m_pByteStream(NULL)
    , m_pAVIO(NULL)
    , m_pAVIOBuffer(NULL)
    , m_pFmt(NULL)
    , m_bOpened(FALSE)
    , m_bStarted(FALSE)
    , m_bShutdown(FALSE)
    , m_nStartedStreams(0)
    , m_nEndedStreams(0)
    , m_bEopPosted(FALSE)
    , m_pAttributes(NULL)
    , m_pPD(NULL)
    , m_nIoTrace(0)
    , m_rtStart(0)
    , m_rtDuration(0)
    , m_llStartHns(0)
    , m_cbSize(0)
    , m_bSeekable(FALSE)
    , m_PrerollPos(0)
{
}

CAVSMediaSource::~CAVSMediaSource()
{
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] ~source this=%p\n", (void*)this);
    CloseFile();
    // one trace point per release: if a host tears the graph down in an order we
    // did not expect, this shows which release faulted (AVS_MFT_TRACE=1)
    if (AVS_TraceOn()) fprintf(stderr, "[avssrc] ~source: put bytestream %p\n", (void*)m_pByteStream);
    SAFE_RELEASE(m_pByteStream);
    if (AVS_TraceOn()) fprintf(stderr, "[avssrc] ~source: put eventqueue %p\n", (void*)m_pEventQueue);
    SAFE_RELEASE(m_pEventQueue);
    if (AVS_TraceOn()) fprintf(stderr, "[avssrc] ~source: put attributes %p\n", (void*)m_pAttributes);
    SAFE_RELEASE(m_pAttributes);
    if (AVS_TraceOn()) fprintf(stderr, "[avssrc] ~source: put pd %p\n", (void*)m_pPD);
    SAFE_RELEASE(m_pPD);
    if (AVS_TraceOn()) fprintf(stderr, "[avssrc] ~source: done\n");
}

HRESULT CAVSMediaSource::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;
    *ppvObject = NULL;
    if (pUnkOuter != NULL)
        return CLASS_E_NOAGGREGATION;

    CAVSMediaSource* pSource = new (std::nothrow) CAVSMediaSource;
    if (pSource == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = pSource->QueryInterface(riid, ppvObject);
    pSource->Release();
    return hr;
}

STDMETHODIMP_(ULONG) CAVSMediaSource::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CAVSMediaSource::Release()
{
    LONG lRef = InterlockedDecrement(&m_RefCount);
    if (lRef == 0)
        delete this;
    return (ULONG)lRef;
}

STDMETHODIMP CAVSMediaSource::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
    {
        *ppvObject = static_cast<IMFMediaSourceEx*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IAVSByteDataSource)
    {
        *ppvObject = static_cast<IAVSByteDataSource*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// IMFMediaEventGenerator
// ---------------------------------------------------------------------------
STDMETHODIMP CAVSMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    AutoLock lock(m_Lock);
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP CAVSMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(m_Lock);
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP CAVSMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(m_Lock);
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP CAVSMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(m_Lock);
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ---------------------------------------------------------------------------
// byte source
// ---------------------------------------------------------------------------
STDMETHODIMP CAVSMediaSource::SetByteStream(IMFByteStream* pByteStream)
{
    if (pByteStream == NULL)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);
    if (m_bOpened)
        return MF_E_INVALIDREQUEST;

    SAFE_RELEASE(m_pByteStream);
    pByteStream->AddRef();
    m_pByteStream = pByteStream;

    QWORD cb = 0;
    if (SUCCEEDED(pByteStream->GetLength(&cb)))
        m_cbSize = cb;

    QWORD q = 0;
    m_bSeekable = SUCCEEDED(pByteStream->Seek(msoBegin, 0, 0, &q)) ? TRUE : FALSE;

    if (m_pAttributes == NULL)
        return MFCreateAttributes(&m_pAttributes, 2);
    return S_OK;
}

// The AVIO callbacks run while Open() is executing, i.e. with m_Lock held, so they must
// not take any lock of their own.  m_pByteStream does not change after SetByteStream.
int AVS_IO_Read(void* pOpaque, uint8_t* pBuffer, int cbSize)
{
    CAVSMediaSource* pSource = (CAVSMediaSource*)pOpaque;
    if (pSource->m_pByteStream == NULL)
        return AVERROR(EIO);

    ULONG cbRead = 0;
    const HRESULT hr = pSource->m_pByteStream->Read(pBuffer, (ULONG)cbSize, &cbRead);
    if (AVS_TraceOn() && (pSource->m_nIoTrace++ % 200) == 199)
        fprintf(stderr, "[avssrc] io read %d byte(s) hr=0x%08X cbRead=%lu\n",
                cbSize, (unsigned)hr, (unsigned long)cbRead);
    if (FAILED(hr))
        return (hr == MF_E_END_OF_STREAM) ? AVERROR_EOF : AVERROR(EIO);
    // a short/zero read at the end has to be reported as an end of file: returning 0
    // makes libavformat retry the same position forever (raw elementary streams hang)
    if (cbRead == 0)
        return AVERROR_EOF;
    return (int)cbRead;
}

int64_t AVS_IO_Seek(void* pOpaque, int64_t llOffset, int iWhence)
{
    CAVSMediaSource* pSource = (CAVSMediaSource*)pOpaque;
    if (pSource->m_pByteStream == NULL)
        return -1;

    if (iWhence == AVSEEK_SIZE)
    {
        QWORD cb = 0;
        if (FAILED(pSource->m_pByteStream->GetLength(&cb)))
            return -1;
        return (int64_t)cb;
    }

    MFBYTESTREAM_SEEK_ORIGIN origin = msoBegin;
    if (iWhence == SEEK_CUR)
        origin = msoCurrent;
    else if (iWhence == SEEK_END)
    {
        QWORD cb = 0;
        if (FAILED(pSource->m_pByteStream->GetLength(&cb)))
            return -1;
        llOffset -= (int64_t)cb;
        origin = msoCurrent;
        if (SUCCEEDED(pSource->m_pByteStream->Seek(msoCurrent, 0, 0, &cb)))
            llOffset += (int64_t)cb;
    }

    QWORD qwNew = 0;
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] io seek origin=%d off=%lld\n", (int)origin, (long long)llOffset);
    if (FAILED(pSource->m_pByteStream->Seek(origin, (LONGLONG)llOffset, 0, &qwNew)))
        return -1;
    return (int64_t)qwNew;
}

STDMETHODIMP CAVSMediaSource::Open(void)
{
    AutoLock lock(m_Lock);
    return OpenLocked();
}

// Is there anything in here that only we can decode?  The byte stream handler asks this
// before it claims a file.  Deliberately narrower than the stream filter in OpenLocked():
// that one also publishes standard codecs, because measuring a standard stream through our
// source is useful - but describing a standard codec well is not a reason to take the
// file away from the system decoder that already plays it.
STDMETHODIMP CAVSMediaSource::IsAvsOrDra(BOOL* pbMine)
{
    if (pbMine == NULL)
        return E_POINTER;
    *pbMine = FALSE;
    AutoLock lock(m_Lock);
    if (m_pFmt == NULL)
        return MF_E_INVALIDREQUEST;                 // asked before Open()
    for (unsigned i = 0; i < m_pFmt->nb_streams; i++)
    {
        AVStream* st = m_pFmt->streams[i];
        if (st == NULL || st->codecpar == NULL)
            continue;
        if (st->codecpar->codec_id == AV_CODEC_ID_CAVS ||
            st->codecpar->codec_id == AV_CODEC_ID_AVS2 ||
            st->codecpar->codec_id == AV_CODEC_ID_AVS3 ||
            st->codecpar->codec_id == AV_CODEC_ID_DRA)
        {
            *pbMine = TRUE;
            break;
        }
    }
    return S_OK;
}

HRESULT CAVSMediaSource::OpenLocked(void)
{
    if (m_bOpened)
        return S_OK;
    if (m_pByteStream == NULL)
        return MF_E_INVALIDREQUEST;

    m_pAVIOBuffer = (uint8_t*)av_malloc(AVS_IO_BUFFER_SIZE);
    if (m_pAVIOBuffer == NULL)
        return E_OUTOFMEMORY;

    m_pAVIO = avio_alloc_context(m_pAVIOBuffer, AVS_IO_BUFFER_SIZE, 0, this,
                                 AVS_IO_Read, NULL, AVS_IO_Seek);
    if (m_pAVIO == NULL)
    {
        av_free(m_pAVIOBuffer);
        m_pAVIOBuffer = NULL;
        return E_OUTOFMEMORY;
    }
    m_pAVIO->seekable = m_bSeekable ? AVIO_SEEKABLE_NORMAL : 0;

    m_pFmt = avformat_alloc_context();
    if (m_pFmt == NULL)
    {
        avio_context_free(&m_pAVIO);
        m_pAVIOBuffer = NULL;
        return E_OUTOFMEMORY;
    }
    m_pFmt->pb = m_pAVIO;
    m_pFmt->flags |= AVFMT_FLAG_CUSTOM_IO;      // the AVIOContext stays ours

    // libavformat leaves the analyze window at zero for formats such as MPEG-TS, which
    // is enough to learn the codec types from the PMT but not enough to look at a
    // sequence header: without this a stream whose size only appears in the bit stream
    // (H.264 in TS, and AVS elementary streams in general) stays "unspecified size".
    m_pFmt->probesize            = 4u * 1024 * 1024;
    m_pFmt->max_analyze_duration = 5 * AV_TIME_BASE;

    // ---- pick the demuxer --------------------------------------------------
    // The file name provides a hint, and it is needed: a raw AVS elementary stream
    // cannot be recognised from its content (its start codes are shared with MPEG
    // video, and the raw demuxers have no probe function). A confident content probe
    // still wins, so a container that was saved under the wrong extension is read as
    // the container it really is.
    const AVInputFormat* pIF = NULL;
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] Open: attribute store %p\n", (void*)m_pAttributes);
    if (m_pAttributes != NULL)
    {
        WCHAR* pwHint = NULL;
        UINT32 cch = 0;
        HRESULT hrLookup = m_pAttributes->GetStringLength(AVS_SOURCE_ATTRIBUTE_DEMUX_HINT, &cch);
        UINT32 cItems = 0;
        m_pAttributes->GetCount(&cItems);
        if (AVS_TraceOn())
            fprintf(stderr, "[avssrc] hint lookup -> 0x%08X (store items %u)\n",
                    (unsigned)hrLookup, (unsigned)cItems);
        if (SUCCEEDED(hrLookup) && cch > 1)
        {
            // ask for one character more than GetStringLength reported: implementations
            // differ on whether the count includes the terminator, and GetString answers
            // E_INVALIDARG when the buffer is one WCHAR too small
            pwHint = new (std::nothrow) WCHAR[cch + 2];
            UINT32 cchWant = cch + 2;
            if (pwHint != NULL &&
                SUCCEEDED(m_pAttributes->GetString(AVS_SOURCE_ATTRIBUTE_DEMUX_HINT, pwHint, cchWant, &cch)))
            {
                std::string hint = Narrow(pwHint);
                if (AVS_TraceOn())
                    fprintf(stderr, "[avssrc] demux hint attribute: %s\n", hint.c_str());
                if (!hint.empty())
                    pIF = av_find_input_format(hint.c_str());
                if (AVS_TraceOn() && !hint.empty() && pIF == NULL)
                    fprintf(stderr, "[avssrc] no such demuxer: %s\n", hint.c_str());
            }
            delete[] pwHint;
        }
    }

    // An AVS elementary stream states its sequence header in band (00 00 01 B0 .. the next
    // start code).  Take the first one the probe region contains, or nothing at all when it
    // states none - guessing a format is worse than not describing one.  Measured on the local
    // files: none of them starts with a sequence header (they start with user data, 00 00 01
    // B6), but they do state one later, so searching rather than assuming byte 0 is what
    // makes this attribute available at all.
    auto AvsLeadingSequenceHeader = [](const BYTE* p, DWORD cb, std::vector<BYTE>& out)
    {
        for (DWORD i = 0; i + 3 < cb; i++)
        {
            if (!(p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x01))
                continue;
            if (p[i + 3] != 0xB0)
                continue;                             // some other access unit
            for (DWORD j = i + 4; j + 3 < cb; j++)
            {
                if (p[j] == 0x00 && p[j + 1] == 0x00 && p[j + 2] == 0x01)
                {
                    out.assign(p + i, p + j);         // the AU, without the next start code
                    return;
                }
            }
            out.assign(p + i, p + (cb < i + 4096 ? cb : i + 4096));
            return;
        }
    };

    if (m_bSeekable)
    {
        // read a piece of the file and let libavformat score it
        BYTE* pProbe = new (std::nothrow) BYTE[AVS_IO_BUFFER_SIZE];
        if (pProbe != NULL)
        {
            ULONG cbRead = 0;
            if (SUCCEEDED(m_pByteStream->Read(pProbe, AVS_IO_BUFFER_SIZE, &cbRead)) && cbRead > 0)
            {
                AVProbeData pd = { NULL, pProbe, (int)cbRead };
                int score = 0;
                const AVInputFormat* pProbed = av_probe_input_format2(&pd, 1, &score);
                // What the probe saw, always.  Without this line the only thing an operator
                // can see is the demuxer that was picked afterwards, and "the probe said
                // nothing" is invisible - which is how a 4 MB buffer full of MP4 bytes ended
                // up being parsed as a raw AVS stream without a trace.
                if (AVS_TraceOn())
                    fprintf(stderr, "[avssrc] probe over %u byte(s): %s (score %d), hint=%s\n",
                            (unsigned)cbRead, pProbed != NULL ? (pProbed->name ? pProbed->name : "?") : "(none)",
                            score, pIF != NULL ? (pIF->name ? pIF->name : "?") : "(none)");
                // 100 is what a real container such as MPEG-TS scores from its sync
                // bytes and tables; anything lower is a guess about an elementary
                // stream and must not override the hint.  The raw AVS demuxers all
                // score around 100 for each other's bit stream (they share the start
                // code space), so inside that family the hint - i.e. the extension -
                // always wins.
                if (pProbed != NULL && score >= 100 && pIF != pProbed &&
                    !(pIF != NULL && AvsIsRawAvsDemuxer(pIF->name) &&
                      AvsIsRawAvsDemuxer(pProbed->name)))
                {
                    if (AVS_TraceOn())
                        fprintf(stderr, "[avssrc] probe says %s (score %d), overriding the hint\n",
                                pProbed->name ? pProbed->name : "?", score);
                    pIF = pProbed;
                }
                else if (pIF == NULL && pProbed != NULL && score >= 25)
                {
                    if (AVS_TraceOn())
                        fprintf(stderr, "[avssrc] no hint, using the probe: %s (score %d)\n",
                                pProbed->name ? pProbed->name : "?", score);
                    pIF = pProbed;
                }
                if (pIF != NULL && AvsIsRawAvsDemuxer(pIF->name))
                {
                    // only for a raw AVS stream: for a container these bytes are TS packets,
                    // where the sequence header has to be found inside PES payloads instead
                    AvsLeadingSequenceHeader(pProbe, cbRead, m_FmtHdr);
                    if (AVS_TraceOn())
                        fprintf(stderr, "[avssrc] leading sequence header: %u byte(s)\n",
                                (unsigned)m_FmtHdr.size());
                }
            }
            delete[] pProbe;
            QWORD q = 0;
            m_pByteStream->Seek(msoBegin, 0, 0, &q);
        }
    }

    // The 128 KB probe region above stated no sequence header?  An AVS+ stream may state it
    // further in: measured, the only sequence header of samples/CCTV-4-short.avs sits at byte
    // 319770, and samples/CCTV-4.avs states 15 of them.  Scan a bounded 1 MB for the first
    // one, in whole chunks, then put the stream back - the same thing the probe above does.
    // Without this the MF_MT_MPEG_SEQUENCE_HEADER path would never run on these files, and an
    // unexercised path cannot be claimed to work.
    if (m_bSeekable && m_FmtHdr.empty() && pIF != NULL && AvsIsRawAvsDemuxer(pIF->name))
    {
        std::vector<BYTE> win;
        BYTE chunk[64 * 1024];
        ULONG cbRead = 0;
        while (win.size() < 1024u * 1024u &&
               SUCCEEDED(m_pByteStream->Read(chunk, (ULONG)sizeof(chunk), &cbRead)) && cbRead > 0)
        {
            win.insert(win.end(), chunk, chunk + cbRead);
            // leave the last bytes out: an access unit may still be cut by the chunk border
            const DWORD scan = win.size() > 8 ? (DWORD)win.size() - 8 : 0;
            if (scan)
                AvsLeadingSequenceHeader(win.data(), scan, m_FmtHdr);
            if (!m_FmtHdr.empty())
                break;
        }
        if (AVS_TraceOn())
            fprintf(stderr, "[avssrc] sequence header after a %u byte scan: %u byte(s)\n",
                    (unsigned)win.size(), (unsigned)m_FmtHdr.size());
        QWORD q = 0;
        m_pByteStream->Seek(msoBegin, 0, 0, &q);
    }

    // No hint from the name and nothing scored by the probe above.  Forcing "cavsvideo" here
    // used to turn a plain MP4 into a described stream of video/AVS 28x8192 with zero frames:
    // this FFmpeg build contains 6 demuxers and mov is not one of them (ask at runtime with
    // bin/ffcaps.exe), so the probe could only answer "none" - and the guess hid that.  Let
    // libavformat probe with its own, larger budget instead of guessing: if it says no, the
    // bytes are not ours.  An AVS elementary stream with an unusual name is unaffected, the raw
    // AVS demuxers score from the start codes and are picked by the probe either way.

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "scan_all_pmts", "1", 0);

    int err = avformat_open_input(&m_pFmt, NULL, pIF, &opts);
    av_dict_free(&opts);
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] open_input demuxer=%s err=%d\n",
                pIF ? pIF->name : "(libavformat probe)", err);
    if (err < 0)
    {
        avio_context_free(&m_pAVIO);
        m_pAVIOBuffer = NULL;
        m_pFmt = NULL;
        return MF_E_INVALIDMEDIATYPE;
    }

    err = avformat_find_stream_info(m_pFmt, NULL);
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] find_stream_info err=%d streams=%u\n",
                err, m_pFmt ? m_pFmt->nb_streams : 0u);
    if (err < 0)
    {
        avformat_close_input(&m_pFmt);
        avio_context_free(&m_pAVIO);
        m_pAVIOBuffer = NULL;
        return MF_E_INVALIDMEDIATYPE;
    }

    // An AVS elementary stream carries no container-level extradata, so the sequence header
    // read while probing becomes the decoder's format description.  The media type code below
    // then publishes it as MF_MT_MPEG_SEQUENCE_HEADER, the way the H.264 control path does.
    if (!m_FmtHdr.empty())
    {
        for (unsigned i = 0; i < m_pFmt->nb_streams; i++)
        {
            AVCodecParameters* par = m_pFmt->streams[i] ? m_pFmt->streams[i]->codecpar : NULL;
            if (par == NULL || par->extradata != NULL || par->extradata_size > 0)
                continue;
            if (par->codec_id != AV_CODEC_ID_CAVS)
                continue;
            par->extradata = (uint8_t*)av_malloc(m_FmtHdr.size() + AV_INPUT_BUFFER_PADDING_SIZE);
            if (par->extradata == NULL)
                continue;
            memcpy(par->extradata, m_FmtHdr.data(), m_FmtHdr.size());
            par->extradata_size = (int)m_FmtHdr.size();
            if (AVS_TraceOn())
                fprintf(stderr, "[avssrc] stream %u: %d byte(s) of sequence header handed "
                                "over as extradata / MF_MT_MPEG_SEQUENCE_HEADER\n",
                        i, par->extradata_size);
        }
    }

    // ---- presentation timeline, rebased on zero ----------------------------
    // m_llStartHns is the raw presentation time of the earliest picture; m_rtStart is
    // kept only as the container start for seeking
    m_llStartHns = 0;
    m_rtStart = 0;
    if (m_pFmt->start_time != AVS_NOPTS)
        m_rtStart = (LONGLONG)av_rescale(m_pFmt->start_time, HNS_PER_SECOND, AV_TIME_BASE);
    m_rtDuration = 0;
    if (m_pFmt->duration != AVS_NOPTS)
        m_rtDuration = (LONGLONG)av_rescale(m_pFmt->duration, HNS_PER_SECOND, AV_TIME_BASE);

    // the first packet of the file is the first one to be decoded, not the first one to
    // be displayed: find out what the earliest display time is and use that as time zero
    PrerollScan();
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] preroll %zu packet(s), start %lld (100ns)\n",
                m_Preroll.size(), m_llStartHns);

    // ---- one stream descriptor per elementary stream ------------------------
    std::vector<IMFStreamDescriptor*> pSDs;
    for (unsigned i = 0; i < m_pFmt->nb_streams; i++)
    {
        AVStream* st = m_pFmt->streams[i];
        GUID tmpSub = GUID_NULL;
        if (st == NULL || st->codecpar == NULL)
            continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            st->codecpar->codec_id != AV_CODEC_ID_CAVS &&
            st->codecpar->codec_id != AV_CODEC_ID_AVS2 &&
            st->codecpar->codec_id != AV_CODEC_ID_AVS3 &&
            !StdVideoSubtype(st->codecpar->codec_id, &tmpSub))
            continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            st->codecpar->codec_id != AV_CODEC_ID_DRA &&
            !StdAudioSubtype(st->codecpar->codec_id, &tmpSub))
            continue;

        IMFStreamDescriptor* pSD = NULL;
        if (FAILED(BuildStreamDescriptor((int)i, &pSD)))
            continue;
        pSDs.push_back(pSD);          // the creation reference, dropped after the PD is built

        StreamEntry e;
        e.pStream = NULL;
        // m_Streams keeps its own reference: CloseFile() releases it, and the
        // presentation descriptor holds an independent one.  Without this AddRef the
        // descriptor is destroyed by CloseFile() and releasing the PD afterwards then
        // walks freed stream descriptors (crash on shutdown).
        pSD->AddRef();
        e.pSD = pSD;
        e.ffIndex = (int)i;
        m_Streams.push_back(e);
    }
    if (pSDs.empty())
    {
        for (size_t i = 0; i < m_Streams.size(); i++)
            SAFE_RELEASE(m_Streams[i].pSD);
        m_Streams.clear();
        avformat_close_input(&m_pFmt);
        avio_context_free(&m_pAVIO);
        m_pAVIOBuffer = NULL;
        return MF_E_INVALIDMEDIATYPE;
    }

    HRESULT hr = MFCreatePresentationDescriptor((DWORD)pSDs.size(), &pSDs[0], &m_pPD);
    for (size_t i = 0; i < pSDs.size(); i++)
        SAFE_RELEASE(pSDs[i]);
    if (FAILED(hr))
        return hr;

    std::vector<BOOL> selected(m_Streams.size(), TRUE);
    for (DWORD i = 0; i < m_Streams.size(); i++)
        m_pPD->SelectStream(i);

    if (m_rtDuration > 0)
        m_pPD->SetUINT64(MF_PD_DURATION, (QWORD)m_rtDuration);

    if (m_pAttributes == NULL)
        IF_FAILED_RETURN(MFCreateAttributes(&m_pAttributes, 2));
    if (m_pEventQueue == NULL)
        IF_FAILED_RETURN(MFCreateEventQueue(&m_pEventQueue));

    m_bOpened = TRUE;
    return S_OK;
}

void CAVSMediaSource::CloseFile(void)
{
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] CloseFile this=%p\n", (void*)this);
    ClearPreroll();
    for (size_t i = 0; i < m_Streams.size(); i++)
    {
        SAFE_RELEASE(m_Streams[i].pStream);
        SAFE_RELEASE(m_Streams[i].pSD);
    }
    m_Streams.clear();

    if (m_pFmt != NULL)
        avformat_close_input(&m_pFmt);
    if (m_pAVIO != NULL)
    {
        avio_context_free(&m_pAVIO);        // also releases the (maybe reallocated) buffer
        m_pAVIOBuffer = NULL;
    }
    m_bOpened = FALSE;
}

// ---------------------------------------------------------------------------
// media types
// ---------------------------------------------------------------------------
HRESULT CAVSMediaSource::BuildVideoType(AVStream* st, IMFMediaType** ppType)
{
    const AVCodecParameters* par = st->codecpar;
    if (par->width <= 0 || par->height <= 0)
        return MF_E_INVALIDMEDIATYPE;

    IMFMediaType* pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    IF_FAILED_RETURN(hr);

    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = st->r_frame_rate;

    GUID sub = MFVideoFormat_AVS;
    BOOL bStd = FALSE;
    if (par->codec_id == AV_CODEC_ID_AVS2)
        sub = MFVideoFormat_AVS2;
    else if (par->codec_id == AV_CODEC_ID_AVS3)
        sub = MFVideoFormat_AVS3;
    else if (StdVideoSubtype(par->codec_id, &sub))
        bStd = TRUE;

    // Measurement only (KR7.2 candidate A, scripts/type-diff.py): publish the same AVS+ stream
    // under a subtype GUID that is not in MF's FourCC layout, to find out whether the GUID
    // layout is a contract item at all.  Off unless AVS_MFT_SUBTYPE_SHAPE is set.
    if (sub == MFVideoFormat_AVS &&
        GetEnvironmentVariableW(L"AVS_MFT_SUBTYPE_SHAPE", NULL, 0) > 0)
        sub = MFVideoFormat_AVS_ALT;

    do
    {
        if (FAILED(hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)))
            break;
        if (FAILED(hr = pType->SetGUID(MF_MT_SUBTYPE, sub)))
            break;
        // A size of 0x0 is not "unknown", it is invalid: the topology loader answers
        // E_INVALIDARG / MF_E_INVALIDTYPE for it.  Leave the attribute out instead -
        // the decoder MFT then negotiates the size when the sequence header arrives.
        if (par->width > 0 && par->height > 0 &&
            FAILED(hr = pType->SetUINT64(MF_MT_FRAME_SIZE,
                                         (((QWORD)(UINT32)par->width) << 32) | (UINT32)par->height)))
            break;
        if (fr.num > 0 && fr.den > 0 &&
            FAILED(hr = pType->SetUINT64(MF_MT_FRAME_RATE,
                                         (((QWORD)(UINT32)fr.num) << 32) | (UINT32)fr.den)))
            break;
        if (par->bit_rate > 0 &&
            FAILED(hr = pType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)par->bit_rate)))
            break;        if (par->extradata != NULL && par->extradata_size > 0 &&
            FAILED(hr = pType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, par->extradata,
                                       (DWORD)par->extradata_size)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)))
            break;
        // MF_MT_DEFAULT_STRIDE is deliberately NOT set here.  It describes the pitch of an
        // *uncompressed* frame; on a compressed elementary stream it is a lie, and it is the
        // one attribute that our published AVS type carried while the H.264 control path
        // (same source, same MFT, same sink) did not - scripts/type-diff.py found it.  The
        // decoder MFT derives the pitch from AvsSampleLayout, never from this attribute.

        // The colour signalling of the stream, as far as the container knows it.  A
        // Matroska AVS3 HDR file states primaries / transfer / range there, and the
        // decoder MFT inherits these attributes from the type it is given; an AVS
        // elementary stream states nothing, so nothing is set.
        {
            AvsColorInfo col;
            col.primaries = par->color_primaries;
            col.transfer  = par->color_trc;
            col.matrix    = par->color_space;
            col.range     = par->color_range;
            AvsApplyColor(pType, col);
        }

        // pixel aspect ratio, when the container states one (AVS states its aspect
        // ratio inside the sequence header instead; see README, geometry)
        if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0)
        {
            MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO,
                                (UINT32)par->sample_aspect_ratio.num,
                                (UINT32)par->sample_aspect_ratio.den);
        }

        // the system decoders want the profile/level of their own format
        if (bStd)
        {
            if (par->profile > 0)
                pType->SetUINT32(MF_MT_MPEG2_PROFILE, (UINT32)par->profile);
            if (par->level > 0)
                pType->SetUINT32(MF_MT_MPEG2_LEVEL, (UINT32)par->level);
        }
    } while (false);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pType);
        return hr;
    }
    *ppType = pType;
    return S_OK;
}

HRESULT CAVSMediaSource::BuildAudioType(AVStream* st, IMFMediaType** ppType)
{
    const AVCodecParameters* par = st->codecpar;

    IMFMediaType* pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    IF_FAILED_RETURN(hr);

    const UINT32 rate = par->sample_rate > 0 ? (UINT32)par->sample_rate : 0;
    const UINT32 ch = par->ch_layout.nb_channels > 0 ? (UINT32)par->ch_layout.nb_channels : 0;

    GUID sub = MFAudioFormat_DRA;
    BOOL bStd = FALSE;
    if (StdAudioSubtype(par->codec_id, &sub))
        bStd = TRUE;

    do
    {
        if (FAILED(hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)))
            break;
        if (FAILED(hr = pType->SetGUID(MF_MT_SUBTYPE, sub)))
            break;
        if (rate > 0 && FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate)))
            break;
        if (ch > 0 && FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch)))
            break;
        // name the speakers when libavformat knows the arrangement, so that the decoder
        // MFT - and through it the audio renderer - does not have to guess from the
        // channel count
        {
            const UINT32 chMask = AvsChannelMaskToKS(par->ch_layout);
            if (chMask != 0)
                pType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, chMask);
        }

        if (bStd)
        {
            UINT32 bits = par->bits_per_coded_sample > 0 ? (UINT32)par->bits_per_coded_sample : 16;
            pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
            // The Dolby bitstreams are byte-oriented with no fixed sample block, so their frame
            // alignment is one byte; AAC counts two-byte syntax elements.  Not checked against a
            // Dolby decoder here because this machine registers none (bin/mftenum.exe), which is why
            // the value is commented rather than treated as known.
            pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                             (sub == MFAudioFormat_AAC)             ? 2u :
                             (sub == MFAudioFormat_Dolby_AC3 ||
                              sub == MFAudioFormat_Dolby_DDPlus)    ? 1u
                                                                    : (UINT32)(ch * (bits / 8)));
            if (rate > 0)
                pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                 (UINT32)((QWORD)rate * ch * (bits / 8)));
            if (sub == MFAudioFormat_AAC)
            {
                pType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0u);
                // raw (not ADTS) AAC, as libavformat hands it to us from TS/MP4.
                // APLI = (profile object type << 5) | level index, and FFmpeg's
                // AV_PROFILE_AAC_* values are exactly the MPEG object types.
                const UINT32 aacProfile = (par->profile > 0) ? (UINT32)(par->profile & 0x7F) : 2u;
                const UINT32 aacLevel = (par->level > 0) ? (UINT32)(par->level & 0x1F) : 4u;
                pType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
                                 (aacProfile << 5) | aacLevel);
                if (par->extradata != NULL && par->extradata_size > 0)
                    pType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, par->extradata,
                                   (DWORD)par->extradata_size);
            }
        }
    } while (false);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pType);
        return hr;
    }
    *ppType = pType;
    return S_OK;
}

HRESULT CAVSMediaSource::BuildStreamDescriptor(int ffIndex, IMFStreamDescriptor** ppSD)
{
    AVStream* st = m_pFmt->streams[ffIndex];

    IMFMediaType* pType = NULL;
    HRESULT hr = (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
               ? BuildVideoType(st, &pType)
               : BuildAudioType(st, &pType);
    IF_FAILED_RETURN(hr);

    const DWORD streamId = (DWORD)m_Streams.size();
    IMFMediaType* pTypes[1] = { pType };
    IMFStreamDescriptor* pSD = NULL;
    hr = MFCreateStreamDescriptor(streamId, 1, pTypes, &pSD);
    SAFE_RELEASE(pType);
    IF_FAILED_RETURN(hr);

    pSD->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId);

    *ppSD = pSD;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IMFMediaSource
// ---------------------------------------------------------------------------
STDMETHODIMP CAVSMediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if (pdwCharacteristics == NULL)
        return E_POINTER;
    AutoLock lock(m_Lock);
    if (!m_bOpened)
        return MF_E_NOT_INITIALIZED;

    DWORD dw = MFMEDIASOURCE_DOES_NOT_USE_NETWORK;
    if (m_bSeekable)
        dw |= MFMEDIASOURCE_CAN_SEEK;
    dw |= MFMEDIASOURCE_CAN_PAUSE;
    *pdwCharacteristics = dw;
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD)
{
    if (ppPD == NULL)
        return E_POINTER;
    AutoLock lock(m_Lock);
    if (!m_bOpened || m_pPD == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pPD->Clone(ppPD);
}

// Every stream of the presentation has finished: the media session only reports the end
// of playback when the source itself says the presentation is over.
void CAVSMediaSource::NotifyStreamEnded(void)
{
    AutoLock lock(m_Lock);

    if (m_bEopPosted || m_pEventQueue == NULL || m_nStartedStreams == 0)
    {
        if (!m_bEopPosted && m_nStartedStreams != 0)
            m_nEndedStreams++;
        return;
    }
    m_nEndedStreams++;
    if (m_nEndedStreams < m_nStartedStreams)
        return;

    m_bEopPosted = TRUE;
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] every stream ended, posting MEEndOfPresentation\n");
    const HRESULT hr = m_pEventQueue->QueueEventParamVar(MEEndOfPresentation, GUID_NULL, S_OK, NULL);
    if (AVS_TraceOn() && FAILED(hr))
        fprintf(stderr, "[avssrc] MEEndOfPresentation -> 0x%08X\n", (unsigned)hr);
}

STDMETHODIMP CAVSMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                    const PROPVARIANT* pvarStartPosition)
{
    if (pPD == NULL)
        return E_INVALIDARG;
    if (pguidTimeFormat != NULL && *pguidTimeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    AutoLock lock(m_Lock);
    if (!m_bOpened || m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;

    LONGLONG rtStart = 0;
    if (pvarStartPosition != NULL)
        IF_FAILED_RETURN(FromHnsProp(pvarStartPosition, &rtStart));

    // which streams does the caller want?  the selection lives in the presentation
    // descriptor that was handed to us (the stream descriptors carry our stream id, so
    // a clone of the presentation descriptor still maps back onto our entries)
    std::vector<BOOL> want(m_Streams.size(), TRUE);
    DWORD cPD = 0;
    if (SUCCEEDED(pPD->GetStreamDescriptorCount(&cPD)))
    {
        for (DWORD i = 0; i < cPD; i++)
        {
            IMFStreamDescriptor* pSD = NULL;
            BOOL bSel = FALSE;
            if (FAILED(pPD->GetStreamDescriptorByIndex(i, &bSel, &pSD)) || pSD == NULL)
                continue;
            size_t idx = i;
            const DWORD sdId = MFGetAttributeUINT32(pSD, MF_DEVICESTREAM_STREAM_ID, 0xFFFFFFFF);
            if (sdId != 0xFFFFFFFF && sdId < m_Streams.size())
                idx = sdId;
            if (idx < m_Streams.size())
                want[idx] = bSel;
            SAFE_RELEASE(pSD);
        }
    }

    if (AVS_TraceOn())
    {
        fprintf(stderr, "[avssrc] Start at %lld (100ns), %zu stream(s):", rtStart,
                m_Streams.size());
        for (size_t i = 0; i < m_Streams.size(); i++)
            fprintf(stderr, " ff=%d %s", m_Streams[i].ffIndex,
                    want[i] ? "selected" : "DESELECTED");
        fprintf(stderr, "\n");
    }

    // create or restart one media stream per selected elementary stream
    for (size_t i = 0; i < m_Streams.size(); i++)
    {
        IMFStreamDescriptor* pSD = m_Streams[i].pSD;
        if (!want[i])
            continue;

        if (m_Streams[i].pStream == NULL)
        {
            CAVSInputStream* pStream = NULL;
            IF_FAILED_RETURN(CAVSInputStream::CreateInstance(this, pSD, m_Streams[i].ffIndex,
                                                             __uuidof(IMFMediaStream),
                                                             (void**)&pStream));
            m_Streams[i].pStream = pStream;

            // announce the stream object on our own queue
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt = VT_UNKNOWN;
            var.punkVal = static_cast<IUnknown*>(static_cast<IMFMediaStream*>(pStream));
            var.punkVal->AddRef();
            HRESULT hr = m_pEventQueue->QueueEventParamVar(MENewStream, GUID_NULL, S_OK, &var);
            PropVariantClear(&var);
            IF_FAILED_RETURN(hr);
        }
        else
        {
            m_Streams[i].pStream->Reset();
        }
    }

    // the presentation restarts: end of presentation has to be reported again
    m_nStartedStreams = 0;
    for (size_t i = 0; i < m_Streams.size(); i++)
        if (want[i])
            m_nStartedStreams++;
    m_nEndedStreams = 0;
    m_bEopPosted = FALSE;

    if (m_pFmt != NULL)
    {
        // rtStart == 0 is the start of the file: the prologue we already read *is* the
        // start, so replay it instead of seeking
        if (rtStart != 0 || m_Preroll.empty())
        {
            if (rtStart != 0)
                ClearPreroll();
            const int64_t ts = av_rescale(m_llStartHns + rtStart, AV_TIME_BASE, HNS_PER_SECOND);
            av_seek_frame(m_pFmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        }
        else
        {
            m_PrerollPos = 0;
        }
    }
    m_bStarted = TRUE;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_UNKNOWN;
    var.punkVal = pPD;
    pPD->AddRef();
    HRESULT hr = m_pEventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);
    PropVariantClear(&var);
    IF_FAILED_RETURN(hr);

    for (size_t i = 0; i < m_Streams.size(); i++)
    {
        if (m_Streams[i].pStream != NULL)
            m_Streams[i].pStream->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, NULL);
    }
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::Stop(void)
{
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] Stop this=%p\n", (void*)this);
    AutoLock lock(m_Lock);
    if (!m_bStarted)
        return MF_E_INVALIDREQUEST;
    m_bStarted = FALSE;

    if (m_pEventQueue != NULL)
        m_pEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, NULL);
    for (size_t i = 0; i < m_Streams.size(); i++)
    {
        if (m_Streams[i].pStream != NULL)
            m_Streams[i].pStream->Reset();
    }
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::Pause(void)
{
    AutoLock lock(m_Lock);
    if (!m_bStarted)
        return MF_E_INVALIDREQUEST;

    if (m_pEventQueue != NULL)
        m_pEventQueue->QueueEventParamVar(MESourcePaused, GUID_NULL, S_OK, NULL);
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::Shutdown(void)
{
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] Shutdown this=%p\n", (void*)this);
    AutoLock lock(m_Lock);
    m_bShutdown = TRUE;
    m_bStarted = FALSE;

    if (m_pEventQueue != NULL)
        m_pEventQueue->Shutdown();
    for (size_t i = 0; i < m_Streams.size(); i++)
    {
        if (m_Streams[i].pStream != NULL)
            m_Streams[i].pStream->ShutdownQueue();
    }
    CloseFile();
    return S_OK;
}

// ---------------------------------------------------------------------------
// IMFMediaSourceEx
// ---------------------------------------------------------------------------
STDMETHODIMP CAVSMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;
    AutoLock lock(m_Lock);
    // The application is expected to configure the source through this store before
    // Open(), so the store has to exist that early; returning E_NOT_SET would leave
    // the caller (SourceReader, byte stream handler, a test) with nowhere to put the
    // request.
    if (m_pAttributes == NULL && FAILED(MFCreateAttributes(&m_pAttributes, 4)))
        return E_OUTOFMEMORY;
    if (AVS_TraceOn())
        fprintf(stderr, "[avssrc] GetSourceAttributes -> %p\n", (void*)m_pAttributes);
    m_pAttributes->AddRef();
    *ppAttributes = m_pAttributes;
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier,
                                                  IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;
    AutoLock lock(m_Lock);

    // dwStreamIdentifier is the index of the stream in the presentation descriptor,
    // which is the same order as m_Streams.  An IMFStreamDescriptor is an IMFAttributes.
    if (dwStreamIdentifier >= m_Streams.size() || m_Streams[dwStreamIdentifier].pSD == NULL)
        return MF_E_INVALIDSTREAMNUMBER;

    m_Streams[dwStreamIdentifier].pSD->AddRef();
    *ppAttributes = static_cast<IMFAttributes*>(m_Streams[dwStreamIdentifier].pSD);
    return S_OK;
}

STDMETHODIMP CAVSMediaSource::SetD3DManager(IUnknown* pManager)
{
    // CPU decoders only for now: refusing here keeps the pipeline on system memory.
    UNREFERENCED_PARAMETER(pManager);
    return MF_E_INVALIDREQUEST;
}

// ---------------------------------------------------------------------------
// demuxing
// ---------------------------------------------------------------------------
LONGLONG CAVSMediaSource::ToHns(AVRational tb, long long ts) const
{
    if (ts == (long long)AVS_NOPTS || tb.num <= 0 || tb.den <= 0)
        return 0;
    AVRational hns;
    hns.num = 1;
    hns.den = (int)HNS_PER_SECOND;
    LONGLONG rt = (LONGLONG)av_rescale_q(ts, tb, hns) - m_llStartHns;
    return (rt < 0) ? 0 : rt;
}

// Did this stream ever carry a real time stamp?  PrerollScan answers that; a raw
// elementary stream (.avs2 / .avs3 / .avs) does not.
bool CAVSMediaSource::StreamHasTimestamps(int ffIndex) const
{
    if (ffIndex < 0 || (size_t)ffIndex >= m_bStreamStamps.size())
        return false;
    return m_bStreamStamps[(size_t)ffIndex] != 0;
}

// The frame rate the probing found for a stream ({0,0} when nothing is known).
AVRational CAVSMediaSource::StreamFrameRate(int ffIndex) const
{
    AVRational fr = { 0, 0 };
    if (m_pFmt == NULL || ffIndex < 0 || (unsigned)ffIndex >= m_pFmt->nb_streams)
        return fr;
    const AVStream* st = m_pFmt->streams[ffIndex];
    if (st == NULL)
        return fr;
    fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0)
        fr = st->r_frame_rate;
    return fr;
}

// read the first packets to find the earliest *presentation* time (see the comment on
// PrerollScan in the header).  The packets are kept and handed out again afterwards, so
// nothing is lost and no second seek is needed.
void CAVSMediaSource::PrerollScan(void)
{
    LONGLONG llMin = 0;
    bool bHave = false;

    const size_t nStreams = m_pFmt ? m_pFmt->nb_streams : 0;
    m_bStreamStamps.assign(nStreams, 0);
    std::vector<long long> llPtsMin(nStreams, 0), llPtsMax(nStreams, 0);
    std::vector<char> bPtsSeen(nStreams, 0);

    for (int i = 0; i < AVS_PREROLL_PACKETS; i++)
    {
        AVPacket* p = av_packet_alloc();
        if (p == NULL)
            break;
        if (av_read_frame(m_pFmt, p) < 0 || p->stream_index < 0 || p->stream_index >= m_pFmt->nb_streams)
        {
            if (AVS_TraceOn())
                fprintf(stderr, "[avssrc] preroll stop after %d packet(s)\n", i);
            av_packet_free(&p);
            break;
        }

        const AVStream* st = m_pFmt->streams[p->stream_index];
        if (p->pts != AVS_NOPTS && st != NULL && st->time_base.num > 0 && st->time_base.den > 0)
        {
            const size_t si = (size_t)p->stream_index;
            if (si < nStreams)
            {
                if (!bPtsSeen[si])
                {
                    bPtsSeen[si] = 1;
                    llPtsMin[si] = llPtsMax[si] = p->pts;
                }
                else
                {
                    if (p->pts < llPtsMin[si]) llPtsMin[si] = p->pts;
                    if (p->pts > llPtsMax[si]) llPtsMax[si] = p->pts;
                }
            }
            AVRational hns;
            hns.num = 1;
            hns.den = (int)HNS_PER_SECOND;
            const LONGLONG rt = (LONGLONG)av_rescale_q(p->pts, st->time_base, hns);
            if (!bHave || rt < llMin)
            {
                llMin = rt;
                bHave = true;
            }
        }
        m_Preroll.push_back(p);
        if (AVS_TraceOn() && (i % 50) == 49)
            fprintf(stderr, "[avssrc] preroll %d packet(s)...\n", i + 1);
    }

    // A stream only has a usable timeline if its packets actually *differ* in time.  The
    // raw AVS3 / AVS2 / AVS elementary demuxers hand out packets that all carry the very
    // same (synthesized) pts, and FFmpeg's raw demuxers use AVFMT_NOTIMESTAMPS: stamping
    // every MF sample with the same value would present the whole file at t = 0.
    for (size_t i = 0; i < nStreams; i++)
        m_bStreamStamps[i] = (bPtsSeen[i] && llPtsMax[i] > llPtsMin[i]) ? 1 : 0;

    if (AVS_TraceOn())
        for (size_t i = 0; i < nStreams; i++)
            fprintf(stderr, "[avssrc] stream %u timeline: %s\n", (unsigned)i,
                    m_bStreamStamps[i] ? "from the container" : "synthesized");

    if (bHave)
        m_llStartHns = llMin;
    m_PrerollPos = 0;
}

void CAVSMediaSource::ClearPreroll(void)
{
    for (size_t i = 0; i < m_Preroll.size(); i++)
        if (m_Preroll[i] != NULL)
            av_packet_free(&m_Preroll[i]);
    m_Preroll.clear();
    m_PrerollPos = 0;
}

HRESULT CAVSMediaSource::ReadPacket(AVPacket** ppPkt)
{
    AutoLock lock(m_DemuxLock);
    if (m_pFmt == NULL)
        return MF_E_NOT_INITIALIZED;

    if (m_PrerollPos < m_Preroll.size())
    {
        AVPacket* pPre = m_Preroll[m_PrerollPos];
        m_PrerollPos++;
        if (pPre != NULL)
        {
            av_packet_ref(*ppPkt, pPre);   // the prologue stays owned by us: Start() can
            return S_OK;                   // replay it after a stop
        }
    }

    if (av_read_frame(m_pFmt, *ppPkt) < 0)
        return MF_E_END_OF_STREAM;
    return S_OK;
}

// ===========================================================================
// CAVSInputStream
// ===========================================================================
CAVSInputStream::CAVSInputStream(CAVSMediaSource* pSource, IMFStreamDescriptor* pSD, int ffIndex)
    : m_RefCount(1)
    , m_pSource(pSource)
    , m_pSD(pSD)
    , m_pEventQueue(NULL)
    , m_ffIndex(ffIndex)
    , m_bEOS(FALSE)
    , m_QueueCount(0)
    , m_rtSynth(0)
{
    if (m_pSD != NULL)
        m_pSD->AddRef();
    if (m_pSource != NULL)
        m_pSource->AddRef();
}

CAVSInputStream::~CAVSInputStream()
{
    if (AVS_TraceOn())
    {
        const int idx = (m_ffIndex >= 0 && m_ffIndex < 8) ? m_ffIndex : 0;
        fprintf(stderr, "[avssrc] ~stream ff=%d this=%p, %ld RequestSample call(s), "
                        "%d event(s) unanswered\n", m_ffIndex, (void*)this,
                        (long)g_avsRequestCount[idx], (int)m_QueueCount);
    }
    ClearQueue();
    SAFE_RELEASE(m_pSD);
    SAFE_RELEASE(m_pEventQueue);
    SAFE_RELEASE(m_pSource);
}

HRESULT CAVSInputStream::CreateInstance(CAVSMediaSource* pSource, IMFStreamDescriptor* pSD,
                                        int ffIndex, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL || pSource == NULL || pSD == NULL)
        return E_POINTER;

    CAVSInputStream* pStream = new (std::nothrow) CAVSInputStream(pSource, pSD, ffIndex);
    if (pStream == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = MFCreateEventQueue(&pStream->m_pEventQueue);
    if (SUCCEEDED(hr))
        hr = pStream->QueryInterface(riid, ppvObject);
    pStream->Release();
    return hr;
}

STDMETHODIMP_(ULONG) CAVSInputStream::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CAVSInputStream::Release()
{
    LONG lRef = InterlockedDecrement(&m_RefCount);
    if (lRef == 0)
        delete this;
    return (ULONG)lRef;
}

STDMETHODIMP CAVSInputStream::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaStream))
    {
        *ppvObject = static_cast<IMFMediaStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

STDMETHODIMP CAVSInputStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP CAVSInputStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP CAVSInputStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP CAVSInputStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP CAVSInputStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if (ppMediaSource == NULL)
        return E_POINTER;
    if (m_pSource == NULL)
        return MF_E_INVALIDREQUEST;
    *ppMediaSource = static_cast<IMFMediaSource*>(m_pSource);
    m_pSource->AddRef();
    return S_OK;
}

STDMETHODIMP CAVSInputStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (ppStreamDescriptor == NULL)
        return E_POINTER;
    if (m_pSD == NULL)
        return MF_E_INVALIDREQUEST;
    m_pSD->AddRef();
    *ppStreamDescriptor = m_pSD;
    return S_OK;
}

// Drop everything that was queued: used on Stop()/Start().
void CAVSInputStream::Reset(void)
{
    ClearQueue();
    m_rtSynth = 0;
}

void CAVSInputStream::ClearQueue(void)
{
    for (size_t i = 0; i < m_Pending.size(); i++)
        SAFE_RELEASE(m_Pending[i]);
    m_Pending.clear();

    if (m_pEventQueue == NULL)
        return;
    IMFMediaEvent* pEvent = NULL;
    while (SUCCEEDED(m_pEventQueue->GetEvent(0, &pEvent)))
        SAFE_RELEASE(pEvent);
    m_bEOS = FALSE;
    m_QueueCount = 0;
}

void CAVSInputStream::ShutdownQueue(void)
{
    if (m_pEventQueue != NULL)
        m_pEventQueue->Shutdown();
}

// IMFMediaStream::RequestSample hands out a token that identifies the request.  The
// pipeline matches the MEMediaSample event against that token, and a sample without it
// is dropped: in a media session that silently loses most of the stream (the decoder
// then sees a fraction of the access units and the presentation ends early).
void CAVSInputStream::AttachToken(IMFSample* pSample, IUnknown* pToken)
{
    if (pSample == NULL || pToken == NULL)
        return;
    pSample->SetUnknown(MFSampleExtension_Token, pToken);
}

HRESULT CAVSInputStream::QueueSample(IMFSample* pSample)
{
    if (pSample == NULL || m_pEventQueue == NULL)
        return E_POINTER;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_UNKNOWN;
    var.punkVal = pSample;
    pSample->AddRef();
    HRESULT hr = m_pEventQueue->QueueEventParamVar(MEMediaSample, GUID_NULL, S_OK, &var);
    PropVariantClear(&var);
    return hr;
}

// A compressed access unit that belongs to this stream but was read while the other
// stream was being served.  It is not posted to the event queue until the consumer asks.
void CAVSInputStream::PushPending(IMFSample* pSample)
{
    if (pSample == NULL)
        return;
    pSample->AddRef();
    m_Pending.push_back(pSample);
}

IMFSample* CAVSInputStream::PopPending(void)
{
    if (m_Pending.empty())
        return NULL;
    IMFSample* pSample = m_Pending.front();
    m_Pending.erase(m_Pending.begin());
    return pSample;
}

HRESULT CAVSInputStream::QueueEndOfStream(void)
{
    if (m_pEventQueue == NULL)
        return MF_E_NOT_INITIALIZED;
    if (m_bEOS)
        return S_OK;                        // already reported, do not count it twice
    m_bEOS = TRUE;
    if (AVS_TraceOn())
    {
        const int idx = (m_ffIndex >= 0 && m_ffIndex < 8) ? m_ffIndex : 0;
        fprintf(stderr, "[avssrc] stream ff=%d: end of stream (%ld sample(s) created, "
                        "%ld request(s), %ld pending, %ld queued events)\n", m_ffIndex,
                (long)g_avsSampleCount[idx], (long)g_avsRequestCount[idx],
                (long)PendingCount(), (long)m_QueueCount);
    }
    const HRESULT hr = m_pEventQueue->QueueEventParamVar(MEEndOfStream, GUID_NULL, S_OK, NULL);
    if (m_pSource != NULL)
        m_pSource->NotifyStreamEnded();
    return hr;
}

// Give one access unit of this stream back as a MEMediaSample event.  Access units of
// the other elementary stream stay in that stream's queue, which is what keeps the
// interleaving of the file.
STDMETHODIMP CAVSInputStream::RequestSample(IUnknown* pToken)
{
    static LONG s_nServed = 0;        // process wide, only used for the trace

    if (AVS_TraceOn())
    {
        const int idx = (m_ffIndex >= 0 && m_ffIndex < 8) ? m_ffIndex : 0;
        const LONG n = InterlockedIncrement(&g_avsRequestCount[idx]);
        if (n <= 3 || (n % 25) == 0)
            fprintf(stderr, "[avssrc] RequestSample ff=%d: %ld\n", m_ffIndex, (long)n);
    }

    if (m_bEOS)
    {
        // every further request has to be answered with another end of stream event:
        // the pipeline keeps asking while it drains, and if nothing ever arrives it never
        // reports the end of the presentation
        if (m_pEventQueue == NULL)
            return MF_E_NOT_INITIALIZED;
        return m_pEventQueue->QueueEventParamVar(MEEndOfStream, GUID_NULL, S_OK, NULL);
    }
    if (m_pSource == NULL)
        return MF_E_NOT_INITIALIZED;

    // an access unit that was already posted (while the other stream was being read)
    // answers this request
    if (m_QueueCount > 0)
    {
        m_QueueCount--;
        return S_OK;
    }

    // an access unit that was read earlier for the other stream's sake answers this
    // request too, without reading further into the file
    IMFSample* pPending = PopPending();
    if (pPending != NULL)
    {
        AttachToken(pPending, pToken);
        const HRESULT hrQ = QueueSample(pPending);
        SAFE_RELEASE(pPending);
        if (AVS_TraceOn() && (InterlockedIncrement(&s_nServed) % 50) == 0)
            fprintf(stderr, "[avssrc] served %d sample(s), %d from the backlog\n",
                    (int)s_nServed, (int)PendingCount());
        return hrQ;
    }

    for (;;)
    {
        AVPacket* pkt = av_packet_alloc();
        if (pkt == NULL)
            return E_OUTOFMEMORY;

        HRESULT hr = m_pSource->ReadPacket(&pkt);
        if (FAILED(hr))
        {
            av_packet_free(&pkt);
            QueueEndOfStream();
            return S_OK;
        }

        CAVSInputStream* pTarget = NULL;
        for (size_t i = 0; i < m_pSource->m_Streams.size(); i++)
        {
            if (m_pSource->m_Streams[i].ffIndex == pkt->stream_index)
                pTarget = m_pSource->m_Streams[i].pStream;
        }
        if (pTarget == NULL)
        {
            av_packet_free(&pkt);
            continue;                       // not a stream we publish
        }

        AVStream* st = m_pSource->m_pFmt ? m_pSource->m_pFmt->streams[pkt->stream_index] : NULL;

        IMFMediaBuffer* pBuffer = NULL;
        IMFSample* pSample = NULL;
        do
        {
            if ((int)pkt->size <= 0)
            {
                hr = MF_E_INVALIDMEDIATYPE;
                break;
            }
            hr = MFCreateMemoryBuffer((DWORD)pkt->size, &pBuffer);
            if (FAILED(hr))
                break;
            BYTE* pb = NULL;
            if (FAILED(pBuffer->Lock(&pb, NULL, NULL)))
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            memcpy(pb, pkt->data, (size_t)pkt->size);
            pBuffer->Unlock();
            pBuffer->SetCurrentLength((DWORD)pkt->size);

            hr = MFCreateSample(&pSample);
            if (FAILED(hr))
                break;
            pSample->AddBuffer(pBuffer);

            LONGLONG rt = 0;
            LONGLONG rtSynthDur = 0;
            if (st != NULL && m_pSource->StreamHasTimestamps(m_ffIndex))
            {
                if (pkt->pts != (long long)AVS_NOPTS)
                    rt = m_pSource->ToHns(st->time_base, pkt->pts);
                else if (pkt->dts != (long long)AVS_NOPTS)
                    rt = m_pSource->ToHns(st->time_base, pkt->dts);
            }
            else if (st != NULL && st->codecpar != NULL &&
                     st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                // A raw elementary stream (.avs2 / .avs3 / .avs) has no time stamps at
                // all.  Leaving them at zero breaks the whole presentation: the sink
                // queues a hundred pictures for t = 0, the clock never moves, and the
                // player shows nothing.  Build the timeline from the frame rate the
                // probing found (25 fps is what AVS uses when nothing is known).
                const AVRational fr = m_pSource->StreamFrameRate(m_ffIndex);
                LONGLONG dur = 400000;
                if (fr.num > 0 && fr.den > 0)
                {
                    dur = (LONGLONG)av_rescale(fr.den, HNS_PER_SECOND, fr.num);
                    if (dur <= 0)
                        dur = 400000;
                }
                rt = m_rtSynth;
                m_rtSynth += dur;
                rtSynthDur = dur;
            }
            if (rt < 0)
                rt = 0;
            pSample->SetSampleTime(rt);
            if (AVS_TraceOn())
            {
                const int idx = (m_ffIndex >= 0 && m_ffIndex < 8) ? m_ffIndex : 0;
                const LONG n = InterlockedIncrement(&g_avsSampleCount[idx]);
                fprintf(stderr, "[avssrc] sample ff=%d #%ld time=%lld (%.3f s)\n", m_ffIndex,
                        (long)n, rt, (double)rt / (double)HNS_PER_SECOND);
            }
            if (pkt->duration > 0 && st != NULL && st->time_base.num > 0 && st->time_base.den > 0)
            {
                AVRational hns;
                hns.num = 1;
                hns.den = (int)HNS_PER_SECOND;
                pSample->SetSampleDuration((LONGLONG)av_rescale_q(pkt->duration, st->time_base, hns));
            }
            else if (rtSynthDur > 0)
                pSample->SetSampleDuration(rtSynthDur);
        } while (false);

        SAFE_RELEASE(pBuffer);
        bool bOwn = (pTarget == this);
        av_packet_free(&pkt);

        if (FAILED(hr))
        {
            SAFE_RELEASE(pSample);
            if (bOwn)
                return hr;
            continue;
        }

        if (bOwn)
        {
            AttachToken(pSample, pToken);
            HRESULT hr2 = QueueSample(pSample);
            SAFE_RELEASE(pSample);
            if (AVS_TraceOn() && (InterlockedIncrement(&s_nServed) % 50) == 0)
                fprintf(stderr, "[avssrc] served %d sample(s)\n", (int)s_nServed);
            return hr2;
        }

        // somebody else's access unit: keep it for that stream's next request.  Do not
        // post it into the event queue: a media session does not expect more than a few
        // samples it did not ask for, and dropping it would lose data.
        if (pTarget->PendingCount() + pTarget->BufferedCount() >= AVS_MAX_PENDING_SAMPLES)
        {
            if (AVS_TraceOn())
                fprintf(stderr, "[avssrc] stream ff=%d backlog is %zu, one sample dropped\n",
                        pTarget->FFIndex(), pTarget->PendingCount());
            SAFE_RELEASE(pSample);
            continue;
        }
        pTarget->PushPending(pSample);
        SAFE_RELEASE(pSample);
    }
}

// ===========================================================================
// CAVSByteStreamHandler - how a player reaches the source when it opens a file
// ===========================================================================
CAVSByteStreamHandler::CAVSByteStreamHandler()
    : m_RefCount(1), m_pObject(NULL), m_CreateHR(MF_E_NOT_INITIALIZED)
{
}

CAVSByteStreamHandler::~CAVSByteStreamHandler()
{
    SAFE_RELEASE(m_pObject);
}

HRESULT CAVSByteStreamHandler::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;
    *ppvObject = NULL;
    if (pUnkOuter != NULL)
        return CLASS_E_NOAGGREGATION;

    CAVSByteStreamHandler* pH = new (std::nothrow) CAVSByteStreamHandler;
    if (pH == NULL)
        return E_OUTOFMEMORY;
    HRESULT hr = pH->QueryInterface(riid, ppvObject);
    pH->Release();
    return hr;
}

STDMETHODIMP_(ULONG) CAVSByteStreamHandler::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CAVSByteStreamHandler::Release()
{
    LONG lRef = InterlockedDecrement(&m_RefCount);
    if (lRef == 0)
        delete this;
    return (ULONG)lRef;
}

STDMETHODIMP CAVSByteStreamHandler::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFByteStreamHandler))
    {
        *ppvObject = static_cast<IMFByteStreamHandler*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

STDMETHODIMP CAVSByteStreamHandler::GetMaxNumberOfBytesRequiredForResolution(QWORD* pqwBytes)
{
    if (pqwBytes == NULL)
        return E_POINTER;
    *pqwBytes = 0;                        // we read what we need, on demand
    return S_OK;
}

STDMETHODIMP CAVSByteStreamHandler::BeginCreateObject(IMFByteStream* pByteStream, LPCWSTR pwszURL,
                                                     DWORD dwFlags, IPropertyStore* pProps,
                                                     IUnknown** ppIUnknownCancelCookie,
                                                     IMFAsyncCallback* pCallback,
                                                     IUnknown* punkState)
{
    UNREFERENCED_PARAMETER(pProps);
    UNREFERENCED_PARAMETER(dwFlags);

    if (pByteStream == NULL || pCallback == NULL)
        return E_POINTER;

    AVS_Trace("BeginCreateObject called", (HRESULT)dwFlags);
    if (AVS_TraceOn() && pwszURL)
        fprintf(stderr, "[avssrc]   url=%s\n", Narrow(pwszURL).c_str());

    // QI the private interface rather than casting: IAVSByteDataSource is not the first
    // base of CAVSMediaSource, so the pointer for that interface is not the object.
    IAVSByteDataSource* pDS = NULL;
    HRESULT hr = CAVSMediaSource::CreateInstance(NULL, IID_IAVSByteDataSource, (void**)&pDS);
    if (SUCCEEDED(hr))
        hr = pDS->SetByteStream(pByteStream);

    if (SUCCEEDED(hr))
    {
        // tell the source which demuxer to use, from the file name
        std::string hint = DemuxHintFromExtension(pwszURL);
        if (!hint.empty())
        {
            IMFAttributes* pAttrs = NULL;
            IMFMediaSourceEx* pEx = NULL;
            // the source's own attribute store (QueryInterface for IMFAttributes on the
            // source is not supported - the store is reached through IMFMediaSourceEx)
            if (SUCCEEDED(pDS->QueryInterface(IID_PPV_ARGS(&pEx))) && pEx != NULL &&
                SUCCEEDED(pEx->GetSourceAttributes(&pAttrs)) && pAttrs != NULL)
            {
                std::wstring w((size_t)hint.size(), 0);
                for (size_t i = 0; i < hint.size(); i++)
                    w[i] = (WCHAR)hint[i];
                pAttrs->SetString(AVS_SOURCE_ATTRIBUTE_DEMUX_HINT, w.c_str());
                SAFE_RELEASE(pAttrs);
            }
            SAFE_RELEASE(pEx);
        }
        hr = pDS->Open();

        // ---- the claim rule ---------------------------------------------------------------
        // Take the bytes when the content is AVS-family video or DRA audio, or when the file
        // carries an extension we register for (that name asked for our pipeline, and our
        // measurement controls live on it).  Everything else goes back with
        // MF_E_UNSUPPORTED_BYTESTREAM_TYPE so the system's own handler and decoders keep
        // standard content: measured today we take standard H.264 in MPEG-TS and a standard
        // MP4, and the MP4 comes back described as video/AVS 28x8192 with zero frames.
        // scripts/claim-contract-test.py (regression 11m) is the judge of this rule.
        if (SUCCEEDED(hr))
        {
            BOOL bMine = FALSE;
            if (FAILED(pDS->IsAvsOrDra(&bMine)))
                bMine = FALSE;                      // cannot tell: only the name can claim
            if (!bMine && !AvsOwnExtension(ExtensionFromUrl(pwszURL)))
            {
                hr = MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
                if (AVS_TraceOn())
                    fprintf(stderr, "[avssrc] no AVS/DRA stream and the extension is not "
                                    "ours: handing the bytes back\n");
            }
        }
    }

    IMFMediaSource* pSource = NULL;
    if (SUCCEEDED(hr))
        hr = pDS->QueryInterface(IID_PPV_ARGS(&pSource));
    SAFE_RELEASE(pDS);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pSource);
    }
    else
    {
        SAFE_RELEASE(m_pObject);
        m_pObject = static_cast<IUnknown*>(static_cast<IMFMediaSource*>(pSource));
        m_pObject->AddRef();
        SAFE_RELEASE(pSource);
        m_CreateHR = S_OK;
    }

    if (ppIUnknownCancelCookie != NULL)
        *ppIUnknownCancelCookie = NULL;

    AVS_Trace("BeginCreateObject result", hr);

    IMFAsyncResult* pResult = NULL;
    if (SUCCEEDED(MFCreateAsyncResult(NULL, pCallback, punkState, &pResult)))
    {
        pResult->SetStatus(hr);
        pCallback->Invoke(pResult);
        SAFE_RELEASE(pResult);
    }
    return hr;
}

STDMETHODIMP CAVSByteStreamHandler::EndCreateObject(IMFAsyncResult* pResult,
                                                   MF_OBJECT_TYPE* pObjectType,
                                                   IUnknown** ppObject)
{
    if (pResult == NULL || pObjectType == NULL || ppObject == NULL)
        return E_POINTER;

    HRESULT hr = pResult->GetStatus();
    *pObjectType = MF_OBJECT_MEDIASOURCE;
    *ppObject = NULL;

    if (SUCCEEDED(hr) && m_pObject != NULL)
    {
        m_pObject->AddRef();
        *ppObject = m_pObject;
    }
    return hr;
}

STDMETHODIMP CAVSByteStreamHandler::CancelObjectCreation(IUnknown* pIUnknownCancelCookie)
{
    UNREFERENCED_PARAMETER(pIUnknownCancelCookie);
    return S_OK;
}
