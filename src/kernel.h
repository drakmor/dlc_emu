/*
 * SDK 2.000 includes _fs.h from kernel.h using the SDK directory first. Keep
 * the duplicate-member workaround active while the real kernel.h is parsed.
 */

#pragma once

#define SCE_DLC_EMU_JOIN2(a, b) a##b
#define SCE_DLC_EMU_JOIN(a, b) SCE_DLC_EMU_JOIN2(a, b)
#define schedulingWindowSize SCE_DLC_EMU_JOIN(schedulingWindowSize_, __COUNTER__)

#include_next <kernel.h>

#undef schedulingWindowSize
#undef SCE_DLC_EMU_JOIN
#undef SCE_DLC_EMU_JOIN2
