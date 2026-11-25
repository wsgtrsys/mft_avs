//----------------------------------------------------------------------------------------------
// AVSVideoDecoderMFT_Process.cpp - the actual decode loop: ProcessInput / ProcessOutput,
// drain, flush and the MF dynamic format change protocol.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "classGuids.h"
#include "AVSVideoDecoderMFT.h"
#include "AvsTypeDump.h"

// AVS_MFT_TRACE=1 turns on the source/MFT lifecycle and progress logs.
static bool AVS_MFT_TraceOn()
{
    static int s_on = -1;
    if (s_on < 0)
        s_on = (GetEnvironmentVariableW(L"AVS_MFT_TRACE", nullptr, 0) > 0) ? 1 : 0;
    return s_on == 1;
}

// Media Foundation never sends MFT_MESSAGE_NOTIFY_END_OF_STREAM to this decoder (seen
// with the tracing below: the graph sends COMMAND_DRAIN and nothing else).  A media
// session then never learns that the branch is finished: the presentation plays to the
// last frame, the clock stops at the end of the content, the topology reports
// MF_TOPOSTATUS_ENDED, but MESessionEnded never arrives.  COMMAND_DRAIN is only sent at
// the end of a segment, so treat "drained and nothing new arrived" as the end of the
// stream as well; AVS_MFT_DRAIN_EOS=1 enables that.  It has no observed effect on the
// missing MESessionEnded, so it stays off by default and is kept only as a probe.
static bool AVS_MFT_DrainEOS()
{
    static int s_on = -1;
    if (s_on < 0)
    {
        wchar_t buf[8] = L"";
        s_on = (GetEnvironmentVariableW(L"AVS_MFT_DRAIN_EOS", buf, ARRAYSIZE(buf)) > 0 &&
                buf[0] == L'1') ? 1 : 0;
    }
    return s_on == 1;
}

// Probe for the end-of-presentation handshake (AVS_MFT_HOLD_LAST=1): keep the newest
// picture back until either another compressed sample or COMMAND_DRAIN arrives.  A
// decoder with an internal delay behaves like that anyway, and the difference for the
// pipeline is that its DRAIN arrives *before* the last picture is handed over - which is
// the point where a decoder is supposed to release its remaining frames and mark the
// last one as the end of the stream.  With our decoder every picture is already out
// before DRAIN arrives, so no sample can ever be marked.
static bool AVS_MFT_HoldLast()
{
    static int s_on = -1;
    if (s_on < 0)
    {
        wchar_t buf[8] = L"";
        s_on = (GetEnvironmentVariableW(L"AVS_MFT_HOLD_LAST", buf, ARRAYSIZE(buf)) > 0 &&
                buf[0] == L'1') ? 1 : 0;
    }
    return s_on == 1;
}

size_t CAVSVideoDecoderMFT::OutputQueueCount()
{
    AutoLock lock(m_Lock);
    return m_OutputQueue.size();
}

//-------------------------------------------------------------------------------------------------
// Asynchronous model: the decoding runs on a worker thread so that ProcessInput only
// queues the access unit and returns immediately.  That is what makes the event
// protocol honest: METransformHaveOutput / METransformNeedInput are then posted from a
// thread other than the one the pipeline called us on, which is what the asynchronous
// MFT proxy of a media session expects.
//-------------------------------------------------------------------------------------------------
HRESULT CAVSVideoDecoderMFT::EnqueueAsync(AsyncInput in)
{
    std::lock_guard<std::mutex> lk(m_AsyncLock);
    if (m_AsyncIn.size() >= 32)
        return MF_E_NOTACCEPTING;          // the worker fell that far behind
    m_AsyncIn.push_back(std::move(in));
    m_AsyncCv.notify_one();
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// A drain is queued only once: a media session sends MFT_MESSAGE_COMMAND_DRAIN and
// afterwards ProcessInput(NULL), and the worker must flush libavcodec just once.
//-------------------------------------------------------------------------------------------------
HRESULT CAVSVideoDecoderMFT::EnqueueDrain()
{
    {
        std::lock_guard<std::mutex> lk(m_AsyncLock);
        if (m_bDrainQueued)
            return S_OK;
        m_bDrainQueued = true;
    }
    AsyncInput d;
    d.bDrain = true;
    return EnqueueAsync(std::move(d));
}

//-------------------------------------------------------------------------------------------------
void CAVSVideoDecoderMFT::WorkerThread()
{
    for (;;)
    {
        AsyncInput in;
        {
            std::unique_lock<std::mutex> lk(m_AsyncLock);
            m_AsyncCv.wait(lk, [this] { return m_bWorkerStop != FALSE || !m_AsyncIn.empty(); });
            if (m_bWorkerStop)
                return;
            in = std::move(m_AsyncIn.front());
            m_AsyncIn.pop_front();
        }

        bool bAskForMore = true;
        if (in.bDrain)
        {
            {
                std::lock_guard<std::mutex> lk(m_AsyncLock);
                m_bDrainQueued = false;
            }
            const size_t nBefore = m_OutputQueue.size();
            const HRESULT hrD = FlushDecoderToQueue();
            if (SUCCEEDED(hrD))
            {
                for (size_t i = m_OutputQueue.size(); i > nBefore; i--)
                    PostTransformEvent(METransformHaveOutput);
                PostTransformEvent(METransformDrainComplete);
            }
        }
        else
        {
            // flow control: the synchronous model reports MF_E_NOTACCEPTING from
            // GetInputStatus while the picture queue is full; here the worker simply
            // waits, the pipeline keeps its own sample queued
            while (OutputQueueCount() >= AVS_MFT_MAX_QUEUED_FRAMES)
                Sleep(5);

            if (EnsureDecoder() == S_OK)
            {
                std::vector<FFmpegVideoDecoder::Frame> frames;
                HRESULT hrD = m_Decoder.Decode(in.data.data(), (int)in.data.size(),
                                              in.rtStart,
                                              (in.rtStart >= 0) ? in.rtStart + in.rtDuration : -1,
                                              frames);
                NoteDecoderColor();
                for (size_t i = 0; i < frames.size() && SUCCEEDED(hrD); i++)
                {
                    while (OutputQueueCount() >= AVS_MFT_MAX_QUEUED_FRAMES)
                        Sleep(5);
                    hrD = QueueOutputFrame(frames[i]);
                    if (SUCCEEDED(hrD))
                        PostTransformEvent(METransformHaveOutput);
                }
            }
            else
                bAskForMore = false;
        }

        if (bAskForMore)
            PostTransformEvent(METransformNeedInput);
    }
}

//-------------------------------------------------------------------------------------------------
// The pipeline feeds one compressed AVS access unit per call.  Passing NULL flushes.
STDMETHODIMP CAVSVideoDecoderMFT::ProcessInput(DWORD dwInputStreamID, IMFSample* pSample,
                                              DWORD dwFlags)
{
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;
    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    AutoLock lock(m_Lock);

    // graph side diagnosis: what the pipeline really feeds us (a media session does not
    // necessarily forward every access unit the source handed out)
    static volatile LONG s_nProcIn = 0;
    if (AVS_MFT_TraceOn())
    {
        LONGLONG rtDbg = -1;
        if (pSample != NULL)
            pSample->GetSampleTime(&rtDbg);
        const LONG n = InterlockedIncrement(&s_nProcIn);
        fprintf(stderr, "[avsmft] ProcessInput #%ld flags=0x%lX time=%lld outq=%zu\n", (long)n,
                (unsigned long)dwFlags, rtDbg, m_OutputQueue.size());
    }

    if (pSample == NULL)
    {
        // a NULL sample means "no more input": pull the pictures that the decoder
        // still holds out of it (MF also sends MFT_MESSAGE_COMMAND_DRAIN for this)
        UNREFERENCED_PARAMETER(dwFlags);
        m_bDrain = TRUE;
        if (m_bAsync)
        {
            return EnqueueDrain();
        }
        const size_t nBefore = m_OutputQueue.size();
        const HRESULT hrF = FlushDecoderToQueue();
        if (SUCCEEDED(hrF))
        {
            for (size_t i = m_OutputQueue.size(); i > nBefore; i--)
                PostTransformEvent(METransformHaveOutput);
            PostTransformEvent(METransformDrainComplete);
        }
        return hrF;
    }

    // A media session sends MFT_MESSAGE_COMMAND_DRAIN as soon as the source signals
    // end of stream, but the access units that are already inside the pipeline are fed
    // to the decoder afterwards.  Refusing them cut the last seconds of the video and
    // the session then ended the presentation early, so a drain only means "hand out
    // what you are holding right now"; input stays open.
    if (m_bDrain)
    {
        m_bDrain = FALSE;
        m_bDecoderFlushed = FALSE;      // a later drain has to flush libavcodec again
    }

    // the synchronous model refuses input while its picture queue is full; the
    // asynchronous one lets the access unit in and the worker waits for room instead.
    // Pictures held from an earlier burst go back into the queue first: if they stayed
    // behind, this call would refuse input forever even though the pipeline drained.
    if (!m_bAsync)
        PumpFlushFramesIntoQueue();
    if (!m_bAsync && (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES ||
                      !m_FlushFrames.empty()))
    {
        if (AVS_MFT_TraceOn())
            fprintf(stderr, "[avsmft] REFUSE-INPUT(B): picture queue full at ProcessInput entry, "
                            "queue=%zu held=%zu\n", m_OutputQueue.size(), m_FlushFrames.size());
        return MF_E_NOTACCEPTING;
    }

    IMFMediaBuffer* pBuffer = NULL;
    HRESULT hr = pSample->ConvertToContiguousBuffer(&pBuffer);
    if (FAILED(hr))
        return hr;

    BYTE* pbData = NULL;
    DWORD cbData = 0;
    hr = pBuffer->Lock(&pbData, NULL, &cbData);
    if (FAILED(hr))
    {
        pBuffer->Release();
        return hr;
    }

    // no time stamp at all is -1, not 0: 0 is a legal sample time
    LONGLONG rtStart = -1, rtDuration = 0;
    if (FAILED(pSample->GetSampleTime(&rtStart)))
        rtStart = -1;
    pSample->GetSampleDuration(&rtDuration);

    if (m_bAsync)
    {
        // hand the access unit to the worker; the pictures come back through
        // ProcessOutput when the pipeline asks for them (after METransformHaveOutput)
        AsyncInput in;
        in.data.assign(pbData, pbData + cbData);
        in.rtStart = rtStart;
        in.rtDuration = rtDuration;

        pBuffer->Unlock();
        pBuffer->Release();

        return EnqueueAsync(std::move(in));
    }

    hr = EnsureDecoder();
    if (SUCCEEDED(hr))
    {
        std::vector<FFmpegVideoDecoder::Frame> frames;
        hr = m_Decoder.Decode(pbData, cbData, rtStart,
                              (rtStart >= 0) ? rtStart + rtDuration : -1, frames);
        NoteDecoderColor();
        for (size_t i = 0; i < frames.size() && SUCCEEDED(hr); i++)
        {
            if (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES)
            {
                // The decoder released more pictures than the picture queue can hold.
                // Keeping them is the difference between a player that shows the whole
                // film and one that shows part of it: an AVS3 stream released 60 pictures
                // at once and 311 of 450 used to be discarded here (regression 11t).
                // Input is refused from GetInputStatus until this list drains, which is
                // what the MFT contract prescribes for a synchronous transform.
                // experiment switch: AVS_MFT_DROP_OVERFLOW=1 restores the old behaviour
                // (discard the rest of the burst) so the two can be compared in one build
                static int s_bDrop = -1;
                if (s_bDrop < 0)
                {
                    const char* pEnv = getenv("AVS_MFT_DROP_OVERFLOW");
                    s_bDrop = (pEnv && *pEnv) ? 1 : 0;
                }
                if (s_bDrop)
                {
                    static volatile LONG s_nDropped = 0;
                    const LONG lost = (LONG)(frames.size() - i);
                    const LONG total = InterlockedExchangeAdd(&s_nDropped, lost) + lost;
                    fprintf(stderr, "[avsmft] DROP: the picture queue is full, %ld picture(s) "
                                    "discarded because of AVS_MFT_DROP_OVERFLOW (total dropped "
                                    "%ld)\n", lost, total);
                    break;
                }
                static volatile LONG s_nHeld = 0;
                const LONG rest = (LONG)(frames.size() - i);
                const LONG total = InterlockedExchangeAdd(&s_nHeld, rest) + rest;
                fprintf(stderr, "[avsmft] HOLD: the picture queue is full, %ld picture(s) are "
                                "kept for the next call (kept so far %ld, nothing dropped)\n",
                        rest, total);
                for (; i < frames.size(); i++)
                    m_FlushFrames.push_back(std::move(frames[i]));
                break;
            }
            hr = QueueOutputFrame(frames[i]);
            // one METransformHaveOutput per picture that became available: the
            // asynchronous protocol counts samples, it does not say "queue non-empty"
            if (SUCCEEDED(hr))
                PostTransformEvent(METransformHaveOutput);
        }
    }

    pBuffer->Unlock();
    pBuffer->Release();

    // The asynchronous model has no GetOutputStatus polling: the pipeline waits for
    // METransformHaveOutput (posted per picture above) and for METransformNeedInput
    // before it feeds the next access unit.  Without the latter the pipeline of an
    // asynchronous MFT never feeds the decoder at all - it waits for the MFT to ask
    // for input.  Exactly one per consumed input: extra ones desynchronize the
    // pipeline's own accounting and it stops sending after a while.
    if (SUCCEEDED(hr) && m_OutputQueue.size() < AVS_MFT_MAX_QUEUED_FRAMES)
        PostTransformEvent(METransformNeedInput);
    return hr;
}

//-------------------------------------------------------------------------------------------------
// IMFMediaEventGenerator.  Only meaningful when MF_TRANSFORM_ASYNC is advertised: the
// media session then drives the MFT through this queue ("the pipeline asked for the
// event" -> BeginGetEvent, "something happened" -> QueueEvent).
STDMETHODIMP CAVSVideoDecoderMFT::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    if (ppEvent == NULL)
        return E_POINTER;
    if (!m_bAsync)
        return MF_E_INVALIDREQUEST;

    HRESULT hr = EnsureEventQueue();
    if (FAILED(hr))
        return hr;
    return m_pEventQueue->GetEvent(dwFlags, ppEvent);
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::BeginGetEvent(IMFAsyncCallback* pCallback,
                                               IUnknown* punkState)
{
    if (pCallback == NULL)
        return E_POINTER;
    if (!m_bAsync)
        return MF_E_INVALIDREQUEST;

    HRESULT hr = EnsureEventQueue();
    if (FAILED(hr))
        return hr;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    if (ppEvent == NULL)
        return E_POINTER;
    if (!m_bAsync)
        return MF_E_INVALIDREQUEST;

    HRESULT hr = EnsureEventQueue();
    if (FAILED(hr))
        return hr;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                            HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    if (!m_bAsync)
        return MF_E_INVALIDREQUEST;

    HRESULT hr = EnsureEventQueue();
    if (FAILED(hr))
        return hr;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::Flush()
{
    // IMFMediaEventGenerator::Flush: drop the events that were queued but not read.
    // (IMFMediaEventQueue has no Flush of its own in this SDK, so drain it here.)
    if (!m_bAsync)
        return MF_E_INVALIDREQUEST;
    if (m_pEventQueue == NULL)
        return S_OK;
    IMFMediaEvent* pEvent = NULL;
    while (SUCCEEDED(m_pEventQueue->GetEvent(MF_EVENT_FLAG_NO_WAIT, &pEvent)))
        SAFE_RELEASE(pEvent);
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
HRESULT CAVSVideoDecoderMFT::EnsureEventQueue()
{
    if (m_pEventQueue != NULL)
        return S_OK;
    return MFCreateEventQueue(&m_pEventQueue);
}

//-------------------------------------------------------------------------------------------------
HRESULT CAVSVideoDecoderMFT::PostTransformEvent(MediaEventType met)
{
    if (!m_bAsync)
        return S_OK;

    HRESULT hr = EnsureEventQueue();
    if (FAILED(hr))
        return hr;

    // IMFMediaEvent is read only in this SDK, so the event is built by the queue.
    hr = m_pEventQueue->QueueEventParamVar(met, GUID_NULL, S_OK, NULL);
    if (AVS_MFT_TraceOn())
    {
        const char* psz = "event";
        if (met == METransformHaveOutput) psz = "METransformHaveOutput";
        else if (met == METransformDrainComplete) psz = "METransformDrainComplete";
        else if (met == METransformNeedInput) psz = "METransformNeedInput";
        fprintf(stderr, "[avsmft] %p posted %s -> 0x%08X\n", (void*)this, psz, (unsigned)hr);
    }
    return hr;
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::GetInputStatus(DWORD dwInputStreamID, DWORD* pdwFlags)
{
    if (pdwFlags == NULL)
        return E_POINTER;
    if (dwInputStreamID != m_InputStreamID)
        return MF_E_INVALIDSTREAMNUMBER;

    AutoLock lock(m_Lock);

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    // Not accepting data while the internal picture queue is full is what keeps the
    // memory footprint of the decoder bounded.  A drain does not stop acceptance.
    // Pictures held back from an over-sized burst count as "full": the pipeline has to
    // take output before we decode more, and that is exactly what it can do here.
    if (AVS_MFT_TraceOn())
        fprintf(stderr, "[avsmft] GetInputStatus: queue=%zu flush=%zu -> %s\n",
                m_OutputQueue.size(), m_FlushFrames.size(),
                (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES ||
                 !m_FlushFrames.empty()) ? "NOT-ACCEPTING" : "accept");
    if (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES || !m_FlushFrames.empty())
        return MF_E_NOTACCEPTING;

    *pdwFlags = MFT_INPUT_STATUS_ACCEPT_DATA;
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::GetOutputStatus(DWORD* pdwFlags)
{
    if (pdwFlags == NULL)
        return E_POINTER;

    AutoLock lock(m_Lock);

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (AVS_MFT_TraceOn())
        fprintf(stderr, "[avsmft] GetOutputStatus: queue=%zu -> %s\n", m_OutputQueue.size(),
                m_OutputQueue.empty() ? "nothing" : "sample ready");
    *pdwFlags = m_OutputQueue.empty() ? 0 : MFT_OUTPUT_STATUS_SAMPLE_READY;
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// One decoded picture per call.  MF_E_TRANSFORM_NEED_MORE_INPUT while the decoder has
// an internal delay, MF_E_TRANSFORM_STREAM_CHANGE when the bit stream told us a frame
// size that differs from the negotiated output type.
STDMETHODIMP CAVSVideoDecoderMFT::ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
                                                MFT_OUTPUT_DATA_BUFFER* pOutputSamples,
                                                DWORD* pdwStatus)
{
    if (pOutputSamples == NULL || cOutputBufferCount == 0)
        return E_INVALIDARG;

    AutoLock lock(m_Lock);

    if (pdwStatus != NULL)
        *pdwStatus = 0;

    for (DWORD i = 0; i < cOutputBufferCount; i++)
        pOutputSamples[i].dwStatus = 0;

    if (m_pInputType == NULL || m_pOutputType == NULL)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (m_bStreamChangePending)
    {
        m_bStreamChangePending = FALSE;
        // This used to be silent, which made every "no stream change happened" claim built
        // from log greps vacuous: the string was never there to grep for.  The session
        // reacts to this return by renegotiating and (for a decoder in a running topology)
        // can restart a branch, so it belongs in the end-of-stream timeline.
        if (AVS_MFT_TraceOn())
            fprintf(stderr, "[avsmft] %p raising MF_E_TRANSFORM_STREAM_CHANGE\n",
                    (void*)this);
        pOutputSamples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        if (pdwStatus != NULL)
            *pdwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
        return MF_E_TRANSFORM_STREAM_CHANGE;
    }

    // A burst that was larger than the picture queue is held here; put it back as soon
    // as taking this sample made room.  Without this the held pictures would wait for a
    // ProcessInput that an end-of-stream pipeline never sends.
    PumpFlushFramesIntoQueue();

    if (m_bDrain && (!m_bDecoderFlushed || !m_FlushFrames.empty()))
    {
        // The decoder may still hold pictures; pull them out (Flush() is a no-op when
        // libavcodec already reached EOF).  Doing this before the picture that is
        // already queued goes out is what makes it possible to tell whether that
        // picture really was the last one.
        IF_FAILED_RETURN(FlushDecoderToQueue());
    }

    if (AVS_MFT_HoldLast() && m_bDrain == FALSE && !m_OutputQueue.empty() &&
        m_OutputQueue.size() + m_FlushFrames.size() <= 1)
    {
        // one picture of deliberate latency, see AVS_MFT_HoldLast()
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    if (m_OutputQueue.empty())
    {
        if (AVS_MFT_TraceOn())
            fprintf(stderr, "[avsmft] NO-OUTPUT: picture queue empty, asking for more input\n");
        // MFT_OUTPUT_DATA_BUFFER_INCOMPLETE is deliberately not set here: this output
        // stream provides its own samples, and telling the pipeline that a partial
        // sample exists makes it wait for something that never arrives.
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    IMFSample* pSample = m_OutputQueue.front();
    m_OutputQueue.erase(m_OutputQueue.begin());
    pOutputSamples[0].pSample = pSample;   // ownership moves to the caller

    // Tell the pipeline that this was the last picture of the stream.  A decoder whose
    // output stream provides its own samples has to mark the end explicitly: nothing
    // else in the stream says "nothing more is coming", and a host that does not learn
    // it never reports the presentation as finished (no MESessionEnded).
    const bool bStreamEnd = m_bOutputStreamEnd || (AVS_MFT_DrainEOS() && m_bDrain);
    if (bStreamEnd && m_OutputQueue.empty() && m_FlushFrames.empty() &&
        m_bDecoderFlushed)
    {
        pOutputSamples[0].dwStatus = MFT_OUTPUT_DATA_BUFFER_STREAM_END;
        if (pdwStatus != NULL)
            *pdwStatus = MFT_OUTPUT_DATA_BUFFER_STREAM_END;
        if (AVS_MFT_TraceOn())
            fprintf(stderr, "[avsmft] %p marked the last output sample as stream end\n",
                    (void*)this);
    }

    // hand-off trace: "emitted" above only means the picture was queued; this is the
    // moment the pipeline really takes it.  The difference shows when a host stops
    // pulling output (the end of a presentation).
    if (AVS_MFT_TraceOn())
    {
        static volatile LONG s_nHanded = 0;
        LONGLONG rtDbg = -1;
        pSample->GetSampleTime(&rtDbg);
        fprintf(stderr, "[avsmft] %p handed to graph #%ld time=%lld (queue left %zu) "
                        "endflag=%d flushed=%d hold=%zu\n",
                (void*)this, (long)InterlockedIncrement(&s_nHanded), rtDbg,
                m_OutputQueue.size(), (int)m_bOutputStreamEnd, (int)m_bDecoderFlushed,
                m_FlushFrames.size());
    }
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// Queue a converted picture as an IMFMediaBuffer + IMFSample.
HRESULT CAVSVideoDecoderMFT::QueueOutputFrame(const FFmpegVideoDecoder::Frame& frame)
{
    if (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES)
    {
        // Distinct from REFUSE-INPUT(B): this happens in the middle of one access unit,
        // after the decoder released a burst larger than the picture queue.  The pictures
        // that did not fit are dropped by the caller's loop - that is data loss, so it
        // has to say which site it came from.
        if (AVS_MFT_TraceOn())
            fprintf(stderr, "[avsmft] REFUSE-INPUT(A): picture queue full while queueing a "
                            "decoded picture, queue=%zu\n", m_OutputQueue.size());
        return MF_E_NOTACCEPTING;
    }

    // Did the bit stream just tell us a different picture size?
    UINT32 negotiatedW = 0, negotiatedH = 0;
    MFGetAttributeSize(m_pOutputType, MF_MT_FRAME_SIZE, &negotiatedW, &negotiatedH);
    if (negotiatedW != frame.width || negotiatedH != frame.height)
    {
        // The picture that is being queued is already in the new size, so it stays in
        // the queue: the pipeline will call GetOutputAvailableType + SetOutputType and
        // then read it back.
        IF_FAILED_RETURN(UpdateOutputTypeForDecodedSize(frame.width, frame.height));
    }

    IMFMediaBuffer* pBuffer = NULL;
    IMFSample* pSample = NULL;

    HRESULT hr = MFCreateMemoryBuffer((DWORD)frame.pixels.size(), &pBuffer);
    if (FAILED(hr))
        return hr;

    BYTE* pbData = NULL;
    DWORD cbMaxLength = 0;
    hr = pBuffer->Lock(&pbData, &cbMaxLength, NULL);
    if (SUCCEEDED(hr))
    {
        if (cbMaxLength < frame.pixels.size())
            hr = E_UNEXPECTED;
        else
        {
            memcpy(pbData, frame.pixels.data(), frame.pixels.size());
            pBuffer->Unlock();
        }
    }
    if (SUCCEEDED(hr))
        hr = pBuffer->SetCurrentLength((DWORD)frame.pixels.size());

    if (SUCCEEDED(hr))
        hr = MFCreateSample(&pSample);
    if (SUCCEEDED(hr))
        hr = pSample->AddBuffer(pBuffer);
    if (SUCCEEDED(hr) && frame.bHasTime)
        pSample->SetSampleTime(frame.rtStart);

    // Every output sample needs a duration.  The last picture of a stream has no
    // "next picture" to derive one from and a compressed packet may not carry a
    // duration at all, which leaves the renderer holding an untimeable final frame.
    // Fall back to the frame rate of the negotiated output type.
    LONGLONG rtDur = (frame.rtStop > frame.rtStart) ? (frame.rtStop - frame.rtStart) : 0;
    if (rtDur <= 0)
        rtDur = DefaultFrameDurationHns();
    if (SUCCEEDED(hr) && rtDur > 0)
        pSample->SetSampleDuration(rtDur);

    SAFE_RELEASE(pBuffer);

    if (FAILED(hr))
    {
        SAFE_RELEASE(pSample);
        return hr;
    }

    // progress trace: a host that owns no logging of its own (MFPlay, a media
    // session, a system player) can still show that the graph is advancing.
    if (AVS_MFT_TraceOn())
    {
        static volatile LONG s_nEmitted = 0;
        const LONG n = InterlockedIncrement(&s_nEmitted);
        fprintf(stderr, "[avsmft] emitted #%ld time=%lld dur=%lld\n", (long)n,
                frame.bHasTime ? frame.rtStart : (LONGLONG)-1, rtDur);
    }

    m_OutputQueue.push_back(pSample);       // the queue owns the reference
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
// Duration implied by the frame rate of the negotiated output type, in 100 ns units.
// Used for pictures that carry no timing information of their own.
LONGLONG CAVSVideoDecoderMFT::DefaultFrameDurationHns()
{
    if (m_pOutputType == NULL)
        return 0;

    UINT32 num = 0, den = 0;
    if (FAILED(MFGetAttributeRatio(m_pOutputType, MF_MT_FRAME_RATE, &num, &den)) ||
        num == 0 || den == 0)
        return 0;

    return (LONGLONG)(((double)den / (double)num) * 10000000.0);
}

//-------------------------------------------------------------------------------------------------
// Move held pictures back into the output queue while there is room.  Called whenever
// room can appear: before decoding another access unit, and after a sample was handed to
// the pipeline.
void CAVSVideoDecoderMFT::PumpFlushFramesIntoQueue()
{
    while (!m_FlushFrames.empty() && m_OutputQueue.size() < AVS_MFT_MAX_QUEUED_FRAMES)
    {
        if (FAILED(QueueOutputFrame(m_FlushFrames.front())))
        {
            // A picture that cannot be converted is a decoder/format problem, not a
            // queue problem; say so rather than dropping it in silence.
            if (AVS_MFT_TraceOn())
                fprintf(stderr, "[avsmft] held picture could not be queued, dropped\n");
            m_FlushFrames.erase(m_FlushFrames.begin());
            continue;
        }
        m_FlushFrames.erase(m_FlushFrames.begin());
        PostTransformEvent(METransformHaveOutput);
    }
}

//-------------------------------------------------------------------------------------------------
// Pull every picture libavcodec still holds (called at drain / EOS).  Pictures that do
// not fit into the output queue stay in m_FlushFrames and go out with the next call;
// dropping them would lose the last pictures of the stream.
HRESULT CAVSVideoDecoderMFT::FlushDecoderToQueue()
{
    if (!m_Decoder.IsOpen())
        return S_OK;

    PumpFlushFramesIntoQueue();
    if (!m_FlushFrames.empty())
        return S_OK;                        // the queue is full again, the rest waits

    if (m_bDecoderFlushed)
        return S_OK;                        // libavcodec was already drained

    std::vector<FFmpegVideoDecoder::Frame> frames;
    const HRESULT hr = m_Decoder.Flush(frames);
    m_bDecoderFlushed = TRUE;
    NoteDecoderColor();

    size_t i = 0;
    for (; i < frames.size(); i++)
    {
        if (m_OutputQueue.size() >= AVS_MFT_MAX_QUEUED_FRAMES)
            break;
        IF_FAILED_RETURN(QueueOutputFrame(frames[i]));
    }
    for (; i < frames.size(); i++)
        m_FlushFrames.push_back(std::move(frames[i]));
    return hr;
}

//-------------------------------------------------------------------------------------------------
// Rebuild the output type with the picture size that the AVS sequence header gave us.
//-------------------------------------------------------------------------------------------------
// Take the colour signalling from the bit stream.  The AVS3 wrapper (and any decoder
// that parses a sequence header with a colour descriptor) puts it on every decoded
// picture, which is the only place it can be found for a raw elementary stream.  The
// values already inherited from the compressed input type are kept if the bit stream
// does not contradict them.
//
// Callers hold m_Lock in the synchronous path; in the asynchronous mode this runs on
// the worker thread, where a stale read would at worst delay the metadata until the
// next output type is built.
//-------------------------------------------------------------------------------------------------
void CAVSVideoDecoderMFT::NoteDecoderColor()
{
    const FFmpegVideoDecoder::Color c = m_Decoder.ColorInfo();

    AvsColorInfo info;
    info.primaries = c.primaries;
    info.transfer  = c.transfer;
    info.matrix    = c.matrix;
    info.range     = c.range;

    if (info.IsEmpty())
        return;

    m_Color.Merge(info);

    // If the negotiated output type says nothing about the sample values but the bit
    // stream does, the description has to be renegotiated: otherwise the renderer
    // converts the pictures with its own guess and the information is lost.  Only once
    // per decoder instance - if the host answers with a bare type there is nothing to
    // gain from asking again, and repeating it would stall the stream.
    if (m_pOutputType == NULL || m_bColorChangeSignalled == TRUE || m_bDrain == TRUE)
        return;

    UINT32 v = 0;
    bool missing = false;
    if (m_Color.primaries != AVCOL_PRI_UNSPECIFIED &&
        FAILED(m_pOutputType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v)))
        missing = true;
    if (m_Color.transfer != AVCOL_TRC_UNSPECIFIED &&
        FAILED(m_pOutputType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v)))
        missing = true;
    if (m_Color.range != AVCOL_RANGE_UNSPECIFIED &&
        FAILED(m_pOutputType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)))
        missing = true;

    if (missing)
    {
        m_bColorChangeSignalled = TRUE;
        m_bStreamChangePending = TRUE;
    }
}

HRESULT CAVSVideoDecoderMFT::UpdateOutputTypeForDecodedSize(UINT32 width, UINT32 height)
{
    IMFMediaType* pNew = NULL;
    IF_FAILED_RETURN(CreateOutputMediaType(width, height, &pNew));
    AvsDumpType("mft-out-live", pNew);   // measurement only

    IMFMediaType* pOld = m_pOutputType;
    m_pOutputType = pNew;
    SAFE_RELEASE(pOld);

    m_bStreamChangePending = TRUE;
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
void CAVSVideoDecoderMFT::ClearOutputQueue()
{
    for (size_t i = 0; i < m_OutputQueue.size(); i++)
        SAFE_RELEASE(m_OutputQueue[i]);
    m_OutputQueue.clear();
    m_FlushFrames.clear();
    m_bDecoderFlushed = FALSE;
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::SetOutputBounds(LONGLONG, LONGLONG)
{
    return E_NOTIMPL;
}

STDMETHODIMP CAVSVideoDecoderMFT::ProcessEvent(DWORD dwInputStreamID, IMFMediaEvent* pEvent)
{
    // A synchronous MFT is not supposed to receive media events, but the graph does
    // send some (MEStreamTearDown, and possibly the end-of-stream notification).  The
    // MFT handler treats a failure here as "this branch cannot be finished", which is
    // exactly what stopped the session from ever reporting MESessionEnded, so accept
    // the event and log what actually arrives.
    if (pEvent == NULL)
        return E_POINTER;

    if (AVS_MFT_TraceOn())
    {
        MediaEventType mt = 0;
        pEvent->GetType(&mt);
        fprintf(stderr, "[avsmft] %p ProcessEvent stream=%u event=0x%X\n",
                (void*)this, (unsigned)dwInputStreamID, (unsigned)mt);
    }
    return S_OK;
}

//-------------------------------------------------------------------------------------------------
STDMETHODIMP CAVSVideoDecoderMFT::ProcessMessage(MFT_MESSAGE_TYPE eMessage, ULONG_PTR ulParam)
{
    AutoLock lock(m_Lock);

    // which control messages the graph actually sends (drain, end of stream, flush).
    // NOTE the real enum values: NOTIFY_END_OF_STREAM is 0x10000002 and
    // NOTIFY_START_OF_STREAM is 0x10000003, SET_D3D_MANAGER is 2, MARKER is 0x20000000.
    if (AVS_MFT_TraceOn())
    {
        const char* pszName = "?";
        switch ((DWORD)eMessage)
        {
        case MFT_MESSAGE_COMMAND_FLUSH:             pszName = "COMMAND_FLUSH"; break;
        case MFT_MESSAGE_COMMAND_DRAIN:             pszName = "COMMAND_DRAIN"; break;
        case MFT_MESSAGE_SET_D3D_MANAGER:           pszName = "SET_D3D_MANAGER"; break;
        case MFT_MESSAGE_DROP_SAMPLES:              pszName = "DROP_SAMPLES"; break;
        case MFT_MESSAGE_COMMAND_TICK:              pszName = "COMMAND_TICK"; break;
        case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:    pszName = "NOTIFY_BEGIN_STREAMING"; break;
        case MFT_MESSAGE_NOTIFY_END_STREAMING:      pszName = "NOTIFY_END_STREAMING"; break;
        case MFT_MESSAGE_NOTIFY_END_OF_STREAM:      pszName = "NOTIFY_END_OF_STREAM"; break;
        case MFT_MESSAGE_NOTIFY_START_OF_STREAM:    pszName = "NOTIFY_START_OF_STREAM"; break;
        case MFT_MESSAGE_NOTIFY_RELEASE_RESOURCES:  pszName = "NOTIFY_RELEASE_RESOURCES"; break;
        case MFT_MESSAGE_NOTIFY_REACQUIRE_RESOURCES:pszName = "NOTIFY_REACQUIRE_RESOURCES"; break;
        case MFT_MESSAGE_NOTIFY_EVENT:              pszName = "NOTIFY_EVENT"; break;
        case MFT_MESSAGE_COMMAND_MARKER:            pszName = "COMMAND_MARKER"; break;
        }
        fprintf(stderr, "[avsmft] %p ProcessMessage %s (0x%X)\n", (void*)this, pszName,
                (unsigned)eMessage);
    }

    switch ((DWORD)eMessage)
    {
    case MFT_MESSAGE_COMMAND_FLUSH:
        // seek: drop everything decoded so far and restart the bit stream parser
        ClearOutputQueue();
        DestroyDecoder();
        m_bDrain = FALSE;
        m_bStreamChangePending = FALSE;
        m_bOutputStreamEnd = FALSE;
        {
            std::lock_guard<std::mutex> lk(m_AsyncLock);
            m_AsyncIn.clear();        // a seek invalidates the queued access units
        }
        PostTransformEvent(METransformNeedInput);
        return S_OK;

    case MFT_MESSAGE_COMMAND_DRAIN:
        m_bDrain = TRUE;
        if (m_bAsync)
            return EnqueueDrain();   // the worker flushes and posts the events
        {
            const size_t nBefore = m_OutputQueue.size();
            const HRESULT hrD = FlushDecoderToQueue();
            if (SUCCEEDED(hrD))
            {
                for (size_t i = m_OutputQueue.size(); i > nBefore; i--)
                    PostTransformEvent(METransformHaveOutput);
                // the asynchronous model's "the drain is over": without it a pipeline
                // that waits for events waits forever
                PostTransformEvent(METransformDrainComplete);
            }
            return hrD;
        }

    case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
        m_bInputStreamActivated = TRUE;
        m_bOutputStreamEnd = FALSE;      // a new run of the stream starts
        if (m_bAsync && !m_bWorkerRunning)
        {
            // the pipeline of an asynchronous MFT waits for the first
            // METransformNeedInput before it feeds anything, so start the worker and
            // send it
            if (SUCCEEDED(EnsureEventQueue()))
            {
                m_bWorkerStop = FALSE;
                m_Worker = std::thread(&CAVSVideoDecoderMFT::WorkerThread, this);
                m_bWorkerRunning = TRUE;
                PostTransformEvent(METransformNeedInput);
            }
        }
        return S_OK;

    case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
    case MFT_MESSAGE_NOTIFY_END_STREAMING:
    case MFT_MESSAGE_COMMAND_MARKER:
    case MFT_MESSAGE_COMMAND_TICK:
    case MFT_MESSAGE_DROP_SAMPLES:
        return S_OK;

    case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
        m_bDrain = TRUE;
        m_bOutputStreamEnd = TRUE;
        {
            const HRESULT hrE = FlushDecoderToQueue();
            if (SUCCEEDED(hrE))
                PostTransformEvent(METransformDrainComplete);
            return hrE;
        }

    case MFT_MESSAGE_SET_D3D_MANAGER:
        // No D3D/DXVA decode path yet (see docs/mft-design.md): the ported libcavs
        // decoder is a CPU decoder.  Refusing here keeps the MFT handler on the
        // system-memory path, which is what we want.
        UNREFERENCED_PARAMETER(ulParam);
        return MF_E_INVALIDREQUEST;

    default:
        UNREFERENCED_PARAMETER(ulParam);
        return S_OK;
    }
}
