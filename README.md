# PS5 DLC-emu

Standalone PS5 replacement modules for fake DLC entitlement tests.

The project builds two PRX/SPRX modules:

- `libSceAppContent.prx` / `libSceAppContent.sprx`
- `libSceNpEntitlementAccess.prx` / `libSceNpEntitlementAccess.sprx`

Configured fake DLC is exposed to AppContent and NpEntitlementAccess list/info/key/mount APIs. 
Installed/native DLC is not merged into fake lists. 
AppContent Download and TemporaryData APIs are delegated to the native service path because this project does not emulate those storage systems.

## Runtime Config

The modules read `/app0/dlc_emu.ini` lazily on the first DLC-related API call.
If the file is missing, invalid, or contains no valid entries, fake DLC APIs
return empty/no-entitlement style results.

Minimal example:

```ini
[PSAC]
content_id=UP0000-PPSA00000_00-SAMPLEPSAC000001
download_status=INSTALLED
mount_point=/app0/addcont0
entitlement_key=00112233445566778899AABBCCDDEEFF

[PSCONS]
content_id=UP0000-PPSA00000_00-SAMPLECONS000001
service_label=CONS01
use_count=0
use_limit=5
consumable=true
```

Supported parameters:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `content_id` | required | 36-character package content ID. The 16-character suffix after the last `-` becomes the unified entitlement label. |
| `download_status` | `INSTALLED` | Reported addcont status: `NO_EXTRA_DATA`, `NO_IN_QUEUE`, `DOWNLOADING`, `DOWNLOAD_SUSPENDED`, or `INSTALLED`. |
| `mount_point` | `/app0/addcontN` for `[PSAC]` | Pre-existing folder returned by fake addcont mount APIs. The folder is not created by dlc-emu. |
| `entitlement_key` | entry index + 1024 | 16-byte key as 32 hex characters. |
| `service_label` | unset | Optional 1-6 alphanumeric label for service entitlement APIs. |
| `use_count` | `0` | Initial consumable/service/unified use counter. |
| `use_limit` | `1` | Maximum consumable use count. |
| `consumable` | `true` for `[PSCONS]`/`[PSVC]` | Returned as `isConsumable` in service entitlement info and controls whether consume requests are accepted and increment `use_count`.  |
| `active_flag` | `true` | Returned entitlement active flag. |
| `active_date` | `0` | `SceRtcTick.tick` active date. |
| `inactive_date` | `18446744073709551615` | `SCE_NP_ENTITLEMENT_ACCESS_INVALID_DATE`. |

Supported sections: `[NONE]`, `[PSGD]`, `[PSAC]`, `[PSAL]`, `[PSCONS]`,
`[PSVC]`, `[PSSUBS]`.

| Section | Number | Typical use |
| --- | ---: | --- |
| `[NONE]` | 0 | No package-type filtering. |
| `[PSGD]` | 1 | Game data entitlement type. |
| `[PSAC]` | 2 | Additional content with optional file data. |
| `[PSAL]` | 3 | Add-on/license-style package type. |
| `[PSCONS]` | 4 | Consumable or product entitlement style used by some titles for deluxe/unlock checks. |
| `[PSVC]` | 5 | Virtual currency style entitlement. |
| `[PSSUBS]` | 6 | Subscription style entitlement. |

## Notes

- Fake addcont mount tracks local mounted/not-mounted state and returns the
  configured path without mounting or creating storage.
- Download, TemporaryData, shrink/expand/format, region, patch-install, and
  related storage-management calls are not fake-emulated.
- Logs are written to `/app0/dlc_emu.log` when `SCE_DLC_EMU_LOG` is enabled in
  `src/dlc_modules/dlc_config.h`.

## Thanks

Thanks to @idlesauce for https://github.com/idlesauce/ps4-eboot-dlc-patcher
