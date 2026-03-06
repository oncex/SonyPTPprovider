/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERADEVICE_H
#define ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERADEVICE_H

#include "utils/Mutex.h"
#include "utils/KeyedVector.h"
#include <android/hardware/camera/device/3.2/ICameraDevice.h>
#include <hidl/Status.h>
#include <hidl/MQDescriptor.h>
#include <vector>
#include "CameraMetadata.h"
#include "PtpCameraDeviceSession.h"
#include "PtpCameraUtils.h"
#include "socc_ptp.h"

namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace V3_4 {
namespace implementation {

using namespace ::android::hardware::camera::device;
using ::android::hardware::camera::common::V1_0::Status;
using ::android::hardware::camera::device::V3_2::ICameraDevice;
using ::android::hardware::camera::device::V3_2::ICameraDeviceCallback;
using ::android::hardware::camera::common::V1_0::TorchMode;
using ::android::hardware::camera::common::V1_0::CameraResourceCost;
using ::android::hardware::camera::ptp::common::PTPCameraConfig;
using ::android::hardware::camera::ptp::common::Size;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::sp;
using ::android::Mutex;

struct PTPCameraDevice : public virtual RefBase {

    PTPCameraDevice(const std::string& devicePath, const PTPCameraConfig& cfg);
    ~PTPCameraDevice();

    // Caller must use this method to check if CameraDevice ctor failed
    bool isInitFailed();

    // Retrieve the HIDL interface, split into its own class to avoid inheritance issues when
    // dealing with minor version revs and simultaneous implementation and interface inheritance
    virtual sp<ICameraDevice> getInterface() {
        return new TrampolineDeviceInterface_3_4(this);
    }

protected:
    // Methods from ::android::hardware::camera::device::V3_2::ICameraDevice follow.
    Return<void> getResourceCost(ICameraDevice::getResourceCost_cb _hidl_cb);
    Return<void> getCameraCharacteristics(ICameraDevice::getCameraCharacteristics_cb _hidl_cb);
    Return<Status> setTorchMode(TorchMode mode);
    Return<void> open(const sp<ICameraDeviceCallback>& callback, ICameraDevice::open_cb _hidl_cb);
    Return<void> dumpState(const ::android::hardware::hidl_handle& fd);

    // End of ICameraDevice methods

    // Helper functions
    bool isInitFailedLocked();
    status_t initCameraCharacteristics();
    status_t initDefaultCharsKeys(
            ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata);
    status_t initCameraControlsCharsKeys(int fd,
            ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata);
    status_t initOutputCharsKeys(int fd,
            ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata);
    status_t initAvailableCapabilities(
            ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata);
    void initSupportedFormatsLocked(int fd);

    // Get candidate supported formats from PTP.
    // Since PTP enumeration is different, we might just assume some formats or try to query.
    // We pass -1 as fd since we don't use v4l2 fd.
    std::vector<SupportedPTPFormat> getCandidateSupportedFormatsLocked(
            int fd, CroppingType cropType,
            const std::vector<PTPCameraConfig::FpsLimitation>& fpsLimits,
            const std::vector<PTPCameraConfig::FpsLimitation>& depthFpsLimits,
            const Size& minStreamSize,
            bool depthEnabled);

    void updateFpsBounds(int fd, CroppingType cropType,
            const std::vector<PTPCameraConfig::FpsLimitation>& fpsLimits,
            SupportedPTPFormat format,
            std::vector<SupportedPTPFormat>& outFmts);

    void getFrameRateList(int fd, double fpsUpperBound, SupportedPTPFormat* format);

    static void trimSupportedFormats(CroppingType cropType,
            /*inout*/std::vector<SupportedPTPFormat>* pFmts);

    template <size_t SIZE>
    status_t initOutputCharskeysByFormat(
            ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata,
            uint32_t fourcc, const std::array<int, SIZE>& halFormats,
            int streamConfigTag, int streamConfiguration, int minFrameDuration, int stallDuration);

    bool calculateMinFps(::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata);

    virtual sp<PTPCameraDeviceSession> createSession(
            const sp<ICameraDeviceCallback>&,
            const PTPCameraConfig& cfg,
            const std::vector<SupportedPTPFormat>& sortedFormats,
            const CroppingType& croppingType,
            const common::V1_0::helper::CameraMetadata& chars,
            const std::string& cameraId,
            std::shared_ptr<com::sony::imaging::remote::socc_ptp> ptp);

    Mutex mLock;
    bool mInitFailed = false;
    std::string mCameraId;
    const std::string mDevicePath;
    const PTPCameraConfig& mCfg;
    common::V1_0::helper::CameraMetadata mCameraCharacteristics;

    std::vector<SupportedPTPFormat> mSupportedFormats;
    CroppingType mCroppingType = VERTICAL;

    wp<PTPCameraDeviceSession> mSession = nullptr;
    const std::vector<int32_t> AVAILABLE_CHARACTERISTICS_KEYS_3_4 = {
        ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_AE_COMPENSATION_RANGE,
        ANDROID_CONTROL_AE_COMPENSATION_STEP,
        ANDROID_CONTROL_AE_LOCK_AVAILABLE,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_EFFECTS,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
        ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
        ANDROID_CONTROL_MAX_REGIONS,
        ANDROID_FLASH_INFO_AVAILABLE,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
        ANDROID_LENS_FACING,
        ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
        ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
        ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SCALER_CROPPING_TYPE,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SHADING_AVAILABLE_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
        ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
        ANDROID_SYNC_MAX_LATENCY};

    bool mInitialized = false;

    struct TrampolineDeviceInterface_3_4 : public ICameraDevice {
        TrampolineDeviceInterface_3_4(sp<PTPCameraDevice> parent) :
                mParent(parent) {}

        virtual Return<void> getResourceCost(ICameraDevice::getResourceCost_cb _hidl_cb) override {
            return mParent->getResourceCost(_hidl_cb);
        }

        virtual Return<void> getCameraCharacteristics(
                ICameraDevice::getCameraCharacteristics_cb _hidl_cb) override {
            return mParent->getCameraCharacteristics(_hidl_cb);
        }

        virtual Return<Status> setTorchMode(TorchMode mode) override {
            return mParent->setTorchMode(mode);
        }

        virtual Return<void> open(const sp<ICameraDeviceCallback>& callback,
                ICameraDevice::open_cb _hidl_cb) override {
            return mParent->open(callback, _hidl_cb);
        }

        virtual Return<void> dumpState(const ::android::hardware::hidl_handle& fd) override {
            return mParent->dumpState(fd);
        }

        
        
    private:
        sp<PTPCameraDevice> mParent;
    };
};

}  // namespace implementation
}  // namespace V3_4
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERADEVICE_H
