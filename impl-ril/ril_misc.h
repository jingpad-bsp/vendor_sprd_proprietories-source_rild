/**
 * ril_misc.h --- Any other requests besides data/sim/call...
 *                process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_MISC_H_
#define RIL_MISC_H_

extern int s_maybeAddCall;
extern int s_screenState;
int processMiscRequests(int request, void *data, size_t datalen,
                           RIL_Token t, RIL_SOCKET_ID socket_id);
int processPropRequests(int request, void *data, size_t datalen, RIL_Token t);
int processMiscUnsolicited(RIL_SOCKET_ID socket_id, const char *s);
void sendCmdSync(int phoneId, char *cmd, char *response, int responseLen);
void sendSignalStrengthCriteriaCommend(RIL_SOCKET_ID socket_id, int commend);
extern int s_smart5GEnable;

void dispatchSPBANDSCAN(RIL_Token t, void *data, void *resp);

#endif  // RIL_MISC_H_
