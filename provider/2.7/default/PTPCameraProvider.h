/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_CAMERA_PROVIDER_V2_7_PTPCAMERAPROVIDER_H
#define ANDROID_HARDWARE_CAMERA_PROVIDER_V2_7_PTPCAMERAPROVIDER_H

#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "PtpCameraUtils.h"
#include "libusb.h"

#include <android/hardware/camera/provider/2.6/ICameraProviderCallback.h>
#include <android/hardware/camera/provider/2.7/ICameraProvider.h>

namespace android {
namespace hardware {
namespace camera {
namespace provider {
namespace V2_7 {
namespace implementation {

using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::camera::common::V1_0::CameraDeviceStatus;
using ::android::hardware::camera::common::V1_0::Status;
using ::android::hardware::camera::common::V1_0::VendorTagSection;
using ::android::hardware::camera::external::common::PTPCameraConfig;
using ::android::hardware::camera::provider::V2_4::ICameraProviderCallback;
using ::android::hardware::camera::provider::V2_5::DeviceState;
using ::android::hardware::camera::provider::V2_7::CameraIdAndStreamCombination;
using ::android::hardware::camera::provider::V2_7::ICameraProvider;
using ::android::hidl::base::V1_0::IBase;

/**
 * The implementation of PTP CameraProvider 2.7.
 *
 * This camera provider supports PTP cameras via libusb and libcameracontrolptp.
 */
struct PTPCameraProvider : public virtual RefBase {
    PTPCameraProvider();
    ~PTPCameraProvider();

    // Caller must use this method to check if CameraProvider ctor failed
    bool isInitFailed() { return false; }

    // Methods from ::android::hardware::camera::provider::V2_4::ICameraProvider follow.
    Return<Status> setCallback(const sp<ICameraProviderCallback>& callback);
    Return<void> getVendorTags(ICameraProvider::getVendorTags_cb _hidl_cb);
    Return<void> getCameraIdList(ICameraProvider::getCameraIdList_cb _hidl_cb);
    Return<void> isSetTorchModeSupported(ICameraProvider::isSetTorchModeSupported_cb _hidl_cb);
    Return<void> getCameraDeviceInterface_V1_x(const hidl_string&,
                                               ICameraProvider::getCameraDeviceInterface_V1_x_cb);
    Return<void> getCameraDeviceInterface_V3_x(const hidl_string&,
                                               ICameraProvider::getCameraDeviceInterface_V3_x_cb);

    // Methods from ::android::hardware::camera::provider::V2_5::ICameraProvider follow.
    Return<void> notifyDeviceStateChange(hidl_bitfield<DeviceState> newState);

    // Methods from ::android::hardware::camera::provider::V2_7::ICameraProvider follow.
    Return<void> getConcurrentStreamingCameraIds(
            ICameraProvider::getConcurrentStreamingCameraIds_cb _hidl_cb);

    Return<void> isConcurrentStreamCombinationSupported(
            const hidl_vec<
                    ::android::hardware::camera::provider::V2_6::CameraIdAndStreamCombination>&
                    configs,
            ICameraProvider::isConcurrentStreamCombinationSupported_cb _hidl_cb);

    Return<void> isConcurrentStreamCombinationSupported_2_7(
            const hidl_vec<CameraIdAndStreamCombination>& configs,
            ICameraProvider::isConcurrentStreamCombinationSupported_2_7_cb _hidl_cb);

    // Public for hotplug callback
    void deviceAdded(int bus, int dev);
    void deviceRemoved(int bus, int dev);

  private:
    void addExternalCamera(std::string deviceName);

    class HotplugThread : public android::Thread {
      public:
        HotplugThread(PTPCameraProvider* parent);
        ~HotplugThread();

        virtual bool threadLoop() override;

      private:
        PTPCameraProvider* mParent = nullptr;
    };

    static int hotplugCallback(libusb_context *ctx, libusb_device *device,
                               libusb_hotplug_event event, void *user_data);

    Mutex mLock;
    sp<ICameraProviderCallback> mCallbacks = nullptr;
    std::unordered_map<std::string, CameraDeviceStatus> mCameraStatusMap;  // camera id -> status
    const PTPCameraConfig mCfg;
    sp<HotplugThread> mHotPlugThread;
    int mPreferredHal3MinorVersion;

    libusb_context* mCtx = nullptr;
    libusb_hotplug_callback_handle mHotplugHandle;
};

}  // namespace implementation
}  // namespace V2_7
}  // namespace provider
}  // namespace camera
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_CAMERA_PROVIDER_V2_7_PTPCAMERAPROVIDER_H
