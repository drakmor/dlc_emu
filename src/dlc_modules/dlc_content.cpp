/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared AppContent/NpEntitlementAccess replacement-module emulation.
 */

#include "dlc_content.h"

#include "dlc_log.h"

#include <sdk_version.h>

#include <_fs.h>
#include <_kernel.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <string.h>
#include <strings.h>

namespace {

extern "C" __attribute__((visibility("hidden"))) int Need_sceLibc = 0;

constexpr size_t kMaxDlcEntries = SCE_DLC_EMU_CONTENTIDS_MAX;
constexpr size_t kMaxConsumeTransactions = 256u;
constexpr size_t kMaxServiceListRequests = 32u;
constexpr size_t kMaxIniFileBytes = 32u * 1024u;
constexpr size_t kIniBufferBytes = kMaxIniFileBytes + 1u;
constexpr uint64_t kDefaultEntitlementKeyBase = 1024u;
constexpr int64_t kSyntheticRequestBase = 0x444c43000000ll;
constexpr int64_t kSyntheticEntryRequestBase = kSyntheticRequestBase + 0x10000000ll;
constexpr int32_t kNpEntitlementTitleTokenError = -2122514407; // 0x817D0019
constexpr int64_t kSyntheticServiceListRequestBase = kSyntheticRequestBase + 0x20000000ll;
constexpr uint32_t kNpReferencePackageTypeMax = 8u;
constexpr uint32_t kAppContentRpcCommandInitialize = 0x20000u;
constexpr uint32_t kAppContentRpcCommandAppParamGetInt = 0x20001u;
constexpr uint32_t kAppContentRpcCommandAppParamGetString = 0x20002u;
constexpr uint32_t kAppContentRpcCommandAddcontEnqueueDownload = 0x20008u;
constexpr uint32_t kAppContentRpcCommandAddcontEnqueueDownloadSp = 0x20009u;
constexpr uint32_t kAppContentRpcCommandTemporaryDataMount = 0x2000bu;
constexpr uint32_t kAppContentRpcCommandTemporaryDataUnmount = 0x2000cu;
constexpr uint32_t kAppContentRpcCommandTemporaryDataFormat = 0x2000du;
constexpr uint32_t kAppContentRpcCommandDownloadDataFormat = 0x2000eu;
constexpr uint32_t kAppContentRpcCommandDataGetAvailableSpaceKb = 0x2000fu;
constexpr uint32_t kAppContentRpcCommandDownloadDataGetBlockSize = 0x20010u;
constexpr uint32_t kAppContentRpcCommandGetRegion = 0x20012u;
constexpr uint32_t kAppContentRpcCommandRequestPatchInstall = 0x20013u;
constexpr uint32_t kAppContentRpcCommandGetDownloadedStoreCountry = 0x20014u;
constexpr uint32_t kAppContentRpcCommandAddcontEnqueueDownloadByEntitlementId = 0x20017u;
constexpr uint32_t kAppContentRpcCommandAddcontShrink = 0x2001cu;
constexpr uint32_t kAppContentRpcCommandGetAddcontDownloadProgress = 0x2001du;
constexpr uint32_t kAppContentRpcCommandRawXZo2 = 0x2001eu;
constexpr uint32_t kAppContentRpcCommandDownload0Shrink = 0x2001fu;
constexpr uint32_t kAppContentRpcCommandDownload0Expand = 0x20020u;
constexpr uint32_t kAppContentRpcCommandDownload1Shrink = 0x20021u;
constexpr uint32_t kAppContentRpcCommandDownload1Expand = 0x20022u;
constexpr uint32_t kAppContentRpcCommandRawUO = 0x20023u;
constexpr uint32_t kAppContentRpcCommandRawMFU = 0x20024u;
constexpr uint32_t kAppContentRpcCommandGetPftFlag = 0x20025u;
constexpr uint32_t kAppContentRpcCommandRawY8me = 0x20026u;
constexpr uint32_t kAppContentRpcCommandRaw1sa = 0x20027u;
constexpr uint32_t kAppContentRpcCommandRawSWV = 0x20028u;
constexpr uint32_t kNpRpcCommandRawEDX = 0x20015u;
constexpr uint32_t kNpRpcCommandGameTrials = 0x20016u;

class DlcMutex {
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }
    }

    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class DlcLockGuard {
public:
    explicit DlcLockGuard(DlcMutex& mutex) : mutex_(&mutex) {
        mutex_->lock();
    }

    ~DlcLockGuard() {
        if (mutex_) {
            mutex_->unlock();
        }
    }

    DlcLockGuard(const DlcLockGuard&) = delete;
    DlcLockGuard& operator=(const DlcLockGuard&) = delete;

private:
    DlcMutex* mutex_{};
};

struct DlcEntry {
    char contentId[37]{};
    SceNpUnifiedEntitlementLabel label{};
    SceNpServiceEntitlementLabel serviceLabel{};
    SceAppContentMountPoint mount{};
    uint8_t key[SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_KEY_SIZE]{};
    uint32_t packageType{SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC};
    uint32_t status{SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED};
    int32_t useCount{0};
    int32_t useLimit{1};
    uint64_t activeDate{0};
    uint64_t inactiveDate{UINT64_MAX};
    bool activeFlag{true};
    bool hasServiceLabel{false};
    bool consumable{false};
};

struct ParsedDlcEntry {
    char contentId[128]{};
    char serviceLabel[128]{};
    char mountPoint[128]{};
    char keyHex[128]{};
    char invalidField[32]{};
    uint32_t downloadStatus{SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED};
    int32_t useCount{0};
    int32_t useLimit{1};
    uint64_t activeDate{0};
    uint64_t inactiveDate{UINT64_MAX};
    bool activeFlag{true};
    bool consumable{false};
    bool consumableSet{false};
    bool valid{true};
};

struct ConsumedTransaction {
    bool used{false};
    size_t entryIndex{0};
    char transactionId[SCE_NP_ENTITLEMENT_ACCESS_TRANSACTION_ID_MAX_SIZE]{};
    int32_t useCount{0};
};

struct ServiceListRequest {
    uint32_t sort{SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_NONE};
    uint32_t direction{SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_NONE};
    uint32_t offset{0};
    uint32_t limit{0};
};

struct ServiceListPendingRequest {
    bool used{false};
    int64_t requestId{0};
    ServiceListRequest request{};
    uint32_t filterCount{0};
    SceNpServiceEntitlementLabel filters[SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE]{};
};

struct DlcState {
    DlcMutex mutex;
    bool loaded{false};
    bool available{false};
    size_t count{0};
    size_t autoMountCount{0};
    size_t mountedCount{0};
    DlcEntry entries[kMaxDlcEntries]{};
    bool mounted[kMaxDlcEntries]{};
    ConsumedTransaction consumed[kMaxConsumeTransactions]{};
    size_t consumedNext{0};
    ServiceListPendingRequest serviceListRequests[kMaxServiceListRequests]{};
    uint64_t serviceListRequestNext{1};
};

alignas(DlcState) unsigned char g_stateStorage[sizeof(DlcState)];
std::atomic<DlcState*> g_state{nullptr};
std::atomic<uint32_t> g_stateInit{0};
std::atomic<uint64_t> g_transactionCounter{0};

struct IpcBuffer {
    void* data;
    uint64_t size;
};

struct AppContentRpcControl {
    uint32_t value;
    int32_t result;
};

using IpmiConfigCtor = void (*)(void*);
using IpmiClientCreate = int32_t (*)(void**, const void*, void*, void*);
using IpmiClientConnect = int32_t (*)(void*, uint64_t, uint64_t, int32_t*);
using IpmiClientInvoke = int32_t (*)(
    void*, uint32_t, const IpcBuffer*, uint32_t, int32_t*, IpcBuffer*, uint32_t);

extern "C" void ipmi_client_config_ctor(void*) __asm__("_ZN4IPMI6Client6ConfigC1Ev");
extern "C" int32_t ipmi_client_create(void**, const void*, void*, void*)
    __asm__("_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_");

struct RpcClientState {
    DlcMutex mutex;
    bool attempted{false};
    bool ready{false};
    void* client{nullptr};
    alignas(16) unsigned char clientStorage[0x17000]{};
};

RpcClientState g_appRpc{};
RpcClientState g_npRpc{};

bool is_zeroed(const void* data, size_t size) {
    if (!data) return false;
    static const unsigned char zeros[64]{};
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size != 0) {
        const size_t chunk = size < sizeof(zeros) ? size : sizeof(zeros);
        if (std::memcmp(bytes, zeros, chunk) != 0) return false;
        bytes += chunk;
        size -= chunk;
    }
    return true;
}

bool parse_hex_key(const char* hex, uint8_t out[16]) {
    if (!hex || !out) return false;
    for (size_t i = 0; i < 32; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(hex[i]))) return false;
    }
    if (hex[32] != '\0') return false;
    for (size_t i = 0; i < 16; ++i) {
        char byteText[3] = {hex[i * 2u], hex[i * 2u + 1u], '\0'};
        out[i] = static_cast<uint8_t>(std::strtoul(byteText, nullptr, 16));
    }
    return true;
}

void make_default_key(size_t index, uint8_t out[16]) {
    uint64_t value = kDefaultEntitlementKeyBase + static_cast<uint64_t>(index);
    std::memcpy(out, &value, sizeof(value));
    std::memset(out + sizeof(value), 0, 16 - sizeof(value));
}

DlcState& state() {
    DlcState* p = g_state.load(std::memory_order_acquire);
    if (p) return *p;

    uint32_t expected = 0;
    if (g_stateInit.compare_exchange_strong(expected,
                                            1u,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        p = new (g_stateStorage) DlcState();
        g_state.store(p, std::memory_order_release);
        g_stateInit.store(2u, std::memory_order_release);
        return *p;
    }
    while (g_stateInit.load(std::memory_order_acquire) != 2u) {
        __builtin_ia32_pause();
    }
    return *g_state.load(std::memory_order_acquire);
}

uint32_t compiled_sdk_version() {
    return SCE_PROSPERO_SDK_VERSION;
}

bool ensure_rpc_unlocked(RpcClientState& rpc,
                         const char* logTag,
                         const char serviceName[16],
                         uint64_t requestBufferSize,
                         uint64_t clientStorageSize) {
    if (rpc.attempted) {
        return rpc.ready;
    }
    rpc.attempted = true;

    alignas(16) unsigned char config[0x180]{};
    ipmi_client_config_ctor(config);
    std::memcpy(config, serviceName, 16);
    *reinterpret_cast<uint64_t*>(config + 0x10u) = 0;
    *reinterpret_cast<uint64_t*>(config + 0x28u) = requestBufferSize;
    *reinterpret_cast<uint64_t*>(config + 0x30u) = clientStorageSize;

    const int32_t createRc =
        ipmi_client_create(&rpc.client, config, nullptr, rpc.clientStorage);
    if (createRc < 0 || !rpc.client) {
        dlc_logf("dlc.%s_rpc create-failed rc=0x%08x",
                          logTag,
                          static_cast<unsigned>(createRc));
        return false;
    }

    auto** vtable = *reinterpret_cast<void***>(rpc.client);
    auto connect = reinterpret_cast<IpmiClientConnect>(vtable[2]);
    int32_t serviceResult = 0;
    const int32_t connectRc = connect(rpc.client, 0, 0, &serviceResult);
    if (connectRc < 0 || serviceResult < 0) {
        dlc_logf("dlc.%s_rpc connect-failed rc=0x%08x service=0x%08x",
                          logTag,
                          static_cast<unsigned>(connectRc),
                          static_cast<unsigned>(serviceResult));
        rpc.client = nullptr;
        return false;
    }

    rpc.ready = true;
    return true;
}

bool ensure_app_rpc_unlocked() {
    static constexpr char kServiceName[16] = {
        'S', 'c', 'e', 'A', 'p', 'p', 'C', 'o',
        'n', 't', 'e', 'n', 't', '\0', '\0', '\0'
    };
    return ensure_rpc_unlocked(g_appRpc, "app", kServiceName, 0x200u, 0xf800u);
}

bool ensure_np_rpc_unlocked() {
    static constexpr char kServiceName[16] = {
        'S', 'c', 'e', 'N', 'p', 'E', 'n', 't',
        'A', 'c', 'c', 'e', 's', 's', '\0', '\0'
    };
    return ensure_rpc_unlocked(g_npRpc, "np", kServiceName, 0x1000u, 0x17000u);
}

int32_t map_app_rpc_result(int32_t rc) {
    const uint32_t sdkVersion = compiled_sdk_version();
    if (sdkVersion < 0x1500000u) {
        return rc;
    }

    if (rc == SCE_OK ||
        rc == SCE_APP_CONTENT_ERROR_NOT_INITIALIZED ||
        rc == SCE_APP_CONTENT_ERROR_PARAMETER ||
        rc == SCE_APP_CONTENT_ERROR_BUSY ||
        rc == SCE_APP_CONTENT_ERROR_NOT_MOUNTED ||
        rc == SCE_APP_CONTENT_ERROR_NOT_FOUND ||
        rc == SCE_APP_CONTENT_ERROR_MOUNT_FULL ||
        rc == SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT ||
        rc == SCE_APP_CONTENT_ERROR_NO_SPACE ||
        rc == SCE_APP_CONTENT_ERROR_NOT_SUPPORTED ||
        rc == SCE_APP_CONTENT_ERROR_INTERNAL ||
        rc == SCE_APP_CONTENT_ERROR_DOWNLOAD_ENTRY_FULL ||
        rc == SCE_APP_CONTENT_ERROR_INVALID_PKG ||
        rc == SCE_APP_CONTENT_ERROR_OTHER_APPLICATION_PKG ||
        rc == SCE_APP_CONTENT_ERROR_CREATE_FULL ||
        rc == SCE_APP_CONTENT_ERROR_MOUNT_OTHER_APP ||
        rc == SCE_APP_CONTENT_ERROR_OF_MEMORY ||
        rc == SCE_APP_CONTENT_ERROR_ADDCONT_SHRANK ||
        rc == SCE_APP_CONTENT_ERROR_ADDCONT_NO_IN_QUEUE ||
        rc == SCE_APP_CONTENT_ERROR_SIGNED_OUT ||
        rc == SCE_APP_CONTENT_ERROR_UNSUPPORTED_COMPRESSION_FORMAT ||
        rc == SCE_APP_CONTENT_ERROR_BROKEN ||
        rc == SCE_APP_CONTENT_ERROR_ADDCONT_NO_SPACE ||
        rc == SCE_APP_CONTENT_ERROR_ADDCONT_ENFILE) {
        return rc;
    }
    if (rc == SCE_APP_CONTENT_ERROR_NETWORK) {
        return sdkVersion >= 0x3500000u ? SCE_APP_CONTENT_ERROR_NETWORK
                                        : SCE_APP_CONTENT_ERROR_INTERNAL;
    }
    if (rc == static_cast<int32_t>(0x80020010u)) {
        return SCE_APP_CONTENT_ERROR_BUSY;
    }
    return SCE_APP_CONTENT_ERROR_INTERNAL;
}

int32_t app_rpc_invoke(uint32_t command,
                       const IpcBuffer* input,
                       uint32_t inputCount,
                       IpcBuffer* output,
                       uint32_t outputCount) {
    DlcLockGuard lock(g_appRpc.mutex);
    if (!ensure_app_rpc_unlocked()) {
        return SCE_APP_CONTENT_ERROR_INTERNAL;
    }
    auto** vtable = *reinterpret_cast<void***>(g_appRpc.client);
    auto invoke = reinterpret_cast<IpmiClientInvoke>(vtable[11]);
    int32_t serviceResult = 0;
    const int32_t invokeRc =
        invoke(g_appRpc.client, command, input, inputCount, &serviceResult, output, outputCount);
    return invokeRc == SCE_OK ? map_app_rpc_result(serviceResult) : map_app_rpc_result(invokeRc);
}

int32_t np_rpc_invoke(uint32_t command,
                      const IpcBuffer* input,
                      uint32_t inputCount,
                      IpcBuffer* output,
                      uint32_t outputCount) {
    DlcLockGuard lock(g_npRpc.mutex);
    if (!ensure_np_rpc_unlocked()) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_INTERNAL;
    }
    auto** vtable = *reinterpret_cast<void***>(g_npRpc.client);
    auto invoke = reinterpret_cast<IpmiClientInvoke>(vtable[11]);
    int32_t serviceResult = 0;
    const int32_t invokeRc =
        invoke(g_npRpc.client, command, input, inputCount, &serviceResult, output, outputCount);
    return invokeRc == SCE_OK ? serviceResult : invokeRc;
}

int32_t app_rpc_initialize(const SceAppContentInitParam* initParam,
                           SceAppContentBootParam* bootParam) {
    const uint32_t sdkVersion = compiled_sdk_version();
    AppContentRpcControl control{sdkVersion, 0};
    IpcBuffer inputs[2] = {
        {&control, sizeof(control.value)},
        {const_cast<SceAppContentInitParam*>(initParam), sizeof(*initParam)}
    };
    IpcBuffer outputs[1] = {{bootParam, sizeof(*bootParam)}};
    return app_rpc_invoke(kAppContentRpcCommandInitialize,
                          inputs,
                          2,
                          outputs,
                          1);
}

int32_t app_rpc_entitlement_command(uint32_t command,
                                    SceNpServiceLabel serviceLabel,
                                    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    AppContentRpcControl control{static_cast<uint32_t>(serviceLabel), 0};
    IpcBuffer inputs[2] = {
        {&control, sizeof(control.value)},
        {const_cast<SceNpUnifiedEntitlementLabel*>(entitlementLabel), sizeof(*entitlementLabel)}
    };
    return app_rpc_invoke(command, inputs, 2, nullptr, 0);
}

int32_t app_rpc_entitlement_progress(uint32_t command,
                                     SceNpServiceLabel serviceLabel,
                                     const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                     SceAppContentAddcontDownloadProgress* progress) {
    AppContentRpcControl control{static_cast<uint32_t>(serviceLabel), 0};
    IpcBuffer inputs[2] = {
        {&control, sizeof(control.value)},
        {const_cast<SceNpUnifiedEntitlementLabel*>(entitlementLabel), sizeof(*entitlementLabel)}
    };
    IpcBuffer outputs[1] = {{progress, sizeof(*progress)}};
    return app_rpc_invoke(command, inputs, 2, outputs, 1);
}

int32_t app_rpc_entitlement_id_command(uint32_t command, const char* entitlementId) {
    char buffer[40]{};
    strlcpy(buffer, entitlementId, sizeof(buffer));
    IpcBuffer inputs[1] = {{buffer, sizeof(buffer)}};
    return app_rpc_invoke(command, inputs, 1, nullptr, 0);
}

int32_t app_rpc_playable_status(SceNpServiceLabel serviceLabel,
                                const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                uint32_t* playableStatus) {
    AppContentRpcControl control{static_cast<uint32_t>(serviceLabel), 0};
    IpcBuffer inputs[2] = {
        {&control, sizeof(control.value)},
        {const_cast<SceNpUnifiedEntitlementLabel*>(entitlementLabel), sizeof(*entitlementLabel)}
    };
    IpcBuffer outputs[1] = {{playableStatus, sizeof(*playableStatus)}};
    return app_rpc_invoke(kAppContentRpcCommandRawY8me,
                          inputs,
                          2,
                          outputs,
                          1);
}

int32_t app_rpc_u32_output(uint32_t command, uint32_t* value) {
    IpcBuffer outputs[1] = {{value, sizeof(*value)}};
    return app_rpc_invoke(command, nullptr, 0, outputs, 1);
}

int32_t app_rpc_blob16_output1(uint32_t command, const void* input, void* output) {
    IpcBuffer inputs[1] = {{const_cast<void*>(input), 16u}};
    IpcBuffer outputs[1] = {{output, 1u}};
    return app_rpc_invoke(command, inputs, 1, outputs, 1);
}

int32_t app_rpc_control_output(uint32_t command, uint32_t value, void* output, uint64_t outputSize) {
    AppContentRpcControl control{value, 0};
    IpcBuffer inputs[1] = {{&control, sizeof(control.value)}};
    IpcBuffer outputs[1] = {{output, outputSize}};
    return app_rpc_invoke(command, inputs, 1, outputs, 1);
}

int32_t app_rpc_app_param_string(SceAppContentAppParamId paramId, char* value, size_t valueSize) {
    struct Input {
        uint32_t paramId;
        uint32_t reserved;
        uint64_t valueSize;
    } input{static_cast<uint32_t>(paramId), 0, static_cast<uint64_t>(valueSize)};
    IpcBuffer inputs[1] = {{&input, sizeof(input)}};
    IpcBuffer outputs[1] = {{value, static_cast<uint64_t>(valueSize)}};
    return app_rpc_invoke(kAppContentRpcCommandAppParamGetString, inputs, 1, outputs, 1);
}

int32_t app_rpc_string_input(uint32_t command, const char* value) {
    IpcBuffer inputs[1] = {{const_cast<char*>(value), std::strlen(value) + 1u}};
    return app_rpc_invoke(command, inputs, 1, nullptr, 0);
}

int32_t app_rpc_mount(uint32_t command,
                      uint32_t option,
                      SceAppContentMountPoint* mountPoint) {
    AppContentRpcControl control{option, 0};
    IpcBuffer input[1] = {{&control, sizeof(control.value)}};
    IpcBuffer output[1] = {{mountPoint, sizeof(*mountPoint)}};
    return app_rpc_invoke(command, input, 1, output, 1);
}

int32_t app_rpc_mount_operation(uint32_t command, const SceAppContentMountPoint* mountPoint) {
    IpcBuffer input[1] = {{const_cast<SceAppContentMountPoint*>(mountPoint), sizeof(*mountPoint)}};
    return app_rpc_invoke(command, input, 1, nullptr, 0);
}

int32_t app_rpc_mount_query(uint32_t command,
                            uint32_t selector,
                            const SceAppContentMountPoint* mountPoint,
                            size_t* value) {
    AppContentRpcControl control{selector, 0};
    IpcBuffer inputs[2] = {
        {&control, sizeof(control.value)},
        {const_cast<SceAppContentMountPoint*>(mountPoint), sizeof(*mountPoint)}
    };
    IpcBuffer outputs[1] = {{value, sizeof(*value)}};
    return app_rpc_invoke(command, inputs, 2, outputs, 1);
}

int32_t app_rpc_mount_handle_command(uint32_t command, const SceAppContentMountPoint* mountPoint) {
    uint64_t handle = reinterpret_cast<uint64_t>(mountPoint);
    IpcBuffer input[1] = {{&handle, sizeof(handle)}};
    return app_rpc_invoke(command, input, 1, nullptr, 0);
}

int32_t np_rpc_u32_output(uint32_t command, uint32_t* value) {
    IpcBuffer outputs[1] = {{value, sizeof(*value)}};
    return np_rpc_invoke(command, nullptr, 0, outputs, 1);
}

const char* package_type_name(uint32_t packageType) {
    switch (packageType) {
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE: return "NONE";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSGD: return "PSGD";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC: return "PSAC";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAL: return "PSAL";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSCONS: return "PSCONS";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSVC: return "PSVC";
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSSUBS: return "PSSUBS";
        case 8u: return "TYPE8";
        default: return "UNKNOWN";
    }
}

const char* download_status_name(uint32_t status) {
    switch (status) {
        case SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_NO_EXTRA_DATA: return "NO_EXTRA_DATA";
        case SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_NO_IN_QUEUE: return "NO_IN_QUEUE";
        case SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_DOWNLOADING: return "DOWNLOADING";
        case SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_DOWNLOAD_SUSPENDED: return "DOWNLOAD_SUSPENDED";
        case SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED: return "INSTALLED";
        default: return "UNKNOWN";
    }
}

bool package_type_supports_mount(uint32_t packageType) {
    return packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC;
}

void format_key_hex(const uint8_t key[16], char out[33]) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    if (!key || !out) return;
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2u] = kHex[key[i] >> 4u];
        out[i * 2u + 1u] = kHex[key[i] & 0x0fu];
    }
    out[32] = '\0';
}

void log_dlc_entry_apply(size_t index, const DlcEntry& entry, const char* keySource) {
    const char* const type = package_type_name(entry.packageType);
    switch (entry.packageType) {
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC: {
            char keyHex[33]{};
            format_key_hex(entry.key, keyHex);
            dlc_logf("dlc.entry apply index=%u type=%s contentId=%s label=%s status=%s mount=%s key=%s keySource=%s",
                              static_cast<unsigned>(index),
                              type,
                              entry.contentId,
                              entry.label.data,
                              download_status_name(entry.status),
                              entry.mount.data,
                              keyHex,
                              keySource ? keySource : "");
            return;
        }
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSGD:
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAL: {
            char keyHex[33]{};
            format_key_hex(entry.key, keyHex);
            dlc_logf("dlc.entry apply index=%u type=%s contentId=%s label=%s status=%s key=%s keySource=%s",
                              static_cast<unsigned>(index),
                              type,
                              entry.contentId,
                              entry.label.data,
                              download_status_name(entry.status),
                              keyHex,
                              keySource ? keySource : "");
            return;
        }
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSCONS:
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSVC:
            dlc_logf("dlc.entry apply index=%u type=%s contentId=%s label=%s serviceLabel=%s consumable=%u useCount=%d useLimit=%d active=%u activeDate=%llu inactiveDate=%llu",
                              static_cast<unsigned>(index),
                              type,
                              entry.contentId,
                              entry.label.data,
                              entry.hasServiceLabel ? entry.serviceLabel.data : "",
                              entry.consumable ? 1u : 0u,
                              entry.useCount,
                              entry.useLimit,
                              entry.activeFlag ? 1u : 0u,
                              static_cast<unsigned long long>(entry.activeDate),
                              static_cast<unsigned long long>(entry.inactiveDate));
            return;
        case SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSSUBS:
            dlc_logf("dlc.entry apply index=%u type=%s contentId=%s label=%s serviceLabel=%s consumable=%u active=%u activeDate=%llu inactiveDate=%llu",
                              static_cast<unsigned>(index),
                              type,
                              entry.contentId,
                              entry.label.data,
                              entry.hasServiceLabel ? entry.serviceLabel.data : "",
                              entry.consumable ? 1u : 0u,
                              entry.activeFlag ? 1u : 0u,
                              static_cast<unsigned long long>(entry.activeDate),
                              static_cast<unsigned long long>(entry.inactiveDate));
            return;
        default:
            dlc_logf("dlc.entry apply index=%u type=%s contentId=%s label=%s",
                              static_cast<unsigned>(index),
                              type,
                              entry.contentId,
                              entry.label.data);
            return;
    }
}

bool parse_u64(const char* text, uint64_t* out);

bool parse_u32(const char* text, uint32_t* out) {
    uint64_t value = 0;
    if (!parse_u64(text, &value) || value > UINT32_MAX) return false;
    if (out) *out = static_cast<uint32_t>(value);
    return true;
}

bool parse_u64(const char* text, uint64_t* out) {
    if (!text || !text[0] || !out) return false;
    if (text[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return false;
    *out = static_cast<uint64_t>(value);
    return true;
}

bool parse_i32(const char* text, int32_t* out) {
    if (!text || !text[0] || !out) return false;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value < INT32_MIN || value > INT32_MAX) return false;
    *out = static_cast<int32_t>(value);
    return true;
}

bool parse_package_type(const char* text, uint32_t* out) {
    if (!text || !text[0] || !out) return false;
    if (::strcasecmp(text, "NONE") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE;
    } else if (::strcasecmp(text, "PSGD") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSGD;
    } else if (::strcasecmp(text, "PSAC") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC;
    } else if (::strcasecmp(text, "PSAL") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAL;
    } else if (::strcasecmp(text, "PSCONS") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSCONS;
    } else if (::strcasecmp(text, "PSVC") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSVC;
    } else if (::strcasecmp(text, "PSSUBS") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSSUBS;
    } else {
        return false;
    }
    return *out <= kNpReferencePackageTypeMax;
}

bool parse_download_status(const char* text, uint32_t* out) {
    if (!text || !text[0] || !out) return false;
    if (::strcasecmp(text, "NO_EXTRA_DATA") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_NO_EXTRA_DATA;
    } else if (::strcasecmp(text, "NO_IN_QUEUE") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_NO_IN_QUEUE;
    } else if (::strcasecmp(text, "DOWNLOADING") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_DOWNLOADING;
    } else if (::strcasecmp(text, "DOWNLOAD_SUSPENDED") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_DOWNLOAD_SUSPENDED;
    } else if (::strcasecmp(text, "INSTALLED") == 0) {
        *out = SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED;
    } else if (!parse_u32(text, out)) {
        return false;
    }
    return *out <= SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED;
}

class DlcIniParser {
public:
    static bool parse(char* text, size_t textSize, DlcState& st) {
        ParsedDlcEntry parsed{};
        uint32_t sectionPackageType = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE;
        bool inDlcSection = false;
        unsigned lineNo = 0;
        char* const end = text + textSize;

        for (char* line = text; line < end && *line;) {
            ++lineNo;
            char* next = line;
            while (next < end && *next != '\n') ++next;
            if (next < end) *next++ = '\0';
            char* nl = std::strchr(line, '\r');
            if (nl) *nl = '\0';
            if (lineNo == 1) strip_bom(line);

            char* comment = std::strchr(line, '#');
            if (comment) *comment = '\0';
            char* body = trim(line);
            if (!body[0]) {
                line = next;
                continue;
            }

            if (body[0] == '[') {
                finish_section(st, parsed, inDlcSection, sectionPackageType);
                inDlcSection = false;
                sectionPackageType = SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE;

                char* close = std::strchr(body + 1, ']');
                if (!close) {
                    dlc_logf("dlc.load status=parse-failed reason=bad-section line=%u",
                                      lineNo);
                    return false;
                }
                *close = '\0';
                char* section = trim(body + 1);
                if (!section[0]) {
                    dlc_logf("dlc.load status=parse-failed reason=empty-section line=%u",
                                      lineNo);
                    return false;
                }
                if (parse_package_type(section, &sectionPackageType)) {
                    parsed = ParsedDlcEntry{};
                    inDlcSection = true;
                }
                line = next;
                continue;
            }

            if (!inDlcSection) {
                line = next;
                continue;
            }

            char* eq = std::strchr(body, '=');
            if (!eq) {
                dlc_logf("dlc.line skip reason=missing-equals line=%u", lineNo);
                line = next;
                continue;
            }
            *eq = '\0';
            char* key = trim(body);
            char* value = trim(eq + 1);
            if (!key[0]) {
                dlc_logf("dlc.line skip reason=empty-key line=%u", lineNo);
                line = next;
                continue;
            }

            read_entry_field(parsed, key, value);
            line = next;
        }

        finish_section(st, parsed, inDlcSection, sectionPackageType);
        return true;
    }

private:
    static void strip_bom(char* line) {
        static const unsigned char bom[] = {0xefu, 0xbbu, 0xbfu};
        if (line && std::memcmp(line, bom, sizeof(bom)) == 0) {
            std::memmove(line, line + sizeof(bom), std::strlen(line + sizeof(bom)) + 1u);
        }
    }

    static char* trim(char* text) {
        while (*text == ' ' || *text == '\t') ++text;
        char* end = text + std::strlen(text);
        while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        return text;
    }

    static bool key_equals(const char* lhs, const char* rhs) {
        if (!lhs || !rhs) return false;
        for (;;) {
            while (*lhs == '_' || *lhs == '-') ++lhs;
            while (*rhs == '_' || *rhs == '-') ++rhs;
            const unsigned char a = static_cast<unsigned char>(*lhs);
            const unsigned char b = static_cast<unsigned char>(*rhs);
            if (std::tolower(a) != std::tolower(b)) return false;
            if (a == '\0') return true;
            ++lhs;
            ++rhs;
        }
    }

    static bool copy_value(char* out, size_t outSize, const char* value) {
        if (!out || outSize == 0 || !value) return false;
        const size_t len = std::strlen(value);
        if (len >= outSize) return false;
        std::memcpy(out, value, len + 1u);
        return true;
    }

    static bool parse_value_bool(const char* token, bool* out) {
        if (!token || !token[0] || !out) return false;
        if (::strcasecmp(token, "true") == 0 ||
            std::strcmp(token, "1") == 0 ||
            ::strcasecmp(token, "yes") == 0 ||
            ::strcasecmp(token, "on") == 0) {
            *out = true;
            return true;
        }
        if (::strcasecmp(token, "false") == 0 ||
            std::strcmp(token, "0") == 0 ||
            ::strcasecmp(token, "no") == 0 ||
            ::strcasecmp(token, "off") == 0) {
            *out = false;
            return true;
        }
        return false;
    }

    static void invalidate(ParsedDlcEntry& parsed, const char* fieldName) {
        parsed.valid = false;
        strlcpy(parsed.invalidField, fieldName ? fieldName : "", sizeof(parsed.invalidField));
    }

    static void read_entry_field(ParsedDlcEntry& parsed, const char* key, const char* value) {
        if (key_equals(key, "content_id")) {
            if (!copy_value(parsed.contentId, sizeof(parsed.contentId), value)) invalidate(parsed, "content_id");
        } else if (key_equals(key, "service_label")) {
            if (!copy_value(parsed.serviceLabel, sizeof(parsed.serviceLabel), value)) invalidate(parsed, "service_label");
        } else if (key_equals(key, "mount_point")) {
            if (!copy_value(parsed.mountPoint, sizeof(parsed.mountPoint), value)) invalidate(parsed, "mount_point");
        } else if (key_equals(key, "entitlement_key")) {
            if (!copy_value(parsed.keyHex, sizeof(parsed.keyHex), value)) invalidate(parsed, "entitlement_key");
        } else if (key_equals(key, "download_status")) {
            if (!parse_download_status(value, &parsed.downloadStatus)) invalidate(parsed, "download_status");
        } else if (key_equals(key, "use_count")) {
            if (!parse_i32(value, &parsed.useCount)) invalidate(parsed, "use_count");
        } else if (key_equals(key, "use_limit")) {
            if (!parse_i32(value, &parsed.useLimit)) invalidate(parsed, "use_limit");
        } else if (key_equals(key, "active_flag")) {
            if (!parse_value_bool(value, &parsed.activeFlag)) invalidate(parsed, "active_flag");
        } else if (key_equals(key, "active_date")) {
            if (!parse_u64(value, &parsed.activeDate)) invalidate(parsed, "active_date");
        } else if (key_equals(key, "inactive_date")) {
            if (!parse_u64(value, &parsed.inactiveDate)) invalidate(parsed, "inactive_date");
        } else if (key_equals(key, "consumable") || key_equals(key, "is_consumable")) {
            if (!parse_value_bool(value, &parsed.consumable)) {
                invalidate(parsed, "consumable");
            } else {
                parsed.consumableSet = true;
            }
        }
    }

    static bool valid_content_id(const char* contentId, const char** label) {
        if (!contentId || !label) return false;
        size_t len = std::strlen(contentId);
        if (len != 36) return false;
        const char* dash = std::strrchr(contentId, '-');
        if (!dash || std::strlen(dash + 1) != 16) return false;
        for (const char* p = dash + 1; *p; ++p) {
            if (!std::isalnum(static_cast<unsigned char>(*p))) return false;
        }
        *label = dash + 1;
        return true;
    }

    static bool label_exists(const DlcState& st, const char* label) {
        for (size_t i = 0; i < st.count; ++i) {
            if (std::strcmp(st.entries[i].label.data, label) == 0) {
                return true;
            }
        }
        return false;
    }

    static bool valid_service_label_text(const char* label) {
        if (!label || !label[0]) return false;
        const size_t len = std::strlen(label);
        if (len >= SCE_NP_SERVICE_ENTITLEMENT_LABEL_SIZE) return false;
        for (const char* p = label; *p; ++p) {
            if (!std::isalnum(static_cast<unsigned char>(*p))) return false;
        }
        return true;
    }

    static bool service_label_exists(const DlcState& st, const char* label) {
        if (!label || !label[0]) return false;
        for (size_t i = 0; i < st.count; ++i) {
            if (st.entries[i].hasServiceLabel &&
                std::strcmp(st.entries[i].serviceLabel.data, label) == 0) {
                return true;
            }
        }
        return false;
    }

    static void finish_section(DlcState& st,
                               ParsedDlcEntry& parsed,
                               bool inDlcSection,
                               uint32_t packageType) {
        if (!inDlcSection) return;
        if (!parsed.valid) {
            dlc_logf("dlc.entry skip reason=bad-field field=%s",
                              parsed.invalidField[0] ? parsed.invalidField : "<unknown>");
            parsed = ParsedDlcEntry{};
            return;
        }
        if (!parsed.contentId[0]) {
            dlc_logf("dlc.entry skip reason=missing-content-id");
            parsed = ParsedDlcEntry{};
            return;
        }
        append_entry(st, parsed, packageType);
        parsed = ParsedDlcEntry{};
    }

    static void append_entry(DlcState& st, const ParsedDlcEntry& parsed, uint32_t packageType) {
        const char* contentId = parsed.contentId;
        if (st.count >= kMaxDlcEntries) {
            dlc_logf("dlc.entry skip reason=max contentId=%s", contentId ? contentId : "<null>");
            return;
        }
        const char* label = nullptr;
        if (!valid_content_id(contentId, &label)) {
            dlc_logf("dlc.entry skip reason=bad-content-id contentId=%s", contentId ? contentId : "<null>");
            return;
        }
        if (label_exists(st, label)) {
            dlc_logf("dlc.entry skip reason=duplicate-label label=%s", label);
            return;
        }

        DlcEntry& entry = st.entries[st.count];
        strlcpy(entry.contentId, contentId, sizeof(entry.contentId));
        strlcpy(entry.label.data, label, sizeof(entry.label.data));
        entry.packageType = packageType;
        entry.status = parsed.downloadStatus;
        entry.useCount = parsed.useCount;
        entry.useLimit = parsed.useLimit;
        entry.activeFlag = parsed.activeFlag;
        entry.activeDate = parsed.activeDate;
        entry.inactiveDate = parsed.inactiveDate;
        entry.consumable = parsed.consumableSet
                               ? parsed.consumable
                               : (packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSCONS ||
                                  packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSVC);
        if (parsed.serviceLabel[0]) {
            if (!valid_service_label_text(parsed.serviceLabel)) {
                dlc_logf("dlc.entry skip reason=bad-service-label label=%s serviceLabel=%s",
                         label,
                         parsed.serviceLabel);
                entry = DlcEntry{};
                return;
            }
            if (service_label_exists(st, parsed.serviceLabel)) {
                dlc_logf("dlc.entry skip reason=duplicate-service-label serviceLabel=%s",
                         parsed.serviceLabel);
                entry = DlcEntry{};
                return;
            }
            strlcpy(entry.serviceLabel.data, parsed.serviceLabel, sizeof(entry.serviceLabel.data));
            entry.hasServiceLabel = true;
        }
        if (parsed.mountPoint[0]) {
            if (std::strlen(parsed.mountPoint) >= sizeof(entry.mount.data)) {
                dlc_logf("dlc.entry skip reason=mount-too-long label=%s", label);
                entry = DlcEntry{};
                return;
            }
            strlcpy(entry.mount.data, parsed.mountPoint, sizeof(entry.mount.data));
        } else if (package_type_supports_mount(packageType)) {
            const int mountLen = std::snprintf(entry.mount.data,
                                               sizeof(entry.mount.data),
                                               "%s%u",
                                               SCE_DLC_EMU_MOUNT_PREFIX,
                                               static_cast<unsigned>(st.autoMountCount));
            if (mountLen < 0 || static_cast<size_t>(mountLen) >= sizeof(entry.mount.data)) {
                dlc_logf("dlc.entry skip reason=mount-too-long label=%s", label);
                entry = DlcEntry{};
                return;
            }
            ++st.autoMountCount;
        }
        if (parsed.keyHex[0]) {
            if (!parse_hex_key(parsed.keyHex, entry.key)) {
                dlc_logf("dlc.entry skip reason=bad-key label=%s", label);
                entry = DlcEntry{};
                return;
            }
        } else {
            make_default_key(st.count, entry.key);
        }

        log_dlc_entry_apply(st.count, entry, parsed.keyHex[0] ? "config" : "default");

        ++st.count;
    }
};

bool load_file(char* out, size_t outSize, size_t* outLen, bool* tooLarge) {
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (tooLarge) *tooLarge = false;
    int fd = ::sceKernelOpen(SCE_DLC_EMU_INI_PATH, SCE_KERNEL_O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }
    size_t total = 0;
    while (total + 1u < outSize) {
        const ssize_t rc = ::sceKernelRead(fd, out + total, outSize - total - 1u);
        if (rc < 0) {
            (void)::sceKernelClose(fd);
            out[0] = '\0';
            return false;
        }
        if (rc == 0) {
            break;
        }
        total += static_cast<size_t>(rc);
    }
    if (total + 1u >= outSize) {
        char extra = '\0';
        const ssize_t rc = ::sceKernelRead(fd, &extra, sizeof(extra));
        if (rc < 0) {
            (void)::sceKernelClose(fd);
            out[0] = '\0';
            return false;
        }
        if (rc > 0) {
            (void)::sceKernelClose(fd);
            out[0] = '\0';
            if (tooLarge) *tooLarge = true;
            return false;
        }
    }
    (void)::sceKernelClose(fd);
    out[total] = '\0';
    if (outLen) *outLen = total;
    return true;
}

DlcState& ensure_loaded() {
    DlcState& st = state();
    DlcLockGuard lock(st.mutex);
    if (st.loaded) {
        return st;
    }
    st.loaded = true;
    st.count = 0;

    static char ini[kIniBufferBytes]{};
    size_t len = 0;
    bool tooLarge = false;
    if (!load_file(ini, sizeof(ini), &len, &tooLarge)) {
        if (tooLarge) {
            dlc_logf("dlc.load status=parse-failed reason=file-too-large path=%s maxBytes=%u",
                              SCE_DLC_EMU_INI_PATH,
                              static_cast<unsigned>(kMaxIniFileBytes));
            return st;
        }
        dlc_logf("dlc.load status=missing path=%s", SCE_DLC_EMU_INI_PATH);
        return st;
    }
    if (!DlcIniParser::parse(ini, len, st)) {
        st.count = 0;
        dlc_logf("dlc.load status=parse-failed path=%s bytes=%u",
                                  SCE_DLC_EMU_INI_PATH,
                                  static_cast<unsigned>(len));
        return st;
    }
    st.available = st.count > 0;
    dlc_logf("dlc.load status=ok path=%s entries=%u",
                              SCE_DLC_EMU_INI_PATH,
                              static_cast<unsigned>(st.count));
    return st;
}

const DlcEntry* find_entry_by_label_unlocked(const DlcState& st, const SceNpUnifiedEntitlementLabel* label) {
    if (!label) return nullptr;
    for (size_t i = 0; i < st.count; ++i) {
        if (std::strncmp(st.entries[i].label.data, label->data, SCE_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1u) == 0) {
            return &st.entries[i];
        }
    }
    return nullptr;
}

const DlcEntry* find_mounted_entry_by_mount_unlocked(const DlcState& st,
                                                     const SceAppContentMountPoint* mountPoint) {
    if (!mountPoint || !mountPoint->data[0]) return nullptr;
    for (size_t i = 0; i < st.count; ++i) {
        if (!st.mounted[i] || !st.entries[i].mount.data[0]) continue;
        if (std::strcmp(st.entries[i].mount.data, mountPoint->data) == 0) {
            return &st.entries[i];
        }
    }
    return nullptr;
}

size_t entry_index_unlocked(const DlcState& st, const DlcEntry* entry) {
    if (!entry || entry < st.entries || entry >= st.entries + st.count) {
        return kMaxDlcEntries;
    }
    return static_cast<size_t>(entry - st.entries);
}

const DlcEntry* find_entry_by_identifier_unlocked(const DlcState& st, const char* identifier) {
    if (!identifier || !identifier[0]) return nullptr;
    for (size_t i = 0; i < st.count; ++i) {
        const DlcEntry& entry = st.entries[i];
        if (std::strcmp(entry.contentId, identifier) == 0 ||
            std::strcmp(entry.label.data, identifier) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

const DlcEntry* find_entry_by_service_label_unlocked(const DlcState& st,
                                                     const SceNpServiceEntitlementLabel* label) {
    if (!label) return nullptr;
    for (size_t i = 0; i < st.count; ++i) {
        if (st.entries[i].hasServiceLabel &&
            std::strncmp(st.entries[i].serviceLabel.data,
                         label->data,
                         SCE_NP_SERVICE_ENTITLEMENT_LABEL_SIZE - 1u) == 0) {
            return &st.entries[i];
        }
    }
    return nullptr;
}

const DlcEntry* find_entry_by_request_unlocked(const DlcState& st, int64_t requestId);

bool copy_entry_by_label(const SceNpUnifiedEntitlementLabel* label, DlcEntry* out, size_t* indexOut = nullptr) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, label)) {
        if (out) *out = *entry;
        if (indexOut) *indexOut = static_cast<size_t>(entry - st.entries);
        return true;
    }
    return false;
}

bool copy_entry_by_identifier(const char* identifier, DlcEntry* out, size_t* indexOut = nullptr) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_identifier_unlocked(st, identifier)) {
        if (out) *out = *entry;
        if (indexOut) *indexOut = static_cast<size_t>(entry - st.entries);
        return true;
    }
    return false;
}

bool copy_entry_by_service_label(const SceNpServiceEntitlementLabel* label,
                                 DlcEntry* out,
                                 size_t* indexOut = nullptr) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_service_label_unlocked(st, label)) {
        if (out) *out = *entry;
        if (indexOut) *indexOut = static_cast<size_t>(entry - st.entries);
        return true;
    }
    return false;
}

bool copy_entry_by_request(int64_t requestId, DlcEntry* out) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_request_unlocked(st, requestId)) {
        if (out) *out = *entry;
        return true;
    }
    return false;
}

int32_t mount_entry_unlocked(DlcState& st, size_t index, SceAppContentMountPoint* mountPoint) {
    if (index >= st.count || !mountPoint) return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    const DlcEntry& entry = st.entries[index];
    if (!package_type_supports_mount(entry.packageType)) return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    if (!entry.mount.data[0]) return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    if (entry.status != SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED) {
        return SCE_APP_CONTENT_ERROR_ADDCONT_NO_IN_QUEUE;
    }
    if (st.mounted[index]) return SCE_APP_CONTENT_ERROR_BUSY;
    if (find_mounted_entry_by_mount_unlocked(st, &entry.mount)) return SCE_APP_CONTENT_ERROR_BUSY;
    if (st.mountedCount >= SCE_APP_CONTENT_ADDCONT_MOUNT_MAXNUM) {
        return SCE_APP_CONTENT_ERROR_MOUNT_FULL;
    }
    st.mounted[index] = true;
    ++st.mountedCount;
    *mountPoint = entry.mount;
    return SCE_OK;
}

bool has_fake_dlc() {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    return st.available;
}

bool entry_matches_package(const DlcEntry& entry, uint32_t packageType) {
    return packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE || entry.packageType == packageType;
}

struct UnifiedListRequest {
    uint32_t packageType{SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE};
    uint32_t sort{SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_NONE};
    uint32_t direction{SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_NONE};
    uint32_t offset{0};
    uint32_t limit{0};
};

bool unified_entry_before(const DlcEntry& lhs,
                          const DlcEntry& rhs,
                          const UnifiedListRequest& request) {
    if (request.sort != SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) {
        return false;
    }
    if (lhs.activeDate == rhs.activeDate) {
        return std::strcmp(lhs.label.data, rhs.label.data) < 0;
    }
    if (request.direction == SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_DESC) {
        return lhs.activeDate > rhs.activeDate;
    }
    return lhs.activeDate < rhs.activeDate;
}

void sort_unified_indices_unlocked(const DlcState& st,
                                   size_t* indices,
                                   uint32_t count,
                                   const UnifiedListRequest& request) {
    if (!indices || request.sort != SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) {
        return;
    }
    for (uint32_t i = 1; i < count; ++i) {
        const size_t value = indices[i];
        uint32_t pos = i;
        while (pos > 0 &&
               unified_entry_before(st.entries[value], st.entries[indices[pos - 1u]], request)) {
            indices[pos] = indices[pos - 1u];
            --pos;
        }
        indices[pos] = value;
    }
}

bool service_entry_before(const DlcEntry& lhs,
                          const DlcEntry& rhs,
                          const ServiceListRequest& request) {
    if (request.sort != SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) {
        return false;
    }
    if (lhs.activeDate == rhs.activeDate) {
        return std::strcmp(lhs.serviceLabel.data, rhs.serviceLabel.data) < 0;
    }
    if (request.direction == SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_DESC) {
        return lhs.activeDate > rhs.activeDate;
    }
    return lhs.activeDate < rhs.activeDate;
}

void sort_service_indices_unlocked(const DlcState& st,
                                   size_t* indices,
                                   uint32_t count,
                                   const ServiceListRequest& request) {
    if (!indices || request.sort != SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) {
        return;
    }
    for (uint32_t i = 1; i < count; ++i) {
        const size_t value = indices[i];
        uint32_t pos = i;
        while (pos > 0 &&
               service_entry_before(st.entries[value], st.entries[indices[pos - 1u]], request)) {
            indices[pos] = indices[pos - 1u];
            --pos;
        }
        indices[pos] = value;
    }
}

void fill_np_info(const DlcEntry& entry, SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->entitlementLabel = entry.label;
    info->packageType = entry.packageType;
    info->downloadStatus = entry.status;
}

void fill_np_raw_addcont_info(const DlcEntry& entry, void* info) {
    if (!info) return;
    constexpr size_t kRawInfoSize = 40u;
    std::memset(info, 0, kRawInfoSize);
    SceNpEntitlementAccessAddcontEntitlementInfo publicInfo{};
    fill_np_info(entry, &publicInfo);
    std::memcpy(info, &publicInfo, sizeof(publicInfo));
}

void fill_app_info(const DlcEntry& entry, SceAppContentAddcontInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->entitlementLabel = entry.label;
    info->status = entry.status;
}

void fill_unified_info(const DlcEntry& entry, SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->entitlementLabel = entry.label;
    info->activeDate.tick = entry.activeDate;
    info->inactiveDate.tick = entry.inactiveDate;
    info->entitlementType = SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_TYPE_UNIFIED;
    info->useCount = entry.useCount;
    info->useLimit = entry.useLimit;
    info->packageType = entry.packageType;
    info->activeFlag = entry.activeFlag;
}

void fill_service_info(const DlcEntry& entry, SceNpEntitlementAccessServiceEntitlementInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->entitlementLabel = entry.serviceLabel;
    info->activeDate.tick = entry.activeDate;
    info->inactiveDate.tick = entry.inactiveDate;
    info->entitlementType = SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_TYPE_SERVICE;
    info->useCount = entry.useCount;
    info->useLimit = entry.useLimit;
    info->activeFlag = entry.activeFlag;
    info->isConsumable = entry.consumable;
}

bool has_nul_terminator(const char* data, size_t size) {
    if (!data || size == 0) return false;
    return std::memchr(data, '\0', size) != nullptr;
}

bool valid_app_unified_label(const SceNpUnifiedEntitlementLabel* label) {
    return label && is_zeroed(label->padding, sizeof(label->padding));
}

bool valid_app_unified_label_for_sdk(const SceNpUnifiedEntitlementLabel* label, uint32_t minSdk) {
    if (!label) return false;
    if (compiled_sdk_version() < minSdk) {
        return true;
    }
    return is_zeroed(label->padding, sizeof(label->padding));
}

bool valid_np_unified_label(const SceNpUnifiedEntitlementLabel* label) {
    return valid_app_unified_label(label) &&
           has_nul_terminator(label->data, sizeof(label->data));
}

bool valid_np_service_label(const SceNpServiceEntitlementLabel* label) {
    return label &&
           label->data[0] &&
           is_zeroed(label->padding, sizeof(label->padding)) &&
           has_nul_terminator(label->data, sizeof(label->data));
}

bool valid_np_transaction_id(const SceNpEntitlementAccessTransactionId* transactionId) {
    return transactionId &&
           is_zeroed(transactionId->padding, sizeof(transactionId->padding)) &&
           has_nul_terminator(transactionId->transactionId, sizeof(transactionId->transactionId));
}

bool valid_app_boot_param_reserved(const SceAppContentBootParam* bootParam) {
    return bootParam &&
           is_zeroed(bootParam->reserved1, sizeof(bootParam->reserved1)) &&
           is_zeroed(bootParam->reserved2, sizeof(bootParam->reserved2));
}

bool valid_np_boot_param_reserved(const SceNpEntitlementAccessBootParam* bootParam) {
    return bootParam && is_zeroed(bootParam->reserved, sizeof(bootParam->reserved));
}

bool valid_patch_install_path(const char* path) {
    return path && std::strlen(path) <= 0xffu;
}

int32_t finish_title_token_poll(int32_t* pResult, int32_t* useLimit) {
    if (!pResult || !useLimit) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    *pResult = kNpEntitlementTitleTokenError;
    *useLimit = -1;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

bool valid_unified_list_package_type(uint32_t packageType) {
    return packageType <= SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSSUBS ||
           packageType == 8u;
}

bool valid_np_unified_list_request(const SceNpUnifiedEntitlementLabel* list,
                                   uint32_t listNum,
                                   const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
                                   const int64_t* requestId) {
    if (!param || !requestId) return false;
    if (param->size != sizeof(SceNpEntitlementAccessRequestEntitlementInfoListParam)) return false;
    if (param->entitlementType != SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_TYPE_UNIFIED) return false;
    if (listNum > SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE) return false;
    if (param->offset < 0 || param->offset > static_cast<int32_t>(kMaxDlcEntries)) return false;
    if (param->limit <= 0) return false;
    if (param->limit > static_cast<int32_t>(SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE)) return false;
    if (!list && listNum != 0) return false;
    if (param->sort > SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) return false;
    if (param->direction > SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_DESC) return false;
    if (param->packageType > kNpReferencePackageTypeMax) return false;
    return valid_unified_list_package_type(param->packageType);
}

bool valid_np_service_list_request(const SceNpServiceEntitlementLabel* list,
                                   uint32_t listNum,
                                   const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
                                   const int64_t* requestId) {
    if (!param || !requestId) return false;
    if (param->size != sizeof(SceNpEntitlementAccessRequestEntitlementInfoListParam)) return false;
    if (param->entitlementType != SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_TYPE_SERVICE) return false;
    if (listNum > SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE) return false;
    if (param->offset < 0 || param->offset > static_cast<int32_t>(kMaxDlcEntries)) return false;
    if (param->limit <= 0) return false;
    if (param->limit > static_cast<int32_t>(SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE)) return false;
    if (!list && listNum != 0) return false;
    for (uint32_t i = 0; i < listNum; ++i) {
        if (!valid_np_service_label(&list[i])) return false;
    }
    if (param->sort > SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE) return false;
    if (param->direction > SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_DESC) return false;
    return param->packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE;
}

int64_t synthetic_request_id_for_index(size_t index) {
    return kSyntheticEntryRequestBase + static_cast<int64_t>(index + 1u);
}

UnifiedListRequest make_unified_list_request(const SceNpEntitlementAccessRequestEntitlementInfoListParam* param) {
    UnifiedListRequest request{};
    if (!param) return request;
    request.packageType = param->packageType;
    request.sort = param->sort;
    request.direction = param->direction;
    request.offset = static_cast<uint32_t>(param->offset);
    request.limit = static_cast<uint32_t>(param->limit);
    return request;
}

int64_t synthetic_list_request_id(const UnifiedListRequest& request) {
    const uint64_t raw =
        (static_cast<uint64_t>(request.packageType) & 0x0fu) |
        ((static_cast<uint64_t>(request.sort) & 0x03u) << 4u) |
        ((static_cast<uint64_t>(request.direction) & 0x03u) << 6u) |
        ((static_cast<uint64_t>(request.offset) & 0x7ffu) << 8u) |
        ((static_cast<uint64_t>(request.limit) & 0x7fu) << 19u);
    return kSyntheticRequestBase + static_cast<int64_t>(raw);
}

ServiceListRequest make_service_list_request(const SceNpEntitlementAccessRequestEntitlementInfoListParam* param) {
    ServiceListRequest request{};
    if (!param) return request;
    request.sort = param->sort;
    request.direction = param->direction;
    request.offset = static_cast<uint32_t>(param->offset);
    request.limit = static_cast<uint32_t>(param->limit);
    return request;
}

bool decode_synthetic_list_request_id(int64_t requestId, UnifiedListRequest* request) {
    if (requestId < kSyntheticRequestBase || requestId >= kSyntheticEntryRequestBase) return false;
    const int64_t raw = requestId - kSyntheticRequestBase;
    if (raw < 0 || raw >= 0x10000000ll) return false;
    UnifiedListRequest decoded{};
    decoded.packageType = static_cast<uint32_t>(raw & 0x0fll);
    decoded.sort = static_cast<uint32_t>((raw >> 4u) & 0x03ll);
    decoded.direction = static_cast<uint32_t>((raw >> 6u) & 0x03ll);
    decoded.offset = static_cast<uint32_t>((raw >> 8u) & 0x7ffll);
    decoded.limit = static_cast<uint32_t>((raw >> 19u) & 0x7fll);
    if (!valid_unified_list_package_type(decoded.packageType) ||
        decoded.sort > SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_ACTIVE_DATE ||
        decoded.direction > SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_DESC ||
        decoded.offset > kMaxDlcEntries ||
        decoded.limit == 0 ||
        decoded.limit > SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE) {
        return false;
    }
    if (request) *request = decoded;
    return true;
}

int64_t next_service_list_request_id_unlocked(DlcState& st) {
    if (st.serviceListRequestNext == 0 || st.serviceListRequestNext >= 0x0fffffffULL) {
        st.serviceListRequestNext = 1;
    }
    return kSyntheticServiceListRequestBase + static_cast<int64_t>(st.serviceListRequestNext++);
}

ServiceListPendingRequest* remember_service_list_request_unlocked(
    DlcState& st,
    const ServiceListRequest& request,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    int64_t* requestId) {
    if (!requestId) return nullptr;
    const int64_t id = next_service_list_request_id_unlocked(st);
    const uint64_t raw = static_cast<uint64_t>(id - kSyntheticServiceListRequestBase - 1);
    ServiceListPendingRequest& slot = st.serviceListRequests[raw % kMaxServiceListRequests];
    slot = ServiceListPendingRequest{};
    slot.used = true;
    slot.requestId = id;
    slot.request = request;
    slot.filterCount = listNum;
    for (uint32_t i = 0; i < listNum; ++i) {
        slot.filters[i] = list[i];
    }
    *requestId = id;
    return &slot;
}

ServiceListPendingRequest* find_service_list_request_unlocked(DlcState& st, int64_t requestId) {
    if (requestId < kSyntheticServiceListRequestBase) return nullptr;
    for (size_t i = 0; i < kMaxServiceListRequests; ++i) {
        if (st.serviceListRequests[i].used && st.serviceListRequests[i].requestId == requestId) {
            return &st.serviceListRequests[i];
        }
    }
    return nullptr;
}

void clear_service_list_request_unlocked(DlcState& st, int64_t requestId) {
    if (ServiceListPendingRequest* request = find_service_list_request_unlocked(st, requestId)) {
        *request = ServiceListPendingRequest{};
    }
}

bool service_list_request_matches(const ServiceListPendingRequest& request, const DlcEntry& entry) {
    if (!entry.hasServiceLabel) return false;
    if (request.filterCount == 0) return true;
    for (uint32_t i = 0; i < request.filterCount; ++i) {
        if (std::strcmp(request.filters[i].data, entry.serviceLabel.data) == 0) {
            return true;
        }
    }
    return false;
}

bool is_synthetic_request_id(int64_t requestId) {
    return requestId >= kSyntheticRequestBase;
}

const DlcEntry* find_entry_by_request_unlocked(const DlcState& st, int64_t requestId) {
    if (requestId <= kSyntheticEntryRequestBase) return nullptr;
    const uint64_t indexPlusOne = static_cast<uint64_t>(requestId - kSyntheticEntryRequestBase);
    if (indexPlusOne == 0 || indexPlusOne > st.count) return nullptr;
    return &st.entries[indexPlusOne - 1u];
}

ConsumedTransaction* find_consumed_transaction_unlocked(DlcState& st,
                                                        const SceNpEntitlementAccessTransactionId* transactionId) {
    if (!transactionId) return nullptr;
    for (size_t i = 0; i < kMaxConsumeTransactions; ++i) {
        if (st.consumed[i].used &&
            std::strcmp(st.consumed[i].transactionId, transactionId->transactionId) == 0) {
            return &st.consumed[i];
        }
    }
    return nullptr;
}

void remember_consumed_transaction_unlocked(DlcState& st,
                                            size_t entryIndex,
                                            const SceNpEntitlementAccessTransactionId* transactionId,
                                            int32_t useCount) {
    if (!transactionId) return;
    ConsumedTransaction& slot = st.consumed[st.consumedNext % kMaxConsumeTransactions];
    slot = ConsumedTransaction{};
    slot.used = true;
    slot.entryIndex = entryIndex;
    slot.useCount = useCount;
    strlcpy(slot.transactionId, transactionId->transactionId, sizeof(slot.transactionId));
    st.consumedNext = (st.consumedNext + 1u) % kMaxConsumeTransactions;
}

int32_t consume_entry_unlocked(DlcState& st,
                               size_t entryIndex,
                               const SceNpEntitlementAccessTransactionId* transactionId,
                               int32_t useCount,
                               int64_t* requestId) {
    if (entryIndex >= st.count || !transactionId || !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    }
    if (useCount <= 0) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;

    if (ConsumedTransaction* consumed = find_consumed_transaction_unlocked(st, transactionId)) {
        if (consumed->entryIndex != entryIndex || consumed->useCount != useCount) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
        }
        *requestId = synthetic_request_id_for_index(entryIndex);
        return SCE_OK;
    }

    DlcEntry& entry = st.entries[entryIndex];
    if (!entry.activeFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    if (!entry.consumable) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    if (entry.useLimit < 0 ||
        useCount > entry.useLimit ||
        entry.useCount > entry.useLimit - useCount) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    }
    entry.useCount += useCount;

    remember_consumed_transaction_unlocked(st, entryIndex, transactionId, useCount);
    *requestId = synthetic_request_id_for_index(entryIndex);
    return SCE_OK;
}

} // namespace

extern "C" {

void dlcEmu_prewarmAppRpc(void) {
    DlcLockGuard lock(g_appRpc.mutex);
    (void)ensure_app_rpc_unlocked();
}

void dlcEmu_prewarmNpRpc(void) {
    DlcLockGuard lock(g_npRpc.mutex);
    (void)ensure_np_rpc_unlocked();
}

int32_t dlcEmu_sceAppContentInitialize(const SceAppContentInitParam* initParam,
                                        SceAppContentBootParam* bootParam) {
    if (!initParam || !bootParam) return SCE_APP_CONTENT_ERROR_PARAMETER;
    if (!is_zeroed(initParam->reserved, sizeof(initParam->reserved)) ||
        !valid_app_boot_param_reserved(bootParam)) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }
    std::memset(bootParam, 0, sizeof(*bootParam));
    return app_rpc_initialize(initParam, bootParam);
}

int32_t dlcEmu_sceAppContentAppParamGetInt(SceAppContentAppParamId paramId, int32_t* value) {
    if (!value) return SCE_APP_CONTENT_ERROR_PARAMETER;
    if (paramId == SCE_APP_CONTENT_APPPARAM_ID_SKU_FLAG) {
        *value = SCE_APP_CONTENT_APPPARAM_SKU_FLAG_FULL;
        return SCE_OK;
    }
    return app_rpc_control_output(kAppContentRpcCommandAppParamGetInt,
                                  static_cast<uint32_t>(paramId),
                                  value,
                                  sizeof(*value));
}

// Old AppContent list API: report configured dlc_emu.ini entries as installed.
int32_t dlcEmu_sceAppContentGetAddcontInfoList(SceNpServiceLabel serviceLabel,
                                                     SceAppContentAddcontInfo* list,
                                                     uint32_t listNum,
                                                     uint32_t* hitNum) {
    (void)serviceLabel;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (!st.available) return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_APP_CONTENT_ERROR_PARAMETER;
        *hitNum = static_cast<uint32_t>(st.count);
        dlc_logf("dlc.app_addcont_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        fill_app_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = static_cast<uint32_t>(st.count);
    dlc_logf("dlc.app_addcont_list.fake mode=list fake=%u written=%u",
                              static_cast<unsigned>(st.count),
                              written);
    return SCE_OK;
}

// Old AppContent single-info API: configured labels are installed.
int32_t dlcEmu_sceAppContentGetAddcontInfo(SceNpServiceLabel serviceLabel,
                                                 const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                 SceAppContentAddcontInfo* info) {
    (void)serviceLabel;
    if (!valid_app_unified_label(entitlementLabel) || !info) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        fill_app_info(entry, info);
        dlc_logf("dlc.app_addcont_info.fake label=%s status=%u",
                                  entry.label.data,
                                  entry.status);
        return SCE_OK;
    }
    return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

// Runtime IRO export: accept either full contentId or 16-byte entitlement label
// for fake entries from dlc_emu.ini.
int32_t dlcEmu_sceAppContentGetAddcontInfoByEntitlementId(
    const char* entitlementId,
    SceAppContentAddcontInfo* info) {
    if (!entitlementId || !info) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_identifier(entitlementId, &entry)) {
        fill_app_info(entry, info);
        dlc_logf("dlc.app_addcont_info.fake entitlementId=%s label=%s status=%u",
                                  entitlementId,
                                  entry.label.data,
                                  entry.status);
        return SCE_OK;
    }
    return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

// Runtime IRO list export: it is another installed addcont list surface.
int32_t dlcEmu_sceAppContentGetAddcontInfoListByIroTag(
    uint32_t iroTag,
    SceAppContentAddcontInfo* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    (void)iroTag;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (!st.available) return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_APP_CONTENT_ERROR_PARAMETER;
        *hitNum = static_cast<uint32_t>(st.count);
        dlc_logf("dlc.app_iro_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        fill_app_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = static_cast<uint32_t>(st.count);
    dlc_logf("dlc.app_iro_list.fake mode=list fake=%u written=%u",
                              static_cast<unsigned>(st.count),
                              written);
    return SCE_OK;
}

// Old AppContent entitlement-key API: return explicit/default key for fake DLC.
int32_t dlcEmu_sceAppContentGetEntitlementKey(SceNpServiceLabel serviceLabel,
                                                    const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                    SceAppContentEntitlementKey* key) {
    (void)serviceLabel;
    if (!valid_app_unified_label(entitlementLabel) || !key) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        std::memcpy(key->data, entry.key, sizeof(entry.key));
        return SCE_OK;
    }
    return SCE_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

// Fake DLC mount state mirrors the SDK-visible addcont state even though the
// returned path is a pre-existing /app0 folder.
int32_t dlcEmu_sceAppContentAddcontMount(SceNpServiceLabel serviceLabel,
                                               const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                               SceAppContentMountPoint* mountPoint) {
    (void)serviceLabel;
    if (!valid_app_unified_label(entitlementLabel) || !mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel);
    const size_t index = entry_index_unlocked(st, entry);
    const int32_t rc = mount_entry_unlocked(st, index, mountPoint);
    if (rc == SCE_OK) {
        dlc_logf("dlc.mount.fake label=%s mount=%s active=%u",
                 entry->label.data,
                 entry->mount.data,
                 static_cast<unsigned>(st.mountedCount));
    }
    return rc;
}

// Entitlement-id mount accepts the full contentId or label for fake DLC and
// returns the pre-existing configured mount point while tracking mounted state.
int32_t dlcEmu_sceAppContentAddcontMountByEntitlemetId(const char* entitlementId,
                                                             SceAppContentMountPoint* mountPoint) {
    if (!entitlementId || !mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    const DlcEntry* entry = find_entry_by_identifier_unlocked(st, entitlementId);
    const size_t index = entry_index_unlocked(st, entry);
    const int32_t rc = mount_entry_unlocked(st, index, mountPoint);
    if (rc == SCE_OK) {
        dlc_logf("dlc.mount.fake entitlementId=%s mount=%s active=%u",
                 entitlementId,
                 entry->mount.data,
                 static_cast<unsigned>(st.mountedCount));
    }
    return rc;
}

// Fake add-on mount points are ordinary /app0 folders, but unmount still follows
// SDK-visible mounted/not-mounted state.
int32_t dlcEmu_sceAppContentAddcontUnmount(const SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    const DlcEntry* entry = find_mounted_entry_by_mount_unlocked(st, mountPoint);
    const size_t index = entry_index_unlocked(st, entry);
    if (index >= st.count || !st.mounted[index]) {
        return SCE_APP_CONTENT_ERROR_NOT_MOUNTED;
    }
    st.mounted[index] = false;
    if (st.mountedCount != 0) --st.mountedCount;
    dlc_logf("dlc.unmount.fake mount=%s active=%u",
             mountPoint->data,
             static_cast<unsigned>(st.mountedCount));
    return SCE_OK;
}

// Delete is deliberately no-op only for fake DLC so /app0/dlcNN remains unchanged.
int32_t dlcEmu_sceAppContentAddcontDelete(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    (void)serviceLabel;
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        return SCE_OK;
    }
    return SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

// Download queueing is not emulated. Delegate it to the native AppContent
// service instead of pretending fake DLC entered a download queue.
int32_t dlcEmu_sceAppContentAddcontEnqueueDownload(SceNpServiceLabel serviceLabel,
                                                         const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_entitlement_command(kAppContentRpcCommandAddcontEnqueueDownload,
                                       serviceLabel,
                                       entitlementLabel);
}

// Same boundary as the public enqueue path: native service owns downloads.
int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadSp(SceNpServiceLabel serviceLabel,
                                                           const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_entitlement_command(kAppContentRpcCommandAddcontEnqueueDownloadSp,
                                       serviceLabel,
                                       entitlementLabel);
}

// Entitlement-id download queueing is pass-through for the native service.
int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadByEntitlemetId(const char* entitlementId) {
    if (!entitlementId) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_entitlement_id_command(kAppContentRpcCommandAddcontEnqueueDownloadByEntitlementId,
                                          entitlementId);
}

// Shrink mutates add-on storage and is therefore outside the fake-DLC overlay.
int32_t dlcEmu_sceAppContentAddcontShrink(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_entitlement_command(kAppContentRpcCommandAddcontShrink,
                                       serviceLabel,
                                       entitlementLabel);
}

int32_t dlcEmu_sceAppContentRaw_xZo2_418Wdo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label_for_sdk(entitlementLabel, 0x1500000u)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    if (copy_entry_by_label(entitlementLabel, nullptr)) {
        return SCE_OK;
    }
    return app_rpc_entitlement_command(kAppContentRpcCommandRawXZo2, serviceLabel, entitlementLabel);
}

int32_t dlcEmu_sceAppContentRaw_UO_gD_XFyGE(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandRawUO, mountPoint);
}

int32_t dlcEmu_sceAppContentRaw_MFUAprB41fA(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandRawMFU, mountPoint);
}

int32_t dlcEmu_sceAppContentRaw_y8meQn_Qy5c(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    uint32_t* playableStatus) {
    if (!valid_app_unified_label_for_sdk(entitlementLabel, 0x3500000u) || !playableStatus) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }
    if (copy_entry_by_label(entitlementLabel, nullptr)) {
        *playableStatus = 1u;
        return SCE_OK;
    }
    return app_rpc_playable_status(serviceLabel, entitlementLabel, playableStatus);
}

int32_t dlcEmu_sceAppContentRaw_1saJukIkcKw(uint32_t* gameTrialsFlag) {
    if (!gameTrialsFlag) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_u32_output(kAppContentRpcCommandRaw1sa, gameTrialsFlag);
}

int32_t dlcEmu_sceAppContentRaw_SWVxsi_ZBlw(const void* input, void* output) {
    if (!input || !output) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_blob16_output1(kAppContentRpcCommandRawSWV, input, output);
}

int32_t dlcEmu_sceAppContentTemporaryDataUnmount(const SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_operation(kAppContentRpcCommandTemporaryDataUnmount, mountPoint);
}

int32_t dlcEmu_sceAppContentTemporaryDataFormat(const SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_operation(kAppContentRpcCommandTemporaryDataFormat, mountPoint);
}

int32_t dlcEmu_sceAppContentTemporaryDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb) {
    if (!mountPoint || !availableSpaceKb) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_query(kAppContentRpcCommandDataGetAvailableSpaceKb,
                               1u,
                               mountPoint,
                               availableSpaceKb);
}

int32_t dlcEmu_sceAppContentDownloadDataFormat(const SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_operation(kAppContentRpcCommandDownloadDataFormat, mountPoint);
}

int32_t dlcEmu_sceAppContentDownloadDataGetAvailableSpaceKb(
    const SceAppContentMountPoint* mountPoint,
    size_t* availableSpaceKb) {
    if (!mountPoint || !availableSpaceKb) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_query(kAppContentRpcCommandDataGetAvailableSpaceKb,
                               0u,
                               mountPoint,
                               availableSpaceKb);
}

int32_t dlcEmu_sceAppContentDownloadDataGetBlockSize(
    const SceAppContentMountPoint* mountPoint,
    size_t* blockSize) {
    if (!mountPoint || !blockSize) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount_query(kAppContentRpcCommandDownloadDataGetBlockSize,
                               0u,
                               mountPoint,
                               blockSize);
}

int32_t dlcEmu_sceAppContentDownload0Shrink(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload0Shrink, mountPoint);
}

int32_t dlcEmu_sceAppContentDownload0Expand(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload0Expand, mountPoint);
}

int32_t dlcEmu_sceAppContentDownload1Shrink(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload1Shrink, mountPoint);
}

int32_t dlcEmu_sceAppContentDownload1Expand(const SceAppContentMountPoint* mountPoint) {
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload1Expand, mountPoint);
}

// Download progress belongs to the native download service, not to the fake
// entitlement overlay.
int32_t dlcEmu_sceAppContentGetAddcontDownloadProgress(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceAppContentAddcontDownloadProgress* progress) {
    if (!valid_app_unified_label(entitlementLabel) || !progress) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_entitlement_progress(kAppContentRpcCommandGetAddcontDownloadProgress,
                                        serviceLabel,
                                        entitlementLabel,
                                        progress);
}

int32_t dlcEmu_sceAppContentGetPftFlag(SceAppContentPftFlag* pftFlag) {
    if (!pftFlag) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_u32_output(kAppContentRpcCommandGetPftFlag, reinterpret_cast<uint32_t*>(pftFlag));
}

int32_t dlcEmu_sceAppContentTemporaryDataMount(SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount(kAppContentRpcCommandTemporaryDataMount,
                         SCE_APP_CONTENT_TEMPORARY_DATA_OPTION_FORMAT,
                         mountPoint);
}

int32_t dlcEmu_sceAppContentTemporaryDataMount2(SceAppContentTemporaryDataOption option,
                                                      SceAppContentMountPoint* mountPoint) {
    if (!mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_mount(kAppContentRpcCommandTemporaryDataMount, option, mountPoint);
}

int32_t dlcEmu_sceAppContentGetRegion(char* region) {
    if (!region) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_u32_output(kAppContentRpcCommandGetRegion, reinterpret_cast<uint32_t*>(region));
}

int32_t dlcEmu_sceAppContentRequestPatchInstall(const char* path) {
    if (!valid_patch_install_path(path)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_string_input(kAppContentRpcCommandRequestPatchInstall, path);
}

int32_t dlcEmu_sceAppContentAppParamGetString(SceAppContentAppParamId paramId,
                                                    char* value,
                                                    size_t valueSize) {
    if (!value) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_app_param_string(paramId, value, valueSize);
}

int32_t dlcEmu_sceAppContentGetDownloadedStoreCountry(char* country) {
    if (!country) return SCE_APP_CONTENT_ERROR_PARAMETER;
    return app_rpc_u32_output(kAppContentRpcCommandGetDownloadedStoreCountry,
                              reinterpret_cast<uint32_t*>(country));
}

// Force full SKU so trial gating does not hide fake DLC.
int32_t dlcEmu_sceNpEntitlementAccessGetSkuFlag(SceNpEntitlementAccessSkuFlag* skuFlag) {
    if (!skuFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    *skuFlag = SCE_NP_ENTITLEMENT_ACCESS_SKU_FLAG_FULL;
    return SCE_OK;
}

int32_t dlcEmu_sceNpEntitlementAccessGetGameTrialsFlag(
    SceNpEntitlementAccessGameTrialsFlag* gameTrialsFlag) {
    if (!gameTrialsFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    return np_rpc_u32_output(kNpRpcCommandGameTrials, reinterpret_cast<uint32_t*>(gameTrialsFlag));
}

int32_t dlcEmu_sceNpEntitlementAccessInitialize(
    const SceNpEntitlementAccessInitParam* initParam,
    SceNpEntitlementAccessBootParam* bootParam) {
    if (!initParam || !bootParam) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    if (!is_zeroed(initParam->reserved, sizeof(initParam->reserved)) ||
        !valid_np_boot_param_reserved(bootParam)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    std::memset(bootParam, 0, sizeof(*bootParam));
    return SCE_OK;
}

// Main PS5 entitlement list API: report configured dlc_emu.ini entries.
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoList(
    SceNpServiceLabel serviceLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    (void)serviceLabel;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (!st.available) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
        *hitNum = static_cast<uint32_t>(st.count);
        dlc_logf("dlc.np_addcont_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        fill_np_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = static_cast<uint32_t>(st.count);
    dlc_logf("dlc.np_addcont_list.fake mode=list fake=%u written=%u",
                              static_cast<unsigned>(st.count),
                              written);
    return SCE_OK;
}

// Main PS5 single entitlement API: configured entries are installed.
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        fill_np_info(entry, info);
        dlc_logf("dlc.np_addcont_info.fake label=%s packageType=%s status=%u",
                                  entry.label.data,
                                  package_type_name(entry.packageType),
                                  entry.status);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Runtime-only individual addcont query: surface fake installed DLC even though
// this symbol is omitted from the SDK 10 weak stub.
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoIndividual(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        fill_np_info(entry, info);
        dlc_logf("dlc.np_addcont_info_individual.fake label=%s packageType=%s status=%u",
                                  entry.label.data,
                                  package_type_name(entry.packageType),
                                  entry.status);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessRaw_l0MTQHIcH3M(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    void* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    (void)serviceLabel;
    if (userId == static_cast<SceUserServiceUserId>(-1) || !hitNum) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;

    DlcState& st = ensure_loaded();
    {
        DlcLockGuard lock(st.mutex);
        if (st.available) {
            const uint32_t total = static_cast<uint32_t>(st.count);
            if (list && listNum != 0) {
                auto* dst = static_cast<unsigned char*>(list);
                uint32_t written = 0;
                for (size_t i = 0; i < st.count && written < listNum; ++i) {
                    fill_np_raw_addcont_info(st.entries[i], dst + static_cast<size_t>(written) * 40u);
                    ++written;
                }
            }
            *hitNum = total;
            return SCE_OK;
        }
    }
    *hitNum = 0;
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessRaw_eDXKe9FndlE(
    SceNpEntitlementAccessGameTrialsFlag* pftFlag) {
    if (!pftFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    return np_rpc_u32_output(kNpRpcCommandRawEDX, reinterpret_cast<uint32_t*>(pftFlag));
}

// Entitlement key API: fake DLC returns explicit config key or default index+1024
// key encoded as little-endian uint64.
int32_t dlcEmu_sceNpEntitlementAccessGetEntitlementKey(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessEntitlementKey* key) {
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) || !key) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry)) {
        std::memcpy(key->data, entry.key, sizeof(entry.key));
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Transaction ids are synthetic but shaped like the SDK sample format.
int32_t dlcEmu_sceNpEntitlementAccessGenerateTransactionId(
    SceNpEntitlementAccessTransactionId* transactionId) {
    if (!transactionId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    std::memset(transactionId, 0, sizeof(*transactionId));
    const uint64_t value = g_transactionCounter.fetch_add(1u, std::memory_order_relaxed) + 1u;
    std::snprintf(transactionId->transactionId,
                  sizeof(transactionId->transactionId),
                  "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(value & 0xffffffffffffull));
    return SCE_OK;
}

// Fake unified entitlement consumption is accepted without touching external
// services or DLC files.
int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) ||
        !valid_np_transaction_id(transactionId) ||
        !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    size_t index = 0;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel)) {
        index = static_cast<size_t>(entry - st.entries);
        const int32_t rc = consume_entry_unlocked(st, index, transactionId, useCount, requestId);
        if (rc != SCE_OK) return rc;
        dlc_logf("dlc.unified_info.request.fake label=%s",
                                  entitlementLabel->data);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    return dlcEmu_sceNpEntitlementAccessRequestConsumeUnifiedEntitlement(
        userId,
        serviceLabel,
        entitlementLabel,
        transactionId,
        useCount,
        requestId);
}

int32_t dlcEmu_sceNpEntitlementAccessRequestConsumeServiceEntitlement(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    const SceNpEntitlementAccessTransactionId* transactionId,
    int32_t useCount,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_service_label(entitlementLabel) ||
        !valid_np_transaction_id(transactionId) ||
        !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_service_label_unlocked(st, entitlementLabel)) {
        const size_t index = static_cast<size_t>(entry - st.entries);
        const int32_t rc = consume_entry_unlocked(st, index, transactionId, useCount, requestId);
        if (rc == SCE_OK) {
            dlc_logf("dlc.service_consume.request.fake serviceLabel=%s useCount=%d",
                     entitlementLabel->data,
                     useCount);
        }
        return rc;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Poll fake unified-consume requests as completed.
int32_t dlcEmu_sceNpEntitlementAccessPollConsumeEntitlement(
    int64_t requestId,
    int32_t* pResult,
    int32_t* useLimit) {
    if (!pResult || !useLimit) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_request(requestId, &entry)) {
        *pResult = SCE_OK;
        *useLimit = entry.useLimit;
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    return finish_title_token_poll(pResult, useLimit);
}

// Request fake unified entitlement info and remember the label in the request id.
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    size_t index = 0;
    if (copy_entry_by_label(entitlementLabel, nullptr, &index)) {
        *requestId = synthetic_request_id_for_index(index);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Poll fake unified entitlement info as active/installed.
int32_t dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    if (!pResult || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_request(requestId, &entry)) {
        *pResult = SCE_OK;
        fill_unified_info(entry, info);
        dlc_logf("dlc.unified_info.poll.fake label=%s packageType=%s",
                                  entry.label.data,
                                  package_type_name(entry.packageType));
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    *pResult = kNpEntitlementTitleTokenError;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

// Request-list API succeeds for fake DLC; poll returns the active fake list.
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_unified_list_request(list, listNum, param, requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    if (has_fake_dlc()) {
        const UnifiedListRequest request = make_unified_list_request(param);
        *requestId = synthetic_list_request_id(request);
        dlc_logf("dlc.unified_list.request.fake packageType=%s offset=%u limit=%u sort=%u direction=%u",
                                  package_type_name(request.packageType),
                                  request.offset,
                                  request.limit,
                                  request.sort,
                                  request.direction);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Poll-list API reports all dlc_emu.ini entries as active unified
// entitlements.
int32_t dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset) {
    if (!pResult || !list || !nextOffset || !previousOffset) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    if (listNum == 0 || listNum > SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    UnifiedListRequest request{};
    if (decode_synthetic_list_request_id(requestId, &request)) {
        DlcState& st = ensure_loaded();
        DlcLockGuard lock(st.mutex);
        if (st.available) {
            size_t indices[kMaxDlcEntries]{};
            uint32_t written = 0;
            uint32_t total = 0;
            for (size_t i = 0; i < st.count; ++i) {
                if (!entry_matches_package(st.entries[i], request.packageType)) continue;
                indices[total++] = i;
            }
            sort_unified_indices_unlocked(st, indices, total, request);

            const uint32_t pageLimit = request.limit < listNum ? request.limit : listNum;
            const uint32_t start = request.offset < total ? request.offset : total;
            const uint32_t available = total - start;
            const uint32_t toWrite = pageLimit < available ? pageLimit : available;
            for (uint32_t i = 0; i < toWrite; ++i) {
                fill_unified_info(st.entries[indices[start + i]], &list[written++]);
            }
            if (hitNum) *hitNum = written;
            const uint32_t next = start + toWrite;
            *nextOffset = next < total ? static_cast<int32_t>(next) : SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
            if (start == 0) {
                *previousOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
            } else if (pageLimit == 0 || start <= pageLimit) {
                *previousOffset = 0;
            } else {
                *previousOffset = static_cast<int32_t>(start - pageLimit);
            }
            *pResult = SCE_OK;
            dlc_logf("dlc.unified_list.poll.fake packageType=%s offset=%u limit=%u total=%u written=%u next=%d previous=%d",
                                      package_type_name(request.packageType),
                                      request.offset,
                                      request.limit,
                                      total,
                                      written,
                                      *nextOffset,
                                      *previousOffset);
            return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
        }
    }
    *pResult = kNpEntitlementTitleTokenError;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestConsumableEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_unified_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel)) {
        if (!entry->consumable) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        *requestId = synthetic_request_id_for_index(static_cast<size_t>(entry - st.entries));
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessPollConsumableEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    return dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfo(requestId, pResult, info);
}

int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_service_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    size_t index = 0;
    if (copy_entry_by_service_label(entitlementLabel, nullptr, &index)) {
        *requestId = synthetic_request_id_for_index(index);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* info) {
    if (!pResult || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_request(requestId, &entry) && entry.hasServiceLabel) {
        *pResult = SCE_OK;
        fill_service_info(entry, info);
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    if (!is_synthetic_request_id(requestId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    *pResult = kNpEntitlementTitleTokenError;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    (void)userId;
    (void)serviceLabel;
    if (!valid_np_service_list_request(list, listNum, param, requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    const ServiceListRequest request = make_service_list_request(param);
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    remember_service_list_request_unlocked(st, request, list, listNum, requestId);
    return SCE_OK;
}

int32_t dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfoList(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum,
    int32_t* nextOffset,
    int32_t* previousOffset) {
    if (!pResult || !list || !nextOffset || !previousOffset) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    if (listNum == 0 || listNum > SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    ServiceListPendingRequest pending{};
    {
        DlcState& st = ensure_loaded();
        DlcLockGuard lock(st.mutex);
        ServiceListPendingRequest* stored = find_service_list_request_unlocked(st, requestId);
        if (!stored) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
        }
        pending = *stored;
    }

    if (!pending.used) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }

    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    size_t indices[kMaxDlcEntries]{};
    uint32_t total = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (!service_list_request_matches(pending, st.entries[i])) continue;
        indices[total++] = i;
    }
    sort_service_indices_unlocked(st, indices, total, pending.request);

    uint32_t written = 0;
    const uint32_t pageLimit = pending.request.limit < listNum ? pending.request.limit : listNum;
    const uint32_t start = pending.request.offset < total ? pending.request.offset : total;
    const uint32_t available = total - start;
    const uint32_t toWrite = pageLimit < available ? pageLimit : available;
    for (uint32_t i = 0; i < toWrite; ++i) {
        fill_service_info(st.entries[indices[start + i]], &list[written++]);
    }
    if (hitNum) *hitNum = written;
    const uint32_t next = start + toWrite;
    *nextOffset = next < total ? static_cast<int32_t>(next) : SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
    if (start == 0) {
        *previousOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
    } else if (start <= pageLimit) {
        *previousOffset = 0;
    } else {
        *previousOffset = static_cast<int32_t>(start - pageLimit);
    }
    *pResult = SCE_OK;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

int32_t dlcEmu_sceNpEntitlementAccessDeleteRequest(int64_t requestId) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    clear_service_list_request_unlocked(st, requestId);
    return SCE_OK;
}

int32_t dlcEmu_sceNpEntitlementAccessAbortRequest(int64_t requestId) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    clear_service_list_request_unlocked(st, requestId);
    return SCE_OK;
}

} // extern "C"
