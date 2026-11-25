// probe_guid.cpp - this Windows SDK declares no MF_SERVICE_* and no
// MF_TOPONODE_TRANSFORM_OBJECT_{DIRECT,LATE} GUIDs at all (a whole-kit grep and a link error both
// proved it), so a session tool has to carry those values itself.  The previous build of this tool
// did carry them; its values are recovered from the linked binary and tested here against a live
// media session: a candidate that returns an interface is the real service GUID, and one that does
// not is only a byte pattern.  Nothing in this file is asserted without the result line below.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GUID ParseGuid(const char* s)
{
    GUID g = GUID_NULL;
    unsigned d4[8];
    sscanf_s(s, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             &g.Data1, &g.Data2, &g.Data3, &d4[0], &d4[1], &d4[2], &d4[3],
             &d4[4], &d4[5], &d4[6], &d4[7]);
    for (int i = 0; i < 8; i++)
        g.Data4[i] = (BYTE)d4[i];
    return g;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: probe-guid <file with one GUID per line>\n");
        return 1;
    }
    FILE* f = fopen(argv[1], "r");
    if (!f)
    {
        printf("cannot open %s\n", argv[1]);
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    IMFMediaSession* pSession = nullptr;
    HRESULT hr = MFCreateMediaSession(nullptr, &pSession);
    printf("MFCreateMediaSession -> 0x%08X\n", (unsigned)hr);
    IMFGetService* pGet = nullptr;
    if (SUCCEEDED(hr))
        hr = pSession->QueryInterface(__uuidof(IMFGetService), (void**)&pGet);
    printf("session QI IMFGetService -> 0x%08X\n", (unsigned)hr);
    if (pGet == nullptr)
        return 1;

    char line[128];
    int nHit = 0;
    while (fgets(line, sizeof(line), f))
    {
        GUID g = ParseGuid(line);
        if (g.Data1 == 0 && g.Data2 == 0 && g.Data3 == 0)
            continue;
        IUnknown* pClock = nullptr;
        HRESULT h1 = pGet->GetService(g, __uuidof(IMFPresentationClock), (void**)&pClock);
        IUnknown* pGen = nullptr;
        HRESULT h2 = pGet->GetService(g, __uuidof(IMFMediaEventGenerator), (void**)&pGen);
        if (SUCCEEDED(h1) || SUCCEEDED(h2))
        {
            printf("%s -> clock=0x%08X ptr=%p   eventgen=0x%08X ptr=%p   *** HIT\n", line,
                   (unsigned)h1, pClock, (unsigned)h2, pGen);
            nHit++;
        }
        if (pClock) pClock->Release();
        if (pGen) pGen->Release();
    }
    printf("candidates tested, hits=%d\n", nHit);
    fclose(f);
    pGet->Release();
    pSession->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
