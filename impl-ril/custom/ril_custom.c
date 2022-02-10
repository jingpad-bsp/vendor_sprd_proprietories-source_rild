/**
 * ril_custom.c --- Compatible between unisoc and custom
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL-CUSTOM"

#include "impl_ril.h"
#include "ril_custom.h"
#include "utils.h"

static char s_simUnlockType[4][5] = {"pin", "pin2", "puk", "puk2"};

/* Bug 523208 set PIN/PUK remain times to prop. @{ */
void setPinPukRemainTimes(int type, int remainTimes,
                             RIL_SOCKET_ID socketId) {
    char num[ARRAY_SIZE];  // max remain times is 10
    char prop[PROPERTY_VALUE_MAX];

    snprintf(prop, sizeof(prop), PIN_PUK_REMAIN_TIMES_PROP,
              s_simUnlockType[type]);

    RLOGD("set %s, remainTimes = %d for SIM%d", prop, remainTimes, socketId);

    snprintf(num, sizeof(num), "%d", remainTimes);
    setProperty(socketId, prop, num);
}
/* }@ */
