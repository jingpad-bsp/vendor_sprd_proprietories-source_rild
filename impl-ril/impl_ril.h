/**
 * impl-ril.h --- functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef UNISOC_RIL_H_
#define UNISOC_RIL_H_

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <stdbool.h>
#include <telephony/ril.h>

#include <utils/Log.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <semaphore.h>
#include <cutils/properties.h>

#include "ril_public.h"
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"

#define NEW_AT
#ifdef NEW_AT
#define AT_PREFIX "+SP"
#else
#define AT_PREFIX "^"
#endif

#define AT_COMMAND_LEN             128
#define AT_RESPONSE_LEN            4096
#define ARRAY_SIZE                 128  // cannot change the value

#define NUM_ELEMS(x)               (sizeof(x) / sizeof(x[0]))
#define RIL_UNUSED_PARM(a)         noopRemoveWarning((void *)&(a));
#define AT_CMD_STR(str)            (str), sizeof((str)) - 1

#define MUTEX_ACQUIRE(mutex)        pthread_mutex_lock(&mutex)
#define MUTEX_RELEASE(mutex)        pthread_mutex_unlock(&mutex)
#define MUTEX_INIT(mutex)           pthread_mutex_init(&mutex, NULL)

#define MODEM_CONFIG_PROP          "persist.vendor.radio.modem.config"
#define PRIMARY_SIM_PROP           "persist.vendor.radio.primarysim"
#define LTE_MANUAL_ATTACH_PROP     "persist.radio.manual.attach"
#define VSIM_PRODUCT_PROP          "persist.vendor.radio.vsim.product"
#define MODEM_TTY_PROP             "ro.vendor.modem.tty"
#define MODEM_ETH_PROP             "ro.vendor.modem.eth"
#define ENG_QOS_PROP               "persist.vendor.sys.qosstate"
#define QOS_SDU_ERROR_RATIO        "ril.data.qos_sdu_error_ratio"
#define QOS_RESIDUAL_BIT_ERROR_RATIO     "ril.data.qos_residual_bit_error_ratio"
#define PS_ATTACH_REQUEST_IPV4_MTU       "persist.vendor.ps.att_req.ipv4mtu"
#define ENABLE_SPECIAL_FALLBACK_CAUSE             "persist.vendor.radio.enablefallbackcause"
#define SPECIAL_FALLBACK_CAUSE             "ril.data.special.fallback.cause"

#define AT_RESPONSE_FREE(rsp)   \
{                                \
    at_response_free(rsp);       \
    rsp = NULL;                  \
}

#define FREEMEMORY(data)    \
{                            \
        free(data);          \
        data = NULL;         \
}

void requestTimedCallback(timedCallback callback, void *param,
                          const struct timeval *relativeTime);

#ifdef RIL_SHLIB
extern const struct RIL_Env *s_rilEnv;
#define RIL_onRequestComplete(t, e, response, responselen) \
            s_rilEnv->OnRequestComplete(t, e, response, responselen)
#define RIL_requestTimedCallback(a, b, c) \
            requestTimedCallback(a, b, c)

#if defined (ANDROID_MULTI_SIM)
#define RIL_onUnsolicitedResponse(a, b, c, d) \
            s_rilEnv->OnUnsolicitedResponse(a, b, c, d)
#else
#define RIL_onUnsolicitedResponse(a, b, c, d)         \
{                                                     \
            s_rilEnv->OnUnsolicitedResponse(a, b, c); \
            RIL_UNUSED_PARM(d)                        \
}
#endif
#endif

/* used as parameter by RIL_requestTimedCallback */
typedef struct {
    RIL_SOCKET_ID socket_id;
    void *para;
} CallbackPara;

#if defined (ANDROID_MULTI_SIM)
void onRequest(int request, void *data, size_t datalen,
                      RIL_Token t, RIL_SOCKET_ID socket_id);
#else
void onRequest(int request, void *data, size_t datalen, RIL_Token t);
#endif

extern bool s_isModemSupportCDMA;
extern bool s_isCDMAPhone[SIM_COUNT];
extern bool s_isRadioUnavailable;
extern bool s_isFirstPowerOn[SIM_COUNT];
extern bool s_isFirstSetAttach[SIM_COUNT];
extern int s_modemConfig;
extern bool s_isVoLteEnable;
extern int s_roModemConfig;
extern int s_isSimPresent[SIM_COUNT];
extern sem_t s_sem[SIM_COUNT];
extern RIL_SOCKET_ID s_multiModeSim;
extern RIL_RadioState s_radioState[SIM_COUNT];
extern const RIL_SOCKET_ID s_socketId[SIM_COUNT];
extern const struct timeval TIMEVAL_CALLSTATEPOLL;
extern const struct timeval TIMEVAL_SIMPOLL;
extern struct ATChannels *s_ATChannels[MAX_AT_CHANNELS];
extern bool s_isScanningNetwork;
extern bool s_isNR;
extern bool s_isSA[SIM_COUNT];

extern const char *requestToString(int request);
extern const char *radioStateToString(RIL_RadioState s);
void *noopRemoveWarning(void *a);
int isRadioOn(RIL_SOCKET_ID socket_id);
bool isVoLteEnable();
RIL_RadioState getRadioState(RIL_SOCKET_ID socket_id);
void setRadioState(RIL_SOCKET_ID socket_id, RIL_RadioState newState);
extern bool isPrimaryCardWorkMode(int workMode);
int sendTRData(RIL_SOCKET_ID socket_id, char *data);
int sendELData(RIL_SOCKET_ID socket_id, char *data);
int phoneIsBusy(RIL_SOCKET_ID socket_id);
int sendDtmfData(RIL_SOCKET_ID socket_id, char *data);
void asyncCmdTimedCallback(RIL_Token t, void *data, void *cmd);
bool isNR(void);

#endif  // UNISOC_RIL_H_
