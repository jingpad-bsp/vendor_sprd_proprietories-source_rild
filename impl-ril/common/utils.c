/**
 * utils.c --- utils functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "utils.h"
#include "impl_ril.h"

int emNStrlen(char *str) {
    return str ? strlen(str) : 0;
}
