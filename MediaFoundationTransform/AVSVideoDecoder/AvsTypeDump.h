// AvsTypeDump.h - measurement only: print every attribute a media type carries.
//
// Why this exists: two session cells behave differently (one ends, one does not) and the only
// remaining difference between them is the media type they describe.  Guessing which attribute
// matters is worthless; the set of attributes has to be on the table.  This dumps GUID + value
// pairs for any IMFAttributes, under AVS_MFT_TRACE, and reads nothing back.
//
// Names are resolved offline by scripts/type-diff.py from the SDK headers, so nothing here
// depends on a hard-coded attribute list and no attribute name can be invented.
//
// Included by the MFT DLL and by the session test tool; keep it header-only and side-effect
// free apart from writing to stderr.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <mfidl.h>

static inline bool AvsTypeDumpOn()
{
    static int cached = -1;
    if (cached < 0)
        cached = (GetEnvironmentVariableW(L"AVS_MFT_TRACE", NULL, 0) > 0) ? 1 : 0;
    return cached == 1;
}

static inline void AvsGuidString(const GUID& g, char* out /* 39 bytes */)
{
    sprintf_s(out, 40, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            (unsigned)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
            (unsigned)g.Data4[0], (unsigned)g.Data4[1], (unsigned)g.Data4[2],
            (unsigned)g.Data4[3], (unsigned)g.Data4[4], (unsigned)g.Data4[5],
            (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
}

// One attribute.  Values are printed in the shape they are stored in: MF keeps sizes and frame
// rates as VT_VECTOR|VT_UI4 pairs, fourCC-style subtypes as plain GUIDs, and blobs as hex.
static inline void AvsDumpOneAttr(const char* tag, const GUID& g, const PROPVARIANT& v)
{
    char gs[40];
    AvsGuidString(g, gs);

    switch (v.vt)
    {
    case VT_EMPTY:
        fprintf(stderr, "[attrdump] %s %s vt=EMPTY\n", tag, gs);
        return;
    case VT_BOOL:
        fprintf(stderr, "[attrdump] %s %s vt=BOOL value=%d\n", tag, gs,
                v.boolVal == VARIANT_TRUE ? 1 : 0);
        return;
    case VT_I2: case VT_I4:
        fprintf(stderr, "[attrdump] %s %s vt=I4 value=%d\n", tag, gs, (int)v.lVal);
        return;
    case VT_UI1:
        fprintf(stderr, "[attrdump] %s %s vt=UI1 value=%u\n", tag, gs, (unsigned)v.uiVal);
        return;
    case VT_UI2:
        fprintf(stderr, "[attrdump] %s %s vt=UI2 value=%u\n", tag, gs, (unsigned)v.uiVal);
        return;
    case VT_UI4:
        fprintf(stderr, "[attrdump] %s %s vt=UI4 value=%u 0x%X\n", tag, gs,
                (unsigned)v.ulVal, (unsigned)v.ulVal);
        return;
    case VT_I8:
        fprintf(stderr, "[attrdump] %s %s vt=I8 value=%lld\n", tag, gs,
                (long long)v.hVal.QuadPart);
        return;
    case VT_UI8:
        fprintf(stderr, "[attrdump] %s %s vt=UI8 value=%llu 0x%llX\n", tag, gs,
                (unsigned long long)v.uhVal.QuadPart,
                (unsigned long long)v.uhVal.QuadPart);
        return;
    case VT_R4:
        fprintf(stderr, "[attrdump] %s %s vt=R4 value=%g\n", tag, gs, (double)v.fltVal);
        return;
    case VT_R8:
        fprintf(stderr, "[attrdump] %s %s vt=R8 value=%g\n", tag, gs, v.dblVal);
        return;
    case VT_LPWSTR:
        fprintf(stderr, "[attrdump] %s %s vt=LPWSTR value=%S\n", tag, gs,
                v.pwszVal ? v.pwszVal : L"(null)");
        return;
    case VT_CLSID:
        if (v.puuid)
        {
            char g2[40];
            AvsGuidString(*v.puuid, g2);
            fprintf(stderr, "[attrdump] %s %s vt=CLSID value=%s\n", tag, gs, g2);
        }
        return;
    case VT_BLOB:
        fprintf(stderr, "[attrdump] %s %s vt=BLOB bytes=%u", tag, gs,
                (unsigned)v.blob.cbSize);
        for (UINT32 i = 0; i < v.blob.cbSize && i < 32; i++)
            fprintf(stderr, " %02X", v.blob.pBlobData[i]);
        fprintf(stderr, "\n");
        return;
    case VT_VECTOR | VT_UI1:
        fprintf(stderr, "[attrdump] %s %s vt=VECTOR|UI1 count=%u", tag, gs,
                (unsigned)v.caub.cElems);
        for (UINT32 i = 0; i < v.caub.cElems && i < 32; i++)
            fprintf(stderr, " %02X", v.caub.pElems[i]);
        fprintf(stderr, "\n");
        return;
    case VT_VECTOR | VT_UI4:
        fprintf(stderr, "[attrdump] %s %s vt=VECTOR|UI4 count=%u", tag, gs,
                (unsigned)v.caul.cElems);
        for (UINT32 i = 0; i < v.caul.cElems && i < 8; i++)
            fprintf(stderr, " %u 0x%X", (unsigned)v.caul.pElems[i],
                    (unsigned)v.caul.pElems[i]);
        fprintf(stderr, "\n");
        return;
    case VT_VECTOR | VT_CLSID:
        fprintf(stderr, "[attrdump] %s %s vt=VECTOR|CLSID count=%u", tag, gs,
                (unsigned)v.cauuid.cElems);
        for (UINT32 i = 0; i < v.cauuid.cElems && i < 4; i++)
        {
            char g2[40];
            AvsGuidString(v.cauuid.pElems[i], g2);
            fprintf(stderr, " %s", g2);
        }
        fprintf(stderr, "\n");
        return;
    case VT_UNKNOWN:
    case VT_UNKNOWN | VT_VECTOR:
    case VT_DISPATCH:
        fprintf(stderr, "[attrdump] %s %s vt=IFACE ptr=%s\n", tag, gs,
                (v.punkVal != NULL) ? "yes" : "no");
        return;
    default:
        fprintf(stderr, "[attrdump] %s %s vt=0x%X (not printed)\n", tag, gs, (unsigned)v.vt);
        return;
    }
}

// Every attribute of a media type (or of any other attribute store), in store order.
static inline void AvsDumpType(const char* tag, IUnknown* pUnk)
{
    if (!AvsTypeDumpOn() || pUnk == NULL)
        return;

    IMFAttributes* pAttrs = NULL;
    if (FAILED(pUnk->QueryInterface(IID_IMFAttributes, (void**)&pAttrs)) || pAttrs == NULL)
        return;

    UINT32 c = 0;
    if (FAILED(pAttrs->GetCount(&c)))
        c = 0;
    fprintf(stderr, "[attrdump] %s count=%u\n", tag, c);
    for (UINT32 i = 0; i < c; i++)
    {
        GUID g = GUID_NULL;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(pAttrs->GetItemByIndex(i, &g, &v)))
        {
            AvsDumpOneAttr(tag, g, v);
            // MF allocates the owned PROPVARIANT payloads with CoTaskMemAlloc; release them
            // by hand so this stays usable inside the MFT without pulling in propsys.
            switch (v.vt)
            {
            case VT_LPWSTR: case VT_CLSID:
            case VT_VECTOR | VT_UI1: case VT_VECTOR | VT_UI4: case VT_VECTOR | VT_CLSID:
                CoTaskMemFree(v.caub.pElems);   // all of these are one pointer at offset 0
                break;
            case VT_BLOB:
                CoTaskMemFree(v.blob.pBlobData);
                break;
            default:
                break;
            }
        }
    }
    pAttrs->Release();
}
