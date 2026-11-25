//----------------------------------------------------------------------------------------------
// DllMain.cpp - COM server entry points, MFT registration and byte stream handler
//               registration.
//
// What DllRegisterServer writes:
//
//   HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32     for the two decoder MFTs, the
//                                                        media source and the handler
//   HKLM\SOFTWARE\Microsoft\Windows Media Foundation\ByteStreamHandlers\<ext>
//                                                        a value named with the CLSID of
//                                                        our handler (exactly how the
//                                                        system registers .mp4/.mkv/.ts),
//                                                        so that a player opening the
//                                                        file ends up in CAVSByteStreamHandler
//   the MFT category registrations for the video/audio decoder categories (MFTRegister)
//
// All of that is HKLM, so it has to run elevated (regsvr32 in an administrator prompt).
// Set AVS_MFT_REGISTER_TS=1 (or AVS_MFT_REGISTER_CONTAINERS=1) to also ask for the
// container extensions .ts/.m2ts/.mkv.  Measured here: those keys belong to the system
// and refuse an Administrator write, so the attempt is refused and reported; the two
// decoder MFTs, the media source and the handler class objects still register, and our
// handler is reached for a container file whenever the system's own handler declines it
// (that is how an AVS-only .ts plays today).  Decoding does not depend on any of this:
// see scripts/container-decode-test.py.
//----------------------------------------------------------------------------------------------
#include "StdAfx.h"
#include "AVSVideoDecoderMFT.h"
#include "AVSAudioDecoderMFT.h"
#include "AVSMediaSource.h"

HMODULE g_hModule;

DEFINE_CLASSFACTORY_SERVER_LOCK

static const WCHAR SZ_DECODER_NAME[] = L"AVS/AVS+/AVS2/AVS3 Video Decoder (FFmpeg)";
static const WCHAR SZ_AUDIO_DECODER_NAME[] = L"DRA (AVS audio) Decoder (FFmpeg)";
static const WCHAR SZ_SOURCE_NAME[] = L"AVS/AVS+/AVS2/AVS3 Media Source (FFmpeg)";
static const WCHAR SZ_HANDLER_NAME[] = L"AVS/AVS+/AVS2/AVS3 Byte Stream Handler (FFmpeg)";

static const WCHAR SZ_BSH_KEY[] = L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers";

static const WCHAR* kDefaultExtensions[] = { L".avs", L".cavs", L".avs2", L".avs3" };
// Container extensions.  The decoder is container-blind: given an AVS bitstream it
// decodes it whatever wrapped it (scripts/container-decode-test.py measures that).  What
// these registrations decide is only whether Media Foundation asks our byte stream handler
// for such a file at all.  On a normal Windows install the system already owns these keys
// and they are write-protected even for an Administrator, so the attempt is expected to
// be refused; it is reported rather than hidden.
static const WCHAR* kContainerExtensions[] = { L".ts", L".m2ts", L".mkv" };

BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls((HMODULE)hModule);
        g_hModule = (HMODULE)hModule;
        break;
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    HRESULT hr = ClassFactory::IsLocked() ? S_FALSE : S_OK;
    if (getenv("AVS_MFT_TRACE"))
        fprintf(stderr, "[avsmft] DllCanUnloadNow -> %s\n",
                hr == S_FALSE ? "S_FALSE (objects of this DLL are alive)" : "S_OK");
    return hr;
}

static void GuidToString(const GUID& g, WCHAR* sz, size_t cch)
{
    StringFromGUID2(g, sz, (int)cch);
}

// How MF really stores this (as the system does for .mp4 / .mkv / .ts): a sub-key per
// extension, and inside it one REG_SZ value whose *name* is the handler CLSID.  The
// (Default) value is empty and a value on the parent key is ignored - both mistakes show
// up as MF_E_UNSUPPORTED_BYTESTREAM_TYPE from MFCreateSourceReaderFromURL.  Several
// handlers may share one extension, which is why nothing is destroyed on registration.
static HRESULT SetHandlerKey(const WCHAR* pwszExt, const GUID& clsid)
{
    WCHAR szClsid[64];
    GuidToString(clsid, szClsid, ARRAY_SIZE(szClsid));

    WCHAR szSub[512];
    if (FAILED(StringCchPrintfW(szSub, ARRAY_SIZE(szSub), L"%s\\%s", SZ_BSH_KEY, pwszExt)))
        return E_INVALIDARG;

    HKEY hKey = NULL;
    LSTATUS ls = RegCreateKeyExW(HKEY_LOCAL_MACHINE, szSub, 0, NULL,
                                 REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
                                 NULL, &hKey, NULL);
    if (ls != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(ls);

    static const WCHAR szEmpty[] = L"";
    ls = RegSetValueExW(hKey, szClsid, 0, REG_SZ, (const BYTE*)szEmpty, sizeof(szEmpty));
    RegDeleteValueW(hKey, NULL);          // an earlier build wrote the CLSID here
    RegCloseKey(hKey);
    return (ls == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(ls);
}

static void DeleteHandlerKey(const WCHAR* pwszExt)
{
    // an even earlier build wrote a value on the parent key; remove that too
    HKEY hParent = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SZ_BSH_KEY, 0,
                      KEY_WRITE | KEY_WOW64_64KEY, &hParent) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hParent, pwszExt);
        RegCloseKey(hParent);
    }

    WCHAR szClsid[64];
    GuidToString(CLSID_AVSByteStreamHandler, szClsid, ARRAY_SIZE(szClsid));

    WCHAR szSub[512];
    if (FAILED(StringCchPrintfW(szSub, ARRAY_SIZE(szSub), L"%s\\%s", SZ_BSH_KEY, pwszExt)))
        return;

    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szSub, 0,
                      KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return;

    RegDeleteValueW(hKey, szClsid);
    RegDeleteValueW(hKey, NULL);

    // drop the extension key only when we were the only handler registered for it
    DWORD cValues = 0, cchMax = 0;
    if (RegQueryInfoKeyW(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &cValues, &cchMax, NULL, NULL, NULL) == ERROR_SUCCESS &&
        cValues == 0)
    {
        RegCloseKey(hKey);
        hKey = NULL;
        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, szSub, KEY_WOW64_64KEY, 0);
    }
    if (hKey)
        RegCloseKey(hKey);
}

static bool RegisterTsExtensions(void)
{
    WCHAR buf[8] = { 0 };
    if (GetEnvironmentVariableW(L"AVS_MFT_REGISTER_TS", buf, ARRAY_SIZE(buf)) == 0)
        return false;
    return buf[0] != L'0';
}

// Same switch under a name that says what it covers: the container extensions.  Either
// variable turns the container registrations on; AVS_MFT_REGISTER_TS is kept because the
// scripts and the report already use it.
static bool RegisterContainerExtensions(void)
{
    WCHAR buf[8] = { 0 };
    if (GetEnvironmentVariableW(L"AVS_MFT_REGISTER_CONTAINERS", buf, ARRAY_SIZE(buf)) != 0)
        return buf[0] != L'0';
    return RegisterTsExtensions();
}

static bool DllTraceOn(void)
{
    WCHAR buf[4] = { 0 };
    return GetEnvironmentVariableW(L"AVS_MFT_TRACE", buf, ARRAY_SIZE(buf)) != 0 && buf[0] != L'0';
}

// One extension, one result, never aborts the loop.  This matters: the system owns the
// keys for .ts/.m2ts/.mkv/.mp4 and they refuse an Administrator write (measured:
// reg add from an elevated prompt -> "拒绝访问", and RegCreateKeyEx/RegSetValueEx return
// ERROR_ACCESS_DENIED), while extensions nobody owns (.avs, .cavs, .avs2, .avs3) accept
// it.  An earlier build stopped at the first refusal, which (a) silently skipped the rest
// of the list and (b) made DllRegisterServer report failure although both decoders were
// registered - that is how "we registered .ts" came to be believed here without ever
// having been true.  Say which one happened instead.
static HRESULT TryHandlerKey(const WCHAR* pwszExt)
{
    HRESULT hr = SetHandlerKey(pwszExt, CLSID_AVSByteStreamHandler);
    if (DllTraceOn())
        fprintf(stderr, "[avsmft] handler registration for %S: %s (0x%08X)\n", pwszExt,
                SUCCEEDED(hr) ? "accepted" : "refused", (unsigned)hr);
    return hr;
}

STDAPI DllRegisterServer()
{
    HRESULT hr = RegisterObject(g_hModule, CLSID_AVSVideoDecoder, SZ_DECODER_NAME, TEXT("Both"));

    if (SUCCEEDED(hr))
    {
        MFT_REGISTER_TYPE_INFO inputTypes[] = {
            { MFMediaType_Video, MFVideoFormat_AVS  },
            { MFMediaType_Video, MFVideoFormat_AVS2 },
            { MFMediaType_Video, MFVideoFormat_AVS3 },
        };
        MFT_REGISTER_TYPE_INFO outputTypes[] = {
            { MFMediaType_Video, MFVideoFormat_NV12 },
            { MFMediaType_Video, MFVideoFormat_I420 },
            { MFMediaType_Video, MFVideoFormat_YV12 },
            // 10 bit samples for the 10 bit AVS2/AVS3 streams; a sink that cannot take
            // them still gets NV12, which is what the enumeration order means
            { MFMediaType_Video, MFVideoFormat_P010 },
        };

        hr = MFTRegister(CLSID_AVSVideoDecoder, MFT_CATEGORY_VIDEO_DECODER,
                         const_cast<LPWSTR>(SZ_DECODER_NAME), 0,
                         (UINT32)ARRAY_SIZE(inputTypes), inputTypes,
                         (UINT32)ARRAY_SIZE(outputTypes), outputTypes, NULL);
    }

    if (SUCCEEDED(hr))
    {
        hr = RegisterObject(g_hModule, CLSID_AVSAudioDecoder, SZ_AUDIO_DECODER_NAME, TEXT("Both"));
    }
    if (SUCCEEDED(hr))
    {
        MFT_REGISTER_TYPE_INFO audioIn[] = {
            { MFMediaType_Audio, MFAudioFormat_DRA },
        };
        MFT_REGISTER_TYPE_INFO audioOut[] = {
            { MFMediaType_Audio, MFAudioFormat_PCM   },
            { MFMediaType_Audio, MFAudioFormat_Float },
        };
        hr = MFTRegister(CLSID_AVSAudioDecoder, MFT_CATEGORY_AUDIO_DECODER,
                         const_cast<LPWSTR>(SZ_AUDIO_DECODER_NAME), 0,
                         (UINT32)ARRAY_SIZE(audioIn), audioIn,
                         (UINT32)ARRAY_SIZE(audioOut), audioOut, NULL);
    }

    // the media source and the handler that reaches it
    if (SUCCEEDED(hr))
        hr = RegisterObject(g_hModule, CLSID_AVSMediaSource, SZ_SOURCE_NAME, TEXT("Both"));
    if (SUCCEEDED(hr))
        hr = RegisterObject(g_hModule, CLSID_AVSByteStreamHandler, SZ_HANDLER_NAME, TEXT("Both"));

    // The handler registrations are independent of each other and of the two decoders:
    // a refused extension must not stop the list, and must not turn the whole call into a
    // failure.  The core of the installation is the two decoder MFTs plus our media
    // source and handler class objects; those are what the topology loader uses.
    if (SUCCEEDED(hr))
    {
        HRESULT hrExt = S_OK;
        for (size_t i = 0; i < ARRAY_SIZE(kDefaultExtensions); i++)
        {
            HRESULT hrOne = TryHandlerKey(kDefaultExtensions[i]);
            if (FAILED(hrOne)) hrExt = hrOne;
        }
        if (RegisterContainerExtensions())
        {
            for (size_t i = 0; i < ARRAY_SIZE(kContainerExtensions); i++)
            {
                HRESULT hrOne = TryHandlerKey(kContainerExtensions[i]);
                if (FAILED(hrOne)) hrExt = hrOne;
            }
        }
        if (FAILED(hrExt) && DllTraceOn())
            fprintf(stderr, "[avsmft] at least one byte stream handler registration was "
                            "refused (0x%08X): the decoders and the media source are "
                            "registered anyway; see scripts/container-decode-test.py for "
                            "what still plays\n", (unsigned)hrExt);
    }
    return hr;
}

STDAPI DllUnregisterServer()
{
    for (size_t i = 0; i < ARRAY_SIZE(kDefaultExtensions); i++)
        DeleteHandlerKey(kDefaultExtensions[i]);
    for (size_t i = 0; i < ARRAY_SIZE(kContainerExtensions); i++)
        DeleteHandlerKey(kContainerExtensions[i]);

    UnregisterObject(CLSID_AVSByteStreamHandler);
    UnregisterObject(CLSID_AVSMediaSource);
    UnregisterObject(CLSID_AVSVideoDecoder);
    MFTUnregister(CLSID_AVSVideoDecoder);
    UnregisterObject(CLSID_AVSAudioDecoder);
    MFTUnregister(CLSID_AVSAudioDecoder);
    return S_OK;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;

    static const struct
    {
        const CLSID*           pClsid;
        CreateInstanceFn       pfnCreate;
    } kClasses[] = {
        { &CLSID_AVSVideoDecoder,      CAVSVideoDecoderMFT::CreateInstance },
        { &CLSID_AVSAudioDecoder,      CAVSAudioDecoderMFT::CreateInstance },
        { &CLSID_AVSMediaSource,       CAVSMediaSource::CreateInstance },
        { &CLSID_AVSByteStreamHandler, CAVSByteStreamHandler::CreateInstance },
    };

    for (size_t i = 0; i < ARRAY_SIZE(kClasses); i++)
    {
        if (clsid != *kClasses[i].pClsid)
            continue;

        ClassFactory* pFactory = new (std::nothrow) ClassFactory(kClasses[i].pfnCreate);
        if (pFactory == NULL)
            return E_OUTOFMEMORY;
        hr = pFactory->QueryInterface(riid, ppv);
        SAFE_RELEASE(pFactory);
        break;
    }
    return hr;
}
