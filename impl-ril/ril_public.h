/**
 * ril_public.h --- public functions declaration
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_PUBLIC_H_
#define RIL_PUBLIC_H_

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/*  Structures defined and Functions declaration related with request threads */
/******************************************************************************/

typedef enum {
    AT_CMD_TYPE_UNKOWN = -1,
    AT_CMD_TYPE_SLOW,
    AT_CMD_TYPE_NORMAL,  // normal and fast requests are processed in the same thread,
                         // the difference is that when normal and fast requests
                         // arrived at the same time, fast requests would be processed firstly.
    AT_CMD_TYPE_FAST,
    AT_CMD_TYPE_DATA,
    AT_CMD_TYPE_OTHER,
} ATCmdType;

typedef enum {
    WG_G = 0,
    WG_WG = 1,
    LG_G = 2,
    LWG_G = 3,
    LWG_WG = 4,
    LWG_LWG = 5,
    NRLWG_LWG = 6,
} ModemConfig;

typedef void (*aysncCmdCallback)(RIL_Token t, void *data, void *cmd);

typedef void (*timedCallback)(void *param);

typedef void (*RIL_processRequest)(int request, void *data, size_t datalen,
                                   RIL_Token t, RIL_SOCKET_ID socket_id);

void setChannelInitialized(RIL_SOCKET_ID socket_id);
int getChannel(RIL_SOCKET_ID socket_id);
void putChannel(int channelID);
RIL_SOCKET_ID getSocketIdByChannelID(int channelID);

void requestHandlerInit(RIL_processRequest processRequest, int simCount);
void enqueueRequestMessgae(int request, ATCmdType cmdType, void *data,
                           size_t datalen, RIL_Token t, RIL_SOCKET_ID socket_id);
void enqueueTimedMessage(timedCallback callback, void *param, long uptimeSec);
void enqueueAsyncCmdMessage(RIL_SOCKET_ID socket_id, RIL_Token t, const char *cmd,
                            void *data, aysncCmdCallback callback, long timeout);
void removeAsyncCmdMessage(RIL_Token t);
void onCompleteAsyncCmdMessage(RIL_SOCKET_ID socket_id, const char *cmd,
                               RIL_Token *t, void **data);

int enqueueTimedMessageCancel(timedCallback callback, void *param, long uptimeMsec);
void removeTimedMessage(int serial);

/******************************************************************************/
/*  Structures defined and Functions declaration related with request process */
/******************************************************************************/

/* stk related macro definition functions declaration @{ */
#define STK_SEND_TR_DATA        0
#define STK_SEND_EL_DATA        1
#define STK_GET_PHONE_STATUS    2
#define GET_SIM_STATE           3
#define STK_GET_MD_MODE         4
#define STK_SEND_DTMF_DATA      5
#define STK_NEED_OPEN_CHANNEL   6

typedef int (*RIL_onRequest)(RIL_SOCKET_ID socket_id, char *data, int cmdId);
typedef int (*RIL_setupDataCall)(RIL_SOCKET_ID socket_id, void *data, size_t datalen);
typedef void (*RIL_deactiveDataConnection)(RIL_SOCKET_ID socket_id, void *data, size_t datalen);
typedef void (*RIL_getDefaultBearerNetAccessName)(RIL_SOCKET_ID socket_id, char *apn, size_t size);
typedef void (*RIL_getEthNameByCid)(RIL_SOCKET_ID socket_id, int cid, char *ethName, size_t len);
typedef void (*OnUnsolicitedResponse)(int unsolResponse, const void *data,
                                      size_t datalen, RIL_SOCKET_ID socket_id);
typedef void (*RIL_sendPsDataOffToExtData)(RIL_SOCKET_ID socket_id, int exemptionInfo, int port);

typedef struct {
    RIL_onRequest onRequest;
    RIL_setupDataCall setupDataCall;
    RIL_deactiveDataConnection deactiveDataConnecton;
    RIL_getDefaultBearerNetAccessName getDefaultBearerNetAccessName;
    RIL_getEthNameByCid getEthNameByCid;
    OnUnsolicitedResponse onUnsolicitedResponse;
    RIL_sendPsDataOffToExtData sendPsDataOffToExtData;
} RIL_StkFunctions;

void resetStkVariables();
void initStk(const RIL_StkFunctions *stkFunction);
void setStkServiceRunning(RIL_SOCKET_ID socket_id, bool isRunning);
void sendEvenLoopThread(void *param);
int lunchOpenChannelDialog(char *data, RIL_SOCKET_ID socket_id);
int parseProCmdIndResponse(char *response, RIL_SOCKET_ID socket_id);
void parseDisplayCmdIndResponse(char *response, RIL_SOCKET_ID socket_id);
int reportStkServiceRunning(RIL_SOCKET_ID socket_id, char *lastResponse, size_t size);
/* }@ */

/* signal process related declaration @{ */
typedef void (*RIL_signalStrengthUnsolResponse)(const void *data,
        RIL_SOCKET_ID socket_id);

typedef struct {
    int reportFrequency;
    RIL_onRequest onRequest;
    RIL_signalStrengthUnsolResponse onSignalStrengthUnsolResponse;
} RIL_NetworkFunctions;

void setCESQValue(int simIndex, bool isCDMAPhone);
void setScreenState(int screenState);
void setCESQGlobalArray(int *rsrp, int *ecno, int *rscp, int *ber, int *rxlev, int *ss_rsrp);
void triggerSignalProcess();
void *signalProcess(void *param);
void setModemConfig(int modemConfig);
/* }@ */

void convertBinToHex(char *bin_ptr, int length, unsigned char *hex_ptr);
int convertHexToBin(const char *hex_ptr, int length, char *bin_ptr);
void convertUcs2ToUtf8(unsigned char *ucs2, int len, unsigned char *buf);
void convertGsm7ToUtf8(unsigned char *gsm7bits, int len, unsigned char *utf8);
void convertUcsToUtf8(unsigned char *ucs2, int len, unsigned char *buf);
int utf8Package(unsigned char *utf8, int offset, int v);
void getProperty(RIL_SOCKET_ID socket_id, const char *property, char *value,
                 const char *defaultVal);
void setProperty(RIL_SOCKET_ID socket_id, const char *property,
                 const char *value);

int matchOperatorInfo(char *longName, char *shortName, char *plmn, const char *data);
int RIL_getONS(char *longName, char *shortName, char *plmn);

void onUnsolResponse(int unsolResponse, const void *data,
                               size_t datalen, RIL_SOCKET_ID socket_id);
int onRILRequest(RIL_SOCKET_ID socket_id, char *data, int cmdId);

#ifdef __cplusplus
}
#endif

#endif  // RIL_PUBLIC_H_
