//--------------------------------------------------------------------------------------------------
// mftenum.cpp - list the Media Foundation video decoders that are registered, with
//               every attribute their activation objects carry.
//
//   mftenum                       all video decoders
//   mftenum <inputSubtypeGuid>    only the ones that accept that sub-type, e.g.
//   mftenum 20535641-0000-0010-8000-00AA00389B71     (MFVideoFormat_AVS)
//
// The attribute names of MFTEnumEx results are not exported as constants in the SDK
// headers, so the attributes are dumped generically: name GUID, value type, and for
// blobs the sub-types inside (MFT_REGISTER_TYPE_INFO is two GUIDs == 32 bytes).
//--------------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfapi.h>
#include <propsys.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mf")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")

static GUID ParseGuid(const std::string& s)
{
    GUID g = GUID_NULL;
    unsigned d1, d2, d3, b[8];
    if (sscanf_s(s.c_str(), "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return GUID_NULL;
    g.Data1 = (unsigned)d1; g.Data2 = (unsigned)d2; g.Data3 = (unsigned)d3;
    for (int i = 0; i < 8; i++)
        g.Data4[i] = (BYTE)b[i];
    return g;
}

static void PrintGuid(const GUID& g)
{
    printf("{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
           g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool FourCC(const GUID& g, char (&out)[5])
{
    if ((g.Data2 != 0 || g.Data3 != 0x0010) ||
        g.Data4[0] != 0x80 || g.Data4[1] != 0x00 || g.Data4[2] != 0x00 ||
        g.Data4[3] != 0xAA || g.Data4[4] != 0x00 || g.Data4[5] != 0x38 ||
        g.Data4[6] != 0x9B || g.Data4[7] != 0x71)
        return false;
    out[0] = (char)(g.Data1 & 0xFF);
    out[1] = (char)((g.Data1 >> 8) & 0xFF);
    out[2] = (char)((g.Data1 >> 16) & 0xFF);
    out[3] = (char)((g.Data1 >> 24) & 0xFF);
    out[4] = 0;
    for (int i = 0; i < 4; i++)
        if (out[i] < 0x20 || out[i] > 0x7E)
            return false;
    return true;
}

static void PrintType(const GUID& subtype)
{
    char cc[5];
    if (FourCC(subtype, cc))
        printf("'%s' ", cc);
    PrintGuid(subtype);
}

static void DumpAttributes(IMFAttributes* pA, const GUID& wantSubtype, bool& matches)
{
    UINT32 count = 0;
    pA->GetCount(&count);
    for (UINT32 i = 0; i < count; i++)
    {
        GUID key;
        PROPVARIANT v; PropVariantInit(&v);
        if (FAILED(pA->GetItemByIndex(i, &key, &v)))
            continue;

        printf("    ");
        PrintGuid(key);
        printf(" type=%d ", (int)v.vt);
        switch (v.vt)
        {
        case VT_UI4:
            printf("0x%X (%u)", v.ulVal, v.ulVal);
            break;
        case VT_UI8:
            printf("0x%llX", (unsigned long long)v.uhVal.QuadPart);
            break;
        case VT_LPWSTR:
            wprintf(L"%s", v.pwszVal);
            break;
        case VT_CLSID:
            if (v.puuid)
                PrintGuid(*v.puuid);
            break;
        case (VT_VECTOR | VT_UI1):
            if (v.caub.cElems % sizeof(MFT_REGISTER_TYPE_INFO) == 0 && v.caub.cElems)
            {
                MFT_REGISTER_TYPE_INFO* p = (MFT_REGISTER_TYPE_INFO*)v.caub.pElems;
                for (size_t k = 0; k < v.caub.cElems / sizeof(MFT_REGISTER_TYPE_INFO); k++)
                {
                    printf("\n        pair: major ");
                    PrintType(p[k].guidMajorType);
                    printf(" sub ");
                    PrintType(p[k].guidSubtype);
                    if (wantSubtype != GUID_NULL && p[k].guidSubtype == wantSubtype)
                        matches = true;
                }
            }
            else
                printf("<blob %u bytes>", v.caub.cElems);
            break;
        default:
            printf("<unsupported>");
            break;
        }
        printf("\n");
        PropVariantClear(&v);
    }
}

int main(int argc, char** argv)
{
    // usage: mftenum [input-subtype-guid] [video|audio]
    GUID want = (argc > 1) ? ParseGuid(argv[1]) : GUID_NULL;
    bool audio = (argc > 2) && _stricmp(argv[2], "audio") == 0;

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
        return 1;
    if (FAILED(MFStartup(MF_VERSION)))
        return 1;

    IMFActivate** ppActivate = nullptr;
    UINT32 c = 0;
    HRESULT hr = MFTEnumEx(audio ? MFT_CATEGORY_AUDIO_DECODER : MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                           MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, nullptr, &ppActivate, &c);
    printf("MFTEnumEx(%s decoder) -> 0x%08X, %u entries\n", audio ? "audio" : "video", hr, c);
    if (FAILED(hr))
        return 1;

    for (UINT32 i = 0; i < c; i++)
    {
        IMFAttributes* pA = ppActivate[i];

        // first pass: does it accept the sub-type we are interested in?
        bool matches = (want == GUID_NULL);
        if (!matches)
        {
            UINT32 count = 0;
            pA->GetCount(&count);
            for (UINT32 k = 0; k < count && !matches; k++)
            {
                GUID key;
                PROPVARIANT v; PropVariantInit(&v);
                if (FAILED(pA->GetItemByIndex(k, &key, &v)))
                    continue;
                if (v.vt == (VT_VECTOR | VT_UI1))
                {
                    MFT_REGISTER_TYPE_INFO* p = (MFT_REGISTER_TYPE_INFO*)v.caub.pElems;
                    for (size_t j = 0; j < v.caub.cElems / sizeof(MFT_REGISTER_TYPE_INFO); j++)
                        if (p[j].guidSubtype == want)
                            matches = true;
                }
                PropVariantClear(&v);
            }
        }
        if (!matches)
        {
            ppActivate[i]->Release();
            continue;
        }

        printf("\nentry %u\n", i);
        bool dummy = false;
        DumpAttributes(pA, want, dummy);

        // and create it, to be sure the class really instantiates as an MFT
        IMFTransform* pMFT = nullptr;
        HRESULT h2 = ppActivate[i]->ActivateObject(IID_PPV_ARGS(&pMFT));
        printf("    ActivateObject -> 0x%08X", h2);
        if (SUCCEEDED(h2))
        {
            DWORD in = 0, out = 0;
            pMFT->GetStreamCount(&in, &out);
            printf(" (in=%u out=%u)", in, out);

            if (want != GUID_NULL)
            {
                // The async MFT contract (MF_TRANSFORM_ASYNC + IMFMediaEventGenerator) is
                // what the newer system decoders implement.  Whether a third party MFT
                // that only implements IMFTransform is treated differently by the media
                // session is worth knowing, so report both here.
                IMFMediaEventGenerator* pEv = nullptr;
                bool bEventGen = SUCCEEDED(pMFT->QueryInterface(IID_PPV_ARGS(&pEv)));
                if (pEv) pEv->Release();
                IMFAttributes* pMA = nullptr;
                BOOL bAsync = FALSE, bAsyncUnlock = FALSE;
                if (SUCCEEDED(pMFT->GetAttributes(&pMA)) && pMA)
                {
                    bAsync = (BOOL)pMA->GetUINT32(MF_TRANSFORM_ASYNC, 0);
                    bAsyncUnlock = (BOOL)pMA->GetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 0);
                    pMA->Release();
                }
                printf("  eventgenerator=%d MF_TRANSFORM_ASYNC=%d ASYNC_UNLOCK=%d",
                       (int)bEventGen, (int)bAsync, (int)bAsyncUnlock);
            }
            pMFT->Release();
        }
        printf("\n");

        ppActivate[i]->Release();
    }

    CoTaskMemFree(ppActivate);
    MFShutdown();
    CoUninitialize();
    return 0;
}
