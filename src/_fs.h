#pragma once

#define SCE_DLC_EMU_JOIN2(a, b) a##b
#define SCE_DLC_EMU_JOIN(a, b) SCE_DLC_EMU_JOIN2(a, b)
#define schedulingWindowSize SCE_DLC_EMU_JOIN(schedulingWindowSize_, __COUNTER__)

#include_next <_fs.h>

#undef schedulingWindowSize
#undef SCE_DLC_EMU_JOIN
#undef SCE_DLC_EMU_JOIN2
