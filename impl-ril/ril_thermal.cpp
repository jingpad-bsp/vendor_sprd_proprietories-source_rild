/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "CPUFREQ"

#include "ril_thermal.h"

#include <utils/Log.h>
#include <string>
#include <power_hal_cli.h>
#include <vendor/sprd/hardware/thermal/1.0/IExtThermal.h>
#include <vendor/sprd/hardware/thermal/1.0/types.h>

using ::vendor::sprd::hardware::thermal::V1_0::IExtThermal;
using ::vendor::sprd::hardware::thermal::V1_0::ExtThermalCmd;

::android::sp<::android::PowerHALManager> powerManager = NULL;
::android::sp<::android::PowerHintScene> sceneCANVIOT = NULL;

::android::sp<IExtThermal> thm = NULL;

void setCPUFrequency(bool enable) {
    if (powerManager == NULL) {
        RLOGD("powerManager is NULL!");
        powerManager = new ::android::PowerHALManager();

        if (powerManager == NULL) {
            RLOGE("return because : powerManager is NULL!");
            return;
        }
        powerManager->init();
    }

    if (sceneCANVIOT == NULL) {
        sceneCANVIOT = powerManager->createPowerHintScene(LOG_TAG,
            static_cast<int>(PowerHintVendor::VENDOR_RADIO_NVIOT), "");
        RLOGD("new seneCANVIOT obj!");
    }

    if (sceneCANVIOT == NULL) {
        RLOGE("return because : sceneCANVIOT is NULL!");
        return;
    }

    if (enable) {
        // lock CUP Frequency
        sceneCANVIOT->acquire();
        RLOGD("set CUP Frequency");
    } else {
        // release CUP Frequency
        sceneCANVIOT->release();
        RLOGD("release CUP Frequency");
    }
}

void setThermal(bool enable) {
    if (thm == NULL) {
        RLOGD("thm is NULL!");
        thm = IExtThermal::getService();
    }

    RLOGD("set thermal and enable = %d!", enable);

    if (thm == NULL) {
        RLOGE("return because : thm is NULL!");
        return;
    }

    if (enable) {
        thm->setExtThermal(ExtThermalCmd::THMCMD_SET_PERF_EN);
        RLOGD("set thermal perf en");
    } else {
        thm->setExtThermal(ExtThermalCmd::THMCMD_SET_PERF_DIS);
        RLOGD("set thermal perf dis");
    }
}
