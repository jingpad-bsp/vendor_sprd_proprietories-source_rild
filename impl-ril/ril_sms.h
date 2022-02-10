/**
 * ril_sms.h --- SMS-related requests
 *               process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_SMS_H_
#define RIL_SMS_H_

void onModemReset_Sms();
int processSmsRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
int processSmsUnsolicited(RIL_SOCKET_ID socket_id, const char *s,
                             const char *sms_pdu);

typedef enum{
    TeleserviceIdentifier,
    ServiceCategory,
    OriginatingAddress,
    OriginatingSubaddress,
    DestinationAddress,
    DestinationSubaddress,
    BearerReplyOption,
    CauseCodes,
    BearerData
} RIL_CDMA_SMS_PARAM;
#endif  // RIL_SMS_H_
