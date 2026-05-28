/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared AppContent/NpEntitlementAccess replacement-module emulation.
 */

#pragma once

#include "dlc_config.h"

#include <app_content.h>
#include <np/np_entitlement_access.h>
#include <np/np_common.h>

#include <cstddef>
#include <cstdint>

#ifndef SCE_OK
#define SCE_OK 0
#endif

#ifndef SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG
#define SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG 0
#endif

#ifndef SCE_APP_CONTENT_APPPARAM_SKU_FLAG_FULL
#define SCE_APP_CONTENT_APPPARAM_SKU_FLAG_FULL 3
#endif

#ifndef SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED
#define SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED 4
#endif

#ifndef SCE_APP_CONTENT_PFT_FLAG_OFF
using SceAppContentPftFlag = uint32_t;
#define SCE_APP_CONTENT_PFT_FLAG_OFF 0
#define SCE_APP_CONTENT_PFT_FLAG_ON 1
#endif

#ifndef SCE_APP_CONTENT_ENTITLEMENT_KEY_SIZE
#define SCE_APP_CONTENT_ENTITLEMENT_KEY_SIZE 16
struct SceAppContentEntitlementKey {
    char data[SCE_APP_CONTENT_ENTITLEMENT_KEY_SIZE];
};
#endif

struct SceAppContentAddcontInfo {
    SceNpUnifiedEntitlementLabel entitlementLabel;
    SceAppContentAddcontDownloadStatus status;
};

#ifndef SCE_NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF
using SceNpEntitlementAccessGameTrialsFlag = uint32_t;
#define SCE_NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF 0
#define SCE_NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_ON 1
#endif

extern "C" {
void dlcEmu_prewarmAppRpc(void);
void dlcEmu_prewarmNpRpc(void);

int32_t dlcEmu_sceAppContentInitialize(const SceAppContentInitParam* initParam,
                                        SceAppContentBootParam* bootParam);
int32_t dlcEmu_sceAppContentAppParamGetInt(SceAppContentAppParamId paramId,
                                                 int32_t* value);
int32_t dlcEmu_sceAppContentGetAddcontInfoList(SceNpServiceLabel serviceLabel,
                                                     SceAppContentAddcontInfo* list,
                                                     uint32_t listNum,
                                                     uint32_t* hitNum);
int32_t dlcEmu_sceAppContentGetAddcontInfo(SceNpServiceLabel serviceLabel,
                                                 const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                 SceAppContentAddcontInfo* info);
int32_t dlcEmu_sceAppContentGetAddcontInfoByEntitlementId(
    const char* entitlementId,
    SceAppContentAddcontInfo* info);
int32_t dlcEmu_sceAppContentGetAddcontInfoListByIroTag(
    uint32_t iroTag,
    SceAppContentAddcontInfo* list,
    uint32_t listNum,
    uint32_t* hitNum);
int32_t dlcEmu_sceAppContentGetEntitlementKey(SceNpServiceLabel serviceLabel,
                                                    const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                    SceAppContentEntitlementKey* key);
int32_t dlcEmu_sceAppContentAddcontMount(SceNpServiceLabel serviceLabel,
                                               const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                               SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentAddcontMountByEntitlemetId(const char* entitlementId,
                                                             SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentAddcontUnmount(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentAddcontDelete(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel);
int32_t dlcEmu_sceAppContentAddcontEnqueueDownload(SceNpServiceLabel serviceLabel,
                                                         const SceNpUnifiedEntitlementLabel* entitlementLabel);
int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadSp(SceNpServiceLabel serviceLabel,
                                                           const SceNpUnifiedEntitlementLabel* entitlementLabel);
int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadByEntitlemetId(const char* entitlementId);
int32_t dlcEmu_sceAppContentAddcontShrink(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel);
int32_t dlcEmu_sceAppContentRaw_xZo2_418Wdo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel);
int32_t dlcEmu_sceAppContentRaw_UO_gD_XFyGE(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentRaw_MFUAprB41fA(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentRaw_y8meQn_Qy5c(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    uint32_t* playableStatus);
int32_t dlcEmu_sceAppContentRaw_1saJukIkcKw(uint32_t* gameTrialsFlag);
int32_t dlcEmu_sceAppContentRaw_SWVxsi_ZBlw(const void* input, void* output);
int32_t dlcEmu_sceAppContentTemporaryDataUnmount(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentTemporaryDataFormat(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentTemporaryDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb);
int32_t dlcEmu_sceAppContentDownloadDataFormat(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentDownloadDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb);
int32_t dlcEmu_sceAppContentDownloadDataGetBlockSize(
    const SceAppContentMountPoint* mountPoint,
    size_t* blockSize);
int32_t dlcEmu_sceAppContentDownload0Shrink(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentDownload0Expand(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentDownload1Shrink(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentDownload1Expand(const SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentGetAddcontDownloadProgress(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceAppContentAddcontDownloadProgress* progress);
int32_t dlcEmu_sceAppContentGetPftFlag(SceAppContentPftFlag* pftFlag);
int32_t dlcEmu_sceAppContentTemporaryDataMount(SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentTemporaryDataMount2(SceAppContentTemporaryDataOption option,
                                                      SceAppContentMountPoint* mountPoint);
int32_t dlcEmu_sceAppContentGetRegion(char* region);
int32_t dlcEmu_sceAppContentRequestPatchInstall(const char* path);
int32_t dlcEmu_sceAppContentAppParamGetString(SceAppContentAppParamId paramId,
                                                    char* value,
                                                    size_t valueSize);
int32_t dlcEmu_sceAppContentGetDownloadedStoreCountry(char* country);

int32_t dlcEmu_sceNpEntitlementAccessInitialize(
    const SceNpEntitlementAccessInitParam* initParam,
    SceNpEntitlementAccessBootParam* bootParam);
int32_t dlcEmu_sceNpEntitlementAccessGetSkuFlag(SceNpEntitlementAccessSkuFlag* skuFlag);
int32_t dlcEmu_sceNpEntitlementAccessGetGameTrialsFlag(
    SceNpEntitlementAccessGameTrialsFlag* gameTrialsFlag);
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoList(
    SceNpServiceLabel serviceLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum);
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info);
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoIndividual(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info);
int32_t dlcEmu_sceNpEntitlementAccessRaw_l0MTQHIcH3M(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    void* list,
    uint32_t listNum,
    uint32_t* hitNum);
int32_t dlcEmu_sceNpEntitlementAccessRaw_eDXKe9FndlE(
    SceNpEntitlementAccessGameTrialsFlag* pftFlag);
int32_t dlcEmu_sceNpEntitlementAccessGetEntitlementKey(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessEntitlementKey* key);
int32_t dlcEmu_sceNpEntitlementAccessGenerateTransactionId(
    SceNpEntitlementAccessTransactionId* transactionId);
int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeServiceEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollConsumeEntitlement(
    int64_t requestId,
    int32_t* pResult,
    int32_t* useLimit);
int32_t dlcEmu_sceNpEntitlementAccessRequestConsumableEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollConsumableEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info);
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info);
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset);
int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* info);
int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId);
int32_t dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset);
int32_t dlcEmu_sceNpEntitlementAccessDeleteRequest(int64_t requestId);
int32_t dlcEmu_sceNpEntitlementAccessAbortRequest(int64_t requestId);
}
