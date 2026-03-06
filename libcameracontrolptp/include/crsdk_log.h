#ifndef HARDWARE_GOOGLE_CAMERA_HAL_DEVICES_CRSDK_LOG
#define HARDWARE_GOOGLE_CAMERA_HAL_DEVICES_CRSDK_LOG
#include <log/log.h>


#define CRLOGE(format, ...) ALOGE("CAM_PTPSDK(ERROR): %s, line:%d : " format "\r\n",__func__, __LINE__, ##__VA_ARGS__)
#define CRLOGI(format, ...) ALOGI("CAM_PTPSDK(INFO): %s, line:%d : " format "\r\n",__func__, __LINE__, ##__VA_ARGS__)


#endif  // HARDWARE_GOOGLE_CAMERA_HAL_DEVICES_CRSDK_LOG