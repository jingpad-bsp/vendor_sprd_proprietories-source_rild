# Copyright (C) 2019 UNISOC Technologies Co.,Ltd.

ifdef_any_of = $(filter-out undefined,$(foreach v,$(1),$(origin $(v))))

ifneq ($(call ifdef_any_of, EMBMS_ENABLE MDT_ENABLE),)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_VENDOR_MODULE := true

LOCAL_SRC_FILES := \
    lite_ril.cpp

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libcutils \
    libimpl-ril \
    libnetutils \
    libhidlbase  \
    libhidltransport \
    libhwbinder \
    vendor.sprd.hardware.radio.lite@1.0 \

LOCAL_CFLAGS := -Wall -Wextra -Wno-unused-parameter -Werror

LOCAL_MODULE:= libril-lite
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)

endif