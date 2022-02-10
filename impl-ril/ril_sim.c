/**
 * ril_sim.c --- SIM-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_sim.h"
#include "ril_stk.h"
#include "ril_call.h"
#include "ril_network.h"
#include "custom/ril_custom.h"
#include "utils.h"

/* Property to save pin for modem assert */
#define SIM_PIN_PROP                            "vendor.ril.sim.pin"
#define MODEM_ASSERT_PROP                       "vendor.ril.modem.assert"
#define AUTO_SAVE_PIN                           "persist.vendor.radio.mdrec.simpin.cache"
#define FACILITY_LOCK_REQUEST                   "2"

#define TYPE_FCP                                0x62
#define COMMAND_GET_RESPONSE                    0xc0
#define TYPE_EF                                 4
#define RESPONSE_EF_SIZE                        15
#define TYPE_FILE_DES_LEN                       5
#define RESPONSE_DATA_FCP_FLAG                  0
#define RESPONSE_DATA_FILE_DES_FLAG             2
#define RESPONSE_DATA_FILE_DES_LEN_FLAG         3
#define RESPONSE_DATA_FILE_TYPE                 6
#define RESPONSE_DATA_FILE_SIZE_1               2
#define RESPONSE_DATA_FILE_SIZE_2               3
#define RESPONSE_DATA_STRUCTURE                 13
#define RESPONSE_DATA_RECORD_LENGTH             14
#define RESPONSE_DATA_FILE_RECORD_COUNT_FLAG    8
#define RESPONSE_DATA_FILE_RECORD_LEN_1         6
#define RESPONSE_DATA_FILE_RECORD_LEN_2         7
#define EF_TYPE_TRANSPARENT                     0x01
#define EF_TYPE_LINEAR_FIXED                    0x02
#define EF_TYPE_CYCLIC                          0x06
#define USIM_DATA_OFFSET_2                      2
#define USIM_DATA_OFFSET_3                      3
#define USIM_FILE_DES_TAG                       0x82
#define USIM_FILE_SIZE_TAG                      0x80
#define TYPE_CHAR_SIZE                          sizeof(char)
#define READ_BINERY                             0xb0
#define READ_RECORD                             0xb2
#define DF_ADF                                  "3F007FFF"
#define DF_GSM                                  "3F007F20"
#define DF_TELECOM                              "3F007F10"
#define MF_SIM                                  "3F00"
#define EFID_SST                                0x6f38
#define EFID_DIR                                0x2f00

// TS 101.220 V 15.0.0 AnnexE & Annex M
#define AID_TYPE_USIM                           "A0000000871002"
#define AID_TYPE_CSIM                           "A0000003431002"
#define AID_TYPE_ISIM                           "A0000000871004"

// According to TS 102.221 AID value max size is 0x10
#define MAX_AID_LENGTH                          32

#define SIM_DROP                                1
#define SIM_REMOVE                              2

#define AUTH_CONTEXT_EAP_SIM                    128
#define AUTH_CONTEXT_EAP_AKA                    129
#define SIM_AUTH_RESPONSE_SUCCESS               0
#define SIM_AUTH_RESPONSE_SYNC_FAILURE          3

#define REQUEST_SIMLOCK_WHITE_LIST_PS           1
#define REQUEST_SIMLOCK_WHITE_LIST_PN           2
#define REQUEST_SIMLOCK_WHITE_LIST_PU           3
#define REQUEST_SIMLOCK_WHITE_LIST_PP           4
#define REQUEST_SIMLOCK_WHITE_LIST_PC           5
#define WHITE_LIST_HEAD_LENGTH                  5
#define WHITE_LIST_PS_PART_LENGTH               (19 + 1)
#define WHITE_LIST_COLUMN                       17
#define IMSI_VAL_NUM                            8
#define IMSI_TOTAL_LEN                          (16 + 1)
#define SMALL_IMSI_LEN                          (2 + 1)
#define SIMLOCK_ATTEMPT_TIMES_PROP              "vendor.sim.attempttimes.%s"

#define IMEI_LEN                                15
#define AIDS_COUNT                              3

extern int s_modemConfig;

static pthread_mutex_t s_remainTimesMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_simPresentMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_presentSIMCountMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_getAidsMutex[SIM_COUNT] = {
        PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 2)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 3)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 4)
       ,PTHREAD_MUTEX_INITIALIZER
#endif
#endif
#endif
};

RIL_AppType s_appType[SIM_COUNT];
static bool s_needQueryPinTimes[SIM_COUNT] = {
        true
#if (SIM_COUNT >= 2)
        ,true
#if (SIM_COUNT >= 3)
        ,true
#if (SIM_COUNT >= 4)
        ,true
#endif
#endif
#endif
        };
static bool s_needQueryPukTimes[SIM_COUNT] = {
        true
#if (SIM_COUNT >= 2)
        ,true
#if (SIM_COUNT >= 3)
        ,true
#if (SIM_COUNT >= 4)
        ,true
#endif
#endif
#endif
        };
static bool s_needQueryPinPuk2Times[SIM_COUNT] = {
        true
#if (SIM_COUNT >= 2)
        ,true
#if (SIM_COUNT >= 3)
        ,true
#if (SIM_COUNT >= 4)
        ,true
#endif
#endif
#endif
        };
int s_imsInitISIM[SIM_COUNT] = {
        -1
#if (SIM_COUNT >= 2)
       ,-1
#if (SIM_COUNT >= 3)
       ,-1
#if (SIM_COUNT >= 4)
       ,-1
#endif
#endif
#endif
        };

const char *base64char =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int s_simSessionId[SIM_COUNT] = {
        -1
#if (SIM_COUNT >= 2)
       ,-1
#if (SIM_COUNT >= 3)
       ,-1
#if (SIM_COUNT >= 4)
       ,-1
#endif
#endif
#endif
};

static pthread_mutex_t s_simStatusMutex[SIM_COUNT] = {
        PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 2)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 3)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 4)
       ,PTHREAD_MUTEX_INITIALIZER
#endif
#endif
#endif
};

/* the index 0 is to save *usim_aid
 * the index 1 is to save *csim_aid
 * the index 2 is to save *isim_aid
 */
static char s_aidsForSIM[SIM_COUNT][AIDS_COUNT][MAX_AID_LENGTH + 1];
static char s_imsi[SIM_COUNT][IMSI_TOTAL_LEN];

pthread_mutex_t s_CglaCrsmMutex[SIM_COUNT] = {
        PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 2)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 3)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 4)
       ,PTHREAD_MUTEX_INITIALIZER
#endif
#endif
#endif
};

pthread_mutex_t s_SPCCHOMutex[SIM_COUNT] = {  // bug1071409
        PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 2)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 3)
       ,PTHREAD_MUTEX_INITIALIZER
#if (SIM_COUNT >= 4)
       ,PTHREAD_MUTEX_INITIALIZER
#endif
#endif
#endif
};

static int queryFDNServiceAvailable(RIL_SOCKET_ID socket_id);
int initISIM(RIL_SOCKET_ID socket_id);
int readSimRecord(RIL_SOCKET_ID socket_id, RIL_SIM_IO_v6 *data, RIL_SIM_IO_Response *sr);
void onIccSlotStatus(RIL_Token t);
extern void saveDataCardProp(RIL_SOCKET_ID socket_id);

static bool needCacheSimPin() {
    bool ret = false;
    char prop[PROPERTY_VALUE_MAX] = { 0 };
    property_get(AUTO_SAVE_PIN, prop, "0");
    if (0 == strcmp(prop, "1")) {
        ret = true;
    }
    return ret;
}

void onModemReset_Sim() {
    RIL_SOCKET_ID socket_id  = 0;
    s_presentSIMCount = 0;

    for (socket_id = RIL_SOCKET_1; socket_id < RIL_SOCKET_NUM; socket_id++) {
        s_isSimPresent[socket_id] = 0;
        s_imsInitISIM[socket_id] = -1;
        s_simSessionId[socket_id] = -1;
        s_appType[socket_id] = 0;

        if (s_simBusy[socket_id].s_sim_busy) {
            pthread_mutex_lock(&s_simBusy[socket_id].s_sim_busy_mutex);
            s_simBusy[socket_id].s_sim_busy = false;
            pthread_cond_signal(&s_simBusy[socket_id].s_sim_busy_cond);
            pthread_mutex_unlock(&s_simBusy[socket_id].s_sim_busy_mutex);
        }
    }
}

static int getSimlockRemainTimes(RIL_SOCKET_ID socket_id, SimUnlockType type) {
    int err, result;
    int remaintime = 3;
    char cmd[AT_COMMAND_LEN] = {0};
    char *line;
    ATResponse *p_response = NULL;

    if (UNLOCK_PUK == type || UNLOCK_PUK2 == type) {
        remaintime = 10;
    }

    snprintf(cmd, sizeof(cmd), "AT+XX=%d", type);
    err = at_send_command_singleline(socket_id, cmd, "+XX:",
                                     &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGD("getSimlockRemainTimes: +XX response error !");
    } else {
        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err == 0) {
            err = at_tok_nextint(&line, &result);
            if (err == 0) {
                remaintime = result;
            }
        }
    }
    at_response_free(p_response);

    /* Bug 523208 set pin/puk remain times to prop. @{ */
    pthread_mutex_lock(&s_remainTimesMutex);
    setPinPukRemainTimes(type, remaintime, socket_id);
    pthread_mutex_unlock(&s_remainTimesMutex);
    /* }@ */
    return remaintime;
}

static void getSIMStatusAgainForSimBusy(void *param) {
    ATResponse *p_response = NULL;
    int err;

    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    if (s_radioState[socket_id] == RADIO_STATE_UNAVAILABLE) {
        goto done;
    }
    err = at_send_command_singleline(socket_id,
            "AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        goto done;
    }
    switch (at_get_cme_error(p_response)) {
        case CME_SIM_BUSY:
            RIL_requestTimedCallback(getSIMStatusAgainForSimBusy,
                    (void *)&s_socketId[socket_id], &TIMEVAL_SIMPOLL);
            goto done;
        default:
            if (s_simBusy[socket_id].s_sim_busy) {
                pthread_mutex_lock(&s_simBusy[socket_id].s_sim_busy_mutex);
                s_simBusy[socket_id].s_sim_busy = false;
                pthread_cond_signal(&s_simBusy[socket_id].s_sim_busy_cond);
                pthread_mutex_unlock(&s_simBusy[socket_id].s_sim_busy_mutex);
            }
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                    NULL, 0, socket_id);
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED,
                    NULL, 0, socket_id);
            goto done;
    }
done:
    at_response_free(p_response);
    return;
}

void encryptPin(int len, char *pin, unsigned char encryptPin[17]) {
    int encryptArray[10][10] = {
            {'r', 's', 'e', 'f', 'a', 'q', 't', 'd', 'w', 'c'},
            {'1', '6', '2', '3', '0', '5', '4', '8', '9', '7'},
            {'z', 's', 'e', 't', 'g', 'j', 'k', 'n', 'p', 'c'},
            {'a', 'u', 'e', 'n', 'z', 'k', 'd', 'm', 'r', 'c'},
            {'u', 't', 'e', 's', 'd', 'g', 'k', 'b', 'c', 'z'},
            {'e', 'r', 'i', 'f', 'd', 'j', 'l', 'm', 'c', 'x'},
            {'5', '2', '7', '8', '4', '1', '3', '6', '0', '9'},
            {'z', 's', 'e', 't', 'g', 'j', 'k', 'n', 'p', 'c'},
            {'1', '6', '2', '3', '0', '5', '4', '8', '9', '7'},
            {'s', 'd', 'f', 'z', 'w', 'e', 't', 'j', 'l', 'c'}};

    int randEncryptArray[10] =
            {'p', 'a', 'r', 'k', 'y', 'o', 'u', 'n', 'g', 'j'};

    int lenData = len;
    int i = 0;
    int offset = 0;

    encryptPin[offset] = randEncryptArray[lenData];
    offset++;

    srand((unsigned int)time(0));

    for (i = 0; i < 8; i++) {
        int code = 0;
        int randVal = rand() % 10;
        encryptPin[offset] = randEncryptArray[randVal];
        offset++;

        if (i < lenData) {
            code = pin[i] - 0x30;
        } else {
            code = rand() % 10;
        }
        encryptPin[offset] = encryptArray[randVal][code];
        offset++;
    }
    return;
}

void decryptPin(char *pin, unsigned char encryptedPin[17]) {
    int encryptArray[10][10] = {
            {'r', 's', 'e', 'f', 'a', 'q', 't', 'd', 'w', 'c'},
            {'1', '6', '2', '3', '0', '5', '4', '8', '9', '7'},
            {'z', 's', 'e', 't', 'g', 'j', 'k', 'n', 'p', 'c'},
            {'a', 'u', 'e', 'n', 'z', 'k', 'd', 'm', 'r', 'c'},
            {'u', 't', 'e', 's', 'd', 'g', 'k', 'b', 'c', 'z'},
            {'e', 'r', 'i', 'f', 'd', 'j', 'l', 'm', 'c', 'x'},
            {'5', '2', '7', '8', '4', '1', '3', '6', '0', '9'},
            {'z', 's', 'e', 't', 'g', 'j', 'k', 'n', 'p', 'c'},
            {'1', '6', '2', '3', '0', '5', '4', '8', '9', '7'},
            {'s', 'd', 'f', 'z', 'w', 'e', 't', 'j', 'l', 'c'}};

    int randEncryptArray[10] =
            {'p', 'a', 'r', 'k', 'y', 'o', 'u', 'n', 'g', 'j'};

    int i = 0;
    int j = 0;
    int offset = 0;
    int pinLen = -1;
    for (i = 0; i < 10; i++) {
        if (randEncryptArray[i] == encryptedPin[offset]) {
            pinLen = i;
        }
    }
    if (pinLen == -1) {
        RLOGD("Cant find SR Len");
        return;
    }
    offset++;
    for (i = 0; i < pinLen; i++) {
        int randVal = -1;
        for (j = 0; j < 10; j++) {
            if (randEncryptArray[j] == encryptedPin[offset]) {
                randVal = j;
            }
        }
        if (randVal == -1) {
            RLOGD("Cant find Val");
            return;
        }
        offset++;
        for (j = 0; j < 10; j++) {
            if (encryptArray[randVal][j] == encryptedPin[offset]) {
                pin[i] = j + 0x30;
                offset++;
                break;
            }
        }
        if (j == 10) {
            RLOGD("Cant find the Code");
            return;
        }
    }

    return;
}

void setSimPresent(RIL_SOCKET_ID socket_id, int hasSim) {
    RLOGD("setSimPresent hasSim = %d", hasSim);
    int oldSimState = s_isSimPresent[socket_id];
    pthread_mutex_lock(&s_simPresentMutex);
    s_isSimPresent[socket_id] = hasSim;
    pthread_mutex_unlock(&s_simPresentMutex);
    if (oldSimState != hasSim) {
        RIL_requestTimedCallback(onIccSlotStatus, NULL, NULL);
        pthread_mutex_lock(&s_presentSIMCountMutex);
        hasSim ? ++s_presentSIMCount : --s_presentSIMCount;
        pthread_mutex_unlock(&s_presentSIMCountMutex);
        // update ecc list for all sim when sim state changed
        RIL_requestTimedCallback(sendUnsolEccList,
                                (void *)&s_socketId[socket_id], NULL);
#if (SIM_COUNT == 2)
        RIL_requestTimedCallback(sendUnsolEccList,
                                (void *)&s_socketId[1 - socket_id], NULL);
#endif
        RLOGD("update ecc list for all sim, sendUnsolEccList");
    }
}

int isSimPresent(RIL_SOCKET_ID socket_id) {
    int hasSim = 0;
    pthread_mutex_lock(&s_simPresentMutex);
    hasSim = s_isSimPresent[socket_id];
    pthread_mutex_unlock(&s_simPresentMutex);

    return hasSim;
}

void initSIMPresentState() {
    int simId = 0;
    for (simId = 0; simId < SIM_COUNT; simId++) {
        if (s_isSimPresent[simId] == SIM_UNKNOWN) {
            RLOGD("s_isSimPresent unknown  %d", simId);
            getSIMStatus(false, simId);
        }
    }
    pthread_mutex_lock(&s_presentSIMCountMutex);
    s_presentSIMCount = 0;
    for (simId = 0; simId < SIM_COUNT; simId++) {
        if (isSimPresent(simId) == 1) {
            ++s_presentSIMCount;
        }
    }
    pthread_mutex_unlock(&s_presentSIMCountMutex);
}

/* Returns SIM_NOT_READY on error */
SimStatus getSIMStatus(int request, RIL_SOCKET_ID socket_id) {
    ATResponse *p_response = NULL;
    int err = -1;
    int ret = SIM_NOT_READY;
    char *cpinLine = NULL;
    char *cpinResult = NULL;

    pthread_mutex_lock(&s_simStatusMutex[socket_id]);

    err = at_send_command_singleline(socket_id, "AT+CPIN?",
                                     "+CPIN:", &p_response);
    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;
        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;
        case CME_SIM_BUSY:
            ret = SIM_ABSENT;
            if (!s_simBusy[socket_id].s_sim_busy) {
                s_simBusy[socket_id].s_sim_busy = true;
                RIL_requestTimedCallback(getSIMStatusAgainForSimBusy,
                    (void *)&s_socketId[socket_id], &TIMEVAL_SIMPOLL);
            }
            goto done;
        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */
    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start(&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp(cpinResult, "SIM PIN")) {
        if (needCacheSimPin()) {
            char modemAssertProp[PROPERTY_VALUE_MAX];

            getProperty(socket_id, MODEM_ASSERT_PROP, modemAssertProp, "0");
            if (strcmp(modemAssertProp, "1") == 0) {
                setProperty(socket_id, MODEM_ASSERT_PROP, "0");

                char cmd[AT_COMMAND_LEN];
                char pin[PROPERTY_VALUE_MAX];
                char encryptedPin[PROPERTY_VALUE_MAX];
                ATResponse *p_resp = NULL;

                memset(pin, 0, sizeof(pin));
                getProperty(socket_id, SIM_PIN_PROP, encryptedPin, "");
                decryptPin(pin, (unsigned char *) encryptedPin);

                if (strlen(pin) != 4) {
                    goto out;
                } else {
                    snprintf(cmd, sizeof(cmd), "AT+CPIN=%s", pin);
                    err = at_send_command(socket_id, cmd, &p_resp);
                    if (err < 0 || p_resp->success == 0) {
                        at_response_free(p_resp);
                        goto out;
                    }
                    at_response_free(p_resp);
                    ret = SIM_NOT_READY;
                    goto done;
                }
            }
        }
out:
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp(cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NET PIN")) {
        ret = SIM_NETWORK_PERSONALIZATION;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NETSUB PIN")) {
        ret = SIM_NETWORK_SUBSET_PERSONALIZATION;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-SP PIN")) {
        ret = SIM_SERVICE_PROVIDER_PERSONALIZATION;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-CORP PIN")) {
        ret = SIM_CORPORATE_PERSONALIZATION;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-SIM PIN")) {
        ret = SIM_SIM_PERSONALIZATION;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NET PUK")) {
        ret = SIM_NETWORK_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NETSUB PUK")) {
        ret = SIM_NETWORK_SUBSET_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-SP PUK")) {
        ret = SIM_SERVICE_PROVIDER_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-CORP PUK")) {
        ret = SIM_CORPORATE_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-SIM PUK")) {
        ret = SIM_SIM_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-INTEGRITY FAIL")) {
        ret = SIM_SIMLOCK_FOREVER;
        goto done;
    } else if (0 == strcmp(cpinResult, "PIN1_BLOCK_PUK1_BLOCK")) {
        ret = SIM_PERM_BLOCK;
        goto done;
    } else if (0 != strcmp(cpinResult, "READY")) {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    ret = SIM_READY;

done:
    at_response_free(p_response);
    if (ret != SIM_ABSENT) {
        char simEnabledProp[PROPERTY_VALUE_MAX] = {0};
        getProperty(socket_id, SIM_ENABLED_PROP, simEnabledProp, "1");
        if (request != RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS &&
                strcmp(simEnabledProp, "0") == 0) {
            ret = SIM_ABSENT;
            RLOGD("freeSimEcclist");
            freeSimEcclist(socket_id);
            RIL_requestTimedCallback(sendUnsolEccList, (void *)&s_socketId[socket_id],
                    NULL);
        }
    }

    if (ret == SIM_ABSENT) {
        /* Bug 1260211  when sim busy, ril doesn't set s_isSimPresent[socket_id] to SIM_UNKNOWN. @{ */
        if (s_simBusy[socket_id].s_sim_busy == true) {
            setSimPresent(socket_id, SIM_UNKNOWN);
        } else {
            setSimPresent(socket_id, ABSENT);
        }
        /* }@ */
    } else {
        setSimPresent(socket_id, PRESENT);
    }

    /* Bug 523208 set pin/puk remain times to prop. @{ */
    if ((s_needQueryPinTimes[socket_id] && (ret == SIM_PIN || ret == SIM_READY))
            || (s_needQueryPukTimes[socket_id] && (ret == SIM_PUK || ret == SIM_PERM_BLOCK))) {
        if (ret == SIM_PIN || ret == SIM_READY) {
            s_needQueryPinTimes[socket_id] = false;
        } else {
            s_needQueryPukTimes[socket_id] = false;
        }
        getSimlockRemainTimes(socket_id, (ret == SIM_PUK || ret == SIM_PERM_BLOCK) ?
                UNLOCK_PUK : UNLOCK_PIN);
    } else if (s_needQueryPinPuk2Times[socket_id] && ret == SIM_READY) {
        s_needQueryPinPuk2Times[socket_id] = false;
        getSimlockRemainTimes(socket_id, UNLOCK_PIN2);
        getSimlockRemainTimes(socket_id, UNLOCK_PUK2);
    } else if (ret == SIM_ABSENT) {
        s_needQueryPinTimes[socket_id] = true;
        s_needQueryPukTimes[socket_id] = true;
        s_needQueryPinPuk2Times[socket_id] = true;
        s_imsInitISIM[socket_id] = -1;
        s_simSessionId[socket_id] = -1;
    }
    /* }@ */
    pthread_mutex_unlock(&s_simStatusMutex[socket_id]);

    return ret;
}

void getSimAtr(RIL_SOCKET_ID socket_id, char *atr, int size) {
    ATResponse *p_response = NULL;
    char *line = NULL, *atrTemp = NULL;
    int err = 0;

    if (atr == NULL) {
        RLOGE("atr buffer is null");
        goto error;
    }
    err = at_send_command_singleline(socket_id, "AT+SPATR?",
                                     "+SPATR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    if (at_tok_start(&line) < 0) {
        goto error;
    }
    if (at_tok_nextstr(&line, &atrTemp) < 0) {
        goto error;
    }
    snprintf(atr, size, "%s", atrTemp);

error:
    at_response_free(p_response);
}

void getIccId(RIL_SOCKET_ID socket_id, char *iccid, int size) {
    int err = 0;
    char *line = NULL;
    char *response = NULL;
    ATResponse *p_response = NULL;

    if (iccid == NULL) {
        RLOGE("iccid buffer is null");
        return;
    }
    err = at_send_command_singleline(socket_id, "AT+CCID?",
                                     "+CCID: ", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &response);
    if (err < 0) goto error;

    snprintf(iccid, size, "%s", response);
error:
    at_response_free(p_response);
}

RIL_AppType getSimType(RIL_SOCKET_ID socket_id) {
    int err, skip;
    int cardType;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_AppType ret = RIL_APPTYPE_UNKNOWN;

    err = at_send_command_singleline(socket_id, "AT+EUICC?",
                                     "+EUICC:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &cardType);
    if (err < 0) goto error;

    if (cardType == 1) {
        ret = RIL_APPTYPE_USIM;
    } else if (cardType == 0) {
        ret = RIL_APPTYPE_SIM;
    } else {
        ret = RIL_APPTYPE_UNKNOWN;
    }

    at_response_free(p_response);
    return ret;

error:
    at_response_free(p_response);
    return RIL_APPTYPE_UNKNOWN;
}

/**
 * UNISOC Add for C2K
 * Get multiple APP card type
 */
RIL_AppType* getSimTypesMultiApp(RIL_SOCKET_ID socket_id, int *num_apps) {
    int err, skip;
    int type_2g_3G; // 0: 2G card, 1: 3G card
    int type_3gpp_3gpp2; // 0: 3GPP only, 1: 3GPP2 only, 2: 3GPP and 3GPP2
    char *line;
    ATResponse *p_response = NULL;
    RIL_AppType *ret = NULL;
    *num_apps = 1;

    err = at_send_command_singleline(socket_id, "AT+EUICC?",
                                     "+EUICC:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &type_2g_3G);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &type_3gpp_3gpp2);
    if (err < 0) goto error;

    /**
     * s_appType is used by some places to check USIM
     */
    if (type_2g_3G == 1) {
        s_appType[socket_id] = RIL_APPTYPE_USIM;
    } else if (type_2g_3G == 0) {
        s_appType[socket_id] = RIL_APPTYPE_SIM;
    } else {
        s_appType[socket_id] = RIL_APPTYPE_UNKNOWN;
    }

    /**
     * AT+EUICC?
       +EUICC: 0,0,0,0 //SIM
       +EUICC: 0,0,0,1 //UIM
       +EUICC: 0,0,0,2 //SIM+UIM
       +EUICC: 0,0,1,0 //USIM
       +EUICC: 0,0,1,1 //CSIM
       +EUICC: 0,0,1,2 //USIM+CSIM
     */

    if (type_2g_3G == 0 && type_3gpp_3gpp2 == 0) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType));
        ret[0] = RIL_APPTYPE_SIM;
    } else if (type_2g_3G == 0 && type_3gpp_3gpp2 == 1) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType));
        ret[0] = RIL_APPTYPE_RUIM;
    }  else if (type_2g_3G == 0 && type_3gpp_3gpp2 == 2) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType) * 2);
        ret[0] = RIL_APPTYPE_SIM;
        ret[1] = RIL_APPTYPE_RUIM;
        *num_apps = 2;
    } else if (type_2g_3G == 1 && type_3gpp_3gpp2 == 0) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType));
        ret[0] = RIL_APPTYPE_USIM;
    } else if (type_2g_3G == 1 && type_3gpp_3gpp2 == 1) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType));
        ret[0] = RIL_APPTYPE_CSIM;
    } else if (type_2g_3G == 1 && type_3gpp_3gpp2 == 2) {
        ret = (RIL_AppType *) malloc(sizeof(RIL_AppType) * 2);
        ret[0] = RIL_APPTYPE_USIM;
        ret[1] = RIL_APPTYPE_CSIM;
        *num_apps = 2;
    } else {
        goto error;
    }
    at_response_free(p_response);
    return ret;

error:
    ret = (RIL_AppType *) malloc(sizeof(RIL_AppType));
    ret[0] = RIL_APPTYPE_UNKNOWN;
    at_response_free(p_response);
    return ret;
}

/*
 * UNISOC Add for C2K
 * Currently, this function only supports usim/csim/isim maximum three ADFs.
 *
 * if the imsi is same with s_imsi, we get three aids from s_aidsForSIM,
 * or, we get three aids from sim card and save them into s_aidsForSIM and
 * update imsi into s_imsi.
 */
static void getAidForMultiApp(RIL_SOCKET_ID socket_id, char **usim_aid, char **csim_aid, char **isim_aid) {
    ATResponse *p_response = NULL;
    int err = -1;
    int numberRecords = 0;
    int recordSize = 0;
    char *aidData = NULL;
    char *aid = NULL;
    char *line = NULL;
    char aid_length_str[3] = {0};
    int aid_length = 0;

    pthread_mutex_lock(&s_getAidsMutex[socket_id]);
    err = at_send_command_numeric(socket_id, "AT+CIMI",
                                          &p_response);

    if (err >= 0 && p_response->success != 0) {
        if (!strcmp(s_imsi[socket_id], p_response->p_intermediates->line)) {
            //if the imsi is not changed, get aids from pre-save data
            RLOGD("getAidForMultiApp: get aid from pre-save data!");
            goto EXIT;
        }

        //if the imsi is changed, save the new imsi and get aids from sim card
        snprintf(s_imsi[socket_id], IMSI_TOTAL_LEN, "%s", p_response->p_intermediates->line);
    }

    AT_RESPONSE_FREE(p_response);
    RLOGD("getAidForMultiApp: get aid from sim card!");

    memset(s_aidsForSIM[socket_id][0], 0, sizeof(s_aidsForSIM[socket_id][0]));
    memset(s_aidsForSIM[socket_id][1], 0, sizeof(s_aidsForSIM[socket_id][1]));
    memset(s_aidsForSIM[socket_id][2], 0, sizeof(s_aidsForSIM[socket_id][2]));

    err = at_send_command_singleline(socket_id, "AT+SPCARDINFO=0,0,12",
                                          "+SPCARDINFO", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto EXIT;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto EXIT;

    err = at_tok_nextint(&line, &recordSize);
    if (err < 0) goto EXIT;

    err = at_tok_nextint(&line, &numberRecords);
    if (err < 0) goto EXIT;

    err = at_tok_nextstr(&line, &aidData);
    if (err < 0) goto EXIT;

    RLOGD("getAidForMultiApp: recordSize = %d, numberRecords = %d, aidData = %s", recordSize, numberRecords, aidData);

    // According to TS 102.221 AID value max size is 0x10.
    aid = (char*)calloc(MAX_AID_LENGTH + 1, sizeof(char));

    for (int numRec = 0; numRec < numberRecords; numRec++) {
        char record[128] = {0};
        strncpy(record, aidData + (recordSize / numberRecords) * numRec * 2, 2 * recordSize / numberRecords);
        // The 4th byte is AID length
        strncpy(aid_length_str, record + 6, 2);

        // Convert HexStr to Integer
        aid_length = strtol(aid_length_str, NULL, 16);
        // skip template tag and aid tag, such as "61184F10".
        strncpy(aid, record + 8, (aid_length >= 1 && aid_length <= 16)
                ? (aid_length * 2) : MAX_AID_LENGTH);

        if (strStartsWith(aid, AID_TYPE_USIM)) {
            strncpy(s_aidsForSIM[socket_id][0], aid, strlen(aid) + 1);
        } else if (strStartsWith(aid, AID_TYPE_CSIM)) {
            strncpy(s_aidsForSIM[socket_id][1], aid, strlen(aid) + 1);
        } else if (strStartsWith(aid, AID_TYPE_ISIM)) {
            strncpy(s_aidsForSIM[socket_id][2], aid, strlen(aid) + 1);
        }

        memset(aid, 0, MAX_AID_LENGTH + 1);
    }

    free(aid);
    aid = NULL;

EXIT:
    *usim_aid = s_aidsForSIM[socket_id][0];
    *csim_aid = s_aidsForSIM[socket_id][1];
    *isim_aid = s_aidsForSIM[socket_id][2];
    AT_RESPONSE_FREE(p_response);
    pthread_mutex_unlock(&s_getAidsMutex[socket_id]);
}

/**
 * UNISOC Add for C2K
 * return values are defined in AT+SPCRSM
 * See ril_sim.h enum SpcrsmType
 */
static int getAppTypeByAidForSPCRSM(char * aid) {
    if (aid == NULL || aid[0] == '\0') {
        return SPCRSM_APPTYPE_UNKNOWN;
    }

    if (strStartsWith(aid, AID_TYPE_USIM)) {
        return SPCRSM_APPTYPE_USIM;
    } else if (strStartsWith(aid, AID_TYPE_ISIM)) {
        return SPCRSM_APPTYPE_ISIM;
    } else if (strStartsWith(aid, AID_TYPE_CSIM)) {
        return SPCRSM_APPTYPE_CSIM;
    }

    return 0;
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(int request, RIL_SOCKET_ID socket_id,
                         RIL_CardStatus_v1_4 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        {RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_NOT_READY = 1
        {RIL_APPTYPE_USIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_READY = 2
        {RIL_APPTYPE_USIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_PIN = 3
        {RIL_APPTYPE_USIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_PUK = 4
        {RIL_APPTYPE_USIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN},
        // SIM_NETWORK_PERSONALIZATION = 5
        {RIL_APPTYPE_USIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // RUIM_ABSENT = 6
        {RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_NOT_READY = 7
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_READY = 8
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_PIN = 9
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // RUIM_PUK = 10
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN},
        // RUIM_NETWORK_PERSONALIZATION = 11
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_NETWORK_SUBSET_PERSONALIZATION = EXT_SIM_STATUS_BASE + 1
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_SERVICE_PROVIDER_PERSONALIZATION = EXT_SIM_STATUS_BASE + 2
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_CORPORATE_PERSONALIZATION = EXT_SIM_STATUS_BASE + 3
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_CORPORATE, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_SIM_PERSONALIZATION = EXT_SIM_STATUS_BASE + 4
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SIM, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_NETWORK_PUK = EXT_SIM_STATUS_BASE + 5
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK = EXT_SIM_STATUS_BASE + 6
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK = EXT_SIM_STATUS_BASE + 7
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_CORPORATE_PUK = EXT_SIM_STATUS_BASE + 8
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_SIM_PUK = EXT_SIM_STATUS_BASE + 9
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SIM_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIMLOCK_FOREVER = EXT_SIM_STATUS_BASE + 10
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIMLOCK_FOREVER, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
         // SIM_PERM_BLOCK = EXT_SIM_STATUS_BASE + 11
         { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK,
         RIL_PERSOSUBSTATE_UNKNOWN, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_PERM_BLOCKED, RIL_PINSTATE_UNKNOWN }
    };
    static RIL_AppStatus ims_app_status_array[] = {
        {RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN}
    };

    RIL_CardState card_state;
    int num_apps;
    int sim_status;
    char simEnabledProp[PROPERTY_VALUE_MAX] = {0};

    getProperty(socket_id, SIM_ENABLED_PROP, simEnabledProp, "1");
    if (request != RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS &&
            strcmp(simEnabledProp, "0") == 0) {
        sim_status = SIM_ABSENT;
    } else {
        sim_status = getSIMStatus(request, socket_id);
    }

    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    /* Allocate and initialize base card status. */
    RIL_CardStatus_v1_4 *p_card_status = calloc(1, sizeof(RIL_CardStatus_v1_4));
    p_card_status->base.base.card_state = card_state;
    p_card_status->base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->base.base.gsm_umts_subscription_app_index = -1;
    p_card_status->base.base.cdma_subscription_app_index = -1;
    p_card_status->base.base.ims_subscription_app_index = -1;
    p_card_status->base.base.num_applications = num_apps;
    p_card_status->base.physicalSlotId = socket_id;
    p_card_status->base.atr = NULL;
    p_card_status->base.iccid = NULL;
    p_card_status->eid = "";
    if (sim_status != SIM_ABSENT) {
        // To support EUICC, atr must be reported.
        // See: https://source.android.com/devices/tech/connect/esim-modem-requirements
        p_card_status->base.atr = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getSimAtr(socket_id, p_card_status->base.atr, AT_COMMAND_LEN);
        p_card_status->base.iccid = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getIccId(socket_id, p_card_status->base.iccid, AT_COMMAND_LEN);
    }
    RLOGD("iccid :%s",p_card_status->base.iccid);
    s_appType[socket_id] = getSimType(socket_id);

    int isimResp = 0;
    if (sim_status == SIM_READY && s_appType[socket_id] == RIL_APPTYPE_USIM) {
        isimResp = initISIM(socket_id);
        RLOGD("app type %d", isimResp);
    }

    /* Initialize application status */
    unsigned int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->base.base.applications[i] = app_status_array[SIM_ABSENT];
    }

    for (i = 0; i < sizeof(app_status_array) / sizeof(RIL_AppStatus); i++) {
        app_status_array[i].app_type = s_appType[socket_id];
    }
    /* Pickup the appropriate application status
     * that reflects sim_status for gsm.
     */
    if (num_apps != 0) {
        if (isimResp != 1)  {
            /* Only support one app, gsm */
            p_card_status->base.base.num_applications = 1;
            p_card_status->base.base.gsm_umts_subscription_app_index = 0;

            /* Get the correct app status */
            p_card_status->base.base.applications[0] = app_status_array[sim_status];
        } else {
            p_card_status->base.base.num_applications = 2;
            p_card_status->base.base.gsm_umts_subscription_app_index = 0;
            p_card_status->base.base.ims_subscription_app_index = 1;
            p_card_status->base.base.applications[0] = app_status_array[sim_status];
            p_card_status->base.base.applications[1] = ims_app_status_array[0];
        }
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * UNISOC Add for C2K
 */

static int getCardStatusC2K(int request, RIL_SOCKET_ID socket_id,
                            RIL_CardStatus_v1_4 **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        {RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_NOT_READY = 1
        {RIL_APPTYPE_USIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_READY = 2
        {RIL_APPTYPE_USIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // SIM_PIN = 3
        {RIL_APPTYPE_USIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_PUK = 4
        {RIL_APPTYPE_USIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN},
        // SIM_NETWORK_PERSONALIZATION = 5
        {RIL_APPTYPE_USIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // RUIM_ABSENT = 6
        {RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_NOT_READY = 7
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_READY = 8
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN},
        // RUIM_PIN = 9
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // RUIM_PUK = 10
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN},
        // RUIM_NETWORK_PERSONALIZATION = 11
        {RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
         NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_NETWORK_SUBSET_PERSONALIZATION = EXT_SIM_STATUS_BASE + 1
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_SERVICE_PROVIDER_PERSONALIZATION = EXT_SIM_STATUS_BASE + 2
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_CORPORATE_PERSONALIZATION = EXT_SIM_STATUS_BASE + 3
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_CORPORATE, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // SIM_SIM_PERSONALIZATION = EXT_SIM_STATUS_BASE + 4
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SIM, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_NETWORK_PUK = EXT_SIM_STATUS_BASE + 5
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK = EXT_SIM_STATUS_BASE + 6
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_NETWORK_SUBSET_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK = EXT_SIM_STATUS_BASE + 7
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SERVICE_PROVIDER_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_CORPORATE_PUK = EXT_SIM_STATUS_BASE + 8
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_CORPORATE_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIM_SIM_PUK = EXT_SIM_STATUS_BASE + 9
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIM_SIM_PUK, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
        // PERSOSUBSTATE_SIMLOCK_FOREVER = EXT_SIM_STATUS_BASE + 10
        {RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO,
         RIL_PERSOSUBSTATE_SIMLOCK_FOREVER, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN},
         // SIM_PERM_BLOCK = EXT_SIM_STATUS_BASE + 11
         { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK,
         RIL_PERSOSUBSTATE_UNKNOWN, NULL, NULL, 0,
         RIL_PINSTATE_ENABLED_PERM_BLOCKED, RIL_PINSTATE_UNKNOWN }
    };
    static RIL_AppStatus ims_app_status_array[] = {
        {RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
         NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN}
    };
    RIL_CardState card_state;
    RIL_AppType* app_types = NULL;
    int num_apps;
    int sim_status;
    char simEnabledProp[PROPERTY_VALUE_MAX] = {0};

    getProperty(socket_id, SIM_ENABLED_PROP, simEnabledProp, "1");
    if (request != RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS &&
            strcmp(simEnabledProp, "0") == 0) {
        sim_status = SIM_ABSENT;
    } else {
        sim_status = getSIMStatus(request, socket_id);
    }

    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
        s_appType[socket_id] = RIL_APPTYPE_UNKNOWN;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    if (num_apps != 0) {
        app_types = getSimTypesMultiApp(socket_id, &num_apps);
    }

    // Only one GSM app, call old function
    if (num_apps == 1 && (app_types[0] == RIL_APPTYPE_USIM || app_types[0] == RIL_APPTYPE_SIM)) {
        free(app_types);
        return getCardStatus(request, socket_id, pp_card_status);
    }

    /* Allocate and initialize base card status. */
    RIL_CardStatus_v1_4 *p_card_status = calloc(1, sizeof(RIL_CardStatus_v1_4));
    p_card_status->base.base.card_state = card_state;
    p_card_status->base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->base.base.gsm_umts_subscription_app_index = -1;
    p_card_status->base.base.cdma_subscription_app_index = -1;
    p_card_status->base.base.ims_subscription_app_index = -1;
    p_card_status->base.base.num_applications = num_apps;
    p_card_status->base.physicalSlotId = socket_id;
    p_card_status->base.atr = NULL;
    p_card_status->base.iccid = NULL;
    p_card_status->eid = "";
    if (sim_status != SIM_ABSENT) {
        // To support EUICC, atr must be reported.
        // See: https://source.android.com/devices/tech/connect/esim-modem-requirements
        p_card_status->base.atr = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getSimAtr(socket_id, p_card_status->base.atr, AT_COMMAND_LEN);
        p_card_status->base.iccid = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getIccId(socket_id, p_card_status->base.iccid, AT_COMMAND_LEN);
    }
    RLOGD("iccid :%s",p_card_status->base.iccid);
    s_appType[socket_id] = getSimType(socket_id);

    for (int i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->base.base.applications[i] = app_status_array[SIM_ABSENT];
    }

    int isimResp = 0;
    if (sim_status == SIM_READY && s_appType[socket_id] == RIL_APPTYPE_USIM) {
        isimResp = initISIM(socket_id);
        RLOGD("app type %d", isimResp);
    }

    if (num_apps == 1) { // Only one CDMA app
        p_card_status->base.base.cdma_subscription_app_index = 0;
        // TODO 3GPP/3GPP2 may have difference
        p_card_status->base.base.applications[0] = app_status_array[sim_status];
        p_card_status->base.base.applications[0].app_type = app_types[0];

        if (isimResp == 1) {
            p_card_status->base.base.ims_subscription_app_index = 1;
            p_card_status->base.base.num_applications++;
            p_card_status->base.base.applications[1] = ims_app_status_array[0];
        }
    } else if (num_apps == 2) { // 3GPP + 3GPP2
        p_card_status->base.base.gsm_umts_subscription_app_index = 0;
        p_card_status->base.base.cdma_subscription_app_index = 1;
        // TODO 3GPP/3GPP2 may have difference
        p_card_status->base.base.applications[0] = app_status_array[sim_status];
        p_card_status->base.base.applications[1] = app_status_array[sim_status];
        p_card_status->base.base.applications[0].app_type = app_types[0];
        p_card_status->base.base.applications[1].app_type = app_types[1];

        if (isimResp == 1) {
            p_card_status->base.base.ims_subscription_app_index = 2;
            p_card_status->base.base.num_applications++;
            p_card_status->base.base.applications[2] = ims_app_status_array[0];
        }
        // Add AID support for multi-apps SIM_IO
        getAidForMultiApp(socket_id, &(p_card_status->base.base.applications[0].aid_ptr),
                &(p_card_status->base.base.applications[1].aid_ptr),
                &(p_card_status->base.base.applications[2].aid_ptr));
    }

    *pp_card_status = p_card_status;
    free(app_types);
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v1_4 *p_card_status) {
    if (p_card_status == NULL) {
        return;
    }
    free(p_card_status->base.iccid);
    free(p_card_status->base.atr);
    free(p_card_status);
}

void setSimLockAttemptTimes(int type, int attemptTimes,
                            RIL_SOCKET_ID socketId) {
    char num[ARRAY_SIZE] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    static char s_simlockType [5][4] = {"PS", "PN", "PU", "PP", "PC"};
    if (type > 0 && type <= 5) {
        snprintf(prop, sizeof(prop), SIMLOCK_ATTEMPT_TIMES_PROP,
                 s_simlockType[type - 1]);
        RLOGD("set %s, attemptTimes = %d for SIM%d", prop, attemptTimes, socketId);
        snprintf(num, sizeof(num), "%d", attemptTimes);
        setProperty(socketId, prop, num);
    } else {
        RLOGE("invalid type: %d", type);
    }
}

int getNetLockRemainTimes(RIL_SOCKET_ID socket_id, int type) {
    int err = -1;
    int ret = -1;
    int fac = type;
    int ck_type = 1;
    int result[2] = {0, 0};
    char *line = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    RLOGD("[MBBMS] fac:%d, ck_type:%d", fac, ck_type);
    snprintf(cmd, sizeof(cmd), "AT+SPSMPN=%d,%d", fac, ck_type);
    err = at_send_command_singleline(socket_id, cmd, "+SPSMPN:",
                                     &p_response);
    if (err < 0 || p_response->success == 0) {
        ret = -1;
    } else {
        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);

        if (err == 0) {
            err = at_tok_nextint(&line, &result[0]);
            if (err == 0) {
                at_tok_nextint(&line, &result[1]);
                err = at_tok_nextint(&line, &result[1]);
            }
        }

        if (err == 0) {
            ret = result[0] - result[1];
            pthread_mutex_lock(&s_remainTimesMutex);
            setSimLockAttemptTimes(type, result[1], socket_id);
            pthread_mutex_unlock(&s_remainTimesMutex);
        } else {
            ret = -1;
        }
    }
    at_response_free(p_response);
    return ret;
}

int getRemainTimes(RIL_SOCKET_ID socket_id, char *type) {
    if (type == NULL) {
        RLOGE("type is null, return -1");
        return -1;
    } else if (0 == strcmp(type, "PS")) {
        return getNetLockRemainTimes(socket_id, 1);
    } else if (0 == strcmp(type, "PN")) {
        return getNetLockRemainTimes(socket_id, 2);
    } else if (0 == strcmp(type, "PU")) {
        return getNetLockRemainTimes(socket_id, 3);
    } else if (0 == strcmp(type, "PP")) {
        return getNetLockRemainTimes(socket_id, 4);
    } else if (0 == strcmp(type, "PC")) {
        return getNetLockRemainTimes(socket_id, 5);
    } else if (0 == strcmp(type, "SC")) {
        return getSimlockRemainTimes(socket_id, 0);
    } else if (0 == strcmp(type, "FD")) {
        return getSimlockRemainTimes(socket_id, 1);
    } else {
        RLOGE("wrong type %s, return -1", type);
        return -1;
    }
}

unsigned char *convertUsimToSim(unsigned char const *byteUSIM, int len,
                                    unsigned char *hexUSIM) {
    int desIndex = 0;
    int sizeIndex = 0;
    int i = 0;
    unsigned char byteSIM[RESPONSE_EF_SIZE] = {0};
    for (i = 0; i < len; i++) {
        if (byteUSIM[i] == USIM_FILE_DES_TAG) {
            desIndex = i;
            break;
        }
    }
    RLOGE("TYPE_FCP_DES index = %d", desIndex);
    for (i = desIndex; i < len;) {
        if (byteUSIM[i] == USIM_FILE_SIZE_TAG) {
            sizeIndex = i;
            break;
        } else {
            i += (byteUSIM[i + 1] + 2);
        }
    }
    RLOGE("TYPE_FCP_SIZE index = %d ", sizeIndex);
    byteSIM[RESPONSE_DATA_FILE_SIZE_1] =
            byteUSIM[sizeIndex + USIM_DATA_OFFSET_2];
    byteSIM[RESPONSE_DATA_FILE_SIZE_2] =
            byteUSIM[sizeIndex + USIM_DATA_OFFSET_3];
    byteSIM[RESPONSE_DATA_FILE_TYPE] = TYPE_EF;
    if ((byteUSIM[desIndex + RESPONSE_DATA_FILE_DES_FLAG] & 0x07) ==
        EF_TYPE_TRANSPARENT) {
        RLOGE("EF_TYPE_TRANSPARENT");
        byteSIM[RESPONSE_DATA_STRUCTURE] = 0;
    } else if ((byteUSIM[desIndex + RESPONSE_DATA_FILE_DES_FLAG] & 0x07) ==
                EF_TYPE_LINEAR_FIXED) {
        RLOGE("EF_TYPE_LINEAR_FIXED");
        if (USIM_FILE_DES_TAG != byteUSIM[RESPONSE_DATA_FILE_DES_FLAG]) {
            RLOGE("USIM_FILE_DES_TAG != ...");
            goto error;
        }
        if (TYPE_FILE_DES_LEN != byteUSIM[RESPONSE_DATA_FILE_DES_LEN_FLAG]) {
            RLOGE("TYPE_FILE_DES_LEN != ...");
            goto error;
        }
        byteSIM[RESPONSE_DATA_STRUCTURE] = 1;
        byteSIM[RESPONSE_DATA_RECORD_LENGTH] =
                ((byteUSIM[RESPONSE_DATA_FILE_RECORD_LEN_1] & 0xff) << 8) +
                (byteUSIM[RESPONSE_DATA_FILE_RECORD_LEN_2] & 0xff);
    } else if ((byteUSIM[desIndex + RESPONSE_DATA_FILE_DES_FLAG] & 0x07) ==
                EF_TYPE_CYCLIC) {
        RLOGE("EF_TYPE_CYCLIC");
        byteSIM[RESPONSE_DATA_STRUCTURE] = 3;
        byteSIM[RESPONSE_DATA_RECORD_LENGTH] =
                ((byteUSIM[RESPONSE_DATA_FILE_RECORD_LEN_1] & 0xff) << 8) +
                (byteUSIM[RESPONSE_DATA_FILE_RECORD_LEN_2] & 0xff);
    }
    convertBinToHex((char *)byteSIM, RESPONSE_EF_SIZE, hexUSIM);
    return hexUSIM;
error:
    RLOGD("convert to sim error, return NULL");
    return NULL;
}

static void requestEnterSimPin(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                   RIL_Token t) {
    int err, ret;
    int remaintime = 3;
    char *cmd = NULL;
    char *cpinLine = NULL;
    char *cpinResult = NULL;
    ATResponse *p_response = NULL;
    SimUnlockType rsqtype = UNLOCK_PIN;
    SimStatus simstatus = SIM_ABSENT;

    const char **strings = (const char **)data;

    if (datalen == 2 * sizeof(char *)) {
        ret = asprintf(&cmd, "AT+CPIN=%s", strings[0]);
        rsqtype = UNLOCK_PIN;
    } else if (datalen == 3 * sizeof(char *)) {
        err = at_send_command_singleline(socket_id, "AT+CPIN?",
                                         "+CPIN:", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        cpinLine = p_response->p_intermediates->line;
        err = at_tok_start(&cpinLine);
        if (err < 0) goto error;
        err = at_tok_nextstr(&cpinLine, &cpinResult);
        if (err < 0) goto error;

        if ((0 == strcmp(cpinResult, "READY")) ||
            (0 == strcmp(cpinResult, "SIM PIN"))) {
            ret = asprintf(&cmd, "ATD**05*%s*%s*%s#", strings[0], strings[1],
                            strings[1]);
        } else {
            ret = asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
        }
        rsqtype = UNLOCK_PUK;
        AT_RESPONSE_FREE(p_response);
    } else {
        goto error;
    }

    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        /* add for modem reboot */
        const char *pin = NULL;

        if (datalen == 2 * sizeof(char *)) {
            pin = strings[0];
        } else if (datalen == 3 * sizeof(char *)) {
            pin = strings[1];
        } else {
            goto out;
        }

        if ((pin != NULL) && needCacheSimPin()) {
            unsigned char encryptedPin[ARRAY_SIZE];
            memset(encryptedPin, 0, sizeof(encryptedPin));
            encryptPin(strlen(pin), (char *)pin, encryptedPin);
            setProperty(socket_id, SIM_PIN_PROP, (const char *)encryptedPin);
        }

out:
        remaintime = getSimlockRemainTimes(socket_id, rsqtype);
        if (UNLOCK_PUK == rsqtype) {
            getSimlockRemainTimes(socket_id, UNLOCK_PIN);
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &remaintime,
                sizeof(remaintime));
        simstatus = getSIMStatus(-1, socket_id);
        if (SIM_NETWORK_PERSONALIZATION == simstatus || SIM_READY == simstatus) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                      NULL, 0, socket_id);
        }
        at_response_free(p_response);
        return;
    }

error:
    remaintime = getSimlockRemainTimes(socket_id, rsqtype);
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintime,
                          sizeof(remaintime));
    at_response_free(p_response);
}

static void requestEnterSimPin2(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int remaintimes = 3;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    SimUnlockType rsqtype = UNLOCK_PIN2;
    RIL_Errno errnoType = RIL_E_PASSWORD_INCORRECT;

    const char **pin2 = (const char **)data;
    snprintf(cmd, sizeof(cmd), "AT+ECPIN2=\"%s\"", pin2[0]);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        errnoType = RIL_E_PASSWORD_INCORRECT;
        goto out;
    }

    errnoType = RIL_E_SUCCESS;

out:
    remaintimes = getSimlockRemainTimes(socket_id, rsqtype);
    RIL_onRequestComplete(t, errnoType, &remaintimes,
                          sizeof(remaintimes));
    at_response_free(p_response);
}

static void requestEnterSimPuk2(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                    RIL_Token t) {
    int err, ret;
    char *cmd = NULL;
    const char **strings = (const char **)data;
    ATResponse *p_response = NULL;
    SimStatus simstatus = SIM_ABSENT;

    if (datalen == 3 * sizeof(char *)) {
        ret = asprintf(&cmd, "ATD**052*%s*%s*%s#", strings[0], strings[1],
                        strings[1]);
        if (ret < 0) {
            RLOGE("Failed to asprintf");
            FREEMEMORY(cmd);
            goto error;
        }
    } else {
        goto error;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    getSimlockRemainTimes(socket_id, UNLOCK_PUK2);
    getSimlockRemainTimes(socket_id, UNLOCK_PIN2);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);

    simstatus = getSIMStatus(-1, socket_id);
    RLOGD("simstatus = %d, radioStatus = %d", simstatus,
          s_radioState[socket_id]);
    if (SIM_NETWORK_PERSONALIZATION == simstatus || SIM_READY == simstatus) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL,
                                  0, socket_id);
    }
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    at_response_free(p_response);
}

static void requestChangeSimPin(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                    RIL_Token t) {
    int err, ret;
    int remaintime = 3;
    char *cmd = NULL;
    char *cpinLine = NULL, *cpinResult = NULL;
    const char **strings = (const char **)data;
    ATResponse *p_response = NULL;

    if (datalen == 3 * sizeof(char *)) {
        err = at_send_command_singleline(socket_id, "AT+CPIN?",
                                        "+CPIN:", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        cpinLine = p_response->p_intermediates->line;
        err = at_tok_start(&cpinLine);
        if (err < 0) goto error;

        err = at_tok_nextstr(&cpinLine, &cpinResult);
        if (err < 0) goto error;

        if (0 == strcmp(cpinResult, "SIM PIN")) {
            ret = asprintf(&cmd, "ATD**04*%s*%s*%s#", strings[0], strings[1],
                            strings[1]);
        } else {
            ret = asprintf(&cmd, "AT+CPWD=\"SC\",\"%s\",\"%s\"", strings[0],
                            strings[1]);
        }
        if (ret < 0) {
            RLOGE("Failed to asprintf");
            FREEMEMORY(cmd);
            goto error;
        }
        AT_RESPONSE_FREE(p_response);
    } else {
        goto error;
    }



    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    remaintime = getSimlockRemainTimes(socket_id, UNLOCK_PIN);
    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        /* add for modem reboot */
        if (needCacheSimPin()) {
            const char *pin = NULL;
            pin = strings[1];
            unsigned char encryptedPin[ARRAY_SIZE];

            memset(encryptedPin, 0, sizeof(encryptedPin));
            encryptPin(strlen(pin), (char *) pin, encryptedPin);
            setProperty(socket_id, SIM_PIN_PROP, (const char *) encryptedPin);
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintime,
                          sizeof(remaintime));
    at_response_free(p_response);
}

static void requestChangeSimPin2(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                     RIL_Token t) {
    int err, ret;
    int remaintime = 3;
    char *cmd = NULL;
    char *cpinLine = NULL, *cpinResult = NULL;
    const char **strings = (const char **)data;
    ATResponse *p_response = NULL;

    if (datalen == 3 * sizeof(char *)) {
        err = at_send_command_singleline(socket_id, "AT+CPIN?",
                                        "+CPIN:", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }
        cpinLine = p_response->p_intermediates->line;
        err = at_tok_start(&cpinLine);
        if (err < 0) goto error;

        err = at_tok_nextstr(&cpinLine, &cpinResult);
        if (err < 0) goto error;

        if (0 == strcmp(cpinResult, "SIM PIN")) {
            ret = asprintf(&cmd, "ATD**042*%s*%s*%s#", strings[0], strings[1],
                            strings[1]);
        } else {
            ret = asprintf(&cmd, "AT+CPWD=\"P2\",\"%s\",\"%s\"", strings[0],
                            strings[1]);
        }
        if (ret < 0) {
            RLOGE("Failed to asprintf");
            FREEMEMORY(cmd);
            goto error;
        }
        AT_RESPONSE_FREE(p_response);
    } else {
        goto error;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    remaintime = getSimlockRemainTimes(socket_id, UNLOCK_PIN2);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintime,
                          sizeof(remaintime));
    at_response_free(p_response);
}

static void requestFacilityLock(int request, RIL_SOCKET_ID socket_id, char **data,
                                size_t datalen, RIL_Token t) {
    int err, result, status;
    int serviceClass = 0;
    int ret = -1;
    int errNum = -1;
    int remainTimes = 10;
    int response[2] = {0};
    char *cmd = NULL, *line = NULL;
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;
    RIL_Errno errnoType = RIL_E_GENERIC_FAILURE;

    char *type = data[0];

    if (datalen != 5 * sizeof(char *)) {
        goto error1;
    }
    if (data[0] == NULL || data[1] == NULL ||
       (data[2] == NULL && request == RIL_REQUEST_SET_FACILITY_LOCK) ||
        strlen(data[0]) == 0 || strlen(data[1]) == 0 ||
       (request == RIL_REQUEST_SET_FACILITY_LOCK && strlen(data[2]) == 0 )) {
        errnoType = RIL_E_INVALID_ARGUMENTS;
        RLOGE("FacilityLock invalid arguments");
        goto error1;
    }

    serviceClass = atoi(data[3]);
    if (serviceClass == 0) {
        ret = asprintf(&cmd, "AT+CLCK=\"%s\",%c,\"%s\"", data[0], *data[1],
                        data[2]);
    } else {
        ret = asprintf(&cmd, "AT+CLCK=\"%s\",%c,\"%s\",%s", data[0], *data[1],
                        data[2], data[3]);
    }

    if (ret < 0) {
        RLOGE("Failed to asprintf");
        goto error1;
    }

    if (*data[1] == '2') {  // query status
        err = at_send_command_multiline(socket_id, cmd, "+CLCK: ",
                                        &p_response);

        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        for (p_cur = p_response->p_intermediates; p_cur != NULL;
             p_cur = p_cur->p_next) {
            line = p_cur->line;

            err = at_tok_start(&line);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &status);
            if (err < 0) goto error;
            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &serviceClass);
                if (err < 0) goto error;
            }
            response[0] = status;
            response[1] |= serviceClass;
        }
        if (0 == strcmp(data[0], "FD")) {
            if (queryFDNServiceAvailable(socket_id) == 2) {
                response[0] = 2;
            }
        }

        if (request == RIL_EXT_REQUEST_QUERY_FACILITY_LOCK) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
        } else {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response[0], sizeof(int));
        }
    } else {  // unlock/lock this facility
        const char *str = "+CLCK:";
        if (!strcmp(data[0], "FD")) {
            int *mode = (int *)malloc(sizeof(int));
            *mode = atoi(data[1]);
            // timeout is in seconds
            RLOGD("enqueueAsyncCmdMessage");
            enqueueAsyncCmdMessage(socket_id, t, str, (void *)mode,
                    asyncCmdTimedCallback, 10);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RLOGD("removeAsyncCmdMessage");
                removeAsyncCmdMessage(t);
                goto error;
            } else {
                getSimlockRemainTimes(socket_id, UNLOCK_PIN2);
                goto done;
            }
        } else if (!strcmp(data[0], "SC")) {
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                goto error;
            } else {
                if (needCacheSimPin()) {
                    char *pin = NULL;
                    pin = data[2];
                    unsigned char encryptedPin[ARRAY_SIZE];
                    memset(encryptedPin, 0, sizeof(encryptedPin));
                    encryptPin(strlen(pin), pin, encryptedPin);

                    setProperty(socket_id, SIM_PIN_PROP, (const char *)encryptedPin);
                }
                getSimlockRemainTimes(socket_id, UNLOCK_PIN);
            }
        } else {
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                goto error;
            }
        }
        result = 1;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &result, sizeof(result));
    }
done:
    free(cmd);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
            strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err >= 0) {
            err = at_tok_nextint(&line, &errNum);
            if (err >= 0) {
                if (errNum == 11) {
                    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                              NULL, 0, socket_id);
                } else if (errNum == 70 || errNum == 128 || errNum == 254) {
                    errnoType = RIL_E_FDN_CHECK_FAILURE;
                } else if (errNum == 12 || errNum == 16) {
                    errnoType = RIL_E_PASSWORD_INCORRECT;
                } else if (errNum == 3 && !strcmp(data[0], "SC") &&
                        *data[1] == '1') {
                    errnoType = RIL_E_SUCCESS;
                }
            }
        }
    }

error1:
    remainTimes = getRemainTimes(socket_id, type);
    RIL_onRequestComplete(t, errnoType, &remainTimes, sizeof(remainTimes));
    at_response_free(p_response);
    free(cmd);
}

static int queryFDNServiceAvailable(RIL_SOCKET_ID socket_id) {
    int status = -1;
    int err;
    char *cmd = NULL;
    char *line = NULL;
    char pad_data = '0';
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));

    if (s_appType[socket_id] == RIL_APPTYPE_USIM) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%c,\"%s\"",
                 READ_BINERY, EFID_SST, 0, 0, 1, pad_data, DF_ADF);
    } else if (s_appType[socket_id] == RIL_APPTYPE_SIM) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%c,\"%s\"",
                 READ_BINERY, EFID_SST, 0, 0, 1, pad_data, DF_GSM);
    } else {
        goto out;
    }
    pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
    err = at_send_command_singleline(socket_id, cmd, "+CRSM:",
                                     &p_response);
    pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto out;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto out;

    err = at_tok_nextint(&line, &(sr.sw1));
    if (err < 0) goto out;

    err = at_tok_nextint(&line, &(sr.sw2));
    if (err < 0) goto out;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr.simResponse));
        if (err < 0) goto out;
    }

    if (sr.simResponse == NULL || strlen(sr.simResponse) < 2) {
        goto out;
    }

    unsigned char byteFdn[1] = {'\0'};
    convertHexToBin(sr.simResponse, 2, (char *)byteFdn);
    RLOGD("queryFDNServiceAvailable: byteFdn[0] = %d", byteFdn[0]);
    if (s_appType[socket_id] == RIL_APPTYPE_USIM) {
        if ((byteFdn[0] & 0x02) != 0x02) status = 2;
    } else if (s_appType[socket_id] == RIL_APPTYPE_SIM) {
        if (((byteFdn[0] >> 4) & 0x01) != 0x01) status = 2;
    }
out:
    at_response_free(p_response);
    return status;
}

static bool isISIMFileId(int fileId) {
    return (fileId == 0x6f04 || fileId == 0x6f02 || fileId == 0x6f03 ||
            fileId == 0x6f07  || fileId == 0x6f09 || fileId == 0x6fe5);
}

int readSimRecord(RIL_SOCKET_ID socket_id, RIL_SIM_IO_v6 *data, RIL_SIM_IO_Response *sr) {
    int err;
    char *cmd = NULL;
    char *line = NULL;
    char pad_data = '0';
    ATResponse *p_response = NULL;

    RIL_SIM_IO_v6 *p_args = data;
    bool isISIMfile = isISIMFileId(p_args->fileid);

    char *aid = p_args->aidPtr;
    bool support_aid = false;

    /* FIXME handle pin2 */
    if (p_args->pin2 != NULL) {
        RLOGI("Reference-ril. requestSIM_IO pin2");
    }
    if (p_args->data == NULL) {
        if (isISIMfile) {
            if (s_simSessionId[socket_id] == -1) {
                RLOGE("s_simSessionId is -1, SIM_IO return ERROR");
                goto error;
            }
            err = asprintf(&cmd, "AT+CRLA=%d,%d,%d,%d,%d,%d,%c,\"%s\"",
                    s_simSessionId[socket_id],
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3,pad_data,p_args->path);
        } else {
            // C2K support multiple app card
            // Bug1017151 Access DF_TELECOM files should not use CSIM AID, so reuse AT+CRSM
            if (aid != NULL && aid[0] != '\0' && !strStartsWith(p_args->path, DF_TELECOM)) {
                support_aid = true;
                int appType = 0;
                appType = getAppTypeByAidForSPCRSM(aid);

                err = asprintf(&cmd, "AT+SPCRSM=%d,%d,%d,%d,%d,%c,\"%s\",%d,%d,\"%s\"",
                        p_args->command, p_args->fileid, p_args->p1, p_args->p2,
                        p_args->p3, pad_data, p_args->path, appType,
                        (int)(strlen(aid) / 2), aid);
            } else {
                err = asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%c,\"%s\"",
                        p_args->command, p_args->fileid, p_args->p1, p_args->p2,
                        p_args->p3, pad_data, p_args->path);
            }
        }
    } else {
        if (isISIMfile) {
            if (s_simSessionId[socket_id] == -1) {
                RLOGE("s_simSessionId is -1, SIM_IO return ERROR");
                goto error;
            }
            err = asprintf(&cmd, "AT+CRLA=%d,%d,%d,%d,%d,%d,\"%s\",\"%s\"",
                    s_simSessionId[socket_id],
                    p_args->command, p_args->fileid,
                    p_args->p1, p_args->p2, p_args->p3, p_args->data,p_args->path);
        } else {
            // C2K support multiple app card
            // Bug1017151 Access DF_TELECOM files should not use CSIM AID, so reuse AT+CRSM
            if (aid != NULL && aid[0] != '\0' && !strStartsWith(p_args->path, DF_TELECOM)) {
                support_aid = true;
                int appType = 0;
                appType = getAppTypeByAidForSPCRSM(aid);
                err = asprintf(&cmd, "AT+SPCRSM=%d,%d,%d,%d,%d,\"%s\",\"%s\",%d,%d,\"%s\"",
                        p_args->command, p_args->fileid, p_args->p1, p_args->p2,
                        p_args->p3, p_args->data, p_args->path, appType,
                        (int)(strlen(aid) / 2), aid);
            } else {
                err = asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,\"%s\",\"%s\"",
                        p_args->command, p_args->fileid, p_args->p1, p_args->p2,
                        p_args->p3, p_args->data, p_args->path);
            }
        }
    }
    if (err < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }

    pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
    if (support_aid) {
        err = at_send_command_singleline(socket_id, cmd,
                "+SPCRSM:",  &p_response);
    } else {
        err = at_send_command_singleline(socket_id, cmd,
                isISIMfile ? "+CRLA:" : "+CRSM:",  &p_response);
    }
    pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr->sw1));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(sr->sw2));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        char *simResponse = NULL;
        err = at_tok_nextstr(&line, &simResponse);
        if (err < 0 || simResponse == NULL) goto error;

        sr->simResponse = (char *)calloc(strlen(simResponse) + 1, sizeof(char));
        snprintf(sr->simResponse, strlen(simResponse) + 1, "%s", simResponse);
    }

    if (s_appType[socket_id] == RIL_APPTYPE_USIM &&
        (p_args->command == COMMAND_GET_RESPONSE)) {
        RLOGD("usim card, change to sim format");
        if (sr->simResponse != NULL) {
            RLOGD("sr.simResponse NOT NULL, convert to sim");
            unsigned char *byteUSIM = NULL;
            // simResponse could not be odd, ex "EF3EF0"
            int usimLen = strlen(sr->simResponse) / 2;
            byteUSIM = (unsigned char *)alloca(usimLen + sizeof(char));
            memset(byteUSIM, 0, usimLen + sizeof(char));
            convertHexToBin(sr->simResponse, strlen(sr->simResponse),
                    (char *)byteUSIM);
            if (byteUSIM[RESPONSE_DATA_FCP_FLAG] != TYPE_FCP) {
                RLOGE("wrong fcp flag, unable to convert to sim ");
                goto error;
            }

            unsigned char hexUSIM[RESPONSE_EF_SIZE * 2 + TYPE_CHAR_SIZE] = {0};
            memset(hexUSIM, 0, RESPONSE_EF_SIZE * 2 + TYPE_CHAR_SIZE);
            if (NULL != convertUsimToSim(byteUSIM, usimLen, hexUSIM)) {
                memset(sr->simResponse, 0, usimLen * 2);
                strncpy(sr->simResponse, (char *)hexUSIM,
                        RESPONSE_EF_SIZE * 2);
            }

            if (sr->simResponse == NULL) {
                 RLOGE("unable convert to sim, return error");
                 goto error;
             }
        }
    }
    at_response_free(p_response);
    return 0;

error:
    at_response_free(p_response);
    return -1;
}

static void requestSIM_IO(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    RIL_SIM_IO_v6 *p_args = (RIL_SIM_IO_v6 *)data;;
    RIL_SIM_IO_Response *sr =
            (RIL_SIM_IO_Response *)calloc(1, sizeof(RIL_SIM_IO_Response));

    if (p_args->command == READ_BINERY && p_args->p3 > 255 ) {
        //if p3 = 780,need send 4 times CRSM
        //1:P1 = 0, P2 = 0, P3 = 255
        //2:P1 = 0, P2 = 255, P3 = 255
        //3:P1 = 1, P2 = 254, P3 = 255
        //4:P1 = 2, P2 = 253, P3 = 15 (780-765)
        int p3 = p_args->p3;
        int times, i, total = 0;
        char *simResponse = (char *)calloc(p3 * 2 + 1, sizeof(char));
        if (p3 % 255 == 0) {
            times = p3 / 255;
        } else {
            times = p3 / 255 + 1;
        }
        for (i = 0; i < times; i++) {
            memset(sr, 0, sizeof(RIL_SIM_IO_Response));

            p_args->p1 = total >> 8;
            p_args->p2 = total & 0xff;
            p_args->p3 = (p3-total) >= 255 ? 255 : p3 % 255;
            total += p_args->p3;
            RLOGD("p1 = %d, p2 = %d, p3 = %d", p_args->p1, p_args->p2, p_args->p3);
            err = readSimRecord(socket_id, p_args, sr);
            if (err < 0) {
                FREEMEMORY(sr->simResponse);
                sr->simResponse = simResponse;
                goto error;
            }
            if (sr->sw1 != 0x90 && sr->sw1 != 0x91 && sr->sw1 != 0x9e
                    && sr->sw1 != 0x9f) {
                sr->simResponse = simResponse;
                goto done;
            }
            strncat(simResponse, sr->simResponse, p_args->p3 * 2);
            FREEMEMORY(sr->simResponse);
        }
        sr->simResponse = simResponse;
        RLOGD("simResponse = %s", sr->simResponse);
    } else {
        err = readSimRecord(socket_id, p_args, sr);
        if (err < 0) goto error;
    }
done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, sr, sizeof(RIL_SIM_IO_Response));
    free(sr->simResponse);
    free(sr);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    free(sr->simResponse);
    free(sr);
}

static void requestTransmitApduBasic(RIL_SOCKET_ID socket_id, void *data,
                                          size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, len;
    char *cmd = NULL;
    char *line = NULL;
    RIL_SIM_APDU *p_args = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_APDU *)data;

retry:
    if ((p_args->data == NULL) || (strlen(p_args->data) == 0)) {
        if (p_args->p3 < 0) {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x\"", 8, p_args->cla,
                    p_args->instruction, p_args->p1, p_args->p2);
        } else {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x\"", 10,
                    p_args->cla, p_args->instruction, p_args->p1, p_args->p2,
                    p_args->p3);
        }
    } else {
        asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x%s\"",
                10 + (int)strlen(p_args->data), p_args->cla,
                p_args->instruction, p_args->p1, p_args->p2, p_args->p3,
                p_args->data);
    }
    err = at_send_command_singleline(socket_id, cmd, "+CSIM:",
                                     &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    // Bug 1111866, to fix CarrierApiTest#testIccTransmitApduBasicChannel
    if ((len == 4) && (sr.sw1 == 0x6C)) {
        p_args->p3 = sr.sw2;
        AT_RESPONSE_FREE(p_response);
        goto retry;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);

    // end sim toolkit session if 90 00 on TERMINAL RESPONSE
    if ((p_args->instruction == 20) && (sr.sw1 == 0x90)) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END,
                NULL, 0, socket_id);
    }

    // return if no sim toolkit proactive command is ready
    if (sr.sw1 != 0x91) {
        return;
    }

    // fetch
    p_response = NULL;
    asprintf(&cmd, "AT+CSIM=10,\"a0120000%02x\"", sr.sw2);
    err = at_send_command_singleline(socket_id, cmd, "+CSIM:",
                                     &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto fetch_error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto fetch_error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto fetch_error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto fetch_error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, sr.simResponse,
                              strlen(sr.simResponse), socket_id);
    goto fetch_error;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

fetch_error:
    at_response_free(p_response);
}

/**
 * Add for Bug1111866
 * If open logical channel with AID NULL, this means open logical channel to MF.
 * If there is P2 value, this P2 value is used for SELECT command.
 * In addition, if SELECT command returns 61xx, GET RESPONSE command needs to send to get data.
 */
static int sendCmdAgainForOpenChannelWithP2(RIL_SOCKET_ID socket_id, char *data,
                                        int p2, int *response, int *rspLen) {
    int len, err;
    char *line = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    RIL_Errno errType = RIL_E_GENERIC_FAILURE;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));
    sscanf(data, "%2x", &(response[0]));  // response[0] is channel number

    // Send SELECT command to MF
    snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,14,\"00A400%02X02%s\"", response[0],
             p2, MF_SIM);
retry:
    pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
    err = at_send_command_singleline(socket_id, cmd, "+CGLA:",
                                     &p_response);
    pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
    if (err < 0) goto done;
    if (p_response != NULL && p_response->success == 0) {
        if (!strcmp(p_response->finalResponse, "+CME ERROR: 21") ||
            !strcmp(p_response->finalResponse, "+CME ERROR: 50")) {
            errType = RIL_E_INVALID_PARAMETER;
        }
        goto done;
    }

    line = p_response->p_intermediates->line;

    if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &len) < 0 ||
        at_tok_nextstr(&line, &(sr.simResponse)) < 0) {
        goto done;
    }

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));

    if (sr.sw1 == 0x61) {  // Need to send GET RESPONSE command
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,10,\"00C00000%s\"",
                 response[0], sr.simResponse + len - 2);
        AT_RESPONSE_FREE(p_response);
        goto retry;
    } else if (sr.sw1 == 0x90 && sr.sw2 == 0x00) {  // 9000 is successful
        int length = len / 2;
        for (*rspLen = 1; *rspLen <= length; (*rspLen)++) {
            sscanf(sr.simResponse, "%02x", &(response[*rspLen]));
            sr.simResponse += 2;
        }
        errType = RIL_E_SUCCESS;
    } else {  // close channel
        snprintf(cmd, sizeof(cmd), "AT+CCHC=%d", response[0]);
        at_send_command(socket_id, cmd, NULL);
    }

done:
    at_response_free(p_response);
    return errType;
}

static void requestOpenLogicalChannel(RIL_SOCKET_ID socket_id, void *data,
                                           size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    int err_no = RIL_E_GENERIC_FAILURE;
    int responseLen = 1;
    int response[260] = {0};
    char *cmd = NULL;
    char *line = NULL;
    char *statusWord = NULL;
    char *responseData = NULL;
    ATResponse *p_response = NULL;

    RIL_OpenChannelParams *params = (RIL_OpenChannelParams *)data;

    pthread_mutex_lock(&s_SPCCHOMutex[socket_id]);
    if (params->aidPtr != NULL) {
        if (params->p2 < 0) {
            asprintf(&cmd, "AT+SPCCHO=\"%s\"", params->aidPtr);
        } else {
            asprintf(&cmd, "AT+SPCCHO=\"%s\",%d", params->aidPtr, params->p2);
        }
        err = at_send_command_singleline(socket_id, cmd,
                                        "+SPCCHO:", &p_response);
        free(cmd);
    } else { // Bug1111866, AID NULL means open a logical channel and Select MF
        err = at_send_command_singleline(socket_id,
                            "AT+CSIM=10,\"0070000001\"", "+CSIM:", &p_response);
    }
    pthread_mutex_unlock(&s_SPCCHOMutex[socket_id]);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        if (!strcmp(p_response->finalResponse, "+CME ERROR: 20")) {
            err_no = RIL_E_MISSING_RESOURCE;
        } else if (!strcmp(p_response->finalResponse, "+CME ERROR: 22")) {
            err_no = RIL_E_NO_SUCH_ELEMENT;
        }
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    // Read channel number
    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    // Read select response (if available)
    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &statusWord);
        if (err < 0) goto error;

        if (params->aidPtr == NULL) {  // response format: +CSIM: 6,019000
            // 9000 represents success, and it's status code
            if (response[0] < (int)strlen("019000") ||
               (strcmp(&(statusWord[response[0] - 4]), "9000") != 0)) {
                goto error;
            }

            if (params->p2 < 0) {
                int length = response[0] / 2;
                for (responseLen = 0; responseLen < length; responseLen++) {
                    sscanf(statusWord, "%02x", &(response[responseLen]));
                    statusWord += 2;
                }
                asprintf(&cmd, "AT+CGLA=%d,14,\"00A4000002%s\"", response[0], MF_SIM);
                pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
                // select MF, doesn't care AT response
                at_send_command_singleline(socket_id, cmd, "+CGLA:", NULL);
                pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
                free(cmd);
            } else {
                err_no = sendCmdAgainForOpenChannelWithP2(socket_id, statusWord,
                                            params->p2, response, &responseLen);
                if (err_no != RIL_E_SUCCESS) {
                    goto error;
                }
            }
        } else {
            if (at_tok_hasmore(&line)) {
                err = at_tok_nextstr(&line, &responseData);
                if (err < 0) goto error;
                int length = strlen(responseData) / 2;
                for (responseLen = 1; responseLen <= length; responseLen++) {
                    sscanf(responseData, "%02x", &(response[responseLen]));
                    responseData += 2;
                }
            }
            sscanf(statusWord, "%02x%02x", &(response[responseLen]),
                    &(response[responseLen + 1]));
            responseLen = responseLen + 2;
        }
    } else {
        // no select response, set status word
        response[responseLen] = 0x90;
        response[responseLen + 1] = 0x00;
        responseLen = responseLen + 2;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, responseLen * sizeof(int));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, err_no, NULL, 0);
    at_response_free(p_response);
}

static void requestCloseLogicalChannel(RIL_SOCKET_ID socket_id, void *data,
                                            size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    int session_id = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    session_id = ((int *)data)[0];

    if (session_id == 0) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCHC=%d", session_id);
        err = at_send_command(socket_id, cmd, &p_response);

        if (err < 0 || p_response->success == 0) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        }
        at_response_free(p_response);
    }
}

static void requestTransmitApdu(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                    RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    int err_no = RIL_E_GENERIC_FAILURE;
    int len;
    char *cmd = NULL;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    RIL_SIM_APDU *p_args = NULL;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_APDU *)data;

retry:
    if ((p_args->data == NULL) || (strlen(p_args->data) == 0)) {
        if (p_args->p3 < 0) {
            asprintf(&cmd, "AT+CGLA=%d,%d,\"%02x%02x%02x%02x\"",
                    p_args->sessionid, 8, p_args->cla, p_args->instruction,
                    p_args->p1, p_args->p2);
        } else {
            asprintf(&cmd, "AT+CGLA=%d,%d,\"%02x%02x%02x%02x%02x\"",
                    p_args->sessionid, 10, p_args->cla, p_args->instruction,
                    p_args->p1, p_args->p2, p_args->p3);
        }
    } else {
        asprintf(&cmd, "AT+CGLA=%d,%d,\"%02x%02x%02x%02x%02x%s\"",
                p_args->sessionid, 10 + (int)strlen(p_args->data), p_args->cla,
                p_args->instruction, p_args->p1, p_args->p2, p_args->p3,
                p_args->data);
    }

    pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
    err = at_send_command_singleline(socket_id, cmd, "+CGLA:",
                                     &p_response);
    pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
    free(cmd);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        if (!strcmp(p_response->finalResponse, "+CME ERROR: 21") ||
            !strcmp(p_response->finalResponse, "+CME ERROR: 50")) {
            err_no = RIL_E_INVALID_PARAMETER;
        }
        goto error;
    }

    line = p_response->p_intermediates->line;

    if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &len) < 0
            || at_tok_nextstr(&line, &(sr.simResponse)) < 0) {
        err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    // Handle status word 6c and 61, REF(ISO/IEC 7816-4):
    // 6cxx means xx data available for GET RESPONSE;
    // 61xx means expected data length (p3 or le) should be xx;
    // Add for OMAPI, if there is data follow with 61xx, report to Apk without retry;
    if (sr.sw1 == 0x6c) {
        p_args->p3 = sr.sw2;
        AT_RESPONSE_FREE(p_response);
        RLOGD("Received APDU sw1 6c. Retry with GET RESPONSE.");
        goto retry;
    } else if (sr.sw1 == 0x61
            && (strlen(sr.simResponse) == 0)) {
        p_args->p1 = 0x00;
        p_args->p2 = 0x00;
        p_args->p3 = sr.sw2;
        p_args->instruction = 0xc0;
        p_args->data = NULL;
        AT_RESPONSE_FREE(p_response);
        RLOGD("Received APDU sw1 61 without data. Retry with correct Le.");
        goto retry;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    AT_RESPONSE_FREE(p_response);

    // end sim toolkit session if 90 00 on TERMINAL RESPONSE
    if ((p_args->instruction == 20) && (sr.sw1 == 0x90)) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0,
                                  socket_id);
    }

    // return if no sim toolkit proactive command is ready
    if (sr.sw1 != 0x91) {
        return;
    }

    // fetch
    p_response = NULL;
    asprintf(&cmd, "AT+CGLA= %d, 10,\"a0120000%02x\"",
            p_args->sessionid, sr.sw2);
    pthread_mutex_lock(&s_CglaCrsmMutex[socket_id]);
    err = at_send_command_singleline(socket_id, cmd, "+CSIM:",
            &p_response);
    pthread_mutex_unlock(&s_CglaCrsmMutex[socket_id]);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto fetch_error;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto fetch_error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto fetch_error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto fetch_error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, sr.simResponse,
                              strlen(sr.simResponse), socket_id);
    goto fetch_error;

error:
    RIL_onRequestComplete(t, err_no, NULL, 0);

fetch_error:
    at_response_free(p_response);
}

int base64_decode(const char *base64, unsigned char *bindata) {
    int i, j;
    unsigned char k;
    unsigned char temp[4];

    for (i = 0, j = 0; base64[i] != '\0'; i += 4) {
        memset(temp, 0xFF, sizeof(temp));
        for (k = 0; k < 64; k++) {
            if (base64char[k] == base64[i]) {
                temp[0] = k;
            }
        }
        for (k = 0; k < 64; k++) {
            if (base64char[k] == base64[i + 1]) {
                temp[1] = k;
            }
        }
        for (k = 0; k < 64; k++) {
            if (base64char[k] == base64[i + 2]) {
                temp[2] = k;
            }
        }
        for (k = 0; k < 64; k++) {
            if (base64char[k] == base64[i + 3]) {
                temp[3] = k;
            }
        }

        bindata[j++] =
                ((unsigned char)(((unsigned char)(temp[0] << 2)) & 0xFC))
                | ((unsigned char)((unsigned char)(temp[1] >> 4) & 0x03));
        if (base64[i + 2] == '=') {
            break;
        }
        bindata[j++] =
                ((unsigned char)(((unsigned char)(temp[1] << 4)) & 0xF0))
                | ((unsigned char)((unsigned char)(temp[2] >> 2) & 0x0F));
        if (base64[i + 3] == '=') {
            break;
        }
        bindata[j++] =
                ((unsigned char)(((unsigned char)(temp[2] << 6)) & 0xF0))
                | ((unsigned char) (temp[3] & 0x3F));
    }
    return j;
}

char *base64_encode(const unsigned char *bindata, char *base64,
                      int binlength) {
    int i, j;
    unsigned char current;

    for (i = 0, j = 0; i < binlength; i += 3) {
        current = (bindata[i] >> 2);
        current &= (unsigned char)0x3F;
        base64[j++] = base64char[(int)current];

        current = ((unsigned char)(bindata[i] << 4)) & ((unsigned char)0x30);
        if (i + 1 >= binlength) {
            base64[j++] = base64char[(int)current];
            base64[j++] = '=';
            base64[j++] = '=';
            break;
        }
        current |= ((unsigned char)(bindata[i + 1] >> 4))
                & ((unsigned char) 0x0F);
        base64[j++] = base64char[(int)current];

        current = ((unsigned char)(bindata[i + 1] << 2))
                & ((unsigned char)0x3C);
        if (i + 2 >= binlength) {
            base64[j++] = base64char[(int)current];
            base64[j++] = '=';
            break;
        }
        current |= ((unsigned char)(bindata[i + 2] >> 6))
                & ((unsigned char)0x03);
        base64[j++] = base64char[(int)current];

        current = ((unsigned char)bindata[i + 2]) & ((unsigned char)0x3F);
        base64[j++] = base64char[(int)current];
    }
    base64[j] = '\0';
    return base64;
}

static void requestUSimAuthentication(RIL_SOCKET_ID socket_id, char *authData,
        RIL_Token t) {
    int err = -1, ret = 0;
    int status = 0;
    int binSimResponseLen = 0;
    int randLen = 0, autnLen = 0, resLen = 0, ckLen = 0, ikLen = 0, autsLen = 0;
    char *cmd = NULL;
    char *line = NULL;
    char *rand = NULL;
    char *autn = NULL;
    char *res = NULL, *ck = NULL, *ik = NULL, *auts = NULL;
    unsigned char *binSimResponse = NULL;
    unsigned char *binAuthData = NULL;
    unsigned char *hexAuthData = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response response;

    memset(&response, 0, sizeof(response));
    response.sw1 = 0x90;
    response.sw2 = 0;

    binAuthData  = (unsigned char *)malloc(sizeof(char) * strlen(authData));
    if (binAuthData == NULL) {
        goto error;
    }
    base64_decode(authData, binAuthData);
    hexAuthData = (unsigned char *)malloc(strlen(authData) * 2 + sizeof(char));
    if (hexAuthData == NULL) {
        goto error;
    }
    memset(hexAuthData, 0, strlen(authData) * 2 + sizeof(char));
    convertBinToHex((char *)binAuthData, strlen(authData), hexAuthData);

    randLen = binAuthData[0];
    autnLen = binAuthData[randLen + 1];
    rand = (char *)malloc(sizeof(char) * (randLen * 2 + sizeof(char)));
    if (rand == NULL) {
        goto error;
    }
    autn = (char *)malloc(sizeof(char) * (autnLen * 2 + sizeof(char)));
    if (autn == NULL) {
        goto error;
    }
    memcpy(rand, hexAuthData + 2, randLen * 2);
    memcpy(rand + randLen * 2, "\0", 1);
    memcpy(autn, hexAuthData + randLen * 2 +4, autnLen * 2);
    memcpy(autn + autnLen * 2, "\0", 1);

    RLOGD("requestUSimAuthentication rand = %s, autn = %s", rand, autn);

    ret = asprintf(&cmd, "AT^MBAU=\"%s\",\"%s\"", rand, autn);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }
    err = at_send_command_singleline(socket_id, cmd, "^MBAU:",
            &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &status);
        if (err < 0) goto error;

        if (status == SIM_AUTH_RESPONSE_SUCCESS) {
            err = at_tok_nextstr(&line, &res);
            if (err < 0) goto error;
            resLen = strlen(res);

            err = at_tok_nextstr(&line, &ck);
            if (err < 0) goto error;
            ckLen = strlen(ck);

            err = at_tok_nextstr(&line, &ik);
            if (err < 0) goto error;
            ikLen = strlen(ik);

            // 0xdb + resLen + res + ckLen + ck  + ikLen + ik + '\0'
            binSimResponseLen =
                    (resLen + ckLen + ikLen) / 2 + 4 * sizeof(char);
            binSimResponse =
                (unsigned char *)malloc(binSimResponseLen + sizeof(char));
            if (binSimResponse == NULL) {
                goto error;
            }
            memset(binSimResponse, 0, binSimResponseLen + sizeof(char));
            // set flag to first byte
            binSimResponse[0] = 0xDB;
            // set resLen and res
            binSimResponse[1] = (resLen / 2) & 0xFF;
            convertHexToBin(res, resLen, (char *)(binSimResponse + 2));
            // set ckLen and ck
            binSimResponse[2 + resLen / 2] = (ckLen / 2) & 0xFF;
            convertHexToBin(ck, ckLen,
                (char *)(binSimResponse + 2 + resLen / 2 + 1));
            // set ikLen and ik
            binSimResponse[2 + resLen / 2 + 1 + ckLen / 2] = (ikLen / 2) & 0xFF;
            convertHexToBin(ik, ikLen,
                (char *)(binSimResponse + 2 + resLen/2 + 1 + ckLen / 2 + 1));

            response.simResponse =
                    (char *)malloc(2 * binSimResponseLen + sizeof(char));
            if (response.simResponse  == NULL) {
                goto error;
            }
            base64_encode(binSimResponse, response.simResponse,
                    binSimResponse[1] + binSimResponse[2 + resLen / 2] +
                    binSimResponse[2 + resLen / 2 + 1 + ckLen / 2] + 4);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                    sizeof(response));
        } else if (status == SIM_AUTH_RESPONSE_SYNC_FAILURE) {
            err = at_tok_nextstr(&line, &auts);
            if (err < 0) goto error;
            autsLen = strlen(auts);
            RLOGD("requestUSimAuthentication auts = %s, autsLen = %d",
                    auts, autsLen);

            binSimResponseLen = autsLen / 2 + 2 * sizeof(char);
            binSimResponse =
                (unsigned char *)malloc(binSimResponseLen + sizeof(char));
            if (binSimResponse  == NULL) {
                goto error;
            }
            memset(binSimResponse, 0, binSimResponseLen + sizeof(char));
            // set flag to first byte
            binSimResponse[0] = 0xDC;
            // set autsLen and auts
            binSimResponse[1] = (autsLen / 2) & 0xFF;
            convertHexToBin(auts, autsLen, (char *)(binSimResponse + 2));

            response.simResponse =
                (char *)malloc(2 * binSimResponseLen + sizeof(char));
            if (response.simResponse  == NULL) {
                goto error;
            }
            base64_encode(binSimResponse, response.simResponse,
                    binSimResponse[1] + 2);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                    sizeof(response));
        } else {
            goto error;
        }
    }
    at_response_free(p_response);

    FREEMEMORY(binAuthData);
    FREEMEMORY(hexAuthData);
    FREEMEMORY(rand);
    FREEMEMORY(autn);
    FREEMEMORY(response.simResponse);
    FREEMEMORY(binSimResponse);
    return;

error:
    FREEMEMORY(binAuthData);
    FREEMEMORY(hexAuthData);
    FREEMEMORY(rand);
    FREEMEMORY(autn);
    FREEMEMORY(response.simResponse);
    FREEMEMORY(binSimResponse);

    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSimAuthentication(RIL_SOCKET_ID socket_id, char *authData,
        RIL_Token t) {
    int err = -1, ret = 0;
    int status = 0;
    int binSimResponseLen = 0;
    int randLen = 0, kcLen = 0, sresLen = 0;
    char *cmd = NULL;
    char *line = NULL;
    char *rand = NULL;
    char *kc = NULL, *sres = NULL;
    unsigned char *binSimResponse = NULL;
    unsigned char *binAuthData = NULL;
    unsigned char *hexAuthData = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response response;

    memset(&response, 0, sizeof(response));
    response.sw1 = 0x90;
    response.sw2 = 0;

    binAuthData  =
            (unsigned char *)malloc(sizeof(char) * strlen(authData));
    if (binAuthData == NULL) {
        goto error;
    }
    base64_decode(authData, binAuthData);
    hexAuthData =
            (unsigned char *)malloc(strlen(authData) * 2 + sizeof(char));
    if (hexAuthData == NULL) {
        goto error;
    }
    memset(hexAuthData, 0, strlen(authData) * 2 + sizeof(char));
    convertBinToHex((char *)binAuthData, strlen(authData), hexAuthData);

    randLen = binAuthData[0];
    rand = (char *)malloc(sizeof(char) * (randLen * 2 + sizeof(char)));
    if (rand == NULL) {
        goto error;
    }
    memcpy(rand, hexAuthData + 2, randLen * 2);
    memcpy(rand + randLen * 2, "\0", 1);

    RLOGD("requestSimAuthentication rand = %s", rand);
    ret = asprintf(&cmd, "AT^MBAU=\"%s\"", rand);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }
    err = at_send_command_singleline(socket_id, cmd, "^MBAU:",
            &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        line = p_response->p_intermediates->line;
        RLOGD("requestSimAuthentication: err= %d line= %s", err, line);
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &status);
        if (err < 0) goto error;

        if (status == SIM_AUTH_RESPONSE_SUCCESS) {
            err = at_tok_nextstr(&line, &kc);
            if (err < 0) goto error;
            kcLen = strlen(kc);

            err = at_tok_nextstr(&line, &sres);
            if (err < 0) goto error;
            sresLen = strlen(sres);

            // sresLen + sres + kcLen + kc + '\0'
            binSimResponseLen = (kcLen + sresLen) / 2 + 3 * sizeof(char);
            binSimResponse =
                (unsigned char *)malloc(binSimResponseLen + sizeof(char));
            if (binSimResponse == NULL) {
                goto error;
            }
            memset(binSimResponse, 0, binSimResponseLen + sizeof(char));
            // set sresLen and sres
            binSimResponse[0] = (sresLen / 2) & 0xFF;
            convertHexToBin(sres, sresLen, (char *)(binSimResponse + 1));
            // set kcLen and kc
            binSimResponse[1 + sresLen / 2] = (kcLen / 2) & 0xFF;
            convertHexToBin(kc, kcLen,
                    (char *)(binSimResponse + 1 + sresLen / 2 + 1));

            response.simResponse =
                    (char *)malloc(2 * binSimResponseLen + sizeof(char));
            if (response.simResponse == NULL) {
                goto error;
            }
            base64_encode(binSimResponse, response.simResponse,
                    binSimResponse[0] + binSimResponse[1 + sresLen / 2] + 2);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                    sizeof(response));
        } else {
            goto error;
        }
    }
    at_response_free(p_response);

    FREEMEMORY(binAuthData);
    FREEMEMORY(hexAuthData);
    FREEMEMORY(rand);
    FREEMEMORY(response.simResponse);
    FREEMEMORY(binSimResponse);
    return;

error:
    FREEMEMORY(binAuthData);
    FREEMEMORY(hexAuthData);
    FREEMEMORY(rand);
    FREEMEMORY(response.simResponse);
    FREEMEMORY(binSimResponse);

    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

int initISIM(RIL_SOCKET_ID socket_id) {
    if (s_imsInitISIM[socket_id] != -1) {
        return s_imsInitISIM[socket_id];
    }
    ATResponse *p_response = NULL;
    char *line = NULL;
    int err = -1;
    err = at_send_command_singleline(socket_id, "AT+ISIM=1",
                                     "+ISIM:", &p_response);
    if (err >= 0 && p_response->success) {
        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err >= 0) {
            err = at_tok_nextint(&line, &s_imsInitISIM[socket_id]);
            RLOGD("Response of ISIM is %d", s_imsInitISIM[socket_id]);
            if (s_imsInitISIM[socket_id] == 1) {
                err = at_tok_nextint(&line, &s_simSessionId[socket_id]);
                RLOGE("SessionId of ISIM is %d", s_simSessionId[socket_id]);
            }
        }
    }
    at_response_free(p_response);
    return s_imsInitISIM[socket_id];
}

static void requestInitISIM(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                            RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);
    int response = initISIM(socket_id);

    if (response == 1) {
        RLOGD("ISIM card, need send AT+IMSCOUNTCFG=1 to CP");
        at_send_command(socket_id, "AT+IMSCOUNTCFG=1", NULL);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
}

void notifySIMStatus(RIL_SOCKET_ID socket_id, void *data, RIL_Token t) {
    RIL_UNUSED_PARM(data);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0,
                              socket_id);
}

void requestSIMPower(RIL_SOCKET_ID socket_id, void *data, RIL_Token t) {
    int err = 0;
    int onOff = ((int *)data)[0];
    ATResponse *p_response = NULL;

    if (onOff == 0) {
        err = at_send_command(socket_id, "AT+SPDISABLESIM=1",
                              NULL);
        err = at_send_command(socket_id, "AT+SFUN=3",
                              &p_response);
    } else if (onOff > 0) {
        err = at_send_command(socket_id, "AT+SFUN=2",
                              &p_response);
    }
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    at_response_free(p_response);

    notifySIMStatus(socket_id, data, t);
    return;

error:
    at_response_free(p_response);
    if (t != NULL) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

#if 0  // unused function
static void setSIMPowerOff(void *param) {
    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
    requestSIMPower(socket_id, 0, NULL);
    pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
}
#endif

static void requestSIMGetAtr(RIL_SOCKET_ID socket_id, RIL_Token t) {
    int err;
    RIL_Errno errType = RIL_E_GENERIC_FAILURE;
    char *line = NULL;
    char *response = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+SPATR?",
                                     "+SPATR:", &p_response);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        if (!strcmp(p_response->finalResponse, "+CME ERROR: 20")) {
            errType = RIL_E_MISSING_RESOURCE;
        } else {
            errType = RIL_E_GENERIC_FAILURE;
        }
        goto error;
    }

    line = p_response->p_intermediates->line;

    if (at_tok_start(&line) < 0) goto error;
    if (at_tok_nextstr(&line, &response) < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, strlen(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, errType, NULL, 0);
    at_response_free(p_response);
}

static void getIMEIPassword(RIL_SOCKET_ID socket_id, char imeiPwd[]) {
    ATResponse *p_response = NULL;
    char password[15];
    int i = 0;
    int j = 0;
    int err = -1;
    char *line = NULL;
    if (socket_id != RIL_SOCKET_1) return;

    err = at_send_command_numeric(socket_id, "AT+CGSN",
            &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    if (strlen(line) != IMEI_LEN) goto error;
    while (*line != '\0') {
        if (i >= IMEI_LEN) break;
        password[i] = *line;
        line++;
        i++;
    }
    for (i = 0, j = 0; i < 14 && j <= 6; i += 2, j++) {
        imeiPwd[j] = (password[i] - 48 + password[i + 1] - 48) % 10 + '0';
    }
    imeiPwd[7] = password[0];
    imeiPwd[8] = '\0';
    at_response_free(p_response);
    return;
error:
    RLOGE(" get IMEI failed or IMEI is not rigth");
    at_response_free(p_response);
    return;
}

static void requestFacilityLockByUser(RIL_SOCKET_ID socket_id, char **data,
                                          size_t datalen, RIL_Token t) {
    ATResponse *p_response = NULL;
    char imeiPwd[9] = {0};
    int err;
    char *cmd = NULL, *line = NULL;
    int errNum = -1;
    int ret = -1;

    if (datalen != 2 * sizeof(char *)) {
        goto error1;
    }

    getIMEIPassword(socket_id, imeiPwd);

    ret = asprintf(&cmd, "AT+CLCK=\"%s\",%c,\"%s\"",
            data[0], *data[1], imeiPwd);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error1;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL && strStartsWith(p_response->finalResponse,
            "+CME ERROR:")) {
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err >= 0) {
            err = at_tok_nextint(&line, &errNum);
            if (err >= 0) {
                if (errNum == 11 || errNum == 12) {
                    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                              NULL, 0, socket_id);
                } else if (errNum == 70 || errNum == 128) {
                    RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
                    at_response_free(p_response);
                    return;
                } else if (errNum == 16) {
                    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
                    at_response_free(p_response);
                    return;
                }
            }
        }
    }
error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetSimLockStatus(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                        RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    char *cmd, *line;
    int err, skip, status;
    int ret = -1;

    int fac = ((int *)data)[0];

    ret = asprintf(&cmd, "AT+SPSMPN=%d,1", fac);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }

    err = at_send_command_singleline(socket_id, cmd, "+SPSMPN:",
                                         &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &status);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &status, sizeof(status));
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}

static void requestGetSimLockDummys(RIL_SOCKET_ID socket_id, RIL_Token t) {
    ATResponse *p_response = NULL;
    int err = -1, i = 0;
    char *line = NULL;
    int dummy[8] = {0};
    err = at_send_command_singleline(socket_id, "AT+SPSLDUM?",
                                         "+SPSLDUM:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    for (i = 0; i < 8; i++) {
        err = at_tok_nextint(&line, &dummy[i]);
        if (err < 0) goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &dummy, 8 * sizeof(int));
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}

static void fillAndCatPlmn(char *plmn, char *mcc, char *mnc, int mncLen) {
    // add for test sim card:mcc=001
    int mccLen = strlen(mcc);
    if (mccLen == 1) {
        strncpy(plmn, "00", strlen("00") + 1);
    } else if (mccLen == 2) {
        strncpy(plmn, "0", strlen("0") + 1);
    }
    strncat(plmn, mcc, mccLen);

    int toFillMncLen = mncLen - strlen(mnc);
    if (toFillMncLen == 1) {
        strncat(plmn, "0", strlen("0"));
    } else if (toFillMncLen == 2) {
        strncat(plmn, "00", strlen("00"));
    }
    strncat(plmn, mnc, strlen(mnc));
}

static void catSimLockWhiteListString(char *whitelist, int whitelistLen,
                                          char **parts, int partRow) {
    int totalLen = 0;
    int i;

    for (i = 0; i < partRow; i++) {
        if (parts[i] == NULL) {
            RLOGE("catSimLockWhiteListString: parts[%d] is NULL!", i);
            break;
        }
        if (strlen(parts[i]) == 0) {
            break;
        }
        totalLen += strlen(parts[i]);
        if (whitelistLen < totalLen) {
            RLOGE("catSimLockWhiteListString overlay!");
            return;
        }
        if (i > 0) {
            strncat(whitelist, ",", strlen(","));
            totalLen++;
        }
        strncat(whitelist, parts[i], strlen(parts[i]));
    }
    RLOGD("catSimLockWhiteListString whitelist=[%s]", whitelist);
}

static void requestGetSimLockWhiteList(RIL_SOCKET_ID socket_id, void *data,
                                           size_t datalen, RIL_Token t) {
    ATResponse *p_response = NULL;
    int err, i;
    char *cmd = NULL, *line = NULL, *mcc = NULL, *mnc = NULL;
    char *whiteList = NULL, *type_ret = NULL, *numlocks_ret = NULL;
    int ret = -1;
    int row = 0;
    int type, type_back, numlocks, mnc_digit;

    if (datalen != 1 * sizeof(int)) {
        goto error;
    }

    type = ((int *)data)[0];

    ret = asprintf(&cmd, "AT+SPSMNW=%d,\"%s\",%d", type, "12345678", 1);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }
    err = at_send_command_singleline(socket_id, cmd, "+SPSMNW:",
                                         &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &type_ret);
    if (err < 0) goto error;
    type_back = atoi(type_ret);

    err = at_tok_nextstr(&line, &numlocks_ret);
    if (err < 0 ) goto error;

    numlocks = atoi(numlocks_ret);
    if (numlocks < 0) goto error;

    int whiteListLen = sizeof(char) * (numlocks * WHITE_LIST_PS_PART_LENGTH
                    + WHITE_LIST_HEAD_LENGTH);
    // 3 is according to PC, 2 is head for fixing type_ret & numlocks_ret
    int partsRow = 3 * numlocks + 2;

    char **parts = NULL;
    parts = (char **)alloca(sizeof(char *) * partsRow);
    for (i = 0; i < partsRow; i++) {
        parts[i] = (char *)alloca(sizeof(char) * WHITE_LIST_COLUMN);
        memset(parts[i], 0, sizeof(char) * WHITE_LIST_COLUMN);
    }

    whiteList = (char *)alloca(whiteListLen);
    memset(whiteList, 0, whiteListLen);

    memcpy(parts[row], type_ret, strlen(type_ret));
    memcpy(parts[++row], numlocks_ret, strlen(numlocks_ret));

    switch (type_back) {
        case REQUEST_SIMLOCK_WHITE_LIST_PS: {
            char *imsi_len, *tmpImsi, *fixedImsi;
            int imsi_index;

            fixedImsi = (char *)alloca(sizeof(char) * SMALL_IMSI_LEN);

            for (i = 0; i < numlocks; i++) {
                err = at_tok_nextstr(&line, &imsi_len);
                if (err < 0) goto error;
                strncat(parts[++row], imsi_len, strlen(imsi_len));
                row++;

                for (imsi_index = 0; imsi_index < IMSI_VAL_NUM; imsi_index++) {
                    err = at_tok_nextstr(&line, &tmpImsi);
                    if (err < 0) goto error;

                    memset(fixedImsi, 0, sizeof(char) * SMALL_IMSI_LEN);

                    int len = strlen(tmpImsi);
                    if (len == 0) {
                        strncpy(fixedImsi, "00", strlen("00") + 1);
                    } else if (len == 1) {
                        strncpy(fixedImsi, "0", strlen("0") + 1);
                    }
                    strncat(fixedImsi, tmpImsi, len);
                    strncat(parts[row], fixedImsi, strlen(fixedImsi));
                }
            }
            break;
        }

        case REQUEST_SIMLOCK_WHITE_LIST_PN: {
            for (i = 0; i < numlocks; i++) {
                err = at_tok_nextstr(&line, &mcc);
                if (err < 0) goto error;
                err = at_tok_nextstr(&line, &mnc);
                if (err < 0) goto error;
                err = at_tok_nextint(&line, &mnc_digit);
                if (err < 0) goto error;
                fillAndCatPlmn(parts[++row], mcc, mnc, mnc_digit);
            }
            break;
        }

        case REQUEST_SIMLOCK_WHITE_LIST_PU: {
            char *network_subset1, *network_subset2;

            for (i = 0; i < numlocks; i++) {
                err = at_tok_nextstr(&line, &mcc);
                if (err < 0) goto error;
                err = at_tok_nextstr(&line, &mnc);
                if (err < 0) goto error;
                err = at_tok_nextint(&line, &mnc_digit);
                if (err < 0) goto error;
                fillAndCatPlmn(parts[++row], mcc, mnc, mnc_digit);

                err = at_tok_nextstr(&line, &network_subset1);
                if (err < 0) goto error;
                strncat(parts[row], network_subset1, strlen(network_subset1));

                err = at_tok_nextstr(&line, &network_subset2);
                if (err < 0) goto error;
                strncat(parts[row], network_subset2, strlen(network_subset2));
            }
            break;
        }

        case REQUEST_SIMLOCK_WHITE_LIST_PP: {
            char *gid1;

            for (i = 0; i < numlocks; i++) {
                err = at_tok_nextstr(&line, &mcc);
                if (err < 0) goto error;
                err = at_tok_nextstr(&line, &mnc);
                if (err < 0) goto error;
                err = at_tok_nextint(&line, &mnc_digit);
                if (err < 0) goto error;
                fillAndCatPlmn(parts[++row], mcc, mnc, mnc_digit);

                err = at_tok_nextstr(&line, &gid1);
                if (err < 0) goto error;
                strncat(parts[++row], gid1, strlen(gid1));
            }
            break;
        }

        case REQUEST_SIMLOCK_WHITE_LIST_PC: {
            char *gid1, *gid2;

            for (i = 0; i < numlocks; i++) {
                err = at_tok_nextstr(&line, &mcc);
                if (err < 0) goto error;
                err = at_tok_nextstr(&line, &mnc);
                if (err < 0) goto error;
                err = at_tok_nextint(&line, &mnc_digit);
                if (err < 0) goto error;
                fillAndCatPlmn(parts[++row], mcc, mnc, mnc_digit);

                err = at_tok_nextstr(&line, &gid1);
                if (err < 0) goto error;
                strncat(parts[++row], gid1, strlen(gid1));

                err = at_tok_nextstr(&line, &gid2);
                if (err < 0) goto error;
                strncat(parts[++row], gid2, strlen(gid2));
            }
            break;
        }

        default:
            goto error;
            break;
    }
    catSimLockWhiteListString(whiteList, whiteListLen, parts, partsRow);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, whiteList, strlen(whiteList) + 1);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}

void getConfigSlotStatus(RIL_SOCKET_ID socket_id, RIL_SimSlotStatus_V1_2 *pSimSlotStatus) {
    if (pSimSlotStatus == NULL) {
        return;
    }
    getSIMStatus(-1, socket_id);
    pSimSlotStatus->base.cardState = s_isSimPresent[socket_id];

    // TODO: slot state is always active now
    pSimSlotStatus->base.slotState = SLOT_STATE_ACTIVE;

    if (s_isSimPresent[socket_id] != SIM_ABSENT) {
        pSimSlotStatus->base.atr = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getSimAtr(socket_id, pSimSlotStatus->base.atr, AT_COMMAND_LEN);

        pSimSlotStatus->base.iccid = (char *)calloc(AT_COMMAND_LEN, sizeof(char));
        getIccId(socket_id, pSimSlotStatus->base.iccid, AT_COMMAND_LEN);
    }

    pSimSlotStatus->base.logicalSlotId = socket_id;
    pSimSlotStatus->eid = "";
}

void getConfigSlotStatusList(RIL_SimSlotStatus_V1_2 **pSimSlotStatusList) {
    if (pSimSlotStatusList == NULL || *pSimSlotStatusList == NULL) {
        return;
    }
    RIL_SimSlotStatus_V1_2 *simSlotStatusList = *pSimSlotStatusList;
    char slotMapping[ARRAY_SIZE] = {0};
    char slotMappingDefault[ARRAY_SIZE] = {0};
    for (int socketId = 0; socketId < RIL_SOCKET_NUM; socketId++) {
        RLOGD("getConfigSlotStatusList[%d] in %d", socketId, RIL_SOCKET_NUM);
        snprintf(slotMappingDefault, sizeof(slotMappingDefault), "%d", socketId);
        getProperty(socketId, SIM_SLOT_MAPPING_PROP, slotMapping, slotMappingDefault);
        RIL_SOCKET_ID PhysicalSlotId = atoi(slotMapping);
        getConfigSlotStatus(PhysicalSlotId, &simSlotStatusList[socketId]);
    }
}

void onIccSlotStatus(RIL_Token t) {
    RIL_SOCKET_ID socket_id = RIL_SOCKET_1;
    RIL_SimSlotStatus_V1_2 *pSimSlotStatusList =
                (RIL_SimSlotStatus_V1_2 *)calloc(RIL_SOCKET_NUM, sizeof(RIL_SimSlotStatus_V1_2));

    getConfigSlotStatusList(&pSimSlotStatusList);

    if (t == NULL) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_CONFIG_ICC_SLOT_STATUS, pSimSlotStatusList,
                 (RIL_SOCKET_NUM) * sizeof(RIL_SimSlotStatus_V1_2), socket_id);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, pSimSlotStatusList,
                (RIL_SOCKET_NUM) * sizeof(RIL_SimSlotStatus_V1_2));
    }

    if (pSimSlotStatusList != NULL) {
        for (socket_id = RIL_SOCKET_1; socket_id < RIL_SOCKET_NUM; socket_id++) {
            free(pSimSlotStatusList[socket_id].base.atr);
            free(pSimSlotStatusList[socket_id].base.iccid);
        }
        free(pSimSlotStatusList);
    }
}

int processSimRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id) {
    int err;
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_GET_SIM_STATUS:
        case RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS: {
            RIL_CardStatus_v1_4 *p_card_status;
            char *p_buffer;
            int buffer_size;
            sem_wait(&(s_sem[socket_id]));
            int result;
            // To support C2K multi app card status
            if (s_isModemSupportCDMA) {
                result = getCardStatusC2K(request, socket_id, &p_card_status);
            } else {
                result = getCardStatus(request, socket_id, &p_card_status);
            }

            sem_post(&(s_sem[socket_id]));

            if (result == RIL_E_SUCCESS) {
                p_buffer = (char *)p_card_status;
                buffer_size = sizeof(*p_card_status);
            } else {
                p_buffer = NULL;
                buffer_size = 0;
            }
            RIL_onRequestComplete(t, result, p_buffer, buffer_size);
            freeCardStatus(p_card_status);
            break;
        }
        case RIL_REQUEST_ENTER_SIM_PIN :
        case RIL_REQUEST_ENTER_SIM_PUK :
            requestEnterSimPin(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_ENTER_SIM_PIN2 :
            requestEnterSimPin2(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_ENTER_SIM_PUK2 :
            requestEnterSimPuk2(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CHANGE_SIM_PIN :
            requestChangeSimPin(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CHANGE_SIM_PIN2 :
            requestChangeSimPin2(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION : {
            if (s_isSimPresent[socket_id] == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_INVALID_SIM_STATE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_GET_IMSI: {
            p_response = NULL;
            err = at_send_command_numeric(socket_id, "AT+CIMI",
                                          &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS,
                        p_response->p_intermediates->line,
                        strlen(p_response->p_intermediates->line) + 1);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_FACILITY_LOCK:
        case RIL_EXT_REQUEST_QUERY_FACILITY_LOCK: {
            char *lockData[4];
            lockData[0] = ((char **)data)[0];
            lockData[1] = FACILITY_LOCK_REQUEST;
            lockData[2] = ((char **)data)[1];
            lockData[3] = ((char **)data)[2];
            requestFacilityLock(request, socket_id, lockData,
                                datalen + sizeof(char *), t);
            break;
        }
        case RIL_REQUEST_SET_FACILITY_LOCK:
            requestFacilityLock(request, socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
            requestTransmitApduBasic(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SIM_OPEN_CHANNEL:
            requestOpenLogicalChannel(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SIM_CLOSE_CHANNEL:
            requestCloseLogicalChannel(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
            requestTransmitApdu(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SIM_AUTHENTICATION: {
            RIL_SimAuthentication *sim_auth = (RIL_SimAuthentication *)data;
            RLOGD("RIL_REQUEST_SIM_AUTHENTICATION authContext = %d,"
                  "rand_autn = %s, aid = %s", sim_auth->authContext,
                  sim_auth->authData, sim_auth->aid);
            if (sim_auth->authContext == AUTH_CONTEXT_EAP_AKA &&
                sim_auth->authData != NULL) {
                requestUSimAuthentication(socket_id, sim_auth->authData, t);
            } else if (sim_auth->authContext == AUTH_CONTEXT_EAP_SIM &&
                       sim_auth->authData != NULL) {
                requestSimAuthentication(socket_id, sim_auth->authData, t);
            } else {
                RLOGE("invalid authContext");
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            }
            break;
        }
        case RIL_EXT_REQUEST_GET_SIMLOCK_REMAIN_TIMES: {
            int fac = ((int *)data)[0];
            int result = getNetLockRemainTimes(socket_id, fac);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &result, sizeof(int));
            break;
        }
        case RIL_EXT_REQUEST_SET_FACILITY_LOCK_FOR_USER:
            requestFacilityLockByUser(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_GET_SIMLOCK_STATUS:
            requestGetSimLockStatus(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_GET_SIMLOCK_DUMMYS:
            requestGetSimLockDummys(socket_id, t);
            break;
        case RIL_EXT_REQUEST_GET_SIMLOCK_WHITE_LIST:
            requestGetSimLockWhiteList(socket_id, data, datalen, t);
            break;
        /* IMS request @{ */
        case RIL_EXT_REQUEST_INIT_ISIM:
            requestInitISIM(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_ENABLE_IMS: {
            char cmd[AT_COMMAND_LEN] = {0};
            snprintf(cmd, sizeof(cmd), "AT+IMSEN=%d", ((int *)data)[0]);
            err = at_send_command(socket_id, cmd, NULL);
            if (err < 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        case RIL_EXT_REQUEST_SIMMGR_SIM_POWER:
            notifySIMStatus(socket_id, data, t);
            break;
        /* }@ */
        case RIL_EXT_REQUEST_SIM_GET_ATR:
            requestSIMGetAtr(socket_id, t);
            break;
        case RIL_EXT_REQUEST_SIM_POWER_REAL:
        case RIL_REQUEST_SET_SIM_CARD_POWER: {
            pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
            requestSIMPower(socket_id, data, t);
            pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
            break;
        }
        case RIL_EXT_REQUEST_GET_SUBSIDYLOCK_STATUS: {
            p_response = NULL;
            char *line = NULL;
            int result = 0;
            err = at_send_command_singleline(socket_id, "AT+SPSLENABLED?",
                    "+SPSLENABLED:", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                line = p_response->p_intermediates->line;
                err = at_tok_start(&line);
                if (err == 0) {
                    err = at_tok_nextint(&line, &result);
                }
                RIL_onRequestComplete(t, RIL_E_SUCCESS, &result, sizeof(result));
            }
            at_response_free(p_response);
            break;
        }
        /* UNISOC CDMA only support RUIM as subscription source. @{ */
        case RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE: {
            if (s_isModemSupportCDMA) {
                int subscription_source = CDMA_SUBSCRIPTION_SOURCE_RUIM_SIM;
                RIL_onRequestComplete(t, RIL_E_SUCCESS, &subscription_source, sizeof(int));
            } else {
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            }
            break;
        }
        /* @} */
        case RIL_REQUEST_CONFIG_GET_SLOT_STATUS: {
            RIL_requestTimedCallback(onIccSlotStatus, (void *)t, NULL);
            break;
        }
        case RIL_REQUEST_CONFIG_SET_SLOT_MAPPING: {
            char slotMap[ARRAY_SIZE] = {0};
            char workMode[ARRAY_SIZE] = {0};
#if (SIM_COUNT == 2)
            snprintf(slotMap, sizeof(slotMap), "%d,%d", ((int *)data)[RIL_SOCKET_1],
                    ((int *)data)[RIL_SOCKET_2]);
            property_set(SIM_SLOT_MAPPING_PROP, slotMap);
#else
            snprintf(slotMap, sizeof(slotMap), "%d,-1", ((int *)data)[RIL_SOCKET_1]);
            property_set(SIM_SLOT_MAPPING_PROP, slotMap);
#endif
            saveDataCardProp(((int *)data)[s_multiModeSim]);
            snprintf(workMode, sizeof(workMode), "%d,%d",
                        s_workMode[((int *)data)[0]], s_workMode[((int *)data)[1]]);
            property_set(MODEM_WORKMODE_PROP, workMode);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            exit(-1);
            break;
        }
        case RIL_REQUEST_CONFIG_GET_PHONE_CAPABILITY: {
            RIL_PhoneCapability *phoneCapability =
                    (RIL_PhoneCapability *)alloca(sizeof(RIL_PhoneCapability));

            char phoneCount[PROPERTY_VALUE_MAX] = {0};
            property_get(PHONE_COUNT_PROP, phoneCount, "2");

            // L+L maxActiveData is 2, and others is 1
            if ((s_modemConfig == LWG_LWG) && !strcmp(phoneCount, "2")) {
                phoneCapability->maxActiveData = 2;
            } else {
                phoneCapability->maxActiveData = 1;
            }

            // DSDS is 1, and DSDA is 2, now only support DSDS
            phoneCapability->maxActiveInternetData = 1;

            // DSDA can support internet lingering
            phoneCapability->isInternetLingeringSupported = false;

            for (int num = 0; num < SIM_COUNT; num++) {
                phoneCapability->logicalModemList[num].modemId = num;
            }
            RIL_onRequestComplete(t, RIL_E_SUCCESS,
                            phoneCapability, sizeof(RIL_PhoneCapability));
            break;
        }
        case RIL_REQUEST_CONFIG_SET_MODEM_CONFIG: {
            RIL_ModemConfig *mdConfig = (RIL_ModemConfig *)data;
            char phoneCount[PROPERTY_VALUE_MAX] = {0};
            if (mdConfig->numOfLiveModems <= 0 || mdConfig->numOfLiveModems > SIM_COUNT) {
                RLOGE("CONFIG_SET_MODEM_CONFIG invalid argument");
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            } else {
                snprintf(phoneCount, sizeof(phoneCount), "%d", mdConfig->numOfLiveModems);
                property_set(PHONE_COUNT_PROP, phoneCount);
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_CONFIG_GET_MODEM_CONFIG: {
            RIL_ModemConfig *mdConfig =
                        (RIL_ModemConfig *)alloca(sizeof(RIL_ModemConfig));
            char phoneCount[PROPERTY_VALUE_MAX] = {0};
            property_get(PHONE_COUNT_PROP, phoneCount, "2");
            mdConfig->numOfLiveModems = atoi(phoneCount);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, mdConfig, sizeof(RIL_ModemConfig));
            break;
        }
        default:
            return 0;
    }

    return 1;
}

void onSimStatusChanged(RIL_SOCKET_ID socket_id, const char *s) {
    int err;
    int type;
    int value = 0, cause = -1;
    char *line = NULL;
    char *tmp;
    line = strdup(s);
    tmp = line;
    at_tok_start(&tmp);

    err = at_tok_nextint(&tmp, &type);
    if (err < 0) goto out;

    if (type == 3) {
        if (at_tok_hasmore(&tmp)) {
            err = at_tok_nextint(&tmp, &value);
            if (err < 0) goto out;
            if (value == 1) {
                if (at_tok_hasmore(&tmp)) {
                    err = at_tok_nextint(&tmp, &cause);
                    if (err < 0) goto out;
                    if (cause == 2 || cause == 34 || cause == 25 || cause == 1
                            || cause == 7) {
                        freeSimEcclist(socket_id);
                        s_imsInitISIM[socket_id] = -1;
                        RIL_onUnsolicitedResponse(
                                RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED, NULL,
                                0, socket_id);
                        RIL_onUnsolicitedResponse(
                                RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL,
                                0, socket_id);
                        // sim hot plug out and set stk to not enable
                        setStkServiceRunning(socket_id, false);
                    }
                }
            } else if (value == 0 || value == 2 || value == 100 || value == 4) {
                if (value == 4) {  // 4 is about simlock
                    s_imsInitISIM[socket_id] = -1;
                }
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                          NULL, 0, socket_id);
                RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED,
                                          NULL, 0, socket_id);
            }
        }
    }

out:
    free(line);
}

int processSimUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    int err;
    char *line = NULL;

    if (strStartsWith(s, "+ECIND:")) {
        onSimStatusChanged(socket_id, s);
    } else if (strStartsWith(s, "+SPEXPIRESIM:")) {
        int simID;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &simID);
        if (err < 0) goto out;

        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIMLOCK_SIM_EXPIRED, &simID,
                sizeof(simID), socket_id);
    } else if (strStartsWith(s, "+CLCK:")) {
        int response;
        char *tmp = NULL;
        char *type = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &type);
        if (err < 0) goto out;

        if (0 == strcmp(type, "FD")) {
            err = at_tok_nextint(&tmp, &response);
            if (err < 0) goto out;

            const char *cmd = "+CLCK:";
            RIL_Token t = NULL;
            void *data = NULL;

            onCompleteAsyncCmdMessage(socket_id, cmd, &t, &data);
            dispatchCLCK(t, data, (void *)(&response));
        }
    } else if (strStartsWith(s, "+SPSLENABLED:")) {
        int status = 0;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);
        if (err < 0)
            goto out;

        err = at_tok_nextint(&tmp, &status);
        if (err < 0) goto out;

        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SUBSIDYLOCK_STATUS_CHANGED,
                &status, sizeof(status), socket_id);
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}

/* AT Command [AT+CLCK="FD",....] used to enable/disable FDN facility.
 * But the AT response is async
 * the status of "+CLCK:"FD",status" URC is the real result
 * dispatchCLCK according the URC to complete the request
 */
void dispatchCLCK(RIL_Token t, void *data, void *resp) {
    if (t == NULL || data == NULL || resp == NULL) {
        return;
    }

    int mode = ((int *)data)[0];
    int status = ((int *)resp)[0];

    if (mode == status) {
        int result = 1;
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &result, sizeof(result));
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    free(data);
}
