/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dlc_log.h"

#if SCE_DLC_EMU_LOG

#include <_fs.h>
#include <_kernel.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" void sceKernelDebugOutText(int dbg_channel, const char* text);

namespace {
constexpr int kLogMode = 0666;
}

void dlc_logf(const char* fmt, ...) {
    if (!fmt || !*fmt) {
        return;
    }

    char line[768]{};
    va_list ap;
    va_start(ap, fmt);
    const int len = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (len <= 0) {
        return;
    }

    size_t used = 0;
    while (used + 1 < sizeof(line) && line[used]) {
        ++used;
    }
    if (used + 1 < sizeof(line)) {
        line[used++] = '\n';
        line[used] = '\0';
    }

#if SCE_DLC_EMU_LOG_KERNEL_OUT
    sceKernelDebugOutText(0, line);
#endif

    const int fd = sceKernelOpen(SCE_DLC_EMU_LOG_PATH,
                                 SCE_KERNEL_O_WRONLY | SCE_KERNEL_O_CREAT | SCE_KERNEL_O_APPEND,
                                 kLogMode);
    if (fd < 0) {
        return;
    }
    (void)sceKernelWrite(fd, line, used);
    (void)sceKernelClose(fd);
}

#endif
