//----------------------------------------------------------------------------------------------
// AVSMediaSource.h - a custom Media Foundation media source for AVS / AVS+ / AVS2 /
//                    AVS3 (+ DRA audio) files.
//
// Why this is needed: the MPEG-2 TS byte stream handler that ships with Windows does not
// know stream_type 0x42 (AVS video) or the 'DRA1' registration descriptor, and it has no
// reader for a raw .avs elementary stream at all, so a player that opens one of our files
// never gets an AVS media type and therefore never reaches the AVS decoder MFT.  The
// media source below does the demuxing (with libavformat, the same library the decoders
// use), publishes the elementary streams with the MFVideoFormat_AVS / MFAudioFormat_DRA
// sub-types that AVSVideoDecoderMFT and AVSAudioDecoderMFT accept, and hands out the
// compressed access units as ordinary MF samples.
//
// Two entry points:
//   * CAVSByteStreamHandler (IMFByteStreamHandler) - the path MF itself uses when a
//     player opens a file whose extension is mapped to this handler in
//     HKLM\SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers.
//   * CAVSMediaSource + IAVSByteDataSource - direct creation from the class factory,
//     used by the avssource-test tool.
//
// Timeline: FFmpeg reports the transport stream timestamps as they are (a 27 bit clock
// that usually does not start at zero).  MF file sources are expected to present a
// presentation that starts at zero, so every timestamp is rebased on the start time of
// the presentation, exactly like the system sources do.
//----------------------------------------------------------------------------------------------
#ifndef AVSMEDIASOURCE_H
#define AVSMEDIASOURCE_H

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/rational.h>
}

#include <vector>

class CAVSInputStream;

// IID_IAVSByteDataSource and AVS_SOURCE_ATTRIBUTE_DEMUX_HINT are defined in
// classGuids.h, like every other GUID of this DLL.

// Lets a caller hand us the bytes without going through a byte stream handler.
interface IAVSByteDataSource : public IUnknown
{
    virtual STDMETHODIMP SetByteStream(IMFByteStream* pByteStream) = 0;
    virtual STDMETHODIMP Open(void) = 0;
    // After Open(): does this byte stream hold AVS-family video or DRA audio?  The byte
    // stream handler asks before deciding whether to claim the file.  Narrower than what
    // the source is able to publish - publishing a standard type is not a reason to claim.
    virtual STDMETHODIMP IsAvsOrDra(BOOL* pbMine) = 0;
};

//----------------------------------------------------------------------------------------------
class CAVSMediaSource : public IMFMediaSourceEx, public IAVSByteDataSource, public BaseObject
{
    friend class CAVSInputStream;
    friend int AVS_IO_Read(void* pOpaque, uint8_t* pBuffer, int cbSize);
    friend int64_t AVS_IO_Seek(void* pOpaque, int64_t llOffset, int iWhence);

public:

    static HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject);

protected:

    CAVSMediaSource();

public:

    virtual ~CAVSMediaSource();

    // IUnknown
    STDMETHODIMP         QueryInterface(REFIID riid, void** ppvObject);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent);
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                           HRESULT hrStatus, const PROPVARIANT* pvValue);

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics);
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD);
    STDMETHODIMP Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                       const PROPVARIANT* pvarStartPosition);
    STDMETHODIMP Stop(void);
    STDMETHODIMP Pause(void);
    STDMETHODIMP Shutdown(void);

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes);
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes);
    STDMETHODIMP SetD3DManager(IUnknown* pManager);

    // IAVSByteDataSource
    STDMETHODIMP SetByteStream(IMFByteStream* pByteStream);
    STDMETHODIMP Open(void);
    STDMETHODIMP IsAvsOrDra(BOOL* pbMine);

private:
    struct StreamEntry
    {
        StreamEntry() : pStream(NULL), pSD(NULL), ffIndex(-1) {}
        CAVSInputStream*  pStream;
        IMFStreamDescriptor* pSD;
        int               ffIndex;      // index inside AVFormatContext
    };

    HRESULT OpenLocked(void);
    HRESULT BuildStreamDescriptor(int ffIndex, IMFStreamDescriptor** ppSD);
    HRESULT BuildVideoType(AVStream* st, IMFMediaType** ppType);
    HRESULT BuildAudioType(AVStream* st, IMFMediaType** ppType);
    HRESULT ReadPacket(AVPacket** ppPkt);            // takes m_DemuxLock itself
    void    PrerollScan(void);                      // sets m_rtStart from the earliest PTS
    void    ClearPreroll(void);
    void    CloseFile(void);
    LONGLONG ToHns(AVRational tb, long long ts) const;

    // Raw elementary streams (.avs2 / .avs3 / .avs) carry no time stamps at all: FFmpeg's
    // raw demuxers hand out packets with pts = NOPTS.  StreamHasTimestamps() records what
    // PrerollScan saw, and StreamFrameRate() gives the rate that the demuxer probing
    // found, which is what a synthesized timeline has to be built from.
    bool       StreamHasTimestamps(int ffIndex) const;
    AVRational StreamFrameRate(int ffIndex) const;

    // A media source has to post MEEndOfPresentation itself once the whole presentation
    // is finished; the media session turns that into MESessionEnded, which is what makes
    // a player stop / report playback finished.  Every stream reports its own end here.
    void    NotifyStreamEnded(void);

    LONG                m_RefCount;
    CriticSection       m_Lock;          // protects the object state
    CriticSection       m_DemuxLock;     // protects AVFormatContext

    IMFMediaEventQueue* m_pEventQueue;
    IMFByteStream*      m_pByteStream;
    AVIOContext*        m_pAVIO;
    uint8_t*            m_pAVIOBuffer;
    AVFormatContext*    m_pFmt;
    BOOL                m_bOpened;
    BOOL                m_bStarted;
    BOOL                m_bShutdown;

    IMFAttributes*      m_pAttributes;
    IMFPresentationDescriptor* m_pPD;
    std::vector<StreamEntry> m_Streams;

    DWORD               m_nStartedStreams;   // streams started by the last Start()
    DWORD               m_nEndedStreams;     // ... of which MEEndOfStream was posted
    BOOL                m_bEopPosted;

    // presentation timeline.  m_llStartHns is the earliest presentation time found in the
    // file (PrerollScan); every sample time is relative to it, so the presentation starts
    // at zero.  m_rtStart is only the container's own start time, used for seeking.
    LONGLONG            m_rtStart;       // container start, 100 ns
    LONGLONG            m_rtDuration;
    LONGLONG            m_llStartHns;    // presentation length, 100ns
    QWORD               m_cbSize;        // byte size of the source, if known
    BOOL                m_bSeekable;

    // the first packets of the file, read during Open() to find out where the
    // presentation really starts (see PrerollScan); handed out again by ReadPacket
    std::vector<AVPacket*> m_Preroll;
    size_t              m_PrerollPos;
    // the leading AVS sequence header of a raw elementary stream, read while probing.  It is
    // handed to libavcodec as extradata and published as MF_MT_MPEG_SEQUENCE_HEADER, which is
    // the half of the compressed-stream contract that says what format the decoder decodes;
    // the H.264 control path carries it, our AVS type did not (scripts/type-diff.py).
    std::vector<BYTE>     m_FmtHdr;
    // one flag per AVFormatContext stream: PrerollScan saw a usable PTS.  Streams that
    // never produce one get a synthesized timeline instead of a pile of zero time stamps.
    std::vector<char>   m_bStreamStamps;
    unsigned            m_nIoTrace;      // trace only (AVS_MFT_TRACE)
};

//----------------------------------------------------------------------------------------------
class CAVSInputStream : public IMFMediaStream, public BaseObject
{
public:

    static HRESULT CreateInstance(CAVSMediaSource* pSource, IMFStreamDescriptor* pSD,
                                  int ffIndex, REFIID riid, void** ppvObject);

protected:

    CAVSInputStream(CAVSMediaSource* pSource, IMFStreamDescriptor* pSD, int ffIndex);

public:

    virtual ~CAVSInputStream();

    // IUnknown
    STDMETHODIMP         QueryInterface(REFIID riid, void** ppvObject);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent);
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                           HRESULT hrStatus, const PROPVARIANT* pvValue);

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource);
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor);
    STDMETHODIMP RequestSample(IUnknown* pToken);

    // used by the source
    int              FFIndex(void) const { return m_ffIndex; }
    HRESULT          QueueSample(IMFSample* pSample);
    void             AttachToken(IMFSample* pSample, IUnknown* pToken);
    HRESULT          QueueEndOfStream(void);
    void             ClearQueue(void);      // drop every pending event
    void             Reset(void);
    void             ShutdownQueue(void);
    WORD             BufferedCount(void) const { return m_QueueCount; }
    void             AddBuffered(void) { if (m_QueueCount < 0xFFFF) m_QueueCount++; }

    // An access unit of this stream that was read while the *other* stream was being
    // served waits here and is handed out by the next RequestSample of this stream.
    // Keeping them out of the MF event queue means the stream never posts samples that
    // nobody asked for: a media session only expects a handful of those, and posting a
    // whole file worth of unrequested MEMediaSample events makes the pipeline stop.
    void             PushPending(IMFSample* pSample);
    IMFSample*       PopPending(void);
    size_t           PendingCount(void) const { return m_Pending.size(); }

private:
    LONG                m_RefCount;
    CAVSMediaSource*    m_pSource;
    IMFStreamDescriptor* m_pSD;
    IMFMediaEventQueue* m_pEventQueue;
    int                 m_ffIndex;
    BOOL                m_bEOS;
    WORD                m_QueueCount;   // events posted for this stream but not asked for
    std::vector<IMFSample*> m_Pending;  // compressed access units, in decode order
    // timeline for streams without any time stamps (raw elementary video)
    LONGLONG            m_rtSynth;
};

//----------------------------------------------------------------------------------------------
class CAVSByteStreamHandler : public IMFByteStreamHandler, public BaseObject
{
public:

    static HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject);

protected:
    CAVSByteStreamHandler();

public:
    virtual ~CAVSByteStreamHandler();

    STDMETHODIMP         QueryInterface(REFIID riid, void** ppvObject);
    STDMETHODIMP_(ULONG) AddRef();
    STDMETHODIMP_(ULONG) Release();

    STDMETHODIMP BeginCreateObject(IMFByteStream* pByteStream, LPCWSTR pwszURL,
                                   DWORD dwFlags, IPropertyStore* pProps,
                                   IUnknown** ppIUnknownCancelCookie,
                                   IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHODIMP EndCreateObject(IMFAsyncResult* pResult, MF_OBJECT_TYPE* pObjectType,
                                 IUnknown** ppObject);
    STDMETHODIMP CancelObjectCreation(IUnknown* pIUnknownCancelCookie);
    STDMETHODIMP GetMaxNumberOfBytesRequiredForResolution(QWORD* pqwBytes);

private:
    LONG        m_RefCount;
    // result of the synchronous create, handed out by EndCreateObject
    IUnknown*   m_pObject;
    HRESULT     m_CreateHR;
};

#endif // AVSMEDIASOURCE_H
