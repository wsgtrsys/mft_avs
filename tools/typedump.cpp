// Dump the NATIVE media types a system byte stream handler reports for a file.
// Purpose: find out what subtype the system MKV / MPEG-2 handlers put on an AVS
// video stream, because that string is the only thing a decoder MFT can match on.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>

static const char* FourccName(const GUID& g)
{
    static char buf[16];
    if (g.Data4[6] == 0 && g.Data4[7] == 0 && (g.Data1 & 0xFF000000u)) {
        snprintf(buf, sizeof buf, "'%c%c%c%c'",
                 (char)(g.Data1 & 0xFF), (char)((g.Data1 >> 8) & 0xFF),
                 (char)((g.Data1 >> 16) & 0xFF), (char)((g.Data1 >> 24) & 0xFF));
        return buf;
    }
    return "";
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { wprintf(L"usage: typedump <file>\n"); return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // MFCreateSourceReaderFromURL is the path Media Player / Movies & TV go through:
    // MF picks a byte stream handler, that handler builds a media source, and the
    // reader hands back whatever native media types that source reports.  Whatever
    // subtype appears below is the only thing a decoder MFT can be matched against.
    IMFSourceReader* pSR = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(argv[1], nullptr, &pSR);
    wprintf(L"MFCreateSourceReaderFromURL -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr) || pSR == nullptr) { MFShutdown(); CoUninitialize(); return 1; }

    for (DWORD i = 0; i < 8; i++) {
        IMFMediaType* pT = nullptr;
        if (FAILED(pSR->GetNativeMediaType(i, 0, &pT))) break;
        GUID mj{}, sub{};
        pT->GetGUID(MF_MT_MAJOR_TYPE, &mj);
        pT->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &w, &h);
        wprintf(L"stream %u: major=%hs subtype=%08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X %s frame=%ux%u\n",
                i,
                (mj == MFMediaType_Video) ? "video" : (mj == MFMediaType_Audio) ? "audio" : "other",
                sub.Data1, sub.Data2, sub.Data3,
                sub.Data4[0], sub.Data4[1], sub.Data4[2], sub.Data4[3],
                sub.Data4[4], sub.Data4[5], sub.Data4[6], sub.Data4[7],
                FourccName(sub), w, h);
        pT->Release();
    }
    pSR->Release();
    MFShutdown(); CoUninitialize();
    return 0;
}
