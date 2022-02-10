# Copyright (C) 2019 UNISOC Technologies Co.,Ltd.

ifndef ENABLE_VENDOR_RIL_SERVICE

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    cli.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../../modules/libatci

LOCAL_SHARED_LIBRARIES := liblog libatci

LOCAL_CFLAGS += -Wall -Wextra -Wno-unused-parameter -Werror

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE:= ril-cli

include $(BUILD_EXECUTABLE)

endif
