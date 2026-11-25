//----------------------------------------------------------------------------------------------
// AVSVideoDecoderMFT.cpp - construction, streaming topology and media type negotiation.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "classGuids.h"
#include "AVSVideoDecoderMFT.h"
#include "AvsColor.h"
#include "AvsTypeDump.h"

// Output formats this decoder offers.  Everything is produced from the planar
// YUV420P that libavcodec outputs, so packed YUV is what an EVR / D3D presenter wants.
// P010 comes last on purpose: a sink that does not know what to ask for gets the 8 bit
// format, and a sink that can carry 10 bit samples (a D3D11 video processor, a 10 bit
// swap chain) asks for it by name.
static const GUID kOutputSubtypes[] = {
    MFVideoFormat_NV12,
    MFVideoFormat_I420,
    MFVideoFormat_YV12,
    MFVideoFormat_P010,
};

// Bytes per sample for our own output formats; P010/P016 are 16 bit per component.
static UINT32 AvsBytesPerSample(const GUID& subtype)
{
    return (subtype == MFVideoFormat_P010 || subtype == MFVideoFormat_P016) ? 2u : 1u;
}

//-------------------------------------------------------------------------------------------------
// The sample layout the decoder produces, stated in one place.  FFmpegDecoder.cpp
// (Convert) follows the same rules, and MF_MT_DEFAULT_STRIDE, MF_MT_SAMPLE_SIZE and
// MFT_OUTPUT_STREAM_INFO::cbSize all have to agree with the buffers that are really
// handed out:
//   * interleaved formats (NV12, P010): one chroma plane with the luma byte pitch;
//   * planar formats (I420, YV12): two half-width chroma planes with a half-width
//     pitch, which is what a consumer assumes when it only reads MF_MT_DEFAULT_STRIDE.
//-------------------------------------------------------------------------------------------------
struct AvsSampleLayout
{
    UINT32 stride;         // luma row pitch, in bytes
    UINT32 chromaStride;   // row pitch of a chroma plane
    BOOL   bInterleaved;
    UINT32 cbSize;         // one whole sample, in bytes
};

static AvsSampleLayout AvsLayoutFor(const GUID& subtype, UINT32 width, UINT32 height)
{
    AvsSampleLayout L;
    L.stride = 0;
    L.chromaStride = 0;
    L.bInterleaved = FALSE;
    L.cbSize = 0;
    if (width == 0 || height == 0)
        return L;

    const UINT32 bps = AvsBytesPerSample(subtype);
    L.stride = (bps == 2) ? ((width * 2 + 15) & ~15u) : ((width + 3) & ~3u);
    L.bInterleaved = (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_P010)
                        ? TRUE : FALSE;
    L.chromaStride = (L.bInterleaved == TRUE) ? L.stride
                        : (((width + 1) / 2 + 3) & ~3u);

    const UINT32 chH = (height + 1) / 2;
    L.cbSize = L.stride * height +
               ((L.bInterleaved == TRUE) ? L.stride * chH : L.chromaStride * chH * 2);
    return L;
}

//-------------------------------------------------------------------------------------------------
// CAVSVideoDecoderMFT::CreateInstance
HRESULT CAVSVideoDecoderMFT::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;

    CAVSVideoDecoderMFT* pAVS = new (std::nothrow) CAVSVideoDecoderMFT();
    if (pAVS == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = pAVS->QueryInterface(riid, ppvObject);
    SAFE_RELEASE(pAVS);
    return hr;
}

//-------------------------------------------------------------------------------------------------
CAVSVideoDecoderMFT::CAVSVideoDecoderMFT()
    : m_RefCount(1)
    , m_nInputStreamCount(1)
    , m_nOutputStreamCount(1)
    , m_InputStreamID(0)
    , m_OutputStreamID(0)
    , m_pInputType(NULL)
    , m_pOutputType(NULL)
    , m_pAttributes(NULL)
    , m_pEventQueue(NULL)
    , m_bAsync(FALSE)
    , m_bWorkerRunning(FALSE)
    , m_bWorkerStop(FALSE)
    , m_bDrainQueued(false)
    , m_bDrain(FALSE)
    , m_bStreamChangePending(FALSE)
    , m_bColorChangeSignalled(FALSE)
    , m_bInputStreamActivated(FALSE)
    , m_bOutputStreamActivated(FALSE)
    , m_CodecId(AV_CODEC_ID_NONE)
    , m_OutputFormat(AV_PIX_FMT_NONE)
    , m_OutputSubtype(MFVideoFormat_NV12)
    , m_bDecoderFlushed(FALSE)
    , m_bOutputStreamEnd(FALSE)
{
    // Synchronous by default.  AVS_MFT_ASYNC=1 makes the MFT advertise
    // MF_TRANSFORM_ASYNC, run the decoding on a worker thread and post the
    // asynchronous MFT notifications instead of letting the pipeline poll.
    const char* pszAsync = getenv("AVS_MFT_ASYNC");
    if (pszAsync != NULL && atoi(pszAsync) != 0)
        m_bAsync = TRUE;
}

CAVSVideoDecoderMFT::~CAVSVideoDecoderMFT()
{
    // the decode worker has to be gone before the decoder and the queues are
    if (m_bWorkerRunning)
    {
        {
            std::lock_guard<std::mutex> lk(m_AsyncLock);
            m_bWorkerStop = TRUE;
        }
        m_AsyncCv.notify_all();
        if (m_Worker.joinable())
            m_Worker.join();
        m_bWorkerRunning = FALSE;
    }

    if (GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0)
        fprintf(stderr, "[avsmft] ~video decoder this=%p\n", (void*)this);
    ClearOutputQueue();
    DestroyDecoder();
    SAFE_RELEASE(m_pInputType);
    SAFE_RELEASE(m_pOutputType);
    SAFE_RELEASE(m_pAttributes);
    SAFE_RELEASE(m_pEventQueue);
}

//-------------------------------------------------------------------------------------------------
// IUnknown
STDMETHODIMP CAVSVideoDecoderMFT::QueryInterface(REFIID iid, void** ppv)
{
    if (ppv == NULL)
        return E_POINTER;

    if (iid == IID_IUnknown || iid == __uuidof(IMFTransform))
    {
        *ppv = static_cast<IMFTransform*>(this);
        AddRef();
        return S_OK;
    }
    // An asynchronous MFT has to expose this; the media session would otherwise refuse
    // the topology as soon as MF_TRANSFORM_ASYNC is advertised.
    if (iid == __uuidof(IMFMediaEventGenerator))
    {
        *ppv = static_cast<IMFMediaEventGenerator*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CAVSVideoDecoderMFT::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CAVSVideoDecoderMFT::Release()
{
    ULONG count = (ULONG)InterlockedDecrement(&m_RefCount);
    if (count < 1 && GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0)
        fprintf(stderr, "[avsmft] video decoder this=%p released to %d\n", (void*)this, (int)count);
    if (count == 0)
        delete this;
    return count;
}

//-------------------------------------------------------------------------------------------------
// Topology: exactly one input stream and one output stream.
STDMETHODIMP CAVSVideoDecoderMFT::GetStreamLimits(DWORD* pdwInputMinimum, DWORD* pdwInputMaximum,
                                                  DWORD* pdwOutputMinimum, DWORD* pdwOutputMaximum)
{
    if (pdwInputMinimum == NULL || pdwInputMaximum == NULL ||
        pdwOutputMinimum == NULL || pdwOutputMaximum == NULL)
        return E_POINTER;

    *pdwInputMinimum = 1;
    *pdwInputMaximum = 1;
    *pdwOutputMinimum = 1;
    *pdwOutputMaximum = 1;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams)
{
    if (pcInputStreams == NULL || pcOutputStreams == NULL)
        return E_POINTER;

    *pcInputStreams = m_nInputStreamCount;
    *pcOutputStreams = m_nOutputStreamCount;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetStreamIDs(DWORD dwInputIDArraySize, DWORD* pdwInputIDs,
                                               DWORD dwOutputIDArraySize, DWORD* pdwOutputIDs)
{
    if (dwInputIDArraySize < m_nInputStreamCount || pdwInputIDs == NULL)
        return E_INVALIDARG;
    if (dwOutputIDArraySize < m_nOutputStreamCount || pdwOutputIDs == NULL)
        return E_INVALIDARG;

    pdwInputIDs[0] = m_InputStreamID;
    pdwOutputIDs[0] = m_OutputStreamID;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetInputStreamInfo(DWORD dwInputStreamID,
                                                     MFT_INPUT_STREAM_INFO* pStreamInfo)
{
    if (pStreamInfo == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    // The compressed sample is copied into libavcodec inside ProcessInput, so the
    // sample itself does not have to stay alive afterwards.
    pStreamInfo->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES
        | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
        | MFT_INPUT_STREAM_DOES_NOT_ADDREF;

    // Decoder delay, in 100ns units: the reorder buffer plus the output queue can hold
    // several pictures.  (Measured: the value does not change what the media session
    // does at the end of a presentation, but an honest bound is better than zero.)
    pStreamInfo->hnsMaxLatency = 5000000;   // 0.5 s
    pStreamInfo->cbSize = 0;
    pStreamInfo->cbAlignment = 0;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetOutputStreamInfo(DWORD dwOutputStreamID,
                                                      MFT_OUTPUT_STREAM_INFO* pStreamInfo)
{
    if (pStreamInfo == NULL)
        return E_POINTER;
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    pStreamInfo->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES
        | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
        | MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;

    pStreamInfo->cbAlignment = 0;
    pStreamInfo->cbSize = 0;
    if (m_pOutputType != NULL)
    {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(m_pOutputType, MF_MT_FRAME_SIZE, &w, &h);
        if (w && h)
        {
            GUID sub = GUID_NULL;
            m_pOutputType->GetGUID(MF_MT_SUBTYPE, &sub);
            const AvsSampleLayout lay = AvsLayoutFor(sub, w, h);
            UINT32 stride = MFGetAttributeUINT32(m_pOutputType, MF_MT_DEFAULT_STRIDE, 0);
            if (stride == 0)
                stride = lay.stride;
            UINT32 cbType = MFGetAttributeUINT32(m_pOutputType, MF_MT_SAMPLE_SIZE, 0);
            pStreamInfo->cbSize = (cbType != 0) ? cbType : lay.cbSize;
            pStreamInfo->dwFlags |= MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE;
        }
    }
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// Attributes: tell the pipeline that this decoder may change the frame size in band.
STDMETHODIMP CAVSVideoDecoderMFT::GetAttributes(IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);

    if (m_pAttributes == NULL)
    {
        IF_FAILED_RETURN(MFCreateAttributes(&m_pAttributes, 2));
        // Whether the output format may change while the stream runs.  A raw AVS
        // elementary stream has no dimensions until the sequence header is decoded, so
        // this is normally required.  AVS_MFT_DYNAMIC_FORMAT=0 turns it off for
        // experiments: the Media Session then has a fixed decoder output type and posts
        // MESessionStreamSinkFormatChanged (see the raw session test).
        UINT32 bDynamic = 1;
        if (const char* e = getenv("AVS_MFT_DYNAMIC_FORMAT"))
            bDynamic = (UINT32)atoi(e);
        m_pAttributes->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, bDynamic);

        // The asynchronous model: MF reads these two attributes right after activating
        // the MFT and, if async is claimed, QIs the MFT for IMFMediaEventGenerator and
        // drives it through the event queue instead of polling GetOutputStatus.
        if (m_bAsync)
        {
            m_pAttributes->SetUINT32(MF_TRANSFORM_ASYNC, TRUE);
            // MF_TRANSFORM_ASYNC_UNLOCK would let the pipeline call us from several
            // threads at once; the decoder wrapper is not free threaded, so the
            // pipeline keeps holding its stream lock around the calls.
            m_pAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, FALSE);
        }

        // MFTransformCategory is not declared in this Windows SDK, so its GUID is
        // taken from the documentation ({FAFD790B-F55E-432B-B6B6-26E4577AFBB9}).
        // MFTRegister() stores the category in the registry; putting it on the instance
        // as well makes the transform look like a decoder node to a topology loader
        // that loaded it directly instead of enumerating it.
        static const GUID AVS_MFT_ATTR_TRANSFORM_CATEGORY =
            { 0xFAFD790B, 0xF55E, 0x432B, { 0xB6, 0xB6, 0x26, 0xE4, 0x57, 0x7A, 0xFB, 0xB9 } };
        m_pAttributes->SetGUID(AVS_MFT_ATTR_TRANSFORM_CATEGORY, MFT_CATEGORY_VIDEO_DECODER);
    }

    *ppAttributes = m_pAttributes;
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetInputStreamAttributes(DWORD dwInputStreamID,
                                                          IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;
    *ppAttributes = NULL;
    return MF_E_INVALIDSTREAMNUMBER;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetOutputStreamAttributes(DWORD dwOutputStreamID,
                                                           IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);

    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    if (m_pOutputType == NULL)
    {
        // The pipeline asks for the stream attributes to build the output stream.
        // Until an input type is known offer a plain NV12 type.
        IF_FAILED_RETURN(CreateOutputMediaType(0, 0, &m_pOutputType));
    }

    *ppAttributes = m_pOutputType;
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::DeleteInputStream(DWORD /*dwStreamID*/)
{
    return MF_E_INVALIDSTREAMNUMBER;
}

STDMETHODIMP CAVSVideoDecoderMFT::AddInputStreams(DWORD /*cStreams*/, DWORD* /*adwStreamIDs*/)
{
    return MF_E_INVALIDSTREAMNUMBER;
}

//-------------------------------------------------------------------------------------------------
// Media type helpers
// Only the attribution measurement in plan/avs-mf-playback.md runs a standard format
// through this decoder (as an explicit topology node).  Off by default, so the
// architecture decision stays: H.264 / HEVC and friends are decoded by the system MFTs,
// and the topology loader can never start picking this MFT for them.
static bool AvsMftAllowH264()
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("AVS_MFT_ALLOW_H264") != NULL) ? 1 : 0;
    return cached == 1;
}

bool CAVSVideoDecoderMFT::IsInputSubtype(const GUID& subtype)
{
    if (subtype == MFVideoFormat_AVS
        || subtype == MFVideoFormat_AVS2
        || subtype == MFVideoFormat_AVS3)
        return true;
    // the same codec under a GUID outside MF's FourCC layout (measurement only)
    if (subtype == MFVideoFormat_AVS_ALT)
        return true;
    // AVSMediaSource publishes H.264 in MPEG-TS as the elementary-stream subtype, which
    // is what the system H.264 decoder also accepts for that path.
    return (subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES)
        && AvsMftAllowH264();
}

HRESULT CAVSVideoDecoderMFT::SubtypeToCodec(const GUID& subtype, AVCodecID& codecId)
{
    if (subtype == MFVideoFormat_AVS || subtype == MFVideoFormat_AVS_ALT)
        codecId = AV_CODEC_ID_CAVS;        // AVS1-P2 / AVS1-P16 (AVS+), libcavs decoder
    else if (subtype == MFVideoFormat_AVS2)
        codecId = AV_CODEC_ID_AVS2;        // libdavs2 / libxavs2 wrapper
    else if (subtype == MFVideoFormat_AVS3)
        codecId = AV_CODEC_ID_AVS3;        // libuavs3d wrapper
    else if ((subtype == MFVideoFormat_H264 || subtype == MFVideoFormat_H264_ES) &&
             AvsMftAllowH264())
        codecId = AV_CODEC_ID_H264;        // measurement only, see IsInputSubtype()
    else
        return MF_E_INVALIDMEDIATYPE;
    return S_OK;
}

HRESULT CAVSVideoDecoderMFT::SubtypeToOutputFormat(const GUID& subtype, AVPixelFormat& fmt,
                                                   GUID& outSubtype)
{
    if (subtype == MFVideoFormat_NV12)
    {
        fmt = AV_PIX_FMT_NV12;
        outSubtype = MFVideoFormat_NV12;
        return S_OK;
    }
    if (subtype == MFVideoFormat_I420)
    {
        fmt = AV_PIX_FMT_YUV420P;
        outSubtype = MFVideoFormat_I420;
        return S_OK;
    }
    if (subtype == MFVideoFormat_YV12)
    {
        fmt = AV_PIX_FMT_YUV420P;
        outSubtype = MFVideoFormat_YV12;
        return S_OK;
    }
    if (subtype == MFVideoFormat_P010)
    {
        // 10 bit samples, the useful bits in the high part of each 16 bit value -
        // which is exactly FFmpeg's p010le
        fmt = AV_PIX_FMT_P010;
        outSubtype = MFVideoFormat_P010;
        return S_OK;
    }
    return MF_E_INVALIDMEDIATYPE;
}

HRESULT CAVSVideoDecoderMFT::CloneMediaType(IMFMediaType* pSource, IMFMediaType** ppOut)
{
    IMFMediaType* pClone = NULL;
    IF_FAILED_RETURN(MFCreateMediaType(&pClone));
    HRESULT hr = pSource->CopyAllItems(pClone);
    if (FAILED(hr))
    {
        SAFE_RELEASE(pClone);
        return hr;
    }
    *ppOut = pClone;
    return S_OK;
}

HRESULT CAVSVideoDecoderMFT::ValidateInputType(IMFMediaType* pType, GUID* pSubtypeOut)
{
    if (pType == NULL)
        return MF_E_INVALIDMEDIATYPE;

    GUID major = GUID_NULL;
    if (FAILED(pType->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video)
        return MF_E_INVALIDMEDIATYPE;

    GUID subtype = GUID_NULL;
    if (FAILED(pType->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;

    if (!IsInputSubtype(subtype))
    {
        if (GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0)
            fprintf(stderr, "[avsmft] input type rejected subtype=%08X allow_h264=%d\n",
                    (unsigned)subtype.Data1, AvsMftAllowH264() ? 1 : 0);
        return MF_E_INVALIDMEDIATYPE;
    }

    // The picture size does not have to be known: AVS carries its sequence header in
    // the bit stream and the MFT reports the size through a stream change.
    if (pSubtypeOut != NULL)
        *pSubtypeOut = subtype;
    return S_OK;
}

HRESULT CAVSVideoDecoderMFT::ValidateOutputType(IMFMediaType* pType, AVPixelFormat* pFormatOut,
                                                GUID* pSubtypeOut)
{
    if (pType == NULL)
        return MF_E_INVALIDMEDIATYPE;

    GUID major = GUID_NULL;
    if (FAILED(pType->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video)
        return MF_E_INVALIDMEDIATYPE;

    GUID subtype = GUID_NULL;
    if (FAILED(pType->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return MF_E_INVALIDMEDIATYPE;

    AVPixelFormat fmt = AV_PIX_FMT_NONE;
    GUID outSub = GUID_NULL;
    IF_FAILED_RETURN(SubtypeToOutputFormat(subtype, fmt, outSub));

    if (pFormatOut)
        *pFormatOut = fmt;
    if (pSubtypeOut)
        *pSubtypeOut = outSub;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetInputAvailableType(DWORD dwInputStreamID, DWORD dwTypeIndex,
                                                        IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    // The topology loader decides whether a transform can serve a stream by enumerating
    // these types, not by calling SetInputType, so a subtype that IsInputSubtype() accepts
    // must appear here too - otherwise the loader answers MF_E_TOPO_CODEC_NOT_FOUND.
    GUID inputSubtypes[6] = { MFVideoFormat_AVS, MFVideoFormat_AVS2, MFVideoFormat_AVS3,
                              GUID_NULL, GUID_NULL, GUID_NULL };
    size_t count = 3;
    if (GetEnvironmentVariableW(L"AVS_MFT_SUBTYPE_SHAPE", nullptr, 0) > 0)
        inputSubtypes[count++] = MFVideoFormat_AVS_ALT;   // measurement only
    if (AvsMftAllowH264())
    {
        inputSubtypes[count++] = MFVideoFormat_H264;      // attribution measurement only
        inputSubtypes[count++] = MFVideoFormat_H264_ES;   // what our source actually sends
    }

    if (dwTypeIndex == 0 && GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0)
        fprintf(stderr, "[avsmft] GetInputAvailableType count=%zu allow_h264=%d\n",
                count, AvsMftAllowH264() ? 1 : 0);
    if (dwTypeIndex >= count)
        return MF_E_NO_MORE_TYPES;

    IMFMediaType* pType = NULL;
    IF_FAILED_RETURN(MFCreateMediaType(&pType));
    pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pType->SetGUID(MF_MT_SUBTYPE, inputSubtypes[dwTypeIndex]);
    *ppType = pType;
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetOutputAvailableType(DWORD dwOutputStreamID, DWORD dwTypeIndex,
                                                         IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    if (dwTypeIndex >= ARRAY_SIZE(kOutputSubtypes))
        return MF_E_NO_MORE_TYPES;

    AutoLock lock(m_Lock);

    if (m_pInputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    // The output frame size mirrors the input when the source knew it; otherwise it
    // stays 0x0 and the real size arrives with the MF_E_TRANSFORM_STREAM_CHANGE that
    // is raised once the first AVS picture has been decoded.
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(m_pInputType, MF_MT_FRAME_SIZE, &w, &h);
    if ((w == 0 || h == 0) && m_Decoder.IsOpen())
    {
        w = m_Decoder.Width();
        h = m_Decoder.Height();
    }

    // The sub-type is only exchanged for this call; the decoder keeps using whatever
    // the pipeline has actually set with SetOutputType.
    GUID wanted = kOutputSubtypes[dwTypeIndex];
    GUID current = m_OutputSubtype;
    m_OutputSubtype = wanted;

    IMFMediaType* pType = NULL;
    HRESULT hr = CreateOutputMediaType(w, h, &pType);

    m_OutputSubtype = current;
    if (FAILED(hr))
        return hr;

    *ppType = pType;
    return S_OK;
}

HRESULT CAVSVideoDecoderMFT::CreateOutputMediaType(UINT32 width, UINT32 height, IMFMediaType** ppType)
{
    IMFMediaType* pType = NULL;
    IF_FAILED_RETURN(MFCreateMediaType(&pType));

    if (FAILED(pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(pType->SetGUID(MF_MT_SUBTYPE, m_OutputSubtype)))
    {
        SAFE_RELEASE(pType);
        return E_OUTOFMEMORY;
    }

    if (width && height)
    {
        const AvsSampleLayout lay = AvsLayoutFor(m_OutputSubtype, width, height);
        const UINT32 stride = lay.stride;
        MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
        pType->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
        pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        // pixel aspect ratio: whatever the source states, otherwise square pixels.
        // The AVS sequence header has a display aspect ratio of its own; reading it is
        // not done yet, see the note on geometry in README.md.
        UINT32 parN = 0, parD = 0;
        if (m_pInputType != NULL)
            MFGetAttributeRatio(m_pInputType, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD);
        if (parN == 0 || parD == 0)
            parN = parD = 1;
        MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, parN, parD);

        // An uncompressed video type is expected to describe the sample layout.  The
        // MFT hands out its own samples (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES), but the
        // sink still negotiates the format, so state the size the way a system decoder
        // does: one plane of luma plus two half-height chroma planes, all on the
        // aligned stride.
        pType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        pType->SetUINT32(MF_MT_SAMPLE_SIZE, lay.cbSize);
        // MF_MT_COMPRESSED = FALSE is what a system video decoder puts on its
        // uncompressed output type; it is accepted here.
        pType->SetUINT32(MF_MT_COMPRESSED, FALSE);
        // MF_MT_AVG_BIT_ERROR_RATE is deliberately NOT set.  Giving it (VT_R4 through
        // IMFAttributes::SetItem) makes the topology loader answer
        // MF_E_INVALIDTYPE (0xC00D36BD) and no session can be started at all - verified
        // by bisection against the working AVS+ graph.

        UINT32 num = 0, den = 1;
        if (m_pInputType != NULL)
            MFGetAttributeRatio(m_pInputType, MF_MT_FRAME_RATE, &num, &den);
        if (num == 0 || den == 0)
            num = 25, den = 1;
        MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(pType, MF_MT_FRAME_RATE_RANGE_MIN, num, den);
        MFSetAttributeRatio(pType, MF_MT_FRAME_RATE_RANGE_MAX, num, den);

        // The colour signalling of the stream: whatever the source published on the
        // compressed type, overridden by what the bit stream itself declared.  Nothing
        // is set for a stream that says nothing (see AvsColor.h).
        AvsApplyColor(pType, m_Color);

        if (m_pInputType != NULL)
        {
            UINT32 cb = 0;
            if (SUCCEEDED(m_pInputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &cb)) && cb)
            {
                std::vector<BYTE> hdr(cb);
                if (SUCCEEDED(m_pInputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, &hdr[0], cb, NULL)))
                    pType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, &hdr[0], cb);
            }
        }
    }

    *ppType = pType;
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
HRESULT CAVSVideoDecoderMFT::CheckInputType(IMFMediaType* pType)
{
    AutoLock lock(m_Lock);
    return ValidateInputType(pType, NULL);
}

HRESULT CAVSVideoDecoderMFT::CheckOutputStreamType(IMFMediaType* pType)
{
    AutoLock lock(m_Lock);
    return ValidateOutputType(pType, NULL, NULL);
}

STDMETHODIMP CAVSVideoDecoderMFT::SetInputType(DWORD dwInputStreamID, IMFMediaType* pType,
                                               DWORD dwFlags)
{
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;
    if (dwFlags & ~MFT_SET_TYPE_TEST_ONLY)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);

    GUID subtype = GUID_NULL;
    if (pType != NULL)
    {
        IF_FAILED_RETURN(ValidateInputType(pType, &subtype));
    }

    if (dwFlags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    // measurement only: what the pipeline really handed us (see scripts/type-diff.py)
    AvsDumpType("mft-in", pType);

    // (Re)start the decoder for the new stream.
    ClearOutputQueue();
    DestroyDecoder();
    m_bDrain = FALSE;
    m_bStreamChangePending = FALSE;
    SAFE_RELEASE(m_pInputType);

    if (pType == NULL)
        return S_OK;

    IF_FAILED_RETURN(CloneMediaType(pType, &m_pInputType));
    IF_FAILED_RETURN(SubtypeToCodec(subtype, m_CodecId));

    // Start from what the source says about the sample values; the bit stream can add
    // to it later (NoteDecoderColor), and the decoder wins when the two disagree.
    m_Color = AvsColorInfo();
    AvsReadColor(m_pInputType, &m_Color);

    // Keep the output format the pipeline already agreed on, and inherit the frame
    // rate / aspect ratio from the input for the attributes we pass on.
    if (m_pOutputType != NULL)
    {
        GUID outSub = GUID_NULL;
        m_pOutputType->GetGUID(MF_MT_SUBTYPE, &outSub);
        IF_FAILED_RETURN(SubtypeToOutputFormat(outSub, m_OutputFormat, m_OutputSubtype));
    }
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::SetOutputType(DWORD dwOutputStreamID, IMFMediaType* pType,
                                                DWORD dwFlags)
{
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;
    if (dwFlags & ~MFT_SET_TYPE_TEST_ONLY)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);

    if (pType != NULL)
    {
        AVPixelFormat fmt = AV_PIX_FMT_NONE;
        GUID sub = GUID_NULL;
        IF_FAILED_RETURN(ValidateOutputType(pType, &fmt, &sub));
    }

    if (dwFlags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    AvsDumpType("mft-out-set", pType);

    if (!m_bStreamChangePending)
        ClearOutputQueue();
    // NOTE: when the change was signalled by the bit stream itself the queue holds the
    // picture that carried the new sequence header, already converted to the new size -
    // dropping it here would lose a frame.
    m_bStreamChangePending = FALSE;

    SAFE_RELEASE(m_pOutputType);

    if (pType == NULL)
        return S_OK;

    IF_FAILED_RETURN(CloneMediaType(pType, &m_pOutputType));

    AVPixelFormat fmt = AV_PIX_FMT_NONE;
    GUID sub = GUID_NULL;
    IF_FAILED_RETURN(ValidateOutputType(m_pOutputType, &fmt, &sub));
    m_OutputFormat = fmt;
    m_OutputSubtype = sub;

    // a decoder cannot accept an output size that differs from what the bit stream
    // says, and the pipeline may not have known it yet - that is handled through the
    // stream-change protocol, so nothing to verify here.
    return S_OK;
}

STDMETHODIMP CAVSVideoDecoderMFT::GetInputCurrentType(DWORD dwInputStreamID, IMFMediaType** ppType)
{
    if (dwInputStreamID != m_InputStreamID || ppType == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);
    if (m_pInputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    return CloneMediaType(m_pInputType, ppType);
}

STDMETHODIMP CAVSVideoDecoderMFT::GetOutputCurrentType(DWORD dwOutputStreamID, IMFMediaType** ppType)
{
    if (dwOutputStreamID != m_OutputStreamID || ppType == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);
    if (m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    return CloneMediaType(m_pOutputType, ppType);
}

//-------------------------------------------------------------------------------------------------
// Decoder instance management
HRESULT CAVSVideoDecoderMFT::EnsureDecoder()
{
    if (m_Decoder.IsOpen())
        return S_OK;
    if (m_pInputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (m_OutputFormat == AV_PIX_FMT_NONE)
    {
        m_OutputFormat = AV_PIX_FMT_NV12;
        m_OutputSubtype = MFVideoFormat_NV12;
    }

    // MF's YV12 is Cr-then-Cb; every AVS decoder is Cb-then-Cr
    m_Decoder.SetChromaVFirst(m_OutputSubtype == MFVideoFormat_YV12);

    UINT8* extra = NULL;
    UINT32 cbExtra = 0;
    UINT32 cb = 0;
    if (SUCCEEDED(m_pInputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &cb)) && cb > 0)
    {
        extra = new (std::nothrow) UINT8[cb];
        if (extra == NULL)
            return E_OUTOFMEMORY;
        if (FAILED(m_pInputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, extra, cb, NULL)))
        {
            delete[] extra;
            extra = NULL;
            cbExtra = 0;
        }
        else
        {
            cbExtra = cb;
        }
    }

    HRESULT hr = m_Decoder.Open(m_CodecId, extra, cbExtra, m_OutputFormat);
    delete[] extra;
    return hr;
}

void CAVSVideoDecoderMFT::DestroyDecoder()
{
    m_Decoder.Close();
}
