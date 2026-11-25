/*
 * avsvk-test: check the Vulkan compute kernels of the AVS reconstruction stage
 * against the software kernels of the same decoder.
 *
 *   bin\avsvk-test.exe                 run the kernel comparison
 *   AVS_VK=1                           required (the backend is opt in)
 *   AVS_VK_ARENA=<MiB>                picture arena, default 96
 */
#include <cstdio>
#include <cstdlib>

#include <windows.h>

extern "C" {
#include "libavcodec/avs_vk.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
}

#include "crashreport.h"

int main(void)
{
    AvsVkStats st;
    struct AvsVkCtx *ctx;
    void *probe;
    int bad;

    setvbuf(stdout, NULL, _IONBF, 0);
    InstallCrashReport();
    _putenv("AVS_VK=1");          /* the backend is opt in, this test opts in */

    printf("start: malloc probe\n");
    probe = av_mallocz(64);
    printf("av_mallocz -> %p\n", probe);
    av_free(probe);

    printf("av_log probe\n");
    av_log(NULL, AV_LOG_WARNING, "logged through libavutil\n");

    printf("create\n");
    ctx = ff_avs_vk_create(NULL);
    printf("create -> %p\n", (void *) ctx);
    if (!ctx) {
        printf("no usable Vulkan device, or the Vulkan loader was not found\n");
        return 2;
    }

    bad = ff_avs_vk_selftest(ctx, NULL);
    ff_avs_vk_get_stats(ctx, &st);
    printf("operations: mc=%llu idct=%llu filter=%llu flush=%llu dispatch=%llu\n",
           (unsigned long long) st.mc_ops, (unsigned long long) st.idct_ops,
           (unsigned long long) st.filter_ops, (unsigned long long) st.flushes,
           (unsigned long long) st.dispatches);
    printf("upload=%llu bytes readback=%llu bytes\n",
           (unsigned long long) st.upload_bytes,
           (unsigned long long) st.readback_bytes);
    printf("self test: %d problem(s)\n", bad);

    ff_avs_vk_destroy(ctx);
    return bad ? 1 : 0;
}
