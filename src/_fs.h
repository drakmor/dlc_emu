/*
 * SDK 2.000 ships target/include/_fs.h with two C struct members named
 * schedulingWindowSize in SceKernelAioSchedulingParam. C++ rejects that header
 * before this project reaches its own code. The project does not access that
 * struct by member name, so give each occurrence a unique preprocessing name and
 * then include the real SDK header. This preserves layout and avoids modifying
 * the local SDK installation.
 */

#pragma once

#define SCE_DLC_EMU_JOIN2(a, b) a##b
#define SCE_DLC_EMU_JOIN(a, b) SCE_DLC_EMU_JOIN2(a, b)
#define schedulingWindowSize SCE_DLC_EMU_JOIN(schedulingWindowSize_, __COUNTER__)

#include_next <_fs.h>

#undef schedulingWindowSize
#undef SCE_DLC_EMU_JOIN
#undef SCE_DLC_EMU_JOIN2
