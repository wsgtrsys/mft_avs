//--------------------------------------------------------------------------------------------------
// avsaudio_test.cpp - exercise the DRA (AVS audio) path.
//
//   avsaudio-test <file> [out.wav] [-mft <dll>] [-frames N] [-direct]
//
//   -direct     decode with libavcodec only (reference)
//   -mft <dll>  feed the compressed packets to the DRA audio decoder MFT
//
// The demuxing is done with the same FFmpeg build that the media source uses, so
// this also shows what the media source will see for a DRA stream (stream type,
// time base, packet sizes).
//--------------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include "crashreport.h"
#include <stdlib.h>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#pragma comment(lib, "mfplat")
#pragma comment(lib, "shell32")   // CommandLineToArgvW
#pragma comment(lib, "mf")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")

// FFmpeg static builds reference Winsock / SChannel / user32 entry points.
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "secur32")
#pragma comment(lib, "crypt32")
#pragma comment(lib, "ncrypt")
#pragma comment(lib, "user32")

// must match classGuids.h of the decoder DLL
static const GUID CLSID_AVSAudioDecoder =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x12 } };
static const GUID MFAudioFormat_DRA =
    { 0x20415244, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// copy out the PCM of a sample handed back by ProcessOutput (an MFT that says
// MFT_OUTPUT_STREAM_PROVIDES_SAMPLES replaces pSample, so read ob.pSample)
static ULONG DrainOutputSample(MFT_OUTPUT_DATA_BUFFER& ob, std::vector<BYTE>& pcm, IMFSample** ppGot)
{
    IMFSample* pGot = ob.pSample;
    if (pGot == NULL)
        return 0;
    pGot->AddRef();
    *ppGot = pGot;

    IMFMediaBuffer* pBuf = NULL;
    ULONG cb = 0;
    if (SUCCEEDED(pGot->ConvertToContiguousBuffer(&pBuf)))
    {
        BYTE* data = NULL;
        if (SUCCEEDED(pBuf->Lock(&data, NULL, &cb)))
        {
            pcm.insert(pcm.end(), data, data + cb);
            pBuf->Unlock();
        }
        pBuf->Release();
    }
    return cb;
}

struct Options
{
    std::wstring input;
    std::wstring output = L"out.wav";
    std::wstring mft;                 // empty -> -direct
    int          maxFrames = -1;
    bool         trace = false;       // per packet / per frame timing details
};

static void Die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static std::wstring WS(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring o(n ? n - 1 : 0, 0);
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &o[0], n);
    return o;
}

static const char* TagStr(unsigned tag)
{
    static char buf[8];
    if (tag == 0)
        return "";
    snprintf(buf, sizeof(buf), "%c%c%c%c", (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
             (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF));
    return buf;
}

static std::string W(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &o[0], n, NULL, NULL);
    return o;
}

static void WriteWavHeader(FILE* f, UINT32 rate, WORD channels, WORD bits, UINT32 pcmBytes)
{
    WORD  align = (WORD)(channels * bits / 8);
    DWORD byteRate = rate * align;
    WORD  fmtTag = 1;                                  // PCM
    DWORD cbData = pcmBytes;
    DWORD total  = 36 + pcmBytes;

    fwrite("RIFF", 1, 4, f);  fwrite(&total,   4, 1, f);
    fwrite("WAVE", 1, 4, f);  fwrite("fmt ", 1, 4, f);
    DWORD cbFmt = 16;
    fwrite(&cbFmt, 4, 1, f);
    fwrite(&fmtTag, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&cbData, 4, 1, f);
}

static std::string NarrowAscii(const std::wstring& w)   // flags are plain ASCII
{
    std::string s(w.size(), '\0');
    for (size_t i = 0; i < w.size(); i++)
        s[i] = (w[i] < 128) ? (char)w[i] : '?';
    return s;
}

// the CRT argv is ANSI (under MSYS it is the OEM code page) and mangles non-ASCII
// file names, so take the real wide command line
static std::vector<std::wstring> WideArgs(int argc, char** argv)
{
    std::vector<std::wstring> v;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w)
    {
        for (int i = 0; i < n; i++)
            v.push_back(w[i]);
        LocalFree(w);
    }
    if ((int)v.size() < argc)
    {
        v.clear();
        for (int i = 0; i < argc; i++)
            v.push_back(WS(argv[i]));
    }
    return v;
}

int main(int argc, char** argv)
{
    // keep the progress readable when the run dies in the middle of a stream
    InstallCrashReport();

    Options opt;
    std::vector<std::wstring> args = WideArgs(argc, argv);
    for (int i = 1; i < (int)args.size(); i++)
    {
        std::string a = NarrowAscii(args[i]);
        if (a == "-mft" && i + 1 < (int)args.size())         opt.mft = args[++i];
        else if (a == "-frames" && i + 1 < (int)args.size()) opt.maxFrames = atoi(NarrowAscii(args[++i]).c_str());
        else if (a == "-trace")                   opt.trace = true;
        else if (a == "-direct")                  opt.mft.clear();
        else if (opt.input.empty())               opt.input = args[i];
        else                                      opt.output = args[i];
    }
    if (opt.input.empty())
        Die("usage: avsaudio-test <file> [out.wav] [-mft <dll> | -direct] [-frames N]");

    std::string in = W(opt.input);

    AVFormatContext* fmtCtx = NULL;
    if (avformat_open_input(&fmtCtx, in.c_str(), NULL, NULL) < 0)
        Die("cannot open [%s] (%d bytes)", in.c_str(), (int)in.size());
    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
        Die("cannot read stream info");

    int audioIndex = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audioIndex = (int)i;
    if (audioIndex < 0)
        Die("no audio stream in %s", in.c_str());

    AVStream* st = fmtCtx->streams[audioIndex];
    AVCodecParameters* par = st->codecpar;
    printf("input:   %s\n", in.c_str());
    printf("         stream %d: %s tag=%s  ch=%d rate=%d bits=%d frame_size=%d\n",
           audioIndex, avcodec_get_name(par->codec_id),
           TagStr(par->codec_tag),
           par->ch_layout.nb_channels, par->sample_rate, par->bits_per_coded_sample,
           par->frame_size);
    printf("         time base 1/%d, duration %.3fs, start %.3fs\n",
           st->time_base.den,
           st->duration != AV_NOPTS_VALUE ? (double)st->duration * av_q2d(st->time_base) : 0.0,
           st->start_time != AV_NOPTS_VALUE ? (double)st->start_time * av_q2d(st->time_base) : 0.0);

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
        Die("CoInitializeEx failed");
    if (FAILED(MFStartup(MF_VERSION)))
        Die("MFStartup failed");

    IMFTransform* pMFT = NULL;
    if (!opt.mft.empty())
    {
        HMODULE h = LoadLibraryExW(opt.mft.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h == NULL)
            Die("LoadLibrary(%S) failed, 0x%08X", opt.mft.c_str(), GetLastError());
        typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
        GetClassObjectFn pGet = (GetClassObjectFn)(FARPROC)GetProcAddress(h, "DllGetClassObject");
        if (pGet == NULL)
            Die("no DllGetClassObject in %S", opt.mft.c_str());
        IClassFactory* pFact = NULL;
        HRESULT hr = pGet(CLSID_AVSAudioDecoder, IID_IClassFactory, (void**)&pFact);
        if (FAILED(hr))
            Die("DllGetClassObject(audio decoder) -> 0x%08X  (is the CLSID the one in classGuids.h?)", hr);
        hr = pFact->CreateInstance(NULL, IID_IMFTransform, (void**)&pMFT);
        pFact->Release();
        if (FAILED(hr))
            Die("CreateInstance -> 0x%08X", hr);
        printf("mft:     %S\n", opt.mft.c_str());
    }
    else
        printf("mode:    direct libavcodec (reference)\n");

    // ---------------------------------------------------------------- decode
    UINT64 pcmTotal = 0;
    int   nFrames = 0, nFed = 0;
    DWORD rate = 0, channels = 0, bits = 16;

    std::vector<BYTE> pcm;      // collected output, written as a WAV at the end
    std::vector<std::pair<LONGLONG, LONGLONG>> times;

    if (pMFT != NULL)
    {
        // ------------------------------------------------------------------ MFT
        IMFMediaType* pIn = NULL;
        MFCreateMediaType(&pIn);
        pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_DRA);
        pIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)(par->ch_layout.nb_channels ? par->ch_layout.nb_channels : 2));
        if (par->sample_rate)
            pIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)par->sample_rate);
        HRESULT hr = pMFT->SetInputType(0, pIn, 0);
        if (FAILED(hr))
            Die("SetInputType(DRA) -> 0x%08X", hr);

        IMFMediaType* pOut = NULL;
        for (DWORD i = 0; i < 8; i++)
        {
            hr = pMFT->GetOutputAvailableType(0, i, &pOut);
            if (FAILED(hr))
                break;
            GUID sub = GUID_NULL;
            pOut->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 ch = 0, sr = 0, bps = 0, blockAlign = 0, validBits = 0;
            pOut->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            pOut->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
            pOut->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
            pOut->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlign);
            pOut->GetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &validBits);
            printf("         output type %d: {%08X-%04X-%04X-...} ch=%u rate=%u bits=%u align=%u valid=%u\n",
                   i, sub.Data1, sub.Data2, sub.Data3, ch, sr, bps, blockAlign, validBits);
            pOut->Release();
        }

        IMFMediaType* pOutType = NULL;
        hr = pMFT->GetOutputAvailableType(0, 0, &pOutType);
        if (FAILED(hr))
            Die("GetOutputAvailableType -> 0x%08X", hr);
        hr = pMFT->SetOutputType(0, pOutType, 0);
        GUID sub = GUID_NULL;
        pOutType->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 ch = 0, sr = 0, bps = 0;
        pOutType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        pOutType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        pOutType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
        printf("         negotiated output: {%08X-%04X-%04X-...} ch=%u rate=%u bits=%u -> 0x%08X\n",
               sub.Data1, sub.Data2, sub.Data3, ch, sr, bps, hr);
        pOutType->Release();
        rate = sr; channels = ch; bits = bps ? bps : 16;

        IMFMediaBuffer* pBuf = NULL;
        MFT_OUTPUT_STREAM_INFO osi = {};
        pMFT->GetOutputStreamInfo(0, &osi);
        UINT32 outCb = osi.cbSize ? osi.cbSize : 64 * 1024;

        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(fmtCtx, pkt) >= 0)
        {
            if (pkt->stream_index != audioIndex)
            {
                av_packet_unref(pkt);
                continue;
            }
            IMFSample* pSample = NULL;
            MFCreateSample(&pSample);
            MFCreateMemoryBuffer((ULONG)pkt->size, &pBuf);
            BYTE* pb = NULL;
            pBuf->Lock(&pb, NULL, NULL);
            memcpy(pb, pkt->data, pkt->size);
            pBuf->Unlock();
            pBuf->SetCurrentLength((ULONG)pkt->size);
            pSample->AddBuffer(pBuf);
            AVRational t100ns = { 1, 10000000 };
            LONGLONG t0  = (LONGLONG)av_rescale_q(pkt->pts, st->time_base, t100ns);
            LONGLONG dur = (LONGLONG)av_rescale_q(pkt->duration, st->time_base, t100ns);
            if (nFed < 4)
                printf("         pkt %d: pts=%lld -> t=%lld dur=%lld size=%d\n", nFed,
                       (long long)pkt->pts, t0, dur, pkt->size);
            pSample->SetSampleTime(t0);
            pSample->SetSampleDuration(dur);
            pBuf->Release();

            HRESULT h2 = pMFT->ProcessInput(0, pSample, 0);
            pSample->Release();
            av_packet_unref(pkt);
            if (FAILED(h2))
            {
                printf("         ProcessInput -> 0x%08X\n", h2);
                break;
            }
            nFed++;

            for (;;)
            {
                IMFSample* pOutSample = NULL;
                MFT_OUTPUT_DATA_BUFFER ob = {};
                DWORD status = 0;
                MFCreateSample(&pOutSample);
                MFCreateMemoryBuffer(outCb, &pBuf);
                pOutSample->AddBuffer(pBuf);
                pBuf->Release();
                ob.pSample = pOutSample;

                HRESULT h3 = pMFT->ProcessOutput(0, 1, &ob, &status);
                if (h3 == MF_E_TRANSFORM_NEED_MORE_INPUT)
                {
                    pOutSample->Release();
                    break;
                }
                if (FAILED(h3))
                {
                    printf("         ProcessOutput -> 0x%08X\n", h3);
                    pOutSample->Release();
                    break;
                }
                IMFSample* pGot = NULL;
                ULONG cb = DrainOutputSample(ob, pcm, &pGot);
                pcmTotal += cb;
                if (pGot != NULL)
                {
                    LONGLONG t0 = 0, dur = 0;
                    pGot->GetSampleTime(&t0);
                    pGot->GetSampleDuration(&dur);
                    times.push_back(std::make_pair(t0, dur));
                    pGot->Release();
                }
                nFrames++;
                pOutSample->Release();
                if (opt.maxFrames > 0 && nFrames >= opt.maxFrames)
                    break;
            }
            if (opt.maxFrames > 0 && nFrames >= opt.maxFrames)
                break;
        }
        av_packet_unref(pkt);

        // signal end of input, then pull whatever the decoder still holds
        pMFT->ProcessInput(0, NULL, 0);
        for (int guard = 0; guard < 500; guard++)
        {
            IMFSample* pOutSample = NULL;
            MFT_OUTPUT_DATA_BUFFER ob = {};
            DWORD status = 0;
            MFCreateSample(&pOutSample);
            MFCreateMemoryBuffer(outCb, &pBuf);
            pOutSample->AddBuffer(pBuf);
            pBuf->Release();
            ob.pSample = pOutSample;
            HRESULT h3 = pMFT->ProcessOutput(0, 1, &ob, &status);
            if (h3 == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                pOutSample->Release();
                break;
            }
            if (FAILED(h3))
            {
                pOutSample->Release();
                break;
            }
            IMFSample* pGot = NULL;
            pcmTotal += DrainOutputSample(ob, pcm, &pGot);
            if (pGot != NULL)
                pGot->Release();
            pOutSample->Release();
            nFrames++;
        }
        // audio must come out in strictly increasing time order, each block starting
        // where the previous one ended (sample count * 100ns / rate)
        bool ordered = true;
        LONGLONG last = (times.empty() ? 0 : times[0].first + times[0].second);
        for (size_t i = times.empty() ? 0 : 1; i < times.size(); i++)
        {
            if (times[i].first != last && ordered)
            {
                printf("         time discontinuity at block %d: previous ends %lld, next starts %lld (delta %+lld)\n",
                       (int)i, last, times[i].first, times[i].first - last);
                ordered = false;
            }
            last = times[i].first + times[i].second;
        }
        if (!times.empty())
        {
            printf("         sample times: %lld .. %lld (100ns), %d blocks, %.3f s of audio [%s]\n",
                   times.front().first, times.back().first, (int)times.size(),
                   (double)(times.back().first + times.back().second - times.front().first) / 1e7,
                   ordered ? "contiguous OK" : "GAPS/OVERLAP");
        }
        pMFT->Release();
    }
    else
    {
        // ------------------------------------------------------------------ direct
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        if (dec == NULL)
        {
            printf("no decoder for %s; codecs in this build:\n", avcodec_get_name(par->codec_id));
            void* it = NULL;
            const AVCodec* c = NULL;
            while ((c = av_codec_iterate(&it)) != NULL)
                if (av_codec_is_decoder(c))
                    printf("         dec %s id=%s\n", c->name, avcodec_get_name(c->id));
            Die("no decoder for %s in this FFmpeg build", avcodec_get_name(par->codec_id));
        }
        AVCodecContext* c = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(c, par);
        c->pkt_timebase = st->time_base;
        if (avcodec_open2(c, dec, NULL) < 0)
            Die("avcodec_open2 failed");
        printf("direct:  decoder '%s'\n", dec->name);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        while (av_read_frame(fmtCtx, pkt) >= 0)
        {
            if (pkt->stream_index == audioIndex)
            {
                if (opt.trace)
                    printf("pkt %3d size=%6lld pts=%lld dts=%lld\n", nFed + 1,
                           (long long)pkt->size, (long long)pkt->pts, (long long)pkt->dts);
                avcodec_send_packet(c, pkt);
                nFed++;
            }
            av_packet_unref(pkt);
            while (avcodec_receive_frame(c, fr) == 0)
            {
                if (opt.trace)
                    printf("      frame nb_samples=%d pts=%lld be=%lld\n", fr->nb_samples,
                           (long long)fr->pts, (long long)fr->best_effort_timestamp);
                if (rate == 0)
                {
                    rate = (DWORD)fr->sample_rate;
                    channels = (DWORD)fr->ch_layout.nb_channels;
                    bits = fr->format == AV_SAMPLE_FMT_S16 ? 16 : (fr->format == AV_SAMPLE_FMT_S32 ? 32 : 32);
                    printf("         first frame: rate=%u ch=%u fmt=%s nb_samples=%d\n",
                           rate, channels, av_get_sample_fmt_name((AVSampleFormat)fr->format), fr->nb_samples);
                }
                int cb = av_samples_get_buffer_size(NULL, fr->ch_layout.nb_channels, fr->nb_samples,
                                                   (AVSampleFormat)fr->format, 1);
                if (cb > 0)
                {
                    pcm.insert(pcm.end(), fr->data[0], fr->data[0] + cb);
                    pcmTotal += cb;
                }
                nFrames++;
                av_frame_unref(fr);
                if (opt.maxFrames > 0 && nFrames >= opt.maxFrames)
                    break;
            }
            if (opt.maxFrames > 0 && nFrames >= opt.maxFrames)
                break;
        }
        avcodec_send_packet(c, NULL);
        while (avcodec_receive_frame(c, fr) == 0 && (opt.maxFrames <= 0 || nFrames < opt.maxFrames))
        {
            int cb = av_samples_get_buffer_size(NULL, fr->ch_layout.nb_channels, fr->nb_samples,
                                               (AVSampleFormat)fr->format, 1);
            if (cb > 0)
            {
                pcm.insert(pcm.end(), fr->data[0], fr->data[0] + cb);
                pcmTotal += cb;
            }
            nFrames++;
            av_frame_unref(fr);
        }
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&c);
    }

    if (rate == 0 || channels == 0)
        rate = rate ? rate : 48000, channels = channels ? channels : 2;
    FILE* f = _wfopen(opt.output.c_str(), L"wb");
    if (f != NULL)
    {
        WriteWavHeader(f, rate, (WORD)channels, (WORD)bits, (UINT32)pcm.size());
        if (!pcm.empty())
            fwrite(pcm.data(), 1, pcm.size(), f);
        fclose(f);
    }

    printf("result:  fed %d packets, %d audio frames, %llu PCM bytes -> %S\n",
           nFed, nFrames, pcmTotal, opt.output.c_str());
    if (rate && channels)
        printf("         WAV parameters: %u Hz, %u ch, %u bit\n", rate, channels, bits);
    else
        printf("         no PCM produced\n");

    MFShutdown();
    CoUninitialize();
    avformat_close_input(&fmtCtx);
    return nFrames > 0 ? 0 : 1;
}
