//--------------------------------------------------------------------------------------------------
// mfprobe.cpp -- what does Media Foundation's *built-in* pipeline make of a file?
//
// Diagnostic used to decide which input sub-type the AVS MFT decoder has to
// advertise: the built-in MPEG-2 TS byte stream handler decides how a TS
// stream_type of 0x42 (AVS / AVS+) - or the DRA audio stream - becomes an
// IMFMediaType, and whether the stream is dropped entirely.  The "native"
// media types reported by the source reader are exactly the types an MFT video
// decoder would be handed as input.
//
//   mfprobe <file>
//--------------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

static std::string S(const WCHAR* w)
{
    char tmp[4096] = { 0 };
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, sizeof(tmp) - 1, NULL, NULL);
    return tmp;
}

static std::string G(const GUID& g)
{
    char s[80];
    sprintf_s(s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
              g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;
}

static void DumpAttributes(IMFAttributes* a, const char* tag)
{
    if (!a) { printf("%s: (null)\n", tag); return; }
    UINT32 cnt = 0;
    a->GetCount(&cnt);
    printf("  --- %s: %u attribute(s) ---\n", tag, cnt);
    for (UINT32 i = 0; i < cnt; i++)
    {
        GUID key = GUID_NULL;
        PROPVARIANT v{};
        if (FAILED(a->GetItemByIndex(i, &key, &v)))
            break;
        switch (v.vt)
        {
        case MF_ATTRIBUTE_STRING:
            printf("  [%2u] %-42s str = %s\n", i, G(key).c_str(), S(v.bstrVal).c_str());
            break;
        case MF_ATTRIBUTE_UINT32:
            printf("  [%2u] %-42s u32 = %u (0x%x)\n", i, G(key).c_str(), v.ulVal, v.ulVal);
            break;
        case MF_ATTRIBUTE_UINT64:
            printf("  [%2u] %-42s u64 = %llu (0x%llx)", i, G(key).c_str(),
                   (unsigned long long)v.uhVal.QuadPart, (unsigned long long)v.uhVal.QuadPart);
            if (key == MF_MT_FRAME_SIZE)
                printf("   -> %ux%u", (UINT32)(v.uhVal.QuadPart >> 32), (UINT32)(v.uhVal.QuadPart & 0xFFFFFFFF));
            else if (key == MF_MT_FRAME_RATE || key == MF_MT_FRAME_RATE_RANGE_MAX ||
                     key == MF_MT_FRAME_RATE_RANGE_MIN || key == MF_MT_PIXEL_ASPECT_RATIO)
                printf("   -> %u/%u", (UINT32)(v.uhVal.QuadPart >> 32), (UINT32)(v.uhVal.QuadPart & 0xFFFFFFFF));
            printf("\n");
            break;
        case MF_ATTRIBUTE_GUID:
            printf("  [%2u] %-42s guid = %s\n", i, G(key).c_str(), G(*v.puuid).c_str());
            break;
        case MF_ATTRIBUTE_BLOB:
            printf("  [%2u] %-42s blob = %u bytes", i, G(key).c_str(), (unsigned)v.caub.cElems);
            if (v.caub.cElems && v.caub.cElems <= 32)
            {
                printf("   ");
                for (UINT32 k = 0; k < v.caub.cElems; k++)
                    printf(" %02X", v.caub.pElems[k]);
            }
            printf("\n");
            break;
        default:
            printf("  [%2u] %-42s vt=%u\n", i, G(key).c_str(), v.vt);
            break;
        }
        PropVariantClear(&v);
    }
}


static void EnumerateNativeTypes(IMFSourceReader* reader)
{
    for (DWORD stream = 0; stream < 8; stream++)
    {
        UINT32 typeIdx = 0;
        IMFMediaType* mt = NULL;
        bool any = false;
        while (SUCCEEDED(reader->GetNativeMediaType(stream, typeIdx, &mt)))
        {
            DumpAttributes(mt, "NATIVE media type (this is the input a decoder MFT gets)");
            mt->Release();
            mt = NULL;
            any = true;
            typeIdx++;
        }
        if (!any)
        {
            if (stream == 0)
                printf("(stream 0 not available)\n");
            break;
        }
        printf("== STREAM %u : %u native media type(s)\n", stream, typeIdx);
    }
}

static void Probe(IMFSourceResolver* resolver, WCHAR* url, DWORD flags, const char* label)
{
    printf("\n################ IMFSourceResolver::CreateObjectFromURL flags=0x%X (%s) ##############\n", flags, label);
    IUnknown* object = NULL;
    MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
    IMFMediaSource* src = NULL;
    IMFSourceReader* reader = NULL;

    HRESULT hr = resolver->CreateObjectFromURL(url, flags, NULL, &objectType, &object);
    printf("CreateObjectFromURL -> hr=0x%08X, object type=%d\n", hr, (int)objectType);
    if (FAILED(hr) || !object)
    {
        printf("MF could not open the file at all\n");
        return;
    }
    if (objectType == MF_OBJECT_BYTESTREAM)
        printf("!!! only a BYTE STREAM: no MF top-level media source for this file\n");

    if (SUCCEEDED(object->QueryInterface(__uuidof(IMFMediaSource), (void**)&src)) && src)
    {
        IMFAttributes* sa = NULL;
        IMFMediaSourceEx* mx = NULL;
        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(src, NULL, &reader)))
        {
            if (SUCCEEDED(src->QueryInterface(__uuidof(IMFMediaSourceEx), (void**)&mx)) && mx)
            {
                if (SUCCEEDED(mx->GetSourceAttributes(&sa)) && sa)
                {
                    DumpAttributes(sa, "source attributes");
                    sa->Release();
                }
                mx->Release();
            }
        }
    }
    else
    {
        IMFByteStream* bs = NULL;
        if (SUCCEEDED(object->QueryInterface(__uuidof(IMFByteStream), (void**)&bs)) && bs)
        {
            HRESULT hr2 = MFCreateSourceReaderFromByteStream(bs, NULL, &reader);
            if (FAILED(hr2))
                printf("MFCreateSourceReaderFromByteStream: 0x%08X -> MF has no parser for this "
                       "container, so no decoder MFT can be attached either\n", hr2);
            bs->Release();
        }
    }

    if (reader)
        EnumerateNativeTypes(reader);

    if (reader) reader->Release();
    if (src) src->Release();
    object->Release();
}

int wmain(int argc, WCHAR** argv)
{
    if (argc < 2)
    {
        wprintf(L"usage: mfprobe <media file>\n");
        return 1;
    }
    if (FAILED(MFStartup(MF_VERSION)))
        return 1;

    IMFSourceResolver* resolver = NULL;
    if (FAILED(MFCreateSourceResolver(&resolver)))
    {
        MFShutdown();
        return 1;
    }

    Probe(resolver, argv[1], MF_RESOLUTION_BYTESTREAM | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
          "force byte stream handler");
    Probe(resolver, argv[1], MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE,
          "force top level media source (what Media Player / Movies & TV does)");

    resolver->Release();
    MFShutdown();
    return 0;
}
