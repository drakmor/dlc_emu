/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libSceGameUpdate replacement exports used by DLC ownership checks.
 */

#include "dlc_content.h"

#if defined(__ORBIS__) || defined(__PROSPERO__)
#define DLC_EXPORT __attribute__((visibility("default")))
#else
#define DLC_EXPORT
#endif

extern "C" {

DLC_EXPORT int32_t sceGameUpdateInitialize(void) {
    return dlcEmu_sceGameUpdateInitialize();
}

DLC_EXPORT int32_t sceGameUpdateTerminate(void) {
    return dlcEmu_sceGameUpdateTerminate();
}

DLC_EXPORT int32_t sceGameUpdateCreateRequest(void) {
    return dlcEmu_sceGameUpdateCreateRequest();
}

DLC_EXPORT int32_t sceGameUpdateCheck(int32_t requestId,
                                      const SceGameUpdateCheckParam* param,
                                      SceGameUpdateCheckResult* result) {
    return dlcEmu_sceGameUpdateCheck(requestId, param, result);
}

DLC_EXPORT int32_t sceGameUpdateCheckTitle(int32_t requestId,
                                           uint32_t serviceLabel,
                                           SceGameUpdateCheckResult* result) {
    return dlcEmu_sceGameUpdateCheckTitle(requestId, serviceLabel, result);
}

DLC_EXPORT int32_t sceGameUpdateAbortRequest(int32_t requestId) {
    return dlcEmu_sceGameUpdateAbortRequest(requestId);
}

DLC_EXPORT int32_t sceGameUpdateDeleteRequest(int32_t requestId) {
    return dlcEmu_sceGameUpdateDeleteRequest(requestId);
}

DLC_EXPORT int32_t sceGameUpdateGetAddcontLatestVersion(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceGameUpdateAddcontVersionInfo* info) {
    return dlcEmu_sceGameUpdateGetAddcontLatestVersion(serviceLabel, entitlementLabel, info);
}

} // extern "C"
