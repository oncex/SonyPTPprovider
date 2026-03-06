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

#ifndef ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERAUTILS_H
#define ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERAUTILS_H

#include <android/hardware/camera/common/1.0/types.h>
#include <android/hardware/camera/device/3.2/types.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <inttypes.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "tinyxml2.h"  // XML parsing
#include "utils/LightRefBase.h"
#include "utils/Timers.h"
#include <CameraMetadata.h>
#include <HandleImporter.h>


using ::android::hardware::graphics::mapper::V2_0::IMapper;
using ::android::hardware::graphics::mapper::V2_0::YCbCrLayout;
using ::android::hardware::camera::common::V1_0::helper::HandleImporter;
using ::android::hardware::camera::common::V1_0::Status;
using ::android::hardware::camera::device::V3_2::ErrorCode;

namespace android {
namespace hardware {
namespace camera {

namespace ptp {
namespace common {

struct Size {
    uint32_t width;
    uint32_t height;

    bool operator==(const Size& other) const {
        return (width == other.width && height == other.height);
    }
};

struct SizeHasher {
    size_t operator()(const Size& sz) const {
        size_t result = 1;
        result = 31 * result + sz.width;
        result = 31 * result + sz.height;
        return result;
    }
};

struct PTPCameraConfig {
    static const char* kDefaultCfgPath;
    static PTPCameraConfig loadFromCfg(const char* cfgPath = kDefaultCfgPath);

    uint32_t cameraIdOffset;
    std::unordered_set<std::string> mInternalDevices;
    uint32_t maxJpegBufSize;
    Size maxVideoSize;
    uint32_t numVideoBuffers;
    uint32_t numStillBuffers;
    bool depthEnabled;

    struct FpsLimitation {
        Size size;
        double fpsUpperBound;
    };
    std::vector<FpsLimitation> fpsLimits;
    std::vector<FpsLimitation> depthFpsLimits;

    Size minStreamSize;
    int32_t orientation;

private:
    PTPCameraConfig();
    static bool updateFpsList(tinyxml2::XMLElement* fpsList, std::vector<FpsLimitation>& fpsLimits);
};

} // common
} // ptp

namespace device {
namespace V3_4 {
namespace implementation {

struct SupportedPTPFormat {
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    struct FrameRate {
        uint32_t durationNumerator;
        uint32_t durationDenominator;
        double getDouble() const;
    };
    std::vector<FrameRate> frameRates;
};

struct Frame : public VirtualLightRefBase {
public:
    Frame(uint32_t width, uint32_t height, uint32_t fourcc);
    virtual ~Frame();
    const uint32_t mWidth;
    const uint32_t mHeight;
    const uint32_t mFourcc;

    virtual int getData(uint8_t** outData, size_t* dataSize) = 0;
};

class PTPFrame : public Frame {
public:
    PTPFrame(uint32_t w, uint32_t h, uint32_t fourcc, uint8_t* data, size_t dataSize);
    ~PTPFrame() override;

    virtual int getData(uint8_t** outData, size_t* dataSize) override;

private:
    uint8_t* mData;
    size_t mDataSize;
};

class AllocatedFrame : public Frame {
public:
    AllocatedFrame(uint32_t w, uint32_t h);
    ~AllocatedFrame() override;

    virtual int getData(uint8_t** outData, size_t* dataSize) override;

    int allocate(YCbCrLayout* out = nullptr);
    int getLayout(YCbCrLayout* out);
    int getCroppedLayout(const IMapper::Rect&, YCbCrLayout* out);
private:
    std::mutex mLock;
    std::vector<uint8_t> mData;
};

enum CroppingType {
    HORIZONTAL = 0,
    VERTICAL = 1
};

#define ASPECT_RATIO(sz) (static_cast<float>((sz).width) / (sz).height)
const float kMaxAspectRatio = std::numeric_limits<float>::max();
const float kMinAspectRatio = 1.f;

bool isAspectRatioClose(float ar1, float ar2);

struct HalStreamBuffer {
    int32_t streamId;
    uint64_t bufferId;
    uint32_t width;
    uint32_t height;
    ::android::hardware::graphics::common::V1_0::PixelFormat format;
    ::android::hardware::camera::device::V3_2::BufferUsageFlags usage;
    buffer_handle_t* bufPtr;
    int acquireFence;
    bool fenceTimeout;
};

struct HalRequest {
    uint32_t frameNumber;
    common::V1_0::helper::CameraMetadata setting;
    sp<Frame> frameIn;
    nsecs_t shutterTs;
    std::vector<HalStreamBuffer> buffers;
};

static const uint64_t BUFFER_ID_NO_BUFFER = 0;

typedef std::unordered_map<uint64_t, buffer_handle_t> CirculatingBuffers;

::android::hardware::camera::common::V1_0::Status importBufferImpl(
        /*inout*/std::map<int, CirculatingBuffers>& circulatingBuffers,
        /*inout*/HandleImporter& handleImporter,
        int32_t streamId,
        uint64_t bufId, buffer_handle_t buf,
        /*out*/buffer_handle_t** outBufPtr,
        bool allowEmptyBuf);

static const uint32_t FLEX_YUV_GENERIC = static_cast<uint32_t>('F') |
        static_cast<uint32_t>('L') << 8 | static_cast<uint32_t>('E') << 16 |
        static_cast<uint32_t>('X') << 24;

uint32_t getFourCcFromLayout(const YCbCrLayout&);

using ::android::hardware::camera::ptp::common::Size;
int getCropRect(CroppingType ct, const Size& inSize,
        const Size& outSize, IMapper::Rect* out);

int formatConvert(const YCbCrLayout& in, const YCbCrLayout& out, Size sz, uint32_t format);

int encodeJpegYU12(const Size &inSz,
        const YCbCrLayout& inLayout, int jpegQuality,
        const void *app1Buffer, size_t app1Size,
        void *out, size_t maxOutSize,
        size_t &actualCodeSize);

Size getMaxThumbnailResolution(const common::V1_0::helper::CameraMetadata&);

void freeReleaseFences(hidl_vec<V3_2::CaptureResult>&);

status_t fillCaptureResultCommon(common::V1_0::helper::CameraMetadata& md, nsecs_t timestamp,
        camera_metadata_ro_entry& activeArraySize);

struct OutputThreadInterface : public virtual RefBase {
    virtual ::android::hardware::camera::common::V1_0::Status importBuffer(
            int32_t streamId, uint64_t bufId, buffer_handle_t buf,
            /*out*/buffer_handle_t** outBufPtr, bool allowEmptyBuf) = 0;

    virtual void notifyError(uint32_t frameNumber, int32_t streamId, ErrorCode ec) = 0;

    virtual ::android::hardware::camera::common::V1_0::Status processCaptureRequestError(
            const std::shared_ptr<HalRequest>&,
            /*out*/std::vector<V3_2::NotifyMsg>* msgs = nullptr,
            /*out*/std::vector<V3_2::CaptureResult>* results = nullptr) = 0;

    virtual ::android::hardware::camera::common::V1_0::Status processCaptureResult(
            std::shared_ptr<HalRequest>&) = 0;

    virtual ssize_t getJpegBufferSize(uint32_t width, uint32_t height) const = 0;
};

}  // namespace implementation
}  // namespace V3_4
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_CAMERA_DEVICE_V3_4_PTPCAMERAUTILS_H
