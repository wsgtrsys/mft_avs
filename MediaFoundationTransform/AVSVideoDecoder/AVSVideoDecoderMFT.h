//----------------------------------------------------------------------------------------------
// AVSVideoDecoderMFT.h - Media Foundation decoder MFT for AVS / AVS+ / AVS2 / AVS3
//
// Pipeline position:
//
//   <file / TS byte stream> -> media source -> [AVS compressed sample] -> THIS MFT
//                                                                    -> [NV12/I420] -> output
//
// Input  : one compressed AVS access unit per IMFMediaBuffer, sub-type
//          MFVideoFormat_AVS / MFVideoFormat_AVS2 / MFVideoFormat_AVS3
// Output : NV12 (default) or I420, MFVideoFormat_NV12 / MFVideoFormat_I420
//
// The MFT can run in either of the two MFT models:
//
//   * synchronous (default, `AVS_MFT_ASYNC=0`): the MFT handler drives it with
//     ProcessInput/ProcessOutput exactly like the Microsoft H.264/HEVC software
//     decoders.  Because a video decoder has an internal delay, ProcessOutput returns
//     MF_E_TRANSFORM_NEED_MORE_INPUT until the decoder produced a picture, and
//     GetInputStatus returns MF_E_NOTACCEPTING while the internal queue of decoded
//     pictures is full.
//
//   * asynchronous (`AVS_MFT_ASYNC=1`): MF_TRANSFORM_ASYNC is advertised on the
//     transform's attribute store, the MFT implements IMFMediaEventGenerator, and the
//     decoding runs on an internal worker thread: ProcessInput copies the access unit
//     into a small queue and returns immediately, the worker decodes it and posts
//     METransformHaveOutput per picture plus METransformNeedInput when it will take
//     another one, and a drain posts METransformDrainComplete.  That is the shape a
//     real decoder - and every system decoder - has.  The synchronous form stays the
//     default because it is what the sample MFTs do and it works with the
//     SourceReader.  See README, "the media session does not end".
//
//
// AVS sequence headers are carried inside the bit stream, so the decoder learns the
// picture size from the first decoded frame.  When that size is different from the
// output media type that was negotiated, the MFT uses MF's dynamic format change
// protocol (MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE + MF_E_TRANSFORM_STREAM_CHANGE) and the
// pipeline re-negotiates the output type.
//----------------------------------------------------------------------------------------------
#ifndef AVSVIDEODECODERMFT_H
#define AVSVIDEODECODERMFT_H

#include "FFmpegDecoder.h"
#include "AvsColor.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define AVS_MFT_MAX_QUEUED_FRAMES 8

// BaseObject takes the class-factory server lock for as long as this object lives,
// so DllCanUnloadNow() refuses to let the host unload the DLL underneath a live MFT.
class CAVSVideoDecoderMFT : public IMFTransform, public IMFMediaEventGenerator, public BaseObject
{
public:

    static HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject);

protected:

    CAVSVideoDecoderMFT();

public:

    virtual ~CAVSVideoDecoderMFT();

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

    // IMFMediaEventGenerator (only used when MF_TRANSFORM_ASYNC is advertised)
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent);
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue);
    STDMETHODIMP Flush();

private:
    // media type helpers (Check* are helpers, not part of IMFTransform)
    HRESULT         CheckInputType(IMFMediaType* pType);
    HRESULT         CheckOutputStreamType(IMFMediaType* pType);
    static bool     IsInputSubtype(const GUID& subtype);
    static HRESULT  SubtypeToCodec(const GUID& subtype, AVCodecID& codecId);
    static HRESULT  SubtypeToOutputFormat(const GUID& subtype, AVPixelFormat& fmt, GUID& outSubtype);
    static HRESULT  CloneMediaType(IMFMediaType* pSource, IMFMediaType** ppOut);
    static HRESULT  CopyMediaTypeAttributes(IMFMediaType* pFrom, IMFMediaType* pTo);
    HRESULT         ValidateInputType(IMFMediaType* pType, GUID* pSubtypeOut);
    HRESULT         ValidateOutputType(IMFMediaType* pType, AVPixelFormat* pFormatOut, GUID* pSubtypeOut);
    HRESULT         CreateOutputMediaType(UINT32 width, UINT32 height, IMFMediaType** ppType);
    HRESULT         EnsureDecoder();
    void            DestroyDecoder();
    HRESULT         QueueOutputFrame(const FFmpegVideoDecoder::Frame& frame);
    void            PumpFlushFramesIntoQueue();
    LONGLONG        DefaultFrameDurationHns();
    void            ClearOutputQueue();
    HRESULT         FlushDecoderToQueue();
    void            NoteDecoderColor();
    HRESULT         UpdateOutputTypeForDecodedSize(UINT32 width, UINT32 height);
    HRESULT         EnsureEventQueue();
    // METransformHaveOutput / METransformDrainComplete / METransformNeedInput: how an
    // asynchronous MFT talks to the pipeline.  No-op in the synchronous model.
    HRESULT         PostTransformEvent(MediaEventType met);

    // ---- asynchronous plumbing, used only when AVS_MFT_ASYNC=1 ------------------
    struct AsyncInput
    {
        std::vector<uint8_t> data;      // the compressed access unit
        LONGLONG             rtStart;
        LONGLONG             rtDuration;
        bool                 bDrain;    // a drain request, no data

        AsyncInput() : rtStart(-1), rtDuration(-1), bDrain(false) {}
    };

    void            WorkerThread();     // the decode loop of m_Worker
    HRESULT         EnqueueAsync(AsyncInput in);
    HRESULT         EnqueueDrain();
    size_t          OutputQueueCount();

    std::thread             m_Worker;
    std::mutex              m_AsyncLock;
    std::condition_variable m_AsyncCv;
    std::deque<AsyncInput>  m_AsyncIn;
    BOOL                    m_bWorkerRunning;
    BOOL                    m_bWorkerStop;
    bool                    m_bDrainQueued;   // a drain request is already in the queue

    LONG                m_RefCount;
    CriticSection       m_Lock;

    DWORD               m_nInputStreamCount;
    DWORD               m_nOutputStreamCount;
    DWORD               m_InputStreamID;
    DWORD               m_OutputStreamID;

    IMFMediaType*       m_pInputType;
    IMFMediaType*       m_pOutputType;
    IMFAttributes*      m_pAttributes;
    IMFMediaEventQueue* m_pEventQueue;      // only when running as an async MFT
    BOOL                m_bAsync;

    BOOL                m_bDrain;                 // ProcessInput(NULL) seen
    BOOL                m_bStreamChangePending;   // decoded size != negotiated size
    // The bit stream revealed colour signalling that the negotiated output type does
    // not carry.  Signalled once per decoder instance (see NoteDecoderColor).
    BOOL                m_bColorChangeSignalled;
    BOOL                m_bInputStreamActivated;
    BOOL                m_bOutputStreamActivated;

    AVCodecID           m_CodecId;
    AVPixelFormat       m_OutputFormat;
    GUID                m_OutputSubtype;

    FFmpegVideoDecoder  m_Decoder;

    // The colour signalling of the stream: inherited from the compressed input type and
    // filled in from the bit stream itself.  Put on every output media type.
    AvsColorInfo        m_Color;

    std::vector<IMFSample*> m_OutputQueue;        // decoded pictures, AddRef'd

    // Pictures that the output queue could not take.  They are handed over by the next
    // ProcessOutput / ProcessInput instead of being dropped: without this the last few
    // pictures of a stream disappear when the queue is full, and so does the rest of any
    // burst that is larger than the queue (an AVS2/AVS3 decoder releases dozens of
    // pictures at once - regression 11t counts exactly that).
    std::vector<FFmpegVideoDecoder::Frame> m_FlushFrames;
    BOOL                m_bDecoderFlushed;       // the decoder drain already happened

    // Set by MFT_MESSAGE_NOTIFY_END_OF_STREAM: no more input will arrive until a flush.
    // The last sample handed to the pipeline then carries
    // MFT_OUTPUT_DATA_BUFFER_STREAM_END (see ProcessOutput).
    BOOL                m_bOutputStreamEnd;
};

#endif // AVSVIDEODECODERMFT_H
