//--------------------------------------------------------------------------------------------------
// avsmft_test.cpp - decode an AVS / AVS+ / AVS2 / AVS3 file through the AVS MFT
// and write the decoded pictures out, so the decoder MFT can be verified without a
// player.
//
//   avsmft-test <input> [output.yuv] [-mft <path to AVSVideoDecoder.dll>]
//                              [-fmt nv12|i420] [-frames N] [-bmp N] [-nosize]
//
// The file is demuxed with FFmpeg (mpegts for a .ts, the cavsvideo elementary
// stream parser for a raw .avs), and every demuxed access unit is handed to the MFT
// as one IMFMediaBuffer/IMFSample - exactly what a media source would do.  The MFT
// is loaded through DllGetClassObject, so it does not have to be registered.
//
// What it proves:
//   * the MFT accepts an AVS sub-type and decodes it (frame count > 0)
//   * the MFT produces NV12/I420 pictures of the right size
//   * the decoded pixels match a reference FFmpeg decode (compare with -ref)
//--------------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <shlwapi.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include "crashreport.h"
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "avformat")
#pragma comment(lib, "avcodec")
#pragma comment(lib, "avutil")
#pragma comment(lib, "swscale")
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "ws2_32")     // avformat's network layer
#pragma comment(lib, "secur32")   // avformat's tls_schannel protocol
#pragma comment(lib, "crypt32")
#pragma comment(lib, "ncrypt")
#pragma comment(lib, "user32")    // avutil's dxva2 device enumeration
#pragma comment(lib, "shell32")   // CommandLineToArgvW

// The C runtime argv is ANSI (and under MSYS it arrives in the OEM code page),
// which mangles non-ASCII file names, so take the real wide command line.
static std::wstring utf8toW(const std::string& s);

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
            v.push_back(utf8toW(argv[i]));
    }
    return v;
}

static std::string NarrowAscii(const std::wstring& w)   // flags are plain ASCII
{
    std::string s(w.size(), '\0');
    for (size_t i = 0; i < w.size(); i++)
        s[i] = (w[i] < 128) ? (char)w[i] : '?';
    return s;
}

static std::string WToUtf8(const std::wstring& w)       // paths for libavformat
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring utf8toW(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ToLowerAscii(std::string s)
{
    for (size_t i = 0; i < s.size(); i++)
        s[i] = (char)tolower((unsigned char)s[i]);
    return s;
}

static std::string AVErrorString(int err)
{
    char buf[128] = { 0 };
    av_strerror(err, buf, sizeof(buf) - 1);
    return buf;
}

static int Clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// the CLSID of the MFT under test (must match AVSVideoDecoder/classGuids.h)
static const CLSID CLSID_AVSVideoDecoder_local_test =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x11 } };

// fourCC based MF sub-types, see classGuids.h
static const GUID MFVideoFormat_AVS  = { 0x20535641, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71} };
static const GUID MFVideoFormat_AVS2 = { 0x32535641, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71} };
static const GUID MFVideoFormat_AVS3 = { 0x33535641, 0x0000, 0x0010, {0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71} };

static void Die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    printf("ERROR: %s\n", buf);
    exit(1);
}

/* Write a decoded frame as 8 bit yuv420p.  libdavs2 and libuavs3d return
 * yuv420p10 for 10 bit streams, so convert when the decoder gives anything
 * else: the reference output only needs to be comparable by eye. */
static void WriteFrameYUV420P(FILE* f, AVFrame* fr, SwsContext** ppSws)
{
    if (fr->format == AV_PIX_FMT_YUV420P)
    {
        for (int plane = 0; plane < 3; plane++)
        {
            int w = plane ? fr->width / 2 : fr->width;
            int hh = plane ? fr->height / 2 : fr->height;
            for (int yy = 0; yy < hh; yy++)
                fwrite(fr->data[plane] + (size_t)yy * fr->linesize[plane], 1, w, f);
        }
        return;
    }

    const int w = fr->width, h = fr->height;

    /* swscale's x86 kernels read the destination with aligned loads, so the
     * scratch frame has to be allocated like any other image buffer. */
    static uint8_t* img[4] = { NULL, NULL, NULL, NULL };
    static int stride[4] = { 0, 0, 0, 0 };
    static int lastW = 0, lastH = 0;
    if (lastW != w || lastH != h)
    {
        if (img[0])
            av_free(img[0]);
        memset(img, 0, sizeof(img));
        if (av_image_alloc(img, stride, w, h, AV_PIX_FMT_YUV420P, 32) <= 0)
            Die("cannot allocate the conversion buffer");
        lastW = w;
        lastH = h;
    }

    if (!*ppSws)
        *ppSws = sws_getContext(w, h, (enum AVPixelFormat)fr->format, w, h,
                                AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!*ppSws)
        Die("cannot convert pixel format %d", fr->format);

    const int res = sws_scale(*ppSws, (const uint8_t* const*)fr->data, fr->linesize,
                              0, h, img, stride);
    if (res <= 0)
        Die("swscale failed for pixel format %d", fr->format);

    for (int plane = 0; plane < 3; plane++)
    {
        const int pw = plane ? w / 2 : w;
        const int ph = plane ? h / 2 : h;
        for (int yy = 0; yy < ph; yy++)
            fwrite(img[plane] + (size_t)yy * stride[plane], 1, pw, f);
    }
}

static HRESULT CreateMFT(const wchar_t* dllPath, IMFTransform** ppMFT)
{
    HMODULE hMod = LoadLibraryExW(dllPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (hMod == NULL)
    {
        wprintf(L"LoadLibraryEx failed for %s (GLE=%u)\n", dllPath, GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    auto getPfn = (FARPROC)GetProcAddress(hMod, "DllGetClassObject");
    if (getPfn == NULL)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
    GetClassObjectFn fn = (GetClassObjectFn)getPfn;

    IClassFactory* pFactory = NULL;
    HRESULT hr = fn(CLSID_AVSVideoDecoder_local_test, IID_IClassFactory, (void**)&pFactory);
    if (FAILED(hr))
        return hr;

    hr = pFactory->CreateInstance(NULL, IID_IMFTransform, (void**)ppMFT);
    pFactory->Release();
    return hr;
}

// what the negotiated output really is, so that a wrong subtype cannot hide in a log
static const char* SubtypeName(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_YV12) return "YV12";
    if (g == MFVideoFormat_P010) return "P010";
    return "unknown";
}

struct Options
{
    std::wstring input;
    std::wstring output = L"out.yuv";
    std::wstring mft    = L"..\\bin\\AVSVideoDecoder.dll";
    GUID         outputSubtype = GUID_NULL;   /* set only by -fmt; see the guard below */
    GUID         defaultSubtype = MFVideoFormat_NV12;  /* used when no file is written */
    int          maxFrames = -1;
    int          bmpFrame  = 30;
    bool         advertiseSize = true;
    bool         direct = false;      // decode with libavcodec directly, no MFT
    bool         listTypes = false;   // print what GetOutputAvailableType offers
};

int main(int argc, char** argv)
{
    // keep the progress readable when the run dies in the middle of a stream
    InstallCrashReport();

    Options opt;
    std::vector<std::wstring> pos;
    std::vector<std::wstring> wargs = WideArgs(argc, argv);
    for (int i = 1; i < (int)wargs.size(); i++)
    {
        std::string a = NarrowAscii(wargs[i]);
        if (a == "-mft" && i + 1 < (int)wargs.size()) { opt.mft = wargs[++i]; }
        else if (a == "-fmt" && i + 1 < (int)wargs.size())
        {
            std::string f = NarrowAscii(wargs[++i]);
            opt.outputSubtype = (f == "i420" || f == "yuv420p") ? MFVideoFormat_I420
                              : (f == "yv12") ? MFVideoFormat_YV12
                              : (f == "p010") ? MFVideoFormat_P010
                                              : MFVideoFormat_NV12;
        }
        else if (a == "-frames" && i + 1 < (int)wargs.size()) { opt.maxFrames = atoi(NarrowAscii(wargs[++i]).c_str()); }
        else if (a == "-bmp" && i + 1 < (int)wargs.size()) { opt.bmpFrame = atoi(NarrowAscii(wargs[++i]).c_str()); }
        else if (a == "-nosize") { opt.advertiseSize = false; }
        else if (a == "-types") { opt.listTypes = true; }
        else if (a == "-direct") { opt.direct = true; }
        else pos.push_back(wargs[i]);
    }
    if (pos.empty())
    {
        printf("usage: avsmft-test <input file> [output.yuv] [-mft dll] [-fmt nv12|i420|yv12|p010]"
               " [-frames N] [-bmp N] [-nosize]\n");
        return 1;
    }
    opt.input = pos[0];
    if (pos.size() > 1)
        opt.output = pos[1];

    // Round 21 lesson: the output layout is part of the experiment, not a default.  A
    // comparison against an I420 reference that silently used the NV12 default once
    // produced "0 of 26 pictures bit exact" for a decode that was in fact correct - the
    // Y plane matched exactly and every difference was in the interleaved UV.  Name the
    // format in the command, or the number is not evidence.
    /* -direct runs the decoder in the same process and writes what it gets; its layout
     * is ffmpeg's, not a choice the caller made, so this rule does not apply. */
    if (!opt.output.empty() && !opt.direct && opt.outputSubtype == GUID_NULL) {
        fprintf(stderr, "refusing to write without -fmt: the byte layout of the output ");
        fprintf(stderr, "is part of what the test measures (nv12 and i420 put chroma ");
        fprintf(stderr, "in different places). Pass -fmt nv12|i420|yv12|p010 explicitly.%c", 10);
        fprintf(stderr, "%c", 10);
        return 1;
    }

    // ---------------------------------------------------------------- demux with FFmpeg
    AVFormatContext* fmtCtx = NULL;
    const char* forcedDemuxer = NULL;
    std::string inUtf8 = WToUtf8(pos[0]);
    std::string ext = ToLowerAscii(inUtf8);
    if (ext.size() > 4 && (ext.substr(ext.size() - 4) == ".avs" || ext.substr(ext.size() - 5) == ".cavs"))
        forcedDemuxer = "cavsvideo";

    int err = avformat_open_input(&fmtCtx, inUtf8.c_str(),
                                  forcedDemuxer ? av_find_input_format(forcedDemuxer) : NULL, NULL);
    if (err < 0)
        Die("cannot open %s (%s)", inUtf8.c_str(), AVErrorString(err).c_str());
    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
        Die("cannot find stream info");

    int videoIndex = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
    {
        AVCodecParameters* par = fmtCtx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            (par->codec_id == AV_CODEC_ID_CAVS || par->codec_id == AV_CODEC_ID_AVS2 ||
             par->codec_id == AV_CODEC_ID_AVS3 || par->codec_id == AV_CODEC_ID_NONE))
        {
            videoIndex = (int)i;
            break;
        }
    }
    if (videoIndex < 0)
        for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                videoIndex = (int)i;
    if (videoIndex < 0)
        Die("no video stream in input");

    AVStream* st = fmtCtx->streams[videoIndex];
    AVCodecParameters* par = st->codecpar;
    GUID inSubtype = MFVideoFormat_AVS;
    if (par->codec_id == AV_CODEC_ID_AVS2)
        inSubtype = MFVideoFormat_AVS2;
    else if (par->codec_id == AV_CODEC_ID_AVS3)
        inSubtype = MFVideoFormat_AVS3;

    printf("input:   %s\n", inUtf8.c_str());
    printf("         stream %d: %s %dx%d, extradata %d bytes\n", videoIndex,
           avcodec_get_name(par->codec_id), par->width, par->height, par->extradata_size);

    // ---------------------------------------------------------------- optional: decode with libavcodec directly (no MFT), as a reference for the MFT path
    if (opt.direct)
    {
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        if (dec == NULL)
            Die("no decoder for %s in this FFmpeg build", avcodec_get_name(par->codec_id));
        AVCodecContext* c = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(c, par);
        c->pkt_timebase = st->time_base;
        if (avcodec_open2(c, dec, NULL) < 0)
            Die("avcodec_open2 failed");

        FILE* f = _wfopen(opt.output.c_str(), L"wb");
        AVFrame* fr = av_frame_alloc();
        AVPacket* p = av_packet_alloc();
        SwsContext* sws = NULL;
        int n = 0, nfed = 0;
        printf("direct:  decoder '%s' (%s)\n", dec->name, avcodec_get_name(par->codec_id));
        while (av_read_frame(fmtCtx, p) >= 0)
        {
            if (p->stream_index == videoIndex)
            {
                avcodec_send_packet(c, p);
                nfed++;
            }
            av_packet_unref(p);
            while (avcodec_receive_frame(c, fr) == 0)
            {
                WriteFrameYUV420P(f, fr, &sws);
                printf("         frame %3d pts=%.3fs\n", n, fr->best_effort_timestamp != AV_NOPTS_VALUE ?
                       (double)fr->best_effort_timestamp * av_q2d(st->time_base) : 0.0);
                n++;
                av_frame_unref(fr);
                if (opt.maxFrames > 0 && n >= opt.maxFrames)
                    break;
            }
            if (opt.maxFrames > 0 && n >= opt.maxFrames)
                break;
        }
        avcodec_send_packet(c, NULL);
        while (avcodec_receive_frame(c, fr) == 0 && (opt.maxFrames <= 0 || n < opt.maxFrames))
        {
            WriteFrameYUV420P(f, fr, &sws);
            printf("         frame %3d pts=%.3fs\n", n, fr->best_effort_timestamp != AV_NOPTS_VALUE ?
                   (double)fr->best_effort_timestamp * av_q2d(st->time_base) : 0.0);
            n++;
            av_frame_unref(fr);
        }
        fclose(f);
        if (sws)
            sws_freeContext(sws);
        printf("direct:  fed %d packets, %d frames (yuv420p) -> %s\n", nfed, n,
               std::string(opt.output.begin(), opt.output.end()).c_str());
        av_frame_free(&fr);
        avcodec_free_context(&c);
        avformat_close_input(&fmtCtx);
        return n > 0 ? 0 : 2;
    }

    // ---------------------------------------------------------------- MF + MFT
    if (FAILED(MFStartup(MF_VERSION)))
        Die("MFStartup failed");

    IMFTransform* pMFT = NULL;
    HRESULT hr = CreateMFT(opt.mft.c_str(), &pMFT);
    if (FAILED(hr))
        Die("creating the MFT from %s failed: 0x%08X", std::string(opt.mft.begin(), opt.mft.end()).c_str(), hr);

    IMFMediaType* pInputType = NULL;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, inSubtype);
    if (opt.advertiseSize && par->width > 0 && par->height > 0)
    {
        MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, par->width, par->height);
        MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE, 25, 1);
    }
    if (par->extradata_size > 0)
        pInputType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, par->extradata, par->extradata_size);

    hr = pMFT->SetInputType(0, pInputType, 0);
    if (FAILED(hr))
        Die("SetInputType failed: 0x%08X (does the MFT accept this sub-type?)", hr);

    IMFMediaType* pOutputType = NULL;
    if (opt.listTypes)
    {
        for (DWORD i = 0; i < 8; i++)
        {
            IMFMediaType* pT = NULL;
            HRESULT h = pMFT->GetOutputAvailableType(0, i, &pT);
            if (FAILED(h))
            {
                printf("         output type %d: none (0x%08X)\n", i, h);
                break;
            }
            GUID sub = GUID_NULL;
            pT->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 tw = 0, th = 0;
            MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &tw, &th);
            printf("         output type %d: subtype {%08X-%04X-%04X-...} %ux%u\n", i,
                   sub.Data1, sub.Data2, sub.Data3, tw, th);
            pT->Release();
        }
    }
    hr = pMFT->GetOutputAvailableType(0, 0, &pOutputType);
    if (FAILED(hr))
        Die("GetOutputAvailableType failed: 0x%08X", hr);
    GUID wantSub = (opt.outputSubtype == GUID_NULL) ? opt.defaultSubtype
                                                      : opt.outputSubtype;
    pOutputType->SetGUID(MF_MT_SUBTYPE, wantSub);
    hr = pMFT->SetOutputType(0, pOutputType, 0);
    if (FAILED(hr))
        Die("SetOutputType failed: 0x%08X", hr);
    pOutputType->Release();

    pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

    // ---------------------------------------------------------------- run
    FILE* outF = _wfopen(opt.output.c_str(), L"wb");
    if (outF == NULL)
        Die("cannot create %s", std::string(opt.output.begin(), opt.output.end()).c_str());

    UINT32 outW = 0, outH = 0, outPitch = 0;
    int fed = 0, frames = 0, formatChanges = 0;
    ULONGLONG sumY = 0;
    std::vector<BYTE> lastFrame;
    LONGLONG firstTime = 0, lastTime = 0;
    std::vector<LONGLONG> times;

    auto ReadOutput = [&]() -> bool
    {
        for (;;)
        {
            MFT_OUTPUT_DATA_BUFFER outBuf = {};
            DWORD status = 0;
            HRESULT h = pMFT->ProcessOutput(0, 1, &outBuf, &status);

            if (h == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return false;
            if (h == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                formatChanges++;
                IMFMediaType* pNew = NULL;
                if (SUCCEEDED(pMFT->GetOutputAvailableType(0, 0, &pNew)))
                {
                    pNew->SetGUID(MF_MT_SUBTYPE, wantSub);
                    if (SUCCEEDED(pMFT->SetOutputType(0, pNew, 0)))
                    {
                        MFGetAttributeSize(pNew, MF_MT_FRAME_SIZE, &outW, &outH);
                        outPitch = MFGetAttributeUINT32(pNew, MF_MT_DEFAULT_STRIDE, outW);
                        printf("  [stream change] output is now %ux%u pitch=%u\n", outW, outH, outPitch);
                    }
                    pNew->Release();
                }
                continue;
            }
            if (FAILED(h))
            {
                printf("  ProcessOutput returned 0x%08X\n", h);
                return false;
            }

            IMFSample* pSample = outBuf.pSample;
            if (pSample == NULL)
            {
                fprintf(stderr, "[tool] ReadOutput: ProcessOutput gave no sample "
                                "(hr=0x%08X status=0x%lX)\n", (unsigned)h, (unsigned long)status);
                return false;
            }

            IMFMediaBuffer* pBuffer = NULL;
            LONGLONG rtStart = 0, rtDur = 0;
            pSample->GetSampleTime(&rtStart);
            pSample->GetSampleDuration(&rtDur);
            if (FAILED(pSample->ConvertToContiguousBuffer(&pBuffer)))
            {
                pSample->Release();
                return false;
            }
            BYTE* pb = NULL;
            DWORD cb = 0;
            if (SUCCEEDED(pBuffer->Lock(&pb, NULL, &cb)))
            {
                IMFMediaType* pCur = NULL;
                if (SUCCEEDED(pMFT->GetOutputCurrentType(0, &pCur)))
                {
                    MFGetAttributeSize(pCur, MF_MT_FRAME_SIZE, &outW, &outH);
                    outPitch = MFGetAttributeUINT32(pCur, MF_MT_DEFAULT_STRIDE, outW);
                    pCur->Release();
                }
                fwrite(pb, 1, cb, outF);
                lastFrame.assign(pb, pb + cb);

                UINT32 chroma = outW * outH;
                if (wantSub == MFVideoFormat_P010)
                {
                    // 16 bit little endian samples; kept at full scale and scaled down
                    // when the mean is printed, so the number is comparable
                    for (UINT32 i = 0; i + 1 < chroma * 2 && i + 1 < cb; i += 2)
                        sumY += (ULONGLONG)pb[i] | ((ULONGLONG)pb[i + 1] << 8);
                }
                else
                {
                    for (UINT32 i = 0; i < chroma && i < cb; i++)
                        sumY += pb[i];
                }
                if (frames == 0) firstTime = rtStart;
                lastTime = rtStart;
                times.push_back(rtStart);
                frames++;
                pBuffer->Unlock();
            }
            pBuffer->Release();
            pSample->Release();

            if (opt.maxFrames > 0 && frames >= opt.maxFrames)
                return false;
        }
    };

    AVPacket* pkt = av_packet_alloc();
    bool stopFeeding = false;     // the requested picture count is out: no point feeding more
    while (av_read_frame(fmtCtx, pkt) >= 0)
    {
        if (pkt->stream_index != videoIndex)
        {
            av_packet_unref(pkt);
            continue;
        }
        if (opt.maxFrames > 0 && frames >= opt.maxFrames)
        {
            av_packet_unref(pkt);
            break;
        }

        // the pipeline must not feed more while the decoder queue is full
        for (;;)
        {
            DWORD flags = 0;
            HRESULT h = pMFT->GetInputStatus(0, &flags);
            if (h == MF_E_NOTACCEPTING)
            {
                if (opt.maxFrames > 0 && frames >= opt.maxFrames)
                {
                    stopFeeding = true;
                    break;
                }
                if (!ReadOutput())
                    continue;
                continue;
            }
            if (FAILED(h))
                Die("GetInputStatus: 0x%08X", h);
            break;
        }
        if (stopFeeding)
        {
            printf("note:   the requested %d frames are out; stopping input here\n",
                   opt.maxFrames);
            av_packet_unref(pkt);
            break;
        }

        IMFMediaBuffer* pBuffer = NULL;
        MFCreateMemoryBuffer(pkt->size > 0 ? (DWORD)pkt->size : 1, &pBuffer);
        BYTE* pb = NULL;
        pBuffer->Lock(&pb, NULL, NULL);
        memcpy(pb, pkt->data, pkt->size);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength((DWORD)pkt->size);

        IMFSample* pSample = NULL;
        MFCreateSample(&pSample);
        pSample->AddBuffer(pBuffer);
        if (pkt->pts != AV_NOPTS_VALUE)
        {
            LONGLONG hns = av_rescale_q(pkt->pts, st->time_base, AVRational{ 1, 10000000 });
            pSample->SetSampleTime(hns);
            pSample->SetSampleDuration(av_rescale_q(pkt->duration ? pkt->duration : 1,
                                                    st->time_base, AVRational{ 1, 10000000 }));
        }
        else
        {
            // A raw elementary stream carries no presentation time, and a bare
            // SetSampleTime(-1) is not a sample time any Media Foundation client would
            // hand over: the MFT's reordering cannot match pictures then, it holds them
            // and the queue fills up.  Synthesise monotonic times from the frame rate, as
            // the media source does when a real player opens the same file.
            LONGLONG dur = 400000;      // 40 ms = 25 fps, the AVS test streams' rate
            AVRational fr = av_guess_frame_rate(fmtCtx, st, NULL);
            if (fr.num > 0 && fr.den > 0)
                dur = av_rescale_q(1, AVRational{ fr.den, fr.num }, AVRational{ 1, 10000000 });
            if (fed == 0)
                printf("note:   the stream has no presentation times; synthesising %lld ms "
                       "sample times (a real MF client always has them)\n", dur / 10000);
            pSample->SetSampleTime((LONGLONG)fed * dur);
            pSample->SetSampleDuration(dur);
        }
        pBuffer->Release();

        HRESULT h = pMFT->ProcessInput(0, pSample, 0);
        // MF_E_NOTACCEPTING from this MFT means "my picture queue is full"
        // (AVS_MFT_MAX_QUEUED_FRAMES is 8, see AVSVideoDecoderMFT.h).  That is flow
        // control in the synchronous model, not a failure: a client takes the pictures
        // out and offers the same access unit again.  Treating it as fatal is what made
        // the isolated benchmark stop the moment the decoder released a burst larger than
        // the picture queue - on AVS2/AVS3 that is a few dozen access units in.
        //
        // Note that ReadOutput() itself drains everything it can and returns false when
        // it is done: the picture count, not its return value, says whether progress was
        // made.
        int retryRounds = 0;
        while (h == MF_E_NOTACCEPTING && retryRounds < 16)
        {
            int before = frames;
            while (ReadOutput())
                {}
            DWORD outFlags = 0;
            bool heldByMft = SUCCEEDED(pMFT->GetOutputStatus(&outFlags)) &&
                             (outFlags & MFT_OUTPUT_STATUS_SAMPLE_READY);
            fprintf(stderr, "[tool] ProcessInput said MF_E_NOTACCEPTING: took %d picture(s), "
                            "the MFT still holds %s (round %d)\n", frames - before,
                            heldByMft ? "more" : "nothing", retryRounds + 1);
            if (frames == before && !heldByMft)
                break;              // refuses input and produces nothing: real deadlock
            if (opt.maxFrames > 0 && frames >= opt.maxFrames)
            {
                printf("note:   the requested %d frames are out; stopping input here\n",
                       opt.maxFrames);
                stopFeeding = true;
                break;
            }
            retryRounds++;
            h = pMFT->ProcessInput(0, pSample, 0);
        }
        pSample->Release();
        if (stopFeeding)
        {
            fed++;
            av_packet_unref(pkt);
            break;
        }
        if (FAILED(h))
            Die("ProcessInput: 0x%08X%s", h,
                h == MF_E_NOTACCEPTING
                    ? " - the MFT still refuses input after taking every picture it had"
                    : "");
        fed++;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    pMFT->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    while (frames < (opt.maxFrames > 0 ? opt.maxFrames : 1 << 30))
    {
        if (!ReadOutput())
            break;
    }
    pMFT->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    fclose(outF);

    printf("result:  fed %d access units, decoded %d frames, %d format change(s)\n",
           fed, frames, formatChanges);
    {
        // the sample times must be non decreasing: a decoder MFT has to hand the
        // pictures over in presentation order, not in decode order
        bool ordered = true;
        for (size_t i = 1; i < times.size(); i++)
            if (times[i] < times[i - 1])
                ordered = false;
        printf("         sample times (s): ");
        for (size_t i = 0; i < times.size() && i < 12; i++)
            printf("%.3f ", times[i] / 1e7);
        printf("%s\n", ordered ? "[presentation order OK]" : "[NOT in presentation order!]");
    }
    printf("         output %ux%u (%s), time %lld .. %lld (100ns), mean Y=%.1f\n",
           outW, outH, SubtypeName(wantSub),
           firstTime, lastTime,
           frames ? (double)sumY * ((wantSub == MFVideoFormat_P010) ? 1.0 / 64.0 : 1.0)
                    / ((double)outW * outH * frames) : 0.0);

    // ---------------------------------------------------------------- write one BMP
    if (opt.bmpFrame >= 0 && !lastFrame.empty() && outW && outH &&
        opt.outputSubtype == MFVideoFormat_P010)
        printf("note: the BMP dump is only implemented for the 8 bit output formats\n");
    if (opt.bmpFrame >= 0 && !lastFrame.empty() && outW && outH &&
        opt.outputSubtype != MFVideoFormat_P010)
    {
        std::wstring bmpName = opt.output + L".bmp";
        FILE* f = _wfopen(bmpName.c_str(), L"wb");
        if (f)
        {
            std::vector<BYTE> rgb((size_t)outW * outH * 3);
            const BYTE* y = lastFrame.data();
            const BYTE* chroma = y + (size_t)outPitch * outH;
            // The chroma row pitch is *not* the luma pitch for the planar formats.
            // This mirrors AvsLayoutFor() in the MFT: planar chroma rows are half the
            // width rounded up, aligned to 4; only the semi-planar formats reuse the
            // luma stride.  Assuming the luma pitch here read about twice past the end
            // of the sample (an access violation in the heap tail, hence intermittent).
            UINT32 cPitch = (opt.outputSubtype == MFVideoFormat_I420 ||
                             opt.outputSubtype == MFVideoFormat_YV12)
                                ? (((outW + 1) / 2 + 3) & ~3u)
                                : outPitch;
            for (UINT32 j = 0; j < outH; j++)
            {
                for (UINT32 i = 0; i < outW; i++)
                {
                    int yy = y[(size_t)outPitch * j + i];
                    int u, v;
                    if (opt.outputSubtype == MFVideoFormat_I420)
                    {
                        u = chroma[(size_t)cPitch * (j / 2) + i / 2];
                        v = chroma[(size_t)cPitch * (outH / 2) + (size_t)cPitch * (j / 2) + i / 2];
                    }
                    else if (opt.outputSubtype == MFVideoFormat_YV12)
                    {
                        // YV12: Cr in the second plane, Cb in the third
                        v = chroma[(size_t)cPitch * (j / 2) + i / 2];
                        u = chroma[(size_t)cPitch * (outH / 2) + (size_t)cPitch * (j / 2) + i / 2];
                    }
                    else
                    {
                        u = chroma[(size_t)cPitch * (j / 2) + 2 * (i / 2)];
                        v = chroma[(size_t)cPitch * (j / 2) + 2 * (i / 2) + 1];
                    }
                    int r = yy + (359 * (v - 128) + 128) / 256;
                    int g = yy - ((88 * (u - 128) + 183 * (v - 128)) + 128) / 256;
                    int b = yy + (454 * (u - 128) + 128) / 256;
                    BYTE* d = &rgb[((size_t)(outH - 1 - j) * outW + i) * 3];
                    d[0] = (BYTE)Clamp8(b); d[1] = (BYTE)Clamp8(g); d[2] = (BYTE)Clamp8(r);
                }
            }
            BITMAPFILEHEADER fh = {};
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(bi);
            bi.biWidth = (LONG)outW;
            bi.biHeight = (LONG)outH;
            bi.biPlanes = 1;
            bi.biBitCount = 24;
            bi.biCompression = BI_RGB;
            bi.biSizeImage = (DWORD)rgb.size();
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(bi);
            fh.bfSize = fh.bfOffBits + (DWORD)rgb.size();
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&bi, sizeof(bi), 1, f);
            fwrite(rgb.data(), 1, rgb.size(), f);
            fclose(f);
            printf("         wrote last decoded frame to %s\n",
                   std::string(bmpName.begin(), bmpName.end()).c_str());
        }
    }

    pMFT->Release();
    pInputType->Release();
    avformat_close_input(&fmtCtx);
    MFShutdown();
    return frames > 0 ? 0 : 2;
}
