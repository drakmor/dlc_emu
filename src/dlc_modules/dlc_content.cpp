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

constexpr size_t kMaxDlcEntries = SCE_DLC_EMU_CONTENTIDS_MAX;
constexpr size_t kMaxConsumeTransactions = 256u;
constexpr size_t kMaxEntryRequests = 64u;
constexpr size_t kMaxUnifiedListRequests = 32u;
constexpr size_t kMaxServiceListRequests = 32u;
constexpr size_t kMaxGameUpdateRequests = 32u;
constexpr size_t kMaxIniFileBytes = 32u * 1024u;
constexpr size_t kIniBufferBytes = kMaxIniFileBytes + 1u;
constexpr uint64_t kDefaultEntitlementKeyBase = 1024u;
constexpr int64_t kSyntheticRequestBase = 0x444c43000000ll;
constexpr int64_t kSyntheticEntryRequestBase = kSyntheticRequestBase + 0x10000000ll;
constexpr int64_t kSyntheticServiceListRequestBase = kSyntheticRequestBase + 0x20000000ll;
constexpr uint32_t kNpReferencePackageTypeMax = 8u;
static_assert(sizeof(SceNpEntitlementAccessAddcontEntitlementInfo) == 28u);
static_assert(sizeof(SceGameUpdateCheckParam) == 48u);
static_assert(sizeof(SceGameUpdateCheckResult) == 48u);
static_assert(sizeof(SceGameUpdateAddcontVersionInfo) == 48u);
constexpr uint32_t kAppContentRpcCommandInitialize = 0x20000u;
constexpr uint32_t kAppContentRpcCommandAppParamGetInt = 0x20001u;
constexpr uint32_t kAppContentRpcCommandAppParamGetString = 0x20002u;
constexpr uint32_t kAppContentRpcCommandTemporaryDataMount = 0x2000bu;
constexpr uint32_t kAppContentRpcCommandTemporaryDataUnmount = 0x2000cu;
constexpr uint32_t kAppContentRpcCommandTemporaryDataFormat = 0x2000du;
constexpr uint32_t kAppContentRpcCommandDownloadDataFormat = 0x2000eu;
constexpr uint32_t kAppContentRpcCommandDataGetAvailableSpaceKb = 0x2000fu;
constexpr uint32_t kAppContentRpcCommandDownloadDataGetBlockSize = 0x20010u;
constexpr uint32_t kAppContentRpcCommandGetRegion = 0x20012u;
constexpr uint32_t kAppContentRpcCommandRequestPatchInstall = 0x20013u;
constexpr uint32_t kAppContentRpcCommandGetDownloadedStoreCountry = 0x20014u;
constexpr uint32_t kAppContentRpcCommandDownload0Shrink = 0x2001fu;
constexpr uint32_t kAppContentRpcCommandDownload0Expand = 0x20020u;
constexpr uint32_t kAppContentRpcCommandDownload1Shrink = 0x20021u;
constexpr uint32_t kAppContentRpcCommandDownload1Expand = 0x20022u;
constexpr uint32_t kAppContentRpcCommandDownload2Shrink = 0x20023u;
constexpr uint32_t kAppContentRpcCommandDownload2Expand = 0x20024u;

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
    uint32_t npServiceLabel{0};
    int32_t useCount{0};
    int32_t useLimit{1};
    uint64_t activeDate{0};
    uint64_t inactiveDate{UINT64_MAX};
    bool activeFlag{true};
    bool hasServiceLabel{false};
    bool consumable{false};
    bool addcontVisible{false};
    bool unifiedVisible{true};
    bool serviceVisible{false};
    bool mountable{false};
    bool npServiceLabelSet{false};
};

struct ParsedDlcEntry {
    char contentId[128]{};
    char label[128]{};
    char serviceLabel[128]{};
    char mountPoint[128]{};
    char keyHex[128]{};
    char invalidField[32]{};
    uint32_t downloadStatus{SCE_NP_ENTITLEMENT_ACCESS_DOWNLOAD_STATUS_INSTALLED};
    uint32_t npServiceLabel{0};
    int32_t useCount{0};
    int32_t useLimit{1};
    uint64_t activeDate{0};
    uint64_t inactiveDate{UINT64_MAX};
    bool activeFlag{true};
    bool consumable{false};
    bool consumableSet{false};
    bool addcontVisible{false};
    bool addcontVisibleSet{false};
    bool unifiedVisible{true};
    bool unifiedVisibleSet{false};
    bool serviceVisible{false};
    bool serviceVisibleSet{false};
    bool mountable{false};
    bool mountableSet{false};
    bool npServiceLabelSet{false};
    bool valid{true};
};

struct ConsumedTransaction {
    bool used{false};
    size_t entryIndex{0};
    char transactionId[SCE_NP_ENTITLEMENT_ACCESS_TRANSACTION_ID_MAX_SIZE]{};
    int32_t useCount{0};
    int32_t resultUseLimit{0};
};

enum class EntryRequestType : uint8_t {
    None,
    UnifiedInfo,
    ConsumableInfo,
    ServiceInfo,
    ConsumeUnified,
    ConsumeService,
};

struct EntryPendingRequest {
    bool used{false};
    bool aborted{false};
    int64_t requestId{0};
    EntryRequestType type{EntryRequestType::None};
    size_t entryIndex{0};
    int32_t resultUseLimit{0};
    DlcEntry snapshot{};
};

struct ServiceListRequest {
    uint32_t sort{SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_NONE};
    uint32_t direction{SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_NONE};
    uint32_t offset{0};
    uint32_t limit{0};
};

struct UnifiedListRequest {
    uint32_t packageType{SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE};
    uint32_t sort{SCE_NP_ENTITLEMENT_ACCESS_SORT_TYPE_NONE};
    uint32_t direction{SCE_NP_ENTITLEMENT_ACCESS_DIRECTION_TYPE_NONE};
    uint32_t offset{0};
    uint32_t limit{0};
};

struct ServiceListPendingRequest {
    bool used{false};
    bool aborted{false};
    int64_t requestId{0};
    SceNpServiceLabel npServiceLabel{0};
    ServiceListRequest request{};
    uint32_t filterCount{0};
    SceNpServiceEntitlementLabel filters[SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE]{};
};

struct UnifiedListPendingRequest {
    bool used{false};
    bool aborted{false};
    int64_t requestId{0};
    SceNpServiceLabel npServiceLabel{0};
    UnifiedListRequest request{};
    uint32_t filterCount{0};
    SceNpUnifiedEntitlementLabel filters[SCE_NP_ENTITLEMENT_ACCESS_ENTITLEMENT_INFO_LIST_MAX_SIZE]{};
};

struct DlcState {
    DlcMutex mutex;
    bool loaded{false};
    size_t count{0};
    size_t autoMountCount{0};
    size_t mountedCount{0};
    DlcEntry entries[kMaxDlcEntries]{};
    bool mounted[kMaxDlcEntries]{};
    ConsumedTransaction consumed[kMaxConsumeTransactions]{};
    size_t consumedNext{0};
    EntryPendingRequest entryRequests[kMaxEntryRequests]{};
    uint64_t entryRequestNext{1};
    UnifiedListPendingRequest unifiedListRequests[kMaxUnifiedListRequests]{};
    uint64_t unifiedListRequestNext{1};
    ServiceListPendingRequest serviceListRequests[kMaxServiceListRequests]{};
    uint64_t serviceListRequestNext{1};
};

enum class GameUpdateRequestState : uint8_t {
    Free,
    Active,
    Aborted,
};

struct GameUpdateState {
    DlcMutex mutex;
    bool initialized{false};
    int32_t nextRequestId{1};
    int32_t requestIds[kMaxGameUpdateRequests]{};
    GameUpdateRequestState requests[kMaxGameUpdateRequests]{};
};

alignas(DlcState) unsigned char g_stateStorage[sizeof(DlcState)];
std::atomic<DlcState*> g_state{nullptr};
std::atomic<uint32_t> g_stateInit{0};
std::atomic<uint64_t> g_transactionCounter{0};
GameUpdateState g_gameUpdateState;

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

// RUNNING firmware, unlike compiled_sdk_version() which is a build-time constant.
// Undeclared by the public SDK; shape read off fw 4.03's libkernel_sys. Reads
// sysctl kern.sdk_version, so (version >> 16) is 0x0403 for 4.03. Exists since 1.00.
struct SceKernelSwVersionRaw {
    uint64_t unknown0;
    char     str[0x1c];
    uint32_t version;       // +0x24
};
static_assert(sizeof(SceKernelSwVersionRaw) == 0x28u, "must match the out-param");

extern "C" int sceKernelGetProsperoSystemSwVersion(SceKernelSwVersionRaw* version);

uint32_t system_sw_version() {
    // Benign race: concurrent callers store the same value, so no lock is needed.
    static uint32_t cached = 0xffffffffu;
    if (cached == 0xffffffffu) {
        SceKernelSwVersionRaw v{};
        cached = (sceKernelGetProsperoSystemSwVersion(&v) == SCE_OK) ? v.version : 0u;
    }
    return cached;
}

// The fw 5.02 additions: a third download-data area and its block-size query
// (RPC 0x20010, 0x20023, 0x20024). Must be decided BEFORE the call -- the invoke
// is synchronous with no timeout, so an unimplemented command wedges the thread
// until the watchdog reboots. Unreadable version counts as absent: a graceful
// NOT_SUPPORTED beats an unrecoverable reboot.
bool firmware_has_download2() {
    return (system_sw_version() >> 16) >= 0x0502u;
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

[[maybe_unused]] int32_t np_rpc_invoke(uint32_t command,
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

int32_t app_rpc_u32_output(uint32_t command, uint32_t* value) {
    IpcBuffer outputs[1] = {{value, sizeof(*value)}};
    return app_rpc_invoke(command, nullptr, 0, outputs, 1);
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

int32_t app_rpc_mount_handle_command(uint32_t command, const void* downloadHandle) {
    uint64_t handle = reinterpret_cast<uint64_t>(downloadHandle);
    IpcBuffer input[1] = {{&handle, sizeof(handle)}};
    return app_rpc_invoke(command, input, 1, nullptr, 0);
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

bool package_type_is_addcont(uint32_t packageType) {
    return packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAC ||
           packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSAL;
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
        } else if (key_equals(key, "label") || key_equals(key, "entitlement_label")) {
            if (!copy_value(parsed.label, sizeof(parsed.label), value)) invalidate(parsed, "label");
        } else if (key_equals(key, "service_label")) {
            if (!copy_value(parsed.serviceLabel, sizeof(parsed.serviceLabel), value)) invalidate(parsed, "service_label");
        } else if (key_equals(key, "mount_point")) {
            if (!copy_value(parsed.mountPoint, sizeof(parsed.mountPoint), value)) invalidate(parsed, "mount_point");
        } else if (key_equals(key, "entitlement_key")) {
            if (!copy_value(parsed.keyHex, sizeof(parsed.keyHex), value)) invalidate(parsed, "entitlement_key");
        } else if (key_equals(key, "download_status")) {
            if (!parse_download_status(value, &parsed.downloadStatus)) invalidate(parsed, "download_status");
        } else if (key_equals(key, "np_service_label")) {
            if (std::strcmp(value, "-1") == 0) {
                parsed.npServiceLabel = 0;
                parsed.npServiceLabelSet = false;
            } else if (!parse_u32(value, &parsed.npServiceLabel)) {
                invalidate(parsed, "np_service_label");
            } else {
                parsed.npServiceLabelSet = true;
            }
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
        } else if (key_equals(key, "addcont_visible")) {
            if (!parse_value_bool(value, &parsed.addcontVisible)) {
                invalidate(parsed, "addcont_visible");
            } else {
                parsed.addcontVisibleSet = true;
            }
        } else if (key_equals(key, "unified_visible")) {
            if (!parse_value_bool(value, &parsed.unifiedVisible)) {
                invalidate(parsed, "unified_visible");
            } else {
                parsed.unifiedVisibleSet = true;
            }
        } else if (key_equals(key, "service_visible")) {
            if (!parse_value_bool(value, &parsed.serviceVisible)) {
                invalidate(parsed, "service_visible");
            } else {
                parsed.serviceVisibleSet = true;
            }
        } else if (key_equals(key, "mountable")) {
            if (!parse_value_bool(value, &parsed.mountable)) {
                invalidate(parsed, "mountable");
            } else {
                parsed.mountableSet = true;
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

    static bool valid_unified_label_text(const char* label) {
        if (!label || !label[0]) return false;
        const size_t len = std::strlen(label);
        if (len >= sizeof(SceNpUnifiedEntitlementLabel{}.data)) return false;
        for (const char* p = label; *p; ++p) {
            if (!std::isalnum(static_cast<unsigned char>(*p))) return false;
        }
        return true;
    }

    static bool label_exists(const DlcState& st, const char* label) {
        for (size_t i = 0; i < st.count; ++i) {
            if (st.entries[i].label.data[0] &&
                std::strcmp(st.entries[i].label.data, label) == 0) {
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
        if (!parsed.contentId[0] && !parsed.label[0] && !parsed.serviceLabel[0]) {
            dlc_logf("dlc.entry skip reason=missing-identifier");
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
        const char* contentLabel = nullptr;
        if (contentId[0] && !valid_content_id(contentId, &contentLabel)) {
            dlc_logf("dlc.entry skip reason=bad-content-id contentId=%s", contentId ? contentId : "<null>");
            return;
        }
        const char* label = parsed.label[0] ? parsed.label : contentLabel;
        const char* const displayLabel = label && label[0] ? label : "<none>";
        const bool defaultUnifiedVisible = label && label[0];
        const bool unifiedVisible = parsed.unifiedVisibleSet ? parsed.unifiedVisible : defaultUnifiedVisible;
        const bool addcontVisible = parsed.addcontVisibleSet
                                        ? parsed.addcontVisible
                                        : package_type_is_addcont(packageType);
        const bool serviceVisible = parsed.serviceVisibleSet
                                        ? parsed.serviceVisible
                                        : parsed.serviceLabel[0] != '\0';
        const bool mountable = parsed.mountableSet
                                   ? parsed.mountable
                                   : package_type_supports_mount(packageType) && addcontVisible;
        if (!addcontVisible && !unifiedVisible && !serviceVisible) {
            dlc_logf("dlc.entry skip reason=no-visible-surface label=%s", displayLabel);
            return;
        }
        if (mountable && !addcontVisible) {
            dlc_logf("dlc.entry skip reason=mountable-without-addcont label=%s", displayLabel);
            return;
        }
        if ((unifiedVisible || addcontVisible) && !valid_unified_label_text(label)) {
            dlc_logf("dlc.entry skip reason=bad-label contentId=%s label=%s",
                     contentId ? contentId : "<null>",
                     label ? label : "<null>");
            return;
        }
        if (label && label[0] && label_exists(st, label)) {
            dlc_logf("dlc.entry skip reason=duplicate-label label=%s", label);
            return;
        }
        if (parsed.useCount < 0 || parsed.useLimit < 0) {
            dlc_logf("dlc.entry skip reason=bad-usage-count label=%s", label ? label : "<none>");
            return;
        }

        DlcEntry& entry = st.entries[st.count];
        strlcpy(entry.contentId, contentId, sizeof(entry.contentId));
        if (label) strlcpy(entry.label.data, label, sizeof(entry.label.data));
        entry.packageType = packageType;
        entry.status = parsed.downloadStatus;
        entry.npServiceLabel = parsed.npServiceLabel;
        entry.npServiceLabelSet = parsed.npServiceLabelSet;
        entry.useCount = parsed.useCount;
        entry.useLimit = parsed.useLimit;
        entry.activeFlag = parsed.activeFlag;
        entry.activeDate = parsed.activeDate;
        entry.inactiveDate = parsed.inactiveDate;
        entry.consumable = parsed.consumableSet
                               ? parsed.consumable
                               : (packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSCONS ||
                                  packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_PSVC);
        entry.addcontVisible = addcontVisible;
        entry.unifiedVisible = unifiedVisible;
        entry.mountable = mountable;
        if (parsed.serviceLabel[0]) {
            if (!valid_service_label_text(parsed.serviceLabel)) {
                dlc_logf("dlc.entry skip reason=bad-service-label label=%s serviceLabel=%s",
                         displayLabel,
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
        entry.serviceVisible = serviceVisible;
        if (entry.serviceVisible && !entry.hasServiceLabel) {
            dlc_logf("dlc.entry skip reason=service-visible-without-label label=%s",
                     label ? label : "<none>");
            entry = DlcEntry{};
            return;
        }
        bool autoMountAssigned = false;
        if (parsed.mountPoint[0]) {
            if (std::strlen(parsed.mountPoint) >= sizeof(entry.mount.data)) {
                dlc_logf("dlc.entry skip reason=mount-too-long label=%s", displayLabel);
                entry = DlcEntry{};
                return;
            }
            strlcpy(entry.mount.data, parsed.mountPoint, sizeof(entry.mount.data));
        } else if (entry.mountable) {
            const int mountLen = std::snprintf(entry.mount.data,
                                               sizeof(entry.mount.data),
                                               "%s%u",
                                               SCE_DLC_EMU_MOUNT_PREFIX,
                                               static_cast<unsigned>(st.autoMountCount));
            if (mountLen < 0 || static_cast<size_t>(mountLen) >= sizeof(entry.mount.data)) {
                dlc_logf("dlc.entry skip reason=mount-too-long label=%s", displayLabel);
                entry = DlcEntry{};
                return;
            }
            autoMountAssigned = true;
        }
        if (parsed.keyHex[0]) {
            if (!parse_hex_key(parsed.keyHex, entry.key)) {
                dlc_logf("dlc.entry skip reason=bad-key label=%s", displayLabel);
                entry = DlcEntry{};
                return;
            }
        } else {
            make_default_key(st.count, entry.key);
        }

        if (autoMountAssigned) ++st.autoMountCount;

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
    dlc_logf("dlc.version value=%s", SCE_DLC_EMU_VERSION);

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

bool entry_matches_np_service(const DlcEntry& entry, SceNpServiceLabel serviceLabel) {
    return !entry.npServiceLabelSet || entry.npServiceLabel == serviceLabel;
}

bool entry_is_addcont(const DlcEntry& entry, SceNpServiceLabel serviceLabel) {
    return entry.addcontVisible && entry.activeFlag &&
           entry_matches_np_service(entry, serviceLabel);
}

bool entry_is_unified(const DlcEntry& entry, SceNpServiceLabel serviceLabel) {
    return entry.unifiedVisible && entry.label.data[0] &&
           entry_matches_np_service(entry, serviceLabel);
}

bool entry_is_service(const DlcEntry& entry, SceNpServiceLabel serviceLabel) {
    return entry.serviceVisible && entry.hasServiceLabel &&
           entry_matches_np_service(entry, serviceLabel);
}

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

bool copy_active_entry_by_label(const SceNpUnifiedEntitlementLabel* label,
                                DlcEntry* out,
                                size_t* indexOut = nullptr) {
    DlcEntry entry{};
    size_t index = 0;
    if (!copy_entry_by_label(label, &entry, &index) || !entry.activeFlag) return false;
    if (out) *out = entry;
    if (indexOut) *indexOut = index;
    return true;
}

int32_t mount_entry_unlocked(DlcState& st, size_t index, SceAppContentMountPoint* mountPoint) {
    if (index >= st.count || !mountPoint) return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    const DlcEntry& entry = st.entries[index];
    if (!entry.addcontVisible || !entry.mountable || !entry.activeFlag) {
        return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    }
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

bool entry_matches_package(const DlcEntry& entry, uint32_t packageType) {
    return entry.unifiedVisible && entry.label.data[0] &&
           (packageType == SCE_NP_ENTITLEMENT_ACCESS_PACKAGE_TYPE_NONE ||
            entry.packageType == packageType);
}

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

void fill_app_info(const DlcEntry& entry, SceAppContentAddcontInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->entitlementLabel = entry.label;
    info->status = entry.status;
}

void fill_game_update_no_addcont_latest_version(SceGameUpdateAddcontVersionInfo* info) {
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->found = false;
}

void fill_game_update_no_update(SceGameUpdateCheckResult* result) {
    if (!result) return;
    std::memset(result, 0, sizeof(*result));
    result->size = sizeof(*result);
    result->found = false;
    result->addcontFound = false;
}

GameUpdateRequestState* find_game_update_request_unlocked(GameUpdateState& st, int32_t requestId) {
    if (requestId <= 0) return nullptr;
    for (size_t i = 0; i < kMaxGameUpdateRequests; ++i) {
        if (st.requests[i] != GameUpdateRequestState::Free && st.requestIds[i] == requestId) {
            return &st.requests[i];
        }
    }
    return nullptr;
}

void clear_game_update_requests_unlocked(GameUpdateState& st) {
    std::memset(st.requestIds, 0, sizeof(st.requestIds));
    for (auto& request : st.requests) {
        request = GameUpdateRequestState::Free;
    }
}

int32_t allocate_game_update_request_id_unlocked(GameUpdateState& st) {
    for (size_t attempt = 0; attempt < kMaxGameUpdateRequests + 1u; ++attempt) {
        int32_t candidate = st.nextRequestId++;
        if (candidate <= 0) {
            st.nextRequestId = 2;
            candidate = 1;
        }
        if (!find_game_update_request_unlocked(st, candidate)) return candidate;
    }
    return 0;
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
           label->data[0] &&
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
           transactionId->transactionId[0] &&
           is_zeroed(transactionId->padding, sizeof(transactionId->padding)) &&
           has_nul_terminator(transactionId->transactionId, sizeof(transactionId->transactionId));
}

bool valid_np_user_id(SceUserServiceUserId userId) {
    return userId != SCE_USER_SERVICE_USER_ID_INVALID;
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
    for (uint32_t i = 0; i < listNum; ++i) {
        if (!valid_np_unified_label(&list[i])) return false;
    }
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

ServiceListRequest make_service_list_request(const SceNpEntitlementAccessRequestEntitlementInfoListParam* param) {
    ServiceListRequest request{};
    if (!param) return request;
    request.sort = param->sort;
    request.direction = param->direction;
    request.offset = static_cast<uint32_t>(param->offset);
    request.limit = static_cast<uint32_t>(param->limit);
    return request;
}

int64_t next_entry_request_id_unlocked(DlcState& st) {
    for (size_t attempt = 0; attempt <= kMaxEntryRequests; ++attempt) {
        if (st.entryRequestNext == 0 || st.entryRequestNext >= 0x0fffffffULL) {
            st.entryRequestNext = 1;
        }
        const int64_t candidate =
            kSyntheticEntryRequestBase + static_cast<int64_t>(st.entryRequestNext++);
        bool used = false;
        for (size_t i = 0; i < kMaxEntryRequests; ++i) {
            if (st.entryRequests[i].used && st.entryRequests[i].requestId == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return candidate;
    }
    return 0;
}

EntryPendingRequest* allocate_entry_request_unlocked(DlcState& st,
                                                     EntryRequestType type,
                                                     size_t entryIndex,
                                                     int32_t resultUseLimit,
                                                     int64_t* requestId) {
    if (!requestId || entryIndex >= st.count) return nullptr;
    for (size_t i = 0; i < kMaxEntryRequests; ++i) {
        if (st.entryRequests[i].used) continue;
        EntryPendingRequest& slot = st.entryRequests[i];
        slot = EntryPendingRequest{};
        slot.used = true;
        slot.requestId = next_entry_request_id_unlocked(st);
        if (slot.requestId == 0) {
            slot = EntryPendingRequest{};
            return nullptr;
        }
        slot.type = type;
        slot.entryIndex = entryIndex;
        slot.resultUseLimit = resultUseLimit;
        slot.snapshot = st.entries[entryIndex];
        *requestId = slot.requestId;
        return &slot;
    }
    return nullptr;
}

EntryPendingRequest* find_entry_request_unlocked(DlcState& st, int64_t requestId) {
    for (size_t i = 0; i < kMaxEntryRequests; ++i) {
        if (st.entryRequests[i].used && st.entryRequests[i].requestId == requestId) {
            return &st.entryRequests[i];
        }
    }
    return nullptr;
}

int64_t next_unified_list_request_id_unlocked(DlcState& st) {
    for (size_t attempt = 0; attempt <= kMaxUnifiedListRequests; ++attempt) {
        if (st.unifiedListRequestNext == 0 || st.unifiedListRequestNext >= 0x0fffffffULL) {
            st.unifiedListRequestNext = 1;
        }
        const int64_t candidate =
            kSyntheticRequestBase + static_cast<int64_t>(st.unifiedListRequestNext++);
        bool used = false;
        for (size_t i = 0; i < kMaxUnifiedListRequests; ++i) {
            if (st.unifiedListRequests[i].used &&
                st.unifiedListRequests[i].requestId == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return candidate;
    }
    return 0;
}

UnifiedListPendingRequest* remember_unified_list_request_unlocked(
    DlcState& st,
    SceNpServiceLabel serviceLabel,
    const UnifiedListRequest& request,
    const SceNpUnifiedEntitlementLabel* list,
    uint32_t listNum,
    int64_t* requestId) {
    if (!requestId) return nullptr;
    for (size_t i = 0; i < kMaxUnifiedListRequests; ++i) {
        if (st.unifiedListRequests[i].used) continue;
        UnifiedListPendingRequest& slot = st.unifiedListRequests[i];
        slot = UnifiedListPendingRequest{};
        slot.used = true;
        slot.requestId = next_unified_list_request_id_unlocked(st);
        if (slot.requestId == 0) {
            slot = UnifiedListPendingRequest{};
            return nullptr;
        }
        slot.npServiceLabel = serviceLabel;
        slot.request = request;
        slot.filterCount = listNum;
        for (uint32_t j = 0; j < listNum; ++j) slot.filters[j] = list[j];
        *requestId = slot.requestId;
        return &slot;
    }
    return nullptr;
}

UnifiedListPendingRequest* find_unified_list_request_unlocked(DlcState& st,
                                                              int64_t requestId) {
    for (size_t i = 0; i < kMaxUnifiedListRequests; ++i) {
        if (st.unifiedListRequests[i].used &&
            st.unifiedListRequests[i].requestId == requestId) {
            return &st.unifiedListRequests[i];
        }
    }
    return nullptr;
}

bool unified_list_request_matches(const UnifiedListPendingRequest& request,
                                  const DlcEntry& entry) {
    if (!entry_matches_np_service(entry, request.npServiceLabel) ||
        !entry_matches_package(entry, request.request.packageType)) {
        return false;
    }
    if (request.filterCount == 0) return true;
    for (uint32_t i = 0; i < request.filterCount; ++i) {
        if (std::strcmp(request.filters[i].data, entry.label.data) == 0) return true;
    }
    return false;
}

int64_t next_service_list_request_id_unlocked(DlcState& st) {
    for (size_t attempt = 0; attempt <= kMaxServiceListRequests; ++attempt) {
        if (st.serviceListRequestNext == 0 || st.serviceListRequestNext >= 0x0fffffffULL) {
            st.serviceListRequestNext = 1;
        }
        const int64_t candidate =
            kSyntheticServiceListRequestBase + static_cast<int64_t>(st.serviceListRequestNext++);
        bool used = false;
        for (size_t i = 0; i < kMaxServiceListRequests; ++i) {
            if (st.serviceListRequests[i].used &&
                st.serviceListRequests[i].requestId == candidate) {
                used = true;
                break;
            }
        }
        if (!used) return candidate;
    }
    return 0;
}

ServiceListPendingRequest* remember_service_list_request_unlocked(
    DlcState& st,
    SceNpServiceLabel serviceLabel,
    const ServiceListRequest& request,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    int64_t* requestId) {
    if (!requestId) return nullptr;
    for (size_t i = 0; i < kMaxServiceListRequests; ++i) {
        if (st.serviceListRequests[i].used) continue;
        ServiceListPendingRequest& slot = st.serviceListRequests[i];
        slot = ServiceListPendingRequest{};
        slot.used = true;
        slot.requestId = next_service_list_request_id_unlocked(st);
        if (slot.requestId == 0) {
            slot = ServiceListPendingRequest{};
            return nullptr;
        }
        slot.npServiceLabel = serviceLabel;
        slot.request = request;
        slot.filterCount = listNum;
        for (uint32_t j = 0; j < listNum; ++j) slot.filters[j] = list[j];
        *requestId = slot.requestId;
        return &slot;
    }
    return nullptr;
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

bool service_list_request_matches(const ServiceListPendingRequest& request, const DlcEntry& entry) {
    if (!entry_is_service(entry, request.npServiceLabel)) return false;
    if (request.filterCount == 0) return true;
    for (uint32_t i = 0; i < request.filterCount; ++i) {
        if (std::strcmp(request.filters[i].data, entry.serviceLabel.data) == 0) {
            return true;
        }
    }
    return false;
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
                                            int32_t useCount,
                                            int32_t resultUseLimit) {
    if (!transactionId) return;
    ConsumedTransaction& slot = st.consumed[st.consumedNext % kMaxConsumeTransactions];
    slot = ConsumedTransaction{};
    slot.used = true;
    slot.entryIndex = entryIndex;
    slot.useCount = useCount;
    slot.resultUseLimit = resultUseLimit;
    strlcpy(slot.transactionId, transactionId->transactionId, sizeof(slot.transactionId));
    st.consumedNext = (st.consumedNext + 1u) % kMaxConsumeTransactions;
}

int32_t consume_entry_unlocked(DlcState& st,
                               size_t entryIndex,
                               const SceNpEntitlementAccessTransactionId* transactionId,
                               int32_t useCount,
                               EntryRequestType requestType,
                               int64_t* requestId) {
    if (entryIndex >= st.count || !transactionId || !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    }
    if (useCount <= 0) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;

    if (ConsumedTransaction* consumed = find_consumed_transaction_unlocked(st, transactionId)) {
        if (consumed->entryIndex != entryIndex || consumed->useCount != useCount) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
        }
        return allocate_entry_request_unlocked(st,
                                               requestType,
                                               entryIndex,
                                               consumed->resultUseLimit,
                                               requestId)
                   ? SCE_OK
                   : SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }

    DlcEntry& entry = st.entries[entryIndex];
    if (!entry.activeFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    if (!entry.consumable) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    if (useCount > entry.useLimit || entry.useCount > INT32_MAX - useCount) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
    }
    const int32_t resultUseLimit = entry.useLimit - useCount;
    if (!allocate_entry_request_unlocked(st,
                                         requestType,
                                         entryIndex,
                                         resultUseLimit,
                                         requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    entry.useCount += useCount;
    entry.useLimit = resultUseLimit;

    remember_consumed_transaction_unlocked(st,
                                           entryIndex,
                                           transactionId,
                                           useCount,
                                           resultUseLimit);
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

// Old AppContent list API: expose only active entries assigned to addcont.
int32_t dlcEmu_sceAppContentGetAddcontInfoList(SceNpServiceLabel serviceLabel,
                                                     SceAppContentAddcontInfo* list,
                                                     uint32_t listNum,
                                                     uint32_t* hitNum) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    uint32_t total = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (entry_is_addcont(st.entries[i], serviceLabel)) ++total;
    }
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_APP_CONTENT_ERROR_PARAMETER;
        *hitNum = total;
        dlc_logf("dlc.app_addcont_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        if (!entry_is_addcont(st.entries[i], serviceLabel)) continue;
        fill_app_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = total;
    dlc_logf("dlc.app_addcont_list.fake mode=list fake=%u written=%u",
                              total,
                              written);
    return SCE_OK;
}

// Old AppContent single-info API: configured labels are installed.
int32_t dlcEmu_sceAppContentGetAddcontInfo(SceNpServiceLabel serviceLabel,
                                                 const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                 SceAppContentAddcontInfo* info) {
    if (!valid_app_unified_label(entitlementLabel) || !info) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry) &&
        entry_is_addcont(entry, serviceLabel)) {
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
    if (copy_entry_by_identifier(entitlementId, &entry) &&
        entry.addcontVisible && entry.activeFlag) {
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
    uint32_t total = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (st.entries[i].addcontVisible && st.entries[i].activeFlag) ++total;
    }
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_APP_CONTENT_ERROR_PARAMETER;
        *hitNum = total;
        dlc_logf("dlc.app_iro_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        if (!st.entries[i].addcontVisible || !st.entries[i].activeFlag) continue;
        fill_app_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = total;
    dlc_logf("dlc.app_iro_list.fake mode=list fake=%u written=%u",
                              total,
                              written);
    return SCE_OK;
}

// Old AppContent entitlement-key API: return explicit/default key for fake DLC.
int32_t dlcEmu_sceAppContentGetEntitlementKey(SceNpServiceLabel serviceLabel,
                                                    const SceNpUnifiedEntitlementLabel* entitlementLabel,
                                                    SceAppContentEntitlementKey* key) {
    if (!valid_app_unified_label(entitlementLabel) || !key) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry) &&
        entry_is_addcont(entry, serviceLabel)) {
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
    if (!valid_app_unified_label(entitlementLabel) || !mountPoint) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel);
    if (entry && !entry_matches_np_service(*entry, serviceLabel)) entry = nullptr;
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

// Delete is a successful no-op so emulated DLC remains available.
int32_t dlcEmu_sceAppContentAddcontDelete(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_label(entitlementLabel, &entry) &&
                   entry_is_addcont(entry, serviceLabel)
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentAddcontEnqueueDownload(SceNpServiceLabel serviceLabel,
                                                         const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_label(entitlementLabel, &entry) &&
                   entry_is_addcont(entry, serviceLabel)
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadSp(SceNpServiceLabel serviceLabel,
                                                           const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_label(entitlementLabel, &entry) &&
                   entry_is_addcont(entry, serviceLabel)
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentAddcontEnqueueDownloadByEntitlemetId(const char* entitlementId) {
    if (!entitlementId) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_identifier(entitlementId, &entry) &&
                   entry.addcontVisible && entry.activeFlag
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentAddcontShrink(SceNpServiceLabel serviceLabel,
                                                const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label(entitlementLabel)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_label(entitlementLabel, &entry) &&
                   entry_is_addcont(entry, serviceLabel)
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentCheckBundleLicenseOnDisc(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel) {
    if (!valid_app_unified_label_for_sdk(entitlementLabel, 0x1500000u)) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    return copy_entry_by_label(entitlementLabel, &entry) &&
                   entry_is_addcont(entry, serviceLabel)
               ? SCE_OK
               : SCE_APP_CONTENT_ERROR_NOT_FOUND;
}

int32_t dlcEmu_sceAppContentDownload2Shrink(const void* downloadHandle) {
    if (!downloadHandle) return SCE_APP_CONTENT_ERROR_PARAMETER;
    // A SIZE in MiB passed by value, despite the pointer-shaped ABI. Never deref.
    if (!firmware_has_download2()) return SCE_APP_CONTENT_ERROR_NOT_SUPPORTED;
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload2Shrink, downloadHandle);
}

int32_t dlcEmu_sceAppContentDownload2Expand(const void* downloadHandle) {
    if (!downloadHandle) return SCE_APP_CONTENT_ERROR_PARAMETER;
    // See Download2Shrink: a size in MiB, not a handle.
    if (!firmware_has_download2()) return SCE_APP_CONTENT_ERROR_NOT_SUPPORTED;
    return app_rpc_mount_handle_command(kAppContentRpcCommandDownload2Expand, downloadHandle);
}

int32_t dlcEmu_sceAppContentGetPlayableStatus(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    uint32_t* playableStatus) {
    if (!valid_app_unified_label_for_sdk(entitlementLabel, 0x3500000u) || !playableStatus) {
        return SCE_APP_CONTENT_ERROR_PARAMETER;
    }
    DlcEntry entry{};
    if (!copy_entry_by_label(entitlementLabel, &entry) ||
        !entry_is_addcont(entry, serviceLabel)) {
        return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    }
    *playableStatus = 1u;
    return SCE_OK;
}

int32_t dlcEmu_sceAppContentGetGameTrialsFlag(uint32_t* gameTrialsFlag) {
    if (!gameTrialsFlag) return SCE_APP_CONTENT_ERROR_PARAMETER;
    *gameTrialsFlag = 0u;
    return SCE_OK;
}

int32_t dlcEmu_sceAppContentUnknownMdid(const void* mdid, bool* matches) {
    if (!mdid || !matches) return SCE_APP_CONTENT_ERROR_PARAMETER;
    *matches = true;
    return SCE_OK;
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
    // Also fw 5.02. *blockSize deliberately left unwritten: a negative Sce error
    // means out-params are not read, which is the firmware's own contract.
    if (!firmware_has_download2()) return SCE_APP_CONTENT_ERROR_NOT_SUPPORTED;
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

int32_t dlcEmu_sceAppContentGetAddcontDownloadProgress(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceAppContentAddcontDownloadProgress* progress) {
    if (!valid_app_unified_label(entitlementLabel) || !progress) return SCE_APP_CONTENT_ERROR_PARAMETER;
    DlcEntry entry{};
    if (!copy_entry_by_label(entitlementLabel, &entry) ||
        !entry_is_addcont(entry, serviceLabel)) {
        return SCE_APP_CONTENT_ERROR_NOT_FOUND;
    }
    progress->dataSize = 1u;
    progress->downloadedSize = 1u;
    return SCE_OK;
}

int32_t dlcEmu_sceAppContentGetPftFlag(SceAppContentPftFlag* pftFlag) {
    if (!pftFlag) return SCE_APP_CONTENT_ERROR_PARAMETER;
    *pftFlag = SCE_APP_CONTENT_PFT_FLAG_OFF;
    return SCE_OK;
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
    *gameTrialsFlag = SCE_NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF;
    return SCE_OK;
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
    if (listNum > SCE_NP_ENTITLEMENT_ACCESS_ADDCONT_ENTITLEMENT_INFO_LIST_MAX_SIZE) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    uint32_t total = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (entry_is_addcont(st.entries[i], serviceLabel)) ++total;
    }
    if (!list || listNum == 0) {
        if (!hitNum) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
        *hitNum = total;
        dlc_logf("dlc.np_addcont_list.fake mode=count fake=%u", *hitNum);
        return SCE_OK;
    }
    uint32_t written = 0;
    for (size_t i = 0; i < st.count && written < listNum; ++i) {
        if (!entry_is_addcont(st.entries[i], serviceLabel)) continue;
        fill_np_info(st.entries[i], &list[written++]);
    }
    if (hitNum) *hitNum = total;
    dlc_logf("dlc.np_addcont_list.fake mode=list fake=%u written=%u",
                              total,
                              written);
    return SCE_OK;
}

// Main PS5 single entitlement API: active configured entries use their configured status.
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfo(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    if (!valid_np_unified_label(entitlementLabel) || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_active_entry_by_label(entitlementLabel, &entry) &&
        entry_is_addcont(entry, serviceLabel)) {
        fill_np_info(entry, info);
        dlc_logf("dlc.np_addcont_info.fake label=%s packageType=%s status=%u",
                                  entry.label.data,
                                  package_type_name(entry.packageType),
                                  entry.status);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Runtime-only individual addcont query: surface active configured DLC even though
// this symbol is omitted from the SDK 10 weak stub.
int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoIndividual(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* info) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_unified_label(entitlementLabel) || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_active_entry_by_label(entitlementLabel, &entry) &&
        entry_is_addcont(entry, serviceLabel)) {
        fill_np_info(entry, info);
        dlc_logf("dlc.np_addcont_info_individual.fake label=%s packageType=%s status=%u",
                                  entry.label.data,
                                  package_type_name(entry.packageType),
                                  entry.status);
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessGetAddcontEntitlementInfoListIndividual(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    SceNpEntitlementAccessAddcontEntitlementInfo* list,
    uint32_t listNum,
    uint32_t* hitNum) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (listNum > SCE_NP_ENTITLEMENT_ACCESS_ADDCONT_ENTITLEMENT_INFO_LIST_MAX_SIZE) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    if ((!list || listNum == 0) && !hitNum) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }

    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    uint32_t written = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (!entry_is_addcont(st.entries[i], serviceLabel)) continue;
        if (list && written < listNum) {
            fill_np_info(st.entries[i], &list[written]);
        }
        ++written;
    }
    if (hitNum) *hitNum = written;
    return SCE_OK;
}

int32_t dlcEmu_sceNpEntitlementAccessGetPftFlag(
    SceNpEntitlementAccessGameTrialsFlag* pftFlag) {
    if (!pftFlag) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    *pftFlag = SCE_NP_ENTITLEMENT_ACCESS_GAME_TRIALS_FLAG_OFF;
    return SCE_OK;
}

// Entitlement key API: fake DLC returns explicit config key or default index+1024
// key encoded as little-endian uint64.
int32_t dlcEmu_sceNpEntitlementAccessGetEntitlementKey(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceNpEntitlementAccessEntitlementKey* key) {
    if (!valid_np_unified_label(entitlementLabel) || !key) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcEntry entry{};
    if (copy_entry_by_label(entitlementLabel, &entry) &&
        entry_is_addcont(entry, serviceLabel)) {
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
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_unified_label(entitlementLabel) ||
        !valid_np_transaction_id(transactionId) ||
        !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    size_t index = 0;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel)) {
        if (!entry_is_unified(*entry, serviceLabel)) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        }
        index = static_cast<size_t>(entry - st.entries);
        const int32_t rc = consume_entry_unlocked(st,
                                                  index,
                                                  transactionId,
                                                  useCount,
                                                  EntryRequestType::ConsumeUnified,
                                                  requestId);
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
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_service_label(entitlementLabel) ||
        !valid_np_transaction_id(transactionId) ||
        !requestId) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_service_label_unlocked(st, entitlementLabel)) {
        if (!entry_is_service(*entry, serviceLabel)) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        }
        const size_t index = static_cast<size_t>(entry - st.entries);
        const int32_t rc = consume_entry_unlocked(st,
                                                  index,
                                                  transactionId,
                                                  useCount,
                                                  EntryRequestType::ConsumeService,
                                                  requestId);
        if (rc == SCE_OK) {
            dlc_logf("dlc.service_consume.request.fake serviceLabel=%s useCount=%d",
                     entitlementLabel->data,
                     useCount);
        }
        return rc;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Poll a typed consume request and return the remaining use limit captured by it.
int32_t dlcEmu_sceNpEntitlementAccessPollConsumeEntitlement(
    int64_t requestId,
    int32_t* pResult,
    int32_t* useLimit) {
    if (!pResult || !useLimit) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    EntryPendingRequest* request = find_entry_request_unlocked(st, requestId);
    if (!request ||
        (request->type != EntryRequestType::ConsumeUnified &&
         request->type != EntryRequestType::ConsumeService)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }
    *pResult = request->aborted ? SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED : SCE_OK;
    *useLimit = request->aborted ? -1 : request->resultUseLimit;
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

// Request fake unified entitlement info and remember the label in the request id.
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_unified_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel)) {
        if (!entry_is_unified(*entry, serviceLabel)) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        }
        const size_t index = static_cast<size_t>(entry - st.entries);
        return allocate_entry_request_unlocked(st,
                                               EntryRequestType::UnifiedInfo,
                                               index,
                                               entry->useLimit,
                                               requestId)
                   ? SCE_OK
                   : SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

// Poll fake unified entitlement info as active/installed.
int32_t dlcEmu_sceNpEntitlementAccessPollUnifiedEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    if (!pResult || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    EntryPendingRequest* request = find_entry_request_unlocked(st, requestId);
    if (!request || request->type != EntryRequestType::UnifiedInfo) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }
    if (request->aborted) {
        std::memset(info, 0, sizeof(*info));
        *pResult = SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED;
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    if (request->entryIndex < st.count) {
        const DlcEntry& entry = request->snapshot;
        *pResult = SCE_OK;
        fill_unified_info(entry, info);
        dlc_logf("dlc.unified_info.poll.fake label=%s packageType=%s",
                                  entry.label.data,
                                  package_type_name(entry.packageType));
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

// Request-list API also succeeds for an empty configured list.
int32_t dlcEmu_sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_unified_list_request(list, listNum, param, requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    const UnifiedListRequest request = make_unified_list_request(param);
    if (!remember_unified_list_request_unlocked(st,
                                                serviceLabel,
                                                request,
                                                list,
                                                listNum,
                                                requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    dlc_logf("dlc.unified_list.request.fake packageType=%s offset=%u limit=%u sort=%u direction=%u",
                              package_type_name(request.packageType),
                              request.offset,
                              request.limit,
                              request.sort,
                              request.direction);
    return SCE_OK;
}

// Poll-list API applies package, service-label and optional entitlement-label filters.
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
    UnifiedListPendingRequest pending{};
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (UnifiedListPendingRequest* stored = find_unified_list_request_unlocked(st, requestId)) {
        pending = *stored;
        if (pending.aborted) {
            *pResult = SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED;
            if (hitNum) *hitNum = 0;
            *nextOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
            *previousOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
            return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
        }
        size_t indices[kMaxDlcEntries]{};
        uint32_t written = 0;
        uint32_t total = 0;
        for (size_t i = 0; i < st.count; ++i) {
            if (!unified_list_request_matches(pending, st.entries[i])) continue;
            indices[total++] = i;
        }
        sort_unified_indices_unlocked(st, indices, total, pending.request);

        const uint32_t pageLimit = pending.request.limit < listNum ? pending.request.limit : listNum;
        const uint32_t start = pending.request.offset < total ? pending.request.offset : total;
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
                                  package_type_name(pending.request.packageType),
                                  pending.request.offset,
                                  pending.request.limit,
                                  total,
                                  written,
                                  *nextOffset,
                                  *previousOffset);
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestConsumableEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_unified_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_label_unlocked(st, entitlementLabel)) {
        if (!entry_is_unified(*entry, serviceLabel) || !entry->consumable) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        }
        const size_t index = static_cast<size_t>(entry - st.entries);
        return allocate_entry_request_unlocked(st,
                                               EntryRequestType::ConsumableInfo,
                                               index,
                                               entry->useLimit,
                                               requestId)
                   ? SCE_OK
                   : SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessPollConsumableEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessUnifiedEntitlementInfo* info) {
    if (!pResult || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    EntryPendingRequest* request = find_entry_request_unlocked(st, requestId);
    if (!request || request->type != EntryRequestType::ConsumableInfo) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }
    if (request->aborted) {
        std::memset(info, 0, sizeof(*info));
        *pResult = SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED;
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    if (request->entryIndex >= st.count) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    *pResult = SCE_OK;
    fill_unified_info(request->snapshot, info);
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfo(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* entitlementLabel,
    int64_t* requestId) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_service_label(entitlementLabel) || !requestId) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (const DlcEntry* entry = find_entry_by_service_label_unlocked(st, entitlementLabel)) {
        if (!entry_is_service(*entry, serviceLabel)) {
            return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
        }
        const size_t index = static_cast<size_t>(entry - st.entries);
        return allocate_entry_request_unlocked(st,
                                               EntryRequestType::ServiceInfo,
                                               index,
                                               entry->useLimit,
                                               requestId)
                   ? SCE_OK
                   : SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_NO_ENTITLEMENT;
}

int32_t dlcEmu_sceNpEntitlementAccessPollServiceEntitlementInfo(
    int64_t requestId,
    int32_t* pResult,
    SceNpEntitlementAccessServiceEntitlementInfo* info) {
    if (!pResult || !info) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    EntryPendingRequest* request = find_entry_request_unlocked(st, requestId);
    if (!request || request->type != EntryRequestType::ServiceInfo) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }
    if (request->aborted) {
        std::memset(info, 0, sizeof(*info));
        *pResult = SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED;
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }
    if (request->entryIndex >= st.count) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    *pResult = SCE_OK;
    fill_service_info(request->snapshot, info);
    return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
}

int32_t dlcEmu_sceNpEntitlementAccessRequestServiceEntitlementInfoList(
    SceUserServiceUserId userId,
    SceNpServiceLabel serviceLabel,
    const SceNpServiceEntitlementLabel* list,
    uint32_t listNum,
    const SceNpEntitlementAccessRequestEntitlementInfoListParam* param,
    int64_t* requestId) {
    if (!valid_np_user_id(userId)) return SCE_NP_ENTITLEMENT_ACCESS_ERROR_USER_NOT_FOUND;
    if (!valid_np_service_list_request(list, listNum, param, requestId)) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_PARAMETER;
    }
    const ServiceListRequest request = make_service_list_request(param);
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    return remember_service_list_request_unlocked(st,
                                                  serviceLabel,
                                                  request,
                                                  list,
                                                  listNum,
                                                  requestId)
               ? SCE_OK
               : SCE_NP_ENTITLEMENT_ACCESS_ERROR_BUSY;
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
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    ServiceListPendingRequest* pending = find_service_list_request_unlocked(st, requestId);
    if (!pending || !pending->used) {
        return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
    }
    if (pending->aborted) {
        *pResult = SCE_NP_ENTITLEMENT_ACCESS_ERROR_ABORTED;
        if (hitNum) *hitNum = 0;
        *nextOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
        *previousOffset = SCE_NP_ENTITLEMENT_ACCESS_INVALID_OFFSET;
        return SCE_NP_ENTITLEMENT_ACCESS_POLL_RET_FINISHED;
    }

    size_t indices[kMaxDlcEntries]{};
    uint32_t total = 0;
    for (size_t i = 0; i < st.count; ++i) {
        if (!service_list_request_matches(*pending, st.entries[i])) continue;
        indices[total++] = i;
    }
    sort_service_indices_unlocked(st, indices, total, pending->request);

    uint32_t written = 0;
    const uint32_t pageLimit = pending->request.limit < listNum ? pending->request.limit : listNum;
    const uint32_t start = pending->request.offset < total ? pending->request.offset : total;
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
    if (EntryPendingRequest* request = find_entry_request_unlocked(st, requestId)) {
        *request = EntryPendingRequest{};
        return SCE_OK;
    }
    if (UnifiedListPendingRequest* request = find_unified_list_request_unlocked(st, requestId)) {
        *request = UnifiedListPendingRequest{};
        return SCE_OK;
    }
    if (ServiceListPendingRequest* request = find_service_list_request_unlocked(st, requestId)) {
        *request = ServiceListPendingRequest{};
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

int32_t dlcEmu_sceNpEntitlementAccessAbortRequest(int64_t requestId) {
    DlcState& st = ensure_loaded();
    DlcLockGuard lock(st.mutex);
    if (EntryPendingRequest* request = find_entry_request_unlocked(st, requestId)) {
        request->aborted = true;
        return SCE_OK;
    }
    if (UnifiedListPendingRequest* request = find_unified_list_request_unlocked(st, requestId)) {
        request->aborted = true;
        return SCE_OK;
    }
    if (ServiceListPendingRequest* request = find_service_list_request_unlocked(st, requestId)) {
        request->aborted = true;
        return SCE_OK;
    }
    return SCE_NP_ENTITLEMENT_ACCESS_ERROR_REQUEST_NOT_FOUND;
}

int32_t dlcEmu_sceGameUpdateInitialize(void) {
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (st.initialized) return SCE_GAME_UPDATE_ERROR_ALREADY_INITIALIZED;
    clear_game_update_requests_unlocked(st);
    st.nextRequestId = 1;
    st.initialized = true;
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateTerminate(void) {
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    clear_game_update_requests_unlocked(st);
    st.initialized = false;
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateCreateRequest(void) {
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    for (size_t i = 0; i < kMaxGameUpdateRequests; ++i) {
        if (st.requests[i] != GameUpdateRequestState::Free) continue;
        const int32_t requestId = allocate_game_update_request_id_unlocked(st);
        if (requestId <= 0) return SCE_GAME_UPDATE_ERROR_TOO_MANY_REQUESTS;
        st.requestIds[i] = requestId;
        st.requests[i] = GameUpdateRequestState::Active;
        return requestId;
    }
    return SCE_GAME_UPDATE_ERROR_TOO_MANY_REQUESTS;
}

int32_t dlcEmu_sceGameUpdateCheck(int32_t requestId,
                                  const SceGameUpdateCheckParam* param,
                                  SceGameUpdateCheckResult* result) {
    if (!param || !result) return SCE_GAME_UPDATE_ERROR_INVALID_ARG;
    if (param->size != sizeof(*param) || result->size != sizeof(*result)) {
        return SCE_GAME_UPDATE_ERROR_INVALID_SIZE;
    }
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    GameUpdateRequestState* request = find_game_update_request_unlocked(st, requestId);
    if (!request) return SCE_GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
    if (*request == GameUpdateRequestState::Aborted) return SCE_GAME_UPDATE_ERROR_ABORTED;
    fill_game_update_no_update(result);
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateCheckTitle(int32_t requestId,
                                       uint32_t serviceLabel,
                                       SceGameUpdateCheckResult* result) {
    (void)serviceLabel;
    if (!result) return SCE_GAME_UPDATE_ERROR_INVALID_ARG;
    if (result->size != sizeof(*result)) return SCE_GAME_UPDATE_ERROR_INVALID_SIZE;
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    GameUpdateRequestState* request = find_game_update_request_unlocked(st, requestId);
    if (!request) return SCE_GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
    if (*request == GameUpdateRequestState::Aborted) return SCE_GAME_UPDATE_ERROR_ABORTED;
    fill_game_update_no_update(result);
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateAbortRequest(int32_t requestId) {
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    GameUpdateRequestState* request = find_game_update_request_unlocked(st, requestId);
    if (!request) return SCE_GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
    *request = GameUpdateRequestState::Aborted;
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateDeleteRequest(int32_t requestId) {
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    GameUpdateRequestState* request = find_game_update_request_unlocked(st, requestId);
    if (!request) return SCE_GAME_UPDATE_ERROR_REQUEST_NOT_FOUND;
    const size_t index = static_cast<size_t>(request - st.requests);
    st.requests[index] = GameUpdateRequestState::Free;
    st.requestIds[index] = 0;
    return SCE_OK;
}

int32_t dlcEmu_sceGameUpdateGetAddcontLatestVersion(
    SceNpServiceLabel serviceLabel,
    const SceNpUnifiedEntitlementLabel* entitlementLabel,
    SceGameUpdateAddcontVersionInfo* info) {
    if (!valid_np_unified_label(entitlementLabel) || !info) return SCE_GAME_UPDATE_ERROR_INVALID_ARG;
    if (info->size != sizeof(*info)) return SCE_GAME_UPDATE_ERROR_INVALID_SIZE;
    GameUpdateState& st = g_gameUpdateState;
    DlcLockGuard lock(st.mutex);
    if (!st.initialized) return SCE_GAME_UPDATE_ERROR_NOT_INITIALIZED;
    (void)serviceLabel;
    fill_game_update_no_addcont_latest_version(info);
    dlc_logf("dlc.game_update_latest.fake label=%s found=0", entitlementLabel->data);
    return SCE_OK;
}

} // extern "C"
