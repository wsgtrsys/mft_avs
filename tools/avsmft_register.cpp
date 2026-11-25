//--------------------------------------------------------------------------------------------------
// avsmft-register.cpp - register / unregister the AVS decoder MFT.
//
//   avsmft-register <path to AVSVideoDecoder.dll>            register
//   avsmft-register <path to AVSVideoDecoder.dll> -u         unregister
//
// Same as "regsvr32 AVSVideoDecoder.dll", but it reports the HRESULT and can be run
// from a build script.  Writing the registry keys needs an elevated command prompt
// (HKLM\SOFTWARE\Classes\CLSID plus the Media Foundation MFT registration).
//
// After registering it READS BACK the byte stream handler keys and prints what Media
// Foundation will actually see, extension by extension.  DllRegisterServer returning
// S_OK does not mean every extension was taken: the system owns .ts/.m2ts/.mkv/.mp4 and
// those keys refuse an Administrator write, and an older build stopped its loop at the
// first refusal, which is how "we registered .ts" was believed here without ever having
// been true.  The read-back is what settles it.
//--------------------------------------------------------------------------------------------------
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <stdio.h>
#include <string>

static std::wstring utf8toW(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Must match CLSID_AVSByteStreamHandler in the DLL (classGuids.h).  The self-check below
// fails loudly if the two ever drift apart.
static const char kOurHandlerClsid[] = "{9E5D6A1B-2C4F-4E7D-8F2B-7A9C0D5E3B14}";

static void report_handler_registrations(void)
{
    static const char* exts[] = { ".avs", ".cavs", ".avs2", ".avs3",
                                  ".ts", ".m2ts", ".mkv", ".mp4" };
    printf("\nbyte stream handler read-back (this is what MF will consult):\n");

    // the registry path separator, spelled so that no source-level escaping is involved
    std::string B(1, (char)92);
    std::string base = "SOFTWARE" + B + "Microsoft" + B + "Windows Media Foundation" + B +
                       "ByteStreamHandlers";

    // self-check: our handler CLSID must point at a server we can find
    std::string clsidKey = "SOFTWARE" + B + "Classes" + B + "CLSID" + B + kOurHandlerClsid +
                           B + "InprocServer32";
    HKEY hc = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hc) == ERROR_SUCCESS)
    {
        char dll[MAX_PATH] = { 0 }; DWORD cb = sizeof dll, ty = 0;
        if (RegQueryValueExA(hc, NULL, NULL, &ty, (BYTE*)dll, &cb) == ERROR_SUCCESS)
            printf("  self-check: %s -> %s\n", kOurHandlerClsid, dll);
        else
            printf("  self-check: %s has no InprocServer32 value\n", kOurHandlerClsid);
        RegCloseKey(hc);
    }
    else
        printf("  self-check: %s is NOT registered (CLSID drift?)\n", kOurHandlerClsid);

    for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
    {
        std::string sub = base + B + exts[i];
        HKEY hk = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hk) != ERROR_SUCCESS)
        {
            printf("  %-5s : no key at all\n", exts[i]);
            continue;
        }
        BOOL ours = (RegQueryValueExA(hk, kOurHandlerClsid, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        int others = 0;
        for (int v = 0; ; v++)
        {
            char name[256]; DWORD nl = sizeof name;
            if (RegEnumValueA(hk, v, name, &nl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (_stricmp(name, kOurHandlerClsid) != 0) others++;
        }
        RegCloseKey(hk);
        printf("  %-5s : ours=%-3s  other handlers=%d%s\n", exts[i], ours ? "yes" : "no", others,
               (!ours && others > 0) ? "   <- owned by the system; an Administrator write is refused" : "");
    }
    printf("  (decoding does not depend on any of the above: once our source reads the file\n"
           "   any container whose bitstream is AVS is decoded - see\n"
           "   scripts/container-decode-test.py. These keys only decide whether MF asks us.)\n");
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: avsmft-register <AVSVideoDecoder.dll> [-u]\n");
        return 1;
    }
    bool unreg = (argc > 2 && std::string(argv[2]) == "-u");

    HMODULE hMod = LoadLibraryExW(utf8toW(argv[1]).c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (hMod == NULL)
    {
        printf("LoadLibrary failed: %u\n", GetLastError());
        return 1;
    }

    typedef HRESULT(STDAPICALLTYPE* Fn)();
    Fn fn = (Fn)GetProcAddress(hMod, unreg ? "DllUnregisterServer" : "DllRegisterServer");
    if (fn == NULL)
    {
        printf("%s not exported\n", unreg ? "DllUnregisterServer" : "DllRegisterServer");
        return 1;
    }

    HRESULT hr = fn();
    printf("%s -> 0x%08X %s\n", unreg ? "DllUnregisterServer" : "DllRegisterServer", hr,
           FAILED(hr) ? "(run elevated to write HKLM)" : "OK");
    if (!unreg)
        report_handler_registrations();
    return SUCCEEDED(hr) ? 0 : 1;
}
