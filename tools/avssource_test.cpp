//----------------------------------------------------------------------------------------------
// avssource_test.cpp - validate the custom media source, by itself and through the
//                      Media Foundation pipeline.
//
//   avssource-test <file> [-mft <dll>] [-raw] [-video] [-audio] [-frames N]
//                        [-url | -handler] [-out <file>] [-bmp N]
//
//   default   the source reader inserts the registered AVS decoder MFTs, so this
//             exercises source -> decoder MFT -> decoded NV12 / PCM, which is the
//             path a player uses.
//   -raw      the compressed elementary stream is requested instead: only the source is
//             exercised (no decoder).  That is how a source bug is told apart from a
//             decoder bug.
//   -url      MFCreateSourceReaderFromURL: works only when the byte stream handler is
//             registered for this file extension (DllRegisterServer).
//   -handler  the byte stream handler object itself, created by CLSID and driven with
//             BeginCreateObject/EndCreateObject, with no registry lookup involved.  Used
//             together with -url this tells a broken handler apart from a lookup that
//             does not find it.
//
// Nothing has to be registered in the system unless -url is used.
//----------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include "crashreport.h"
#include "AvsDemuxHint.h"
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "shell32")   // CommandLineToArgvW

template <class T> static void SafeRelease(T** ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}
#define SAFE_RELEASE(p) if ((p)) { SafeRelease(&(p)); }

// ---- the GUIDs of AVSVideoDecoder.dll (classGuids.h) ---------------------------------
static const CLSID CLSID_AVSMediaSource =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x13 } };
static const GUID IID_IAVSByteDataSource =
    { 0x7b8e2a4c, 0x1d3f, 0x4a6b, { 0x9c, 0x8d, 0x2e, 0x4f, 0x6a, 0x7b, 0x8c, 0x9d } };
static const GUID AVS_SOURCE_ATTRIBUTE_DEMUX_HINT =
    { 0xa1b2c3d4, 0x5e6f, 0x4a7b, { 0x8c, 0x9d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d } };
static const CLSID CLSID_AVSByteStreamHandler =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x14 } };

// IMFByteStreamHandler only creates objects asynchronously; the handler in the DLL calls
// the callback before BeginCreateObject returns, so a callback that just keeps the
// result is enough to drive it from a test.
class SyncCallback : public IMFAsyncCallback
{
    LONG m_Ref = 1;
public:
    IMFAsyncResult* pResult = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback))
        {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG rc = InterlockedDecrement(&m_Ref);
        if (rc == 0) delete this;
        return rc;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* pResult) override
    {
        if (pResult) pResult->AddRef();
        SAFE_RELEASE(this->pResult);
        this->pResult = pResult;
        return S_OK;
    }
    // IMFAsyncCallback also asks where it may be invoked; the handler in the DLL calls
    // us synchronously so the queue does not matter here.
    STDMETHODIMP GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) override
    {
        if (pdwFlags) *pdwFlags = 0;
        if (pdwQueue) *pdwQueue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
        return S_OK;
    }
};

interface IAVSByteDataSource : public IUnknown
{
    virtual STDMETHODIMP SetByteStream(IMFByteStream* pByteStream) = 0;
    virtual STDMETHODIMP Open(void) = 0;
};

typedef HRESULT (__stdcall *DllGetClassObjectFn)(REFCLSID, REFIID, void**);

static DWORD Align(DWORD v, DWORD a) { return (v + a - 1) / a * a; }

static std::wstring WS(const char* s)
{
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 1 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static std::string NarrowAscii(const std::wstring& w)   // flags are plain ASCII
{
    std::string s(w.size(), '\0');
    for (size_t i = 0; i < w.size(); i++)
        s[i] = (w[i] < 128) ? (char)w[i] : '?';
    return s;
}

// the CRT argv is ANSI (under MSYS it is the OEM code page) and mangles non-ASCII
// file names, so the tools always take the real wide command line
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

static std::string GuidStr(const GUID& g)
{
    OLECHAR buf[64] = { 0 };
    StringFromGUID2(g, buf, 64);
    std::string s;
    for (int i = 0; buf[i]; i++) s += (char)buf[i];
    return s;
}

static std::string FourCc(const GUID& g)
{
    // all MF fourCC media sub-types are <fourcc>-0000-0010-8000-00AA00389B71
    if (g.Data2 == 0 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 && g.Data4[7] == 0x71)
    {
        char s[5] = { (char)(g.Data1 & 0xFF), (char)((g.Data1 >> 8) & 0xFF),
                      (char)((g.Data1 >> 16) & 0xFF), (char)((g.Data1 >> 24) & 0xFF), 0 };
        for (int i = 0; i < 4; i++) if (s[i] < 0x20 || s[i] > 0x7E) return std::string();
        return std::string(s);
    }
    return std::string();
}

static std::string NameOf(const GUID& g)
{
    if (g == MFMediaType_Video) return "video";
    if (g == MFMediaType_Audio) return "audio";
    if (g == MFAudioFormat_PCM) return "PCM";
    if (g == MFAudioFormat_Float) return "Float";
    std::string f = FourCc(g);
    if (!f.empty()) return f;
    return GuidStr(g);
}

// MF_MT_INTERLACE_MODE values are an SDK enum (mfobjects.h); printed by name because the
// numbers 2/3/4/5/6/7 mean nothing to a reader of a test log, and "unset" is a result too.
static const char* InterlaceName(UINT32 v)
{
    switch (v)
    {
    case MFVideoInterlace_Unknown:                     return "unknown";
    case MFVideoInterlace_Progressive:                 return "progressive";
    case MFVideoInterlace_FieldInterleavedUpperFirst:  return "field-interleaved-upper-first";
    case MFVideoInterlace_FieldInterleavedLowerFirst:  return "field-interleaved-lower-first";
    case MFVideoInterlace_FieldSingleUpper:            return "field-single-upper";
    case MFVideoInterlace_FieldSingleLower:            return "field-single-lower";
    case MFVideoInterlace_MixedInterlaceOrProgressive: return "mixed";
    default:                                          return "other";
    }
}

static void PrintMediaType(IMFMediaType* t, const char* tag)
{
    GUID maj = GUID_NULL, sub = GUID_NULL;
    t->GetGUID(MF_MT_MAJOR_TYPE, &maj);
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)))
    {
        UINT64 fr = MFGetAttributeUINT64(t, MF_MT_FRAME_RATE, 0);
        printf("%s: %s/%s %ux%u", tag, NameOf(maj).c_str(), NameOf(sub).c_str(), w, h);
        if ((DWORD)fr) printf(" fps=%u.%03u", (DWORD)(fr >> 32), (DWORD)fr);
        // the colour signalling, because "the picture is correct" also means "the
        // picture is described correctly", and this is the one place the test shows it
        const UINT32 pri = MFGetAttributeUINT32(t, MF_MT_VIDEO_PRIMARIES, 0);
        const UINT32 trc = MFGetAttributeUINT32(t, MF_MT_TRANSFER_FUNCTION, 0);
        const UINT32 rng = MFGetAttributeUINT32(t, MF_MT_VIDEO_NOMINAL_RANGE, 0);
        if (pri || trc || rng)
            printf(" color prim=%u trc=%u range=%u", pri, trc, rng);
        UINT32 pn = 0, pd = 0;
        MFGetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd);
        if (pn && pd && !(pn == pd))
            printf(" par=%u:%u", pn, pd);
        // picture structure: this is the attribute that decides whether the renderer
        // de-interlaces at all, so it is reported on every type line, including "unset".
        UINT32 il = 0;
        if (FAILED(t->GetUINT32(MF_MT_INTERLACE_MODE, &il))) printf(" interlace=unset");
        else printf(" interlace=%s(%u)", InterlaceName(il), il);
        printf("\n");
    }
    else
    {
        printf("%s: %s/%s ch=%u rate=%u bits=%u mask=0x%X\n", tag, NameOf(maj).c_str(),
               NameOf(sub).c_str(),
               MFGetAttributeUINT32(t, MF_MT_AUDIO_NUM_CHANNELS, 0),
               MFGetAttributeUINT32(t, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0),
               MFGetAttributeUINT32(t, MF_MT_AUDIO_BITS_PER_SAMPLE, 0),
               MFGetAttributeUINT32(t, MF_MT_AUDIO_CHANNEL_MASK, 0));
    }
}

static std::wstring ExtOf(const std::wstring& path)
{
    size_t d = path.find_last_of(L'.');
    if (d == std::wstring::npos) return std::wstring();
    std::wstring e = path.substr(d);
    for (size_t i = 0; i < e.size(); i++) if (e[i] < 128) e[i] = (wchar_t)tolower((int)e[i]);
    return e;
}

// the same table the byte stream handler uses: the raw demuxer decides the codec
// id, so an AVS2 / AVS3 elementary stream must not be handed to cavsvideo
static std::wstring DemuxHint(const std::wstring& ext)
{
    std::string e;
    for (size_t i = 0; i < ext.size(); i++)
        e += (char)ext[i];              // extensions are ASCII
    std::string h = AvsDemuxHintForExt(e);
    std::wstring w;
    for (size_t i = 0; i < h.size(); i++)
        w += (wchar_t)h[i];
    return w;
}

// what the source itself publishes (only available on the direct-create path)
static void DumpPresentation(IMFMediaSource* pSource)
{
    IMFPresentationDescriptor* pPD = nullptr;
    if (FAILED(pSource->CreatePresentationDescriptor(&pPD)))
    {
        printf("source: CreatePresentationDescriptor failed\n");
        return;
    }
    DWORD cStreams = 0;
    pPD->GetStreamDescriptorCount(&cStreams);
    printf("presentation: %lu stream(s), duration %.3f s\n", cStreams,
           (double)MFGetAttributeUINT64(pPD, MF_PD_DURATION, 0) / 1e7);

    for (DWORD i = 0; i < cStreams; i++)
    {
        IMFStreamDescriptor* pSD = nullptr;
        BOOL bSelected = FALSE;
        if (FAILED(pPD->GetStreamDescriptorByIndex(i, &bSelected, &pSD)))
            continue;
        IMFMediaTypeHandler* pH = nullptr;
        pSD->GetMediaTypeHandler(&pH);
        DWORD cTypes = 0;
        if (pH) pH->GetMediaTypeCount(&cTypes);
        printf("  stream %lu: selected=%d id=%u types=%lu\n", i, (int)bSelected,
               MFGetAttributeUINT32(pSD, MF_DEVICESTREAM_STREAM_ID, 0xFFFFFFFF), cTypes);
        for (DWORD k = 0; pH && k < cTypes; k++)
        {
            IMFMediaType* pT = nullptr;
            if (SUCCEEDED(pH->GetMediaTypeByIndex(k, &pT)))
            {
                PrintMediaType(pT, "    offered");
                SAFE_RELEASE(pT);
            }
        }
        SAFE_RELEASE(pH);
        SAFE_RELEASE(pSD);
    }
    SAFE_RELEASE(pPD);
}

static void WriteBmp(const wchar_t* path, const BYTE* pY, const BYTE* pUV,
                     UINT32 w, UINT32 h, UINT32 strideY, UINT32 strideUV)
{
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;
    const DWORD rowBytes = w * 3;
    const DWORD imgSize = Align(rowBytes * h, 4);
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER bi = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(bi));
    fh.bfSize = fh.bfOffBits + imgSize;
    bi.biSize = (DWORD)sizeof(bi);
    bi.biWidth = (LONG)w;
    bi.biHeight = (LONG)h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biSizeImage = imgSize;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);

    std::vector<BYTE> row(rowBytes, 0);
    for (UINT32 y = 0; y < h; y++)
    {
        const BYTE* py = pY + (size_t)y * strideY;
        const BYTE* puv = pUV + (size_t)(y >> 1) * strideUV;
        for (UINT32 x = 0; x < w; x++)
        {
            int Y = py[x], U = puv[(x >> 1) * 2], V = puv[(x >> 1) * 2 + 1];
            int C = Y - 16, D = U - 128, E = V - 128;
            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;
            row[x * 3 + 0] = (BYTE)(B < 0 ? 0 : B > 255 ? 255 : B);
            row[x * 3 + 1] = (BYTE)(G < 0 ? 0 : G > 255 ? 255 : G);
            row[x * 3 + 2] = (BYTE)(R < 0 ? 0 : R > 255 ? 255 : R);
        }
        fwrite(&row[0], 1, rowBytes, f);
        static const BYTE pad[4] = { 0 };
        if (rowBytes % 4) fwrite(pad, 1, 4 - (rowBytes % 4), f);
    }
    fclose(f);
}

// -------------------------------------------------------------------------------------
struct Target
{
    DWORD ix;
    bool  bVideo;
    bool  bCompressed;
};

struct Sink
{
    std::vector<BYTE> pcm;
    std::vector<DWORD> sizes;         // byte size of every audio buffer, for the time check
    FILE*             pYuv;
    UINT32            w, h, strideY, strideUV;
    std::vector<BYTE> lastY, lastUV;
    std::vector<LONGLONG> times, durations;
    long  frames;
    long  formatChanges;
    double sumY;
    // the per-sample half of the interlace contract, counted over every delivered sample
    long  nInterlaced = 0;
    long  nBottomFirst = 0;
};

// print every attribute of a media type: what a real decoder puts on its output type
// is the best reference for what our own decoder should be putting there
static void DumpType(IMFMediaType* pT)
{
    UINT32 c = 0;
    if (FAILED(pT->GetCount(&c)))
        return;
    printf("  --- media type has %u attribute(s) ---\n", c);
    for (UINT32 i = 0; i < c; i++)
    {
        GUID key = GUID_NULL;
        PROPVARIANT var = {};
        PropVariantInit(&var);
        if (FAILED(pT->GetItemByIndex(i, &key, &var)))
            continue;
        WCHAR szKey[64] = L"";
        StringFromGUID2(key, szKey, 64);
        printf("  %S vt=%3u", szKey, (unsigned)var.vt);

        UINT32 cbBlob = 0;
        const HRESULT hrBlob = pT->GetBlob(key, nullptr, 0, &cbBlob);
        switch (var.vt)
        {
        case VT_UI4:
        case VT_I4:
            printf(" = %u (0x%X)", (unsigned)var.ulVal, (unsigned)var.ulVal);
            break;
        case VT_UI8:
        case VT_I8:
            printf(" = %llu (0x%llX)", (unsigned long long)var.hVal.QuadPart,
                   (unsigned long long)var.hVal.QuadPart);
            break;
        case VT_CLSID:
        {
            WCHAR g[64] = L"";
            if (var.puuid)
                StringFromGUID2(*var.puuid, g, 64);
            printf(" = %S", g);
            break;
        }
        default:
            if (SUCCEEDED(hrBlob))
                printf(" = blob, %u bytes", cbBlob);
            break;
        }
        printf("\n");
        PropVariantClear(&var);
    }
    printf("  --- end ---\n");
}

int main(int argc, char** argv)
{
    // keep the progress readable when the run dies in the middle of a stream
    InstallCrashReport();

    if (argc < 2)
    {
        printf("usage: avssource-test <file> [-mft <dll>] [-raw] [-video] [-audio] [-frames N]\n"
               "                       [-url | -handler] [-out <file>] [-bmp N] [-nv12|-i420|-yv12|-p010] [-float]\n");
        return 2;
    }

    std::vector<std::wstring> args = WideArgs(argc, argv);
    std::wstring inPath = args[1];
    std::wstring mftDll = L"D:\\avs\\MediaFoundationTransform\\AVSVideoDecoder\\bin\\x64\\Release\\AVSVideoDecoder.dll";
    std::wstring outPath;
    std::string inPathA = NarrowAscii(inPath);   // only used for the extension / .bmp name
    bool bRaw = false, bUrl = false, bHandler = false, wantVideo = true, wantAudio = true;
    bool wantFloat = false;
    GUID wantSub = { 0x3231564E, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } }; // NV12
    int maxFrames = 120, bmpAt = -1;

    // the CRT argv is ANSI (under MSYS it is the OEM code page), which mangles
    // non-ASCII file names, so use the real wide command line
    for (int i = 2; i < (int)args.size(); i++)
    {
        std::string a = NarrowAscii(args[i]);
        if (a == "-mft" && i + 1 < (int)args.size()) mftDll = args[++i];
        else if (a == "-raw") bRaw = true;
        else if (a == "-url") bUrl = true;
        else if (a == "-handler") bHandler = true;
        else if (a == "-nv12") wantSub = MFVideoFormat_NV12;
        else if (a == "-i420") wantSub = MFVideoFormat_I420;
        else if (a == "-yv12") wantSub = MFVideoFormat_YV12;
        else if (a == "-p010") wantSub = MFVideoFormat_P010;
        else if (a == "-float") wantFloat = true;
        else if (a == "-video") { wantVideo = true; wantAudio = false; }
        else if (a == "-audio") { wantAudio = true; wantVideo = false; }
        else if (a == "-frames" && i + 1 < (int)args.size()) maxFrames = atoi(NarrowAscii(args[++i]).c_str());
        else if (a == "-bmp" && i + 1 < (int)args.size()) bmpAt = atoi(NarrowAscii(args[++i]).c_str());
        else if (a == "-out" && i + 1 < (int)args.size()) outPath = args[++i];
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    IMFSourceReader* pSR = nullptr;
    IMFMediaSource* pSource = nullptr;
    IMFAttributes* pSRAttrs = nullptr;

    MFCreateAttributes(&pSRAttrs, 2);
    if (pSRAttrs)
        pSRAttrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);

    if (bUrl)
    {
        HRESULT hr = MFCreateSourceReaderFromURL(inPath.c_str(), pSRAttrs, &pSR);
        printf("MFCreateSourceReaderFromURL -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;
    }
    else if (bHandler)
    {
        // the handler class itself, without MF's extension lookup: this is what MF
        // would do once the .avs -> CLSID mapping in the registry is found
        IMFByteStream* pBS = nullptr;
        HRESULT hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                  MF_FILEFLAGS_NONE, inPath.c_str(), &pBS);
        printf("MFCreateFile -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;

        IMFByteStreamHandler* pBH = nullptr;
        hr = CoCreateInstance(CLSID_AVSByteStreamHandler, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&pBH));
        printf("CoCreateInstance(CLSID_AVSByteStreamHandler) -> 0x%08X\n", hr);
        if (FAILED(hr)) { SAFE_RELEASE(pBS); return 1; }

        SyncCallback* pCB = new SyncCallback();
        IUnknown* pCookie = nullptr;
        // Measured, not assumed: with dwFlags = 0 this probe asked for nothing, and the handler
        // answered EndCreateObject with MF_OBJECT_INVALID for every file - a probe that proves
        // nothing.  MF asks a registered handler for a media source, so ask the way a player does.
        const DWORD dwFlags = MF_RESOLUTION_MEDIASOURCE |
                              MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE;
        hr = pBH->BeginCreateObject(pBS, inPath.c_str(), dwFlags, nullptr, &pCookie, pCB, nullptr);
        printf("BeginCreateObject(flags=0x%X: mediasource, content need not match extension) "
               "-> 0x%08X\n", dwFlags, hr);
        if (FAILED(hr)) { SAFE_RELEASE(pCB); SAFE_RELEASE(pBH); SAFE_RELEASE(pBS); return 1; }

        MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
        IUnknown* pUnk = nullptr;
        hr = pBH->EndCreateObject(pCB->pResult, &objType, &pUnk);
        // Names come from the SDK's own ordering (mfidl.h:1378: MF_OBJECT_MEDIASOURCE = 0,
        // MF_OBJECT_BYTESTREAM = 1, MF_OBJECT_INVALID = 2).  Assuming "INVALID = 0" here once made a
        // perfectly healthy run look broken, so the identifiers, not the numbers, are what we print.
        const char* szObjType = "unknown";
        switch ((int)objType)
        {
            case MF_OBJECT_MEDIASOURCE: szObjType = "MEDIASOURCE"; break;
            case MF_OBJECT_BYTESTREAM:  szObjType = "BYTESTREAM";  break;
            case MF_OBJECT_INVALID:     szObjType = "INVALID";     break;
        }
        printf("EndCreateObject -> 0x%08X, object type %d (%s)\n", hr, (int)objType, szObjType);
        SAFE_RELEASE(pCB);
        SAFE_RELEASE(pBH);
        SAFE_RELEASE(pBS);
        if (FAILED(hr) || !pUnk) return 1;

        hr = pUnk->QueryInterface(IID_PPV_ARGS(&pSource));
        SAFE_RELEASE(pUnk);
        printf("QI(IMFMediaSource) -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;

        hr = MFCreateSourceReaderFromMediaSource(pSource, pSRAttrs, &pSR);
        printf("MFCreateSourceReaderFromMediaSource -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;
    }
    else
    {
        HMODULE hMod = LoadLibraryW(mftDll.c_str());
        if (!hMod)
        {
            printf("cannot load %S  (err %lu)\n", mftDll.c_str(), GetLastError());
            return 1;
        }
        DllGetClassObjectFn pGetClass = (DllGetClassObjectFn)GetProcAddress(hMod, "DllGetClassObject");
        IClassFactory* pCF = nullptr;
        IAVSByteDataSource* pDS = nullptr;
        IMFByteStream* pBS = nullptr;
        HRESULT hr = pGetClass ? pGetClass(CLSID_AVSMediaSource, IID_IClassFactory, (void**)&pCF)
                               : E_FAIL;
        if (SUCCEEDED(hr)) hr = pCF->CreateInstance(nullptr, IID_IAVSByteDataSource, (void**)&pDS);
        if (SUCCEEDED(hr))
            hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                              MF_FILEFLAGS_NONE, inPath.c_str(), &pBS);
        if (SUCCEEDED(hr)) hr = pDS->SetByteStream(pBS);

        std::wstring hint = DemuxHint(ExtOf(inPath));
        if (SUCCEEDED(hr) && !hint.empty())
        {
            IMFMediaSourceEx* pEx = nullptr;
            IMFAttributes* pAttrs = nullptr;
            HRESULT hrA = pDS->QueryInterface(__uuidof(IMFMediaSourceEx), (void**)&pEx);
            if (SUCCEEDED(hrA)) hrA = pEx->GetSourceAttributes(&pAttrs);
            if (SUCCEEDED(hrA))
            {
                hrA = pAttrs->SetString(AVS_SOURCE_ATTRIBUTE_DEMUX_HINT, hint.c_str());
                printf("demux hint: %S (attributes -> 0x%08X)\n", hint.c_str(), (unsigned)hrA);
            }
            else
                printf("source attribute store -> 0x%08X\n", (unsigned)hrA);
            SAFE_RELEASE(pAttrs);
            SAFE_RELEASE(pEx);
        }
        if (SUCCEEDED(hr)) hr = pDS->Open();
        if (SUCCEEDED(hr)) hr = pDS->QueryInterface(__uuidof(IMFMediaSource), (void**)&pSource);
        printf("media source create/open -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;

        DumpPresentation(pSource);

        hr = MFCreateSourceReaderFromMediaSource(pSource, pSRAttrs, &pSR);
        printf("MFCreateSourceReaderFromMediaSource -> 0x%08X\n", hr);
        if (FAILED(hr)) return 1;
    }

    // ---- which streams are there, and what should we ask for --------------------------
    std::vector<Target> targets;
    for (DWORD ix = 0; ; ix++)
    {
        IMFMediaType* pNative = nullptr;
        if (FAILED(pSR->GetNativeMediaType(ix, 0, &pNative)) || pNative == nullptr)
            break;

        GUID maj = GUID_NULL;
        pNative->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        const bool bVideo = (maj == MFMediaType_Video);
        PrintMediaType(pNative, "native media type");

        if ((bVideo && wantVideo) || (!bVideo && wantAudio))
        {
            pSR->SetStreamSelection(ix, TRUE);
            Target t = { ix, bVideo, bRaw };
            targets.push_back(t);

            if (bRaw)
            {
                HRESULT hr = pSR->SetCurrentMediaType(ix, nullptr, pNative);
                printf("stream %lu: request compressed -> 0x%08X\n", ix, hr);
            }
            else
            {
                // ask for the decoded format explicitly: the reader then has to find an
                // MFT that converts our AVS / DRA sub-type into it, which is the whole
                // point of this test
                IMFMediaType* pOut = nullptr;
                HRESULT hr = E_INVALIDARG;
                if (bVideo)
                {
                    UINT32 w = 0, h = 0;
                    MFGetAttributeSize(pNative, MF_MT_FRAME_SIZE, &w, &h);
                    MFCreateMediaType(&pOut);
                    if (pOut)
                    {
                        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                        pOut->SetGUID(MF_MT_SUBTYPE, wantSub);
                        pOut->SetUINT64(MF_MT_FRAME_SIZE, (((QWORD)w) << 32) | (QWORD)h);
                        pOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                        const UINT64 fr = MFGetAttributeUINT64(pNative, MF_MT_FRAME_RATE, 0);
                        if (fr)
                            pOut->SetUINT64(MF_MT_FRAME_RATE, fr);
                        // A player carries the stream's own descriptions over to the
                        // format it asks for.  Doing it here is what makes the test
                        // show whether the pipeline propagates colour signalling; a
                        // bare type would lose it before the decoder is even reached.
                        {
                            static const GUID copyAttrs[] = {
                                MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION,
                                MF_MT_VIDEO_NOMINAL_RANGE, MF_MT_PIXEL_ASPECT_RATIO,
                            };
                            for (size_t k = 0; k < sizeof(copyAttrs) / sizeof(copyAttrs[0]); k++)
                            {
                                UINT32 v = 0;
                                if (SUCCEEDED(pNative->GetUINT32(copyAttrs[k], &v)))
                                    pOut->SetUINT32(copyAttrs[k], v);
                            }
                        }
                        hr = pSR->SetCurrentMediaType(ix, nullptr, pOut);
                        printf("stream %lu: request %s output -> 0x%08X\n", ix,
                               FourCc(wantSub).c_str(), hr);
                    }
                }
                else
                {
                    MFCreateMediaType(&pOut);
                    if (pOut)
                    {
                        const UINT32 rate = MFGetAttributeUINT32(pNative, MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                        const UINT32 ch = MFGetAttributeUINT32(pNative, MF_MT_AUDIO_NUM_CHANNELS, 2);
                        const UINT32 bits = wantFloat ? 32u : 16u;
                        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        pOut->SetGUID(MF_MT_SUBTYPE, wantFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
                        pOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                        pOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
                        pOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
                        pOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * (bits / 8));
                        pOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * (bits / 8));
                        // carry the speaker arrangement over, like a player does
                        UINT32 mask = 0;
                        if (SUCCEEDED(pNative->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &mask)) && mask)
                            pOut->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, mask);
                        hr = pSR->SetCurrentMediaType(ix, nullptr, pOut);
                        printf("stream %lu: request %s output -> 0x%08X\n", ix,
                               wantFloat ? "Float" : "PCM", hr);
                    }
                }
                SAFE_RELEASE(pOut);
            }
            IMFMediaType* pCur = nullptr;
            if (SUCCEEDED(pSR->GetCurrentMediaType(ix, &pCur)) && pCur)
            {
                PrintMediaType(pCur, bRaw ? "stream compressed" : "stream decoder output");
                SAFE_RELEASE(pCur);
            }
        }
        else
        {
            pSR->SetStreamSelection(ix, FALSE);
        }
        SAFE_RELEASE(pNative);
    }
    printf("streams to read: %zu\n", targets.size());
    if (targets.empty())
        return 1;

    // ---- read ------------------------------------------------------------------------
    Sink sink;
    sink.pYuv = nullptr;
    sink.w = sink.h = sink.strideY = sink.strideUV = 0;
    sink.frames = 0;
    sink.formatChanges = 0;
    sink.sumY = 0;

    if (!outPath.empty() && targets[0].bVideo)
    {
        sink.pYuv = _wfopen(outPath.c_str(), L"wb");
        if (!sink.pYuv) printf("cannot open %s\n", NarrowAscii(outPath).c_str());
    }

    for (size_t ti = 0; ti < targets.size(); ti++)
    {
        const DWORD ix = targets[ti].ix;
        const bool bVideo = targets[ti].bVideo;
        long n = 0;
        double sumY = 0;

        // what will actually come out (the reader has negotiated the decoder by now)
        {
            IMFMediaType* pT = nullptr;
            if (SUCCEEDED(pSR->GetCurrentMediaType(ix, &pT)) && pT)
            {
                if (getenv("AVS_DUMP_TYPES"))
                    DumpType(pT);
                if (bVideo)
                {
                    MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &sink.w, &sink.h);
                    sink.strideY = MFGetAttributeUINT32(pT, MF_MT_DEFAULT_STRIDE, sink.w);
                    if (!sink.strideY) sink.strideY = sink.w;
                    sink.strideUV = MFGetAttributeUINT32(pT, MF_MT_DEFAULT_STRIDE, sink.w);
                    if (!sink.strideUV) sink.strideUV = sink.w;
                }
                else
                {
                    printf("audio format: ch=%u rate=%u bits=%u\n",
                           MFGetAttributeUINT32(pT, MF_MT_AUDIO_NUM_CHANNELS, 0),
                           MFGetAttributeUINT32(pT, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0),
                           MFGetAttributeUINT32(pT, MF_MT_AUDIO_BITS_PER_SAMPLE, 0));
                }
                SAFE_RELEASE(pT);
            }
        }

        for (;;)
        {
            IMFSample* pSample = nullptr;
            IMFMediaBuffer* pContig = nullptr;
            DWORD flags = 0;
            LONGLONG rt = 0;
            DWORD actual = 0;

            HRESULT hr = pSR->ReadSample(ix, 0, &actual, &flags, &rt, &pSample);
            if (FAILED(hr))
            {
                printf("ReadSample(stream %lu) -> 0x%08X\n", ix, hr);
                break;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            {
                sink.formatChanges++;
                IMFMediaType* pT = nullptr;
                if (SUCCEEDED(pSR->GetCurrentMediaType(ix, &pT)) && pT)
                {
                    PrintMediaType(pT, "decoded output type");
                    if (bVideo)
                    {
                        MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &sink.w, &sink.h);
                        sink.strideY = MFGetAttributeUINT32(pT, MF_MT_DEFAULT_STRIDE, sink.w);
                        if (!sink.strideY) sink.strideY = sink.w;
                        sink.strideUV = MFGetAttributeUINT32(pT, MF_MT_DEFAULT_STRIDE, sink.w);
                        if (!sink.strideUV) sink.strideUV = sink.w;   // NV12: one UV row = w bytes
                    }
                    SAFE_RELEASE(pT);
                }
            }
            if (pSample == nullptr)
            {
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                {
                    printf("stream %lu: end of stream after %ld samples\n", ix, n);
                    break;
                }
                continue;
            }

            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pContig)))
            {
                BYTE* pb = nullptr;
                DWORD cb = 0;
                if (SUCCEEDED(pContig->Lock(&pb, nullptr, &cb)))
                {
                    if (bVideo)
                    {
                        if (sink.pYuv) fwrite(pb, 1, cb, sink.pYuv);
                        if (sink.w && sink.h)
                        {
                            const DWORD planeY = sink.strideY * sink.h;
                            for (DWORD k = 0; k < planeY && k < cb; k++) sumY += pb[k];
                            if (bmpAt >= 0)
                            {
                                sink.lastY.assign(pb, pb + planeY);
                                if (cb > planeY) sink.lastUV.assign(pb + planeY, pb + cb);
                            }
                        }
                    }
                    else
                    {
                        sink.pcm.insert(sink.pcm.end(), pb, pb + cb);
                        sink.sizes.push_back(cb);
                    }
                    pContig->Unlock();
                }
            }
            if (bVideo && pSample)
            {
                UINT32 v = 0;
                if (SUCCEEDED(pSample->GetUINT32(MFSampleExtension_Interlaced, &v)) && v)
                    sink.nInterlaced++;
                v = 0;
                if (SUCCEEDED(pSample->GetUINT32(MFSampleExtension_BottomFieldFirst, &v)) && v)
                    sink.nBottomFirst++;
            }
            SAFE_RELEASE(pContig);
            SAFE_RELEASE(pSample);

            sink.times.push_back(rt);
            n++;
            sink.frames++;

            if (maxFrames > 0 && n >= maxFrames)
            {
                printf("stream %lu: stopped at %ld samples (limit)\n", ix, n);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                printf("stream %lu: end of stream after %ld samples\n", ix, n);
                break;
            }
        }

        if (bVideo)
        {
            printf("video stream %lu: %ld frames %ux%u strideY=%u mean Y=%.1f last time %.3f s\n"
                   "           field flags on samples: interlaced=%ld bottom-first=%ld of %ld\n",
                   ix, n, sink.w, sink.h, sink.strideY,
                   n ? sumY / ((double)n * (double)(sink.strideY * sink.h)) : 0.0,
                   sink.times.empty() ? 0.0 : (double)sink.times.back() / 1e7,
                   sink.nInterlaced, sink.nBottomFirst, n);
        }
        else
        {
            printf("audio stream %lu: %ld buffers, %zu bytes\n", ix, n, sink.pcm.size());

            // audio has to come out back to back: buffer N ends exactly where buffer
            // N+1 starts, otherwise the audio renderer will stutter or overlap
            if (!sink.sizes.empty())
            {
                IMFMediaType* pT = nullptr;
                UINT32 rate = 48000, ch = 2, bits = 16;
                if (SUCCEEDED(pSR->GetCurrentMediaType(ix, &pT)) && pT)
                {
                    rate = MFGetAttributeUINT32(pT, MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                    ch = MFGetAttributeUINT32(pT, MF_MT_AUDIO_NUM_CHANNELS, ch);
                    bits = MFGetAttributeUINT32(pT, MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
                    SAFE_RELEASE(pT);
                }
                const DWORD blockBytes = ch * (bits / 8);
                bool bContiguous = true;
                LONGLONG llBad = 0;
                for (size_t i = 0; i + 1 < sink.times.size(); i++)
                {
                    const LONGLONG expect = sink.times[i] +
                        (LONGLONG)((double)sink.sizes[i] / blockBytes / rate * 1e7 + 0.5);
                    if (sink.times[i + 1] != expect)
                    {
                        bContiguous = false;
                        llBad = (LONGLONG)i;
                        break;
                    }
                }
                if (bContiguous)
                    printf("audio timing: %lu Hz %u ch, blocks back to back [contiguous OK]\n", rate, ch);
                else
                    printf("audio timing: gap/overlap at block %lld (expected %.3f s, got %.3f s)\n",
                           llBad,
                           (double)(sink.times[llBad] + (LONGLONG)((double)sink.sizes[llBad] / blockBytes / rate * 1e7 + 0.5)) / 1e7,
                           (double)sink.times[llBad + 1] / 1e7);
            }
        }

        if (!sink.times.empty())
        {
            bool monotonic = true;
            for (size_t i = 1; i < sink.times.size(); i++)
                if (sink.times[i] < sink.times[i - 1]) monotonic = false;
            printf("sample times (first 12 s):");
            for (size_t i = 0; i < sink.times.size() && i < 12; i++)
                printf(" %.3f", (double)sink.times[i] / 1e7);
            printf(" [%s] start %.3f s end %.3f s\n", monotonic ? "presentation order OK" : "NOT MONOTONIC",
                   (double)sink.times.front() / 1e7, (double)sink.times.back() / 1e7);
        }
    }

    if (sink.pYuv) fclose(sink.pYuv);

    // ---- write the audio as a WAV ----------------------------------------------------
    for (size_t ti = 0; ti < targets.size(); ti++)
    {
        if (targets[ti].bVideo || sink.pcm.empty() || outPath.empty())
            continue;
        FILE* f = _wfopen(outPath.c_str(), L"wb");
        if (!f)
            continue;
        IMFMediaType* pT = nullptr;
        UINT32 rate = 48000, ch = 2, bits = 16;
        if (SUCCEEDED(pSR->GetCurrentMediaType(targets[ti].ix, &pT)) && pT)
        {
            rate = MFGetAttributeUINT32(pT, MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            ch = MFGetAttributeUINT32(pT, MF_MT_AUDIO_NUM_CHANNELS, ch);
            bits = MFGetAttributeUINT32(pT, MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
            SAFE_RELEASE(pT);
        }
        WAVEFORMATEX wf;
        wf.wFormatTag = 1;
        wf.nChannels = (WORD)ch;
        wf.nSamplesPerSec = rate;
        wf.nAvgBytesPerSec = rate * ch * (bits / 8);
        wf.nBlockAlign = (WORD)(ch * (bits / 8));
        wf.wBitsPerSample = (WORD)bits;
        wf.cbSize = 0;
        const DWORD dsize = (DWORD)sink.pcm.size();
        const DWORD riffSize = 36 + dsize;
        fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); const DWORD fsize = 16; fwrite(&fsize, 4, 1, f);
        // sizeof(WAVEFORMATEX) is 18: the trailing cbSize does not belong to a 16 byte
        // PCM fmt chunk and would shift the data chunk by two bytes.
        fwrite(&wf, 1, sizeof(wf) - sizeof(wf.cbSize), f);
        fwrite("data", 1, 4, f); fwrite(&dsize, 4, 1, f);
        fwrite(&sink.pcm[0], 1, dsize, f);
        fclose(f);
        printf("wrote %s (%u Hz, %u ch, %u bit, %lu bytes)\n", NarrowAscii(outPath).c_str(), rate, ch, bits,
               (unsigned long)dsize);
    }

    if (bmpAt >= 0 && !sink.lastY.empty() && sink.w && sink.h)
    {
        std::wstring bmp = outPath.empty() ? inPath + L".bmp" : outPath + L".bmp";
        WriteBmp(bmp.c_str(), &sink.lastY[0], sink.lastUV.empty() ? nullptr : &sink.lastUV[0],
                 sink.w, sink.h, sink.strideY, sink.strideUV);
        printf("wrote %s\n", NarrowAscii(bmp).c_str());
    }

    printf("result: %ld sample(s), %ld media type change(s)\n", sink.frames, sink.formatChanges);

    SAFE_RELEASE(pSRAttrs);
    SAFE_RELEASE(pSR);
    SAFE_RELEASE(pSource);
    MFShutdown();
    CoUninitialize();
    return 0;
}
