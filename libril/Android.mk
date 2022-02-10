# Copyright 2006 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES:= \
    ril.cpp \
    ril_event.cpp \
    RilSapSocket.cpp \
    ril_service.cpp \
    sap_service.cpp \
    se_service.cpp \
    ril_config.cpp \

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libcutils \
    libhardware_legacy \
    librilutils \
    android.hardware.radio@1.0 \
    android.hardware.radio@1.1 \
    android.hardware.radio@1.2 \
    android.hardware.radio@1.3 \
    android.hardware.radio@1.4 \
    android.hardware.radio.deprecated@1.0 \
    android.hardware.radio.config@1.0 \
    android.hardware.radio.config@1.1 \
    android.hardware.radio.config@1.2 \
    android.hardware.secure_element@1.0 \
    libhidlbase  \
    libhidltransport \
    libhwbinder \
    vendor.sprd.hardware.radio@1.0 \

LOCAL_STATIC_LIBRARIES := \
    libprotobuf-c-nano-enable_malloc-32bit \

LOCAL_CFLAGS += -Wall -Wextra -Wno-unused-parameter -Werror
LOCAL_CFLAGS += -DPB_FIELD_32BIT

ifneq ($(SIM_COUNT), 1)
    LOCAL_CFLAGS += -DANDROID_MULTI_SIM
endif

ifeq ($(SIM_COUNT), 2)
    LOCAL_CFLAGS += -DANDROID_SIM_COUNT_2 -DDSDA_RILD1
endif

ifneq ($(DISABLE_RILD_OEM_HOOK),)
    LOCAL_CFLAGS += -DOEM_HOOK_DISABLED
endif

ifeq ($(EMBMS_ENABLE), 1)
    LOCAL_CFLAGS += -DEMBMS_ENABLE
endif

ifeq ($(MDT_ENABLE), 1)
    LOCAL_CFLAGS += -DMDT_ENABLE
endif

LOCAL_C_INCLUDES += external/nanopb-c
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../include
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/../include

LOCAL_MODULE := librilcore
LOCAL_OVERRIDES_MODULES := libril
LOCAL_SANITIZE := integer

include $(BUILD_SHARED_LIBRARY)
