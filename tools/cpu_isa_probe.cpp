// Which SIMD does this machine actually have?  Answers a question that must not be guessed:
// whether AVX-512 kernels would even run here, and whether the OS enables the ZMM state.
#include <windows.h>
#include <intrin.h>
#include <stdio.h>

int main(void)
{
    int c[4];
    __cpuid(c, 0);
    const unsigned maxLeaf = (unsigned)c[0];
    printf("highest basic CPUID leaf: %u\n", maxLeaf);
    if (maxLeaf < 7) { printf("no leaf 7: no AVX2/AVX-512 is enumerable\n"); return 0; }

    __cpuidex(c, 1, 0);
    const unsigned ecx1 = (unsigned)c[2], ebx1 = (unsigned)c[3];
    __cpuidex(c, 7, 0);
    const unsigned ebx7 = (unsigned)c[1], ecx7 = (unsigned)c[2];

    /* XCR0: bits 1,2 = SSE/AVX (ymm), bits 5,6,7 = AVX-512 opmask / ZMM hi256 / ZMM hi16 */
    unsigned long long xcr0 = 0;
    if ((ecx1 >> 27) & 1)   /* OSXSAVE */
        xcr0 = _xgetbv(0);

    struct { const char* name; unsigned bit; unsigned where; int needed; } f[] = {
        { "SSE4.1",       19, 1, 1 },
        { "AVX",           28, 1, 1 },
        { "AVX2",           5, 7, 0 },
        { "AVX512F",      16, 7, 0 },
        { "AVX512BW",     30, 7, 0 },
        { "AVX512VL",     31, 7, 0 },
        { "AVX512DQ",     17, 7, 0 },
        { "AVX512VBMI",    1, 7, 1 },
        { "AVX512_VNNI",  11, 7, 1 },
        { "AVX512_VBMI2",  8, 7, 1 },
    };
    printf("brand: ");
    if (maxLeaf >= 0x80000004) {
        char brand[49] = { 0 };
        for (unsigned i = 0; i < 3; i++) {
            __cpuid((int*)(brand + i * 16), 0x80000002 + i);
        }
        printf("%s\n", brand);
    } else {
        printf("not enumerable\n");
    }
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        const unsigned v = (f[i].where == 1) ? (f[i].needed ? ecx1 : ebx1)
                                             : (f[i].needed ? ecx7 : ebx7);
        const bool hw = ((v >> f[i].bit) & 1) != 0;
        bool os = hw;
        if (!strcmp(f[i].name, "AVX2") || !strcmp(f[i].name, "AVX"))
            os = hw && (xcr0 & 0x6) == 0x6;
        if (strstr(f[i].name, "AVX512"))
            os = hw && (xcr0 & 0xE0) == 0xE0;
        printf("  %-12s hardware=%s  usable-by-OS=%s\n", f[i].name, hw ? "yes" : "no",
               os ? "yes" : "no");
    }
    printf("XCR0 = 0x%016llX\n", xcr0);
    return 0;
}
