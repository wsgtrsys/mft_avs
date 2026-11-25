// probe_evq.cpp - can the session's event generator and clock be reached without the service
// GUIDs this SDK does not declare?  QI first; if that fails, test the GUID constants recovered
// from the previous build of the session tool, inside a session that has a real topology (a
// clock only exists once the graph is built, which is why an earlier version of this probe,
// run on an empty session, proved nothing).
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
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
static void Str(const GUID& g, char* o)
{
    sprintf_s(o, 39, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
              g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: probe-evq <media file> <candidate guid file>\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    WCHAR wszUrl[4096];
    swprintf_s(wszUrl, L"file://%s", argv[1]);
    IMFSourceResolver* pRes = nullptr;
    IMFMediaSource* pSrc = nullptr;
    IMFPresentationDescriptor* pPD = nullptr;
    IMFMediaSession* pSession = nullptr;
    IMFTopology* pTopo = nullptr;
    IMFActivate* pAud = nullptr;
    IMFActivate* pVid = nullptr;
    IUnknown* pRenderer = nullptr;

    MFCreateSourceResolver(&pRes);
    MF_OBJECT_TYPE t = MF_OBJECT_INVALID;
    IUnknown* pUnk = nullptr;
    printf("CreateObjectFromURL -> 0x%08X\n",
           (unsigned)pRes->CreateObjectFromURL(wszUrl, MF_RESOLUTION_MEDIASOURCE, nullptr, &t, &pUnk));
    if (pUnk == nullptr)
        return 1;
    pUnk->QueryInterface(__uuidof(IMFMediaSource), (void**)&pSrc);
    pUnk->Release();
    if (pSrc == nullptr)
        return 1;
    pSrc->CreatePresentationDescriptor(&pPD);

    // an output node is required for the graph to hold a clock
    MFCreateAudioRendererActivate(&pAud);
    if (pAud)
    {
        HRESULT hr = pAud->ActivateObject(IID_IUnknown, (void**)&pRenderer);
        printf("audio renderer ActivateObject -> 0x%08X\n", (unsigned)hr);
    }
    MFCreateTopology(&pTopo);
    MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, (IMFTopologyNode**)&pUnk);
    IMFTopologyNode* pSrcNode = (IMFTopologyNode*)pUnk;
    if (pSrcNode)
    {
        IMFStreamDescriptor* pSD = nullptr;
        BOOL bSel = FALSE;
        pPD->GetStreamDescriptorByIndex(0, &bSel, &pSD);
        pSrcNode->SetUnknown(MF_TOPONODE_SOURCE, pSrc);
        pSrcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
        if (pSD)
            pSrcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);
        printf("source node added -> 0x%08X\n", (unsigned)pTopo->AddNode(pSrcNode));
        if (pRenderer)
        {
            IMFTopologyNode* pOut = nullptr;
            MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOut);
            if (pOut)
            {
                pOut->SetUnknown(MF_DEVICETYPE_GUID, pRenderer);
                printf("output node added -> 0x%08X\n", (unsigned)pTopo->AddNode(pOut));
                IMFOutputProps* pProps = nullptr;
                pSrcNode->AddOutputPath(0, 0, pOut, 0, (void**)&pProps);
                if (pProps) pProps->Release();
                pOut->Release();
            }
        }
        pSrcNode->Release();
    }

    MFCreateMediaSession(nullptr, &pSession);
    IMFMediaEventGenerator* pGenQI = nullptr;
    printf("session QI IMFMediaEventGenerator -> 0x%08X\n",
           (unsigned)pSession->QueryInterface(__uuidof(IMFMediaEventGenerator), (void**)&pGenQI));
    if (pGenQI) pGenQI->Release();

    IMFGetService* pGet = nullptr;
    pSession->QueryInterface(__uuidof(IMFGetService), (void**)&pGet);
    if (pGet == nullptr)
        return 1;

    printf("SetTopology -> 0x%08X\n", (unsigned)pSession->SetTopology(0, pTopo));

    FILE* f = fopen(argv[2], "r");
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
            char s[39];
            Str(g, s);
            printf("%s clock=0x%08X ptr=%p eventgen=0x%08X ptr=%p  *** HIT\n", s,
                   (unsigned)h1, pClock, (unsigned)h2, pGen);
            nHit++;
        }
        if (pClock) pClock->Release();
        if (pGen) pGen->Release();
    }
    printf("hits=%d\n", nHit);
    fclose(f);
    pGet->Release();
    pSession->Release();
    pTopo->Release();
    if (pRenderer) pRenderer->Release();
    if (pAud) { pAud->ShutdownObject(); pAud->Release(); }
    if (pVid) pVid->Release();
    pPD->Release();
    pSrc->Release();
    pRes->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
