# Copyright 2006 The Android Open Source Project

# XXX using libutils for simulator build only...
#
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    common/at_tok.c \
    common/atchannel.c \
    common/misc.c \
    common/utils.c \
    common/channel_controller.c \
    custom/ril_custom.c \
    impl_ril.c \
    ril_sim.c \
    ril_network.c \
    ril_data.c \
    ril_call.c \
    ril_ss.c \
    ril_sms.c \
    ril_misc.c \
    ril_stk.c \
    ril_se.c \
    ril_thermal.cpp

LOCAL_SHARED_LIBRARIES := \
    liblog libcutils libutils librilcore librilutils libnetutils

LOCAL_SHARED_LIBRARIES += libhidlbase \
                          libhidltransport \
                          vendor.sprd.hardware.thermal@1.0 \
                          libpowerhal_cli

ifeq ($(strip $(TARGET_ARCH)), arm)
LOCAL_LDFLAGS := $(TARGET_OUT_VENDOR)/lib/libril-private.so
else ifeq ($(strip $(TARGET_ARCH)), arm64)
LOCAL_LDFLAGS_32 := $(TARGET_OUT_VENDOR)/lib/libril-private.so
LOCAL_LDFLAGS_64 := $(TARGET_OUT_VENDOR)/lib64/libril-private.so
else ifeq ($(strip $(TARGET_ARCH)), x86)
LOCAL_LDFLAGS := $(TARGET_OUT_VENDOR)/lib/libril-private.so
else ifeq ($(strip $(TARGET_ARCH)), x86_64)
LOCAL_LDFLAGS_32 := $(TARGET_OUT_VENDOR)/lib/libril-private.so
LOCAL_LDFLAGS_64 := $(TARGET_OUT_VENDOR)/lib64/libril-private.so
endif

# for asprinf
LOCAL_CFLAGS := -D_GNU_SOURCE
LOCAL_CFLAGS += -Wall -Wextra -Wno-unused-parameter -Werror

LOCAL_C_INCLUDES += $(LOCAL_PATH)/../include
LOCAL_C_INCLUDES += external/libnl/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/common

LOCAL_CFLAGS += -DSIM_AUTO_POWERON  -DRIL_EXTENSION

ifneq ($(SIM_COUNT), 1)
    LOCAL_CFLAGS += -DANDROID_MULTI_SIM
endif

ifeq ($(SIM_COUNT), 2)
    LOCAL_CFLAGS += -DANDROID_SIM_COUNT_2
endif

LOCAL_PROPRIETARY_MODULE := true

ifeq (foo,foo)
#build shared library
LOCAL_SHARED_LIBRARIES += \
        libcutils libutils
LOCAL_CFLAGS += -DRIL_SHLIB
LOCAL_MODULE:= libimpl-ril
LOCAL_OVERRIDES_MODULES := libreference-ril
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
else
#build executable
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE:= impl-ril
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_STEM_32 := impl-ril
LOCAL_MODULE_STEM_64 := impl-ril64
LOCAL_MULTILIB := both
include $(BUILD_EXECUTABLE)

endif