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

#ifndef __HWC_SESSION_H__
#define __HWC_SESSION_H__

#include <cutils/native_handle.h>
#include <config/device_interface.h>
#include <core/core_interface.h>
#include <utils/locker.h>
#include <qd_utils.h>
#include <display_config.h>
#include <vector>
#include <string>

#include "hwc_callbacks.h"
#include "hwc_layers.h"
#include "hwc_display.h"
#include "hwc_display_builtin.h"
#include "hwc_display_pluggable.h"
#include "hwc_display_dummy.h"
#include "hwc_display_virtual.h"
#include "hwc_display_pluggable_test.h"
#include "hwc_color_manager.h"
#include "hwc_socket_handler.h"
#include "hwc_buffer_sync_handler.h"

#include "worker.h"

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace extension {
namespace pixel {

class IPowerExt;

} // namespace pixel
} // namespace extension
} // namespace power
} // namespace hardware
} // namespace google
} // namespace aidl

namespace sdm {

using ::android::hardware::Return;
using ::android::hardware::hidl_string;

typedef DisplayConfig::DisplayType DispType;

// Create a singleton uevent listener thread valid for life of hardware composer process.
// This thread blocks on uevents poll inside uevent library implementation. This poll exits
// only when there is a valid uevent, it can not be interrupted otherwise. Tieing life cycle
// of this thread with HWC session cause HWC deinitialization to wait infinitely for the
// thread to exit.
class HWCUEventListener {
 public:
  virtual ~HWCUEventListener() {}
  virtual void UEventHandler(const char *uevent_data, int length) = 0;
};

class HWCUEvent {
 public:
  HWCUEvent();
  static void UEventThread(HWCUEvent *hwc_event);
  void Register(HWCUEventListener *uevent_listener);
  inline bool InitDone() { return init_done_; }

 private:
  std::mutex mutex_;
  std::condition_variable caller_cv_;
  HWCUEventListener *uevent_listener_ = nullptr;
  bool init_done_ = false;
};

class HWCSession : hwc2_device_t, HWCUEventListener, public qClient::BnQClient,
                      public DisplayConfig::ClientContext {
 public:
  struct HWCModuleMethods : public hw_module_methods_t {
    HWCModuleMethods() { hw_module_methods_t::open = HWCSession::Open; }
  };

  explicit HWCSession(const hw_module_t *module);
  int Init();
  int Deinit();
  HWC2::Error CreateVirtualDisplayObj(uint32_t width, uint32_t height, int32_t *format,
                                      hwc2_display_t *out_display_id);

  template <typename... Args>
  static int32_t CallDisplayFunction(hwc2_device_t *device, hwc2_display_t display,
                                     HWC2::Error (HWCDisplay::*member)(Args...), Args... args) {
    if (!device) {
      return HWC2_ERROR_BAD_PARAMETER;
    }

    if (display >= HWCCallbacks::kNumDisplays) {
      return HWC2_ERROR_BAD_DISPLAY;
    }

    SCOPE_LOCK(locker_[display]);
    HWCSession *hwc_session = static_cast<HWCSession *>(device);
    auto status = HWC2::Error::BadDisplay;
    if (hwc_session->hwc_display_[display]) {
      auto hwc_display = hwc_session->hwc_display_[display];
      status = (hwc_display->*member)(std::forward<Args>(args)...);
    }
    return INT32(status);
  }

  template <typename... Args>
  static int32_t CallLayerFunction(hwc2_device_t *device, hwc2_display_t display,
                                   hwc2_layer_t layer, HWC2::Error (HWCLayer::*member)(Args...),
                                   Args... args) {
    if (!device) {
      return HWC2_ERROR_BAD_PARAMETER;
    }

    if (display >= HWCCallbacks::kNumDisplays) {
      return HWC2_ERROR_BAD_DISPLAY;
    }

    SCOPE_LOCK(locker_[display]);
    HWCSession *hwc_session = static_cast<HWCSession *>(device);
    auto status = HWC2::Error::BadDisplay;
    if (hwc_session->hwc_display_[display]) {
      status = HWC2::Error::BadLayer;
      auto hwc_layer = hwc_session->hwc_display_[display]->GetHWCLayer(layer);
      if (hwc_layer != nullptr) {
        status = (hwc_layer->*member)(std::forward<Args>(args)...);
        if (hwc_session->hwc_display_[display]->GetGeometryChanges()) {
          hwc_session->hwc_display_[display]->ResetValidation();
        }
      }
    }
    return INT32(status);
  }

  // HWC2 Functions that require a concrete implementation in hwc session
  // and hence need to be member functions
  static int32_t AcceptDisplayChanges(hwc2_device_t *device, hwc2_display_t display);
  static int32_t CreateLayer(hwc2_device_t *device, hwc2_display_t display,
                             hwc2_layer_t *out_layer_id);
  static int32_t CreateVirtualDisplay(hwc2_device_t *device, uint32_t width, uint32_t height,
                                      int32_t *format, hwc2_display_t *out_display_id);
  static int32_t DestroyLayer(hwc2_device_t *device, hwc2_display_t display, hwc2_layer_t layer);
  static int32_t DestroyVirtualDisplay(hwc2_device_t *device, hwc2_display_t display);
  static void Dump(hwc2_device_t *device, uint32_t *out_size, char *out_buffer);
  static int32_t PresentDisplay(hwc2_device_t *device, hwc2_display_t display,
                                int32_t *out_retire_fence);
  static int32_t RegisterCallback(hwc2_device_t *device, int32_t descriptor,
                                  hwc2_callback_data_t callback_data,
                                  hwc2_function_pointer_t pointer);
  static int32_t SetOutputBuffer(hwc2_device_t *device, hwc2_display_t display,
                                 buffer_handle_t buffer, int32_t releaseFence);
  static int32_t SetPowerMode(hwc2_device_t *device, hwc2_display_t display, int32_t int_mode);
  static int32_t ValidateDisplay(hwc2_device_t *device, hwc2_display_t display,
                                 uint32_t *out_num_types, uint32_t *out_num_requests);
  static int32_t SetColorMode(hwc2_device_t *device, hwc2_display_t display,
                              int32_t /*android_color_mode_t*/ int_mode);
  static int32_t SetColorModeWithRenderIntent(hwc2_device_t *device, hwc2_display_t display,
                                                 int32_t /*ColorMode*/ int_mode,
                                                 int32_t /*RenderIntent*/ int_render_intent);

  static int32_t SetColorTransform(hwc2_device_t *device, hwc2_display_t display,
                                   const float *matrix, int32_t /*android_color_transform_t*/ hint);
  static int32_t GetDisplayIdentificationData(hwc2_device_t *device, hwc2_display_t display,
                                              uint8_t *outPort, uint32_t *outDataSize,
                                              uint8_t *outData);
  static int32_t GetRenderIntents(hwc2_device_t *device, hwc2_display_t display,
                                int32_t /*ColorMode*/ int_mode, uint32_t *out_num_intents,
                                int32_t /*RenderIntent*/ *int_out_intents);
  static int32_t GetDisplayCapabilities(hwc2_device_t *device, hwc2_display_t display,
                                        uint32_t *outNumCapabilities, uint32_t *outCapabilities);
  static int32_t GetDisplayBrightnessSupport(hwc2_device_t *device, hwc2_display_t display,
                                             bool *outSupport);
  static int32_t SetDisplayBrightness(hwc2_device_t *device, hwc2_display_t display,
                                      float brightness);

  virtual int RegisterClientContext(std::shared_ptr<DisplayConfig::ConfigCallback> callback,
                                    DisplayConfig::ConfigInterface **intf);
  virtual void UnRegisterClientContext(DisplayConfig::ConfigInterface *intf);

  static int32_t SetVsyncEnabled(hwc2_device_t *device, hwc2_display_t display,
                                 int32_t int_enabled);
  static int32_t GetDozeSupport(hwc2_device_t *device, hwc2_display_t display,
                                int32_t *out_support);

  static Locker locker_[HWCCallbacks::kNumDisplays];

 protected:
  void updateRefreshRateHint();

 private:

  int32_t checkPowerHalExtHintSupport(const std::string& mode);

  class DisplayConfigImpl: public DisplayConfig::ConfigInterface {
   public:
    explicit DisplayConfigImpl(std::weak_ptr<DisplayConfig::ConfigCallback> callback,
                               HWCSession *hwc_session);

   private:
    virtual int IsDisplayConnected(DispType dpy, bool *connected);
    virtual int SetDisplayStatus(DispType dpy, DisplayConfig::ExternalStatus status);
    virtual int ConfigureDynRefreshRate(DisplayConfig::DynRefreshRateOp op, uint32_t refresh_rate);
    virtual int GetConfigCount(DispType dpy, uint32_t *count);
    virtual int GetActiveConfig(DispType dpy, uint32_t *config);
    virtual int SetActiveConfig(DispType dpy, uint32_t config);
    virtual int GetDisplayAttributes(uint32_t config_index, DispType dpy,
                                     DisplayConfig::Attributes *attributes);
    virtual int SetPanelBrightness(uint32_t level);
    virtual int GetPanelBrightness(uint32_t *level);
    virtual int MinHdcpEncryptionLevelChanged(DispType dpy, uint32_t min_enc_level);
    virtual int RefreshScreen();
    virtual int ControlPartialUpdate(DispType dpy, bool enable);
    virtual int ToggleScreenUpdate(bool on);
    virtual int SetIdleTimeout(uint32_t value);
    virtual int GetHDRCapabilities(DispType dpy, DisplayConfig::HDRCapsParams *caps);
    virtual int SetCameraLaunchStatus(uint32_t on);
    virtual int DisplayBWTransactionPending(bool *status);
    virtual int SetDisplayAnimating(uint64_t display_id, bool animating);
    virtual int ControlIdlePowerCollapse(bool enable, bool synchronous);
    virtual int GetWriteBackCapabilities(bool *is_wb_ubwc_supported);
    virtual int SetDisplayDppsAdROI(uint32_t display_id, uint32_t h_start, uint32_t h_end,
                                    uint32_t v_start, uint32_t v_end, uint32_t factor_in,
                                    uint32_t factor_out);
    virtual int UpdateVSyncSourceOnPowerModeOff();
    virtual int UpdateVSyncSourceOnPowerModeDoze();
    virtual int SetPowerMode(uint32_t disp_id, DisplayConfig::PowerMode power_mode);
    virtual int IsPowerModeOverrideSupported(uint32_t disp_id, bool *supported);
    virtual int IsHDRSupported(uint32_t disp_id, bool *supported);
    virtual int IsWCGSupported(uint32_t disp_id, bool *supported);
    virtual int SetLayerAsMask(uint32_t disp_id, uint64_t layer_id);
    virtual int GetDebugProperty(const std::string prop_name, std::string value);
    virtual int GetActiveBuiltinDisplayAttributes(DisplayConfig::Attributes *attr);
    virtual int SetPanelLuminanceAttributes(uint32_t disp_id, float min_lum, float max_lum);
    virtual int IsBuiltInDisplay(uint32_t disp_id, bool *is_builtin);

    std::weak_ptr<DisplayConfig::ConfigCallback> callback_;
    HWCSession *hwc_session_ = nullptr;
  };

  struct DisplayMapInfo {
    hwc2_display_t client_id = HWCCallbacks::kNumDisplays;    // mapped sf id for this display
    int32_t sdm_id = -1;                                      // sdm id for this display
    sdm:: DisplayType disp_type = kDisplayTypeMax;            // sdm display type
    bool test_pattern = false;                                // display will show test pattern
    void Reset() {
      // Do not clear client id
      sdm_id = -1;
      disp_type = kDisplayTypeMax;
      test_pattern = false;
    }
  };

  static const int kExternalConnectionTimeoutMs = 500;
  static const int kPartialUpdateControlTimeoutMs = 100;

  // hwc methods
  static int Open(const hw_module_t *module, const char *name, hw_device_t **device);
  static int Close(hw_device_t *device);
  static void GetCapabilities(struct hwc2_device *device, uint32_t *outCount,
                              int32_t *outCapabilities);
  static hwc2_function_pointer_t GetFunction(struct hwc2_device *device, int32_t descriptor);

  // Uevent handler
  virtual void UEventHandler(const char *uevent_data, int length);
  void ResetPanel();
  void InitSupportedDisplaySlots();
  int GetDisplayIndex(int dpy);
  int CreatePrimaryDisplay();
  void CreateNullDisplay();
  int CreateBuiltInDisplays();
  int CreatePluggableDisplays(bool delay_hotplug);
  int HandleConnectedDisplays(HWDisplaysInfo *hw_displays_info, bool delay_hotplug);
  int HandleDisconnectedDisplays(HWDisplaysInfo *hw_displays_info);
  void DestroyDisplay(DisplayMapInfo *map_info);
  void DestroyPluggableDisplay(DisplayMapInfo *map_info);
  void DestroyNonPluggableDisplay(DisplayMapInfo *map_info);
  int GetVsyncPeriod(int disp);
  int GetConfigCount(int disp_id, uint32_t *count);
  int GetActiveConfigIndex(int disp_id, uint32_t *config);
  int SetActiveConfigIndex(int disp_id, uint32_t config);
  int ControlPartialUpdate(int dpy, bool enable);
  int DisplayBWTransactionPending(bool *status);
  int SetDisplayStatus(int disp_id, HWCDisplay::DisplayStatus status);
  int MinHdcpEncryptionLevelChanged(int disp_id, uint32_t min_enc_level);
  int IsWbUbwcSupported(bool *value);
  int SetIdleTimeout(uint32_t value);
  int ToggleScreenUpdate(bool on);
  int SetCameraLaunchStatus(uint32_t on);
  int SetDisplayDppsAdROI(uint32_t display_id, uint32_t h_start, uint32_t h_end,
                          uint32_t v_start, uint32_t v_end, uint32_t factor_in,
                          uint32_t factor_out);
  int ControlIdlePowerCollapse(bool enable, bool synchronous);
  int32_t SetDynamicDSIClock(int64_t disp_id, uint32_t bitrate);
  bool HasHDRSupport(HWCDisplay *hwc_display);
  int32_t getDisplayBrightness(uint32_t display, float *brightness);
  int32_t setDisplayBrightness(uint32_t display, float brightness);

  // service methods
  void StartServices();

  // QClient methods
  virtual android::status_t notifyCallback(uint32_t command, const android::Parcel *input_parcel,
                                           android::Parcel *output_parcel);
  void DynamicDebug(const android::Parcel *input_parcel);
  android::status_t SetFrameDumpConfig(const android::Parcel *input_parcel);
  android::status_t SetMaxMixerStages(const android::Parcel *input_parcel);
  android::status_t SetDisplayMode(const android::Parcel *input_parcel);
  android::status_t ConfigureRefreshRate(const android::Parcel *input_parcel);
  android::status_t QdcmCMDHandler(const android::Parcel *input_parcel,
                                   android::Parcel *output_parcel);
  android::status_t QdcmCMDDispatch(uint32_t display_id,
                                    const PPDisplayAPIPayload &req_payload,
                                    PPDisplayAPIPayload *resp_payload,
                                    PPPendingParams *pending_action);
  android::status_t GetDisplayAttributesForConfig(const android::Parcel *input_parcel,
                                                  android::Parcel *output_parcel);
  android::status_t GetVisibleDisplayRect(const android::Parcel *input_parcel,
                                          android::Parcel *output_parcel);
  android::status_t SetMixerResolution(const android::Parcel *input_parcel);
  android::status_t SetColorModeOverride(const android::Parcel *input_parcel);

  android::status_t SetColorModeById(const android::Parcel *input_parcel);
  android::status_t getComposerStatus();
  android::status_t RefreshScreen(const android::Parcel *input_parcel);
  android::status_t SetDsiClk(const android::Parcel *input_parcel);
  android::status_t GetDsiClk(const android::Parcel *input_parcel, android::Parcel *output_parcel);
  android::status_t GetSupportedDsiClk(const android::Parcel *input_parcel,
                                       android::Parcel *output_parcel);

  void Refresh(hwc2_display_t display);
  void HotPlug(hwc2_display_t display, HWC2::Connection state);
  void HandleConcurrency(hwc2_display_t disp);
  void ActivateDisplay(hwc2_display_t disp, bool enable);
  void NonBuiltinConcurrency(hwc2_display_t disp, bool is_built_in_2_on);
  void HandleBuiltInDisplays();
  void HandlePendingRefresh();
  bool GetSecondBuiltinStatus();
  hwc2_display_t GetNextBuiltinIndex();
  HWC2::Error ValidateDisplayInternal(hwc2_display_t display, uint32_t *out_num_types,
                                      uint32_t *out_num_requests);
  HWC2::Error PresentDisplayInternal(hwc2_display_t display, int32_t *out_retire_fence);
  hwc2_display_t GetActiveBuiltinDisplay();

  CoreInterface *core_intf_ = nullptr;
  HWCDisplay *hwc_display_[HWCCallbacks::kNumDisplays] = {nullptr};
  HWCDisplay *hwc_display_builtin_[HWCCallbacks::kNumBuiltIn] = {nullptr};
  HWCCallbacks callbacks_;
  HWCBufferAllocator buffer_allocator_;
  HWCBufferSyncHandler buffer_sync_handler_;
  HWCColorManager *color_mgr_ = nullptr;
  DisplayMapInfo map_info_primary_;                 // Primary display (either builtin or pluggable)
  std::vector<DisplayMapInfo> map_info_builtin_;    // Builtin displays excluding primary
  std::vector<DisplayMapInfo> map_info_pluggable_;  // Pluggable displays excluding primary
  std::vector<DisplayMapInfo> map_info_virtual_;    // Virtual displays
  std::vector<bool> is_hdr_display_;    // info on HDR supported
  bool reset_panel_ = false;
  bool secure_display_active_ = false;
  bool primary_ready_ = false;
  bool client_connected_ = false;
  bool new_bw_mode_ = false;
  bool need_invalidate_ = false;
  int bw_mode_release_fd_ = -1;
  qService::QService *qservice_ = nullptr;
  HWCSocketHandler socket_handler_;
  bool pluggable_is_primary_ = false;
  bool null_display_active_ = false;
  bool is_composer_up_ = false;
  Locker callbacks_lock_;
  int hpd_bpp_ = 0;
  int hpd_pattern_ = 0;
  std::bitset<HWCCallbacks::kNumDisplays> pending_refresh_;

  /* Display hint to notify power hal */
  class PowerHalHintWorker : public Worker {
  public:
      PowerHalHintWorker();
      void signalRefreshRate(HWC2::PowerMode powerMode, uint32_t vsyncPeriod);
      void signalIdle();
  protected:
      void Routine() override;
  private:
      int32_t connectPowerHalExt();
      int32_t checkPowerHalExtHintSupport(const std::string& mode);
      int32_t sendPowerHalExtHint(const std::string& mode, bool enabled);
      int32_t checkRefreshRateHintSupport(int refreshRate);
      int32_t updateRefreshRateHintInternal(HWC2::PowerMode powerMode,
                                            uint32_t vsyncPeriod);
      int32_t sendRefreshRateHint(int refreshRate, bool enabled);
      int32_t checkIdleHintSupport();
      int32_t updateIdleHint(uint64_t deadlineTime);
      bool mNeedUpdateRefreshRateHint;
      // previous refresh rate
      int mPrevRefreshRate;
      // the refresh rate whose hint failed to be disabled
      int mPendingPrevRefreshRate;
      // support list of refresh rate hints
      std::map<int, bool> mRefreshRateHintSupportMap;
      bool mIdleHintIsEnabled;
      uint64_t mIdleHintDeadlineTime;
      // whether idle hint support is checked
      bool mIdleHintSupportIsChecked;
      // whether idle hint is supported
      bool mIdleHintIsSupported;
      HWC2::PowerMode mPowerModeState;
      uint32_t mVsyncPeriod;
      // for power HAL extension hints
      std::shared_ptr<aidl::google::hardware::power::extension::pixel::IPowerExt>
               mPowerHalExtAidl;
  };
      PowerHalHintWorker mPowerHalHint;

};

}  // namespace sdm

#endif  // __HWC_SESSION_H__
