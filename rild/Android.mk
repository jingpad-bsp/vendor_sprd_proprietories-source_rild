# Copyright 2006 The Android Open Source Project

ifndef ENABLE_VENDOR_RIL_SERVICE

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    rild.c

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libdl \
    liblog \
    librilcore

# Temporary hack for broken vendor RILs.
LOCAL_WHOLE_STATIC_LIBRARIES := \
    librilutils

LOCAL_CFLAGS := -DRIL_SHLIB
LOCAL_CFLAGS += -Wall -Wextra -Werror

ifeq ($(SIM_COUNT), 2)
    LOCAL_CFLAGS += -DANDROID_MULTI_SIM
    LOCAL_CFLAGS += -DANDROID_SIM_COUNT_2
endif

ifeq ($(SIM_COUNT), 1)
    LOCAL_VINTF_FRAGMENTS := manifest_singlesim.xml
else
    LOCAL_VINTF_FRAGMENTS := manifest_dualsim.xml
endif

ifeq ($(EMBMS_ENABLE), 1)
    LOCAL_VINTF_FRAGMENTS += manifest_embms.xml
endif

ifeq ($(MDT_ENABLE), 1)
    LOCAL_VINTF_FRAGMENTS += manifest_mdt.xml
endif

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE:= urild
LOCAL_OVERRIDES_MODULES := rild
ifeq ($(PRODUCT_COMPATIBLE_PROPERTY),true)
LOCAL_INIT_RC := unisoc.rild.rc
LOCAL_CFLAGS += -DPRODUCT_COMPATIBLE_PROPERTY
else
LOCAL_INIT_RC := unisoc.rild.legacy.rc
endif

include $(BUILD_EXECUTABLE)

endif
