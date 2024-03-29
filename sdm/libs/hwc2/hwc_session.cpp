/*
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android/binder_manager.h>
#include <core/buffer_allocator.h>
#include <private/color_params.h>
#include <utils/constants.h>
#include <utils/String16.h>
#include <cutils/properties.h>
#include <hardware_legacy/uevent.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <binder/Parcel.h>
#include <QService.h>
#include <utils/debug.h>
#include <sync/sync.h>
#include <utils/utils.h>
#include <algorithm>
#include <string>
#include <bitset>
#include <thread>
#include <memory>
#include <vector>

#include <map>

#include "hwc_buffer_allocator.h"
#include "hwc_session.h"
#include "hwc_debugger.h"
#include <processgroup/processgroup.h>
#include <system/graphics.h>

#define __CLASS__ "HWCSession"

#define HWC_UEVENT_SWITCH_HDMI "change@/devices/virtual/switch/hdmi"
#define HWC_UEVENT_GRAPHICS_FB0 "change@/devices/virtual/graphics/fb0"
#define HWC_UEVENT_DRM_EXT_HOTPLUG "mdss_mdp/drm/card"

static sdm::HWCSession::HWCModuleMethods g_hwc_module_methods;

#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;
using namespace std::chrono_literals;

hwc_module_t HAL_MODULE_INFO_SYM = {
  .common = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 3,
    .version_minor = 0,
    .id = HWC_HARDWARE_MODULE_ID,
    .name = "QTI Hardware Composer Module",
    .author = "CodeAurora Forum",
    .methods = &g_hwc_module_methods,
    .dso = 0,
    .reserved = {0},
  }
};

namespace sdm {

constexpr float nsecsPerSec = std::chrono::nanoseconds(1s).count();
constexpr int64_t nsecsIdleHintTimeout = std::chrono::nanoseconds(100ms).count();
HWCSession::PowerHalHintWorker::PowerHalHintWorker()
      : Worker("DisplayHints", HAL_PRIORITY_URGENT_DISPLAY),
        mNeedUpdateRefreshRateHint(false),
        mPrevRefreshRate(0),
        mPendingPrevRefreshRate(0),
        mIdleHintIsEnabled(false),
        mIdleHintDeadlineTime(0),
        mIdleHintSupportIsChecked(false),
        mIdleHintIsSupported(false),
        mPowerModeState(HWC2::PowerMode::Off),
        mVsyncPeriod(16666666),
        mPowerHalExtAidl(nullptr) {
    InitWorker();
}

int32_t HWCSession::PowerHalHintWorker::connectPowerHalExt() {
    if (mPowerHalExtAidl) {
        return android::NO_ERROR;
    }
    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder pwBinder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    ndk::SpAIBinder pwExtBinder;
    AIBinder_getExtension(pwBinder.get(), pwExtBinder.getR());
    mPowerHalExtAidl = IPowerExt::fromBinder(pwExtBinder);
    if (!mPowerHalExtAidl) {
        DLOGE("failed to connect power HAL extension");
        return -EINVAL;
    }
    ALOGI("connect power HAL extension successfully");
    return android::NO_ERROR;
}

int32_t HWCSession::PowerHalHintWorker::checkPowerHalExtHintSupport(const std::string &mode) {
    if (mode.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    bool isSupported = false;
    auto ret = mPowerHalExtAidl->isModeSupported(mode.c_str(), &isSupported);
    if (!ret.isOk()) {
        DLOGE("failed to check power HAL extension hint: mode=%s", mode.c_str());
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            DLOGE("binder transaction failed for power HAL extension hint");
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    if (!isSupported) {
        DLOGW("power HAL extension hint is not supported: mode=%s", mode.c_str());
        return -EOPNOTSUPP;
    }
    DLOGI("power HAL extension hint is supported: mode=%s", mode.c_str());
    return android::NO_ERROR;
}

int32_t HWCSession::PowerHalHintWorker::sendPowerHalExtHint(const std::string &mode,
                                                               bool enabled) {
    if (mode.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    auto ret = mPowerHalExtAidl->setMode(mode.c_str(), enabled);
    if (!ret.isOk()) {
        DLOGE("failed to send power HAL extension hint: mode=%s, enabled=%d", mode.c_str(),
              enabled);
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            DLOGE("binder transaction failed for power HAL extension hint");
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    return android::NO_ERROR;
}

int32_t HWCSession::PowerHalHintWorker::checkRefreshRateHintSupport(int refreshRate) {
    int32_t ret = android::NO_ERROR;
    const auto its = mRefreshRateHintSupportMap.find(refreshRate);
    if (its == mRefreshRateHintSupportMap.end()) {
        /* check new hint */
        std::string refreshRateHintStr = "REFRESH_" + std::to_string(refreshRate) + "FPS";
        ret = checkPowerHalExtHintSupport(refreshRateHintStr);
        if (ret == android::NO_ERROR || ret == -EOPNOTSUPP) {
            mRefreshRateHintSupportMap[refreshRate] = (ret == android::NO_ERROR);
            DLOGI("cache refresh rate hint %s: %d", refreshRateHintStr.c_str(), !ret);
        } else {
            DLOGE("failed to check the support of refresh rate hint, ret %d", ret);
        }
    } else {
        /* check existing hint */
        if (!its->second) {
            ret = -EOPNOTSUPP;
        }
    }
    return ret;
}

int32_t HWCSession::PowerHalHintWorker::sendRefreshRateHint(int refreshRate, bool enabled) {
    std::string hintStr = "REFRESH_" + std::to_string(refreshRate) + "FPS";
    int32_t ret = sendPowerHalExtHint(hintStr, enabled);
    if (ret == -ENOTCONN) {
        /* Reset the hints when binder failure occurs */
        mPrevRefreshRate = 0;
        mPendingPrevRefreshRate = 0;
    }
    return ret;
}

int32_t HWCSession::PowerHalHintWorker::updateRefreshRateHintInternal(
        HWC2::PowerMode powerMode, uint32_t vsyncPeriod) {
    int32_t ret = android::NO_ERROR;
    /* We should disable pending hint before other operations */
    if (mPendingPrevRefreshRate) {
        ret = sendRefreshRateHint(mPendingPrevRefreshRate, false);
        if (ret == android::NO_ERROR) {
            mPendingPrevRefreshRate = 0;
        } else {
            return ret;
        }
    }
    if (powerMode != HWC2::PowerMode::On) {
        if (mPrevRefreshRate) {
            ret = sendRefreshRateHint(mPrevRefreshRate, false);
            // DLOGI("RefreshRate hint = %d disabled", mPrevRefreshRate);
            if (ret == android::NO_ERROR) {
                mPrevRefreshRate = 0;
            }
        }
        return ret;
    }
    /* TODO: add refresh rate buckets, tracked in b/181100731 */
    int refreshRate = static_cast<int>(round(nsecsPerSec / vsyncPeriod * 0.1f) * 10);
    if (mPrevRefreshRate == refreshRate) {
        return android::NO_ERROR;
    }
    ret = checkRefreshRateHintSupport(refreshRate);
    if (ret != android::NO_ERROR) {
        return ret;
    }
    /*
     * According to PowerHAL design, while switching to next refresh rate, we
     * have to enable the next hint first, then disable the previous one so
     * that the next hint can take effect.
     */
    ret = sendRefreshRateHint(refreshRate, true);
    // DLOGI("RefreshRate hint = %d enabled", refreshRate);
    if (ret != android::NO_ERROR) {
        return ret;
    }
    if (mPrevRefreshRate) {
        ret = sendRefreshRateHint(mPrevRefreshRate, false);
        if (ret != android::NO_ERROR) {
            if (ret != -ENOTCONN) {
                /*
                 * We may fail to disable the previous hint and end up multiple
                 * hints enabled. Save the failed hint as pending hint here, we
                 * will try to disable it first while entering this function.
                 */
                mPendingPrevRefreshRate = mPrevRefreshRate;
                mPrevRefreshRate = refreshRate;
            }
            return ret;
        }
    }
    mPrevRefreshRate = refreshRate;
    return ret;
}

int32_t HWCSession::PowerHalHintWorker::checkIdleHintSupport(void) {
    int32_t ret = android::NO_ERROR;
    Lock();
    if (mIdleHintSupportIsChecked) {
        ret = mIdleHintIsSupported ? android::NO_ERROR : -EOPNOTSUPP;
        Unlock();
        return ret;
    }
    Unlock();
    ret = checkPowerHalExtHintSupport("DISPLAY_IDLE");
    Lock();
    if (ret == android::NO_ERROR) {
        mIdleHintIsSupported = true;
        mIdleHintSupportIsChecked = true;
        DLOGI("display idle hint is supported");
    } else if (ret == -EOPNOTSUPP) {
        mIdleHintSupportIsChecked = true;
        DLOGI("display idle hint is unsupported");
    } else {
        DLOGW("failed to check the support of display idle hint, ret %d", ret);
    }
    Unlock();
    return ret;
}

int32_t HWCSession::PowerHalHintWorker::updateIdleHint(uint64_t deadlineTime) {
    int32_t ret = checkIdleHintSupport();
    if (ret != android::NO_ERROR) {
        return ret;
    }
    bool enableIdleHint = (deadlineTime < systemTime(SYSTEM_TIME_MONOTONIC));

    if (mIdleHintIsEnabled != enableIdleHint) {
        // DLOGI("idle hint = %d", enableIdleHint);
        ret = sendPowerHalExtHint("DISPLAY_IDLE", enableIdleHint);
        if (ret == android::NO_ERROR) {
            mIdleHintIsEnabled = enableIdleHint;
        }
    }
    return ret;
}

void HWCSession::PowerHalHintWorker::signalRefreshRate(HWC2::PowerMode powerMode,
                                                          uint32_t vsyncPeriod) {
    Lock();
    mPowerModeState = powerMode;
    mVsyncPeriod = vsyncPeriod;
    mNeedUpdateRefreshRateHint = true;
    Unlock();
    Signal();
}

void HWCSession::PowerHalHintWorker::signalIdle() {
    Lock();
    if (mIdleHintSupportIsChecked && !mIdleHintIsSupported) {
        Unlock();
        return;
    }
    mIdleHintDeadlineTime = static_cast<uint64_t>(systemTime(SYSTEM_TIME_MONOTONIC) + nsecsIdleHintTimeout);
    Unlock();
    Signal();
}

void HWCSession::PowerHalHintWorker::Routine() {
    Lock();
    int ret = android::NO_ERROR;
    if (!mNeedUpdateRefreshRateHint) {
        if (!mIdleHintIsSupported || mIdleHintIsEnabled) {
            ret = WaitForSignalOrExitLocked();
        } else {
            uint64_t currentTime = static_cast<uint_t>(systemTime(SYSTEM_TIME_MONOTONIC));
            if (mIdleHintDeadlineTime > currentTime) {
                uint64_t timeout = mIdleHintDeadlineTime - currentTime;
                ret = WaitForSignalOrExitLocked(static_cast<uint_t>(timeout));
            }
        }
    }
    if (ret == -EINTR) {
        Unlock();
        return;
    }
    bool needUpdateRefreshRateHint = mNeedUpdateRefreshRateHint;
    uint64_t deadlineTime = mIdleHintDeadlineTime;
    HWC2::PowerMode powerMode = mPowerModeState;
    uint32_t vsyncPeriod = mVsyncPeriod;
    /*
     * Clear the flags here instead of clearing them after calling the hint
     * update functions. The flags may be set by signals after Unlock() and
     * before the hint update functions are done. Thus we may miss the newest
     * hints if we clear the flags after the hint update functions work without
     * errors.
     */
    mNeedUpdateRefreshRateHint = false;
    Unlock();
    updateIdleHint(deadlineTime);
    if (needUpdateRefreshRateHint) {
        int32_t rc = updateRefreshRateHintInternal(powerMode, vsyncPeriod);
        if (rc != android::NO_ERROR && rc != -EOPNOTSUPP) {
            Lock();
            if (mPowerModeState == HWC2::PowerMode::On) {
                /* Set the flag to trigger update again for next loop */
                mNeedUpdateRefreshRateHint = true;
            }
            Unlock();
        }
    }
}

static HWCUEvent g_hwc_uevent_;
Locker HWCSession::locker_[HWCCallbacks::kNumDisplays];
static const int kSolidFillDelay = 100 * 1000;

void HWCUEvent::UEventThread(HWCUEvent *hwc_uevent) {
  const char *uevent_thread_name = "HWC_UeventThread";

  prctl(PR_SET_NAME, uevent_thread_name, 0, 0, 0);
  setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);

  int status = uevent_init();
  if (!status) {
    std::unique_lock<std::mutex> caller_lock(hwc_uevent->mutex_);
    hwc_uevent->caller_cv_.notify_one();
    DLOGE("Failed to init uevent with err %d", status);
    return;
  }

  {
    // Signal caller thread that worker thread is ready to listen to events.
    std::unique_lock<std::mutex> caller_lock(hwc_uevent->mutex_);
    hwc_uevent->init_done_ = true;
    hwc_uevent->caller_cv_.notify_one();
  }

  while (1) {
    char uevent_data[PAGE_SIZE] = {};

    // keep last 2 zeros to ensure double 0 termination
    int length = uevent_next_event(uevent_data, INT32(sizeof(uevent_data)) - 2);

    // scope of lock to this block only, so that caller is free to set event handler to nullptr;
    {
      std::lock_guard<std::mutex> guard(hwc_uevent->mutex_);
      if (hwc_uevent->uevent_listener_) {
        hwc_uevent->uevent_listener_->UEventHandler(uevent_data, length);
      } else {
        DLOGW("UEvent dropped. No uevent listener.");
      }
    }
  }
}

HWCUEvent::HWCUEvent() {
  std::unique_lock<std::mutex> caller_lock(mutex_);
  std::thread thread(HWCUEvent::UEventThread, this);
  thread.detach();
  caller_cv_.wait(caller_lock);
}

void HWCUEvent::Register(HWCUEventListener *uevent_listener) {
  DLOGI("Set uevent listener = %p", uevent_listener);

  std::lock_guard<std::mutex> obj(mutex_);
  uevent_listener_ = uevent_listener;
}

HWCSession::HWCSession(const hw_module_t *module) {
  hwc2_device_t::common.tag = HARDWARE_DEVICE_TAG;
  hwc2_device_t::common.version = HWC_DEVICE_API_VERSION_2_0;
  hwc2_device_t::common.module = const_cast<hw_module_t *>(module);
  hwc2_device_t::common.close = Close;
  hwc2_device_t::getCapabilities = GetCapabilities;
  hwc2_device_t::getFunction = GetFunction;
}

int HWCSession::Init() {
  SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);

  int status = -EINVAL;
  const char *qservice_name = "display.qservice";

  if (!g_hwc_uevent_.InitDone()) {
    return status;
  }


  // Start QService and connect to it.
  qService::QService::init();
  android::sp<qService::IQService> iqservice = android::interface_cast<qService::IQService>(
      android::defaultServiceManager()->getService(android::String16(qservice_name)));

  if (iqservice.get()) {
    iqservice->connect(android::sp<qClient::IQClient>(this));
    qservice_ = reinterpret_cast<qService::QService *>(iqservice.get());
  } else {
    DLOGE("Failed to acquire %s", qservice_name);
    return -EINVAL;
  }

  StartServices();

  g_hwc_uevent_.Register(this);

  InitSupportedDisplaySlots();
  // Create primary display here. Remaining builtin displays will be created after client has set
  // display indexes which may happen sometime before callback is registered.
  status = CreatePrimaryDisplay();
  if (status) {
    Deinit();
    return status;
  }

  is_composer_up_ = true;

  return 0;
}

int HWCSession::Deinit() {
  // Destroy all connected displays
  DestroyDisplay(&map_info_primary_);

  for (auto &map_info : map_info_builtin_) {
    DestroyDisplay(&map_info);
  }

  for (auto &map_info : map_info_pluggable_) {
    DestroyDisplay(&map_info);
  }

  for (auto &map_info : map_info_virtual_) {
    DestroyDisplay(&map_info);
  }

  if (color_mgr_) {
    color_mgr_->DestroyColorManager();
  }

  g_hwc_uevent_.Register(nullptr);
  CoreInterface::DestroyCore();

  return 0;
}

void HWCSession::InitSupportedDisplaySlots() {
  // Default slots:
  //    Primary = 0, External = 1
  //    Additional external displays 2,3,...max_pluggable_count.
  //    Additional builtin displays max_pluggable_count + 1, max_pluggable_count + 2,...
  //    Last slots for virtual displays.
  // Virtual display id is only for SF <--> HWC communication.
  // It need not align with hwccomposer_defs

  map_info_primary_.client_id = qdutils::DISPLAY_PRIMARY;

  DisplayError error = CoreInterface::CreateCore(&buffer_allocator_, &buffer_sync_handler_,
                                                 &socket_handler_, &core_intf_);
  if (error != kErrorNone) {
    DLOGE("Failed to create CoreInterface");
    return;
  }

  HWDisplayInterfaceInfo hw_disp_info = {};
  error = core_intf_->GetFirstDisplayInterfaceType(&hw_disp_info);
  if (error != kErrorNone) {
    CoreInterface::DestroyCore();
    DLOGE("Primary display type not recognized. Error = %d", error);
    return;
  }

  int max_builtin = 0;
  int max_pluggable = 0;
  int max_virtual = 0;

  error = core_intf_->GetMaxDisplaysSupported(kBuiltIn, &max_builtin);
  if (error != kErrorNone) {
    CoreInterface::DestroyCore();
    DLOGE("Could not find maximum built-in displays supported. Error = %d", error);
    return;
  }

  error = core_intf_->GetMaxDisplaysSupported(kPluggable, &max_pluggable);
  if (error != kErrorNone) {
    CoreInterface::DestroyCore();
    DLOGE("Could not find maximum pluggable displays supported. Error = %d", error);
    return;
  }

  error = core_intf_->GetMaxDisplaysSupported(kVirtual, &max_virtual);
  if (error != kErrorNone) {
    CoreInterface::DestroyCore();
    DLOGE("Could not find maximum virtual displays supported. Error = %d", error);
    return;
  }

  if (kPluggable == hw_disp_info.type) {
    // If primary is a pluggable display, we have already used one pluggable display interface.
    max_pluggable--;
  } else {
    max_builtin--;
  }

  // Init slots in accordance to h/w capability.
  uint32_t disp_count = UINT32(std::min(max_pluggable, HWCCallbacks::kNumPluggable));
  hwc2_display_t base_id = qdutils::DISPLAY_EXTERNAL;
  map_info_pluggable_.resize(disp_count);
  for (auto &map_info : map_info_pluggable_) {
    map_info.client_id = base_id++;
  }

  disp_count = UINT32(std::min(max_builtin, HWCCallbacks::kNumBuiltIn));
  map_info_builtin_.resize(disp_count);
  for (auto &map_info : map_info_builtin_) {
    map_info.client_id = base_id++;
  }

  disp_count = UINT32(std::min(max_virtual, HWCCallbacks::kNumVirtual));
  map_info_virtual_.resize(disp_count);
  for (auto &map_info : map_info_virtual_) {
    map_info.client_id = base_id++;
  }

  // resize HDR supported map to total number of displays.
  is_hdr_display_.resize(UINT32(base_id));
}

int HWCSession::GetDisplayIndex(int dpy) {
  DisplayMapInfo *map_info = nullptr;
  switch (dpy) {
    case qdutils::DISPLAY_PRIMARY:
      map_info = &map_info_primary_;
      break;
    case qdutils::DISPLAY_EXTERNAL:
      map_info = map_info_pluggable_.size() ? &map_info_pluggable_[0] : nullptr;
      break;
    case qdutils::DISPLAY_VIRTUAL:
      map_info = map_info_virtual_.size() ? &map_info_virtual_[0] : nullptr;
      break;
    case qdutils::DISPLAY_BUILTIN_2:
      map_info = map_info_builtin_.size() ? &map_info_builtin_[0] : nullptr;
      break;
  }

  if (!map_info) {
    return -1;
  }

  return INT(map_info->client_id);
}

int HWCSession::Open(const hw_module_t *module, const char *name, hw_device_t **device) {
  if (!module || !name || !device) {
    DLOGE("Invalid parameters.");
    return -EINVAL;
  }

  if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
    HWCSession *hwc_session = new HWCSession(module);
    if (!hwc_session) {
      return -ENOMEM;
    }

    int status = hwc_session->Init();
    if (status != 0) {
      delete hwc_session;
      hwc_session = NULL;
      return status;
    }

    hwc2_device_t *composer_device = hwc_session;
    *device = reinterpret_cast<hw_device_t *>(composer_device);
  }

  return 0;
}

int HWCSession::Close(hw_device_t *device) {
  if (!device) {
    return -EINVAL;
  }

  hwc2_device_t *composer_device = reinterpret_cast<hwc2_device_t *>(device);
  HWCSession *hwc_session = static_cast<HWCSession *>(composer_device);

  hwc_session->Deinit();

  return 0;
}

void HWCSession::GetCapabilities(struct hwc2_device *device, uint32_t *outCount,
                                 int32_t *outCapabilities) {
  if (!outCount) {
    return;
  }

  int value = 0;
  bool disable_skip_validate = false;
  if (Debug::Get()->GetProperty(DISABLE_SKIP_VALIDATE_PROP, &value) == kErrorNone) {
    disable_skip_validate = (value == 1);
  }
  uint32_t count = 1 + (disable_skip_validate ? 0 : 1);

  if (outCapabilities != nullptr && (*outCount >= count)) {
    outCapabilities[0] = HWC2_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
    if (!disable_skip_validate) {
      outCapabilities[1] = HWC2_CAPABILITY_SKIP_VALIDATE;
    }
  }
  *outCount = count;
}

template <typename PFN, typename T>
static hwc2_function_pointer_t AsFP(T function) {
  static_assert(std::is_same<PFN, T>::value, "Incompatible function pointer");
  return reinterpret_cast<hwc2_function_pointer_t>(function);
}

// HWC2 functions returned in GetFunction
// Defined in the same order as in the HWC2 header

int32_t HWCSession::AcceptDisplayChanges(hwc2_device_t *device, hwc2_display_t display) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::AcceptDisplayChanges);
}

int32_t HWCSession::CreateLayer(hwc2_device_t *device, hwc2_display_t display,
                                hwc2_layer_t *out_layer_id) {
  if (!out_layer_id) {
    return  HWC2_ERROR_BAD_PARAMETER;
  }

  return CallDisplayFunction(device, display, &HWCDisplay::CreateLayer, out_layer_id);
}

int32_t HWCSession::CreateVirtualDisplay(hwc2_device_t *device, uint32_t width, uint32_t height,
                                         int32_t *format, hwc2_display_t *out_display_id) {
  // TODO(user): Handle concurrency with HDMI
  if (!device) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  if (!out_display_id || !width || !height || !format) {
    return  HWC2_ERROR_BAD_PARAMETER;
  }

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  auto status = hwc_session->CreateVirtualDisplayObj(width, height, format, out_display_id);
  if (status == HWC2::Error::None) {
    DLOGI("Created virtual display id:% " PRIu64 ", res: %dx%d", *out_display_id, width, height);
  } else {
    DLOGE("Failed to create virtual display: %s", to_string(status).c_str());
  }

  hwc_session->HandleConcurrency(*out_display_id);
  return INT32(status);
}

int32_t HWCSession::DestroyLayer(hwc2_device_t *device, hwc2_display_t display,
                                 hwc2_layer_t layer) {
  return CallDisplayFunction(device, display, &HWCDisplay::DestroyLayer, layer);
}

int32_t HWCSession::DestroyVirtualDisplay(hwc2_device_t *device, hwc2_display_t display) {
  if (!device || display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  auto *hwc_session = static_cast<HWCSession *>(device);

  for (auto &map_info : hwc_session->map_info_virtual_) {
    if (map_info.client_id == display) {
      DLOGI("Destroying virtual display id:%" PRIu64, display);
      hwc_session->DestroyDisplay(&map_info);
      hwc_session->HandleConcurrency(display);
      break;
    }
  }

  return HWC2_ERROR_NONE;
}

void HWCSession::Dump(hwc2_device_t *device, uint32_t *out_size, char *out_buffer) {
  if (!device || !out_size) {
    return;
  }

  auto *hwc_session = static_cast<HWCSession *>(device);
  const size_t max_dump_size = 8192;

  if (out_buffer == nullptr) {
    *out_size = max_dump_size;
  } else {
    std::string s {};
    for (int id = 0; id < HWCCallbacks::kNumDisplays; id++) {
      SCOPE_LOCK(locker_[id]);
      if (hwc_session->hwc_display_[id]) {
        s += hwc_session->hwc_display_[id]->Dump();
      }
    }
    auto copied = s.copy(out_buffer, std::min(s.size(), max_dump_size), 0);
    *out_size = UINT32(copied);
  }
}

static int32_t GetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t *out_config) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetActiveConfig, out_config);
}

static int32_t GetChangedCompositionTypes(hwc2_device_t *device, hwc2_display_t display,
                                          uint32_t *out_num_elements, hwc2_layer_t *out_layers,
                                          int32_t *out_types) {
  // null_ptr check only for out_num_elements, as out_layers and out_types can be null.
  if (!out_num_elements) {
    return  HWC2_ERROR_BAD_PARAMETER;
  }
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetChangedCompositionTypes,
                                         out_num_elements, out_layers, out_types);
}

static int32_t GetClientTargetSupport(hwc2_device_t *device, hwc2_display_t display, uint32_t width,
                                      uint32_t height, int32_t format, int32_t dataspace) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetClientTargetSupport,
                                         width, height, format, dataspace);
}

static int32_t GetColorModes(hwc2_device_t *device, hwc2_display_t display, uint32_t *out_num_modes,
                             int32_t /*android_color_mode_t*/ *int_out_modes) {
  auto out_modes = reinterpret_cast<android_color_mode_t *>(int_out_modes);
  if (out_num_modes == nullptr) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetColorModes, out_num_modes,
                                         out_modes);
}

static int32_t GetPerFrameMetadataKeys(hwc2_device_t *device, hwc2_display_t display,
                                       uint32_t *out_num_keys, int32_t *int_out_keys) {
  auto out_keys = reinterpret_cast<PerFrameMetadataKey *>(int_out_keys);
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetPerFrameMetadataKeys,
                                         out_num_keys, out_keys);
}

static int32_t SetLayerPerFrameMetadata(hwc2_device_t *device, hwc2_display_t display,
                                        hwc2_layer_t layer, uint32_t num_elements,
                                        const int32_t *int_keys, const float *metadata) {
  auto keys = reinterpret_cast<const PerFrameMetadataKey *>(int_keys);
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerPerFrameMetadata,
                                       num_elements, keys, metadata);
}

static int32_t GetDisplayAttribute(hwc2_device_t *device, hwc2_display_t display,
                                   hwc2_config_t config, int32_t int_attribute,
                                   int32_t *out_value) {
  if (out_value == nullptr || int_attribute < HWC2_ATTRIBUTE_INVALID ||
      int_attribute > HWC2_ATTRIBUTE_DPI_Y) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  auto attribute = static_cast<HWC2::Attribute>(int_attribute);
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetDisplayAttribute, config,
                                         attribute, out_value);
}

static int32_t GetDisplayConfigs(hwc2_device_t *device, hwc2_display_t display,
                                 uint32_t *out_num_configs, hwc2_config_t *out_configs) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetDisplayConfigs,
                                         out_num_configs, out_configs);
}

static int32_t GetDisplayName(hwc2_device_t *device, hwc2_display_t display, uint32_t *out_size,
                              char *out_name) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetDisplayName, out_size,
                                         out_name);
}

static int32_t GetDisplayRequests(hwc2_device_t *device, hwc2_display_t display,
                                  int32_t *out_display_requests, uint32_t *out_num_elements,
                                  hwc2_layer_t *out_layers, int32_t *out_layer_requests) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetDisplayRequests,
                                         out_display_requests, out_num_elements, out_layers,
                                         out_layer_requests);
}

static int32_t GetDisplayType(hwc2_device_t *device, hwc2_display_t display, int32_t *out_type) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetDisplayType, out_type);
}


static int32_t GetHdrCapabilities(hwc2_device_t* device, hwc2_display_t display,
                                  uint32_t* out_num_types, int32_t* out_types,
                                  float* out_max_luminance, float* out_max_average_luminance,
                                  float* out_min_luminance) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetHdrCapabilities,
                                         out_num_types, out_types, out_max_luminance,
                                         out_max_average_luminance, out_min_luminance);
}

static uint32_t GetMaxVirtualDisplayCount(hwc2_device_t *device) {
  if (device == nullptr) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  return 1;
}

static int32_t GetReleaseFences(hwc2_device_t *device, hwc2_display_t display,
                                uint32_t *out_num_elements, hwc2_layer_t *out_layers,
                                int32_t *out_fences) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::GetReleaseFences,
                                         out_num_elements, out_layers, out_fences);
}

int32_t HWCSession::PresentDisplay(hwc2_device_t *device, hwc2_display_t display,
                                   int32_t *out_retire_fence) {
  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  auto status = HWC2::Error::BadDisplay;
  DTRACE_SCOPED();

  thread_local bool setTaskProfileDone = false;
  if (setTaskProfileDone == false) {
        if (!SetTaskProfiles(gettid(), {"SFMainPolicy"})) {
          DLOGW("Failed to add `%d` into SFMainPolicy", gettid());
      }
      setTaskProfileDone = true;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  {
    SEQUENCE_EXIT_SCOPE_LOCK(locker_[display]);
    if (!device) {
      return HWC2_ERROR_BAD_DISPLAY;
    }

    if (out_retire_fence == nullptr) {
      return HWC2_ERROR_BAD_PARAMETER;
    }

    // TODO(user): Handle virtual display/HDMI concurrency
    if (hwc_session->hwc_display_[display]) {
      // Check if hwc's refresh trigger is getting exercised.
      if (hwc_session->callbacks_.NeedsRefresh(display)) {
        hwc_session->hwc_display_[display]->SetPendingRefresh();
        hwc_session->callbacks_.ResetRefresh(display);
      }
      status = hwc_session->PresentDisplayInternal(display, out_retire_fence);
      hwc_session->mPowerHalHint.signalIdle();
    }
  }

  if (status != HWC2::Error::None && status != HWC2::Error::NotValidated) {
    SEQUENCE_CANCEL_SCOPE_LOCK(locker_[display]);
  }

  // Handle pending builtin/pluggable display connections
  if (!hwc_session->primary_ready_ && (display == HWC_DISPLAY_PRIMARY)) {
    hwc_session->primary_ready_ = true;
    if (!hwc_session->pluggable_is_primary_) {
      hwc_session->CreatePluggableDisplays(false);
    }
  }

  hwc_session->HandlePendingRefresh();

  return INT32(status);
}

void HWCSession::HandlePendingRefresh() {
  if (pending_refresh_.none()) {
    return;
  }

  for (size_t i = 0; i < pending_refresh_.size(); i++) {
    if (pending_refresh_.test(i)) {
      Refresh(i);
      // SF refreshes all displays on refresh request.
      break;
    }
  }
  pending_refresh_.reset();
}

void HWCSession::HandleBuiltInDisplays() {
  // This would be called after client connection establishes.
  int status = 0;
  HWDisplaysInfo hw_displays_info = {};

  DisplayError error = core_intf_->GetDisplaysStatus(&hw_displays_info);
  if (error != kErrorNone) {
    DLOGE("Failed to get connected display list. Error = %d", error);
    return;
  }

  size_t client_id = 0;
  for (auto &iter : hw_displays_info) {
    auto &info = iter.second;
    if (info.is_primary || info.display_type != kBuiltIn || !info.is_connected) {
      continue;
    }

    if (client_id >= map_info_builtin_.size()) {
      DLOGW("Insufficient builtin display slots. All displays could not be created.");
      return;
    }

    DisplayMapInfo &map_info = map_info_builtin_[client_id];
    DLOGI("Create builtin display, sdm id = %d, client id = %d",
      info.display_id, map_info.client_id);
    status = HWCDisplayBuiltIn::Create(core_intf_, &buffer_allocator_, &callbacks_, qservice_,
                                       map_info.client_id, info.display_id, info.is_primary,
                                       &hwc_display_[map_info.client_id]);
    if (status) {
      DLOGE("Builtin display creation failed.");
      break;
    }
    client_id++;
    map_info.disp_type = info.display_type;
    map_info.sdm_id = info.display_id;
    DLOGI("Builtin display created client_id %d sdm_id %d ", map_info.client_id, map_info.sdm_id);

    client_id++;
  }
}

int32_t HWCSession::RegisterCallback(hwc2_device_t *device, int32_t descriptor,
                                     hwc2_callback_data_t callback_data,
                                     hwc2_function_pointer_t pointer) {
  if (!device) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  SCOPE_LOCK(hwc_session->callbacks_lock_);
  auto desc = static_cast<HWC2::Callback>(descriptor);
  auto error = hwc_session->callbacks_.Register(desc, callback_data, pointer);
  if (error != HWC2::Error::None) {
    return INT32(error);
  }

  DLOGD("%s callback: %s", pointer ? "Registering" : "Deregistering", to_string(desc).c_str());
  if (descriptor == HWC2_CALLBACK_HOTPLUG && pointer) {
    if (!hwc_session->client_connected_) {
      // Map Builtin displays that got created during init.
      hwc_session->HandleBuiltInDisplays();
    }

    // Notify All connected Displays.
    for (hwc2_display_t disp = 0; disp < HWCCallbacks::kNumDisplays; disp++) {
      if (!hwc_session->hwc_display_[disp]) {
        continue;
      }
      hwc_session->callbacks_.Hotplug(disp, HWC2::Connection::Connected);
    }
    hwc_session->client_connected_ = true;
  }
  hwc_session->need_invalidate_ = false;
  hwc_session->callbacks_lock_.Broadcast();

  return 0;
}

static int32_t SetActiveConfig(hwc2_device_t *device, hwc2_display_t display,
                               hwc2_config_t config) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetActiveConfig, config);
}

static int32_t SetClientTarget(hwc2_device_t *device, hwc2_display_t display,
                               buffer_handle_t target, int32_t acquire_fence,
                               int32_t dataspace, hwc_region_t damage) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetClientTarget, target,
                                         acquire_fence, dataspace, damage);
}

int32_t HWCSession::SetColorMode(hwc2_device_t *device, hwc2_display_t display,
                                 int32_t /*android_color_mode_t*/ int_mode) {
  if (int_mode < HAL_COLOR_MODE_NATIVE || int_mode > HAL_COLOR_MODE_DISPLAY_P3) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  auto mode = static_cast<android_color_mode_t>(int_mode);
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetColorMode, mode);
}

int32_t HWCSession::SetColorModeWithRenderIntent(hwc2_device_t *device, hwc2_display_t display,
                                                 int32_t /*ColorMode*/ int_mode,
                                                 int32_t /*RenderIntent*/ int_render_intent) {
  auto mode = static_cast<android_color_mode_t>(int_mode);
  if (mode < HAL_COLOR_MODE_NATIVE || mode > HAL_COLOR_MODE_DISPLAY_P3) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  auto render_intent = static_cast<RenderIntent>(int_render_intent);
  if ((render_intent < RenderIntent::COLORIMETRIC) ||
      (render_intent > RenderIntent::TONE_MAP_ENHANCE)) {
    DLOGE("Invalid RenderIntent: %d", render_intent);
    return HWC2_ERROR_BAD_PARAMETER;
  }
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetColorMode, mode);
}

int32_t HWCSession::SetColorTransform(hwc2_device_t *device, hwc2_display_t display,
                                      const float *matrix,
                                      int32_t /*android_color_transform_t*/ hint) {
  if (!matrix || hint < HAL_COLOR_TRANSFORM_IDENTITY ||
       hint > HAL_COLOR_TRANSFORM_CORRECT_TRITANOPIA) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  android_color_transform_t transform_hint = static_cast<android_color_transform_t>(hint);
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetColorTransform, matrix,
                                         transform_hint);
}

static int32_t SetCursorPosition(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t x, int32_t y) {
  auto status = INT32(HWC2::Error::None);
  status = HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetCursorPosition,
                                           layer, x, y);
  if (status == INT32(HWC2::Error::None)) {
    // Update cursor position
    HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetCursorPosition, x, y);
  }
  return status;
}

static int32_t SetLayerBlendMode(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t int_mode) {
  if (int_mode < HWC2_BLEND_MODE_INVALID || int_mode > HWC2_BLEND_MODE_COVERAGE) {
    return HWC2_ERROR_BAD_PARAMETER;
  }
  auto mode = static_cast<HWC2::BlendMode>(int_mode);
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerBlendMode, mode);
}

static int32_t SetLayerBuffer(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                              buffer_handle_t buffer, int32_t acquire_fence) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerBuffer, buffer,
                                       acquire_fence);
}

static int32_t SetLayerColor(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                             hwc_color_t color) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerColor, color);
}

static int32_t SetLayerCompositionType(hwc2_device_t *device, hwc2_display_t display,
                                       hwc2_layer_t layer, int32_t int_type) {
  auto type = static_cast<HWC2::Composition>(int_type);
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerCompositionType,
                                       type);
}

static int32_t SetLayerDataspace(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t dataspace) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerDataspace,
                                       dataspace);
}

static int32_t SetLayerDisplayFrame(hwc2_device_t *device, hwc2_display_t display,
                                    hwc2_layer_t layer, hwc_rect_t frame) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerDisplayFrame,
                                       frame);
}

static int32_t SetLayerPlaneAlpha(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                  float alpha) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerPlaneAlpha,
                                       alpha);
}

static int32_t SetLayerSourceCrop(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                  hwc_frect_t crop) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerSourceCrop, crop);
}

static int32_t SetLayerSurfaceDamage(hwc2_device_t *device, hwc2_display_t display,
                                     hwc2_layer_t layer, hwc_region_t damage) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerSurfaceDamage,
                                       damage);
}

static int32_t SetLayerTransform(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                                 int32_t int_transform) {
  auto transform = static_cast<HWC2::Transform>(int_transform);
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerTransform,
                                       transform);
}

static int32_t SetLayerVisibleRegion(hwc2_device_t *device, hwc2_display_t display,
                                     hwc2_layer_t layer, hwc_region_t visible) {
  return HWCSession::CallLayerFunction(device, display, layer, &HWCLayer::SetLayerVisibleRegion,
                                       visible);
}

static int32_t SetLayerZOrder(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer,
                              uint32_t z) {
  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetLayerZOrder, layer, z);
}

int32_t HWCSession::SetOutputBuffer(hwc2_device_t *device, hwc2_display_t display,
                                    buffer_handle_t buffer, int32_t releaseFence) {
  if (!device) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  auto *hwc_session = static_cast<HWCSession *>(device);
  if (INT32(display) != hwc_session->GetDisplayIndex(qdutils::DISPLAY_VIRTUAL)) {
    return HWC2_ERROR_UNSUPPORTED;
  }

  SCOPE_LOCK(locker_[display]);
  if (hwc_session->hwc_display_[display]) {
    auto vds = reinterpret_cast<HWCDisplayVirtual *>(hwc_session->hwc_display_[display]);
    auto status = vds->SetOutputBuffer(buffer, releaseFence);
    return INT32(status);
  } else {
    return HWC2_ERROR_BAD_DISPLAY;
  }
}

int32_t HWCSession::SetPowerMode(hwc2_device_t *device, hwc2_display_t display, int32_t int_mode) {
  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  //  validate device and also avoid undefined behavior in cast to HWC2::PowerMode
  if (!device || int_mode < HWC2_POWER_MODE_OFF || int_mode > HWC2_POWER_MODE_DOZE_SUSPEND) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  auto mode = static_cast<HWC2::PowerMode>(int_mode);

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  //  all displays support on/off. Check for doze modes
  int support = 0;
  hwc_session->GetDozeSupport(device, display, &support);
  if (!support && (mode == HWC2::PowerMode::Doze || mode == HWC2::PowerMode::DozeSuspend)) {
    return HWC2_ERROR_UNSUPPORTED;
  }

  auto error = CallDisplayFunction(device, display, &HWCDisplay::SetPowerMode, mode);
  if (error != HWC2_ERROR_NONE) {
    return error;
  }

  hwc_session->HandleConcurrency(display);

  if (mode == HWC2::PowerMode::Doze) {
    // Trigger refresh for doze mode to take effect.
    hwc_session->Refresh(display);
    // Trigger one more refresh for PP features to take effect.
    hwc_session->pending_refresh_.set(UINT32(display));
  } else {
    // Reset the pending refresh bit
    hwc_session->pending_refresh_.reset(UINT32(display));
  }

  hwc_session->updateRefreshRateHint();

  return HWC2_ERROR_NONE;
}

int32_t HWCSession::SetVsyncEnabled(hwc2_device_t *device, hwc2_display_t display,
                                    int32_t int_enabled) {
  //  avoid undefined behavior in cast to HWC2::Vsync
  if (int_enabled < HWC2_VSYNC_INVALID || int_enabled > HWC2_VSYNC_DISABLE) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  auto enabled = static_cast<HWC2::Vsync>(int_enabled);

  HWCSession *hwc_session = static_cast<HWCSession *>(device);

  if (int_enabled == HWC2_VSYNC_ENABLE) {
    hwc_session->callbacks_.UpdateVsyncSource(display);
  }

  return HWCSession::CallDisplayFunction(device, display, &HWCDisplay::SetVsyncEnabled, enabled);
}

int32_t HWCSession::GetDozeSupport(hwc2_device_t *device, hwc2_display_t display,
                                   int32_t *out_support) {
  if (!device || !out_support) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  *out_support = 0;
  if (display == HWC_DISPLAY_PRIMARY || display == hwc_session->GetNextBuiltinIndex()) {
    *out_support = 1;
  }

  return HWC2_ERROR_NONE;
}

int32_t HWCSession::ValidateDisplay(hwc2_device_t *device, hwc2_display_t display,
                                    uint32_t *out_num_types, uint32_t *out_num_requests) {
  //  out_num_types and out_num_requests will be non-NULL
  if (!device) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  DTRACE_SCOPED();
  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  // TODO(user): Handle secure session, handle QDCM solid fill
  auto status = HWC2::Error::BadDisplay;
  {
    SEQUENCE_ENTRY_SCOPE_LOCK(locker_[display]);
    if (hwc_session->hwc_display_[display]) {
      status = hwc_session->ValidateDisplayInternal(display, out_num_types, out_num_requests);
    }
  }

  // Sequence locking currently begins on Validate, so cancel the sequence lock on failures
  if (status != HWC2::Error::None && status != HWC2::Error::HasChanges) {
    SEQUENCE_CANCEL_SCOPE_LOCK(locker_[display]);
  }

  return INT32(status);
}

hwc2_function_pointer_t HWCSession::GetFunction(struct hwc2_device *device,
                                                int32_t int_descriptor) {
  auto descriptor = static_cast<HWC2::FunctionDescriptor>(int_descriptor);

  switch (descriptor) {
    case HWC2::FunctionDescriptor::AcceptDisplayChanges:
      return AsFP<HWC2_PFN_ACCEPT_DISPLAY_CHANGES>(HWCSession::AcceptDisplayChanges);
    case HWC2::FunctionDescriptor::CreateLayer:
      return AsFP<HWC2_PFN_CREATE_LAYER>(CreateLayer);
    case HWC2::FunctionDescriptor::CreateVirtualDisplay:
      return AsFP<HWC2_PFN_CREATE_VIRTUAL_DISPLAY>(HWCSession::CreateVirtualDisplay);
    case HWC2::FunctionDescriptor::DestroyLayer:
      return AsFP<HWC2_PFN_DESTROY_LAYER>(DestroyLayer);
    case HWC2::FunctionDescriptor::DestroyVirtualDisplay:
      return AsFP<HWC2_PFN_DESTROY_VIRTUAL_DISPLAY>(HWCSession::DestroyVirtualDisplay);
    case HWC2::FunctionDescriptor::Dump:
      return AsFP<HWC2_PFN_DUMP>(HWCSession::Dump);
    case HWC2::FunctionDescriptor::GetActiveConfig:
      return AsFP<HWC2_PFN_GET_ACTIVE_CONFIG>(GetActiveConfig);
    case HWC2::FunctionDescriptor::GetChangedCompositionTypes:
      return AsFP<HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES>(GetChangedCompositionTypes);
    case HWC2::FunctionDescriptor::GetClientTargetSupport:
      return AsFP<HWC2_PFN_GET_CLIENT_TARGET_SUPPORT>(GetClientTargetSupport);
    case HWC2::FunctionDescriptor::GetColorModes:
      return AsFP<HWC2_PFN_GET_COLOR_MODES>(GetColorModes);
    case HWC2::FunctionDescriptor::GetDisplayAttribute:
      return AsFP<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(GetDisplayAttribute);
    case HWC2::FunctionDescriptor::GetDisplayConfigs:
      return AsFP<HWC2_PFN_GET_DISPLAY_CONFIGS>(GetDisplayConfigs);
    case HWC2::FunctionDescriptor::GetDisplayName:
      return AsFP<HWC2_PFN_GET_DISPLAY_NAME>(GetDisplayName);
    case HWC2::FunctionDescriptor::GetDisplayRequests:
      return AsFP<HWC2_PFN_GET_DISPLAY_REQUESTS>(GetDisplayRequests);
    case HWC2::FunctionDescriptor::GetDisplayType:
      return AsFP<HWC2_PFN_GET_DISPLAY_TYPE>(GetDisplayType);
    case HWC2::FunctionDescriptor::GetHdrCapabilities:
      return AsFP<HWC2_PFN_GET_HDR_CAPABILITIES>(GetHdrCapabilities);
    case HWC2::FunctionDescriptor::GetDozeSupport:
      return AsFP<HWC2_PFN_GET_DOZE_SUPPORT>(GetDozeSupport);
    case HWC2::FunctionDescriptor::GetMaxVirtualDisplayCount:
      return AsFP<HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT>(GetMaxVirtualDisplayCount);
    case HWC2::FunctionDescriptor::GetReleaseFences:
      return AsFP<HWC2_PFN_GET_RELEASE_FENCES>(GetReleaseFences);
    case HWC2::FunctionDescriptor::PresentDisplay:
      return AsFP<HWC2_PFN_PRESENT_DISPLAY>(PresentDisplay);
    case HWC2::FunctionDescriptor::RegisterCallback:
      return AsFP<HWC2_PFN_REGISTER_CALLBACK>(RegisterCallback);
    case HWC2::FunctionDescriptor::SetActiveConfig:
      return AsFP<HWC2_PFN_SET_ACTIVE_CONFIG>(SetActiveConfig);
    case HWC2::FunctionDescriptor::SetClientTarget:
      return AsFP<HWC2_PFN_SET_CLIENT_TARGET>(SetClientTarget);
    case HWC2::FunctionDescriptor::SetColorMode:
      return AsFP<HWC2_PFN_SET_COLOR_MODE>(SetColorMode);
    case HWC2::FunctionDescriptor::SetColorTransform:
      return AsFP<HWC2_PFN_SET_COLOR_TRANSFORM>(SetColorTransform);
    case HWC2::FunctionDescriptor::SetCursorPosition:
      return AsFP<HWC2_PFN_SET_CURSOR_POSITION>(SetCursorPosition);
    case HWC2::FunctionDescriptor::SetLayerBlendMode:
      return AsFP<HWC2_PFN_SET_LAYER_BLEND_MODE>(SetLayerBlendMode);
    case HWC2::FunctionDescriptor::SetLayerBuffer:
      return AsFP<HWC2_PFN_SET_LAYER_BUFFER>(SetLayerBuffer);
    case HWC2::FunctionDescriptor::SetLayerColor:
      return AsFP<HWC2_PFN_SET_LAYER_COLOR>(SetLayerColor);
    case HWC2::FunctionDescriptor::SetLayerCompositionType:
      return AsFP<HWC2_PFN_SET_LAYER_COMPOSITION_TYPE>(SetLayerCompositionType);
    case HWC2::FunctionDescriptor::SetLayerDataspace:
      return AsFP<HWC2_PFN_SET_LAYER_DATASPACE>(SetLayerDataspace);
    case HWC2::FunctionDescriptor::SetLayerDisplayFrame:
      return AsFP<HWC2_PFN_SET_LAYER_DISPLAY_FRAME>(SetLayerDisplayFrame);
    case HWC2::FunctionDescriptor::SetLayerPlaneAlpha:
      return AsFP<HWC2_PFN_SET_LAYER_PLANE_ALPHA>(SetLayerPlaneAlpha);
    // Sideband stream is not supported
    // case HWC2::FunctionDescriptor::SetLayerSidebandStream:
    case HWC2::FunctionDescriptor::SetLayerSourceCrop:
      return AsFP<HWC2_PFN_SET_LAYER_SOURCE_CROP>(SetLayerSourceCrop);
    case HWC2::FunctionDescriptor::SetLayerSurfaceDamage:
      return AsFP<HWC2_PFN_SET_LAYER_SURFACE_DAMAGE>(SetLayerSurfaceDamage);
    case HWC2::FunctionDescriptor::SetLayerTransform:
      return AsFP<HWC2_PFN_SET_LAYER_TRANSFORM>(SetLayerTransform);
    case HWC2::FunctionDescriptor::SetLayerVisibleRegion:
      return AsFP<HWC2_PFN_SET_LAYER_VISIBLE_REGION>(SetLayerVisibleRegion);
    case HWC2::FunctionDescriptor::SetLayerZOrder:
      return AsFP<HWC2_PFN_SET_LAYER_Z_ORDER>(SetLayerZOrder);
    case HWC2::FunctionDescriptor::SetOutputBuffer:
      return AsFP<HWC2_PFN_SET_OUTPUT_BUFFER>(SetOutputBuffer);
    case HWC2::FunctionDescriptor::SetPowerMode:
      return AsFP<HWC2_PFN_SET_POWER_MODE>(SetPowerMode);
    case HWC2::FunctionDescriptor::SetVsyncEnabled:
      return AsFP<HWC2_PFN_SET_VSYNC_ENABLED>(SetVsyncEnabled);
    case HWC2::FunctionDescriptor::ValidateDisplay:
      return AsFP<HWC2_PFN_VALIDATE_DISPLAY>(HWCSession::ValidateDisplay);
    case HWC2::FunctionDescriptor::GetDisplayIdentificationData:
      return AsFP<HWC2_PFN_GET_DISPLAY_IDENTIFICATION_DATA>
             (HWCSession::GetDisplayIdentificationData);
    case HWC2::FunctionDescriptor::GetPerFrameMetadataKeys:
      return AsFP<HWC2_PFN_GET_PER_FRAME_METADATA_KEYS>(GetPerFrameMetadataKeys);
    case HWC2::FunctionDescriptor::SetLayerPerFrameMetadata:
      return AsFP<HWC2_PFN_SET_LAYER_PER_FRAME_METADATA>(SetLayerPerFrameMetadata);
    case HWC2::FunctionDescriptor::GetRenderIntents:
      return AsFP<HWC2_PFN_GET_RENDER_INTENTS>(HWCSession::GetRenderIntents);
    case HWC2::FunctionDescriptor::SetColorModeWithRenderIntent:
      return AsFP<HWC2_PFN_SET_COLOR_MODE_WITH_RENDER_INTENT>
             (HWCSession::SetColorModeWithRenderIntent);
    case HWC2::FunctionDescriptor::GetDisplayCapabilities:
      return AsFP<HWC2_PFN_GET_DISPLAY_CAPABILITIES>(HWCSession::GetDisplayCapabilities);
    case HWC2::FunctionDescriptor::GetDisplayBrightnessSupport:
      return AsFP<HWC2_PFN_GET_DISPLAY_BRIGHTNESS_SUPPORT>(HWCSession::GetDisplayBrightnessSupport);
    case HWC2::FunctionDescriptor::SetDisplayBrightness:
      return AsFP<HWC2_PFN_SET_DISPLAY_BRIGHTNESS>(HWCSession::SetDisplayBrightness);
    default:
      DLOGD("Unknown/Unimplemented function descriptor: %d (%s)", int_descriptor,
            to_string(descriptor).c_str());
      return nullptr;
  }
  return nullptr;
}

HWC2::Error HWCSession::CreateVirtualDisplayObj(uint32_t width, uint32_t height, int32_t *format,
                                                hwc2_display_t *out_display_id) {
  if (!client_connected_) {
    DLOGE("Client is not ready yet.");
    return HWC2::Error::BadDisplay;
  }

  HWDisplaysInfo hw_displays_info = {};
  DisplayError error = core_intf_->GetDisplaysStatus(&hw_displays_info);
  if (error != kErrorNone) {
    DLOGE("Failed to get connected display list. Error = %d", error);
    return HWC2::Error::BadDisplay;
  }

  // Lock confined to this scope
  int status = -EINVAL;
  for (auto &iter : hw_displays_info) {
    auto &info = iter.second;
    if (info.display_type != kVirtual) {
      continue;
    }

    for (auto &map_info : map_info_virtual_) {
      hwc2_display_t client_id = map_info.client_id;
      {
        SCOPE_LOCK(locker_[client_id]);
        auto &hwc_display = hwc_display_[client_id];
        if (hwc_display) {
          continue;
        }

        status = HWCDisplayVirtual::Create(core_intf_, &buffer_allocator_, &callbacks_, client_id,
                                           info.display_id, width, height, format, &hwc_display);
        // TODO(user): validate width and height support
        if (status) {
          return HWC2::Error::BadDisplay;
        }

        is_hdr_display_[UINT32(client_id)] = HasHDRSupport(hwc_display);
        DLOGI("Created virtual display id:% " PRIu64 " with res: %dx%d", client_id, width, height);

        *out_display_id = client_id;
        map_info.disp_type = info.display_type;
        map_info.sdm_id = info.display_id;
        break;
      }
    }
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);
  hwc_display_[HWC_DISPLAY_PRIMARY]->ResetValidation();

  return HWC2::Error::None;
}

hwc2_display_t HWCSession::GetNextBuiltinIndex() {
  for (auto &map_info : map_info_builtin_) {
    if (hwc_display_[map_info.client_id]) {
      return map_info.client_id;
    }
  }
  return 0;
}

bool HWCSession::GetSecondBuiltinStatus() {
  if (!GetNextBuiltinIndex()) {
    // Single Builtin
    return false;
  }
  auto &hwc_display = hwc_display_[GetNextBuiltinIndex()];
  if (hwc_display) {
    return hwc_display->GetLastPowerMode() != HWC2::PowerMode::Off;
  }
  return false;
}

void HWCSession::HandleConcurrency(hwc2_display_t disp) {
  if (!primary_ready_) {
    DLOGI("Primary isnt ready yet!!");
    return;
  }

  hwc2_display_t vir_disp_idx = (hwc2_display_t)GetDisplayIndex(qdutils::DISPLAY_VIRTUAL);
  hwc2_display_t ext_disp_idx = (hwc2_display_t)GetDisplayIndex(qdutils::DISPLAY_EXTERNAL);
  bool vir_disp_present = (vir_disp_idx < HWCCallbacks::kNumDisplays) &&
                          hwc_display_[vir_disp_idx];
  bool ext_disp_present = (ext_disp_idx < HWCCallbacks::kNumDisplays) &&
                          hwc_display_[ext_disp_idx];
  // Valid Concurrencies
  // For two builtins   --> Builtin + Builtin
  // For Single Builtin --> Builtin + Virtual OR Builtin + External

  bool sec_builtin_active = GetSecondBuiltinStatus();

  DLOGI("sec_builtin_active %d", sec_builtin_active);
  if (disp == GetNextBuiltinIndex()) {
    // Activate non-built_in displays if any.
    if (sec_builtin_active) {
      if (ext_disp_present) {
        ActivateDisplay(ext_disp_idx, false);
      }
      if (vir_disp_present) {
        ActivateDisplay(vir_disp_idx, false);
      }
    } else {
      // Activate One of the two connected displays.
      if (ext_disp_present) {
        ActivateDisplay(ext_disp_idx, true);
      } else if (vir_disp_present) {
        ActivateDisplay(vir_disp_idx, true);
      }
    }
    return;
  }

  NonBuiltinConcurrency(disp, sec_builtin_active);
}

void HWCSession::NonBuiltinConcurrency(hwc2_display_t disp, bool builtin_active) {
  hwc2_display_t vir_disp_idx = (hwc2_display_t)GetDisplayIndex(qdutils::DISPLAY_VIRTUAL);
  hwc2_display_t ext_disp_idx = (hwc2_display_t)GetDisplayIndex(qdutils::DISPLAY_EXTERNAL);

  if ((disp != ext_disp_idx) && (disp != vir_disp_idx)) {
    return;
  }

  bool display_created = hwc_display_[disp];
  // Virtual and External cant be active at the same time.
  hwc2_display_t cocu_disp = (disp == ext_disp_idx) ? vir_disp_idx
                             : ext_disp_idx;
  bool cocu_disp_present = (cocu_disp < HWCCallbacks::kNumDisplays) &&
                            hwc_display_[cocu_disp];
  DLOGI("Disp: %d created: %d cocu_disp %d", disp, display_created, cocu_disp);
  if (display_created) {
    if (builtin_active || cocu_disp_present) {
      ActivateDisplay(disp, false);
    }
  } else {
    // Activate pending Virtual Display if any.
    if (!builtin_active && cocu_disp_present) {
      ActivateDisplay(cocu_disp, true);
    }
  }
}

// Qclient methods
android::status_t HWCSession::notifyCallback(uint32_t command, const android::Parcel *input_parcel,
                                             android::Parcel *output_parcel) {
  android::status_t status = -EINVAL;

  switch (command) {
    case qService::IQService::DYNAMIC_DEBUG:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = 0;
      DynamicDebug(input_parcel);
      break;

    case qService::IQService::SCREEN_REFRESH:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = RefreshScreen(input_parcel);
      break;

    case qService::IQService::SET_IDLE_TIMEOUT:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetIdleTimeout(UINT32(input_parcel->readInt32()));
      break;

    case qService::IQService::SET_FRAME_DUMP_CONFIG:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetFrameDumpConfig(input_parcel);
      break;

    case qService::IQService::SET_MAX_PIPES_PER_MIXER:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetMaxMixerStages(input_parcel);
      break;

    case qService::IQService::SET_DISPLAY_MODE:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetDisplayMode(input_parcel);
      break;

    case qService::IQService::SET_SECONDARY_DISPLAY_STATUS: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int disp_id = INT(input_parcel->readInt32());
        HWCDisplay::DisplayStatus disp_status =
              static_cast<HWCDisplay::DisplayStatus>(input_parcel->readInt32());
        status = SetDisplayStatus(disp_id, disp_status);
        output_parcel->writeInt32(status);
      }
      break;

    case qService::IQService::CONFIGURE_DYN_REFRESH_RATE:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = ConfigureRefreshRate(input_parcel);
      break;

    case qService::IQService::SET_VIEW_FRAME:
      status = 0;
      break;

    case qService::IQService::TOGGLE_SCREEN_UPDATES: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int32_t input = input_parcel->readInt32();
        status = ToggleScreenUpdate(input == 1);
        output_parcel->writeInt32(status);
      }
      break;

    case qService::IQService::QDCM_SVC_CMDS:
      if (!input_parcel || !output_parcel) {
        DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
        break;
      }
      status = QdcmCMDHandler(input_parcel, output_parcel);
      break;

    case qService::IQService::MIN_HDCP_ENCRYPTION_LEVEL_CHANGED: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int disp_id = input_parcel->readInt32();
        uint32_t min_enc_level = UINT32(input_parcel->readInt32());
        status = MinHdcpEncryptionLevelChanged(disp_id, min_enc_level);
        output_parcel->writeInt32(status);
      }
      break;

    case qService::IQService::CONTROL_PARTIAL_UPDATE: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int disp_id = input_parcel->readInt32();
        uint32_t enable = UINT32(input_parcel->readInt32());
        status = ControlPartialUpdate(disp_id, enable == 1);
        output_parcel->writeInt32(status);
      }
      break;

    case qService::IQService::SET_ACTIVE_CONFIG: {
        if (!input_parcel) {
          DLOGE("QService command = %d: input_parcel needed.", command);
          break;
        }
        uint32_t config = UINT32(input_parcel->readInt32());
        int disp_id = input_parcel->readInt32();
        status = SetActiveConfigIndex(disp_id, config);
      }
      break;

    case qService::IQService::GET_ACTIVE_CONFIG: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int disp_id = input_parcel->readInt32();
        uint32_t config = 0;
        status = GetActiveConfigIndex(disp_id, &config);
        output_parcel->writeInt32(INT(config));
      }
      break;

    case qService::IQService::GET_CONFIG_COUNT: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int disp_id = input_parcel->readInt32();
        uint32_t count = 0;
        status = GetConfigCount(disp_id, &count);
        output_parcel->writeInt32(INT(count));
      }
      break;

    case qService::IQService::GET_DISPLAY_ATTRIBUTES_FOR_CONFIG:
      if (!input_parcel || !output_parcel) {
        DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
        break;
      }
      status = GetDisplayAttributesForConfig(input_parcel, output_parcel);
      break;

    case qService::IQService::GET_PANEL_BRIGHTNESS: {
        if (!output_parcel) {
          DLOGE("QService command = %d: output_parcel needed.", command);
          break;
        }
        float brightness = -1.0f;
        uint32_t display = input_parcel->readUint32();
        status = getDisplayBrightness(display, &brightness);
        if (brightness == -1.0f) {
          output_parcel->writeInt32(0);
        } else {
          output_parcel->writeInt32(INT32(brightness*254 + 1));
        }
      }
      break;

    case qService::IQService::SET_PANEL_BRIGHTNESS: {
        if (!input_parcel || !output_parcel) {
          DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
          break;
        }
        int level = input_parcel->readInt32();
        hwc2_device_t *device = static_cast<hwc2_device_t *>(this);
        if (level == 0) {
          status = SetDisplayBrightness(device, HWC_DISPLAY_PRIMARY, -1.0f);
        } else {
          status = SetDisplayBrightness(device, HWC_DISPLAY_PRIMARY, (level - 1)/254.0f);
        }
        output_parcel->writeInt32(status);
      }
      break;

    case qService::IQService::GET_DISPLAY_VISIBLE_REGION:
      if (!input_parcel || !output_parcel) {
        DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
        break;
      }
      status = GetVisibleDisplayRect(input_parcel, output_parcel);
      break;

    case qService::IQService::SET_CAMERA_STATUS: {
        if (!input_parcel) {
          DLOGE("QService command = %d: input_parcel needed.", command);
          break;
        }
        uint32_t camera_status = UINT32(input_parcel->readInt32());
        status = SetCameraLaunchStatus(camera_status);
      }
      break;

    case qService::IQService::GET_BW_TRANSACTION_STATUS: {
        if (!output_parcel) {
          DLOGE("QService command = %d: output_parcel needed.", command);
          break;
        }
        bool state = true;
        status = DisplayBWTransactionPending(&state);
        output_parcel->writeInt32(state);
      }
      break;

    case qService::IQService::SET_LAYER_MIXER_RESOLUTION:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetMixerResolution(input_parcel);
      break;

    case qService::IQService::SET_COLOR_MODE:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetColorModeOverride(input_parcel);
      break;

    case qService::IQService::SET_COLOR_MODE_BY_ID:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetColorModeById(input_parcel);
      break;

    case qService::IQService::GET_COMPOSER_STATUS:
      if (!output_parcel) {
        DLOGE("QService command = %d: output_parcel needed.", command);
        break;
      }
      status = 0;
      output_parcel->writeInt32(getComposerStatus());
      break;

    case qService::IQService::SET_DSI_CLK:
      if (!input_parcel) {
        DLOGE("QService command = %d: input_parcel needed.", command);
        break;
      }
      status = SetDsiClk(input_parcel);
      break;

    case qService::IQService::GET_DSI_CLK:
      if (!input_parcel || !output_parcel) {
        DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
        break;
      }
      status = GetDsiClk(input_parcel, output_parcel);
      break;

    case qService::IQService::GET_SUPPORTED_DSI_CLK:
      if (!input_parcel || !output_parcel) {
        DLOGE("QService command = %d: input_parcel and output_parcel needed.", command);
        break;
      }
      status = GetSupportedDsiClk(input_parcel, output_parcel);
      break;

    default:
      DLOGW("QService command = %d is not supported.", command);
      break;
  }

  return status;
}

android::status_t HWCSession::getComposerStatus() {
  return is_composer_up_;
}

android::status_t HWCSession::GetDisplayAttributesForConfig(const android::Parcel *input_parcel,
                                                            android::Parcel *output_parcel) {
  int config = input_parcel->readInt32();
  int dpy = input_parcel->readInt32();
  int error = android::BAD_VALUE;
  DisplayConfigVariableInfo display_attributes;

  int disp_idx = GetDisplayIndex(dpy);
  if (disp_idx == -1 || config < 0) {
    DLOGE("Invalid display = %d, or config = %d", dpy, config);
    return android::BAD_VALUE;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
  if (hwc_display_[disp_idx]) {
    error = hwc_display_[disp_idx]->GetDisplayAttributesForConfig(config, &display_attributes);
    if (error == 0) {
      output_parcel->writeInt32(INT(display_attributes.vsync_period_ns));
      output_parcel->writeInt32(INT(display_attributes.x_pixels));
      output_parcel->writeInt32(INT(display_attributes.y_pixels));
      output_parcel->writeFloat(display_attributes.x_dpi);
      output_parcel->writeFloat(display_attributes.y_dpi);
      output_parcel->writeInt32(0);  // Panel type, unsupported.
    }
  }

  return error;
}

android::status_t HWCSession::ConfigureRefreshRate(const android::Parcel *input_parcel) {
  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);

  uint32_t operation = UINT32(input_parcel->readInt32());
  HWCDisplay *hwc_display = hwc_display_[HWC_DISPLAY_PRIMARY];

  if (!hwc_display) {
    DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
    return -ENODEV;
  }

  switch (operation) {
    case qdutils::DISABLE_METADATA_DYN_REFRESH_RATE:
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_METADATA_DYN_REFRESH_RATE, false);

    case qdutils::ENABLE_METADATA_DYN_REFRESH_RATE:
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_METADATA_DYN_REFRESH_RATE, true);

    case qdutils::SET_BINDER_DYN_REFRESH_RATE: {
      uint32_t refresh_rate = UINT32(input_parcel->readInt32());
      return hwc_display->Perform(HWCDisplayBuiltIn::SET_BINDER_DYN_REFRESH_RATE, refresh_rate);
    }

    default:
      DLOGW("Invalid operation %d", operation);
      return -EINVAL;
  }

  return 0;
}

android::status_t HWCSession::SetDisplayMode(const android::Parcel *input_parcel) {
  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);

  if (!hwc_display_[HWC_DISPLAY_PRIMARY]) {
    DLOGW("Display = %d is not connected.", HWC_DISPLAY_PRIMARY);
    return -ENODEV;
  }

  uint32_t mode = UINT32(input_parcel->readInt32());
  return hwc_display_[HWC_DISPLAY_PRIMARY]->Perform(HWCDisplayBuiltIn::SET_DISPLAY_MODE, mode);
}

android::status_t HWCSession::SetMaxMixerStages(const android::Parcel *input_parcel) {
  DisplayError error = kErrorNone;
  std::bitset<32> bit_mask_display_type = UINT32(input_parcel->readInt32());
  uint32_t max_mixer_stages = UINT32(input_parcel->readInt32());
  android::status_t status = 0;

  for (uint32_t i = 0; i < 32; i++) {
    if (!bit_mask_display_type.test(i)) {
      continue;
    }
    int disp_idx = GetDisplayIndex(INT(i));
    if (disp_idx == -1) {
      continue;
    }
    SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
    auto &hwc_display = hwc_display_[disp_idx];
    if (!hwc_display) {
      DLOGW("Display = %d is not connected.", disp_idx);
      status = (status)? status : -ENODEV;  // Return higher priority error.
      continue;
    }

    error = hwc_display->SetMaxMixerStages(max_mixer_stages);
    if (error != kErrorNone) {
      status = -EINVAL;
    }
  }

  return status;
}

android::status_t HWCSession::SetFrameDumpConfig(const android::Parcel *input_parcel) {
  uint32_t frame_dump_count = UINT32(input_parcel->readInt32());
  std::bitset<32> bit_mask_display_type = UINT32(input_parcel->readInt32());
  uint32_t bit_mask_layer_type = UINT32(input_parcel->readInt32());
  android::status_t status = 0;

  for (uint32_t i = 0; i < 32; i++) {
    if (!bit_mask_display_type.test(i)) {
      continue;
    }
    int disp_idx = GetDisplayIndex(INT(i));
    if (disp_idx == -1) {
      continue;
    }
    SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
    auto &hwc_display = hwc_display_[disp_idx];
    if (!hwc_display) {
      DLOGW("Display = %d is not connected.", disp_idx);
      status = (status)? status : -ENODEV;  // Return higher priority error.
      continue;
    }

    HWC2::Error error = hwc_display->SetFrameDumpConfig(frame_dump_count, bit_mask_layer_type);
    if (error != HWC2::Error::None) {
      status = (HWC2::Error::NoResources == error) ? -ENOMEM : -EINVAL;
    }
  }

  return status;
}

android::status_t HWCSession::SetMixerResolution(const android::Parcel *input_parcel) {
  DisplayError error = kErrorNone;
  uint32_t dpy = UINT32(input_parcel->readInt32());

  if (dpy != HWC_DISPLAY_PRIMARY) {
    DLOGW("Resolution change not supported for this display = %d", dpy);
    return -EINVAL;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);
  if (!hwc_display_[HWC_DISPLAY_PRIMARY]) {
    DLOGW("Primary display is not initialized");
    return -ENODEV;
  }

  uint32_t width = UINT32(input_parcel->readInt32());
  uint32_t height = UINT32(input_parcel->readInt32());

  error = hwc_display_[HWC_DISPLAY_PRIMARY]->SetMixerResolution(width, height);
  if (error != kErrorNone) {
    return -EINVAL;
  }

  return 0;
}

android::status_t HWCSession::SetColorModeOverride(const android::Parcel *input_parcel) {
  int display = static_cast<int>(input_parcel->readInt32());
  auto mode = static_cast<android_color_mode_t>(input_parcel->readInt32());
  auto device = static_cast<hwc2_device_t *>(this);

  int disp_idx = GetDisplayIndex(display);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", display);
    return -EINVAL;
  }

  auto err = CallDisplayFunction(device, static_cast<hwc2_display_t>(disp_idx),
                                 &HWCDisplay::SetColorMode, mode);
  if (err != HWC2_ERROR_NONE)
    return -EINVAL;

  return 0;
}

android::status_t HWCSession::SetColorModeById(const android::Parcel *input_parcel) {
  int display = input_parcel->readInt32();
  auto mode = input_parcel->readInt32();
  auto device = static_cast<hwc2_device_t *>(this);

  int disp_idx = GetDisplayIndex(display);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", display);
    return -EINVAL;
  }

  auto err = CallDisplayFunction(device, static_cast<hwc2_display_t>(disp_idx),
                                 &HWCDisplay::SetColorModeById, mode);
  if (err != HWC2_ERROR_NONE)
    return -EINVAL;

  return 0;
}

android::status_t HWCSession::RefreshScreen(const android::Parcel *input_parcel) {
  int display = input_parcel->readInt32();

  int disp_idx = GetDisplayIndex(display);
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", display);
    return -EINVAL;
  }

  Refresh(static_cast<hwc2_display_t>(disp_idx));

  return 0;
}

void HWCSession::DynamicDebug(const android::Parcel *input_parcel) {
  int type = input_parcel->readInt32();
  bool enable = (input_parcel->readInt32() > 0);
  DLOGI("type = %d enable = %d", type, enable);
  int verbose_level = input_parcel->readInt32();

  switch (type) {
    case qService::IQService::DEBUG_ALL:
      HWCDebugHandler::DebugAll(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_MDPCOMP:
      HWCDebugHandler::DebugStrategy(enable, verbose_level);
      HWCDebugHandler::DebugCompManager(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_PIPE_LIFECYCLE:
      HWCDebugHandler::DebugResources(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_DRIVER_CONFIG:
      HWCDebugHandler::DebugDriverConfig(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_ROTATOR:
      HWCDebugHandler::DebugResources(enable, verbose_level);
      HWCDebugHandler::DebugDriverConfig(enable, verbose_level);
      HWCDebugHandler::DebugRotator(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_QDCM:
      HWCDebugHandler::DebugQdcm(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_SCALAR:
      HWCDebugHandler::DebugScalar(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_CLIENT:
      HWCDebugHandler::DebugClient(enable, verbose_level);
      break;

    case qService::IQService::DEBUG_DISPLAY:
      HWCDebugHandler::DebugDisplay(enable, verbose_level);
      break;

    default:
      DLOGW("type = %d is not supported", type);
  }
}

android::status_t HWCSession::QdcmCMDDispatch(uint32_t display_id,
                                              const PPDisplayAPIPayload &req_payload,
                                              PPDisplayAPIPayload *resp_payload,
                                              PPPendingParams *pending_action) {
  int ret = 0;
  bool is_physical_display = false;

  if (display_id >= HWCCallbacks::kNumDisplays || !hwc_display_[display_id]) {
      DLOGW("Invalid display id or display = %d is not connected.", display_id);
      return -ENODEV;
  }

  if (display_id == map_info_primary_.client_id) {
    is_physical_display = true;
  } else {
    for (auto &map_info : map_info_builtin_) {
      if (map_info.client_id == display_id) {
        is_physical_display = true;
        break;
     }
    }
  }

  if (!is_physical_display) {
    DLOGW("Skipping QDCM command dispatch on display = %d", display_id);
    return ret;
  }

  ret = hwc_display_[display_id]->ColorSVCRequestRoute(req_payload,
                                                       resp_payload,
                                                       pending_action);

  return ret;
}

android::status_t HWCSession::QdcmCMDHandler(const android::Parcel *input_parcel,
                                             android::Parcel *output_parcel) {
  int ret = 0;
  float *brightness = NULL;
  uint32_t display_id(0);
  PPPendingParams pending_action;
  PPDisplayAPIPayload resp_payload, req_payload;
  disp_id_config *disp_id = NULL;

  if (!color_mgr_) {
    DLOGW("color_mgr_ not initialized.");
    return -ENOENT;
  }

  pending_action.action = kNoAction;
  pending_action.params = NULL;

  // Read display_id, payload_size and payload from in_parcel.
  ret = HWCColorManager::CreatePayloadFromParcel(*input_parcel, &display_id, &req_payload);
  if (!ret) {
    ret = QdcmCMDDispatch(display_id, req_payload, &resp_payload, &pending_action);
  }

  if (ret || pending_action.action == kNoAction) {
    output_parcel->writeInt32(ret);  // first field in out parcel indicates return code.
    if (pending_action.action == kNoAction) {
      HWCColorManager::MarshallStructIntoParcel(resp_payload, output_parcel);
    }
    req_payload.DestroyPayload();
    resp_payload.DestroyPayload();
    return ret;
  }

  int32_t action = pending_action.action;
  int count = -1;
  bool invalidate_needed = true;
  while (action > 0) {
    count++;
    int32_t bit = (action & 1);
    action = action >> 1;

    if (!bit)
      continue;

    DLOGV_IF(kTagQDCM, "pending action = %d, display_id = %d", BITMAP(count), display_id);
    switch (BITMAP(count)) {
    case kInvalidating:
      {
        invalidate_needed = false;
        Refresh(display_id);
      }
      break;
    case kEnterQDCMMode:
      ret = color_mgr_->EnableQDCMMode(true, hwc_display_[display_id]);
      break;
    case kExitQDCMMode:
      ret = color_mgr_->EnableQDCMMode(false, hwc_display_[display_id]);
      break;
    case kApplySolidFill:
      {
        SCOPE_LOCK(locker_[display_id]);
        ret = color_mgr_->SetSolidFill(pending_action.params,
                                       true, hwc_display_[display_id]);
      }
      Refresh(display_id);
      usleep(kSolidFillDelay);
      break;
    case kDisableSolidFill:
      {
      SCOPE_LOCK(locker_[display_id]);
      ret = color_mgr_->SetSolidFill(pending_action.params,
                                     false, hwc_display_[display_id]);
      }
      Refresh(display_id);
      usleep(kSolidFillDelay);
      break;
    case kSetPanelBrightness:
      ret = -EINVAL;
      brightness = reinterpret_cast<float *>(resp_payload.payload);
      if (brightness == NULL) {
        DLOGE("Brightness payload is Null");
      } else {
        ret = INT(SetDisplayBrightness(static_cast<hwc2_device_t *>(this),
                  static_cast<hwc2_display_t>(display_id), *brightness));
      }
      break;
    case kEnableFrameCapture:
      ret = color_mgr_->SetFrameCapture(pending_action.params, true,
                                        hwc_display_[display_id]);
      Refresh(display_id);
      break;
    case kDisableFrameCapture:
      ret = color_mgr_->SetFrameCapture(pending_action.params, false,
                                        hwc_display_[display_id]);
      break;
    case kConfigureDetailedEnhancer:
      ret = color_mgr_->SetDetailedEnhancer(pending_action.params,
                                            hwc_display_[display_id]);
      Refresh(display_id);
      break;
    case kModeSet:
      ret = static_cast<int>
               (hwc_display_[display_id]->RestoreColorTransform());
      Refresh(display_id);
      break;
    case kNoAction:
      break;
    case kMultiDispProc:
      for (auto &map_info : map_info_builtin_) {
        uint32_t id = UINT32(map_info.client_id);
        if (id < HWCCallbacks::kNumDisplays && hwc_display_[id]) {
          int result = 0;
          resp_payload.DestroyPayload();
          result = hwc_display_[id]->ColorSVCRequestRoute(req_payload,
                                                          &resp_payload,
                                                          &pending_action);
          if (result) {
            DLOGW("Failed to dispatch action to disp %d ret %d", id, result);
            ret = result;
          }
        }
      }
      break;
    case kMultiDispGetId:
      ret = resp_payload.CreatePayload<disp_id_config>(disp_id);
      if (ret) {
        DLOGW("Unable to create response payload!");
      } else {
        for (int i = 0; i < HWC_NUM_DISPLAY_TYPES; i++) {
          disp_id->disp_id[i] = INVALID_DISPLAY;
        }
        if (hwc_display_[HWC_DISPLAY_PRIMARY]) {
          disp_id->disp_id[HWC_DISPLAY_PRIMARY] = HWC_DISPLAY_PRIMARY;
        }
        for (auto &map_info : map_info_builtin_) {
          uint64_t id = map_info.client_id;
          if (id < HWCCallbacks::kNumDisplays && hwc_display_[id]) {
            disp_id->disp_id[id] = id;
          }
        }
      }
      break;
    default:
      DLOGW("Invalid pending action = %d!", pending_action.action);
      break;
    }
  }
  // for display API getter case, marshall returned params into out_parcel.
  output_parcel->writeInt32(ret);
  HWCColorManager::MarshallStructIntoParcel(resp_payload, output_parcel);
  req_payload.DestroyPayload();
  resp_payload.DestroyPayload();
  if (invalidate_needed && !hwc_display_[display_id]->CommitPending()) {
    hwc_display_[display_id]->ResetValidation();
  }

  return ret;
}

int GetEventValue(const char *uevent_data, int length, const char *event_info) {
  const char *iterator_str = uevent_data;
  while (((iterator_str - uevent_data) <= length) && (*iterator_str)) {
    const char *pstr = strstr(iterator_str, event_info);
    if (pstr != NULL) {
      return (atoi(iterator_str + strlen(event_info)));
    }
    iterator_str += strlen(iterator_str) + 1;
  }

  return -1;
}

const char *GetTokenValue(const char *uevent_data, int length, const char *token) {
  const char *iterator_str = uevent_data;
  const char *pstr = NULL;
  while (((iterator_str - uevent_data) <= length) && (*iterator_str)) {
    pstr = strstr(iterator_str, token);
    if (pstr) {
      break;
    }
    iterator_str += strlen(iterator_str) + 1;
  }

  if (pstr)
    pstr = pstr+strlen(token);

  return pstr;
}

android::status_t HWCSession::SetDsiClk(const android::Parcel *input_parcel) {
  int disp_id = input_parcel->readInt32();
  uint64_t clk = UINT32(input_parcel->readInt64());
  if (disp_id < 0 || !hwc_display_[disp_id]) {
    return -EINVAL;
  }

  return hwc_display_[disp_id]->SetDynamicDSIClock(clk);
}

android::status_t HWCSession::GetDsiClk(const android::Parcel *input_parcel,
                                        android::Parcel *output_parcel) {
  int disp_id = input_parcel->readInt32();
  if (disp_id < 0 || !hwc_display_[disp_id]) {
    return -EINVAL;
  }

  uint64_t bitrate = 0;
  hwc_display_[disp_id]->GetDynamicDSIClock(&bitrate);
  output_parcel->writeUint64(bitrate);
  return 0;
}

android::status_t HWCSession::GetSupportedDsiClk(const android::Parcel *input_parcel,
                                                 android::Parcel *output_parcel) {
  int disp_id = input_parcel->readInt32();
  if (disp_id < 0 || !hwc_display_[disp_id]) {
    return -EINVAL;
  }

  std::vector<uint64_t> bit_rates;
  hwc_display_[disp_id]->GetSupportedDSIClock(&bit_rates);
  output_parcel->writeInt32(INT32(bit_rates.size()));
  for (auto &bit_rate : bit_rates) {
    output_parcel->writeUint64(bit_rate);
  }
  return 0;
}

void HWCSession::UEventHandler(const char *uevent_data, int length) {
  if (strcasestr(uevent_data, HWC_UEVENT_GRAPHICS_FB0)) {
    DLOGI("Uevent FB0 = %s", uevent_data);
    int panel_reset = GetEventValue(uevent_data, length, "PANEL_ALIVE=");
    if (panel_reset == 0) {
      Refresh(0);
      reset_panel_ = true;
    }
    return;
  }

  if (strcasestr(uevent_data, HWC_UEVENT_DRM_EXT_HOTPLUG)) {
    // MST hotplug will not carry connection status/test pattern etc.
    // Pluggable display handler will check all connection status' and take action accordingly.
    const char *str_status = GetTokenValue(uevent_data, length, "status=");
    const char *str_mst = GetTokenValue(uevent_data, length, "MST_HOTPLUG=");
    if (!str_status && !str_mst) {
      return;
    }

    hpd_bpp_ = GetEventValue(uevent_data, length, "bpp=");
    hpd_pattern_ = GetEventValue(uevent_data, length, "pattern=");
    DLOGI("Uevent = %s, bpp = %d, pattern = %d", uevent_data, hpd_bpp_, hpd_pattern_);
    if (CreatePluggableDisplays(true)) {
      DLOGE("Could not handle hotplug. Event dropped.");
    }

    if (str_status) {
      bool connected = (strncmp(str_status, "connected", strlen("connected")) == 0);
      DLOGI("Connected = %d", connected);
      qservice_->onHdmiHotplug(INT(connected));
    }
  }
}

void HWCSession::ResetPanel() {
  HWC2::Error status;

  DLOGI("Powering off primary");
  status = hwc_display_[HWC_DISPLAY_PRIMARY]->SetPowerMode(HWC2::PowerMode::Off);
  if (status != HWC2::Error::None) {
    DLOGE("power-off on primary failed with error = %d", status);
  }

  DLOGI("Restoring power mode on primary");
  HWC2::PowerMode mode = hwc_display_[HWC_DISPLAY_PRIMARY]->GetLastPowerMode();
  status = hwc_display_[HWC_DISPLAY_PRIMARY]->SetPowerMode(mode);
  if (status != HWC2::Error::None) {
    DLOGE("Setting power mode = %d on primary failed with error = %d", mode, status);
  }

  status = hwc_display_[HWC_DISPLAY_PRIMARY]->SetVsyncEnabled(HWC2::Vsync::Enable);
  if (status != HWC2::Error::None) {
    DLOGE("enabling vsync failed for primary with error = %d", status);
  }

  reset_panel_ = false;
}

int HWCSession::GetVsyncPeriod(int disp) {
  SCOPE_LOCK(locker_[disp]);
  // default value
  int32_t vsync_period = 1000000000l / 60;
  auto attribute = HWC2::Attribute::VsyncPeriod;

  if (hwc_display_[disp]) {
    hwc_display_[disp]->GetDisplayAttribute(0, attribute, &vsync_period);
  }

  return vsync_period;
}

android::status_t HWCSession::GetVisibleDisplayRect(const android::Parcel *input_parcel,
                                                    android::Parcel *output_parcel) {
  int disp_idx = GetDisplayIndex(input_parcel->readInt32());
  if (disp_idx == -1) {
    DLOGE("Invalid display = %d", disp_idx);
    return android::BAD_VALUE;
  }

  SEQUENCE_WAIT_SCOPE_LOCK(locker_[disp_idx]);
  if (!hwc_display_[disp_idx]) {
    return android::NO_INIT;
  }

  hwc_rect_t visible_rect = {0, 0, 0, 0};
  int error = hwc_display_[disp_idx]->GetVisibleDisplayRect(&visible_rect);
  if (error < 0) {
    return error;
  }

  output_parcel->writeInt32(visible_rect.left);
  output_parcel->writeInt32(visible_rect.top);
  output_parcel->writeInt32(visible_rect.right);
  output_parcel->writeInt32(visible_rect.bottom);

  return android::NO_ERROR;
}

void HWCSession::Refresh(hwc2_display_t display) {
  SCOPE_LOCK(callbacks_lock_);
  HWC2::Error err = callbacks_.Refresh(display);
  while (err != HWC2::Error::None) {
    callbacks_lock_.Wait();
    err = callbacks_.Refresh(display);
  }
}

void HWCSession::HotPlug(hwc2_display_t display, HWC2::Connection state) {
  SCOPE_LOCK(callbacks_lock_);
  HWC2::Error err = callbacks_.Hotplug(display, state);
  while (err != HWC2::Error::None) {
    callbacks_lock_.Wait();
    err = callbacks_.Hotplug(display, state);
  }
}

int HWCSession::CreatePrimaryDisplay() {
  int status = 1;
  HWDisplaysInfo hw_displays_info = {};

  DisplayError error = core_intf_->GetDisplaysStatus(&hw_displays_info);
  if (error != kErrorNone) {
    DLOGE("Failed to get connected display list. Error = %d", error);
    return status;
  }

  for (auto &iter : hw_displays_info) {
    auto &info = iter.second;
    if (!info.is_primary) {
      DLOGE("!info.is_primary");
      continue;
    }

    auto hwc_display = &hwc_display_[HWC_DISPLAY_PRIMARY];
    hwc2_display_t client_id = map_info_primary_.client_id;

    DLOGI("Create primary display type = %d, sdm id = %d, client id = %d", info.display_type,
                                                                    info.display_id, client_id);
    if (!info.is_connected && info.display_type == kPluggable) {
      pluggable_is_primary_ = true;
      null_display_active_ = true;
      status = HWCDisplayDummy::Create(core_intf_, &buffer_allocator_, &callbacks_, qservice_,
                                       client_id, info.display_id, hwc_display);
      DLOGI("Pluggable display is primary but not connected!");
    } else if (info.display_type == kBuiltIn) {
      status = HWCDisplayBuiltIn::Create(core_intf_, &buffer_allocator_, &callbacks_, qservice_,
                                         client_id, info.display_id, info.is_primary, hwc_display);
    } else if (info.is_connected && info.display_type == kPluggable) {
      pluggable_is_primary_ = true;
      DLOGI("Pluggable display is primary and is connected!");
      status = HWCDisplayPluggable::Create(core_intf_, &buffer_allocator_, &callbacks_, qservice_,
                                           client_id, info.display_id, 0, 0, false, hwc_display);
    } else {
      DLOGE("Spurious primary display type = %d", info.display_type);
      break;
    }

    if (!status) {
      is_hdr_display_[UINT32(client_id)] = HasHDRSupport(*hwc_display);
      DLOGI("Primary display created.");
      map_info_primary_.disp_type = info.display_type;
      map_info_primary_.sdm_id = info.display_id;

      color_mgr_ = HWCColorManager::CreateColorManager(&buffer_allocator_);
      if (!color_mgr_) {
        DLOGW("Failed to load HWCColorManager.");
      }
    } else {
      DLOGE("Primary display creation failed.");
    }

    // Primary display is found, no need to parse more.
    break;
  }

  return status;
}

int HWCSession::CreatePluggableDisplays(bool delay_hotplug) {
  if (!primary_ready_) {
    DLOGI("Primary display is not ready. Connect displays later if any.");
    return 0;
  }
  if (null_display_active_) {
    SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);
    auto &hwc_display = hwc_display_[HWC_DISPLAY_PRIMARY];
    HWCDisplayDummy::Destroy(hwc_display);
    CoreInterface::DestroyCore();
    DLOGI("Primary pluggable display is connected. Abort!");
    abort();
  }
  HWDisplaysInfo hw_displays_info = {};

  DisplayError error = core_intf_->GetDisplaysStatus(&hw_displays_info);
  if (error != kErrorNone) {
    DLOGE("Failed to get connected display list. Error = %d", error);
    return -EINVAL;
  }

  int status = HandleDisconnectedDisplays(&hw_displays_info);
  if (status) {
    DLOGE("All displays could not be disconnected.");
    return status;
  }

  status = HandleConnectedDisplays(&hw_displays_info, delay_hotplug);
  if (status) {
    DLOGE("All displays could not be connected.");
    return status;
  }

  return 0;
}

int HWCSession::HandleConnectedDisplays(HWDisplaysInfo *hw_displays_info, bool delay_hotplug) {
  int status = 0;
  std::vector<hwc2_display_t> pending_hotplugs = {};

  for (auto &iter : *hw_displays_info) {
    auto &info = iter.second;

    // Do not recreate primary display or if display is not connected.
    if (pluggable_is_primary_) {
      DisplayMapInfo map_info = map_info_primary_;
      hwc2_display_t client_id = map_info.client_id;
      {
        SCOPE_LOCK(locker_[client_id]);
        auto &hwc_display = hwc_display_[client_id];
        if (hwc_display && info.is_primary && info.display_type == kPluggable
            && info.is_connected) {
          DLOGI("Create primary pluggable display, sdm id = %d, client id = %d",
                info.display_id, client_id);
          status = hwc_display->SetState(true);
          if (status) {
            DLOGE("Pluggable display creation failed.");
            return status;
          }
          is_hdr_display_[UINT32(client_id)] = HasHDRSupport(hwc_display);
          DLOGI("Created primary pluggable display successfully: sdm id = %d,"
                "client id = %d", info.display_id, client_id);
          map_info.disp_type = info.display_type;
          map_info.sdm_id = info.display_id;

        }
      }
      {
          SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);
          hwc_display_[HWC_DISPLAY_PRIMARY]->ResetValidation();
      }
      Refresh(0);
    }
    if (!pluggable_is_primary_ && (info.is_primary || info.display_type != kPluggable
        || !info.is_connected)) {
      continue;
    }

    // find an empty slot to create display.
    for (auto &map_info : map_info_pluggable_) {
      hwc2_display_t client_id = map_info.client_id;

      // Lock confined to this scope
      {
        SCOPE_LOCK(locker_[client_id]);
        auto &hwc_display = hwc_display_[client_id];
        if (hwc_display) {
          // Display is already connected.
          continue;
        }

        DLOGI("Create pluggable display, sdm id = %d, client id = %d", info.display_id, client_id);

        // Test pattern generation ?
        map_info.test_pattern = (hpd_bpp_ > 0) && (hpd_pattern_ > 0);
        if (!map_info.test_pattern) {
          status = HWCDisplayPluggable::Create(core_intf_, &buffer_allocator_, &callbacks_,
                                               qservice_, client_id, info.display_id, 0, 0, false,
                                               &hwc_display);
        } else {
          status = HWCDisplayPluggableTest::Create(core_intf_, &buffer_allocator_, &callbacks_,
                                                   qservice_, client_id, info.display_id,
                                                   UINT32(hpd_bpp_), UINT32(hpd_pattern_),
                                                   &hwc_display);
        }

        if (status) {
          DLOGE("Pluggable display creation failed.");
          return status;
        }

        is_hdr_display_[UINT32(client_id)] = HasHDRSupport(hwc_display);
        DLOGI("Created pluggable display successfully: sdm id = %d, client id = %d",
              info.display_id, client_id);
      }

      map_info.disp_type = info.display_type;
      map_info.sdm_id = info.display_id;

      pending_hotplugs.push_back((hwc2_display_t)client_id);

      HandleConcurrency(client_id);
      // Display is created for this sdm id, move to next connected display.
      break;
    }
  }

  // No display was created.
  if (!pending_hotplugs.size()) {
    return 0;
  }

  // Primary display needs revalidation
  {
    SCOPE_LOCK(locker_[HWC_DISPLAY_PRIMARY]);
    hwc_display_[HWC_DISPLAY_PRIMARY]->ResetValidation();
  }

  Refresh(0);

  // Do not sleep if this method is called from client thread.
  if (delay_hotplug) {
    // wait sufficient time to ensure resources are available for new display connection.
    usleep(UINT32(GetVsyncPeriod(HWC_DISPLAY_PRIMARY)) * 2 / 1000);
  }

  for (auto client_id : pending_hotplugs) {
    DLOGI("Notify hotplug connected: client id = %d", client_id);
    callbacks_.Hotplug(client_id, HWC2::Connection::Connected);
    HandleConcurrency(client_id);
  }

  return 0;
}

bool HWCSession::HasHDRSupport(HWCDisplay *hwc_display) {
  // query number of hdr types
  uint32_t out_num_types = 0;
  float out_max_luminance = 0.0f;
  float out_max_average_luminance = 0.0f;
  float out_min_luminance = 0.0f;
  if (hwc_display->GetHdrCapabilities(&out_num_types, nullptr, &out_max_luminance,
                                      &out_max_average_luminance, &out_min_luminance)
                                      != HWC2::Error::None) {
    return false;
  }

  return (out_num_types > 0);
}

int HWCSession::HandleDisconnectedDisplays(HWDisplaysInfo *hw_displays_info) {
  // Destroy pluggable displays which were connected earlier but got disconnected now.
  if (pluggable_is_primary_) {
    bool disconnect = true;
    DisplayMapInfo map_info = map_info_primary_;
    for (auto &iter : *hw_displays_info) {
      auto &info = iter.second;
      if (info.display_id != map_info.sdm_id) {
        continue;
      }
      if (info.is_connected) {
        disconnect = false;
      }
    }
    if (disconnect) {
      DestroyDisplay(&map_info);
    }
  }

  for (auto &map_info : map_info_pluggable_) {
    bool disconnect = true;   // disconnect in case display id is not found in list.

    for (auto &iter : *hw_displays_info) {
      auto &info = iter.second;
      if (info.display_id != map_info.sdm_id) {
        continue;
      }
      if (info.is_connected) {
        disconnect = false;
      }
    }

    if (disconnect) {
      DestroyDisplay(&map_info);
    }
  }

  return 0;
}

void HWCSession::DestroyDisplay(DisplayMapInfo *map_info) {
  switch (map_info->disp_type) {
    case kPluggable:
      DestroyPluggableDisplay(map_info);
      break;
    default:
      DestroyNonPluggableDisplay(map_info);
      break;
    }
}

void HWCSession::DestroyPluggableDisplay(DisplayMapInfo *map_info) {
  hwc2_display_t client_id = map_info->client_id;

  DLOGI("Notify hotplug display disconnected: client id = %d", client_id);
  if (!pluggable_is_primary_) {
    // Notify SurfaceFlinger.
    callbacks_.Hotplug(client_id, HWC2::Connection::Disconnected);
  }
  Refresh(0);
  // wait for sufficient time to ensure sufficient resources are available to process
  // connection.
  usleep(UINT32(GetVsyncPeriod(HWC_DISPLAY_PRIMARY)) * 2 / 1000);

  {
    SCOPE_LOCK(locker_[client_id]);
    auto &hwc_display = hwc_display_[client_id];
    if (!hwc_display) {
      return;
    }
    DLOGI("Destroy display %d-%d, client id = %d", map_info->sdm_id, map_info->disp_type,
         client_id);

    if (pluggable_is_primary_){
      hwc_display_[HWC_DISPLAY_PRIMARY]->SetState(false);
      return;
    }
    is_hdr_display_[UINT32(client_id)] = false;
    if (!map_info->test_pattern) {
      HWCDisplayPluggable::Destroy(hwc_display);
    } else {
      HWCDisplayPluggableTest::Destroy(hwc_display);
    }

    hwc_display = nullptr;
    map_info->Reset();
    HandleConcurrency(client_id);
  }
}

void HWCSession::DestroyNonPluggableDisplay(DisplayMapInfo *map_info) {
  hwc2_display_t client_id = map_info->client_id;

  SCOPE_LOCK(locker_[client_id]);
  auto &hwc_display = hwc_display_[client_id];
  if (!hwc_display) {
    return;
  }
  DLOGI("Destroy display %d-%d, client id = %d", map_info->sdm_id, map_info->disp_type,
        client_id);
  is_hdr_display_[UINT32(client_id)] = false;
  switch (map_info->disp_type) {
    case kBuiltIn:
      HWCDisplayBuiltIn::Destroy(hwc_display);
      break;
    default:
      HWCDisplayVirtual::Destroy(hwc_display);
      break;
    }

    hwc_display = nullptr;
    map_info->Reset();
}

int32_t HWCSession::GetDisplayIdentificationData(hwc2_device_t *device, hwc2_display_t display,
                                                 uint8_t *outPort, uint32_t *outDataSize,
                                                 uint8_t *outData) {
  if (!outPort || !outDataSize) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  return CallDisplayFunction(device, display, &HWCDisplay::GetDisplayIdentificationData, outPort,
                             outDataSize, outData);
}

int32_t HWCSession::GetRenderIntents(hwc2_device_t *device, hwc2_display_t display,
                                int32_t /*ColorMode*/ int_mode, uint32_t *out_num_intents,
                                int32_t /*RenderIntent*/ *int_out_intents) {
  auto mode = static_cast<android_color_mode_t>(int_mode);
  auto out_intents = reinterpret_cast<RenderIntent *>(int_out_intents);
  if (out_num_intents == nullptr) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (!device || display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  if (mode < HAL_COLOR_MODE_NATIVE || mode > HAL_COLOR_MODE_DISPLAY_P3) {
    DLOGE("Invalid ColorMode: %d", mode);
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (out_intents == nullptr) {
    *out_num_intents = 1;
  } else if (out_intents && *out_num_intents > 0) {
    *out_num_intents = 1;
    out_intents[0] = RenderIntent::COLORIMETRIC;
  }

  return HWC2_ERROR_NONE;
}

void HWCSession::ActivateDisplay(hwc2_display_t disp, bool enable) {
  if (!hwc_display_[disp]) {
    return;
  }
  hwc_display_[disp]->ActivateDisplay(enable);
  DLOGI("Disp: %d, Active: %d", disp, enable);
}

int32_t HWCSession::GetDisplayCapabilities(hwc2_device_t *device, hwc2_display_t display,
                                           uint32_t *outNumCapabilities,
                                           uint32_t *outCapabilities) {
  if (!outNumCapabilities || !device) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  HWCDisplay *hwc_display = hwc_session->hwc_display_[display];
  if (!hwc_display) {
    DLOGE("Expected valid hwc_display");
    return HWC2_ERROR_BAD_PARAMETER;
  }
  bool isBuiltin = (hwc_display->GetDisplayClass() == DISPLAY_CLASS_BUILTIN);
  if (!outCapabilities) {
    *outNumCapabilities = 0;
    if (isBuiltin) {
      *outNumCapabilities = 3;
    }
    return HWC2_ERROR_NONE;
  } else {
    if (isBuiltin) {
      // TODO(user): Handle SKIP_CLIENT_COLOR_TRANSFORM based on DSPP availability
      outCapabilities[0] = HWC2_DISPLAY_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
      outCapabilities[1] = HWC2_DISPLAY_CAPABILITY_DOZE;
      outCapabilities[2] = HWC2_DISPLAY_CAPABILITY_BRIGHTNESS;
      *outNumCapabilities = 3;
    }
    return HWC2_ERROR_NONE;
  }
}


int32_t HWCSession::GetDisplayBrightnessSupport(hwc2_device_t *device, hwc2_display_t display,
                                                bool *outSupport) {
  if (!device || !outSupport) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  HWCDisplay *hwc_display = hwc_session->hwc_display_[display];
  if (!hwc_display) {
    DLOGE("Expected valid hwc_display");
    return HWC2_ERROR_BAD_PARAMETER;
  }
  *outSupport = (hwc_display->GetDisplayClass() == DISPLAY_CLASS_BUILTIN);
  return HWC2_ERROR_NONE;
}

int32_t HWCSession::SetDisplayBrightness(hwc2_device_t *device, hwc2_display_t display,
                                         float brightness) {
  if (!device) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  if (display >= HWCCallbacks::kNumDisplays) {
    return HWC2_ERROR_BAD_DISPLAY;
  }

  HWCSession *hwc_session = static_cast<HWCSession *>(device);
  HWCDisplay *hwc_display = hwc_session->hwc_display_[display];

  if (!hwc_display) {
    return HWC2_ERROR_BAD_PARAMETER;
  }

  return INT32(hwc_display->SetPanelBrightness(brightness));
}

HWC2::Error HWCSession::ValidateDisplayInternal(hwc2_display_t display, uint32_t *out_num_types,
                                                uint32_t *out_num_requests) {
  HWCDisplay *hwc_display = hwc_display_[display];
  if (hwc_display->IsInternalValidateState()) {
    // Internal Validation has already been done on display, get the Output params.
    return hwc_display->GetValidateDisplayOutput(out_num_types, out_num_requests);
  }

  if (display == HWC_DISPLAY_PRIMARY) {
    // TODO(user): This can be moved to HWCDisplayPrimary
    if (reset_panel_) {
      DLOGW("panel is in bad state, resetting the panel");
      ResetPanel();
    }

    if (need_invalidate_) {
      Refresh(display);
      need_invalidate_ = false;
    }

    if (color_mgr_) {
      color_mgr_->SetColorModeDetailEnhancer(hwc_display_[display]);
    }
  }

  return hwc_display->Validate(out_num_types, out_num_requests);
}

HWC2::Error HWCSession::PresentDisplayInternal(hwc2_display_t display, int32_t *out_retire_fence) {
  HWCDisplay *hwc_display = hwc_display_[display];
  // If display is in Skip-Validate state and Validate cannot be skipped, do Internal
  // Validation to optimize for the frames which don't require the Client composition.
  if (hwc_display->IsSkipValidateState() && !hwc_display->CanSkipValidate()) {
    uint32_t out_num_types = 0, out_num_requests = 0;
    HWC2::Error error = ValidateDisplayInternal(display, &out_num_types, &out_num_requests);
    if ((error != HWC2::Error::None) || hwc_display->HasClientComposition()) {
      hwc_display->SetValidationState(HWCDisplay::kInternalValidate);
      return HWC2::Error::NotValidated;
    }
  }

  return hwc_display->Present(out_retire_fence);
}

hwc2_display_t HWCSession::GetActiveBuiltinDisplay() {
  hwc2_display_t disp_id = HWCCallbacks::kNumDisplays;
  // Get first active display among primary and built-in displays.
  std::vector<DisplayMapInfo> map_info = {map_info_primary_};
  std::copy(map_info_builtin_.begin(), map_info_builtin_.end(), std::back_inserter(map_info));

  for (auto &info : map_info) {
    SCOPE_LOCK(locker_[info.client_id]);
    auto &hwc_display = hwc_display_[info.client_id];
    if (hwc_display && hwc_display->GetLastPowerMode() != HWC2::PowerMode::Off) {
      disp_id = info.client_id;
      break;
    }
  }

  return disp_id;
}

void HWCSession::updateRefreshRateHint() {
    uint_t mVsyncPeriod = static_cast<uint_t>(GetVsyncPeriod(HWC_DISPLAY_PRIMARY));
    HWC2::PowerMode mPowerModeState = hwc_display_[HWC_DISPLAY_PRIMARY]->GetLastPowerMode();
//    DLOGI("UpdateRefreshrate: powermode=%d, vSyncPeriod=%d", mPowerModeState, mVsyncPeriod);
    if (mVsyncPeriod) {
        mPowerHalHint.signalRefreshRate(mPowerModeState, mVsyncPeriod);
    }
}

}  // namespace sdm
