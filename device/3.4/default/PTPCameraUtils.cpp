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
#define LOG_TAG "PTPCamUtils@3.4"
//#define LOG_NDEBUG 0
#include <log/log.h>

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <linux/videodev2.h>

#define HAVE_JPEG // required for libyuv.h to export MJPEG decode APIs
#include <libyuv.h>

#include <jpeglib.h>

#include "PtpCameraUtils.h"

namespace {

buffer_handle_t sEmptyBuffer = nullptr;

} // Anonymous namespace

namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace V3_4 {
namespace implementation {

Frame::Frame(uint32_t width, uint32_t height, uint32_t fourcc) :
        mWidth(width), mHeight(height), mFourcc(fourcc) {}

Frame::~Frame() {}

PTPFrame::PTPFrame(
        uint32_t w, uint32_t h, uint32_t fourcc,
        uint8_t* data, size_t dataSize) :
        Frame(w, h, fourcc),
        mData(data), mDataSize(dataSize) {}

PTPFrame::~PTPFrame() {
    if (mData) {
        free(mData);
    }
}

int PTPFrame::getData(uint8_t** outData, size_t* dataSize) {
    if (outData == nullptr || dataSize == nullptr) {
        return -EINVAL;
    }
    *outData = mData;
    *dataSize = mDataSize;
    return 0;
}

AllocatedFrame::AllocatedFrame(
        uint32_t w, uint32_t h) :
        Frame(w, h, V4L2_PIX_FMT_YUV420) {};

AllocatedFrame::~AllocatedFrame() {}

int AllocatedFrame::allocate(YCbCrLayout* out) {
    std::lock_guard<std::mutex> lk(mLock);
    if ((mWidth % 2) || (mHeight % 2)) {
        ALOGE("%s: bad dimension %dx%d (not multiple of 2)", __FUNCTION__, mWidth, mHeight);
        return -EINVAL;
    }

    size_t dataSize = mWidth * mHeight * 3 / 2;  // YUV420

    size_t cbWidth = mWidth / 2;
    size_t requiredCbWidth = DCTSIZE * ((cbWidth + DCTSIZE - 1) / DCTSIZE);
    size_t padding = requiredCbWidth - cbWidth;
    size_t finalSize = dataSize + padding;

    if (mData.size() != finalSize) {
        mData.resize(finalSize);
    }

    if (out != nullptr) {
        out->y = mData.data();
        out->yStride = mWidth;
        uint8_t* cbStart = mData.data() + mWidth * mHeight;
        uint8_t* crStart = cbStart + mWidth * mHeight / 4;
        out->cb = cbStart;
        out->cr = crStart;
        out->cStride = mWidth / 2;
        out->chromaStep = 1;
    }
    return 0;
}

int AllocatedFrame::getData(uint8_t** outData, size_t* dataSize) {
    YCbCrLayout layout;
    int ret = allocate(&layout);
    if (ret != 0) {
        return ret;
    }
    *outData = mData.data();
    *dataSize = mData.size();
    return 0;
}

int AllocatedFrame::getLayout(YCbCrLayout* out) {
    IMapper::Rect noCrop = {0, 0,
            static_cast<int32_t>(mWidth),
            static_cast<int32_t>(mHeight)};
    return getCroppedLayout(noCrop, out);
}

int AllocatedFrame::getCroppedLayout(const IMapper::Rect& rect, YCbCrLayout* out) {
    if (out == nullptr) {
        ALOGE("%s: null out", __FUNCTION__);
        return -1;
    }

    std::lock_guard<std::mutex> lk(mLock);
    if ((rect.left + rect.width) > static_cast<int>(mWidth) ||
        (rect.top + rect.height) > static_cast<int>(mHeight) ||
            (rect.left % 2) || (rect.top % 2) || (rect.width % 2) || (rect.height % 2)) {
        ALOGE("%s: bad rect left %d top %d w %d h %d", __FUNCTION__,
                rect.left, rect.top, rect.width, rect.height);
        return -1;
    }

    out->y = mData.data() + mWidth * rect.top + rect.left;
    out->yStride = mWidth;
    uint8_t* cbStart = mData.data() + mWidth * mHeight;
    uint8_t* crStart = cbStart + mWidth * mHeight / 4;
    out->cb = cbStart + mWidth * rect.top / 4 + rect.left / 2;
    out->cr = crStart + mWidth * rect.top / 4 + rect.left / 2;
    out->cStride = mWidth / 2;
    out->chromaStep = 1;
    return 0;
}

bool isAspectRatioClose(float ar1, float ar2) {
    const float kAspectRatioMatchThres = 0.025f;
    return (std::abs(ar1 - ar2) < kAspectRatioMatchThres);
}

double SupportedPTPFormat::FrameRate::getDouble() const {
    return durationDenominator / static_cast<double>(durationNumerator);
}

::android::hardware::camera::common::V1_0::Status importBufferImpl(
        /*inout*/std::map<int, CirculatingBuffers>& circulatingBuffers,
        /*inout*/HandleImporter& handleImporter,
        int32_t streamId,
        uint64_t bufId, buffer_handle_t buf,
        /*out*/buffer_handle_t** outBufPtr,
        bool allowEmptyBuf) {
    using ::android::hardware::camera::common::V1_0::Status;
    if (buf == nullptr && bufId == BUFFER_ID_NO_BUFFER) {
        if (allowEmptyBuf) {
            *outBufPtr = &sEmptyBuffer;
            return Status::OK;
        } else {
            ALOGE("%s: bufferId %" PRIu64 " has null buffer handle!", __FUNCTION__, bufId);
            return Status::ILLEGAL_ARGUMENT;
        }
    }

    CirculatingBuffers& cbs = circulatingBuffers[streamId];
    if (cbs.count(bufId) == 0) {
        if (buf == nullptr) {
            ALOGE("%s: bufferId %" PRIu64 " has null buffer handle!", __FUNCTION__, bufId);
            return Status::ILLEGAL_ARGUMENT;
        }
        // Register a newly seen buffer
        buffer_handle_t importedBuf = buf;
        handleImporter.importBuffer(importedBuf);
        if (importedBuf == nullptr) {
            ALOGE("%s: output buffer for stream %d is invalid!", __FUNCTION__, streamId);
            return Status::INTERNAL_ERROR;
        } else {
            cbs[bufId] = importedBuf;
        }
    }
    *outBufPtr = &cbs[bufId];
    return Status::OK;
}

uint32_t getFourCcFromLayout(const YCbCrLayout& layout) {
    intptr_t cb = reinterpret_cast<intptr_t>(layout.cb);
    intptr_t cr = reinterpret_cast<intptr_t>(layout.cr);
    if (std::abs(cb - cr) == 1 && layout.chromaStep == 2) {
        // Interleaved format
        if (layout.cb > layout.cr) {
            return V4L2_PIX_FMT_NV21;
        } else {
            return V4L2_PIX_FMT_NV12;
        }
    } else if (layout.chromaStep == 1) {
        // Planar format
        if (layout.cb > layout.cr) {
            return V4L2_PIX_FMT_YVU420; // YV12
        } else {
            return V4L2_PIX_FMT_YUV420; // YU12
        }
    } else {
        return FLEX_YUV_GENERIC;
    }
}

int getCropRect(
        CroppingType ct, const Size& inSize, const Size& outSize, IMapper::Rect* out) {
    if (out == nullptr) {
        ALOGE("%s: out is null", __FUNCTION__);
        return -1;
    }

    uint32_t inW = inSize.width;
    uint32_t inH = inSize.height;
    uint32_t outW = outSize.width;
    uint32_t outH = outSize.height;

    float arIn = ASPECT_RATIO(inSize);
    float arOut = ASPECT_RATIO(outSize);
    if (isAspectRatioClose(arIn, arOut)) {
        out->left = 0;
        out->top = 0;
        out->width = inW;
        out->height = inH;
        return 0;
    }

    if (ct == VERTICAL) {
        uint64_t scaledOutH = static_cast<uint64_t>(outH) * inW / outW;
        if (scaledOutH > inH) {
            ALOGE("%s: Output size %dx%d cannot be vertically cropped from input size %dx%d",
                    __FUNCTION__, outW, outH, inW, inH);
            return -1;
        }
        scaledOutH = scaledOutH & ~0x1; // make it multiple of 2

        out->left = 0;
        out->top = ((inH - scaledOutH) / 2) & ~0x1;
        out->width = inW;
        out->height = static_cast<int32_t>(scaledOutH);
    } else {
        uint64_t scaledOutW = static_cast<uint64_t>(outW) * inH / outH;
        if (scaledOutW > inW) {
            ALOGE("%s: Output size %dx%d cannot be horizontally cropped from input size %dx%d",
                    __FUNCTION__, outW, outH, inW, inH);
            return -1;
        }
        scaledOutW = scaledOutW & ~0x1; // make it multiple of 2

        out->left = ((inW - scaledOutW) / 2) & ~0x1;
        out->top = 0;
        out->width = static_cast<int32_t>(scaledOutW);
        out->height = inH;
    }

    return 0;
}

int formatConvert(
        const YCbCrLayout& in, const YCbCrLayout& out, Size sz, uint32_t format) {
    int ret = 0;
    switch (format) {
        case V4L2_PIX_FMT_NV21:
            ret = libyuv::I420ToNV21(
                    static_cast<uint8_t*>(in.y),
                    in.yStride,
                    static_cast<uint8_t*>(in.cb),
                    in.cStride,
                    static_cast<uint8_t*>(in.cr),
                    in.cStride,
                    static_cast<uint8_t*>(out.y),
                    out.yStride,
                    static_cast<uint8_t*>(out.cr),
                    out.cStride,
                    sz.width,
                    sz.height);
            break;
        case V4L2_PIX_FMT_NV12:
            ret = libyuv::I420ToNV12(
                    static_cast<uint8_t*>(in.y),
                    in.yStride,
                    static_cast<uint8_t*>(in.cb),
                    in.cStride,
                    static_cast<uint8_t*>(in.cr),
                    in.cStride,
                    static_cast<uint8_t*>(out.y),
                    out.yStride,
                    static_cast<uint8_t*>(out.cb),
                    out.cStride,
                    sz.width,
                    sz.height);
            break;
        case V4L2_PIX_FMT_YVU420: // YV12
        case V4L2_PIX_FMT_YUV420: // YU12
            ret = libyuv::I420Copy(
                    static_cast<uint8_t*>(in.y),
                    in.yStride,
                    static_cast<uint8_t*>(in.cb),
                    in.cStride,
                    static_cast<uint8_t*>(in.cr),
                    in.cStride,
                    static_cast<uint8_t*>(out.y),
                    out.yStride,
                    static_cast<uint8_t*>(out.cb),
                    out.cStride,
                    static_cast<uint8_t*>(out.cr),
                    out.cStride,
                    sz.width,
                    sz.height);
            break;
        case FLEX_YUV_GENERIC:
            return -1;
        default:
            return -1;
    }
    return ret;
}

int encodeJpegYU12(
        const Size &inSz, const YCbCrLayout& inLayout,
        int jpegQuality, const void *app1Buffer, size_t app1Size,
        void *out, const size_t maxOutSize, size_t &actualCodeSize)
{
    struct CustomJpegDestMgr {
        struct jpeg_destination_mgr mgr;
        JOCTET *mBuffer;
        size_t mBufferSize;
        size_t mEncodedSize;
        bool mSuccess;
    } dmgr;

    jpeg_compress_struct cinfo = {};
    jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);

    cinfo.err->output_message = [](j_common_ptr cinfo) {
        char buffer[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buffer);
        ALOGE("libjpeg error: %s", buffer);
    };
    cinfo.err->error_exit = [](j_common_ptr cinfo) {
        (*cinfo->err->output_message)(cinfo);
        if(cinfo->client_data) {
            auto & dmgr =
                *reinterpret_cast<CustomJpegDestMgr*>(cinfo->client_data);
            dmgr.mSuccess = false;
        }
    };
    jpeg_create_compress(&cinfo);

    dmgr.mBuffer = static_cast<JOCTET*>(out);
    dmgr.mBufferSize = maxOutSize;
    dmgr.mEncodedSize = 0;
    dmgr.mSuccess = true;
    cinfo.client_data = static_cast<void*>(&dmgr);

    dmgr.mgr.init_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = reinterpret_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.mgr.next_output_byte = dmgr.mBuffer;
        dmgr.mgr.free_in_buffer = dmgr.mBufferSize;
    };

    dmgr.mgr.empty_output_buffer = [](j_compress_ptr cinfo __unused) {
        return 0;
    };

    dmgr.mgr.term_destination = [](j_compress_ptr cinfo) {
        auto & dmgr = reinterpret_cast<CustomJpegDestMgr&>(*cinfo->dest);
        dmgr.mEncodedSize = dmgr.mBufferSize - dmgr.mgr.free_in_buffer;
    };
    cinfo.dest = reinterpret_cast<struct jpeg_destination_mgr*>(&dmgr);

    cinfo.image_width = inSz.width;
    cinfo.image_height = inSz.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;

    jpeg_set_defaults(&cinfo);

    jpeg_set_quality(&cinfo, jpegQuality, 1);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    cinfo.raw_data_in = 1;
    cinfo.dct_method = JDCT_IFAST;

    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;

    int maxVSampFactor = std::max( {
        cinfo.comp_info[0].v_samp_factor,
        cinfo.comp_info[1].v_samp_factor,
        cinfo.comp_info[2].v_samp_factor
    });
    int cVSubSampling = cinfo.comp_info[0].v_samp_factor /
                        cinfo.comp_info[1].v_samp_factor;

    jpeg_start_compress(&cinfo, TRUE);

    size_t mcuV = DCTSIZE*maxVSampFactor;
    size_t paddedHeight = mcuV * ((inSz.height + mcuV - 1) / mcuV);

    std::vector<JSAMPROW> yLines (paddedHeight);
    std::vector<JSAMPROW> cbLines(paddedHeight/cVSubSampling);
    std::vector<JSAMPROW> crLines(paddedHeight/cVSubSampling);

    uint8_t *py = static_cast<uint8_t*>(inLayout.y);
    uint8_t *pcr = static_cast<uint8_t*>(inLayout.cr);
    uint8_t *pcb = static_cast<uint8_t*>(inLayout.cb);

    for(uint32_t i = 0; i < paddedHeight; i++)
    {
        int li = std::min(i, static_cast<uint32_t>(inSz.height - 1));
        yLines[i]  = static_cast<JSAMPROW>(py + li * inLayout.yStride);
        if(i < paddedHeight / cVSubSampling)
        {
            li = std::min(i, static_cast<uint32_t>((inSz.height - 1) / cVSubSampling));
            crLines[i] = static_cast<JSAMPROW>(pcr + li * inLayout.cStride);
            cbLines[i] = static_cast<JSAMPROW>(pcb + li * inLayout.cStride);
        }
    }

    if(app1Buffer && app1Size)
    {
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1,
             static_cast<const JOCTET*>(app1Buffer), app1Size);
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        const uint32_t batchSize = DCTSIZE * maxVSampFactor;
        const uint32_t nl = cinfo.next_scanline;
        JSAMPARRAY planes[3]{ &yLines[nl],
                              &cbLines[nl/cVSubSampling],
                              &crLines[nl/cVSubSampling] };

        uint32_t done = jpeg_write_raw_data(&cinfo, planes, batchSize);

        if (done != batchSize) {
            return -1;
        }
    }

    jpeg_finish_compress(&cinfo);
    actualCodeSize = dmgr.mEncodedSize;
    jpeg_destroy_compress(&cinfo);

    return 0;
}

Size getMaxThumbnailResolution(const common::V1_0::helper::CameraMetadata& chars) {
    Size thumbSize { 0, 0 };
    camera_metadata_ro_entry entry =
        chars.find(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES);
    for(uint32_t i = 0; i < entry.count; i += 2) {
        Size sz { static_cast<uint32_t>(entry.data.i32[i]),
                  static_cast<uint32_t>(entry.data.i32[i+1]) };
        if(sz.width * sz.height > thumbSize.width * thumbSize.height) {
            thumbSize = sz;
        }
    }

    if (thumbSize.width * thumbSize.height == 0) {
        ALOGW("%s: non-zero thumbnail size not available", __FUNCTION__);
    }

    return thumbSize;
}

void freeReleaseFences(hidl_vec<V3_2::CaptureResult>& results) {
    for (auto& result : results) {
        if (result.inputBuffer.releaseFence.getNativeHandle() != nullptr) {
            native_handle_t* handle = const_cast<native_handle_t*>(
                    result.inputBuffer.releaseFence.getNativeHandle());
            native_handle_close(handle);
            native_handle_delete(handle);
        }
        for (auto& buf : result.outputBuffers) {
            if (buf.releaseFence.getNativeHandle() != nullptr) {
                native_handle_t* handle = const_cast<native_handle_t*>(
                        buf.releaseFence.getNativeHandle());
                native_handle_close(handle);
                native_handle_delete(handle);
            }
        }
    }
    return;
}
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define UPDATE(md, tag, data, size)               \
do {                                              \
    if ((md).update((tag), (data), (size))) {     \
        ALOGE("Update " #tag " failed!");         \
        return BAD_VALUE;                         \
    }                                             \
} while (0)
status_t fillCaptureResultCommon(
    common::V1_0::helper::CameraMetadata &md, nsecs_t timestamp,
    camera_metadata_ro_entry& activeArraySize) {
if (activeArraySize.count < 4) {
    ALOGE("%s: cannot find active array size!", __FUNCTION__);
    return -EINVAL;
}
// android.control
// For USB camera, we don't know the AE state. Set the state to converged to
// indicate the frame should be good to use. Then apps don't have to wait the
// AE state.
const uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
UPDATE(md, ANDROID_CONTROL_AE_STATE, &aeState, 1);

const uint8_t ae_lock = ANDROID_CONTROL_AE_LOCK_OFF;
UPDATE(md, ANDROID_CONTROL_AE_LOCK, &ae_lock, 1);

// Set AWB state to converged to indicate the frame should be good to use.
const uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
UPDATE(md, ANDROID_CONTROL_AWB_STATE, &awbState, 1);

const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
UPDATE(md, ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);

const uint8_t flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
UPDATE(md, ANDROID_FLASH_STATE, &flashState, 1);

// This means pipeline latency of X frame intervals. The maximum number is 4.
const uint8_t requestPipelineMaxDepth = 4;
UPDATE(md, ANDROID_REQUEST_PIPELINE_DEPTH, &requestPipelineMaxDepth, 1);

// android.scaler
const int32_t crop_region[] = {
      activeArraySize.data.i32[0], activeArraySize.data.i32[1],
      activeArraySize.data.i32[2], activeArraySize.data.i32[3],
};
UPDATE(md, ANDROID_SCALER_CROP_REGION, crop_region, ARRAY_SIZE(crop_region));

// android.sensor
UPDATE(md, ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);

// android.statistics
const uint8_t lensShadingMapMode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
UPDATE(md, ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMapMode, 1);

const uint8_t sceneFlicker = ANDROID_STATISTICS_SCENE_FLICKER_NONE;
UPDATE(md, ANDROID_STATISTICS_SCENE_FLICKER, &sceneFlicker, 1);

return OK;
}

}  // namespace implementation
}  // namespace V3_4
}  // namespace device


namespace ptp {
namespace common {

namespace {
    const int kDefaultCameraIdOffset = 100;
    const int kDefaultJpegBufSize = 5 << 20; // 5MB
    const int kDefaultNumVideoBuffer = 4;
    const int kDefaultNumStillBuffer = 2;
    const int kDefaultOrientation = 0;
} // anonymous namespace

const char* PTPCameraConfig::kDefaultCfgPath = "/vendor/etc/external_camera_config.xml";

PTPCameraConfig PTPCameraConfig::loadFromCfg(const char* cfgPath) {
    using namespace tinyxml2;
    PTPCameraConfig ret;

    XMLDocument configXml;
    XMLError err = configXml.LoadFile(cfgPath);
    if (err != XML_SUCCESS) {
        ALOGE("%s: Unable to load external camera config file '%s'. Error: %s",
                __FUNCTION__, cfgPath, XMLDocument::ErrorIDToName(err));
        return ret;
    }

    XMLElement *extCam = configXml.FirstChildElement("ExternalCamera");
    if (extCam == nullptr) {
        return ret;
    }

    XMLElement *providerCfg = extCam->FirstChildElement("Provider");
    if (providerCfg == nullptr) {
        return ret;
    }

    XMLElement *cameraIdOffset = providerCfg->FirstChildElement("CameraIdOffset");
    if (cameraIdOffset != nullptr) {
        ret.cameraIdOffset = std::atoi(cameraIdOffset->GetText());
    }

    XMLElement *ignore = providerCfg->FirstChildElement("ignore");
    if (ignore != nullptr) {
        XMLElement *id = ignore->FirstChildElement("id");
        while (id != nullptr) {
            const char* text = id->GetText();
            if (text != nullptr) {
                ret.mInternalDevices.insert(text);
            }
            id = id->NextSiblingElement("id");
        }
    }

    XMLElement *deviceCfg = extCam->FirstChildElement("Device");
    if (deviceCfg == nullptr) {
        return ret;
    }

    XMLElement *jpegBufSz = deviceCfg->FirstChildElement("MaxJpegBufferSize");
    if (jpegBufSz != nullptr) {
        ret.maxJpegBufSize = jpegBufSz->UnsignedAttribute("bytes", /*Default*/kDefaultJpegBufSize);
    }

    XMLElement *numVideoBuf = deviceCfg->FirstChildElement("NumVideoBuffers");
    if (numVideoBuf != nullptr) {
        ret.numVideoBuffers =
                numVideoBuf->UnsignedAttribute("count", /*Default*/kDefaultNumVideoBuffer);
    }

    XMLElement *numStillBuf = deviceCfg->FirstChildElement("NumStillBuffers");
    if (numStillBuf != nullptr) {
        ret.numStillBuffers =
                numStillBuf->UnsignedAttribute("count", /*Default*/kDefaultNumStillBuffer);
    }

    XMLElement *fpsList = deviceCfg->FirstChildElement("FpsList");
    if (fpsList != nullptr) {
        if (!updateFpsList(fpsList, ret.fpsLimits)) {
            return ret;
        }
    }

    XMLElement *depth = deviceCfg->FirstChildElement("Depth16Supported");
    if (depth != nullptr) {
        ret.depthEnabled = depth->BoolAttribute("enabled", false);
    }

    if(ret.depthEnabled) {
        XMLElement *depthFpsList = deviceCfg->FirstChildElement("DepthFpsList");
        if (depthFpsList != nullptr) {
            if(!updateFpsList(depthFpsList, ret.depthFpsLimits)) {
                return ret;
            }
        }
    }

    XMLElement *minStreamSize = deviceCfg->FirstChildElement("MinimumStreamSize");
    if (minStreamSize != nullptr) {
        ret.minStreamSize = {
                minStreamSize->UnsignedAttribute("width", /*Default*/0),
                minStreamSize->UnsignedAttribute("height", /*Default*/0)};
    }

    XMLElement *orientation = deviceCfg->FirstChildElement("Orientation");
    if (orientation != nullptr) {
        ret.orientation = orientation->IntAttribute("degree", /*Default*/kDefaultOrientation);
    }

    return ret;
}

bool PTPCameraConfig::updateFpsList(tinyxml2::XMLElement* fpsList,
        std::vector<FpsLimitation>& fpsLimits) {
    using namespace tinyxml2;
    std::vector<FpsLimitation> limits;
    XMLElement* row = fpsList->FirstChildElement("Limit");
    while (row != nullptr) {
        FpsLimitation prevLimit{{0, 0}, 1000.0};
        FpsLimitation limit;
        limit.size = {row->UnsignedAttribute("width", /*Default*/ 0),
                      row->UnsignedAttribute("height", /*Default*/ 0)};
        limit.fpsUpperBound = row->DoubleAttribute("fpsBound", /*Default*/ 1000.0);
        if (limit.size.width <= prevLimit.size.width ||
            limit.size.height <= prevLimit.size.height ||
            limit.fpsUpperBound >= prevLimit.fpsUpperBound) {
            return false;
        }
        limits.push_back(limit);
        row = row->NextSiblingElement("Limit");
    }
    fpsLimits = limits;
    return true;
}

PTPCameraConfig::PTPCameraConfig() :
        cameraIdOffset(kDefaultCameraIdOffset),
        maxJpegBufSize(kDefaultJpegBufSize),
        numVideoBuffers(kDefaultNumVideoBuffer),
        numStillBuffers(kDefaultNumStillBuffer),
        depthEnabled(false),
        orientation(kDefaultOrientation) {
    fpsLimits.push_back({/*Size*/{ 640,  480}, /*FPS upper bound*/30.0});
    fpsLimits.push_back({/*Size*/{1280,  720}, /*FPS upper bound*/7.5});
    fpsLimits.push_back({/*Size*/{1920, 1080}, /*FPS upper bound*/5.0});
    minStreamSize = {0, 0};
}


}  // namespace common
}  // namespace ptp
}  // namespace camera
}  // namespace hardware
}  // namespace android
