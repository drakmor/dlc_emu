/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tiny optional logger for the standalone DLC replacement modules.
 */

#pragma once

#include "dlc_config.h"

#if SCE_DLC_EMU_LOG
void dlc_logf(const char* fmt, ...);
#else
inline void dlc_logf(const char* fmt, ...) {
    (void)fmt;
}
#endif
