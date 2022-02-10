/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef CHANNEL_CONTROLLER_H_
#define CHANNEL_CONTROLLER_H_

#include "impl_ril.h"
#include "ril_data.h"

#define BLOCKED_MAX_COUNT       5

#define AT_RESULT_OK            0
#define AT_RESULT_NG            -1

#define AT_RSP_TYPE_OK          0
#define AT_RSP_TYPE_MID         1
#define AT_RSP_TYPE_ERROR       2
#define AT_RSP_TYPE_CONNECT     3

#define MODEM_ASSERT_PROP       "vendor.ril.modem.assert"

typedef struct cmd_table {
    const char *cmd;
    int len;
    int timeout;  // timeout for response
} cmd_table;

typedef enum {
    MODEM_ALIVE,
    MODEM_OFFLINE,
} ModemState;

extern int s_ATTableSize;
extern int s_fdModemBlockWrite;
extern const cmd_table s_ATTimeoutTable[];
extern ModemState s_modemState;

void *detectModemState();
void *signal_process();

void reWriteIntermediate(ATResponse *sp_response, char *newLine);
void reverseNewIntermediates(ATResponse *sp_response);
int getATResponseType(char *str);
int findInBuf(char *buf, int len, char *needle);

#endif  // CHANNEL_CONTROLLER_H_
