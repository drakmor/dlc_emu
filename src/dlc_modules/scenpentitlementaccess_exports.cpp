/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Export layer for the standalone libSceNpEntitlementAccess replacement PRX.
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
               : SCE_NP_ENTITLEMENT_ACCESS_ERROR_NOT_INITIALIZED;
}
} // namespace

#define DLC_NP_CALL(expr) \
    do { \
        const int32_t initRc = check_initialized(); \
        if (initRc != SCE_OK) return initRc; \
        return (expr); \
    } while (0)

extern "C" {

DLC_EXPORT int32_t sceNpEntitlementAccessGetGameTrialsFlag_GameTrials(
    SceNpEntitlementAccessGameTrialsFlag* gameTrialsFlag);

DLC_EXPORT int32_t sceNpEntitlementAccessRaw_l0MTQHIcH3M(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    void* list,
    uint32_t listNum,
    uint32_t* hitNum) __asm__("l0MTQHIcH3M");
DLC_EXPORT int32_t sceNpEntitlementAccessRaw_eDXKe9FndlE(
    SceNpEntitlementAccessGameTrialsFlag* pftFlag) __asm__("eDXKe9FndlE");

static void __attribute__((constructor)) sceNpEntitlementAccessDlcInit(void) {
    dlcEmu_prewarmNpRpc();
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

DLC_EXPORT int32_t sceNpEntitlementAccessInitialize(
    const SceNpEntitlementAccessInitParam* initParam,
    SceNpEntitlementAccessBootParam* bootParam) {
    if (g_initialized.load(std::memory_order_acquire) != 0) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    const int32_t rc = dlcEmu_sceNpEntitlementAccessInitialize(initParam, bootParam);
    if (rc == SCE_OK) {
        g_initialized.store(1, std::memory_order_release);
    }
    return rc;
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetSkuFlag(SceNpEntitlementAccessSkuFlag* skuFlag) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetSkuFlag(skuFlag));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetAddcontEntitlementInfoList(
    SceNpServiceLabel serviceLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoList(
        serviceLabel,
        list,
        listNum,
        hitNum));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetAddcontEntitlementInfo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfo(
        serviceLabel,
        entitlementLabel,
        info));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetAddcontEntitlementInfoIndividual(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    if (userId == static_cast<SceUserServiceUserId>(-1)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoIndividual(
        userId,
        serviceLabel,
        entitlementLabel,
        info));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRaw_l0MTQHIcH3M(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    void* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    if (userId == static_cast<SceUserServiceUserId>(-1)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRaw_l0MTQHIcH3M(
        userId,
        serviceLabel,
        list,
        listNum,
        hitNum));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetEntitlementKey(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessEntitlementKey* key) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetEntitlementKey(serviceLabel, entitlementLabel, key));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGenerateTransactionId(
    SceNpEntitlementAccessTransactionId* transactionId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGenerateTransactionId(transactionId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestConsumeEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestConsumeEntitlement(
        userId,
        serviceLabel,
        entitlementLabel,
        transactionId,
        useCount,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(
        userId,
        serviceLabel,
        entitlementLabel,
        transactionId,
        useCount,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestConsumeServiceEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestConsumeServiceEntitlement(
        userId,
        serviceLabel,
        entitlementLabel,
        transactionId,
        useCount,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollConsumeEntitlement(int64_t requestId,
                                                                int32_t* pResult,
                                                                int32_t* useLimit) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollConsumeEntitlement(requestId, pResult, useLimit));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestConsumableEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestConsumableEntitlementInfo(
        userId,
        serviceLabel,
        entitlementLabel,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollConsumableEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollConsumableEntitlementInfo(
        requestId,
        pResult,
        info));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestUnifiedEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfo(
        userId,
        serviceLabel,
        entitlementLabel,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollUnifiedEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfo(requestId, pResult, info));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(
        userId,
        serviceLabel,
        list,
        listNum,
        param,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollUnifiedEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfoList(
        requestId,
        pResult,
        list,
        listNum,
        hitNum,
        nextOffset,
        previousOffset));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestServiceEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfo(
        userId,
        serviceLabel,
        entitlementLabel,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollServiceEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* info) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfo(requestId, pResult, info));
}

DLC_EXPORT int32_t sceNpEntitlementAccessRequestServiceEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfoList(
        userId,
        serviceLabel,
        list,
        listNum,
        param,
        requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessPollServiceEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfoList(
        requestId,
        pResult,
        list,
        listNum,
        hitNum,
        nextOffset,
        previousOffset));
}

DLC_EXPORT int32_t sceNpEntitlementAccessDeleteRequest(int64_t requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessDeleteRequest(requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessAbortRequest(int64_t requestId) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessAbortRequest(requestId));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetGameTrialsFlag(
    SceNpEntitlementAccessGameTrialsFlag* gameTrialsFlag) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessGetGameTrialsFlag(gameTrialsFlag));
}

DLC_EXPORT int32_t sceNpEntitlementAccessGetGameTrialsFlag_GameTrials(
    SceNpEntitlementAccessGameTrialsFlag* gameTrialsFlag) {
    return sceNpEntitlementAccessGetGameTrialsFlag(gameTrialsFlag);
}

DLC_EXPORT int32_t sceNpEntitlementAccessRaw_eDXKe9FndlE(
    SceNpEntitlementAccessGameTrialsFlag* pftFlag) {
    DLC_NP_CALL(dlcEmu_sceNpEntitlementAccessRaw_eDXKe9FndlE(pftFlag));
}

} // extern "C"
