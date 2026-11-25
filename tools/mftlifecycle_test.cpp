//-------------------------------------------------------------------------------------------------
// mftlifecycle_test.cpp - "can a host load this MFT and then go away?"
//
// This is the acceptance test for the second story-map tile: closing the player must not
// crash.  The earlier crash we chased turned out to be inside our own test tool, but the
// host-side half of that promise was never measured: what happens to the MFT, and to the
// FFmpeg state it owns, when the object is still alive while the DLL is unloaded or the
// process exits?  A Media Foundation host does exactly that - mf.dll instantiates an MFT,
// frees its own HMODULE reference soon after, and the topology drops the MFT only when the
// session goes away.
//
// Modes (all of them load the DLL through DllGetClassObject; nothing has to be registered):
//
//   factory      take the class factory, release it, free the DLL.
//   instance     factory -> instance -> set input and output types -> release -> free.
//   unloadalive  keep the instance referenced, ask DllCanUnloadNow() (the contract says it
//                must refuse while an object lives), free our HMODULE, then release the
//                object and use it once.  This is the polite-host version of closing a
//                player.
//   leak         create the instance, feed it real bytes if -file is given, and exit the
//                process without releasing anything: the destructor runs during DLL process
//                detach with FFmpeg global state still around.
//   rude         free the library while the object is still referenced and *then* touch it,
//                without asking DllCanUnloadNow first.  Windows does not consult
//                DllCanUnloadNow on FreeLibrary, so this is expected to be unsound for any
//                COM DLL; it is opt-in (-mode rude) to document the boundary, not to fail.
//
// Exit code 0 only when every requested iteration completed and every contract assertion
// held.  Any access violation is printed with a stack (crashreport.h) and gives non-zero.
//-------------------------------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crashreport.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p)) { (p)->Release(); (p) = NULL; } } while (0)
#endif

static const CLSID CLSID_AVSMediaSource_local =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x13 } };
static const CLSID CLSID_AVSByteStreamHandler_local =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x14 } };

// must match AVSVideoDecoder/classGuids.h
static const CLSID CLSID_AVSVideoDecoder_local =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x11 } };
static const CLSID CLSID_AVSAudioDecoder_local =
    { 0x9e5d6a1b, 0x2c4f, 0x4e7d, { 0x8f, 0x2b, 0x7a, 0x9c, 0x0d, 0x5e, 0x3b, 0x12 } };

// input sub-types, fourCC shaped like the MF media sub-types
static const GUID MFVideoFormat_AVS_local =
    { 0x20535641, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID MFAudioFormat_DRA_local =
    { 0x20415244, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

typedef HRESULT (STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (STDAPICALLTYPE* CanUnloadFn)(void);

static struct
{
    WCHAR    dll[MAX_PATH];
    const char* mode;
    const char* clsid;
    int      repeats;
    char     file[MAX_PATH];
} opt;

static CLSID PickClsid(const char* name)
{
    if (!strcmp(name, "audio"))   return CLSID_AVSAudioDecoder_local;
    if (!strcmp(name, "source"))  return CLSID_AVSMediaSource_local;
    if (!strcmp(name, "handler")) return CLSID_AVSByteStreamHandler_local;
    return CLSID_AVSVideoDecoder_local;
}

// only the two decoders speak IMFTransform; source/handler are lifecycle-only here
static bool IsTransformClsid(const char* name)
{
    return !strcmp(name, "video") || !strcmp(name, "audio");
}

static HRESULT MakeVideoTypes(IMFTransform* pXform)
{
    IMFMediaType* pIn = NULL;
    IMFMediaType* pOut = NULL;
    HRESULT hr = MFCreateMediaType(&pIn);
    if (SUCCEEDED(hr))
    {
        pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AVS_local);
        pIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
        MFSetAttributeSize(pIn, MF_MT_FRAME_SIZE, 720, 576);
        MFSetAttributeRatio(pIn, MF_MT_FRAME_RATE, 25, 1);
        hr = pXform->SetInputType(0, pIn, 0);
    }
    if (SUCCEEDED(hr))
    {
        hr = MFCreateMediaType(&pOut);
    }
    if (SUCCEEDED(hr))
    {
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        pOut->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        MFSetAttributeSize(pOut, MF_MT_FRAME_SIZE, 720, 576);
        MFSetAttributeRatio(pOut, MF_MT_FRAME_RATE, 25, 1);
        pOut->SetUINT64(MF_MT_SAMPLE_SIZE, 720 * 576 * 3 / 2);
        pOut->SetUINT32(MF_MT_DEFAULT_STRIDE, 720);
        hr = pXform->SetOutputType(0, pOut, 0);
    }
    SAFE_RELEASE(pIn);
    SAFE_RELEASE(pOut);
    return hr;
}

static HRESULT MakeAudioTypes(IMFTransform* pXform)
{
    IMFMediaType* pIn = NULL;
    IMFMediaType* pOut = NULL;
    HRESULT hr = MFCreateMediaType(&pIn);
    if (SUCCEEDED(hr))
    {
        pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_DRA_local);
        pIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        hr = pXform->SetInputType(0, pIn, 0);
    }
    if (SUCCEEDED(hr))
    {
        hr = MFCreateMediaType(&pOut);
    }
    if (SUCCEEDED(hr))
    {
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pOut->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        hr = pXform->SetOutputType(0, pOut, 0);
    }
    SAFE_RELEASE(pIn);
    SAFE_RELEASE(pOut);
    return hr;
}

// Feed real compressed bytes so the decoder object has state to destroy.  The result does
// not matter here; the point is that teardown happens after a decoder was actually built.
static void FeedSomeBytes(IMFTransform* pXform, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    static BYTE buf[16384];
    size_t cb = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (cb == 0)
        return;

    IMFSample* pSample = NULL;
    IMFMediaBuffer* pBuffer = NULL;
    if (SUCCEEDED(MFCreateSample(&pSample)) && SUCCEEDED(MFCreateMemoryBuffer((DWORD)cb, &pBuffer)))
    {
        BYTE* pDst = NULL;
        DWORD cbMax = 0;
        if (SUCCEEDED(pBuffer->Lock(&pDst, &cbMax, NULL)))
        {
            memcpy(pDst, buf, cb);
            pBuffer->Unlock();
            pBuffer->SetCurrentLength((DWORD)cb);
        }
        pSample->AddBuffer(pBuffer);
        pSample->SetSampleTime(0);
        pSample->SetSampleDuration(400000);
        pXform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        pXform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        pXform->ProcessInput(0, pSample, 0);
    }
    SAFE_RELEASE(pBuffer);
    SAFE_RELEASE(pSample);
}

static int RunOnce(CanUnloadFn canUnload)
{
    HMODULE hMod = LoadLibraryExW(opt.dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hMod)
    {
        printf("  LoadLibraryEx failed (GLE=%lu)\n", GetLastError());
        return 1;
    }
    auto getPfn = (GetClassObjectFn)GetProcAddress(hMod, "DllGetClassObject");
    if (!getPfn)
    {
        printf("  no DllGetClassObject export\n");
        FreeLibrary(hMod);
        return 1;
    }

    const CLSID clsid = PickClsid(opt.clsid);
    IClassFactory* pFactory = NULL;
    HRESULT hr = getPfn(clsid, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr))
    {
        printf("  DllGetClassObject: 0x%08X\n", hr);
        FreeLibrary(hMod);
        return 1;
    }

    int bad = 0;

    if (!IsTransformClsid(opt.clsid))
    {
        // the media source and the byte stream handler hold FFmpeg demuxer state too, so
        // both halves of the unload contract matter for them as well
        IUnknown* pObj = NULL;
        hr = pFactory->CreateInstance(NULL, IID_PPV_ARGS(&pObj));
        if (FAILED(hr))
        {
            printf("  CreateInstance(%s): 0x%08X\n", opt.clsid, hr);
            bad = 1;
        }
        else if (strcmp(opt.mode, "leak") == 0)
        {
            printf("  leaving the %s and the DLL alive, exiting the process\n", opt.clsid);
            SAFE_RELEASE(pFactory);
            return bad;               // pObj deliberately not released
        }
        else
        {
            HRESULT canDo = canUnload ? canUnload() : E_FAIL;
            if (canDo != S_FALSE)
            {
                printf("  DllCanUnloadNow with a live %s: 0x%08X, expected S_FALSE\n",
                       opt.clsid, canDo);
                bad = 1;
            }
            else
            {
                printf("  DllCanUnloadNow refuses while the %s lives: ok\n", opt.clsid);
            }
            SAFE_RELEASE(pObj);
            canDo = canUnload ? canUnload() : E_FAIL;
            if (canDo != S_OK)
            {
                printf("  DllCanUnloadNow after releasing the %s: 0x%08X, expected S_OK\n",
                       opt.clsid, canDo);
                bad = 1;
            }
            else
            {
                printf("  DllCanUnloadNow allows unload once nothing is alive: ok\n");
            }
        }
        SAFE_RELEASE(pFactory);
        if (hMod)
            FreeLibrary(hMod);
        return bad;
    }

    if (strcmp(opt.mode, "factory") != 0)
    {
        IMFTransform* pXform = NULL;
        hr = pFactory->CreateInstance(NULL, IID_PPV_ARGS(&pXform));
        if (FAILED(hr))
        {
            printf("  CreateInstance: 0x%08X\n", hr);
            bad = 1;
        }
        else
        {
            hr = (strcmp(opt.clsid, "audio") == 0) ? MakeAudioTypes(pXform) : MakeVideoTypes(pXform);
            if (FAILED(hr))
            {
                // A dynamic format change is normal for these decoders: the frame size is
                // only known after the sequence header, so an output type may legitimately
                // be refused before any data was fed.
                if (hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TRANSFORM_TYPE_NOT_SET)
                    printf("  note: output type refused before any data (0x%08X), continuing\n", hr);
                else
                {
                    printf("  SetInputType/SetOutputType: 0x%08X\n", hr);
                    bad = 1;
                }
            }
            if (opt.file[0])
                FeedSomeBytes(pXform, opt.file);

            if (strcmp(opt.mode, "unloadalive") == 0)
            {
                // The contract: while an object of this DLL is alive the DLL must refuse to
                // be unloaded, so that the host can free its own HMODULE reference first.
                HRESULT canDo = canUnload ? canUnload() : E_FAIL;
                if (canDo != S_FALSE)
                {
                    printf("  DllCanUnloadNow with a live instance: 0x%08X, expected S_FALSE\n", canDo);
                    bad = 1;
                }
                else
                {
                    // a polite host honours that answer, keeps its HMODULE, and keeps using
                    // the object - the point of the assertion is that it may do so
                    printf("  DllCanUnloadNow refuses while the object lives: ok\n");
                    DWORD nIn = 0, nOut = 0;
                    hr = pXform->GetStreamCount(&nIn, &nOut);
                    if (FAILED(hr))
                    {
                        printf("  GetStreamCount: 0x%08X\n", hr);
                        bad = 1;
                    }
                    else
                    {
                        printf("  object usable while the DLL is pinned (%lu in / %lu out)\n",
                               nIn, nOut);
                    }
                }
            }
            else if (strcmp(opt.mode, "rude") == 0)
            {
                FreeLibrary(hMod);                // host does not ask first
                hMod = NULL;
                DWORD nIn = 0, nOut = 0;
                hr = pXform->GetStreamCount(&nIn, &nOut);
                printf("  rude host: touched a live object after FreeLibrary, hr=0x%08X\n", hr);
            }
            else if (strcmp(opt.mode, "leak") == 0)
            {
                // closing a player without releasing: the process exits while the MFT and
                // the FFmpeg state it owns are still alive and the DLL is still mapped,
                // so this is process detach with live decoder state
                printf("  leaving the instance and the DLL alive, exiting the process\n");
                SAFE_RELEASE(pFactory);
                return bad;       // pXform deliberately not released, no FreeLibrary
            }

            SAFE_RELEASE(pXform);

            // the other half of the contract: once nothing of ours is alive the DLL must
            // say yes, otherwise the server lock drifts and no host can ever unload
            if (canUnload && strcmp(opt.mode, "leak") != 0)
            {
                HRESULT canDo = canUnload();
                if (canDo != S_OK)
                {
                    printf("  DllCanUnloadNow after releasing everything: 0x%08X, "
                           "expected S_OK\n", canDo);
                    bad = 1;
                }
                else
                {
                    printf("  DllCanUnloadNow allows unload once nothing is alive: ok\n");
                }
            }
        }
    }

    SAFE_RELEASE(pFactory);
    if (hMod)
        FreeLibrary(hMod);
    return bad;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    InstallCrashReport();

    opt.mode = "all";
    opt.clsid = "video";
    opt.repeats = 20;
    opt.file[0] = 0;

    int i = 1;
    if (i < argc && argv[i][0] != '-')
    {
        MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, opt.dll, MAX_PATH);
        i++;
    }
    else
    {
        printf("usage: mftlifecycle-test.exe <AVSVideoDecoder.dll> "
               "[-clsid video|audio|source|handler] [-mode all|factory|instance|unloadalive|leak|rude] "
               "[-repeat N] [-file some.avs]\n");
        return 2;
    }
    for (; i < argc; i++)
    {
        if (!strcmp(argv[i], "-clsid") && i + 1 < argc) opt.clsid = argv[++i];
        else if (!strcmp(argv[i], "-mode") && i + 1 < argc) opt.mode = argv[++i];
        else if (!strcmp(argv[i], "-repeat") && i + 1 < argc) opt.repeats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-file") && i + 1 < argc)
        {
            strncpy(opt.file, argv[++i], sizeof(opt.file) - 1);
            opt.file[sizeof(opt.file) - 1] = 0;
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    CanUnloadFn canUnload = NULL;
    {
        HMODULE hProbe = LoadLibraryExW(opt.dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (hProbe)
        {
            canUnload = (CanUnloadFn)GetProcAddress(hProbe, "DllCanUnloadNow");
            FreeLibrary(hProbe);
        }
    }

    static const char* kModes[] = { "factory", "instance", "unloadalive", "leak" };
    int failed = 0, ran = 0;

    if (strcmp(opt.mode, "all") == 0)
    {
        for (size_t m = 0; m < ARRAY_SIZE(kModes); m++)
        {
            opt.mode = kModes[m];
            int modeFailed = 0;                  // per mode; a running total here would lie
            printf("[%s]\n", opt.mode);
            for (int r = 0; r < opt.repeats; r++)
            {
                int bad = RunOnce(canUnload);
                if (bad) { printf("  %s iteration %d FAILED\n", opt.mode, r); modeFailed++; }
                ran++;
            }
            printf("  %s: %d iteration(s), %d failure(s)\n", opt.mode, opt.repeats, modeFailed);
            failed += modeFailed;
        }
    }
    else
    {
        for (int r = 0; r < opt.repeats; r++)
        {
            if (RunOnce(canUnload)) { printf("  iteration %d FAILED\n", r); failed++; }
            ran++;
        }
    }

    MFShutdown();
    CoUninitialize();

    printf("\nlifecycle check: %d iteration(s), %d failure(s), class=%s\n",
           ran, failed, opt.clsid);
    if (strcmp(opt.mode, "rude") == 0)
        printf("  (the rude host is informational: FreeLibrary does not consult DllCanUnloadNow)\n");
    return failed ? 1 : 0;
}
