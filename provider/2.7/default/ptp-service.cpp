#define LOG_TAG "android.hardware.camera.provider@2.7-service-ptp"

#include <android/hardware/camera/provider/2.7/ICameraProvider.h>
#include <binder/ProcessState.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include "PTPCameraProvider.h"
#include "CameraProvider_2_7.h"

using android::status_t;
using android::hardware::camera::provider::V2_7::ICameraProvider;
using android::hidl::base::V1_0::IBase;

int main() {
    using namespace android::hardware::camera::provider::V2_7::implementation;

    ALOGI("PTP Camera Provider service is starting.");

    // The camera HAL may communicate to other vendor components via
    // /dev/vndbinder
    android::ProcessState::initWithDriver("/dev/vndbinder");

    android::hardware::configureRpcThreadpool(6, true);

    android::sp<ICameraProvider> provider = new CameraProvider<PTPCameraProvider>();

    android::status_t status = provider->registerAsService("ptp/0");

    if (status != android::OK) {
        ALOGE("Cannot register PTP camera provider service: %d", status);
        return 1;
    }

    android::hardware::joinRpcThreadpool();

    return 0;
}
