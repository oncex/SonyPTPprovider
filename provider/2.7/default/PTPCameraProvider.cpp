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

#define LOG_TAG "CamPrvdr@2.7-ptp"
//#define LOG_NDEBUG 0
#include <log/log.h>

#include <cutils/properties.h>
#include <errno.h>
#include <regex>
#include <string>
#include "PtpCameraDevice_3_4.h"
#include "PTPCameraProvider.h"

namespace android {
namespace hardware {
namespace camera {
namespace provider {
namespace V2_7 {
namespace implementation {

namespace {
// "device@<version>/external/<id>"
const std::regex kDeviceNameRE("device@([0-9]+\\.[0-9]+)/external/(.+)");

bool matchDeviceName(int cameraIdOffset, const hidl_string& deviceName, std::string* deviceVersion,
                     std::string* cameraDevicePath) {
    std::string deviceNameStd(deviceName.c_str());
    std::smatch sm;
    if (std::regex_match(deviceNameStd, sm, kDeviceNameRE)) {
        if (deviceVersion != nullptr) {
            *deviceVersion = sm[1];
        }
        if (cameraDevicePath != nullptr) {
            // Reconstruct the device path "usb:bus:dev" from the ID (which might be just an index or encoded)
            // For PTP, we might store "usb:bus:dev" directly as the ID suffix if it's safe.
            // But Android Camera ID should be numeric usually for legacy apps, though external can be anything.
            // Let's assume the ID passed here is the one we registered.
            // Wait, we register "device@3.4/external/<id>".
            // The <id> part is what we need to map back to "usb:bus:dev".
            // Since we don't persist the mapping, we might need to rely on the fact that we use "usb:bus:dev" as the underlying ID,
            // but mapped to a numeric ID for the HAL if needed.
            // However, ExternalCameraProvider uses "videoX" index.
            // Let's stick to a simple mapping or just use the suffix if possible.
            // But wait, the ID must be numeric for some frameworks? No, external cameras have arbitrary IDs in HAL v3.
            // But `mCfg.cameraIdOffset` suggests we produce numeric IDs.
            // Let's look at `addExternalCamera`.
            // It parses `devName` (e.g. "/dev/video0") to get the number.
            // For PTP "usb:001:002", we can't easily get a number.
            // We might need to maintain a map or just hash it.
            // Or just use a simple counter.
            // For now, let's assume we can recover the path or lookup in a map.
            // But `getCameraDeviceInterface_V3_x` is stateless regarding previous `deviceAdded` calls if the provider restarted?
            // Actually, `mCameraStatusMap` stores the status.
            // We can store the device path in the map or a separate map.
            // But `matchDeviceName` is static-like helper.
            // Let's just return the suffix as the path for now, and handle it in `getCameraDeviceInterface_V3_x`.
             *cameraDevicePath = sm[2];
        }
        return true;
    }
    return false;
}

}  // anonymous namespace

PTPCameraProvider::PTPCameraProvider()
    : mCfg(PTPCameraConfig::loadFromCfg()) {
    
    int rc = libusb_init(&mCtx);
    if (rc < 0) {
        ALOGE("libusb_init failed: %s", libusb_error_name(rc));
        return;
    }

    // Register hotplug callback
    rc = libusb_hotplug_register_callback(mCtx,
        (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        hotplugCallback,
        this,
        &mHotplugHandle);
        
    if (rc != LIBUSB_SUCCESS) {
        ALOGE("Error registering hotplug callback: %s", libusb_error_name(rc));
    }

    mHotPlugThread = sp<HotplugThread>::make(this);
    mHotPlugThread->run("PTPCamHotPlug", PRIORITY_BACKGROUND);

    mPreferredHal3MinorVersion =
            property_get_int32("ro.vendor.camera.external.hal3TrebleMinorVersion", 4);
    ALOGV("Preferred HAL 3 minor version is %d", mPreferredHal3MinorVersion);
}

PTPCameraProvider::~PTPCameraProvider() {
    mHotPlugThread->requestExit();
    if (mCtx) {
        libusb_hotplug_deregister_callback(mCtx, mHotplugHandle);
        libusb_exit(mCtx);
    }
}

Return<Status> PTPCameraProvider::setCallback(
        const sp<ICameraProviderCallback>& callback) {
    if (callback == nullptr) {
        return Status::ILLEGAL_ARGUMENT;
    }
    Mutex::Autolock _l(mLock);
    mCallbacks = callback;
    // Send a callback for all devices to initialize
    {
        for (const auto& pair : mCameraStatusMap) {
            mCallbacks->cameraDeviceStatusChange(pair.first, pair.second);
        }
    }

    return Status::OK;
}

Return<void> PTPCameraProvider::getVendorTags(
        ICameraProvider::getVendorTags_cb _hidl_cb) {
    // No vendor tag support for USB camera
    hidl_vec<VendorTagSection> zeroSections;
    _hidl_cb(Status::OK, zeroSections);
    return Void();
}

Return<void> PTPCameraProvider::getCameraIdList(
        ICameraProvider::getCameraIdList_cb _hidl_cb) {
    // External camera HAL always report 0 camera, and extra cameras
    // are just reported via cameraDeviceStatusChange callbacks
    hidl_vec<hidl_string> hidlDeviceNameList;
    _hidl_cb(Status::OK, hidlDeviceNameList);
    return Void();
}

Return<void> PTPCameraProvider::isSetTorchModeSupported(
        ICameraProvider::isSetTorchModeSupported_cb _hidl_cb) {
    // setTorchMode API is supported, though right now no external camera device
    // has a flash unit.
    _hidl_cb(Status::OK, true);
    return Void();
}

Return<void> PTPCameraProvider::getCameraDeviceInterface_V1_x(
        const hidl_string&, ICameraProvider::getCameraDeviceInterface_V1_x_cb _hidl_cb) {
    // External Camera HAL does not support HAL1
    _hidl_cb(Status::OPERATION_NOT_SUPPORTED, nullptr);
    return Void();
}

Return<void> PTPCameraProvider::getCameraDeviceInterface_V3_x(
        const hidl_string& cameraDeviceName,
        ICameraProvider::getCameraDeviceInterface_V3_x_cb _hidl_cb) {
    std::string cameraDevicePath, deviceVersion;
    bool match = matchDeviceName(mCfg.cameraIdOffset, cameraDeviceName, &deviceVersion,
                                 &cameraDevicePath);
    if (!match) {
        _hidl_cb(Status::ILLEGAL_ARGUMENT, nullptr);
        return Void();
    }

    if (mCameraStatusMap.count(cameraDeviceName) == 0 ||
        mCameraStatusMap[cameraDeviceName] != CameraDeviceStatus::PRESENT) {
        _hidl_cb(Status::ILLEGAL_ARGUMENT, nullptr);
        return Void();
    }

    // cameraDevicePath is "usb:bus:dev" or mapped ID. 
    // We used the ID as the path in deviceAdded.
    
    sp<device::V3_4::implementation::PTPCameraDevice> deviceImpl;
    ALOGV("Constructing v3.4 ptp camera device");
    deviceImpl = new device::V3_4::implementation::PTPCameraDevice(cameraDevicePath, mCfg);

    if (deviceImpl == nullptr || deviceImpl->isInitFailed()) {
        ALOGE("%s: camera device %s init failed!", __FUNCTION__, cameraDevicePath.c_str());
        _hidl_cb(Status::INTERNAL_ERROR, nullptr);
        return Void();
    }

    _hidl_cb(Status::OK, deviceImpl->getInterface());
    return Void();
}

void PTPCameraProvider::addExternalCamera(std::string deviceId) {
    ALOGV("PTPCam: adding %s to PTP Camera HAL!", deviceId.c_str());
    Mutex::Autolock _l(mLock);
    std::string deviceName;
    
    // deviceId is "usb:bus:dev"
    // We need to construct a unique ID.
    // Let's use the deviceId string itself as the suffix, or hash it if it needs to be numeric?
    // The ExternalCameraProvider uses numeric offset + index.
    // For simplicity, we'll use the string directly.
    
    if (mPreferredHal3MinorVersion == 6) {
        deviceName = std::string("device@3.6/external/") + deviceId;
    } else if (mPreferredHal3MinorVersion == 5) {
        deviceName = std::string("device@3.5/external/") + deviceId;
    } else {
        deviceName = std::string("device@3.4/external/") + deviceId;
    }
    mCameraStatusMap[deviceName] = CameraDeviceStatus::PRESENT;
    if (mCallbacks != nullptr) {
        mCallbacks->cameraDeviceStatusChange(deviceName, CameraDeviceStatus::PRESENT);
    }
}

void PTPCameraProvider::deviceAdded(int bus, int dev) {
    char deviceId[32];
    snprintf(deviceId, sizeof(deviceId), "usb:%d:%d", bus, dev);
    
    // Check if it's a PTP device?
    // Already checked in callback.
    
    ALOGV("PTP device added: %s", deviceId);
    
    // See if we can initialize PTPCameraDevice correctly
    sp<device::V3_4::implementation::PTPCameraDevice> deviceImpl =
            new device::V3_4::implementation::PTPCameraDevice(deviceId, mCfg);
    if (deviceImpl == nullptr || deviceImpl->isInitFailed()) {
        ALOGW("%s: Attempt to init camera device %s failed!", __FUNCTION__, deviceId);
        return;
    }
    deviceImpl.clear();

    addExternalCamera(deviceId);
}

void PTPCameraProvider::deviceRemoved(int bus, int dev) {
    Mutex::Autolock _l(mLock);
    char deviceId[32];
    snprintf(deviceId, sizeof(deviceId), "usb:%d:%d", bus, dev);
    
    std::string deviceName;
    if (mPreferredHal3MinorVersion == 6) {
        deviceName = std::string("device@3.6/external/") + deviceId;
    } else if (mPreferredHal3MinorVersion == 5) {
        deviceName = std::string("device@3.5/external/") + deviceId;
    } else {
        deviceName = std::string("device@3.4/external/") + deviceId;
    }
    
    if (mCameraStatusMap.erase(deviceName) != 0) {
        if (mCallbacks != nullptr) {
            mCallbacks->cameraDeviceStatusChange(deviceName, CameraDeviceStatus::NOT_PRESENT);
        }
    } else {
        ALOGE("%s: cannot find camera device %s", __FUNCTION__, deviceName.c_str());
    }
}

int PTPCameraProvider::hotplugCallback(libusb_context *ctx, libusb_device *device,
                           libusb_hotplug_event event, void *user_data) {
    PTPCameraProvider *self = (PTPCameraProvider *)user_data;
    struct libusb_device_descriptor desc;
    int rc = libusb_get_device_descriptor(device, &desc);
    if (LIBUSB_SUCCESS != rc) {
        ALOGE("Error getting device descriptor: %s", libusb_error_name(rc));
        return 0;
    }

    int bus = libusb_get_bus_number(device);
    int dev = libusb_get_device_address(device);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        // Check for PTP class
        // Class 6 (Image), Subclass 1 (Still Image Capture), Protocol 1 (PTP)
        // Note: Sometimes the class is in the interface descriptor, not device descriptor.
        // But usually device class is 0 or 6.
        // We should check config descriptor -> interface descriptor.
        
        bool isPTP = false;
        if (desc.bDeviceClass == 6 && desc.bDeviceSubClass == 1 && desc.bDeviceProtocol == 1) {
            isPTP = true;
        } else if (desc.bDeviceClass == 0) {
            struct libusb_config_descriptor *config;
            rc = libusb_get_active_config_descriptor(device, &config);
            if (rc == LIBUSB_SUCCESS) {
                for (int i = 0; i < config->bNumInterfaces; i++) {
                    const struct libusb_interface_descriptor *inter = &config->interface[i].altsetting[0];
                    if (inter->bInterfaceClass == 6 && 
                        inter->bInterfaceSubClass == 1 && 
                        inter->bInterfaceProtocol == 1) {
                        isPTP = true;
                        break;
                    }
                }
                libusb_free_config_descriptor(config);
            }
        }
        
        if (isPTP) {
            self->deviceAdded(bus, dev);
        }
    } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        // We don't know if it was PTP without checking logic, but we can try to remove it.
        // If it wasn't added, deviceRemoved will just log error or do nothing.
        self->deviceRemoved(bus, dev);
    }
    
    return 0;
}

PTPCameraProvider::HotplugThread::HotplugThread(PTPCameraProvider* parent)
    : Thread(/*canCallJava*/ false),
      mParent(parent) {}

PTPCameraProvider::HotplugThread::~HotplugThread() {}

bool PTPCameraProvider::HotplugThread::threadLoop() {
    // Handle events
    if (mParent && mParent->mCtx) {
        struct timeval tv = {1, 0}; // 1 second timeout
        libusb_handle_events_timeout_completed(mParent->mCtx, &tv, NULL);
    }
    return true; // Keep running
}

Return<void> PTPCameraProvider::notifyDeviceStateChange(
        hidl_bitfield<DeviceState> /*newState*/) {
    return Void();
}

Return<void> PTPCameraProvider::getConcurrentStreamingCameraIds(
        ICameraProvider::getConcurrentStreamingCameraIds_cb _hidl_cb) {
    hidl_vec<hidl_vec<hidl_string>> hidl_camera_id_combinations;
    _hidl_cb(Status::OK, hidl_camera_id_combinations);
    return Void();
}

Return<void> PTPCameraProvider::isConcurrentStreamCombinationSupported(
        const hidl_vec<::android::hardware::camera::provider::V2_6::
                               CameraIdAndStreamCombination>& /* configs */,
        ICameraProvider::isConcurrentStreamCombinationSupported_cb _hidl_cb) {
    _hidl_cb(Status::OK, false);
    return Void();
}

Return<void> PTPCameraProvider::isConcurrentStreamCombinationSupported_2_7(
        const hidl_vec<CameraIdAndStreamCombination>& /* configs */,
        ICameraProvider::isConcurrentStreamCombinationSupported_2_7_cb _hidl_cb) {
    _hidl_cb(Status::OK, false);
    return Void();
}

}  // namespace implementation
}  // namespace V2_7
}  // namespace provider
}  // namespace camera
}  // namespace hardware
}  // namespace android