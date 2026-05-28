/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Export layer for the standalone libSceAppContent replacement PRX.
 */

#include "dlc_content.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#define DLC_EXPORT

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
#endif

namespace {
std::atomic<int> g_initialized{0};

int32_t check_initialized() {
    return g_initialized.load(std::memory_order_acquire) != 0
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_INITIALIZED;
}
} // namespace

#define DLC_APP_CALL(expr) \
    do { \
        const int32_t initRc = check_initialized(); \
        if (initRc != SCE_OK) return initRc; \
        return (expr); \
    } while (0)

extern "C" {

DLC_EXPORT void sceAppContentRaw_AS45QoYHjc4(void) __asm__("AS45QoYHjc4");
DLC_EXPORT int32_t sceAppContentRaw_xZo2_418Wdo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) __asm__("xZo2-418Wdo");
DLC_EXPORT int32_t sceAppContentRaw_UO_gD_XFyGE(
    const SceAppContentMountPoint* mountPoint) __asm__("UO-gD-XFyGE");
DLC_EXPORT int32_t sceAppContentRaw_MFUAprB41fA(
    const SceAppContentMountPoint* mountPoint) __asm__("MFUAprB41fA");
DLC_EXPORT int32_t sceAppContentRaw_y8meQn_Qy5c(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    uint32_t* playableStatus) __asm__("y8meQn-Qy5c");
DLC_EXPORT int32_t sceAppContentRaw_1saJukIkcKw(uint32_t* gameTrialsFlag) __asm__("1saJukIkcKw");
DLC_EXPORT int32_t sceAppContentRaw_1saJukIkcKw_GameTrials(uint32_t* gameTrialsFlag);
DLC_EXPORT int32_t sceAppContentRaw_SWVxsi_ZBlw(const void* input, void* output) __asm__("SWVxsi-ZBlw");

static void __attribute__((constructor)) sceAppContentDlcInit(void) {
    dlcEmu_prewarmAppRpc();
}

int module_start(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    return 0;
}

int module_stop(size_t args, const void* argp) {
    (void)args;
    (void)argp;
    g_initialized.store(0, std::memory_order_release);
    return 0;
}

DLC_EXPORT void sceAppContentRaw_AS45QoYHjc4(void) {}

DLC_EXPORT int32_t sceAppContentInitialize(const SceAppContentInitParam* initParam,
                                           SceAppContentBootParam* bootParam) {
    if (g_initialized.load(std::memory_order_acquire) != 0) {
        return SCE_APP_CONTENT_ERROR_BUSY;
    }
    const int32_t rc = dlcEmu_sceAppContentInitialize(initParam, bootParam);
    if (rc == SCE_OK) {
        g_initialized.store(1, std::memory_order_release);
    }
    return rc;
}

DLC_EXPORT int32_t sceAppContentAppParamGetInt(SceAppContentAppParamId paramId, int32_t* value) {
    DLC_APP_CALL(dlcEmu_sceAppContentAppParamGetInt(paramId, value));
}

DLC_EXPORT int32_t sceAppContentGetAddcontInfoList(SceNpServiceLabel serviceLabel,
                                                   SceAppContentAddcontInfo* list,
                                                   uint32_t listNum,
                                                   uint32_t* hitNum) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetAddcontInfoList(serviceLabel, list, listNum, hitNum));
}

DLC_EXPORT int32_t sceAppContentGetAddcontInfo(SceNpServiceLabel serviceLabel,
                                               const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                               SceAppContentAddcontInfo* info) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetAddcontInfo(serviceLabel, entitlementLabel, info));
}

DLC_EXPORT int32_t sceAppContentAddcontMount(SceNpServiceLabel serviceLabel,
                                             const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                             SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontMount(serviceLabel, entitlementLabel, mountPoint));
}

DLC_EXPORT int32_t sceAppContentAddcontUnmount(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontUnmount(mountPoint));
}

DLC_EXPORT int32_t sceAppContentAddcontEnqueueDownload(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontEnqueueDownload(serviceLabel, entitlementLabel));
}

DLC_EXPORT int32_t sceAppContentAddcontEnqueueDownloadSp(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontEnqueueDownloadSp(serviceLabel, entitlementLabel));
}

DLC_EXPORT int32_t sceAppContentGetEntitlementKey(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceAppContentEntitlementKey* key) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetEntitlementKey(serviceLabel, entitlementLabel, key));
}

DLC_EXPORT int32_t sceAppContentTemporaryDataMount(SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentTemporaryDataMount(mountPoint));
}

DLC_EXPORT int32_t sceAppContentTemporaryDataMount2(SceAppContentTemporaryDataOption option,
                                                    SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentTemporaryDataMount2(option, mountPoint));
}

DLC_EXPORT int32_t sceAppContentTemporaryDataUnmount(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentTemporaryDataUnmount(mountPoint));
}

DLC_EXPORT int32_t sceAppContentTemporaryDataFormat(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentTemporaryDataFormat(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownloadDataFormat(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownloadDataFormat(mountPoint));
}

DLC_EXPORT int32_t sceAppContentTemporaryDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb) {
    DLC_APP_CALL(dlcEmu_sceAppContentTemporaryDataGetAvailableSpaceKb(mountPoint, availableSpaceKb));
}

DLC_EXPORT int32_t sceAppContentDownloadDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownloadDataGetAvailableSpaceKb(mountPoint, availableSpaceKb));
}

DLC_EXPORT int32_t sceAppContentAddcontDelete(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontDelete(serviceLabel, entitlementLabel));
}

DLC_EXPORT int32_t sceAppContentGetRegion(char* region) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetRegion(region));
}

DLC_EXPORT int32_t sceAppContentRequestPatchInstall(const char* path) {
    DLC_APP_CALL(dlcEmu_sceAppContentRequestPatchInstall(path));
}

DLC_EXPORT int32_t sceAppContentAppParamGetString(SceAppContentAppParamId paramId,
                                                  char* value,
                                                  size_t valueSize) {
    DLC_APP_CALL(dlcEmu_sceAppContentAppParamGetString(paramId, value, valueSize));
}

DLC_EXPORT int32_t sceAppContentGetDownloadedStoreCountry(char* country) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetDownloadedStoreCountry(country));
}

DLC_EXPORT int32_t sceAppContentGetAddcontInfoByEntitlementId(const char* entitlementId,
                                                              SceAppContentAddcontInfo* info) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetAddcontInfoByEntitlementId(entitlementId, info));
}

DLC_EXPORT int32_t sceAppContentGetAddcontInfoListByIroTag(uint32_t iroTag,
                                                           SceAppContentAddcontInfo* list,
                                                           uint32_t listNum,
                                                           uint32_t* hitNum) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetAddcontInfoListByIroTag(iroTag, list, listNum, hitNum));
}

DLC_EXPORT int32_t sceAppContentAddcontEnqueueDownloadByEntitlemetId(const char* entitlementId) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontEnqueueDownloadByEntitlemetId(entitlementId));
}

DLC_EXPORT int32_t sceAppContentAddcontMountByEntitlemetId(const char* entitlementId,
                                                           SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontMountByEntitlemetId(entitlementId, mountPoint));
}

DLC_EXPORT int32_t sceAppContentAddcontShrink(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    DLC_APP_CALL(dlcEmu_sceAppContentAddcontShrink(serviceLabel, entitlementLabel));
}

DLC_EXPORT int32_t sceAppContentRaw_xZo2_418Wdo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_xZo2_418Wdo(serviceLabel, entitlementLabel));
}

DLC_EXPORT int32_t sceAppContentGetAddcontDownloadProgress(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceAppContentAddcontDownloadProgress* progress) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetAddcontDownloadProgress(serviceLabel, entitlementLabel, progress));
}

DLC_EXPORT int32_t sceAppContentRaw_UO_gD_XFyGE(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_UO_gD_XFyGE(mountPoint));
}

DLC_EXPORT int32_t sceAppContentRaw_MFUAprB41fA(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_MFUAprB41fA(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownload0Shrink(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownload0Shrink(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownload0Expand(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownload0Expand(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownload1Shrink(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownload1Shrink(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownload1Expand(const SceAppContentMountPoint* mountPoint) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownload1Expand(mountPoint));
}

DLC_EXPORT int32_t sceAppContentDownloadDataGetBlockSize(const SceAppContentMountPoint* mountPoint,
                                                         size_t* blockSize) {
    DLC_APP_CALL(dlcEmu_sceAppContentDownloadDataGetBlockSize(mountPoint, blockSize));
}

DLC_EXPORT int32_t sceAppContentGetPftFlag(SceAppContentPftFlag* pftFlag) {
    DLC_APP_CALL(dlcEmu_sceAppContentGetPftFlag(pftFlag));
}

DLC_EXPORT int32_t sceAppContentRaw_y8meQn_Qy5c(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    uint32_t* playableStatus) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_y8meQn_Qy5c(serviceLabel, entitlementLabel, playableStatus));
}

DLC_EXPORT int32_t sceAppContentRaw_1saJukIkcKw(uint32_t* gameTrialsFlag) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_1saJukIkcKw(gameTrialsFlag));
}

DLC_EXPORT int32_t sceAppContentRaw_1saJukIkcKw_GameTrials(uint32_t* gameTrialsFlag) {
    return sceAppContentRaw_1saJukIkcKw(gameTrialsFlag);
}

DLC_EXPORT int32_t sceAppContentRaw_SWVxsi_ZBlw(const void* input, void* output) {
    DLC_APP_CALL(dlcEmu_sceAppContentRaw_SWVxsi_ZBlw(input, output));
}

} // extern "C"
