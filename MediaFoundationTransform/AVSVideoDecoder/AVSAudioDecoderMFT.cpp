//----------------------------------------------------------------------------------------------
// AVSAudioDecoderMFT.cpp - DRA audio decoder MFT.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "classGuids.h"
#include "AVSAudioDecoderMFT.h"
#include "AvsChannelMask.h"

#include <vector>

#define HNS_PER_SECOND 10000000LL

// trace only: AVS_MFT_TRACE=1 in the environment.  Handing decoded audio blocks to the
// graph is what proves that the DRA path is really inside the Media Foundation graph.
static bool AVSAudio_TraceOn(void)
{
    static int on = -1;
    if (on < 0)
        on = (GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0) ? 1 : 0;
    return on != 0;
}

// ---------------------------------------------------------------------------
// creation / destruction
// ---------------------------------------------------------------------------
CAVSAudioDecoderMFT::CAVSAudioDecoderMFT()
    : m_RefCount(1)
    , m_InputStreamID(0)
    , m_OutputStreamID(0)
    , m_pInputType(NULL)
    , m_pOutputType(NULL)
    , m_pAttributes(NULL)
    , m_bDrain(FALSE)
    , m_bStreamChangePending(FALSE)
    , m_CodecId(AV_CODEC_ID_NONE)
    , m_Rate(0)
    , m_Channels(0)
    , m_ChannelMask(0)
    , m_bMaskChangeSignalled(false)
    , m_bFloatOutput(false)
    , m_bDecoderFlushed(FALSE)
{
}

CAVSAudioDecoderMFT::~CAVSAudioDecoderMFT()
{
    ClearOutputQueue();
    DestroyDecoder();
    SAFE_RELEASE(m_pInputType);
    SAFE_RELEASE(m_pOutputType);
    SAFE_RELEASE(m_pAttributes);
}

HRESULT CAVSAudioDecoderMFT::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
    if (ppvObject == NULL)
        return E_POINTER;
    *ppvObject = NULL;
    if (pUnkOuter != NULL)
        return CLASS_E_NOAGGREGATION;

    CAVSAudioDecoderMFT* pMFT = new (std::nothrow) CAVSAudioDecoderMFT;
    if (pMFT == NULL)
        return E_OUTOFMEMORY;

    HRESULT hr = pMFT->QueryInterface(riid, ppvObject);
    pMFT->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
STDMETHODIMP_(ULONG) CAVSAudioDecoderMFT::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_RefCount);
}

STDMETHODIMP_(ULONG) CAVSAudioDecoderMFT::Release()
{
    LONG lRef = InterlockedDecrement(&m_RefCount);
    if (lRef == 0)
        delete this;
    return (ULONG)lRef;
}

STDMETHODIMP CAVSAudioDecoderMFT::QueryInterface(REFIID iid, void** ppv)
{
    if (ppv == NULL)
        return E_POINTER;

    if (iid == IID_IUnknown || iid == __uuidof(IMFTransform))
    {
        *ppv = static_cast<IMFTransform*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// stream topology: exactly one compressed input, one PCM output
// ---------------------------------------------------------------------------
STDMETHODIMP CAVSAudioDecoderMFT::GetStreamLimits(DWORD* pdwInputMinimum, DWORD* pdwInputMaximum,
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

STDMETHODIMP CAVSAudioDecoderMFT::GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams)
{
    if (pcInputStreams == NULL || pcOutputStreams == NULL)
        return E_POINTER;
    *pcInputStreams = 1;
    *pcOutputStreams = 1;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetStreamIDs(DWORD dwInputIDArraySize, DWORD* pdwInputIDs,
                                               DWORD dwOutputIDArraySize, DWORD* pdwOutputIDs)
{
    UNREFERENCED_PARAMETER(dwInputIDArraySize);
    UNREFERENCED_PARAMETER(pdwInputIDs);
    UNREFERENCED_PARAMETER(dwOutputIDArraySize);
    UNREFERENCED_PARAMETER(pdwOutputIDs);
    return E_NOTIMPL;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetInputStreamInfo(DWORD dwInputStreamID,
                                                     MFT_INPUT_STREAM_INFO* pStreamInfo)
{
    if (pStreamInfo == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    pStreamInfo->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES
        | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER
        | MFT_INPUT_STREAM_DOES_NOT_ADDREF;
    pStreamInfo->hnsMaxLatency = 0;
    pStreamInfo->cbSize = 0;
    pStreamInfo->cbAlignment = 0;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetOutputStreamInfo(DWORD dwOutputStreamID,
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
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetAttributes(IMFAttributes** ppAttributes)
{
    if (ppAttributes == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);

    if (m_pAttributes == NULL)
    {
        HRESULT hr = MFCreateAttributes(&m_pAttributes, 2);
        IF_FAILED_RETURN(hr);
        hr = m_pAttributes->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, TRUE);
        IF_FAILED_RETURN(hr);
    }
    m_pAttributes->AddRef();
    *ppAttributes = m_pAttributes;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetInputStreamAttributes(DWORD dwInputStreamID,
                                                          IMFAttributes** ppAttributes)
{
    UNREFERENCED_PARAMETER(dwInputStreamID);
    UNREFERENCED_PARAMETER(ppAttributes);
    return E_POINTER;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetOutputStreamAttributes(DWORD dwOutputStreamID,
                                                           IMFAttributes** ppAttributes)
{
    UNREFERENCED_PARAMETER(dwOutputStreamID);
    UNREFERENCED_PARAMETER(ppAttributes);
    return E_POINTER;
}

STDMETHODIMP CAVSAudioDecoderMFT::DeleteInputStream(DWORD dwStreamID)
{
    UNREFERENCED_PARAMETER(dwStreamID);
    return E_INVALIDARG;
}

STDMETHODIMP CAVSAudioDecoderMFT::AddInputStreams(DWORD cStreams, DWORD* adwStreamIDs)
{
    UNREFERENCED_PARAMETER(cStreams);
    UNREFERENCED_PARAMETER(adwStreamIDs);
    return E_INVALIDARG;
}

// ---------------------------------------------------------------------------
// media types
// ---------------------------------------------------------------------------
static HRESULT CloneAudioType(IMFMediaType* pSource, IMFMediaType** ppOut)
{
    IMFMediaType* pNew = NULL;
    HRESULT hr = MFCreateMediaType(&pNew);
    IF_FAILED_RETURN(hr);

    hr = pSource->CopyAllItems(pNew);
    if (FAILED(hr))
        SAFE_RELEASE(pNew);
    else
        *ppOut = pNew;
    return hr;
}

bool CAVSAudioDecoderMFT::IsInputSubtype(const GUID& subtype)
{
    return subtype == MFAudioFormat_DRA;
}

HRESULT CAVSAudioDecoderMFT::SubtypeToCodec(const GUID& subtype, AVCodecID& codecId)
{
    if (subtype == MFAudioFormat_DRA)
    {
        codecId = AV_CODEC_ID_DRA;
        return S_OK;
    }
    return MF_E_INVALIDMEDIATYPE;
}

HRESULT CAVSAudioDecoderMFT::CheckInputType(IMFMediaType* pType)
{
    if (pType == NULL)
        return E_INVALIDARG;

    GUID major = GUID_NULL;
    HRESULT hr = pType->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || major != MFMediaType_Audio)
        return MF_E_INVALIDMEDIATYPE;

    GUID subtype = GUID_NULL;
    hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || !IsInputSubtype(subtype))
        return MF_E_INVALIDMEDIATYPE;

    return S_OK;
}

HRESULT CAVSAudioDecoderMFT::CheckOutputType(IMFMediaType* pType)
{
    if (pType == NULL)
        return E_INVALIDARG;

    GUID major = GUID_NULL;
    HRESULT hr = pType->GetGUID(MF_MT_MAJOR_TYPE, &major);
    if (FAILED(hr) || major != MFMediaType_Audio)
        return MF_E_INVALIDMEDIATYPE;

    GUID subtype = GUID_NULL;
    hr = pType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr) || (subtype != MFAudioFormat_PCM && subtype != MFAudioFormat_Float))
        return MF_E_INVALIDMEDIATYPE;

    UINT32 bits = MFGetAttributeUINT32(pType, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    if (subtype == MFAudioFormat_PCM && bits != 16)
        return MF_E_INVALIDMEDIATYPE;

    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetInputAvailableType(DWORD dwInputStreamID,
                                                        DWORD dwTypeIndex, IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    if (dwTypeIndex > 0)
        return MF_E_NO_MORE_TYPES;

    HRESULT hr = MFCreateMediaType(ppType);
    IF_FAILED_RETURN(hr);

    hr = (*ppType)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr))
        hr = (*ppType)->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_DRA);
    if (FAILED(hr))
        SAFE_RELEASE(*ppType);
    return hr;
}

HRESULT CAVSAudioDecoderMFT::CreateOutputMediaType(UINT32 rate, UINT32 channels, bool ieeeFloat,
                                                   IMFMediaType** ppType)
{
    if (rate == 0)
        rate = 48000;
    if (channels == 0)
        channels = 2;

    IMFMediaType* pType = NULL;
    HRESULT hr = MFCreateMediaType(&pType);
    IF_FAILED_RETURN(hr);

    const UINT32 bits = ieeeFloat ? 32 : 16;
    const UINT32 blockAlign = channels * (bits / 8);

    do
    {
        if (FAILED(hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)))
            break;
        if (FAILED(hr = pType->SetGUID(MF_MT_SUBTYPE, ieeeFloat ? MFAudioFormat_Float
                                                                : MFAudioFormat_PCM)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels)))
            break;
        // Name the speakers when the arrangement is known.  A mask that does not describe
        // exactly `channels` speakers would be worse than none, so it is checked here.
        if (m_ChannelMask != 0 && (UINT32)AvsCountSpeakers(m_ChannelMask) == channels)
        {
            if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, m_ChannelMask)))
                break;
        }
        if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                         (UINT32)rate * blockAlign)))
            break;
        if (!ieeeFloat && FAILED(hr = pType->SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, 16)))
            break;
        if (FAILED(hr = pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE)))
            break;
    } while (false);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pType);
        return hr;
    }
    *ppType = pType;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetOutputAvailableType(DWORD dwOutputStreamID,
                                                        DWORD dwTypeIndex, IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    // 0 = 16 bit PCM, 1 = 32 bit IEEE float
    if (dwTypeIndex >= 2)
        return MF_E_NO_MORE_TYPES;

    UINT32 rate = m_Rate, channels = m_Channels;

    // prefer whatever the input type told us
    if (m_pInputType != NULL)
    {
        UINT32 r = MFGetAttributeUINT32(m_pInputType, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        UINT32 c = MFGetAttributeUINT32(m_pInputType, MF_MT_AUDIO_NUM_CHANNELS, 0);
        if (r)
            rate = r;
        if (c)
            channels = c;
    }
    if (rate == 0)
        rate = (UINT32)m_Decoder.SampleRate();
    if (channels == 0)
        channels = (UINT32)m_Decoder.Channels();

    return CreateOutputMediaType(rate, channels, dwTypeIndex == 1, ppType);
}

STDMETHODIMP CAVSAudioDecoderMFT::SetInputType(DWORD dwInputStreamID, IMFMediaType* pType,
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
        IF_FAILED_RETURN(CheckInputType(pType));
        IF_FAILED_RETURN(pType->GetGUID(MF_MT_SUBTYPE, &subtype));
    }

    if (dwFlags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    ClearOutputQueue();
    DestroyDecoder();
    m_bDrain = FALSE;
    m_bStreamChangePending = FALSE;
    SAFE_RELEASE(m_pInputType);

    if (pType == NULL)
        return S_OK;

    IF_FAILED_RETURN(CloneAudioType(pType, &m_pInputType));
    IF_FAILED_RETURN(SubtypeToCodec(subtype, m_CodecId));

    // A container that probed the DRA stream tells us rate/channels here; when it
    // did not, the first decoded frame will.
    m_Rate = MFGetAttributeUINT32(m_pInputType, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    m_Channels = MFGetAttributeUINT32(m_pInputType, MF_MT_AUDIO_NUM_CHANNELS, 0);
    // A container that knows the arrangement names it (our own media source does, from
    // the demuxer's channel layout).  Otherwise the bit stream will, see
    // NoteDecodedChannelMask().
    m_ChannelMask = MFGetAttributeUINT32(m_pInputType, MF_MT_AUDIO_CHANNEL_MASK, 0);
    m_bMaskChangeSignalled = false;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::SetOutputType(DWORD dwOutputStreamID, IMFMediaType* pType,
                                                DWORD dwFlags)
{
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;
    if (dwFlags & ~MFT_SET_TYPE_TEST_ONLY)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);

    if (pType != NULL)
        IF_FAILED_RETURN(CheckOutputType(pType));

    if (dwFlags & MFT_SET_TYPE_TEST_ONLY)
        return S_OK;

    // keep the PCM that made us raise the stream change, it is already valid
    if (!m_bStreamChangePending)
        ClearOutputQueue();
    m_bStreamChangePending = FALSE;

    SAFE_RELEASE(m_pOutputType);

    if (pType == NULL)
        return S_OK;

    IF_FAILED_RETURN(CloneAudioType(pType, &m_pOutputType));

    GUID subtype = GUID_NULL;
    IF_FAILED_RETURN(m_pOutputType->GetGUID(MF_MT_SUBTYPE, &subtype));
    m_bFloatOutput = (subtype == MFAudioFormat_Float);

    UINT32 rate = MFGetAttributeUINT32(m_pOutputType, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    UINT32 ch = MFGetAttributeUINT32(m_pOutputType, MF_MT_AUDIO_NUM_CHANNELS, 0);
    if (rate)
        m_Rate = rate;
    if (ch)
        m_Channels = ch;
    // the type the host picked is now the truth, including its channel mask
    m_ChannelMask = MFGetAttributeUINT32(m_pOutputType, MF_MT_AUDIO_CHANNEL_MASK, 0);
    m_bMaskChangeSignalled = false;
    if (m_pOutputType != NULL && m_Rate && m_Channels)
        IF_FAILED_RETURN(UpdateOutputTypeForDecodedFormat(m_Rate, m_Channels));
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetInputCurrentType(DWORD dwInputStreamID, IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);
    if (m_pInputType == NULL)
        return MF_E_INVALIDMEDIATYPE;

    m_pInputType->AddRef();
    *ppType = m_pInputType;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetOutputCurrentType(DWORD dwOutputStreamID, IMFMediaType** ppType)
{
    if (ppType == NULL)
        return E_POINTER;
    if (dwOutputStreamID != m_OutputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);
    if (m_pOutputType == NULL)
        return MF_E_INVALIDMEDIATYPE;

    m_pOutputType->AddRef();
    *ppType = m_pOutputType;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetInputStatus(DWORD dwInputStreamID, DWORD* pdwFlags)
{
    if (pdwFlags == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (m_OutputQueue.size() >= AVS_AUDIO_MFT_MAX_QUEUED_FRAMES)
        return MF_E_NOTACCEPTING;

    *pdwFlags = MFT_INPUT_STATUS_ACCEPT_DATA;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::GetOutputStatus(DWORD* pdwFlags)
{
    if (pdwFlags == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *pdwFlags = m_OutputQueue.empty() ? 0 : MFT_OUTPUT_STATUS_SAMPLE_READY;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::SetOutputBounds(LONGLONG hnsLowerBound, LONGLONG hnsUpperBound)
{
    UNREFERENCED_PARAMETER(hnsLowerBound);
    UNREFERENCED_PARAMETER(hnsUpperBound);
    return E_NOTIMPL;
}

STDMETHODIMP CAVSAudioDecoderMFT::ProcessEvent(DWORD dwInputStreamID, IMFMediaEvent* pEvent)
{
    UNREFERENCED_PARAMETER(dwInputStreamID);
    if (pEvent == NULL)
        return E_INVALIDARG;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::ProcessMessage(MFT_MESSAGE_TYPE eMessage, ULONG_PTR ulParam)
{
    AutoLock lock(m_Lock);

    switch ((DWORD)eMessage)
    {
    case MFT_MESSAGE_COMMAND_FLUSH:
        ClearOutputQueue();
        DestroyDecoder();
        m_bDrain = FALSE;
        m_bStreamChangePending = FALSE;
        return S_OK;

    case MFT_MESSAGE_COMMAND_DRAIN:
        m_bDrain = TRUE;
        return FlushDecoderToQueue();

    case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
    case MFT_MESSAGE_NOTIFY_END_STREAMING:
    case MFT_MESSAGE_COMMAND_MARKER:
    case MFT_MESSAGE_DROP_SAMPLES:
        return S_OK;

    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        m_bDrain = TRUE;
        return FlushDecoderToQueue();

    case MFT_MESSAGE_SET_D3D_MANAGER:
        UNREFERENCED_PARAMETER(ulParam);
        return MF_E_INVALIDREQUEST;

    default:
        return MF_E_INVALIDREQUEST;
    }
}

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------
HRESULT CAVSAudioDecoderMFT::EnsureDecoder()
{
    if (m_Decoder.IsOpen())
        return S_OK;
    if (m_CodecId == AV_CODEC_ID_NONE)
        return MF_E_INVALIDMEDIATYPE;
    return m_Decoder.Open(m_CodecId);
}

void CAVSAudioDecoderMFT::DestroyDecoder()
{
    m_Decoder.Close();
}

void CAVSAudioDecoderMFT::ClearOutputQueue()
{
    for (size_t i = 0; i < m_OutputQueue.size(); i++)
        SAFE_RELEASE(m_OutputQueue[i]);
    m_OutputQueue.clear();
    m_FlushFrames.clear();
    m_bDecoderFlushed = FALSE;
}

// Blocks that do not fit into the output queue stay in m_FlushFrames, so that draining
// the decoder cannot lose the last audio blocks of a stream.
HRESULT CAVSAudioDecoderMFT::FlushDecoderToQueue()
{
    if (!m_Decoder.IsOpen())
        return S_OK;

    while (!m_FlushFrames.empty())
    {
        const HRESULT hrQ = QueueOutputFrame(m_FlushFrames.front());
        if (hrQ == MF_E_NOTACCEPTING)
            return S_OK;                    // the queue is full again, the rest waits
        if (FAILED(hrQ))
            return hrQ;
        m_FlushFrames.erase(m_FlushFrames.begin());
    }

    if (m_bDecoderFlushed)
        return S_OK;                        // the decoder was already drained

    std::vector<FFmpegAudioDecoder::Frame> frames;
    const HRESULT hr = m_Decoder.Flush(frames);
    m_bDecoderFlushed = TRUE;
    NoteDecodedChannelMask();

    size_t i = 0;
    for (; i < frames.size(); i++)
    {
        const HRESULT hrQ = QueueOutputFrame(frames[i]);
        if (hrQ == MF_E_NOTACCEPTING)
            break;
        if (FAILED(hrQ))
            return hrQ;
    }
    for (; i < frames.size(); i++)
        m_FlushFrames.push_back(std::move(frames[i]));
    return hr;
}

HRESULT CAVSAudioDecoderMFT::QueueOutputFrame(const FFmpegAudioDecoder::Frame& frame)
{
    const UINT32 channels = frame.channels;
    const UINT32 rate = (UINT32)frame.rate;
    const UINT32 cb = (UINT32)frame.pcm.size();
    if (channels == 0 || rate == 0 || cb == 0)
        return MF_E_INVALIDMEDIATYPE;

    if (m_OutputQueue.size() >= AVS_AUDIO_MFT_MAX_QUEUED_FRAMES)
        return MF_E_NOTACCEPTING;

    IMFMediaBuffer* pBuffer = NULL;
    IMFSample* pSample = NULL;

    HRESULT hr = MFCreateMemoryBuffer(cb, &pBuffer);
    if (FAILED(hr))
        return hr;

    BYTE* pbData = NULL;
    DWORD cbMaxLength = 0;
    hr = pBuffer->Lock(&pbData, &cbMaxLength, NULL);
    if (SUCCEEDED(hr))
    {
        if (cbMaxLength < cb)
        {
            hr = E_UNEXPECTED;
        }
        else
        {
            if (m_bFloatOutput)
            {
                float* dst = (float*)pbData;
                const short* src = (const short*)frame.pcm.data();
                const size_t n = frame.pcm.size() / sizeof(short);
                for (size_t i = 0; i < n; i++)
                    dst[i] = (float)src[i] / 32768.0f;
            }
            else
            {
                memcpy(pbData, frame.pcm.data(), cb);
            }
            pBuffer->Unlock();
        }
    }
    if (SUCCEEDED(hr))
        hr = pBuffer->SetCurrentLength(cb);

    if (SUCCEEDED(hr))
        hr = MFCreateSample(&pSample);
    if (SUCCEEDED(hr))
    {
        hr = pSample->AddBuffer(pBuffer);
        pSample->SetSampleTime(frame.rtStart);
        const LONGLONG hns = ((LONGLONG)frame.nbSamples * HNS_PER_SECOND) / (LONGLONG)rate;
        pSample->SetSampleDuration(hns);
    }
    SAFE_RELEASE(pBuffer);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pSample);
        return hr;
    }

    m_OutputQueue.push_back(pSample);   // the queue owns the reference
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// The channel arrangement that the DRA bit stream itself declares.  Containers normally
// only fill in a channel count, so this is where the mask becomes known: without it the
// audio renderer lays out the channels itself, and a 5.1 stream then easily ends up with
// the centre channel and the LFE in the wrong speakers.  When the type the pipeline has
// says nothing about speakers, the ordinary format change protocol is used so the
// renderer is configured with them.
//-------------------------------------------------------------------------------------------------
void CAVSAudioDecoderMFT::NoteDecodedChannelMask()
{
    const UINT32 mask = AvsChannelMaskToKS(m_Decoder.ChannelMaskFF(),
                                           (UINT32)m_Decoder.Channels());
    if (mask == 0 || mask == m_ChannelMask)
        return;

    m_ChannelMask = mask;

    if (m_pOutputType == NULL || m_bMaskChangeSignalled || m_bDrain)
        return;
    if (MFGetAttributeUINT32(m_pOutputType, MF_MT_AUDIO_CHANNEL_MASK, 0) != 0)
        return;                 // the host picked a type that already names speakers

    m_bMaskChangeSignalled = true;
    m_bStreamChangePending = TRUE;
}

HRESULT CAVSAudioDecoderMFT::UpdateOutputTypeForDecodedFormat(UINT32 rate, UINT32 channels)
{
    IMFMediaType* pType = NULL;
    IF_FAILED_RETURN(CreateOutputMediaType(rate, channels, m_bFloatOutput, &pType));

    SAFE_RELEASE(m_pOutputType);
    m_pOutputType = pType;
    m_Rate = rate;
    m_Channels = channels;
    return S_OK;
}

STDMETHODIMP CAVSAudioDecoderMFT::ProcessInput(DWORD dwInputStreamID, IMFSample* pSample,
                                               DWORD dwFlags)
{
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;
    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    AutoLock lock(m_Lock);

    if (pSample == NULL)
    {
        UNREFERENCED_PARAMETER(dwFlags);
        m_bDrain = TRUE;
        return FlushDecoderToQueue();
    }
    // see the note in the video decoder: a media session drains as soon as the source
    // signals end of stream, but keeps feeding the blocks that are already in the
    // pipeline afterwards, so draining must not close the input
    if (m_bDrain)
    {
        m_bDrain = FALSE;
        m_bDecoderFlushed = FALSE;      // a later drain has to flush libavcodec again
    }
    if (m_OutputQueue.size() >= AVS_AUDIO_MFT_MAX_QUEUED_FRAMES)
        return MF_E_NOTACCEPTING;

    IMFMediaBuffer* pBuffer = NULL;
    BYTE* pbData = NULL;
    DWORD cbData = 0;

    HRESULT hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr))
        return hr;
    hr = pBuffer->Lock(&pbData, NULL, &cbData);
    if (FAILED(hr))
    {
        pBuffer->Release();
        return hr;
    }
    if (cbData != 0)
    {
        hr = EnsureDecoder();
        if (FAILED(hr))
        {
            pBuffer->Unlock();
            pBuffer->Release();
            return hr;
        }
        // -1 means "no time stamp": 0 is a legal sample time
        LONGLONG rtStart = -1, rtDuration = 0;
        if (FAILED(pSample->GetSampleTime(&rtStart)))
            rtStart = -1;
        pSample->GetSampleDuration(&rtDuration);
        if (rtDuration < 0)
            rtDuration = 0;

        std::vector<FFmpegAudioDecoder::Frame> frames;
        hr = m_Decoder.Decode(pbData, cbData, rtStart,
                              (rtStart >= 0) ? rtStart + rtDuration : -1, frames);
        NoteDecodedChannelMask();

        // DRA frames carry their own rate/channels; if the negotiated output type
        // disagrees (or said nothing) fix it up and tell the pipeline.
        for (size_t i = 0; i < frames.size() && SUCCEEDED(hr); i++)
        {
            if (m_Rate != 0 && m_Channels != 0 &&
                (m_Rate != (UINT32)frames[i].rate || m_Channels != (UINT32)frames[i].channels))
            {
                m_bStreamChangePending = TRUE;
                break;
            }
            if (m_Rate == 0 || m_Channels == 0)
            {
                m_Rate = (UINT32)frames[i].rate;
                m_Channels = (UINT32)frames[i].channels;
                IF_FAILED_RETURN(UpdateOutputTypeForDecodedFormat(m_Rate, m_Channels));
            }
            IF_FAILED_RETURN(QueueOutputFrame(frames[i]));
        }
    }
    pBuffer->Unlock();
    pBuffer->Release();
    return hr;
}

STDMETHODIMP CAVSAudioDecoderMFT::ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
                                                MFT_OUTPUT_DATA_BUFFER* pOutputSamples,
                                                DWORD* pdwStatus)
{
    UNREFERENCED_PARAMETER(dwFlags);
    if (pOutputSamples == NULL || pdwStatus == NULL)
        return E_POINTER;
    if (cOutputBufferCount == 0)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);

    *pdwStatus = 0;
    for (DWORD i = 0; i < cOutputBufferCount; i++)
        pOutputSamples[i].dwStatus = 0;

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (m_bStreamChangePending)
    {
        // the pipeline has to renegotiate before it sees more samples
        if (AVSAudio_TraceOn())
            fprintf(stderr, "[avsaud] %p raising MF_E_TRANSFORM_STREAM_CHANGE\n",
                    (void*)this);
        if (m_Rate != 0 && m_Channels != 0)
            IF_FAILED_RETURN(UpdateOutputTypeForDecodedFormat(m_Rate, m_Channels));
        m_bStreamChangePending = FALSE;
        pOutputSamples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        *pdwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        return MF_E_TRANSFORM_STREAM_CHANGE;
    }

    if (m_OutputQueue.empty() && m_bDrain)
        IF_FAILED_RETURN(FlushDecoderToQueue());

    if (m_OutputQueue.empty())
    {
        pOutputSamples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_INCOMPLETE;
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    IMFSample* pSample = m_OutputQueue.front();
    m_OutputQueue.erase(m_OutputQueue.begin());

    if (AVSAudio_TraceOn())
    {
        static LONG s_nOut = 0;
        const LONG n = InterlockedIncrement(&s_nOut);
        if (n <= 2 || (n % 50) == 0)
            fprintf(stderr, "[avsmft] audio decoder handed %ld PCM block(s) to the graph\n",
                    (long)n);
    }

    pOutputSamples[0].pSample = pSample;      // ownership moves to the caller
    return S_OK;
}
