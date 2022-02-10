/* //device/libs/telephony/ril.cpp
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "RILC"

#include <hardware_legacy/power.h>
#include <telephony/ril.h>
#include <telephony/ril_cdma_sms.h>
#include <cutils/sockets.h>
#include <cutils/jstring.h>
#include <telephony/record_stream.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <pthread.h>
#include <cutils/jstring.h>
#include <sys/types.h>
#include <sys/limits.h>
#include <sys/system_properties.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <sys/un.h>
#include <assert.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <cutils/properties.h>
#include <RilSapSocket.h>
#include <ril_service.h>
#include <sap_service.h>
#include <se_service.h>

extern "C" void
RIL_onRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen);

extern "C" void
RIL_onRequestAck(RIL_Token t);
namespace android {

#define PHONE_PROCESS "radio"
#define BLUETOOTH_PROCESS "bluetooth"

#define ANDROID_WAKE_LOCK_NAME "radio-interface"

#define ANDROID_WAKE_LOCK_SECS 0
#define ANDROID_WAKE_LOCK_USECS 200000

#define PROPERTY_RIL_IMPL "gsm.version.ril-impl"

// match with constant in RIL.java
#define MAX_COMMAND_BYTES (8 * 1024)

// Basically: memset buffers that the client library
// shouldn't be using anymore in an attempt to find
// memory usage issues sooner.
#define MEMSET_FREED 1

#define NUM_ELEMS(a)     (sizeof (a) / sizeof (a)[0])

/* Negative values for private RIL errno's */
#define RIL_ERRNO_INVALID_RESPONSE (-1)
#define RIL_ERRNO_NO_MEMORY (-12)

// request, response, and unsolicited msg print macro
#define PRINTBUF_SIZE 8096

enum WakeType {DONT_WAKE, WAKE_PARTIAL};

typedef struct {
    int requestNumber;
    int (*responseFunction) (int slotId, int responseType, int token,
            RIL_Errno e, void *response, size_t responselen);
    WakeType wakeType;
} UnsolResponseInfo;

typedef struct UserCallbackInfo {
    RIL_TimedCallback p_callback;
    void *userParam;
    struct ril_event event;
    struct UserCallbackInfo *p_next;
} UserCallbackInfo;

extern "C" const char * failCauseToString(RIL_Errno);
extern "C" const char * callStateToString(RIL_CallState);
extern "C" const char * radioStateToString(RIL_RadioState);
extern "C" const char * rilSocketIdToString(RIL_SOCKET_ID socket_id);

extern "C"
char ril_service_name_base[MAX_SERVICE_NAME_LENGTH] = RIL_SERVICE_NAME_BASE;
extern "C"
char ril_service_name[MAX_SERVICE_NAME_LENGTH] = RIL1_SERVICE_NAME;

extern "C"
char secureElement_service_name_base[MAX_SERVICE_NAME_LENGTH] = SE_ON_SIM_SERVICE_NAME_BASE;
extern "C"
char secureElement_service_name[MAX_SERVICE_NAME_LENGTH] = SE_ON_SIM1_SERVICE_NAME;

/*******************************************************************/
RIL_RadioFunctions s_callbacks = {0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
SE_Functions s_seCallbacks = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static int s_registerCalled = 0;

int s_wakelock_count = 0;

#ifdef EVENT_LOOP_ENABLED
static pthread_t s_tid_dispatch;
static int s_started = 0;

static int s_fdWakeupRead;
static int s_fdWakeupWrite;

static struct ril_event s_wakeupfd_event;
static pthread_mutex_t s_startupMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_startupCond = PTHREAD_COND_INITIALIZER;
#endif

static pthread_mutex_t s_pendingRequestsMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_wakeLockCountMutex = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests = NULL;

#if (SIM_COUNT >= 2)
static pthread_mutex_t s_pendingRequestsMutex_socket2  = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket2          = NULL;
#endif

#if (SIM_COUNT >= 3)
static pthread_mutex_t s_pendingRequestsMutex_socket3  = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket3          = NULL;
#endif

#if (SIM_COUNT >= 4)
static pthread_mutex_t s_pendingRequestsMutex_socket4  = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket4          = NULL;
#endif

static const struct timeval TIMEVAL_WAKE_TIMEOUT = {ANDROID_WAKE_LOCK_SECS,ANDROID_WAKE_LOCK_USECS};

static UserCallbackInfo *s_last_wake_timeout_info = NULL;

static void *s_lastNITZTimeData = NULL;
static size_t s_lastNITZTimeDataSize;

#if RILC_LOG
    static char printBuf[PRINTBUF_SIZE];
#endif

/*******************************************************************/
static void grabPartialWakeLock();
void releaseWakeLock();
static void wakeTimeoutCallback(void *);

#ifdef RIL_SHLIB
#if defined(ANDROID_MULTI_SIM)
extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen, RIL_SOCKET_ID socket_id);
#else
extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen);
#endif
#endif

#if defined(ANDROID_MULTI_SIM)
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c), (d))
#else
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c))
#endif

static UserCallbackInfo * internalRequestTimedCallback
    (RIL_TimedCallback callback, void *param,
        const struct timeval *relativeTime);

/** Index == requestNumber */
static CommandInfo s_commands[] = {
#include "ril_commands.h"
};

static UnsolResponseInfo s_unsolResponses[] = {
#include "ril_unsol_commands.h"
};

/* OEMSOCKET Request @{ */
static CommandInfo s_oemCommands[] = {
#include "ril_oem_commands.h"
};

static UnsolResponseInfo s_oemUnsolResponses[] = {
#include "ril_oem_unsol_commands.h"
};
/* }@ */

/* VSIM Request @{ */
static CommandInfo s_atcCommands[] = {
#include "ril_atc_commands.h"
};

static UnsolResponseInfo s_atcUnsolResponses[] = {
#include "ril_atc_unsol_commands.h"
};
/* }@ */

/* Radio Config Request @{ */
static CommandInfo s_configCommands[] = {
#include "ril_config_command.h"
};

static UnsolResponseInfo s_configUnsolResponses[] = {
#include "ril_config_unsol_commands.h"
};
/* }@ */

char * RIL_getServiceName() {
    return ril_service_name;
}

char * SE_getServiceName() {
    return secureElement_service_name;
}

RequestInfo *
addRequestToList(int serial, int slotId, int request) {
    RequestInfo *pRI;
    int ret = 0;
    RIL_SOCKET_ID socket_id = (RIL_SOCKET_ID) slotId;
    /* Hook for current context */
    /* pendingRequestsMutextHook refer to &s_pendingRequestsMutex */
    pthread_mutex_t* pendingRequestsMutexHook = &s_pendingRequestsMutex;
    /* pendingRequestsHook refer to &s_pendingRequests */
    RequestInfo**    pendingRequestsHook = &s_pendingRequests;

#if (SIM_COUNT >= 2)
    if (socket_id == RIL_SOCKET_2) {
        pendingRequestsMutexHook = &s_pendingRequestsMutex_socket2;
        pendingRequestsHook = &s_pendingRequests_socket2;
    }
#if (SIM_COUNT >= 3)
    else if (socket_id == RIL_SOCKET_3) {
        pendingRequestsMutexHook = &s_pendingRequestsMutex_socket3;
        pendingRequestsHook = &s_pendingRequests_socket3;
    }
#endif
#if (SIM_COUNT >= 4)
    else if (socket_id == RIL_SOCKET_4) {
        pendingRequestsMutexHook = &s_pendingRequestsMutex_socket4;
        pendingRequestsHook = &s_pendingRequests_socket4;
    }
#endif
#endif

    pRI = (RequestInfo *)calloc(1, sizeof(RequestInfo));
    if (pRI == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        return NULL;
    }

    if (request > 0 && request <= RIL_REQUEST_LAST) {
        pRI->pCI = &(s_commands[request]);
    } else if (request > RIL_EXT_REQUEST_BASE && request <= RIL_EXT_REQUEST_LAST) {
        request = request - RIL_EXT_REQUEST_BASE;
        pRI->pCI = &(s_oemCommands[request]);
    } else if (request > RIL_ATC_REQUEST_BASE && request <= RIL_ATC_REQUEST_LAST) {
        request = request - RIL_ATC_REQUEST_BASE;
        pRI->pCI = &(s_atcCommands[request]);
    } else if (request >= RIL_REQUEST_RADIO_CONFIG_BASE && request <= RIL_REQUEST_RADIO_CONFIG_LAST) {
        request = request - RIL_REQUEST_RADIO_CONFIG_BASE;
        pRI->pCI = &(s_configCommands[request]);
    }

    pRI->token = serial;
    pRI->socket_id = socket_id;

    ret = pthread_mutex_lock(pendingRequestsMutexHook);
    assert (ret == 0);

    pRI->p_next = *pendingRequestsHook;
    *pendingRequestsHook = pRI;

    ret = pthread_mutex_unlock(pendingRequestsMutexHook);
    assert (ret == 0);

    return pRI;
}

static void resendLastNITZTimeData(RIL_SOCKET_ID socket_id) {
    if (s_lastNITZTimeData != NULL) {
        int responseType = (s_callbacks.version >= 13)
                           ? RESPONSE_UNSOLICITED_ACK_EXP
                           : RESPONSE_UNSOLICITED;
        // acquire read lock for the service before calling nitzTimeReceivedInd() since it reads
        // nitzTimeReceived in ril_service
        pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(
                (int) socket_id);
        int rwlockRet = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);

        int ret = radio::nitzTimeReceivedInd(
            (int)socket_id, responseType, 0,
            RIL_E_SUCCESS, s_lastNITZTimeData, s_lastNITZTimeDataSize);
        if (ret == 0) {
            free(s_lastNITZTimeData);
            s_lastNITZTimeData = NULL;
        }
        rwlockRet = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);
    }
}

void onNewCommandConnect(RIL_SOCKET_ID socket_id) {
    // Init Variables
    s_callbacks.initVaribales(socket_id);

    // Inform we are connected and the ril version
    int rilVer = s_callbacks.version;
    RIL_UNSOL_RESPONSE(RIL_UNSOL_RIL_CONNECTED,
                                    &rilVer, sizeof(rilVer), socket_id);

    // implicit radio state changed
    RIL_UNSOL_RESPONSE(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                    NULL, 0, socket_id);

    // Send last NITZ time data, in case it was missed
    if (s_lastNITZTimeData != NULL) {
        resendLastNITZTimeData(socket_id);
    }

    // Get version string
    if (s_callbacks.getVersion != NULL) {
        const char *version;
        version = s_callbacks.getVersion();
        RLOGI("RIL Daemon version: %s\n", version);

        property_set(PROPERTY_RIL_IMPL, version);
    } else {
        RLOGI("RIL Daemon version: unavailable\n");
        property_set(PROPERTY_RIL_IMPL, "unavailable");
    }
}

#ifdef EVENT_LOOP_ENABLED
static void triggerEvLoop() {
    int ret = 0;
    if (!pthread_equal(pthread_self(), s_tid_dispatch)) {
        /* trigger event loop to wakeup. No reason to do this,
         * if we're in the event loop thread */
         do {
            ret = write (s_fdWakeupWrite, " ", 1);
         } while (ret < 0 && errno == EINTR);
    }
}

static void rilEventAddWakeup(struct ril_event *ev) {
    ril_event_add(ev);
    triggerEvLoop();
}

/**
 * A write on the wakeup fd is done just to pop us out of select()
 * We empty the buffer here and then ril_event will reset the timers on the
 * way back down
 */
static void processWakeupCallback(int fd, short flags, void *param) {
    char buff[16];
    int ret = 0;

    RLOGV("processWakeupCallback");

    /* empty our wakeup socket out */
    do {
        ret = read(s_fdWakeupRead, &buff, sizeof(buff));
    } while (ret > 0 || (ret < 0 && errno == EINTR));
}

static void *
eventLoop(void *param) {
    int ret = 0;
    int filedes[2];

    ril_event_init();

    pthread_mutex_lock(&s_startupMutex);

    s_started = 1;
    pthread_cond_broadcast(&s_startupCond);

    pthread_mutex_unlock(&s_startupMutex);

    ret = pipe(filedes);

    if (ret < 0) {
        RLOGE("Error in pipe() errno:%d", errno);
        return NULL;
    }

    s_fdWakeupRead = filedes[0];
    s_fdWakeupWrite = filedes[1];

    fcntl(s_fdWakeupRead, F_SETFL, O_NONBLOCK);

    ril_event_set (&s_wakeupfd_event, s_fdWakeupRead, true,
                processWakeupCallback, NULL);

    rilEventAddWakeup (&s_wakeupfd_event);

    // Only returns on error
    ril_event_loop();
    RLOGE ("error in event_loop_base errno:%d", errno);
    // kill self to restart on error
    kill(0, SIGKILL);

    return NULL;
}
#endif

extern "C" void
RIL_startEventLoop(void) {
#ifdef EVENT_LOOP_ENABLED
    /* spin up eventLoop thread and wait for it to get started */
    s_started = 0;
    pthread_mutex_lock(&s_startupMutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int result = pthread_create(&s_tid_dispatch, &attr, eventLoop, NULL);
    if (result != 0) {
        RLOGE("Failed to create dispatch thread: %s", strerror(result));
        goto done;
    }

    while (s_started == 0) {
        pthread_cond_wait(&s_startupCond, &s_startupMutex);
    }

done:
    pthread_mutex_unlock(&s_startupMutex);
#endif
}

// Used for testing purpose only.
extern "C" void RIL_setcallbacks (const RIL_RadioFunctions *callbacks) {
    memcpy(&s_callbacks, callbacks, sizeof (RIL_RadioFunctions));
}

#if (defined EMBMS_ENABLE) || (defined MDT_ENABLE)
static void *start_lite_service(void *param) {
    int ret = -1;
    void *dlHandle = NULL;
    int (*serviceInit)(const char *) = NULL;
    const char *libName = "libril-lite.so";

    dlHandle = dlopen(libName, RTLD_NOW);
    if (dlHandle == NULL) {
        RLOGE("dlopen %s failed: %s", libName, dlerror());
        return NULL;
    }

    dlerror(); // Clear any previous dlerror
    serviceInit = (int (*)(const char *))dlsym(dlHandle, "service_init");
    if (serviceInit == NULL) {
        RLOGE("service_init not defined or exported in %s", libName);
        return NULL;
    }

#ifdef EMBMS_ENABLE
    ret = serviceInit("embms");
    if (ret < 0) {
        RLOGE("Failed to start embms service");
    }
#endif

#ifdef MDT_ENABLE
    ret = serviceInit("mdt");
    if (ret < 0) {
        RLOGE("Failed to start mdt service");
    }
#endif

    return NULL;
}
#endif

extern "C" void
RIL_register (const RIL_RadioFunctions *callbacks) {
    RLOGI("SIM_COUNT: %d", SIM_COUNT);

    if (callbacks == NULL) {
        RLOGE("RIL_register: RIL_RadioFunctions * null");
        return;
    }
    if (callbacks->version < RIL_VERSION_MIN) {
        RLOGE("RIL_register: version %d is to old, min version is %d",
             callbacks->version, RIL_VERSION_MIN);
        return;
    }

    RLOGE("RIL_register: RIL version %d", callbacks->version);

    if (s_registerCalled > 0) {
        RLOGE("RIL_register has been called more than once. "
                "Subsequent call ignored");
        return;
    }

    memcpy(&s_callbacks, callbacks, sizeof (RIL_RadioFunctions));
    memcpy(&s_seCallbacks, callbacks->seFunctions, sizeof (SE_Functions));

    s_registerCalled = 1;

    RLOGI("s_registerCalled flag set");
    // Little self-check

    for (int i = 0; i < (int)NUM_ELEMS(s_commands); i++) {
        assert(i == s_commands[i].requestNumber);
    }

    for (int i = 0; i < (int)NUM_ELEMS(s_unsolResponses); i++) {
        assert(i + RIL_UNSOL_RESPONSE_BASE
                == s_unsolResponses[i].requestNumber);
    }

    /* RadioInteractor Request @{ */
    for (int i = 1; i <= RIL_EXT_REQUEST_LAST - RIL_EXT_REQUEST_BASE; i++) {
        assert(i == s_oemCommands[i].requestNumber - RIL_EXT_REQUEST_BASE);
    }
    for (int i = 0; i <= RIL_EXT_UNSOL_RESPONSE_LAST -
                            RIL_EXT_UNSOL_RESPONSE_BASE; i++) {
        assert(i == s_oemUnsolResponses[i].requestNumber -
                    RIL_EXT_UNSOL_RESPONSE_BASE);
    }
    /* }@ */

    /* Radio Config Request @{ */
    for (int i = 1; i < (int)NUM_ELEMS(s_configCommands); i++) {
        assert(i == s_configCommands[i].requestNumber - RIL_REQUEST_RADIO_CONFIG_BASE);
    }

    for (int i = 0; i < (int)NUM_ELEMS(s_configUnsolResponses); i++) {
        assert(i == s_configUnsolResponses[i].requestNumber - RIL_UNSOL_RESPONSE_RADIO_CONFIG_BASE);
    }
    /* }@ */
    radio::registerService(&s_callbacks, s_commands);
    radio::registerConfigService(&s_callbacks, s_configCommands);
    RLOGI("RILHIDL called registerService");

#if (defined EMBMS_ENABLE) || (defined MDT_ENABLE)
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    int retValue = pthread_create(&tid, &attr, start_lite_service, NULL);
    if (retValue < 0) {
        RLOGE("Failed to create thread to start lite service");
    }
#endif

    secureElement::registerService(&s_seCallbacks);
    RLOGI("SEHIDL called registerService");
}

extern "C" void
RIL_register_socket (const RIL_RadioFunctions *(*Init)(const struct RIL_Env *, int, char **),
        RIL_SOCKET_TYPE socketType, int argc, char **argv) {

    const RIL_RadioFunctions* UimFuncs = NULL;

    if(Init) {
        UimFuncs = Init(&RilSapSocket::uimRilEnv, argc, argv);

        switch(socketType) {
            case RIL_SAP_SOCKET:
                RilSapSocket::initSapSocket(RIL1_SERVICE_NAME, UimFuncs);

#if (SIM_COUNT >= 2)
                RilSapSocket::initSapSocket(RIL2_SERVICE_NAME, UimFuncs);
#endif

#if (SIM_COUNT >= 3)
                RilSapSocket::initSapSocket(RIL3_SERVICE_NAME, UimFuncs);
#endif

#if (SIM_COUNT >= 4)
                RilSapSocket::initSapSocket(RIL4_SERVICE_NAME, UimFuncs);
#endif
                break;
            default:;
        }

        RLOGI("RIL_register_socket: calling registerService");
        sap::registerService(UimFuncs);
    }
}

// Check and remove RequestInfo if its a response and not just ack sent back
static int
checkAndDequeueRequestInfoIfAck(struct RequestInfo *pRI, bool isAck) {
    int ret = 0;
    /* Hook for current context
       pendingRequestsMutextHook refer to &s_pendingRequestsMutex */
    pthread_mutex_t* pendingRequestsMutexHook = &s_pendingRequestsMutex;
    /* pendingRequestsHook refer to &s_pendingRequests */
    RequestInfo ** pendingRequestsHook = &s_pendingRequests;

    if (pRI == NULL) {
        return 0;
    }

#if (SIM_COUNT >= 2)
    if (pRI->socket_id == RIL_SOCKET_2) {
        pendingRequestsMutexHook = &s_pendingRequestsMutex_socket2;
        pendingRequestsHook = &s_pendingRequests_socket2;
    }
#if (SIM_COUNT >= 3)
        if (pRI->socket_id == RIL_SOCKET_3) {
            pendingRequestsMutexHook = &s_pendingRequestsMutex_socket3;
            pendingRequestsHook = &s_pendingRequests_socket3;
        }
#endif
#if (SIM_COUNT >= 4)
    if (pRI->socket_id == RIL_SOCKET_4) {
        pendingRequestsMutexHook = &s_pendingRequestsMutex_socket4;
        pendingRequestsHook = &s_pendingRequests_socket4;
    }
#endif
#endif
    pthread_mutex_lock(pendingRequestsMutexHook);

    for(RequestInfo **ppCur = pendingRequestsHook
        ; *ppCur != NULL
        ; ppCur = &((*ppCur)->p_next)
    ) {
        if (pRI == *ppCur) {
            ret = 1;
            if (isAck) { // Async ack
                if (pRI->wasAckSent == 1) {
                    RLOGD("Ack was already sent for %s", requestToString(pRI->pCI->requestNumber));
                } else {
                    pRI->wasAckSent = 1;
                }
            } else {
                *ppCur = (*ppCur)->p_next;
            }
            break;
        }
    }

    pthread_mutex_unlock(pendingRequestsMutexHook);

    return ret;
}

extern "C" void
RIL_onRequestAck(RIL_Token t) {
    RequestInfo *pRI;

    RIL_SOCKET_ID socket_id = RIL_SOCKET_1;

    pRI = (RequestInfo *)t;

    if (!checkAndDequeueRequestInfoIfAck(pRI, true)) {
        RLOGE ("RIL_onRequestAck: invalid RIL_Token");
        return;
    }

    socket_id = pRI->socket_id;

#if VDBG
    RLOGD("Request Ack, %s", rilSocketIdToString(socket_id));
#endif

    appendPrintBuf("Ack [%04d]< %s", pRI->token, requestToString(pRI->pCI->requestNumber));

    if (pRI->cancelled == 0) {
        pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(
                (int) socket_id);
        int rwlockRet = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);

        radio::acknowledgeRequest((int) socket_id, pRI->token);

        rwlockRet = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);
    }
}
extern "C" void
RIL_onRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen) {
    RequestInfo *pRI;
    int ret = 0;
    RIL_SOCKET_ID socket_id = RIL_SOCKET_1;
    RIL_SOCKET_ID serviceId = RIL_SOCKET_1;

    pRI = (RequestInfo *)t;

    if (!checkAndDequeueRequestInfoIfAck(pRI, false)) {
        RLOGE ("RIL_onRequestComplete: invalid RIL_Token");
        return;
    }

    socket_id = pRI->socket_id;
    serviceId = (RIL_SOCKET_ID)radio::getServiceIdBySocketId((int)socket_id);

#if VDBG
    RLOGD("RequestComplete, %s", rilSocketIdToString(serviceId));
#endif

    if (pRI->local > 0) {
        // Locally issued command...void only!
        // response does not go back up the command socket
        RLOGD("C[locl]< %s", requestToString(pRI->pCI->requestNumber));

        if (pRI->cb != NULL) {
            pRI->cb(pRI->data, pRI->dataLen);
        }
        free(pRI);
        return;
    }

    appendPrintBuf("[%04d]< %s",
        pRI->token, requestToString(pRI->pCI->requestNumber));

    if (pRI->cancelled == 0) {
        int responseType;
        if (s_callbacks.version >= 13 && pRI->wasAckSent == 1) {
            // If ack was already sent, then this call is an asynchronous response. So we need to
            // send id indicating that we expect an ack from RIL.java as we acquire wakelock here.
            responseType = RESPONSE_SOLICITED_ACK_EXP;
            grabPartialWakeLock();
        } else {
            responseType = RESPONSE_SOLICITED;
        }

        // there is a response payload, no matter success or not.
#if VDBG
        RLOGE ("Calling responseFunction() for token %d", pRI->token);
#endif

        pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock((int) serviceId);
        int rwlockRet = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);

        ret = pRI->pCI->responseFunction((int) serviceId,
                responseType, pRI->token, e, response, responselen);

        rwlockRet = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);
    }
    if (pRI->cb != NULL) {
        pRI->cb(pRI->data, pRI->dataLen);
    }
    free(pRI);
}

static void
grabPartialWakeLock() {
    if (s_callbacks.version >= 13) {
        int ret = 0;
        ret = pthread_mutex_lock(&s_wakeLockCountMutex);
        assert(ret == 0);
        acquire_wake_lock(PARTIAL_WAKE_LOCK, ANDROID_WAKE_LOCK_NAME);

        UserCallbackInfo *p_info =
                internalRequestTimedCallback(wakeTimeoutCallback, NULL, &TIMEVAL_WAKE_TIMEOUT);
        if (p_info == NULL) {
            release_wake_lock(ANDROID_WAKE_LOCK_NAME);
        } else {
            s_wakelock_count++;
            if (s_last_wake_timeout_info != NULL) {
                s_last_wake_timeout_info->userParam = (void *)1;
            }
            s_last_wake_timeout_info = p_info;
        }
        ret = pthread_mutex_unlock(&s_wakeLockCountMutex);
        assert(ret == 0);
    } else {
        acquire_wake_lock(PARTIAL_WAKE_LOCK, ANDROID_WAKE_LOCK_NAME);
    }
}

void
releaseWakeLock() {
    if (s_callbacks.version >= 13) {
        int ret = 0;
        ret = pthread_mutex_lock(&s_wakeLockCountMutex);
        assert(ret == 0);

        if (s_wakelock_count > 1) {
            s_wakelock_count--;
        } else {
            s_wakelock_count = 0;
            release_wake_lock(ANDROID_WAKE_LOCK_NAME);
            if (s_last_wake_timeout_info != NULL) {
                s_last_wake_timeout_info->userParam = (void *)1;
            }
        }

        ret = pthread_mutex_unlock(&s_wakeLockCountMutex);
        assert(ret == 0);
    } else {
        release_wake_lock(ANDROID_WAKE_LOCK_NAME);
    }
}

/**
 * Timer callback to put us back to sleep before the default timeout
 */
static void
wakeTimeoutCallback (void *param) {
    // We're using "param != NULL" as a cancellation mechanism
    if (s_callbacks.version >= 13) {
        if (param == NULL) {
            int ret = 0;
            ret = pthread_mutex_lock(&s_wakeLockCountMutex);
            assert(ret == 0);
            s_wakelock_count = 0;
            release_wake_lock(ANDROID_WAKE_LOCK_NAME);
            ret = pthread_mutex_unlock(&s_wakeLockCountMutex);
            assert(ret == 0);
        }
    } else {
        if (param == NULL) {
            releaseWakeLock();
        }
    }
}

#if defined(ANDROID_MULTI_SIM)
extern "C"
void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen, RIL_SOCKET_ID socket_id)
#else
extern "C"
void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen)
#endif
{
    int unsolResponseIndex = 0;
    int ret = 0;
    bool shouldScheduleTimeout = false;
    RIL_SOCKET_ID soc_id = RIL_SOCKET_1;
    RIL_SOCKET_ID serviceId = RIL_SOCKET_1;
    UnsolResponseInfo *pURI = NULL;

#if defined(ANDROID_MULTI_SIM)
    soc_id = socket_id;
#endif
    serviceId = (RIL_SOCKET_ID)radio::getServiceIdBySocketId((int)soc_id);

    if (s_registerCalled == 0) {
        // Ignore RIL_onUnsolicitedResponse before RIL_register
        RLOGW("RIL_onUnsolicitedResponse called before RIL_register");
        return;
    }

    unsolResponseIndex = unsolResponse - RIL_UNSOL_RESPONSE_BASE;

    if ((unsolResponse < RIL_UNSOL_RESPONSE_BASE)
        || (unsolResponse > RIL_UNSOL_RESPONSE_LAST
                && unsolResponse < RIL_UNSOL_RESPONSE_RADIO_CONFIG_BASE)
        || (unsolResponse > RIL_UNSOL_RESPONSE_RADIO_CONFIG_LAST
                && unsolResponse < RIL_EXT_UNSOL_RESPONSE_BASE)
        || (unsolResponse > RIL_EXT_UNSOL_RESPONSE_LAST
                && unsolResponse < RIL_ATC_REQUEST_BASE)
        || (unsolResponse > RIL_ATC_UNSOL_RESPONSE_LAST)) {
        RLOGE("unsupported unsolicited response code %d", unsolResponse);
        return;
    }

    if (unsolResponse >= RIL_UNSOL_RESPONSE_BASE
            && unsolResponse <= RIL_UNSOL_RESPONSE_LAST) {
        unsolResponseIndex = unsolResponse - RIL_UNSOL_RESPONSE_BASE;
        pURI = &(s_unsolResponses[unsolResponseIndex]);
    } else if (unsolResponse >= RIL_EXT_UNSOL_RESPONSE_BASE
            && unsolResponse <= RIL_EXT_UNSOL_RESPONSE_LAST) {
        unsolResponseIndex = unsolResponse - RIL_EXT_UNSOL_RESPONSE_BASE;
        pURI = &(s_oemUnsolResponses[unsolResponseIndex]);
    } else if (unsolResponse >= RIL_ATC_UNSOL_RESPONSE_BASE
            && unsolResponse <= RIL_ATC_UNSOL_RESPONSE_LAST) {
        unsolResponseIndex = unsolResponse - RIL_ATC_UNSOL_RESPONSE_BASE;
        pURI = &(s_atcUnsolResponses[unsolResponseIndex]);
    } else if (unsolResponse >= RIL_UNSOL_RESPONSE_RADIO_CONFIG_BASE
            && unsolResponse <= RIL_UNSOL_RESPONSE_RADIO_CONFIG_LAST) {
        unsolResponseIndex = unsolResponse - RIL_UNSOL_RESPONSE_RADIO_CONFIG_BASE;
        pURI = &(s_configUnsolResponses[unsolResponseIndex]);
    }

    // Grab a wake lock if needed for this reponse,
    // as we exit we'll either release it immediately
    // or set a timer to release it later.
    switch (s_unsolResponses[unsolResponseIndex].wakeType) {
        case WAKE_PARTIAL:
            grabPartialWakeLock();
            shouldScheduleTimeout = true;
        break;

        case DONT_WAKE:
        default:
            // No wake lock is grabed so don't set timeout
            shouldScheduleTimeout = false;
            break;
    }

    appendPrintBuf("[UNSL]< %s", requestToString(unsolResponse));

    int responseType;
    if (s_callbacks.version >= 13
                && s_unsolResponses[unsolResponseIndex].wakeType == WAKE_PARTIAL) {
        responseType = RESPONSE_UNSOLICITED_ACK_EXP;
    } else {
        responseType = RESPONSE_UNSOLICITED;
    }

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock((int) serviceId);
    int rwlockRet;

    if (unsolResponse == RIL_UNSOL_NITZ_TIME_RECEIVED) {
        // get a write lock in caes of NITZ since setNitzTimeReceived() is called
        rwlockRet = pthread_rwlock_wrlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);
        radio::setNitzTimeReceived((int) serviceId, android::elapsedRealtime());
    } else {
        rwlockRet = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(rwlockRet == 0);
    }

    if (pURI != NULL && pURI->responseFunction != NULL) {
        ret = pURI->responseFunction((int) serviceId, responseType, 0, RIL_E_SUCCESS,
                const_cast<void*>(data), datalen);
    }

    rwlockRet = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(rwlockRet == 0);

    if (s_callbacks.version < 13) {
        if (shouldScheduleTimeout) {
            UserCallbackInfo *p_info = internalRequestTimedCallback(wakeTimeoutCallback, NULL,
                    &TIMEVAL_WAKE_TIMEOUT);

            if (p_info == NULL) {
                goto error_exit;
            } else {
                // Cancel the previous request
                if (s_last_wake_timeout_info != NULL) {
                    s_last_wake_timeout_info->userParam = (void *)1;
                }
                s_last_wake_timeout_info = p_info;
            }
        }
    }

#if VDBG
    RLOGI("%s UNSOLICITED: %s length:%zu", rilSocketIdToString(serviceId),
            requestToString(unsolResponse), datalen);
#endif

    if (ret != 0 && unsolResponse == RIL_UNSOL_NITZ_TIME_RECEIVED) {
        // Unfortunately, NITZ time is not poll/update like everything
        // else in the system. So, if the upstream client isn't connected,
        // keep a copy of the last NITZ response (with receive time noted
        // above) around so we can deliver it when it is connected

        if (s_lastNITZTimeData != NULL) {
            free(s_lastNITZTimeData);
            s_lastNITZTimeData = NULL;
        }

        s_lastNITZTimeData = calloc(datalen, 1);
        if (s_lastNITZTimeData == NULL) {
            RLOGE("Memory allocation failed in RIL_onUnsolicitedResponse");
            goto error_exit;
        }
        s_lastNITZTimeDataSize = datalen;
        memcpy(s_lastNITZTimeData, data, datalen);
    }

    // Normal exit
    return;

error_exit:
    if (shouldScheduleTimeout) {
        releaseWakeLock();
    }
}

#ifdef EVENT_LOOP_ENABLED
static void userTimerCallback(int fd, short flags, void *param) {
#else
static void userTimerCallback(void *param) {
#endif
    UserCallbackInfo *p_info;

    p_info = (UserCallbackInfo *)param;

    p_info->p_callback(p_info->userParam);


    // FIXME generalize this...there should be a cancel mechanism
    if (s_last_wake_timeout_info != NULL && s_last_wake_timeout_info == p_info) {
        s_last_wake_timeout_info = NULL;
    }

    free(p_info);
}

/** FIXME generalize this if you track UserCAllbackInfo, clear it
    when the callback occurs
*/
static UserCallbackInfo *
internalRequestTimedCallback (RIL_TimedCallback callback, void *param,
                                const struct timeval *relativeTime)
{
    UserCallbackInfo *p_info;

    p_info = (UserCallbackInfo *) calloc(1, sizeof(UserCallbackInfo));
    if (p_info == NULL) {
        RLOGE("Memory allocation failed in internalRequestTimedCallback");
        return p_info;
    }

    p_info->p_callback = callback;
    p_info->userParam = param;

#ifdef EVENT_LOOP_ENABLED
    struct timeval myRelativeTime;
    if (relativeTime == NULL) {
        /* treat null parameter as a 0 relative time */
        memset (&myRelativeTime, 0, sizeof(myRelativeTime));
    } else {
        /* FIXME I think event_add's tv param is really const anyway */
        memcpy (&myRelativeTime, relativeTime, sizeof(myRelativeTime));
    }

    ril_event_set(&(p_info->event), -1, false, userTimerCallback, p_info);

    ril_timer_add(&(p_info->event), &myRelativeTime);

    triggerEvLoop();
#else
    s_callbacks.handlerTimedCallback(userTimerCallback, p_info,
                        &TIMEVAL_WAKE_TIMEOUT);
#endif

    return p_info;
}

extern "C" void
RIL_requestTimedCallback (RIL_TimedCallback callback, void *param,
                                const struct timeval *relativeTime) {
#ifdef EVENT_LOOP_ENABLED
    internalRequestTimedCallback (callback, param, relativeTime);
#else
    RLOGE("ril event loop has been abandoned");
#endif
}

const char *
failCauseToString(RIL_Errno e) {
    switch(e) {
        case RIL_E_SUCCESS: return "E_SUCCESS";
        case RIL_E_RADIO_NOT_AVAILABLE: return "E_RADIO_NOT_AVAILABLE";
        case RIL_E_GENERIC_FAILURE: return "E_GENERIC_FAILURE";
        case RIL_E_PASSWORD_INCORRECT: return "E_PASSWORD_INCORRECT";
        case RIL_E_SIM_PIN2: return "E_SIM_PIN2";
        case RIL_E_SIM_PUK2: return "E_SIM_PUK2";
        case RIL_E_REQUEST_NOT_SUPPORTED: return "E_REQUEST_NOT_SUPPORTED";
        case RIL_E_CANCELLED: return "E_CANCELLED";
        case RIL_E_OP_NOT_ALLOWED_DURING_VOICE_CALL: return "E_OP_NOT_ALLOWED_DURING_VOICE_CALL";
        case RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW: return "E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW";
        case RIL_E_SMS_SEND_FAIL_RETRY: return "E_SMS_SEND_FAIL_RETRY";
        case RIL_E_SIM_ABSENT:return "E_SIM_ABSENT";
        case RIL_E_ILLEGAL_SIM_OR_ME:return "E_ILLEGAL_SIM_OR_ME";
#ifdef FEATURE_MULTIMODE_ANDROID
        case RIL_E_SUBSCRIPTION_NOT_AVAILABLE:return "E_SUBSCRIPTION_NOT_AVAILABLE";
        case RIL_E_MODE_NOT_SUPPORTED:return "E_MODE_NOT_SUPPORTED";
#endif
        case RIL_E_FDN_CHECK_FAILURE: return "E_FDN_CHECK_FAILURE";
        case RIL_E_MISSING_RESOURCE: return "E_MISSING_RESOURCE";
        case RIL_E_NO_SUCH_ELEMENT: return "E_NO_SUCH_ELEMENT";
        case RIL_E_DIAL_MODIFIED_TO_USSD: return "E_DIAL_MODIFIED_TO_USSD";
        case RIL_E_DIAL_MODIFIED_TO_SS: return "E_DIAL_MODIFIED_TO_SS";
        case RIL_E_DIAL_MODIFIED_TO_DIAL: return "E_DIAL_MODIFIED_TO_DIAL";
        case RIL_E_USSD_MODIFIED_TO_DIAL: return "E_USSD_MODIFIED_TO_DIAL";
        case RIL_E_USSD_MODIFIED_TO_SS: return "E_USSD_MODIFIED_TO_SS";
        case RIL_E_USSD_MODIFIED_TO_USSD: return "E_USSD_MODIFIED_TO_USSD";
        case RIL_E_SS_MODIFIED_TO_DIAL: return "E_SS_MODIFIED_TO_DIAL";
        case RIL_E_SS_MODIFIED_TO_USSD: return "E_SS_MODIFIED_TO_USSD";
        case RIL_E_SUBSCRIPTION_NOT_SUPPORTED: return "E_SUBSCRIPTION_NOT_SUPPORTED";
        case RIL_E_SS_MODIFIED_TO_SS: return "E_SS_MODIFIED_TO_SS";
        case RIL_E_LCE_NOT_SUPPORTED: return "E_LCE_NOT_SUPPORTED";
        case RIL_E_NO_MEMORY: return "E_NO_MEMORY";
        case RIL_E_INTERNAL_ERR: return "E_INTERNAL_ERR";
        case RIL_E_SYSTEM_ERR: return "E_SYSTEM_ERR";
        case RIL_E_MODEM_ERR: return "E_MODEM_ERR";
        case RIL_E_INVALID_STATE: return "E_INVALID_STATE";
        case RIL_E_NO_RESOURCES: return "E_NO_RESOURCES";
        case RIL_E_SIM_ERR: return "E_SIM_ERR";
        case RIL_E_INVALID_ARGUMENTS: return "E_INVALID_ARGUMENTS";
        case RIL_E_INVALID_SIM_STATE: return "E_INVALID_SIM_STATE";
        case RIL_E_INVALID_MODEM_STATE: return "E_INVALID_MODEM_STATE";
        case RIL_E_INVALID_CALL_ID: return "E_INVALID_CALL_ID";
        case RIL_E_NO_SMS_TO_ACK: return "E_NO_SMS_TO_ACK";
        case RIL_E_NETWORK_ERR: return "E_NETWORK_ERR";
        case RIL_E_REQUEST_RATE_LIMITED: return "E_REQUEST_RATE_LIMITED";
        case RIL_E_SIM_BUSY: return "E_SIM_BUSY";
        case RIL_E_SIM_FULL: return "E_SIM_FULL";
        case RIL_E_NETWORK_REJECT: return "E_NETWORK_REJECT";
        case RIL_E_OPERATION_NOT_ALLOWED: return "E_OPERATION_NOT_ALLOWED";
        case RIL_E_EMPTY_RECORD: return "E_EMPTY_RECORD";
        case RIL_E_INVALID_SMS_FORMAT: return "E_INVALID_SMS_FORMAT";
        case RIL_E_ENCODING_ERR: return "E_ENCODING_ERR";
        case RIL_E_INVALID_SMSC_ADDRESS: return "E_INVALID_SMSC_ADDRESS";
        case RIL_E_NO_SUCH_ENTRY: return "E_NO_SUCH_ENTRY";
        case RIL_E_NETWORK_NOT_READY: return "E_NETWORK_NOT_READY";
        case RIL_E_NOT_PROVISIONED: return "E_NOT_PROVISIONED";
        case RIL_E_NO_SUBSCRIPTION: return "E_NO_SUBSCRIPTION";
        case RIL_E_NO_NETWORK_FOUND: return "E_NO_NETWORK_FOUND";
        case RIL_E_DEVICE_IN_USE: return "E_DEVICE_IN_USE";
        case RIL_E_ABORTED: return "E_ABORTED";
        case RIL_E_INVALID_RESPONSE: return "INVALID_RESPONSE";
        case RIL_E_OEM_ERROR_1: return "E_OEM_ERROR_1";
        case RIL_E_OEM_ERROR_2: return "E_OEM_ERROR_2";
        case RIL_E_OEM_ERROR_3: return "E_OEM_ERROR_3";
        case RIL_E_OEM_ERROR_4: return "E_OEM_ERROR_4";
        case RIL_E_OEM_ERROR_5: return "E_OEM_ERROR_5";
        case RIL_E_OEM_ERROR_6: return "E_OEM_ERROR_6";
        case RIL_E_OEM_ERROR_7: return "E_OEM_ERROR_7";
        case RIL_E_OEM_ERROR_8: return "E_OEM_ERROR_8";
        case RIL_E_OEM_ERROR_9: return "E_OEM_ERROR_9";
        case RIL_E_OEM_ERROR_10: return "E_OEM_ERROR_10";
        case RIL_E_OEM_ERROR_11: return "E_OEM_ERROR_11";
        case RIL_E_OEM_ERROR_12: return "E_OEM_ERROR_12";
        case RIL_E_OEM_ERROR_13: return "E_OEM_ERROR_13";
        case RIL_E_OEM_ERROR_14: return "E_OEM_ERROR_14";
        case RIL_E_OEM_ERROR_15: return "E_OEM_ERROR_15";
        case RIL_E_OEM_ERROR_16: return "E_OEM_ERROR_16";
        case RIL_E_OEM_ERROR_17: return "E_OEM_ERROR_17";
        case RIL_E_OEM_ERROR_18: return "E_OEM_ERROR_18";
        case RIL_E_OEM_ERROR_19: return "E_OEM_ERROR_19";
        case RIL_E_OEM_ERROR_20: return "E_OEM_ERROR_20";
        case RIL_E_OEM_ERROR_21: return "E_OEM_ERROR_21";
        case RIL_E_OEM_ERROR_22: return "E_OEM_ERROR_22";
        case RIL_E_OEM_ERROR_23: return "E_OEM_ERROR_23";
        case RIL_E_OEM_ERROR_24: return "E_OEM_ERROR_24";
        case RIL_E_OEM_ERROR_25: return "E_OEM_ERROR_25";
        default: return "<unknown error>";
    }
}

const char *
radioStateToString(RIL_RadioState s) {
    switch(s) {
        case RADIO_STATE_OFF: return "RADIO_OFF";
        case RADIO_STATE_UNAVAILABLE: return "RADIO_UNAVAILABLE";
        case RADIO_STATE_ON:return"RADIO_ON";
        default: return "<unknown state>";
    }
}

const char *
callStateToString(RIL_CallState s) {
    switch(s) {
        case RIL_CALL_ACTIVE : return "ACTIVE";
        case RIL_CALL_HOLDING: return "HOLDING";
        case RIL_CALL_DIALING: return "DIALING";
        case RIL_CALL_ALERTING: return "ALERTING";
        case RIL_CALL_INCOMING: return "INCOMING";
        case RIL_CALL_WAITING: return "WAITING";
        default: return "<unknown state>";
    }
}

const char *requestToString(int request) {
/*
 cat libs/telephony/ril_commands.h \
 | egrep "^ *{RIL_" \
 | sed -re 's/\{RIL_([^,]+),[^,]+,([^}]+).+/case RIL_\1: return "\1";/'


 cat libs/telephony/ril_unsol_commands.h \
 | egrep "^ *{RIL_" \
 | sed -re 's/\{RIL_([^,]+),([^}]+).+/case RIL_\1: return "\1";/'
*/
    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS: return "GET_SIM_STATUS";
        case RIL_REQUEST_ENTER_SIM_PIN: return "ENTER_SIM_PIN";
        case RIL_REQUEST_ENTER_SIM_PUK: return "ENTER_SIM_PUK";
        case RIL_REQUEST_ENTER_SIM_PIN2: return "ENTER_SIM_PIN2";
        case RIL_REQUEST_ENTER_SIM_PUK2: return "ENTER_SIM_PUK2";
        case RIL_REQUEST_CHANGE_SIM_PIN: return "CHANGE_SIM_PIN";
        case RIL_REQUEST_CHANGE_SIM_PIN2: return "CHANGE_SIM_PIN2";
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION: return "ENTER_NETWORK_DEPERSONALIZATION";
        case RIL_REQUEST_GET_CURRENT_CALLS: return "GET_CURRENT_CALLS";
        case RIL_REQUEST_DIAL: return "DIAL";
        case RIL_REQUEST_GET_IMSI: return "GET_IMSI";
        case RIL_REQUEST_HANGUP: return "HANGUP";
        case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND: return "HANGUP_WAITING_OR_BACKGROUND";
        case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND: return "HANGUP_FOREGROUND_RESUME_BACKGROUND";
        case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE: return "SWITCH_WAITING_OR_HOLDING_AND_ACTIVE";
        case RIL_REQUEST_CONFERENCE: return "CONFERENCE";
        case RIL_REQUEST_UDUB: return "UDUB";
        case RIL_REQUEST_LAST_CALL_FAIL_CAUSE: return "LAST_CALL_FAIL_CAUSE";
        case RIL_REQUEST_SIGNAL_STRENGTH: return "SIGNAL_STRENGTH";
        case RIL_REQUEST_VOICE_REGISTRATION_STATE: return "VOICE_REGISTRATION_STATE";
        case RIL_REQUEST_DATA_REGISTRATION_STATE: return "DATA_REGISTRATION_STATE";
        case RIL_REQUEST_OPERATOR: return "OPERATOR";
        case RIL_REQUEST_RADIO_POWER: return "RADIO_POWER";
        case RIL_REQUEST_DTMF: return "DTMF";
        case RIL_REQUEST_SEND_SMS: return "SEND_SMS";
        case RIL_REQUEST_SEND_SMS_EXPECT_MORE: return "SEND_SMS_EXPECT_MORE";
        case RIL_REQUEST_SETUP_DATA_CALL: return "SETUP_DATA_CALL";
        case RIL_REQUEST_SIM_IO: return "SIM_IO";
        case RIL_REQUEST_SEND_USSD: return "SEND_USSD";
        case RIL_REQUEST_CANCEL_USSD: return "CANCEL_USSD";
        case RIL_REQUEST_GET_CLIR: return "GET_CLIR";
        case RIL_REQUEST_SET_CLIR: return "SET_CLIR";
        case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS: return "QUERY_CALL_FORWARD_STATUS";
        case RIL_REQUEST_SET_CALL_FORWARD: return "SET_CALL_FORWARD";
        case RIL_REQUEST_QUERY_CALL_WAITING: return "QUERY_CALL_WAITING";
        case RIL_REQUEST_SET_CALL_WAITING: return "SET_CALL_WAITING";
        case RIL_REQUEST_SMS_ACKNOWLEDGE: return "SMS_ACKNOWLEDGE";
        case RIL_REQUEST_GET_IMEI: return "GET_IMEI";
        case RIL_REQUEST_GET_IMEISV: return "GET_IMEISV";
        case RIL_REQUEST_ANSWER: return "ANSWER";
        case RIL_REQUEST_DEACTIVATE_DATA_CALL: return "DEACTIVATE_DATA_CALL";
        case RIL_REQUEST_QUERY_FACILITY_LOCK: return "QUERY_FACILITY_LOCK";
        case RIL_REQUEST_SET_FACILITY_LOCK: return "SET_FACILITY_LOCK";
        case RIL_REQUEST_CHANGE_BARRING_PASSWORD: return "CHANGE_BARRING_PASSWORD";
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE: return "QUERY_NETWORK_SELECTION_MODE";
        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC: return "SET_NETWORK_SELECTION_AUTOMATIC";
        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL: return "SET_NETWORK_SELECTION_MANUAL";
        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS : return "QUERY_AVAILABLE_NETWORKS ";
        case RIL_REQUEST_DTMF_START: return "DTMF_START";
        case RIL_REQUEST_DTMF_STOP: return "DTMF_STOP";
        case RIL_REQUEST_BASEBAND_VERSION: return "BASEBAND_VERSION";
        case RIL_REQUEST_SEPARATE_CONNECTION: return "SEPARATE_CONNECTION";
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE: return "SET_PREFERRED_NETWORK_TYPE";
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE: return "GET_PREFERRED_NETWORK_TYPE";
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS: return "GET_NEIGHBORING_CELL_IDS";
        case RIL_REQUEST_SET_MUTE: return "SET_MUTE";
        case RIL_REQUEST_GET_MUTE: return "GET_MUTE";
        case RIL_REQUEST_QUERY_CLIP: return "QUERY_CLIP";
        case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE: return "LAST_DATA_CALL_FAIL_CAUSE";
        case RIL_REQUEST_DATA_CALL_LIST: return "DATA_CALL_LIST";
        case RIL_REQUEST_RESET_RADIO: return "RESET_RADIO";
        case RIL_REQUEST_OEM_HOOK_RAW: return "OEM_HOOK_RAW";
        case RIL_REQUEST_OEM_HOOK_STRINGS: return "OEM_HOOK_STRINGS";
        case RIL_REQUEST_SET_BAND_MODE: return "SET_BAND_MODE";
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE: return "QUERY_AVAILABLE_BAND_MODE";
        case RIL_REQUEST_STK_GET_PROFILE: return "STK_GET_PROFILE";
        case RIL_REQUEST_STK_SET_PROFILE: return "STK_SET_PROFILE";
        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND: return "STK_SEND_ENVELOPE_COMMAND";
        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE: return "STK_SEND_TERMINAL_RESPONSE";
        case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM: return "STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM";
        case RIL_REQUEST_SCREEN_STATE: return "SCREEN_STATE";
        case RIL_REQUEST_EXPLICIT_CALL_TRANSFER: return "EXPLICIT_CALL_TRANSFER";
        case RIL_REQUEST_SET_LOCATION_UPDATES: return "SET_LOCATION_UPDATES";
        case RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE:return"CDMA_SET_SUBSCRIPTION_SOURCE";
        case RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE:return"CDMA_SET_ROAMING_PREFERENCE";
        case RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE:return"CDMA_QUERY_ROAMING_PREFERENCE";
        case RIL_REQUEST_SET_TTY_MODE:return"SET_TTY_MODE";
        case RIL_REQUEST_QUERY_TTY_MODE:return"QUERY_TTY_MODE";
        case RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE:return"CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE";
        case RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE:return"CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE";
        case RIL_REQUEST_CDMA_FLASH:return"CDMA_FLASH";
        case RIL_REQUEST_CDMA_BURST_DTMF:return"CDMA_BURST_DTMF";
        case RIL_REQUEST_CDMA_SEND_SMS:return"CDMA_SEND_SMS";
        case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:return"CDMA_SMS_ACKNOWLEDGE";
        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:return"GSM_GET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:return"GSM_SET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION: return "GSM_SMS_BROADCAST_ACTIVATION";
        case RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG:return "CDMA_GET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG:return "CDMA_SET_BROADCAST_SMS_CONFIG";
        case RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION:return "CDMA_SMS_BROADCAST_ACTIVATION";
        case RIL_REQUEST_CDMA_VALIDATE_AND_WRITE_AKEY: return"CDMA_VALIDATE_AND_WRITE_AKEY";
        case RIL_REQUEST_CDMA_SUBSCRIPTION: return"CDMA_SUBSCRIPTION";
        case RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM: return "CDMA_WRITE_SMS_TO_RUIM";
        case RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM: return "CDMA_DELETE_SMS_ON_RUIM";
        case RIL_REQUEST_DEVICE_IDENTITY: return "DEVICE_IDENTITY";
        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE: return "EXIT_EMERGENCY_CALLBACK_MODE";
        case RIL_REQUEST_GET_SMSC_ADDRESS: return "GET_SMSC_ADDRESS";
        case RIL_REQUEST_SET_SMSC_ADDRESS: return "SET_SMSC_ADDRESS";
        case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS: return "REPORT_SMS_MEMORY_STATUS";
        case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING: return "REPORT_STK_SERVICE_IS_RUNNING";
        case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE: return "CDMA_GET_SUBSCRIPTION_SOURCE";
        case RIL_REQUEST_ISIM_AUTHENTICATION: return "ISIM_AUTHENTICATION";
        case RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU: return "RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU";
        case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS: return "RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS";
        case RIL_REQUEST_VOICE_RADIO_TECH: return "VOICE_RADIO_TECH";
        case RIL_REQUEST_WRITE_SMS_TO_SIM: return "WRITE_SMS_TO_SIM";
        case RIL_REQUEST_GET_CELL_INFO_LIST: return"GET_CELL_INFO_LIST";
        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE: return"SET_UNSOL_CELL_INFO_LIST_RATE";
        case RIL_REQUEST_SET_INITIAL_ATTACH_APN: return "RIL_REQUEST_SET_INITIAL_ATTACH_APN";
        case RIL_REQUEST_IMS_REGISTRATION_STATE: return "IMS_REGISTRATION_STATE";
        case RIL_REQUEST_IMS_SEND_SMS: return "IMS_SEND_SMS";
        case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC: return "SIM_TRANSMIT_APDU_BASIC";
        case RIL_REQUEST_SIM_OPEN_CHANNEL: return "SIM_OPEN_CHANNEL";
        case RIL_REQUEST_SIM_CLOSE_CHANNEL: return "SIM_CLOSE_CHANNEL";
        case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL: return "SIM_TRANSMIT_APDU_CHANNEL";
        case RIL_REQUEST_GET_RADIO_CAPABILITY: return "GET_RADIO_CAPABILITY";
        case RIL_REQUEST_SET_RADIO_CAPABILITY: return "SET_RADIO_CAPABILITY";
        case RIL_REQUEST_START_LCE : return "START_LCE";
        case RIL_REQUEST_STOP_LCE : return "STOP_LCE";
        case RIL_REQUEST_SET_UICC_SUBSCRIPTION: return "SET_UICC_SUBSCRIPTION";
        case RIL_REQUEST_ALLOW_DATA: return "ALLOW_DATA";
        case RIL_REQUEST_GET_HARDWARE_CONFIG: return "GET_HARDWARE_CONFIG";
        case RIL_REQUEST_SIM_AUTHENTICATION: return "SIM_AUTHENTICATION";
        case RIL_REQUEST_GET_DC_RT_INFO: return "GET_DC_RT_INFO";
        case RIL_REQUEST_SET_DC_RT_INFO_RATE: return "SET_DC_RT_INFO_RATE";
        case RIL_REQUEST_SET_DATA_PROFILE: return "SET_DATA_PROFILE";
        case RIL_REQUEST_SHUTDOWN: return "SHUTDOWN";
        case RIL_REQUEST_PULL_LCEDATA: return "PULL_LCEDATA";
        case RIL_REQUEST_GET_ACTIVITY_INFO: return "GET_ACTIVITY_INFO";
        case RIL_REQUEST_SET_CARRIER_RESTRICTIONS: return "SET_CARRIER_RESTRICTIONS";
        case RIL_REQUEST_GET_CARRIER_RESTRICTIONS: return "GET_CARRIER_RESTRICTIONS";
        case RIL_REQUEST_SEND_DEVICE_STATE: return "SEND_DEVICE_STATE";
        case RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER: return "SET_UNSOLICITED_RESPONSE_FILTER";
        case RIL_REQUEST_SET_SIM_CARD_POWER: return "SET_SIM_CARD_POWER";
        case RIL_REQUEST_SET_CARRIER_INFO_IMSI_ENCRYPTION: return "SET_CARRIER_INFO_IMSI_ENCRYPTION";
        case RIL_REQUEST_START_NETWORK_SCAN: return "START_NETWORK_SCAN";
        case RIL_REQUEST_STOP_NETWORK_SCAN: return "STOP_NETWORK_SCAN";
        case RIL_REQUEST_START_KEEPALIVE: return "START_KEEPALIVE";
        case RIL_REQUEST_STOP_KEEPALIVE: return "STOP_KEEPALIVE";
        case RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA: return "SET_SIGNAL_STRENGTH_REPORTING_CRITERIA";
        case RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA: return "SET_LINK_CAPACITY_REPORTING_CRITERIA";
        case RIL_REQUEST_SET_SYSTEM_SELECTION_CHANNELS: return "SET_SYSTEM_SELECTION_CHANNELS";
        case RIL_REQUEST_ENABLE_MODEM: return "ENABLE_MODEM";
        case RIL_REQUEST_GET_MODEM_STATUS: return "GET_MODEM_STATUS";
        case RIL_REQUEST_EMERGENCY_DIAL: return "EMERGENCY_DIAL";
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP: return "GET_PREFERRED_NETWORK_TYPE_BITMAP";
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE_BITMAP: return "SET_PREFERRED_NETWORK_TYPE_BITMAP";
        /* IMS @{ */
        case RIL_EXT_REQUEST_GET_IMS_CURRENT_CALLS: return "GET_IMS_CURRENT_CALLS";
        case RIL_EXT_REQUEST_SET_IMS_VOICE_CALL_AVAILABILITY: return "SET_IMS_VOICE_CALL_AVAILABILITY";
        case RIL_EXT_REQUEST_GET_IMS_VOICE_CALL_AVAILABILITY: return "GET_IMS_VOICE_CALL_AVAILABILITY";
        case RIL_EXT_REQUEST_INIT_ISIM: return "INIT_ISIM";
        case RIL_EXT_REQUEST_IMS_CALL_REQUEST_MEDIA_CHANGE: return "IMS_CALL_REQUEST_MEDIA_CHANGE";
        case RIL_EXT_REQUEST_IMS_CALL_RESPONSE_MEDIA_CHANGE: return "IMS_CALL_RESPONSE_MEDIA_CHANGE";
        case RIL_EXT_REQUEST_SET_IMS_SMSC: return "SET_IMS_SMSC";
        case RIL_EXT_REQUEST_IMS_CALL_FALL_BACK_TO_VOICE: return "IMS_CALL_FALL_BACK_TO_VOICE";
        case RIL_EXT_REQUEST_QUERY_CALL_FORWARD_STATUS_URI: return "QUERY_CALL_FORWARD_STATUS_URI";
        case RIL_EXT_REQUEST_SET_CALL_FORWARD_URI: return "SET_CALL_FORWARD_URI";
        case RIL_EXT_REQUEST_IMS_INITIAL_GROUP_CALL: return "IMS_INITIAL_GROUP_CALL";
        case RIL_EXT_REQUEST_IMS_ADD_TO_GROUP_CALL: return "IMS_ADD_TO_GROUP_CALL";
        case RIL_EXT_REQUEST_ENABLE_IMS: return "ENABLE_IMS";
        case RIL_EXT_REQUEST_GET_IMS_BEARER_STATE: return "GET_IMS_BEARER_STATE";
        case RIL_EXT_REQUEST_SET_INITIAL_ATTACH_APN: return "SET_INITIAL_ATTACH_APN";
        case RIL_EXT_REQUEST_IMS_HANDOVER: return "IMS_HANDOVER";
        case RIL_EXT_REQUEST_IMS_HANDOVER_STATUS_UPDATE: return "IMS_HANDOVER_STATUS_UPDATE";
        case RIL_EXT_REQUEST_IMS_NETWORK_INFO_CHANGE: return "IMS_NETWORK_INFO_CHANGE";
        case RIL_EXT_REQUEST_IMS_HANDOVER_CALL_END: return "IMS_HANDOVER_CALL_END";
        case RIL_EXT_REQUEST_IMS_WIFI_ENABLE: return "IMS_WIFI_ENABLE";
        case RIL_EXT_REQUEST_IMS_WIFI_CALL_STATE_CHANGE: return "IMS_WIFI_CALL_STATE_CHANGE";
        case RIL_EXT_REQUEST_IMS_UPDATE_DATA_ROUTER: return "IMS_UPDATE_DATA_ROUTER";
        case RIL_EXT_REQUEST_IMS_HOLD_SINGLE_CALL: return "IMS_HOLD_SINGLE_CALL";
        case RIL_EXT_REQUEST_IMS_MUTE_SINGLE_CALL: return "IMS_MUTE_SINGLE_CALL";
        case RIL_EXT_REQUEST_IMS_SILENCE_SINGLE_CALL: return "IMS_SILENCE_SINGLE_CALL";
        case RIL_EXT_REQUEST_IMS_ENABLE_LOCAL_CONFERENCE: return "ENABLE_LOCAL_CONFERENCE";
        case RIL_EXT_REQUEST_IMS_NOTIFY_HANDOVER_CALL_INFO: return "IMS_NOTIFY_HANDOVER_CALL_INFO";
        case RIL_EXT_REQUEST_GET_IMS_SRVCC_CAPBILITY: return "GET_IMS_SRVCC_CAPBILITY";
        case RIL_EXT_REQUEST_GET_IMS_PCSCF_ADDR: return "GET_IMS_PCSCF_ADDR";
        case RIL_EXT_REQUEST_SET_IMS_PCSCF_ADDR: return "SET_VOWIFI_PCSCF_ADDR";
        case RIL_EXT_REQUEST_QUERY_FACILITY_LOCK: return "EXT_QUERY_FACILITY_LOCK";
        case RIL_EXT_REQUEST_IMS_REGADDR: return "IMS_REGADDR";
        case RIL_EXT_REQUEST_GET_IMS_PANI_INFO: return "CONFIG_GET_IMS_PANI_INFO";
        case RIL_REQUEST_CONFIG_GET_SLOT_STATUS: return "CONFIG_GET_SLOT_STATUS";
        case RIL_REQUEST_CONFIG_SET_SLOT_MAPPING: return "CONFIG_SET_SLOT_MAPPING";
        case RIL_REQUEST_CONFIG_GET_PHONE_CAPABILITY: return "CONFIG_GET_PHONE_CAPABILITY";
        case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM: return "CONFIG_SET_PREFERRED_DATA_MODEM";
        case RIL_REQUEST_CONFIG_SET_MODEM_CONFIG: return "CONFIG_SET_MODEM_CONFIG";
        case RIL_REQUEST_CONFIG_GET_MODEM_CONFIG: return "CONFIG_GET_MODEM_CONFIG";
        /* }@ */
        /* OEM SOCKET REQUEST @{*/
        /* videophone @{ */
        case RIL_EXT_REQUEST_VIDEOPHONE_DIAL: return "VIDEOPHONE_DIAL";
        case RIL_EXT_REQUEST_VIDEOPHONE_CODEC: return "VIDEOPHONE_CODEC";
        case RIL_EXT_REQUEST_VIDEOPHONE_FALLBACK: return "VIDEOPHONE_FALLBACK";
        case RIL_EXT_REQUEST_VIDEOPHONE_STRING: return "VIDEOPHONE_STRING";
        case RIL_EXT_REQUEST_VIDEOPHONE_LOCAL_MEDIA: return "VIDEOPHONE_LOCAL_MEDIA";
        case RIL_EXT_REQUEST_VIDEOPHONE_CONTROL_IFRAME: return "VIDEOPHONE_CONTROL_IFRAME";
        /* }@ */
        case RIL_EXT_REQUEST_TRAFFIC_CLASS: return "TRAFFIC_CLASS";
        case RIL_EXT_REQUEST_ENABLE_LTE: return "ENABLE_LTE";
        case RIL_EXT_REQUEST_ATTACH_DATA: return "ATTACH_DATA";
        case RIL_EXT_REQUEST_FORCE_DETACH: return "FORCE_DETACH";
        case RIL_EXT_REQUEST_GET_HD_VOICE_STATE: return "GET_HD_VOICE_STATE";
        case RIL_EXT_REQUEST_SIMMGR_SIM_POWER: return "SIMMGR_SIM_POWER";
        case RIL_EXT_REQUEST_ENABLE_RAU_NOTIFY: return "ENABLE_RAU_NOTIFY";
        case RIL_EXT_REQUEST_SIM_GET_ATR: return "SIM_GET_ATR";
        case RIL_EXT_REQUEST_EXPLICIT_CALL_TRANSFER: return "EXT_EXPLICIT_CALL_TRANSFER";
        case RIL_EXT_REQUEST_GET_SIM_CAPACITY: return "GET_SIM_CAPACITY";
        case RIL_EXT_REQUEST_STORE_SMS_TO_SIM: return "STORE_SMS_TO_SIM";
        case RIL_EXT_REQUEST_QUERY_SMS_STORAGE_MODE: return "QUERY_SMS_STORAGE_MODE";
        case RIL_EXT_REQUEST_GET_SIMLOCK_REMAIN_TIMES: return "GET_SIMLOCK_REMAIN_TIMES";
        case RIL_EXT_REQUEST_SET_FACILITY_LOCK_FOR_USER: return "SET_FACILITY_LOCK_FOR_USER";
        case RIL_EXT_REQUEST_GET_SIMLOCK_STATUS: return "GET_SIMLOCK_STATUS";
        case RIL_EXT_REQUEST_GET_SIMLOCK_DUMMYS: return "GET_SIMLOCK_DUMMYS";
        case RIL_EXT_REQUEST_GET_SIMLOCK_WHITE_LIST: return "GET_SIMLOCK_WHITE_LIST";
        case RIL_EXT_REQUEST_UPDATE_ECCLIST: return "UPDATE_ECCLIST";
        case RIL_EXT_REQUEST_SET_SINGLE_PDN: return "SET_SINGLE_PDN";
        case RIL_EXT_REQUEST_QUERY_COLP: return "QUERY_COLP";
        case RIL_EXT_REQUEST_QUERY_COLR: return "QUERY_COLR";
        case RIL_EXT_REQUEST_UPDATE_OPERATOR_NAME: return "UPDATE_OPERATOR_NAME";
        case RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS: return "SIMMGR_GET_SIM_STATUS";
        case RIL_EXT_REQUEST_SET_XCAP_IP_ADDR: return "SET_XCAP_IP_ADDR";
        case RIL_EXT_REQUEST_SEND_CMD: return "SEND_CMD";
        case RIL_EXT_REQUEST_REATTACH: return "REATTACH";
        case RIL_EXT_REQUEST_SET_PREFERRED_NETWORK_TYPE: return "EXT_SET_PREFERRED_NETWORK_TYPE";
        case RIL_EXT_REQUEST_SHUTDOWN: return "EXT_SHUTDOWN";
        case RIL_EXT_REQUEST_UPDATE_CLIP: return "UPDATE_CLIP";
        case RIL_EXT_REQUEST_SET_TPMR_STATE: return "EXT_SET_TPMR_STATE";
        case RIL_EXT_REQUEST_GET_TPMR_STATE: return "EXT_GET_TPMR_STATE";
        case RIL_EXT_REQUEST_SET_VIDEO_RESOLUTION: return "SET_VIDEO_RESOLUTION";
        case RIL_EXT_REQUEST_ENABLE_LOCAL_HOLD: return "ENABLE_LOCAL_HOLD";
        case RIL_EXT_REQUEST_ENABLE_WIFI_PARAM_REPORT: return "ENABLE_WIFI_PARAM_REPORT";
        case RIL_EXT_REQUEST_CALL_MEDIA_CHANGE_REQUEST_TIMEOUT: return "CALL_MEDIA_CHANGE_REQUEST_TIMEOUT";
        case RIL_EXT_REQUEST_SET_LOCAL_TONE: return "SET_LOCAL_TONE";
        case RIL_EXT_REQUEST_UPDATE_PLMN: return "UPDATE_PLMN";
        case RIL_EXT_REQUEST_QUERY_PLMN: return "QUERY_PLMN";
        case RIL_EXT_REQUEST_SIM_POWER_REAL: return "REQUEST_SIM_POWER_REAL";
        case RIL_ATC_REQUEST_VSIM_SEND_CMD: return "REQUEST_VSIM_SEND_CMD";
        case RIL_EXT_REQUEST_GET_RADIO_PREFERENCE: return "REQUEST_GET_RADIO_PREFERENCE";
        case RIL_EXT_REQUEST_SET_RADIO_PREFERENCE: return "REQUEST_SET_RADIO_PREFERENCE";
        case RIL_EXT_REQUEST_GET_PREFERRED_NETWORK_TYPE: return "EXT_GET_PREFERRED_NETWORK_TYPE";
        case RIL_EXT_REQUEST_RADIO_POWER_FALLBACK: return "RADIO_POWER_FALLBACK";
        case RIL_EXT_REQUEST_GET_CNAP: return "GET_CNAP";
        case RIL_EXT_REQUEST_SET_LOCATION_INFO: return "EXT_SET_LOCATION_INFO";
        case RIL_EXT_REQUEST_GET_SPECIAL_RATCAP: return "GET_SPECIAL_RATCAP";
        case RIL_EXT_REQUEST_GET_VIDEO_RESOLUTION: return "GET_VIDEO_RESOLUTION";
        case RIL_EXT_REQUEST_SET_EMERGENCY_ONLY: return "SET_EMERGENCY_ONLY";
        case RIL_EXT_REQUEST_GET_SUBSIDYLOCK_STATUS: return "GET_SUBSIDYLOCK_STATUS";
        case RIL_EXT_REQUEST_SET_IMS_USER_AGENT: return "SET_IMS_USER_AGENT";
        case RIL_EXT_REQUEST_RESET_MODEM: return "RESET_MODEM";
        case RIL_EXT_REQUEST_GET_VOLTE_ALLOWED_PLMN: return "GET_VOLTE_ALLOWED_PLMN";
        case RIL_EXT_REQUEST_SET_SMS_BEARER: return "REQUEST_SET_SMS_BEARER";
        case RIL_EXT_REQUEST_GET_SMS_BEARER: return "REQUEST_GET_SMS_BEARER";
        case RIL_EXT_REQUEST_QUERY_ROOT_NODE: return "REQUEST_QUERY_ROOT_NODE";
        case RIL_EXT_REQUEST_PS_DATA_OFF: return "REQUEST_PS_DATA_OFF";
        case RIL_EXT_REQUEST_LTE_SPEED_AND_SIGNAL_STRENGTH: return "LTE_SPEED_AND_SIGNAL_STRENGTH";
        case RIL_EXT_REQUEST_ENABLE_NR_SWITCH: return "ENABLE_NR_SWITCH";
        case RIL_EXT_REQUEST_SET_USBSHARE_SWITCH: return "EXT_USBSHARE_SWITCH";
        case RIL_EXT_REQUEST_SET_STAND_ALONE: return "EXT_REQUEST_SET_STAND_ALONE";
        case RIL_EXT_REQUEST_GET_STAND_ALONE: return "EXT_REQUEST_GET_STAND_ALONE";
        /* }@ */

        case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED: return "UNSOL_RESPONSE_RADIO_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED: return "UNSOL_RESPONSE_CALL_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED: return "UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED";
        case RIL_UNSOL_RESPONSE_NEW_SMS: return "UNSOL_RESPONSE_NEW_SMS";
        case RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT: return "UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT";
        case RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM: return "UNSOL_RESPONSE_NEW_SMS_ON_SIM";
        case RIL_UNSOL_ON_USSD: return "UNSOL_ON_USSD";
        case RIL_UNSOL_ON_USSD_REQUEST: return "UNSOL_ON_USSD_REQUEST(obsolete)";
        case RIL_UNSOL_NITZ_TIME_RECEIVED: return "UNSOL_NITZ_TIME_RECEIVED";
        case RIL_UNSOL_SIGNAL_STRENGTH: return "UNSOL_SIGNAL_STRENGTH";
        case RIL_UNSOL_SUPP_SVC_NOTIFICATION: return "UNSOL_SUPP_SVC_NOTIFICATION";
        case RIL_UNSOL_STK_SESSION_END: return "UNSOL_STK_SESSION_END";
        case RIL_UNSOL_STK_PROACTIVE_COMMAND: return "UNSOL_STK_PROACTIVE_COMMAND";
        case RIL_UNSOL_STK_EVENT_NOTIFY: return "UNSOL_STK_EVENT_NOTIFY";
        case RIL_UNSOL_STK_CALL_SETUP: return "UNSOL_STK_CALL_SETUP";
        case RIL_UNSOL_SIM_SMS_STORAGE_FULL: return "UNSOL_SIM_SMS_STORAGE_FUL";
        case RIL_UNSOL_SIM_REFRESH: return "UNSOL_SIM_REFRESH";
        case RIL_UNSOL_DATA_CALL_LIST_CHANGED: return "UNSOL_DATA_CALL_LIST_CHANGED";
        case RIL_UNSOL_CALL_RING: return "UNSOL_CALL_RING";
        case RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED: return "UNSOL_RESPONSE_SIM_STATUS_CHANGED";
        case RIL_UNSOL_RESPONSE_CDMA_NEW_SMS: return "UNSOL_NEW_CDMA_SMS";
        case RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS: return "UNSOL_NEW_BROADCAST_SMS";
        case RIL_UNSOL_CDMA_RUIM_SMS_STORAGE_FULL: return "UNSOL_CDMA_RUIM_SMS_STORAGE_FULL";
        case RIL_UNSOL_RESTRICTED_STATE_CHANGED: return "UNSOL_RESTRICTED_STATE_CHANGED";
        case RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE: return "UNSOL_ENTER_EMERGENCY_CALLBACK_MODE";
        case RIL_UNSOL_CDMA_CALL_WAITING: return "UNSOL_CDMA_CALL_WAITING";
        case RIL_UNSOL_CDMA_OTA_PROVISION_STATUS: return "UNSOL_CDMA_OTA_PROVISION_STATUS";
        case RIL_UNSOL_CDMA_INFO_REC: return "UNSOL_CDMA_INFO_REC";
        case RIL_UNSOL_OEM_HOOK_RAW: return "UNSOL_OEM_HOOK_RAW";
        case RIL_UNSOL_RINGBACK_TONE: return "UNSOL_RINGBACK_TONE";
        case RIL_UNSOL_RESEND_INCALL_MUTE: return "UNSOL_RESEND_INCALL_MUTE";
        case RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED: return "UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED";
        case RIL_UNSOL_CDMA_PRL_CHANGED: return "UNSOL_CDMA_PRL_CHANGED";
        case RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE: return "UNSOL_EXIT_EMERGENCY_CALLBACK_MODE";
        case RIL_UNSOL_RIL_CONNECTED: return "UNSOL_RIL_CONNECTED";
        case RIL_UNSOL_VOICE_RADIO_TECH_CHANGED: return "UNSOL_VOICE_RADIO_TECH_CHANGED";
        case RIL_UNSOL_CELL_INFO_LIST: return "UNSOL_CELL_INFO_LIST";
        case RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED: return "RESPONSE_IMS_NETWORK_STATE_CHANGED";
        case RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED: return "UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED";
        case RIL_UNSOL_SRVCC_STATE_NOTIFY: return "UNSOL_SRVCC_STATE_NOTIFY";
        case RIL_UNSOL_HARDWARE_CONFIG_CHANGED: return "HARDWARE_CONFIG_CHANGED";
        case RIL_UNSOL_DC_RT_INFO_CHANGED: return "UNSOL_DC_RT_INFO_CHANGED";
        case RIL_UNSOL_RADIO_CAPABILITY: return "UNSOL_RADIO_CAPABILITY";
        case RIL_RESPONSE_ACKNOWLEDGEMENT: return "RIL_RESPONSE_ACKNOWLEDGEMENT";
        case RIL_UNSOL_ON_SS: return "UNSOL_ON_SS";
        case RIL_UNSOL_STK_CC_ALPHA_NOTIFY: return "UNSOL_STK_CC_ALPHA_NOTIFY";
        case RIL_UNSOL_LCEDATA_RECV: return "UNSOL_LCEDATA_RECV";
        case RIL_UNSOL_PCO_DATA: return "UNSOL_PCO_DATA";
        case RIL_UNSOL_MODEM_RESTART: return "UNSOL_MODEM_RESTART";
        case RIL_UNSOL_CARRIER_INFO_IMSI_ENCRYPTION: return "UNSOL_CARRIER_INFO_IMSI_ENCRYPTION";
        case RIL_UNSOL_NETWORK_SCAN_RESULT: return "UNSOL_NETWORK_SCAN_RESULT";
        case RIL_UNSOL_KEEPALIVE_STATUS: return "UNSOL_KEEPALIVE_STATUS";
        case RIL_UNSOL_LINK_CAPACITY_ESTIMATE: return "UNSOL_LINK_CAPACITY_ESTIMATE";
        case RIL_UNSOL_PHYSICAL_CHANNEL_CONFIG: return "UNSOL_PHYSICAL_CHANNEL_CONFIG";
        case RIL_UNSOL_EMERGENCY_NUMBER_LIST: return "UNSOL_EMERGENCY_NUMBER_LIST";

        /* IMS unsolicited response @{ */
        case RIL_EXT_UNSOL_RESPONSE_IMS_CALL_STATE_CHANGED: return "UNSOL_IMS_CALL_STATE_CHANGED";
        case RIL_EXT_UNSOL_RESPONSE_VIDEO_QUALITY: return "UNSOL_VIDEO_QUALITY";
        case RIL_EXT_UNSOL_RESPONSE_IMS_BEARER_ESTABLISTED: return "UNSOL_RESPONSE_IMS_BEARER_ESTABLISTED";
        case RIL_EXT_UNSOL_IMS_HANDOVER_REQUEST: return "UNSOL_IMS_HANDOVER_REQUEST";
        case RIL_EXT_UNSOL_IMS_HANDOVER_STATUS_CHANGE: return "UNSOL_IMS_HANDOVER_STATUS_CHANGE";
        case RIL_EXT_UNSOL_IMS_NETWORK_INFO_CHANGE: return "UNSOL_IMS_NETWORK_INFO_CHANGE";
        case RIL_EXT_UNSOL_IMS_REGISTER_ADDRESS_CHANGE: return "UNSOL_IMS_REGISTER_ADDRESS_CHANGE";
        case RIL_EXT_UNSOL_IMS_WIFI_PARAM: return "UNSOL_IMS_WIFI_PARAM";
        case RIL_EXT_UNSOL_IMS_NETWORK_STATE_CHANGED: return "UNSOL_IMS_NETWORK_STATE_CHANGED";
        case RIL_EXT_UNSOL_UPDATE_HD_VOICE_STATE: return "UNSOL_UPDATE_HD_VOICE_STATE";
        case RIL_UNSOL_CONFIG_ICC_SLOT_STATUS: return "UNSOL_CONFIG_ICC_SLOT_STATUS";

        /* }@ */
        /* videophone @{ */
        case RIL_EXT_UNSOL_VIDEOPHONE_CODEC: return "UNSOL_VIDEOPHONE_CODEC";
        case RIL_EXT_UNSOL_VIDEOPHONE_DSCI: return "UNSOL_VIDEOPHONE_DSCI";
        case RIL_EXT_UNSOL_VIDEOPHONE_STRING: return "UNSOL_VIDEOPHONE_STRING";
        case RIL_EXT_UNSOL_VIDEOPHONE_REMOTE_MEDIA: return "UNSOL_VIDEOPHONE_REMOTE_MEDIA";
        case RIL_EXT_UNSOL_VIDEOPHONE_MM_RING: return "UNSOL_VIDEOPHONE_MM_RING";
        case RIL_EXT_UNSOL_VIDEOPHONE_RELEASING: return "UNSOL_VIDEOPHONE_RELEASING";
        case RIL_EXT_UNSOL_VIDEOPHONE_RECORD_VIDEO: return "UNSOL_VIDEOPHONE_RECORD_VIDEO";
        case RIL_EXT_UNSOL_VIDEOPHONE_MEDIA_START: return "UNSOL_VIDEOPHONE_MEDIA_START";
        /* }@ */
        case RIL_EXT_UNSOL_RAU_SUCCESS: return "UNSOL_RAU_SUCCESS";
        case RIL_EXT_UNSOL_CLEAR_CODE_FALLBACK: return "UNSOL_CLEAR_CODE_FALLBACK";
        case RIL_EXT_UNSOL_RIL_CONNECTED: return "UNSOL_RIL_CONNECTED";
        case RIL_EXT_UNSOL_SIMLOCK_SIM_EXPIRED: return "UNSOL_SIMLOCK_SIM_EXPIRED";
        case RIL_EXT_UNSOL_SIM_PS_REJECT: return "UNSOL_SIM_PS_REJECT";
        case RIL_EXT_UNSOL_EARLY_MEDIA: return "UNSOL_EARLY_MEDIA";
        case RIL_EXT_UNSOL_SPUCOPS_LIST: return "UNSOL_SPUCOPS_LIST";
        case RIL_ATC_UNSOL_VSIM_RSIM_REQ: return "UNSOL_VSIM_RSIM_REQ";
//        case RIL_EXT_UNSOL_SETUP_DATA_FOR_CP: return "UNSOL_SETUP_DATA_FOR_CP";
        case RIL_EXT_UNSOL_SUBSIDYLOCK_STATUS_CHANGED: return "UNSOL_SUBSIDYLOCK_STATUS_CHANGED";
        case RIL_EXT_UNSOL_IMS_CSFB_VENDOR_CAUSE: return "UNSOL_IMS_CSFB_VENDOR_CAUSE";
        case RIL_EXT_UNSOL_IMS_ERROR_CAUSE: return "UNSOL_IMS_ERROR_CAUSE";
        case RIL_EXT_UNSOL_CNAP: return "UNSOL_CNAP";
        case RIL_EXT_UNSOL_SIGNAL_CONN_STATUS: return "UNSOL_SIGNAL_CONN_STATUS";
        case RIL_EXT_UNSOL_SMART_NR_CHANGED: return "UNSOL_SMART_NR_CHANGED";
        case RIL_EXT_UNSOL_NR_CFG_INFO: return "UNSOL_NR_CFG_INFO";
        case RIL_EXT_UNSOL_MODEM_STATE_CHANGED: return "UNSOL_MODEM_STATE_CHANGED";
        default: return "<unknown request>";
    }
}

const char *
rilSocketIdToString(RIL_SOCKET_ID socket_id)
{
    switch(socket_id) {
        case RIL_SOCKET_1:
            return "RIL_SOCKET_1";
#if (SIM_COUNT >= 2)
        case RIL_SOCKET_2:
            return "RIL_SOCKET_2";
#endif
#if (SIM_COUNT >= 3)
        case RIL_SOCKET_3:
            return "RIL_SOCKET_3";
#endif
#if (SIM_COUNT >= 4)
        case RIL_SOCKET_4:
            return "RIL_SOCKET_4";
#endif
        default:
            return "not a valid RIL";
    }
}

void getProperty(RIL_SOCKET_ID socket_id, const char *property, char *value,
                   const char *defaultVal) {
    int simId = 0;
    char prop[PROPERTY_VALUE_MAX] = {0};
    int len = property_get(property, prop, "");
    char *p[RIL_SOCKET_NUM];
    char *buf = prop;
    char *ptr = NULL;
    RLOGD("get sim%d [%s] property: %s", socket_id, property, prop);

    if (value == NULL) {
        RLOGE("The memory to save prop is NULL!");
        return;
    }

    memset(p, 0, RIL_SOCKET_NUM * sizeof(char *));
    if (len > 0) {
        for (simId = 0; simId < RIL_SOCKET_NUM; simId++) {
            ptr = strsep(&buf, ",");
            p[simId] = ptr;
        }

        if (socket_id >= RIL_SOCKET_1 && socket_id < RIL_SOCKET_NUM &&
                (p[socket_id] != NULL) && strcmp(p[socket_id], "")) {
            memcpy(value, p[socket_id], strlen(p[socket_id]) + 1);
            return;
        }
    }

    if (defaultVal != NULL) {
        len = strlen(defaultVal);
        memcpy(value, defaultVal, len);
        value[len] = '\0';
    }
}

} /* namespace android */
