//----------------------------------------------------------------------------------------------
// AVSAudioDecoderMFT.h - DRA (AVS audio) decoder MFT: compressed DRA access units in,
//                        PCM 16 / IEEE float out.
//
// Media Foundation has no sub-type for DRA either, so the input type is the custom
// MFAudioFormat_DRA ('DRA ') - or the 'DRA1' fourCC that MPEG-TS registration
// descriptors carry.  Everything else follows the ordinary audio decoder pattern:
// the stream format (rate / channel count) normally comes from the input media type,
// and when the container did not know it the values of the first decoded frame are
// used and a stream change is raised.
//----------------------------------------------------------------------------------------------
#ifndef AVSAUDIODECODERMFT_H
#define AVSAUDIODECODERMFT_H

#include "FFmpegAudioDecoder.h"

#define AVS_AUDIO_MFT_MAX_QUEUED_FRAMES 16

class CAVSAudioDecoderMFT : public IMFTransform, public BaseObject
{
public:

    static HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject);

protected:

    CAVSAudioDecoderMFT();

public:

    virtual ~CAVSAudioDecoderMFT();

    // IUnknown
    STDMETHODIMP         QueryInterface(REFIID iid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IMFTransform
    STDMETHODIMP GetStreamLimits(DWORD* pdwInputMinimum, DWORD* pdwInputMaximum,
                                 DWORD* pdwOutputMinimum, DWORD* pdwOutputMaximum);
    STDMETHODIMP GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams);
    STDMETHODIMP GetStreamIDs(DWORD dwInputIDArraySize, DWORD* pdwInputIDs,
                              DWORD dwOutputIDArraySize, DWORD* pdwOutputIDs);
    STDMETHODIMP GetInputStreamInfo(DWORD dwInputStreamID, MFT_INPUT_STREAM_INFO* pStreamInfo);
    STDMETHODIMP GetOutputStreamInfo(DWORD dwOutputStreamID, MFT_OUTPUT_STREAM_INFO* pStreamInfo);
    STDMETHODIMP GetAttributes(IMFAttributes** ppAttributes);
    STDMETHODIMP GetInputStreamAttributes(DWORD dwInputStreamID, IMFAttributes** ppAttributes);
    STDMETHODIMP GetOutputStreamAttributes(DWORD dwOutputStreamID, IMFAttributes** ppAttributes);
    STDMETHODIMP DeleteInputStream(DWORD dwStreamID);
    STDMETHODIMP AddInputStreams(DWORD cStreams, DWORD* adwStreamIDs);
    STDMETHODIMP GetInputAvailableType(DWORD dwInputStreamID, DWORD dwTypeIndex, IMFMediaType** ppType);
    STDMETHODIMP GetOutputAvailableType(DWORD dwOutputStreamID, DWORD dwTypeIndex, IMFMediaType** ppType);
    STDMETHODIMP SetInputType(DWORD dwInputStreamID, IMFMediaType* pType, DWORD dwFlags);
    STDMETHODIMP SetOutputType(DWORD dwOutputStreamID, IMFMediaType* pType, DWORD dwFlags);
    STDMETHODIMP GetInputCurrentType(DWORD dwInputStreamID, IMFMediaType** ppType);
    STDMETHODIMP GetOutputCurrentType(DWORD dwOutputStreamID, IMFMediaType** ppType);
    STDMETHODIMP GetInputStatus(DWORD dwInputStreamID, DWORD* pdwFlags);
    STDMETHODIMP GetOutputStatus(DWORD* pdwFlags);
    STDMETHODIMP SetOutputBounds(LONGLONG hnsLowerBound, LONGLONG hnsUpperBound);
    STDMETHODIMP ProcessEvent(DWORD dwInputStreamID, IMFMediaEvent* pEvent);
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE eMessage, ULONG_PTR ulParam);
    STDMETHODIMP ProcessInput(DWORD dwInputStreamID, IMFSample* pSample, DWORD dwFlags);
    STDMETHODIMP ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
                               MFT_OUTPUT_DATA_BUFFER* pOutputSamples, DWORD* pdwStatus);

private:
    HRESULT         CheckInputType(IMFMediaType* pType);
    HRESULT         CheckOutputType(IMFMediaType* pType);
    static bool     IsInputSubtype(const GUID& subtype);
    static HRESULT  SubtypeToCodec(const GUID& subtype, AVCodecID& codecId);
    HRESULT         CreateOutputMediaType(UINT32 rate, UINT32 channels, bool ieeeFloat,
                                          IMFMediaType** ppType);
    HRESULT         EnsureDecoder();
    void            ClearOutputQueue();
    HRESULT         QueueOutputFrame(const FFmpegAudioDecoder::Frame& frame);
    HRESULT         UpdateOutputTypeForDecodedFormat(UINT32 rate, UINT32 channels);
    void            NoteDecodedChannelMask();
    void            DestroyDecoder();
    HRESULT         FlushDecoderToQueue();

    // the negotiated stream format
    UINT32          m_Rate;
    UINT32          m_Channels;
    // KSAUDIO_CHANNEL_MASK for the stream (0 = nobody told us).  The container usually
    // only gives a channel count, so this normally comes from the DRA bit stream.
    UINT32          m_ChannelMask;
    bool            m_bMaskChangeSignalled;
    bool            m_bFloatOutput;

    LONG            m_RefCount;
    CriticSection   m_Lock;

    DWORD           m_InputStreamID;
    DWORD           m_OutputStreamID;

    IMFMediaType*   m_pInputType;
    IMFMediaType*   m_pOutputType;
    IMFAttributes*  m_pAttributes;

    BOOL            m_bDrain;
    BOOL            m_bStreamChangePending;

    AVCodecID       m_CodecId;
    FFmpegAudioDecoder m_Decoder;
    std::vector<IMFSample*> m_OutputQueue;

    // Blocks taken out of the decoder at drain time that did not fit into the output
    // queue: they are handed out by the next ProcessOutput instead of being dropped.
    std::vector<FFmpegAudioDecoder::Frame> m_FlushFrames;
    BOOL            m_bDecoderFlushed;
};

#endif // AVSAUDIODECODERMFT_H
