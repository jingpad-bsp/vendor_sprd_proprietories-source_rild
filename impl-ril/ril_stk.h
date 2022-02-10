/**
 * ril_stk.h --- Requests related to stk
 *               process functions/struct/variables declaration and definition
 *
 *Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_STK_H_
#define RIL_STK_H_

void onModemReset_Stk();
int processStkRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
int processStkUnsolicited(RIL_SOCKET_ID socket_id, const char *s);

#endif  // RIL_STK_H_
