/**
 * ril_network.c --- Network-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_network.h"
#include "ril_sim.h"
#include "ril_data.h"
#include "ril_misc.h"
#include "ril_call.h"
#include "channel_controller.h"
#include "utils.h"
#include "time.h"

/* Save physical cellID for AGPS */
//#define PHYSICAL_CELLID_PROP    "gsm.cell.physical_cellid"
/* Save NITZ operator name string for UI to display right PLMN name */
#define NITZ_OPERATOR_PROP      "vendor.ril.nitz.info"
#define FIXED_SLOT_PROP         "ro.vendor.radio.fixed_slot"
#define COPS_MODE_PROP          "persist.vendor.radio.copsmode"
/* set network type for engineer mode */
#define ENGTEST_ENABLE_PROP     "persist.vendor.radio.engtest.enable"
/* set the comb-register flag */
#define CEMODE_PROP             "persist.vendor.radio.cemode"
/* Save SA tac and cellID */
#define SAINFO_PROP             "persist.vendor.radio.sainfo"

RIL_RegState s_PSRegStateDetail[SIM_COUNT] = {
        RIL_UNKNOWN
#if (SIM_COUNT >= 2)
        ,RIL_UNKNOWN
#if (SIM_COUNT >= 3)
        ,RIL_UNKNOWN
#if (SIM_COUNT >= 4)
        ,RIL_UNKNOWN
#endif
#endif
#endif
        };
LTE_PS_REG_STATE s_PSRegState[SIM_COUNT] = {
        STATE_OUT_OF_SERVICE
#if (SIM_COUNT >= 2)
        ,STATE_OUT_OF_SERVICE
#if (SIM_COUNT >= 3)
        ,STATE_OUT_OF_SERVICE
#if (SIM_COUNT >= 4)
        ,STATE_OUT_OF_SERVICE
#endif
#endif
#endif
        };
SimBusy s_simBusy[SIM_COUNT] = {
        {false, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER}
#if (SIM_COUNT >= 2)
       ,{false, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER}
#endif
#if (SIM_COUNT >= 3)
       ,{false, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER}
#endif
#if (SIM_COUNT >= 4)
       ,{false, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER}
#endif
        };
static pthread_mutex_t s_workModeMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t s_LTEAttachMutex[SIM_COUNT] = {
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
pthread_mutex_t s_radioPowerMutex[SIM_COUNT] = {
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
pthread_mutex_t s_operatorInfoListMutex[SIM_COUNT] = {
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
pthread_mutex_t s_operatorXmlInfoListMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t s_operatorInfoMutex = PTHREAD_MUTEX_INITIALIZER;

int s_imsRegistered[SIM_COUNT];  // 0 == unregistered
int s_imsBearerEstablished[SIM_COUNT];
int s_in4G[SIM_COUNT];
int s_in2G[SIM_COUNT] = {0};
int s_workMode[SIM_COUNT] = {0};
int s_desiredRadioState[SIM_COUNT] = {0};
int s_requestSetRC[SIM_COUNT] = {0};
int s_sessionId[SIM_COUNT] = {0};
int s_radioAccessFamily[SIM_COUNT] = {0};
int s_presentSIMCount = 0;
bool s_isScanningNetwork = false;
bool s_setSignalStrengthReporting = false;
static bool s_radioOnError[SIM_COUNT];  // 0 -- false, 1 -- true
static char s_nitzOperatorInfo[SIM_COUNT][ARRAY_SIZE];
OperatorInfoList s_operatorInfoList[SIM_COUNT];
OperatorInfoList s_operatorXmlInfoList;
RIL_SOCKET_ID s_multiModeSim = RIL_SOCKET_1;
bool s_isCesqNewVersion = false;

int s_psOpened[SIM_COUNT] = {0};
int s_rxlev[SIM_COUNT], s_ber[SIM_COUNT], s_rscp[SIM_COUNT];
int s_ecno[SIM_COUNT], s_rsrq[SIM_COUNT], s_rsrp[SIM_COUNT];
int s_ss_rsrq[SIM_COUNT], s_ss_rsrp[SIM_COUNT], s_ss_sinr[SIM_COUNT];

bool s_nrStatusNotRestricted[SIM_COUNT];
bool s_nrStatusConnected[SIM_COUNT];
bool s_sigConnStatusWait[SIM_COUNT];
int s_cancelSerial[SIM_COUNT] = {-1};
int s_lastSigConnStatus[SIM_COUNT] = {-1};
int s_lastNrCfgInfo[SIM_COUNT] = {-1};
int s_nrCfgInfo[SIM_COUNT][2];
RIL_SingnalConnStatus s_sigConnStatus[SIM_COUNT];

long int s_rxBytes = 0;
long int s_txBytes = 0;
pthread_mutex_t s_usbSharedMutex = PTHREAD_MUTEX_INITIALIZER;
int s_UsbShareFlag = 512;  // for usb Shared flag

// for Nr Switch
unsigned int s_NrSwitchStatus = {0xDF};  // Nr Switch default open
pthread_mutex_t s_nrSwitchStatusMutex = PTHREAD_MUTEX_INITIALIZER;

int s_smart5GEnable = 0;

void setWorkMode();
void initWorkMode();

void onModemReset_Network() {
    RIL_SOCKET_ID socket_id  = 0;

    for (socket_id = RIL_SOCKET_1; socket_id < RIL_SOCKET_NUM; socket_id++) {
        s_PSRegStateDetail[socket_id] = RIL_UNKNOWN;
        s_PSRegState[socket_id] = STATE_OUT_OF_SERVICE;
        s_in4G[socket_id] = 0;
        s_in2G[socket_id] = 0;
        s_imsRegistered[socket_id] = 0;

        // signal process related
        s_psOpened[socket_id] = 0;
        s_rxlev[socket_id] = 0;
        s_ber[socket_id] = 0;
        s_rscp[socket_id] = 0;
        s_ecno[socket_id] = 0;
        s_rsrq[socket_id] = 0;
        s_rsrp[socket_id] = 0;
        s_ss_rsrq[socket_id] = 0;
        s_ss_rsrp[socket_id] = 0;
        s_ss_sinr[socket_id] = 0;

        // Clear for nr state
        s_nrStatusNotRestricted[socket_id] = false;
        s_sigConnStatusWait[socket_id] = false;
        s_nrStatusConnected[socket_id] = false;
        s_lastSigConnStatus[socket_id] = -1;
        s_lastNrCfgInfo[socket_id] = -1;
    }
}

//Type: 1 G_PHONE;  2 C_PHONE; 0 unkown
void setPhoneType(int type, RIL_SOCKET_ID socket_id) {
    if (type == 2) {
        s_isCDMAPhone[socket_id] = true;
    } else {
        s_isCDMAPhone[socket_id] = false;
    }

    RLOGD("setPhoneType = %d, s_isCDMAPhone[%d] = %d", type, socket_id, s_isCDMAPhone[socket_id]);
}

// for L+W product
bool isPrimaryCardWorkMode(int workMode) {
    if (workMode == GSM_ONLY || workMode == WCDMA_ONLY ||
        workMode == WCDMA_AND_GSM || workMode == TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM ||
        workMode == NONE) {
        return false;
    }
    return true;
}

bool isNRWorkMode(int workMode) {
    if (workMode == NR_ONLY || workMode == NR_AND_TD_LTE_AND_LTE_FDD ||
        workMode == NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM) {
        return true;
    }
    return false;
}

void initPrimarySim() {
    char prop[PROPERTY_VALUE_MAX] = {0};

    property_get(PRIMARY_SIM_PROP, prop, "0");
    s_multiModeSim = atoi(prop);
    RLOGD("before initPrimarySim: s_multiModeSim = %d", s_multiModeSim);

    initWorkMode();

#if (SIM_COUNT == 2)
    char numToStr[ARRAY_SIZE] = {0};
    RIL_SOCKET_ID simId = RIL_SOCKET_1;

    property_get(MODEM_CONFIG_PROP, prop, "");
    if (s_modemConfig == LWG_G || s_modemConfig == WG_G ||
        s_modemConfig == LG_G) {
        for (simId = RIL_SOCKET_1; simId < RIL_SOCKET_NUM; simId++) {
            if (s_workMode[simId] == 10 && s_multiModeSim == simId) {
                s_multiModeSim = 1 - simId;
                snprintf(numToStr, sizeof(numToStr), "%d", s_multiModeSim);
                property_set(PRIMARY_SIM_PROP, numToStr);
            }
        }
    } else if (s_modemConfig == LWG_WG) {
        for (simId = RIL_SOCKET_1; simId < RIL_SOCKET_NUM; simId++) {
            if (!isPrimaryCardWorkMode(s_workMode[simId]) && s_multiModeSim == simId) {
                s_multiModeSim = 1 - simId;
                snprintf(numToStr, sizeof(numToStr), "%d", s_multiModeSim);
                property_set(PRIMARY_SIM_PROP, numToStr);
            }
        }
    } else if (s_modemConfig == NRLWG_LWG) {
        for (simId = RIL_SOCKET_1; simId < RIL_SOCKET_NUM; simId++) {
            if (isNRWorkMode(s_workMode[simId]) && s_multiModeSim != simId) {
                s_multiModeSim = simId;
                snprintf(numToStr, sizeof(numToStr), "%d", s_multiModeSim);
                property_set(PRIMARY_SIM_PROP, numToStr);
            }
        }
    }
#endif
    RLOGD("after initPrimarySim : s_multiModeSim = %d", s_multiModeSim);
}

int getMultiMode() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    int workMode = 0;
    property_get(MODEM_CONFIG_PROP, prop, "");
    if (strcmp(prop, "") == 0) {
        // WCDMA
        workMode = WCDMA_AND_GSM;
    } else if (strstr(prop, "TL_LF_TD_W_G")) {
        workMode = TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM;
    } else if (strstr(prop, "TL_LF_W_G")) {
        workMode = TD_LTE_AND_LTE_FDD_AND_W_AND_GSM;
    } else if (strstr(prop, "TL_TD_G")) {
        workMode = TD_LTE_AND_TD_AND_GSM;
    } else if (strcmp(prop, "W_G,G") == 0) {
        workMode = WCDMA_AND_GSM;
    } else if (strcmp(prop, "W_G,W_G") == 0) {
        workMode = PRIMARY_WCDMA_AND_GSM;
    } else if (strcmp(prop, "TL_LF_G,G") == 0) {
        workMode = TD_LTE_AND_LTE_FDD_AND_GSM;
    } else if (strstr(prop, "TL_LF_TD_W_C_G")) {
        workMode = TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
    } else if (strstr(prop, "TD_W_C_G")) {
        workMode = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
    } else if (strstr(prop, "NR_TL_LF_W_G")) {
        workMode = NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM;
    }
    return workMode;
}

void initWorkMode() {
    int workMode[SIM_COUNT];
    char prop[PROPERTY_VALUE_MAX] = {0};
    char numToStr[ARRAY_SIZE] = {0};
    RIL_SOCKET_ID simId = RIL_SOCKET_1;

    pthread_mutex_lock(&s_workModeMutex);

    for (simId = RIL_SOCKET_1; simId < SIM_COUNT; simId++) {
        memset(prop, 0, sizeof(prop));
        getProperty(simId, MODEM_WORKMODE_PROP, prop, "10");
        s_workMode[simId] = atoi(prop);
        workMode[simId] = s_workMode[simId];
    }

#if (SIM_COUNT >= 2)
    if (s_modemConfig == LWG_G || s_modemConfig == WG_G ||
        s_modemConfig == LG_G) {
        if (workMode[RIL_SOCKET_1] != GSM_ONLY &&
            workMode[RIL_SOCKET_2] != GSM_ONLY) {
            RLOGD("initWorkMode: change the work mode to 10");
            if (RIL_SOCKET_1 == s_multiModeSim) {
                workMode[RIL_SOCKET_2] = GSM_ONLY;
            } else {
                workMode[RIL_SOCKET_1] = GSM_ONLY;
            }
        }
    } else if (s_modemConfig == LWG_WG) {
        if (isPrimaryCardWorkMode(workMode[RIL_SOCKET_1]) &&
            isPrimaryCardWorkMode(workMode[RIL_SOCKET_2])) {
           RLOGD("initWorkMode: change the work mode to 255");
           if (RIL_SOCKET_1 == s_multiModeSim) {
               workMode[RIL_SOCKET_2] = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
           } else {
               workMode[RIL_SOCKET_1] = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
           }
       }
    }
    if (workMode[RIL_SOCKET_1] != s_workMode[RIL_SOCKET_1] ||
        workMode[RIL_SOCKET_2] != s_workMode[RIL_SOCKET_2]) {
        snprintf(numToStr, sizeof(numToStr), "%d,%d", workMode[RIL_SOCKET_1],
                 workMode[RIL_SOCKET_2]);
        RLOGD("initWorkMode: %s", numToStr);
        s_workMode[RIL_SOCKET_1] = workMode[RIL_SOCKET_1];
        s_workMode[RIL_SOCKET_2] = workMode[RIL_SOCKET_2];
        property_set(MODEM_WORKMODE_PROP, numToStr);
    }
#else
    if (workMode[RIL_SOCKET_1] != s_workMode[RIL_SOCKET_1]) {
        snprintf(numToStr, sizeof(numToStr), "%d,10", workMode[RIL_SOCKET_1]);
        RLOGD("initWorkMode: %s", numToStr);
        s_workMode[RIL_SOCKET_1] = workMode[RIL_SOCKET_1];
        property_set(MODEM_WORKMODE_PROP, numToStr);
    }
#endif

    pthread_mutex_unlock(&s_workModeMutex);
}

void buildWorkModeCmd(char *cmd, size_t size) {
    int simId;
    char strFormatter[AT_COMMAND_LEN] = {0};

    for (simId = 0; simId < SIM_COUNT; simId++) {
        if (simId == 0) {
            snprintf(cmd, size, "AT+SPTESTMODEM=%d", s_workMode[simId]);
            if (SIM_COUNT == 1) {
                strncpy(strFormatter, cmd, size);
                strncat(strFormatter, ",%d", strlen(",%d"));
                snprintf(cmd, size, strFormatter, GSM_ONLY);
            }
        } else {
            strncpy(strFormatter, cmd, size);
            strncat(strFormatter, ",%d", strlen(",%d"));
            snprintf(cmd, size, strFormatter, s_workMode[simId]);
        }
    }
    if (s_modemConfig == NRLWG_LWG) {
        snprintf(cmd + strlen(cmd), size - strlen(cmd), ",%d", s_multiModeSim);
    }
}

void initSmartNr(RIL_SOCKET_ID socket_id) {
    int err = -1;
    int enable = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id,
            "AT+SPLASDUMMY=\"get power optimization params\"", "+SPLASDUMMY:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("AT+SPLASDUMMY=\"get power optimization params\" return error");
        goto out;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto out;

    skipNextComma(&line);
    err = at_tok_nextint(&line, &enable);
    if (err < 0) {
        RLOGE("parse enable failed");
        goto out;
    }
    RLOGD("initSmartNr enable=%d", enable);

out:
    at_response_free(p_response);
    if (enable == 0) {
        property_set(MODEM_SMART_NR_PROP, "false");
    } else {
        property_set(MODEM_SMART_NR_PROP, "true");
    }
}

static int mapCgregResponse(int in_response) {
    int out_response = 0;

    switch (in_response) {
        case 0:
            out_response = RADIO_TECH_GPRS;    /* GPRS */
            break;
        case 3:
            out_response = RADIO_TECH_EDGE;    /* EDGE */
            break;
        case 2:
            out_response = RADIO_TECH_UMTS;    /* TD */
            break;
        case 4:
            out_response = RADIO_TECH_HSDPA;    /* HSDPA */
            break;
        case 5:
            out_response = RADIO_TECH_HSUPA;   /* HSUPA */
            break;
        case 6:
            out_response = RADIO_TECH_HSPA;   /* HSPA */
            break;
        case 15:
            out_response = RADIO_TECH_HSPAP;   /* HSPA+ */
            break;
        case 7:
        case 13:
            out_response = RADIO_TECH_LTE;   /* LTE */
            break;
        case 16:
            out_response = RADIO_TECH_LTE_CA;   /* LTE_CA */
            break;
        case 10:
        case 11:
            out_response = RADIO_TECH_NR;    /* NR */
            break;
        default:
            out_response = RADIO_TECH_UNKNOWN;    /* UNKNOWN */
            break;
    }
    return out_response;
}

static int mapRegState(int inResponse) {
    int outResponse = RIL_UNKNOWN;

    switch (inResponse) {
        case 0:
            outResponse = RIL_NOT_REG_AND_NOT_SEARCHING;
            break;
        case 1:
            outResponse = RIL_REG_HOME;
            break;
        case 2:
            outResponse = RIL_NOT_REG_AND_SEARCHING;
            break;
        case 3:
            outResponse = RIL_REG_DENIED;
            break;
        case 4:
            outResponse = RIL_UNKNOWN;
            break;
        case 5:
            outResponse = RIL_REG_ROAMING;
            break;
        case 8:
        case 10:
            outResponse = RIL_NOT_REG_AND_EMERGENCY_AVAILABLE_AND_NOT_SEARCHING;
            break;
        case 12:
            outResponse = RIL_NOT_REG_AND_EMERGENCY_AVAILABLE_AND_SEARCHING;
            break;
        case 13:
            outResponse = RIL_REG_DENIED_AND_EMERGENCY_AVAILABLE;
            break;
        case 14:
            outResponse = RIL_UNKNOWN_AND_EMERGENCY_AVAILABLE;
            break;
        default:
            outResponse = RIL_UNKNOWN;
            break;
    }
    return outResponse;
}

static int convertToRrequencyRange(int range) {
    int out_response = 0;
    switch (range) {
        case 1:
            out_response = FREQUENCY_RANGE_LOW;
            break;
        case 2:
            out_response = FREQUENCY_RANGE_MID;
            break;
        case 3:
            out_response = FREQUENCY_RANGE_HIGH;
            break;
        case 4:
            out_response = FREQUENCY_RANGE_MMWAVE;
            break;
        default:
            out_response = FREQUENCY_RANGE_LOW;
            break;
    }
    return out_response;
}

/* 1 explain that CS/PS domain is 4G */
int is4G(int urcNetType, int mapNetType, RIL_SOCKET_ID socketId) {
    if (urcNetType == 7 || urcNetType == 16 || urcNetType == 13) {
        s_in4G[socketId] = 1;
    } else if (mapNetType == 14 || mapNetType == 19) {
        s_in4G[socketId] = 1;
    } else {
        s_in4G[socketId] = 0;
    }
    return s_in4G[socketId];
}

/* Calculation the number of one-bits in input parameter */
int bitCount(int rat) {
    int num = 0;
    while (rat) {
        if (rat % 2 == 1) {
            num++;
        }
        rat = rat / 2;
    }
    return num;
}

int isMaxRat(int rat) {
    int simId = 0, bitNum = 0, maxBitValue = -1;
    for (simId = 0; simId < SIM_COUNT; simId++) {
        bitNum = bitCount(s_radioAccessFamily[simId]);
        if (bitNum > maxBitValue) {
            maxBitValue = bitNum;
        }
    }
    return (bitCount(rat) == maxBitValue ? 1 : 0);
}

static bool isEscapeCharacter(char character) {
    char escapeCharacter[] = {'\a', '\b', '\f', '\n', '\r', '\t', '\v', '\'', '\\'};
    int size = sizeof(escapeCharacter) / sizeof(char);
    int i = 0;
    bool ret = false;

    for (i = 0; i < size; i++) {
        if (character == escapeCharacter[i]) {
            ret = true;
            break;
        }
    }

    return ret;
}

static void changeEscapeCharacterToSpace(char *data, int dataLen) {
    int i = 0;

    for (i = 0; i < dataLen; i++) {
        if (isEscapeCharacter(data[i])) {
            data[i] = ' ';
        }
    }
}

static void requestSignalStrength(RIL_SOCKET_ID socket_id, void *data,
                                  size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    char *line = NULL;
    int response[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    ATResponse *p_response = NULL;
    ATResponse *p_newResponse = NULL;
    RIL_SignalStrength_v1_4 responseV1_4;

    RIL_SIGNALSTRENGTH_INIT_1_4(responseV1_4);

    err = at_send_command_singleline(socket_id, "AT+CESQ",
                                     "+CESQ:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    cesq_execute_cmd_rsp(socket_id, p_response, &p_newResponse);
    if (p_newResponse == NULL) goto error;

    line = p_newResponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[2]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[3]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[4]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[5]);
    if (err < 0) goto error;

    if (s_modemConfig == NRLWG_LWG) {
        err = at_tok_nextint(&line, &response[6]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[7]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[8]);
        if (err < 0) goto error;
    }

    if (!s_isCDMAPhone[socket_id]) {
        if (response[0] != -1 && response[0] != 99 && response[0] != 255) {
            responseV1_4.gsm.signalStrength = response[0];
            s_rxlev[socket_id] = response[0];
        }
        if (response[2] != -1 && response[2] != 255) {  // response[2] is cp reported 3G value
            int dBm = convert3GValueTodBm(response[2]);
            responseV1_4.wcdma.signalStrength = getWcdmaSigStrengthBydBm(dBm);
            responseV1_4.wcdma.rscp = getWcdmaRscpBydBm(dBm);
            responseV1_4.wcdma.bitErrorRate = response[1];
            responseV1_4.wcdma.ecno = response[3];
            s_rscp[socket_id] = response[2];
        }
        if (response[7] != -1 && response[7] != 255 && response[7] != -255) {
            responseV1_4.nr.ssRsrp = response[7];
        }
    } else {
        if (response[0] != -1 && response[0] != 255) {
            responseV1_4.cdma.dbm = response[0];
            s_rxlev[socket_id] = response[0];
        }
        if (response[1] != -1 && response[1] != 255) {
            responseV1_4.cdma.ecio = response[1];
            s_ber[socket_id] = response[1];
        }
        if (response[2] != -1 && response[2] != 255) {
            responseV1_4.evdo.dbm = response[2];
            s_rscp[socket_id] = response[2];
        }
        if (response[3] != -1 && response[3] != 255) {
            responseV1_4.evdo.signalNoiseRatio = response[3];
            s_ecno[socket_id] = response[3];
        }
    }
    if (response[5] != -1 && response[5] != 255 && response[5] != -255) {
        responseV1_4.lte.rsrp = response[5];
        s_rsrp[socket_id] = response[5];
    }

    triggerSignalProcess();
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &responseV1_4,
                          sizeof(RIL_SignalStrength_v1_4));

    at_response_free(p_response);
    at_response_free(p_newResponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    at_response_free(p_newResponse);
}

static void setCEMODE(RIL_SOCKET_ID socket_id) {
    int err = -1;
    int cemode = 1;
    int response = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    char cemodeProp[PROPERTY_VALUE_MAX] = {0};

    property_get(CEMODE_PROP, cemodeProp, "");
    if (strcmp(cemodeProp, "")) {
        cemode = atoi(cemodeProp);
    } else {
        /**
        * +CEUS: <n>
        * 0:Voice centric
        * 1:Data centric
        **/
        err = at_send_command_singleline(socket_id, "AT+CEUS?",
                                         "+CEUS:", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &response);
        if (err < 0) goto error;

        if (0 == response) {
            cemode = 1;
        } else if (1 == response) {
            cemode = 2;
        }
error:
        at_response_free(p_response);
    }

    snprintf(cmd, sizeof(cmd), "AT+CEMODE=%d", cemode);
    at_send_command(socket_id, cmd, NULL);
    return;
}

static void queryCeregTac(RIL_SOCKET_ID socket_id, int *tac) {
    int err = -1;
    int commas, skip;
    int response[2] = {-1, -1};
    char *p = NULL;
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id,
            "AT+CEREG?", "+CEREG:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    commas = 0;
    for (p = line; *p != '\0'; p++) {
        if (*p == ',') commas++;
    }
    // +CEREG: <n>,<stat>[,[<tac>,[<ci>],[<AcT>]]{,<cause_type>,<reject_cause>]}
    if (commas >= 2) {
        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[1]);
        if (err < 0) goto error;
    }

    if (response[1] != -1) {
        RLOGD("CEREG tac = %x", response[1]);
        *tac = response[1];
    }

error:
    at_response_free(p_response);
}

static void processRequestTimerDelay(void *param) {
    RLOGE("processRequestTimerDelay");
    if (param == NULL) {
        RLOGE("param is NULL");
        return;
    }
    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    RLOGD("need to unsol when time is out! %d", socket_id);
    s_sigConnStatusWait[socket_id] = false;
    RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIGNAL_CONN_STATUS,
            &s_sigConnStatus[socket_id], sizeof(s_sigConnStatus[socket_id]), socket_id);
    RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_NR_CFG_INFO,
            &s_nrCfgInfo[socket_id], sizeof(int) * 2, socket_id);
}

static void cancelTimerNRStateChanged(RIL_SOCKET_ID socket_id) {
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }
    RLOGD("need to cancel timer when not in 4/5g or 5g go back! %d", socket_id);
    s_sigConnStatusWait[socket_id] = false;
    RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIGNAL_CONN_STATUS,
            &s_sigConnStatus[socket_id], sizeof(s_sigConnStatus[socket_id]), socket_id);
    RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_NR_CFG_INFO,
            &s_nrCfgInfo[socket_id], sizeof(int) * 2, socket_id);
}

static void cancelTimerOperation(RIL_SOCKET_ID socket_id) {
    if (s_sigConnStatusWait[socket_id]) {
        removeTimedMessage(s_cancelSerial[socket_id]);
        RLOGD("cancelTimerOperation: s_cancelSerial[%d], %d",
                socket_id, s_cancelSerial[socket_id]);
        cancelTimerNRStateChanged(socket_id);
    }
}

static void queryNrIndicators(RIL_SOCKET_ID socket_id, int *endcAvailable,
        int *dcNrRestricted, int *nrAvailable) {
    int err = -1;
    int response[3] = {-1, -1, -1};
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id,
           "AT+SP5GCMDS=\"get nr indicators\"", "+SP5GCMDS:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    skipNextComma(&line);
    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response[2]);
    if (err < 0) goto error;

    RLOGD("NrIndicators endcAvailable = %d, dcNrRestricted = %d, nrAvailable = %d",
            response[0], response[1], response[2]);
    *endcAvailable = response[0];
    *dcNrRestricted = response[1];
    *nrAvailable = response[2];

error:
    at_response_free(p_response);
}

static void setSaInfoProp(int tac, char *sa_cellid) {
    char *endptr = NULL;
    long cellid = 0;
    char prop[PROPERTY_VALUE_MAX] = {0};
    char numToStr[ARRAY_SIZE] = {0};
    property_get(SAINFO_PROP, prop, "0,0");

    cellid = strtol(sa_cellid, &endptr, 16);
    snprintf(numToStr, sizeof(numToStr), "%d,%ld", tac, cellid);
    RLOGD("tac and cellid of SA is: %s", numToStr);

    if (!strcmp(prop, numToStr)) {
        RLOGD("The current SA info is the same as last time");
    } else {
        property_set(SAINFO_PROP, numToStr);
    }
}

static void requestRegistrationState(RIL_SOCKET_ID socket_id, int request,
                                     void *data, size_t datalen,
                                     RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    int commas, skip;
    int response[7] = {-1, -1, -1, -1, -1, -1, -1};
    int spcainfo[2] = {-1, -1};
    char *responseStr[15] = {NULL};
    char res[8][20] = {};
    char *line, *p, *cid = NULL, *tmpLine = NULL;
    const char *cmd;
    const char *prefix;
    ATResponse *p_response = NULL;
    ATResponse *tp_response = NULL;

    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE ||
        request == RIL_REQUEST_VOICE_RADIO_TECH) {
        cmd = "AT+CREG?";
        prefix = "+CREG:";
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        if (s_isSA[socket_id]) {
            cmd = "AT+C5GREG?";
            prefix = "+C5GREG:";
        } else {
            cmd = "AT+CEREG?";
            prefix = "+CEREG:";
        }
    } else if (request == RIL_REQUEST_IMS_REGISTRATION_STATE) {
        cmd = "AT+CIREG?";
        prefix = "+CIREG:";
    }  else {
        assert(0);
        goto error;
    }

    err = at_send_command_singleline(socket_id, cmd, prefix,
                                     &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line; *p != '\0'; p++) {
        if (*p == ',') commas++;
    }

    switch (commas) {
        case 0: {  /* +CREG: <stat> */
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            break;
        }
        case 1: {  /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            break;
        }
        case 2: {
            // +CREG: <stat>, <lac>, <cid>
            // or+CIREG: <n>, <reg_info>, [<ext_info>]
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            break;
        }
        case 3: {  /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            break;
        }
        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4: {  /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[3]);
            if (err < 0) goto error;
            break;
        }
        case 5: {  /* +CEREG: <n>, <stat>, <lac>, <rac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[3]);
            if (err < 0) goto error;
            break;
        }
        case 6: {  /* +C5GREG: <n>, <stat>, <tac>, <ci>, <AcT>, <Allowed_NSSAI_length>, <Allowed_NSSAI> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]);
            if (err < 0) goto error;
            err = at_tok_nextstr(&line, &cid);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[3]);
            if (err < 0) goto error;
            setSaInfoProp(response[2], cid);
            break;
        }
        default:
            goto error;
    }

    int regState = mapRegState(response[0]);

    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        s_PSRegStateDetail[socket_id] = regState;
    }

    if (8 == response[0]) {
        response[0] = RIL_NOT_REG_AND_EMERGENCY_AVAILABLE_AND_NOT_SEARCHING;
    }
    snprintf(res[0], sizeof(res[0]), "%d", response[0]);
    responseStr[0] = res[0];

    if (response[1] != -1) {
        snprintf(res[1], sizeof(res[1]), "%x", response[1]);
        responseStr[1] = res[1];
    }

    if (response[2] != -1) {
        snprintf(res[2], sizeof(res[2]), "%x", response[2]);
        responseStr[2] = res[2];
    }

    RLOGD("s_workMode[%d] = %d", socket_id, s_workMode[socket_id]);
    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        if (response[3] == 13) {
            queryNrIndicators(socket_id, &(response[4]), &(response[5]), &(response[6]));
            s_nrStatusNotRestricted[socket_id] = true;
            if (s_sigConnStatus[socket_id].mode == 1) {
                err = at_send_command_singleline(socket_id,
                        "AT+SPCAINFO?", "+SPCAINFO", &tp_response);
                if (!(err != 0 || tp_response->success == 0)) {
                    tmpLine = tp_response->p_intermediates->line;
                    do {
                        err = at_tok_start(&tmpLine);
                        if (err < 0) break;
                        err = at_tok_nextint(&tmpLine, &spcainfo[0]);
                        if (err < 0) break;
                        err = at_tok_nextint(&tmpLine, &spcainfo[1]);
                        if (err < 0) break;
                        if (spcainfo[0] == 7 && spcainfo[1] == 1) {
                           response[3] = 16;
                        }
                    } while (0);
                }
                at_response_free(tp_response);
            }
        } else {
            s_nrStatusNotRestricted[socket_id] = false;
        }
    }

    if (response[3] != -1) {
        response[3] = mapCgregResponse(response[3]);
        /* STK case27.22.4.27.2.8 :if the command is rejected because
         * the class B terminal(Register State is 2g) is busy on a call
         */
        if (response[3] == RADIO_TECH_GPRS
                || response[3] == RADIO_TECH_EDGE) {
            s_in2G[socket_id] = 1;
        }
        snprintf(res[3], sizeof(res[3]), "%d", response[3]);
        responseStr[3] = res[3];
    }

    if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        if (response[0] == 1 || response[0] == 5) {
            pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
            if (s_PSRegState[socket_id] == STATE_OUT_OF_SERVICE) {
                s_PSRegState[socket_id] = STATE_IN_SERVICE;
            }
            pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            is4G(-1, response[3], socket_id);
        } else {
            pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
            if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                s_PSRegState[socket_id] = STATE_OUT_OF_SERVICE;
            }
            pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            s_in4G[socket_id] = 0;
        }
    }

    // in 2/3g or no service,need to cancel timeout
    if (!s_in4G[socket_id] && response[3] != RADIO_TECH_NR) {
        cancelTimerOperation(socket_id);
    }

    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        snprintf(res[4], sizeof(res[4]), "0");
        responseStr[7] = res[4];
        if (s_in4G[socket_id]) {
            queryCeregTac(socket_id, &(response[1]));
            snprintf(res[1], sizeof(res[1]), "%x", response[1]);
            responseStr[1] = res[1];
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                              15 * sizeof(char *));
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        snprintf(res[4], sizeof(res[4]), "3");
        responseStr[5] = res[4];
        /* UNISOC add for 5G reg report */
        snprintf(res[5], sizeof(res[5]), "%d", response[4]);
        responseStr[9] = res[5];
        snprintf(res[6], sizeof(res[6]), "%d", response[5]);
        responseStr[10] = res[6];
        snprintf(res[7], sizeof(res[7]), "%d", response[6]);
        responseStr[11] = res[7];
        RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                              12 * sizeof(char *));
    } else if (request == RIL_REQUEST_IMS_REGISTRATION_STATE) {
        s_imsRegistered[socket_id] = response[1];
        RLOGD("imsRegistered[%d] = %d", socket_id, s_imsRegistered[socket_id]);
        int imsResp[2] = {0, 0};
        imsResp[0] = response[1];
        imsResp[1] = response[2];
        RIL_onRequestComplete(t, RIL_E_SUCCESS, imsResp, sizeof(imsResp));
    } else if (request == RIL_REQUEST_VOICE_RADIO_TECH) {
        RIL_RadioAccessFamily rAFamliy = RAF_UNKNOWN;
        if (response[3] != -1) {
            rAFamliy = 1 << response[3];
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &rAFamliy, sizeof(int));
    }
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int mapCcregResponse(int in_response) {
    int out_response = 0;

    switch (in_response) {
        case 1:
            out_response = RADIO_TECH_1xRTT;
            break;
        case 2:
            out_response = RADIO_TECH_EVDO_A;
            break;
        case 3:
            out_response = RADIO_TECH_EHRPD;
            break;
        default:
            out_response = RADIO_TECH_UNKNOWN;    /* UNKNOWN */
            break;
    }

    return out_response;
}

static void requestRegistrationStateCDMA(RIL_SOCKET_ID socket_id, int request,
                                         void *data, size_t datalen,
                                         RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    int skip;
    int response[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    char *responseStr[15] = {NULL};
    char res[10][20];
    char *line = NULL;
    const char *cmd = NULL;
    const char *prefix = NULL;
    ATResponse *p_response = NULL;

    if (request == RIL_REQUEST_VOICE_REGISTRATION_STATE) {
        cmd = "AT+CCREG?";
        prefix = "+CCREG:";

        err = at_send_command_singleline(socket_id, cmd, prefix,
                                     &p_response);
        if (err != 0 || p_response->success == 0) {
            goto error;
        }

        line = p_response->p_intermediates->line;

        /* +CCREG: <mode>, <state> [,<Latitude>,<Longitude>,<CSS Indicator>,
         *         <Roaming indicator>, < networkId >,< systemId >,
         *         < baseStationId >,[,<Available data radio technology>]]
         */

        err = at_tok_start(&line);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[1]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[2]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[3]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[4]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[5]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[6]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[7]);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[8]);
        if (err < 0) goto error;

        // int regState = mapRegState(response[0]);

        if (8 == response[0]) {//state
            response[0] = RIL_NOT_REG_AND_EMERGENCY_AVAILABLE_AND_NOT_SEARCHING;
        }

        snprintf(res[0], sizeof(res[0]), "%d", response[0]);
        responseStr[0] = res[0];

        if (response[8] != -1) {//Available data radio technology
            response[8] = mapCcregResponse(response[8]);

            // Out of service, set RAT to unknown
            if (response[0] == 0 || response[0] == 2) {
                RLOGD("OOS set RAT to Unknown.");
                response[8] = RADIO_TECH_UNKNOWN;
            }

            snprintf(res[1], sizeof(res[1]), "%d", response[8]);
            responseStr[3] = res[1];
        }

        if (response[3] != -1) {//CSS Indicator
            snprintf(res[2], sizeof(res[2]), "%d", response[3]);
            responseStr[7] = res[2];
        }

        if (response[4] != -1) {//Roaming indicator
            if (response[8] == RADIO_TECH_UNKNOWN) {
                // Bug1197701 CDMA CS no service, PS in service, set Roaming Indicator to OFF
                // to avoid Data reg state changed to Roaming.
                RLOGD("Set CDMA Roaming Indicator to OFF when CS RAT is unknown.");
                snprintf(res[3], sizeof(res[3]), "%d", 1);
            } else {
                snprintf(res[3], sizeof(res[3]), "%d", response[4]);
            }
            responseStr[10] = res[3];
        }
        if (response[5] != -1) {//networkId
            snprintf(res[4], sizeof(res[4]), "%d", response[5]);
            responseStr[9] = res[4];
        }

        if (response[6] != -1) {//systemId
            snprintf(res[5], sizeof(res[5]), "%d", response[6]);
            responseStr[8] = res[5];
        }

        if (response[7] != -1) {//basestationId
            snprintf(res[6], sizeof(res[6]), "%d", response[7]);
            responseStr[4] = res[6];
        }

        if (response[2] != -1) {//longitude
            snprintf(res[7], sizeof(res[7]), "%d", response[2]);
            responseStr[6] = res[7];
        }

        if (response[1] != -1) {//latitude
            snprintf(res[8], sizeof(res[8]), "%d", response[1]);
            responseStr[5] = res[8];
        }

        // TODO:
        // Add to remove roaming icon.
        // To be removed when modem can report correct data
        responseStr[11] = "1";

        RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                              15 * sizeof(char *));
    } else if (request == RIL_REQUEST_DATA_REGISTRATION_STATE) {
        cmd = "AT+CEREG?";
        prefix = "+CEREG:";

        err = at_send_command_singleline(socket_id, cmd, prefix,
                                     &p_response);
        if (err != 0 || p_response->success == 0) {
            goto error;
        }

        // If register on LTE(7)/LTE_CA(16), reuse AT+CEREG,
        // else it is on CDMA and use AT+CCGREG.
        line = p_response->p_intermediates->line;
        line = strrchr(line, ',');
        line = line + 1;
        if ((!strcmp(line, "7")) || (!strcmp(line, "16"))) {
            requestRegistrationState(socket_id, request, data, datalen, t);
        } else {// register on cdma
            cmd = "AT+CCGREG?";
            prefix = "+CCGREG:";

            err = at_send_command_singleline(socket_id, cmd, prefix,
                                     &p_response);
            if (err != 0 || p_response->success == 0) {
                goto error;
            }

            line = p_response->p_intermediates->line;

            err = at_tok_start(&line);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[0]);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[1]);
            if (err < 0) goto error;

            int regState = mapRegState(response[0]);
            s_PSRegStateDetail[socket_id] = regState;

            if (8 == response[0]) {//state
                response[0] = RIL_NOT_REG_AND_EMERGENCY_AVAILABLE_AND_NOT_SEARCHING;
            }

            snprintf(res[0], sizeof(res[0]), "%d", response[0]);
            responseStr[0] = res[0];

            if (response[1] != -1) {//Available data radio technology
                response[1] = mapCcregResponse(response[1]);

                // Out of service, set RAT to unknown
                if (response[0] == 0 || response[0] == 2) {
                    RLOGD("OOS set RAT to Unknown.");
                    response[1] = RADIO_TECH_UNKNOWN;
                }

                snprintf(res[1], sizeof(res[1]), "%d", response[1]);
                responseStr[3] = res[1];
            }

            if (response[0] == 1 || response[0] == 5) {
                pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
                if (s_PSRegState[socket_id] == STATE_OUT_OF_SERVICE) {
                    s_PSRegState[socket_id] = STATE_IN_SERVICE;
                }
                pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            } else {
                pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
                if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                    s_PSRegState[socket_id] = STATE_OUT_OF_SERVICE;
                }
                pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            }

            s_in4G[socket_id] = 0;//register on cdma which is 2G/3g
            RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                                  6 * sizeof(char *));
        }
    }  else {
        goto error;
    }

    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int getOperatorName(char *longName, char *shortName, char *plmn,
                           OperatorInfoList *optList,
                           pthread_mutex_t *optListMutex) {
    if (plmn == NULL || longName == NULL || shortName == NULL) {
        return -1;
    }

    int ret = -1;
    MUTEX_ACQUIRE(*optListMutex);
    OperatorInfoList *pList = optList->next;
    OperatorInfoList *next;
    while (pList != NULL && pList != optList) {
        next = pList->next;
        if (pList->plmn == NULL) {
            RLOGD("getOperatorName plmn is NULL");
            break;
        }
        if (strcmp(plmn, pList->plmn) == 0) {
            memcpy(longName, pList->longName, strlen(pList->longName) + 1);
            memcpy(shortName, pList->shortName, strlen(pList->shortName) + 1);
            ret = 0;
            break;
        }
        pList = next;
    }
    MUTEX_RELEASE(*optListMutex);
    return ret;
}

static void addToOperatorInfoList(char *longName, char *shortName, char *plmn,
                                  OperatorInfoList *optList,
                                  pthread_mutex_t *optListMutex) {
    if (plmn == NULL || longName == NULL || shortName == NULL) {
        return;
    }

    MUTEX_ACQUIRE(*optListMutex);

    OperatorInfoList *pList = optList->next;
    OperatorInfoList *next;
    while (pList != optList) {
        next = pList->next;
        if (strcmp(plmn, pList->plmn) == 0) {
            RLOGD("addToOperatorInfoList: had add this operator before");
            goto exit;
        }
        pList = next;
    }

    int plmnLen = strlen(plmn) + 1;
    int lnLen = strlen(longName) + 1;
    int snLen = strlen(shortName) + 1;

    OperatorInfoList *pNode =
            (OperatorInfoList *)calloc(1, sizeof(OperatorInfoList));
    pNode->plmn = (char *)calloc(plmnLen, sizeof(char));
    pNode->longName = (char *)calloc(lnLen, sizeof(char));
    pNode->shortName = (char *)calloc(snLen, sizeof(char));

    memcpy(pNode->plmn, plmn, plmnLen);
    memcpy(pNode->longName, longName, lnLen);
    memcpy(pNode->shortName, shortName, snLen);

    OperatorInfoList *pHead = optList;
    pNode->next = pHead;
    pNode->prev = pHead->prev;
    pHead->prev->next = pNode;
    pHead->prev = pNode;
exit:
    MUTEX_RELEASE(*optListMutex);
}

static void requestOperator(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                            RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err, i, skip;
    int ret = -1;
    char *response[3];
    char longName[ARRAY_SIZE] = {0};
    char shortName[ARRAY_SIZE] = {0};
    char longNameTmp[ARRAY_SIZE] = {0};
    char shortNameTmp[ARRAY_SIZE] = {0};
    char newLongName[ARRAY_SIZE] = {0};
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;

    memset(response, 0, sizeof(response));

    err = at_send_command_multiline(socket_id,
            "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?", "+COPS:",
            &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */
    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next, i++) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        /* If we're unregistered, we may just get
         * a "+COPS: 0" response
         */
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        /* a "+COPS: 0, n" response is also possible */
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

#if defined (RIL_EXTENSION)
    //prevent no long name or short name in NITZ and numeric_operator.xml
    if (response[0] != NULL && strcmp(response[0], "")) {
        snprintf(longName, sizeof(longName), "%s", response[0]);
    }
    if (response[1] != NULL && strcmp(response[1], "")) {
        snprintf(shortName, sizeof(shortName), "%s", response[1]);
    }
    if (response[2] != NULL) {
        if (strcmp(s_nitzOperatorInfo[socket_id], "")) {
            ret = matchOperatorInfo(longName, shortName, response[2],
                                    s_nitzOperatorInfo[socket_id]);
        }
        if (ret != 0) {
            pthread_mutex_lock(&s_operatorInfoMutex);
            ret = getOperatorName(longName, shortName, response[2],
                                  &s_operatorXmlInfoList,
                                  &s_operatorXmlInfoListMutex);
            if (ret != 0) {
                ret = RIL_getONS(longName, shortName, response[2]);
                if (0 == ret && response[1] != NULL && strcmp(response[1], "")) {
                    addToOperatorInfoList(longName, shortName, response[2],
                                          &s_operatorXmlInfoList,
                                          &s_operatorXmlInfoListMutex);
                }
            }
            pthread_mutex_unlock(&s_operatorInfoMutex);
        }
        if (0 == ret) {
            response[0] = longName;
            response[1] = shortName;
            RLOGD("get Operator longName: %s, shortName: %s", response[0], response[1]);
        }

        ret = getOperatorName(longNameTmp, shortNameTmp, response[2],
                              &s_operatorInfoList[socket_id],
                              &s_operatorInfoListMutex[socket_id]);
        if (ret != 0) {
            ret = updatePlmn(socket_id, -1, (const char *)(response[2]),
                             newLongName, sizeof(newLongName));
            if (ret == 0 && strcmp(newLongName, "")) {
                RLOGD("updated plmn name = %s", newLongName);
                response[0] = newLongName;
                response[1] = newLongName;
                addToOperatorInfoList(newLongName, newLongName, response[2],
                                      &s_operatorInfoList[socket_id],
                                      &s_operatorInfoListMutex[socket_id]);
            }
        } else if (strcmp(longNameTmp, "")) {
            response[0] = longNameTmp;
            response[1] = shortNameTmp;
        }
        RLOGD("get Operator longName = %s, shortName = %s", response[0], response[1]);
    }
#endif

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int getRadioFeatures(RIL_SOCKET_ID socket_id) {
    int rat = 0;
    const int WCDMA = RAF_HSDPA | RAF_HSUPA | RAF_HSPA | RAF_HSPAP | RAF_UMTS;
    const int GSM = RAF_GSM | RAF_GPRS | RAF_EDGE;
    const int LTE = RAF_LTE | RAF_LTE_CA;
    int workMode = 0;

    if (s_modemConfig == NRLWG_LWG) {
        if (socket_id == s_multiModeSim) { //TODO: Need to handle s_multiModeSim when 5G dual-sim device is coming.
           rat = RAF_NR | RAF_LTE | WCDMA | GSM;
        } else {
           rat = RAF_LTE | WCDMA | GSM;
        }
    } else if (s_modemConfig == LWG_WG) {
        if (socket_id == s_multiModeSim) {
            rat = LTE | WCDMA | GSM;
        } else {
            rat = WCDMA | GSM;
        }
    } else if (s_modemConfig == LWG_LWG) {
        rat = LTE | WCDMA | GSM;
    } else if (s_modemConfig == WG_WG) {
        rat = WCDMA | GSM;
    } else if (s_modemConfig == LG_G) {
        if (socket_id == s_multiModeSim) {
            rat = LTE | GSM;
        } else {
            rat = GSM;
        }
    } else if (s_modemConfig == WG_G) {
        if (socket_id == s_multiModeSim) {
            rat = WCDMA | GSM;
        } else {
            rat = GSM;
        }
    } else if (s_modemConfig == LWG_G ) {
        if (socket_id == s_multiModeSim) {
            rat = LTE | WCDMA | GSM;
        } else {
            rat = GSM;
        }
    } else {
        workMode = s_workMode[socket_id];
        if (workMode == GSM_ONLY) {
            rat = GSM;
        } else if (workMode == NONE) {
            if (isSimPresent(socket_id)) {
                rat = GSM;
            } else {
                rat = RAF_UNKNOWN;
            }
        } else {
            rat = LTE | WCDMA | GSM;
        }
    }
    RLOGD("getRadioFeatures rat %d", rat);
    return rat;
}

static void sendUnsolRadioCapability() {
    RIL_RadioCapability *responseRc = (RIL_RadioCapability *)malloc(
             sizeof(RIL_RadioCapability));
    memset(responseRc, 0, sizeof(RIL_RadioCapability));
    responseRc->version = RIL_RADIO_CAPABILITY_VERSION;
    responseRc->session = s_sessionId[s_multiModeSim];
    responseRc->phase = RC_PHASE_UNSOL_RSP;
    responseRc->rat = getRadioFeatures(s_multiModeSim);
    responseRc->status = RC_STATUS_SUCCESS;
    strncpy(responseRc->logicalModemUuid, "com.unisoc.modem_multiMode",
            sizeof("com.unisoc.modem_multiMode"));
    RIL_onUnsolicitedResponse(RIL_UNSOL_RADIO_CAPABILITY, responseRc,
            sizeof(RIL_RadioCapability), s_multiModeSim);
    s_sessionId[s_multiModeSim] = 0;
#if (SIM_COUNT == 2)
    RIL_SOCKET_ID singleModeSim = RIL_SOCKET_1;
    if (s_multiModeSim == RIL_SOCKET_1) {
        singleModeSim = RIL_SOCKET_2;
    } else if (s_multiModeSim == RIL_SOCKET_2) {
        singleModeSim = RIL_SOCKET_1;
    }
    responseRc->session = s_sessionId[singleModeSim];
    responseRc->rat = getRadioFeatures(singleModeSim);
    strncpy(responseRc->logicalModemUuid, "com.unisoc.modem_singleMode",
            sizeof("com.unisoc.modem_singleMode"));
    RIL_onUnsolicitedResponse(RIL_UNSOL_RADIO_CAPABILITY, responseRc,
            sizeof(RIL_RadioCapability), singleModeSim);
    s_sessionId[singleModeSim] = 0;
#endif
    free(responseRc);
}

static void requestRadioPower(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                              RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, i, rc;
    int radioState = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    char simEnabledProp[PROPERTY_VALUE_MAX] = {0};
    char manualAttachProp[PROPERTY_VALUE_MAX] = {0};
    char modemEnabledProp[PROPERTY_VALUE_MAX] = {0};
    char smartNr[PROPERTY_VALUE_MAX] = {0};
    struct timespec timeout;
    ATResponse *p_response = NULL;

    assert(datalen >= sizeof(int *));
    radioState = ((int *)data)[0];

    getProperty(socket_id, MODEM_ENABLED_PROP, modemEnabledProp, "1");

    if (radioState == 0) {
        getSIMStatus(false, socket_id);
        /* The system ask to shutdown the radio */
        err = at_send_command(socket_id,
                "AT+SFUN=5", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        for (i = 0; i < MAX_PDP; i++) {
            if (s_dataAllowed[socket_id] && s_PDP[socket_id][i].state == PDP_BUSY) {
                RLOGD("s_PDP[%d].state = %d", i, s_PDP[socket_id][i].state);
                putPDP(socket_id, i);
            }
        }
        setRadioState(socket_id, RADIO_STATE_OFF);
    } else if (radioState > 0 && s_radioState[socket_id] == RADIO_STATE_OFF &&
                strcmp(modemEnabledProp, "1") == 0) {
        getSIMStatus(false, socket_id);
        if (s_simBusy[socket_id].s_sim_busy) {
            RLOGD("SIM is busy now, wait for CPIN status!");
            clock_gettime(CLOCK_MONOTONIC, &timeout);
            timeout.tv_sec += 8;
            pthread_mutex_lock(&s_simBusy[socket_id].s_sim_busy_mutex);
            rc = pthread_cond_timedwait(&s_simBusy[socket_id].s_sim_busy_cond,
                                &s_simBusy[socket_id].s_sim_busy_mutex,
                                &timeout);
            if (rc == ETIMEDOUT) {
                RLOGD("stop waiting when time is out!");
            } else {
                RLOGD("CPIN is OK now, do it!");
            }
            pthread_mutex_unlock(&s_simBusy[socket_id].s_sim_busy_mutex);
        }
        initSIMPresentState();
        getProperty(socket_id, SIM_ENABLED_PROP, simEnabledProp, "1");
        if (strcmp(simEnabledProp, "0") == 0) {
            RLOGE("sim enable false,radio power on failed");
            goto error;
        }

        initWorkMode();

        // Claro Test APN private failed
        property_get(LTE_MANUAL_ATTACH_PROP, manualAttachProp, "0");
        RLOGD("network : persist.radio.manual.attach: %s", manualAttachProp);
        if (!strcmp(manualAttachProp, "1") && s_isFirstPowerOn[socket_id]
                && s_isFirstSetAttach[socket_id]
                && (s_roModemConfig == LWG_LWG || socket_id == s_multiModeSim)) {
            at_send_command(socket_id, "AT+SPMANUATTACH=1", NULL);
            s_isFirstPowerOn[socket_id] = false;
        }

        buildWorkModeCmd(cmd, sizeof(cmd));

#if (SIM_COUNT == 2)
        if (s_presentSIMCount == 0) {
            if (s_simBusy[1 - socket_id].s_sim_busy) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                          NULL, 0, socket_id);
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                return;
            } else {
                getProperty(1 - socket_id, SIM_ENABLED_PROP, simEnabledProp, "1");
                if (socket_id != s_multiModeSim && !strcmp(simEnabledProp, "1")) {
                    if (s_radioState[1 - socket_id] == RADIO_STATE_OFF ||
                            s_radioState[1 - socket_id] == RADIO_STATE_UNAVAILABLE) {
                        int *data = (int *)calloc(1, sizeof(int));
                        data[0] = 1;
                        onRequest(RIL_REQUEST_RADIO_POWER, data, sizeof(int), NULL, 1 - socket_id);
                    }
                    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                              NULL, 0, socket_id);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                    return;
                }
            }
        } else if (s_presentSIMCount == 1) {
            if (isSimPresent(socket_id) == 0) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                          NULL, 0, socket_id);
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                return;
            }
        }
#endif

        err = at_send_command(socket_id, cmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            if (p_response != NULL &&
                    strcmp(p_response->finalResponse, "+CME ERROR: 15") == 0) {
                RLOGE("set wrong workmode in cmcc version");
            #if (SIM_COUNT == 2)
                if (s_multiModeSim == RIL_SOCKET_1) {
                    s_multiModeSim = RIL_SOCKET_2;
                } else if (s_multiModeSim == RIL_SOCKET_2) {
                    s_multiModeSim = RIL_SOCKET_1;
                }
            #endif
                setWorkMode();
                buildWorkModeCmd(cmd, sizeof(cmd));
                at_send_command(socket_id, cmd, NULL);
            }
        }
        AT_RESPONSE_FREE(p_response);

        /* UNISOC:Modify for Bug1254912,set CEMODE according to Engineering mode */
        setCEMODE(socket_id);

        if (s_roModemConfig >= LWG_LWG) {
            RLOGD("socket_id = %d, s_multiModeSim = %d", socket_id,
                    s_multiModeSim);
            if (socket_id == s_multiModeSim) {
                snprintf(cmd, sizeof(cmd), "AT+SPSWDATA");
                at_send_command(socket_id, cmd, NULL);
            }
        } else {
            if (s_presentSIMCount == 1 && socket_id == s_multiModeSim) {
                if (!strcmp(manualAttachProp, "0")) {
                    err = at_send_command(socket_id,
                            "AT+SAUTOATT=1", &p_response);
                    if (err < 0 || p_response->success == 0) {
                        RLOGE("GPRS auto attach failed!");
                    }
                    AT_RESPONSE_FREE(p_response);
                }
            }
#if defined (ANDROID_MULTI_SIM)
            else {
                if (socket_id != s_multiModeSim) {
                    RLOGD("socket_id = %d, s_dataAllowed = %d", socket_id,
                            s_dataAllowed[socket_id]);
                    if (s_modemConfig == WG_WG) {
                        RLOGD("switch data card according to allow data");
                        snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,%d", socket_id,s_dataAllowed[socket_id]);
                        at_send_command(socket_id, cmd, NULL);
                        if (s_dataAllowed[socket_id] == 1) {
                            at_send_command(socket_id, "AT+SAUTOATT=1",NULL);
                        }
                    } else {
                        snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,0", socket_id);
                        at_send_command(socket_id, cmd, NULL);
                    }
                }
            }
#endif
        }

        err = at_send_command(socket_id, "AT+SFUN=4",
                              &p_response);
        if (err < 0|| p_response->success == 0) {
            /* Some stacks return an error when there is no SIM,
             * but they really turn the RF portion on
             * So, if we get an error, let's check to see if it
             * turned on anyway
             */
            if (isRadioOn(socket_id) != 1) {
                goto error;
            }

            if (err == AT_ERROR_TIMEOUT) {
                s_radioOnError[socket_id] = true;
                RLOGD("requestRadioPower: radio on ERROR");
                goto error;
            }
        }
        setRadioState(socket_id, RADIO_STATE_ON);
        property_get(MODEM_SMART_NR_PROP, smartNr, "");
        RLOGD("requestRadioPower: smartNr: %s", smartNr);
        if (s_isNR && (strcmp(smartNr, "") == 0)) {
            initSmartNr(socket_id);
        }
    }

    at_response_free(p_response);
    if (t != NULL) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    return;

error:
    at_response_free(p_response);
    if (t != NULL) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

static void requestEnableModem(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                               RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int isOn = *((int *)data);
    char prop[PROPERTY_VALUE_MAX] = {0};

    RLOGD("enableModem: %d, socket_id: %d", isOn, socket_id);
    snprintf(prop, PROPERTY_VALUE_MAX, "%d", isOn);
    setProperty(socket_id, MODEM_ENABLED_PROP, prop);

    if (isOn) {  // enableModem: 1
        if (s_radioState[socket_id] == RADIO_STATE_OFF
                && s_desiredRadioState[socket_id] == 1) {
            requestRadioPower(socket_id, data, datalen, NULL);
        }
    } else {  // enableModem: 0
        if (s_radioState[socket_id] == RADIO_STATE_ON) {
            requestRadioPower(socket_id, data, datalen, NULL);
        }
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetModemStatus(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                  RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    char prop[PROPERTY_VALUE_MAX] = {0};
    getProperty(socket_id, MODEM_ENABLED_PROP, prop, "1");

    int isOn = atoi(prop);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &isOn, sizeof(int));
}

static void requestQueryNetworkSelectionMode(RIL_SOCKET_ID socket_id, void *data,
                                             size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    int response = 0;
    char *line;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+COPS?",
                                     "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetNetworkSelectionAutomatic(RIL_SOCKET_ID socket_id, void *data,
                                                size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    ATResponse *p_response = NULL;

    // UNISOC Bug[1029439] Modem cannot support set network selection mode in CDMA phone
    if (s_isCDMAPhone[socket_id]) {
        goto error;
    }

    err = at_send_command(socket_id, "AT+COPS=0", &p_response);
    if (err < 0 || p_response->success == 0) {
        if (err == AT_ERROR_TIMEOUT) {
            at_send_command(socket_id, "AT+SAC", NULL);
        }
        goto error;
    }

    AT_RESPONSE_FREE(p_response);
    err = at_send_command(socket_id, "AT+CGAUTO=1", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
            !strcmp(p_response->finalResponse, "+CME ERROR: 30")) {
        RIL_onRequestComplete(t, RIL_E_ILLEGAL_SIM_OR_ME, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestNetworkRegistration(RIL_SOCKET_ID socket_id, void *data,
                                       size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    RIL_NetworkList *network =
            (RIL_NetworkList *)calloc(1, sizeof(RIL_NetworkList));
    network->operatorNumeric = (char *)data;

    char prop[PROPERTY_VALUE_MAX] = {0};
    int copsMode = 1;
    property_get(COPS_MODE_PROP, prop, "manual");
    if (!strcmp(prop, "automatic")) {
        copsMode = 4;
    }
    RLOGD("cops mode = %d", copsMode);

    char *p = strstr(network->operatorNumeric, " ");
    if (p != NULL) {
        network->act = atoi(p + 1);
        *p = 0;
    }
    if (network->act >= 0) {
        snprintf(cmd, sizeof(cmd), "AT+COPS=%d,2,\"%s\",%d", copsMode,
                 network->operatorNumeric, network->act);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+COPS=%d,2,\"%s\"", copsMode,
                 network->operatorNumeric);
    }
    err = at_send_command(socket_id, cmd, &p_response);
    if (err != 0 || p_response->success == 0) {
        if (err == AT_ERROR_TIMEOUT) {
            at_send_command(socket_id, "AT+SAC", NULL);
        }
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    free(network);
    return;

error:
    if (p_response != NULL &&
            !strcmp(p_response->finalResponse, "+CME ERROR: 30")) {
        RIL_onRequestComplete(t, RIL_E_ILLEGAL_SIM_OR_ME, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
    free(network);
}

static void requestNetworkList(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                               RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    static char *statStr[] = {
        "unknown",
        "available",
        "current",
        "forbidden"
    };
    int err, stat, act;
    int tok = 0, count = 0, i = 0;
    char *line;
    char **responses, **cur;
    char *tmp, *startTmp = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+COPS=?",
                                     "+COPS:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    while (*line) {
        if (*line == '(')
            tok++;
        if (*line  == ')') {
            if (tok == 1) {
                count++;
                tok--;
            }
        }
        if (*line == '"') {
            do {
                line++;
                if (*line == 0)
                    break;
                else if (*line == '"')
                    break;
            } while (1);
        }
        if (*line != 0)
            line++;
    }
    RLOGD("Searched available network list numbers = %d", count - 2);
    if (count <= 2) {
        goto error;
    }
    count -= 2;

    line = p_response->p_intermediates->line;
    // (,,,,),,(0-4),(0-2)
    if (strstr(line, ",,,,")) {
        RLOGD("No network");
        goto error;
    }

    // (1,"CHINA MOBILE","CMCC","46000",0), (2,"CHINA MOBILE","CMCC","46000",2),
    // (3,"CHN-UNICOM","CUCC","46001",0),,(0-4),(0-2)
    responses = alloca(count * 4 * sizeof(char *));
    cur = responses;
    tmp = (char *)malloc(count * sizeof(char) * 32);
    startTmp = tmp;

    char *updatedNetList = (char *)alloca(count * sizeof(char) * 64);
    while ((line = strchr(line, '(')) && (i++ < count)) {
        line++;
        err = at_tok_nextint(&line, &stat);
        if (err < 0) continue;

        cur[3] = statStr[stat];

        err = at_tok_nextstr(&line, &(cur[0]));
        if (err < 0) continue;

        err = at_tok_nextstr(&line, &(cur[1]));
        if (err < 0) continue;

        err = at_tok_nextstr(&line, &(cur[2]));
        if (err < 0) continue;

        err = at_tok_nextint(&line, &act);
        if (err < 0) continue;

        snprintf(tmp, count * sizeof(char) * 32, "%s%s%d", cur[2], " ", act);
        RLOGD("requestNetworkList cur[2] act = %s", tmp);
        cur[2] = tmp;

#if defined (RIL_EXTENSION)
        err = updateNetworkList(socket_id, cur, 4 * sizeof(char *),
                                updatedNetList, count * sizeof(char) * 64);
        if (err == 0) {
            RLOGD("updatedNetworkList: %s", updatedNetList);
            cur[0] = updatedNetList;
        }
        updatedNetList += 64;
#endif
        cur += 4;
        tmp += 32;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responses,
            count * 4 * sizeof(char *));
    at_response_free(p_response);
    free(startTmp);
    return;

error:
    if (p_response != NULL &&
            !strcmp(p_response->finalResponse, "+CME ERROR: 3")) {
        RIL_onRequestComplete(t, RIL_E_OPERATION_NOT_ALLOWED, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
    free(startTmp);
}

static void requestResetRadio(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                              RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    ATResponse *p_response = NULL;

    err = at_send_command(socket_id, "AT+CFUN=0", &p_response);
    if (err < 0 || p_response->success == 0) goto error;
    AT_RESPONSE_FREE(p_response);

    setRadioState(socket_id, RADIO_STATE_OFF);

    err = at_send_command(socket_id, "AT+CFUN=1", &p_response);
    if (err < 0 || p_response->success == 0) {
        /* Some stacks return an error when there is no SIM,
         * but they really turn the RF portion on
         * So, if we get an error, let's check to see if it turned on anyway
         */
        if (isRadioOn(socket_id) != 1) goto error;
    }

    setRadioState(socket_id, RADIO_STATE_ON);

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

static void requestSetBandMode(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                               RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    switch (((int *)data)[0]) {
        case BAND_MODE_UNSPECIFIED:
            at_send_command(socket_id, "AT+SBAND=1,13,14", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,1,1", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,2,1", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,5,1", NULL);
            break;
        case BAND_MODE_EURO:
            at_send_command(socket_id, "AT+SBAND=1,13,4", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,1,1", NULL);
            break;
        case BAND_MODE_USA:
            at_send_command(socket_id, "AT+SBAND=1,13,7", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,2,1", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,5,1", NULL);
            break;
        case BAND_MODE_JPN:
            at_send_command(socket_id, "AT+SPFDDBAND=1,2,1", NULL);
            break;
        case BAND_MODE_AUS:
            at_send_command(socket_id, "AT+SBAND=1,13,4", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,2,1", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,5,1", NULL);
            break;
        case BAND_MODE_AUS_2:
            at_send_command(socket_id, "AT+SBAND=1,13,4", NULL);
            at_send_command(socket_id, "AT+SPFDDBAND=1,5,1", NULL);
            break;
        default:
            break;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetBandMode(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                               RIL_Token t) {
    RIL_UNUSED_PARM(socket_id);
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int i, size = 5;
    int response[20] = {0};

    response[0] = size;
    for (i = 1; i <= size; i++) {
        response[i] = i - 1;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS,
            response, (size + 1) * sizeof(int));
}

static int requestSetPreferredNetType(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, type = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    ATResponse *p_response = NULL;
    RIL_Errno errType = RIL_E_GENERIC_FAILURE;
    if (s_callCount[socket_id] > 0) {
        RLOGE("is calling, set network mode failed.");
        RIL_onRequestComplete(t, errType, NULL, 0);
        return -1;
    }
    property_get(ENGTEST_ENABLE_PROP, prop, "false");
    RLOGD("ENGTEST_ENABLE_PROP is %s", prop);

    pthread_mutex_lock(&s_workModeMutex);
    if (0 == strcmp(prop, "true")) {  // request by engineer mode
        switch (((int *)data)[0]) {
            case NT_TD_LTE:
                type = TD_LTE;
                break;
            case NT_LTE_FDD:
                type = LTE_FDD;
                break;
            case NT_LTE_FDD_TD_LTE:
                type = TD_LTE_AND_LTE_FDD;
                break;
            case NT_LTE_FDD_WCDMA_GSM:
                type = LTE_FDD_AND_W_AND_GSM;
                break;
            case NT_TD_LTE_WCDMA_GSM:
                type = TD_LTE_AND_W_AND_GSM;
                break;
            case NT_LTE_FDD_TD_LTE_WCDMA_GSM:
                type = TD_LTE_AND_LTE_FDD_AND_W_AND_GSM;
                break;
            case NT_TD_LTE_TDSCDMA_GSM:
                type = TD_LTE_AND_TD_AND_GSM;
                break;
            case NT_LTE_FDD_TD_LTE_TDSCDMA_GSM:
                type = TD_LTE_AND_LTE_FDD_AND_TD_AND_GSM;
                break;
            case NT_LTE_FDD_TD_LTE_WCDMA_TDSCDMA_GSM:
                type = TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM;
                break;
            case NT_GSM: {
                type = GSM_ONLY;
                if (s_modemConfig < LWG_LWG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_GSM_ONLY;
                    }
                }
                break;
            }
            case NT_WCDMA: {
                type = WCDMA_ONLY;
                if (s_modemConfig == LWG_WG || s_modemConfig == WG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_WCDMA_ONLY;
                    }
                }
                break;
            }
            case NT_TDSCDMA: {
                type = TD_ONLY;
                break;
            }
            case NT_TDSCDMA_GSM: {
                type = TD_AND_GSM;
                break;
            }
            case NT_WCDMA_GSM: {
                type = WCDMA_AND_GSM;
                if (s_modemConfig == LWG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_WCDMA_AND_GSM;
                    } else {
                        type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                    }
                } else if (s_modemConfig == WG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_WCDMA_AND_GSM;
                    }
                }
                break;
            }
            case NT_WCDMA_TDSCDMA_EVDO_CDMA_GSM: {
                if (s_modemConfig == LWG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_TD_AND_WCDMA;
                    } else {
                        type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                    }
                } else {
                    type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                }
                break;
            }
            case NT_LTE_FDD_TD_LTE_GSM:
                type = TD_LTE_AND_LTE_FDD_AND_GSM;
                break;
            case NT_LTE_WCDMA_TDSCDMA_EVDO_CDMA_GSM:
                type = TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                break;
            case NT_EVDO_CDMA:
                type = EVDO_AND_CDMA;
                break;
            case NT_EVDO:
                type = EVDO_ONLY;
                break;
            case NT_CDMA:
                type = CDMA_ONLY;
                break;
            case NT_NR:
                type = NR_ONLY;
                break;
            case NT_NR_LTE_FDD_TD_LTE:
                type = NR_AND_TD_LTE_AND_LTE_FDD;
                break;
            case NT_NR_LTE_FDD_TD_LTE_GSM_WCDMA:
                type = NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM;
                break;
            default:
                break;
        }
    } else {  // request by FWK
        switch (((int *)data)[0]) {
            case NETWORK_MODE_LTE_GSM_WCDMA:
            case NETWORK_MODE_LTE_GSM:
                type = getMultiMode();
                break;
            case NETWORK_MODE_WCDMA_PREF: {
                if (s_modemConfig == LWG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_TD_AND_WCDMA;
                    } else {
                        type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                    }
                } else if (s_modemConfig == WG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_WCDMA_AND_GSM;
                    } else {
                        type = WCDMA_AND_GSM;
                    }
                } else {
                    int mode = getMultiMode();
                    type = mode;
                    if (mode == TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM ||
                        mode == TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM) {
                        type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                    } else if (mode == TD_LTE_AND_LTE_FDD_AND_W_AND_GSM) {
                        type = WCDMA_AND_GSM;
                    } else if (mode == TD_LTE_AND_TD_AND_GSM) {
                        type = TD_AND_GSM;
                    }
                }
                break;
            }
            case NETWORK_MODE_GSM_ONLY:
                type = GSM_ONLY;
                if (s_modemConfig < LWG_LWG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_GSM_ONLY;
                    }
                }
                break;
            case NETWORK_MODE_LTE_ONLY: {
                if (s_modemConfig == WG_WG || s_modemConfig == WG_G) {
                    errType = RIL_E_MODE_NOT_SUPPORTED;
                    RLOGE("lte only not supported");
                    goto done;
                }
                int mode = getMultiMode();
                type = mode;
                if (mode == TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM ||
                    mode == TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM ||
                    mode == TD_LTE_AND_LTE_FDD_AND_W_AND_GSM ||
                    mode == TD_LTE_AND_LTE_FDD_AND_GSM) {
                    type = TD_LTE_AND_LTE_FDD;
                } else if (mode == TD_LTE_AND_TD_AND_GSM) {
                    type = TD_LTE;
                }
                break;
            }
            case NETWORK_MODE_WCDMA_ONLY:
                type = WCDMA_ONLY;
                if (s_modemConfig == LWG_WG || s_modemConfig == WG_WG) {
                    if (socket_id == s_multiModeSim) {
                        type = PRIMARY_WCDMA_ONLY;
                    }
                }
                break;
            case NETWORK_MODE_LTE_WCDMA:
                type = TD_LTE_AND_LTE_FDD_WCDMA;
                break;
            case NETWORK_MODE_TDSCDMA_CDMA_EVDO_GSM_WCDMA:
                type = TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                break;
            case NETWORK_MODE_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA:
                type = TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM;
                break;
            case NETWORK_MODE_NR_LTE_GSM_WCDMA:
                type = NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM;
                break;
            default:
                break;
        }
    }

    if (0 == type) {
        RLOGE("set preferred network failed, type incorrect: %d",
              ((int *)data)[0]);
        errType = RIL_E_GENERIC_FAILURE;
        goto done;
    }

    int workMode;
    char numToStr[ARRAY_SIZE];

    workMode = s_workMode[socket_id];
    if (s_modemConfig == LWG_G || s_modemConfig == WG_G ||
        s_modemConfig == LG_G) {
        if (workMode == NONE || workMode == GSM_ONLY) {
            RLOGD("SetLTEPreferredNetType: not data card");
            errType = RIL_E_SUCCESS;
            goto done;
        }
    } else if (s_modemConfig == LWG_WG) {
        if (s_multiModeSim != socket_id && isPrimaryCardWorkMode(type)) {
            RLOGE("SetLTEPreferredNetType: not data card");
            errType = RIL_E_SUCCESS;
            goto done;
        }
    } else if (s_modemConfig == NRLWG_LWG) {
        if (s_multiModeSim != socket_id && isNRWorkMode(type)) {
            RLOGE("SetLTEPreferredNetType: not data card");
            errType = RIL_E_SUCCESS;
            goto done;
        }
    }

    if (type == workMode) {
        RLOGD("SetPreferredNetType: has send the request before");
        errType = RIL_E_SUCCESS;
        goto done;
    }

#if defined (ANDROID_MULTI_SIM)
#if (SIM_COUNT == 2)
    if (socket_id == RIL_SOCKET_1) {
        snprintf(numToStr, sizeof(numToStr), "%d,%d", type,
                s_workMode[RIL_SOCKET_2]);
    } else if (socket_id == RIL_SOCKET_2) {
        snprintf(numToStr, sizeof(numToStr), "%d,%d", s_workMode[RIL_SOCKET_1],
                type);
    }
#endif
#else
     snprintf(numToStr, sizeof(numToStr), "%d,10", type);
#endif
    RLOGD("set network type workmode:%s", numToStr);
    if (s_modemConfig == NRLWG_LWG) {
        snprintf(cmd, sizeof(cmd), "AT+SPTESTMODE=%s,%d", numToStr, s_multiModeSim);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+SPTESTMODE=%s", numToStr);
    }
    const char *respCmd = "+SPTESTMODE:";
    int retryTimes = 0;

again:
    /* timeout is in seconds
     * Due to AT+SPTESTMODE's response maybe be later than URC response,
     * so addAsyncCmdList before at_send_command
     */
    enqueueAsyncCmdMessage(socket_id, t, respCmd, NULL, asyncCmdTimedCallback, 120);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        if (retryTimes < 3 && p_response != NULL &&
                strcmp(p_response->finalResponse, "+CME ERROR: 3") == 0) {
            RLOGE("AT+SPTESTMODE return +CME ERROR: 3, try again");
            retryTimes++;
            AT_RESPONSE_FREE(p_response);
            removeAsyncCmdMessage(t);
            sleep(3);
            goto again;
        } else {
            errType = RIL_E_GENERIC_FAILURE;
            removeAsyncCmdMessage(t);
            goto done;
        }
    }

    s_workMode[socket_id] = type;
    property_set(MODEM_WORKMODE_PROP, numToStr);

    at_response_free(p_response);
    pthread_mutex_unlock(&s_workModeMutex);
    return 0;

done:
    pthread_mutex_unlock(&s_workModeMutex);
    at_response_free(p_response);
    RIL_onRequestComplete(t, errType, NULL, 0);
    return -1;
}

static int getRadioAccessFamilyFromNetworkMode(int type) {
    int GSM = RAF_GSM | RAF_GPRS | RAF_EDGE;
    int CDMA = RAF_IS95A | RAF_IS95B | RAF_1xRTT;
    int EVDO = RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD;
    int HS = RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP;
    int LTE = RAF_LTE | RAF_LTE_CA;
    int WCDMA = HS | RAF_UMTS;
    int NR = RAF_NR;

    switch (type) {
        case NETWORK_MODE_WCDMA_PREF:
            return GSM | WCDMA;
        case NETWORK_MODE_GSM_ONLY:
            return GSM;
        case NETWORK_MODE_WCDMA_ONLY:
            return WCDMA;
        case NETWORK_MODE_GSM_UMTS:
            return GSM | WCDMA;
        case NETWORK_MODE_CDMA:
            return CDMA | EVDO;
        case NETWORK_MODE_LTE_CDMA_EVDO:
            return LTE | CDMA | EVDO;
        case NETWORK_MODE_LTE_GSM_WCDMA:
            return LTE | GSM | WCDMA;
        case NETWORK_MODE_LTE_CDMA_EVDO_GSM_WCDMA:
            return LTE | CDMA | EVDO | GSM | WCDMA;
        case NETWORK_MODE_LTE_ONLY:
            return LTE;
        case NETWORK_MODE_LTE_WCDMA:
            return LTE | WCDMA;
        case NETWORK_MODE_CDMA_NO_EVDO:
            return CDMA;
        case NETWORK_MODE_EVDO_NO_CDMA:
            return EVDO;
        case NETWORK_MODE_GLOBAL:
            return GSM | WCDMA | CDMA | EVDO;
        case NETWORK_MODE_TDSCDMA_ONLY:
            return RAF_TD_SCDMA;
        case NETWORK_MODE_TDSCDMA_WCDMA:
            return RAF_TD_SCDMA | WCDMA;
        case NETWORK_MODE_LTE_TDSCDMA:
            return LTE | RAF_TD_SCDMA;
        case NETWORK_MODE_TDSCDMA_GSM:
            return RAF_TD_SCDMA | GSM;
        case NETWORK_MODE_LTE_TDSCDMA_GSM:
            return LTE | RAF_TD_SCDMA | GSM;
        case NETWORK_MODE_TDSCDMA_GSM_WCDMA:
            return RAF_TD_SCDMA | GSM | WCDMA;
        case NETWORK_MODE_LTE_TDSCDMA_WCDMA:
            return LTE | RAF_TD_SCDMA | WCDMA;
        case NETWORK_MODE_LTE_TDSCDMA_GSM_WCDMA:
            return LTE | RAF_TD_SCDMA | GSM | WCDMA;
        case NETWORK_MODE_TDSCDMA_CDMA_EVDO_GSM_WCDMA:
            return RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA;
        case NETWORK_MODE_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA:
            return LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA;
        case NETWORK_MODE_NR_ONLY:
            return NR;
        case NETWORK_MODE_NR_LTE:
            return NR | LTE;
        case NETWORK_MODE_NR_LTE_CDMA_EVDO:
            return NR | LTE | CDMA | EVDO;
        case NETWORK_MODE_NR_LTE_GSM_WCDMA:
            return NR | LTE | GSM | WCDMA;
        case NETWORK_MODE_NR_LTE_CDMA_EVDO_GSM_WCDMA:
            return NR | LTE | CDMA | EVDO | GSM | WCDMA;
        case NETWORK_MODE_NR_LTE_WCDMA:
            return NR | LTE | WCDMA;
        case NETWORK_MODE_NR_LTE_TDSCDMA:
            return NR | LTE | RAF_TD_SCDMA;
        case NETWORK_MODE_NR_LTE_TDSCDMA_GSM:
            return NR | LTE | RAF_TD_SCDMA | GSM;
        case NETWORK_MODE_NR_LTE_TDSCDMA_WCDMA:
            return NR | LTE | RAF_TD_SCDMA | WCDMA;
        case NETWORK_MODE_NR_LTE_TDSCDMA_GSM_WCDMA:
            return NR | LTE | RAF_TD_SCDMA | GSM | WCDMA;
        case NETWORK_MODE_NR_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA:
            return NR | LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA;
        default:
            return RAF_UNKNOWN;
    }
}

/**
 * if the raf includes ANY bit set for a group
 * adjust it to contain ALL the bits for that group
 */
static int getAdjustedRaf(int raf) {
    int GSM = RAF_GSM | RAF_GPRS | RAF_EDGE;
    int CDMA = RAF_IS95A | RAF_IS95B | RAF_1xRTT;
    int EVDO = RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD;
    int HS = RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP;
    int LTE = RAF_LTE | RAF_LTE_CA;
    int WCDMA = HS | RAF_UMTS;
    int NR = RAF_NR;

    raf = ((GSM & raf) > 0) ? (GSM | raf) : raf;
    raf = ((WCDMA & raf) > 0) ? (WCDMA | raf) : raf;
    raf = ((CDMA & raf) > 0) ? (CDMA | raf) : raf;
    raf = ((EVDO & raf) > 0) ? (EVDO | raf) : raf;
    raf = ((LTE & raf) > 0) ? (LTE | raf) : raf;
    raf = ((NR & raf) > 0) ? (NR | raf) : raf;

    return raf;
}

static int getNetworkModeFromRadioAccessFamily(int raf) {
    int GSM = RAF_GSM | RAF_GPRS | RAF_EDGE;
    int CDMA = RAF_IS95A | RAF_IS95B | RAF_1xRTT;
    int EVDO = RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD;
    int HS = RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP;
    int LTE = RAF_LTE | RAF_LTE_CA;
    int WCDMA = HS | RAF_UMTS;
    int NR = RAF_NR;

    int adjustedRaf = getAdjustedRaf(raf);

    if (adjustedRaf == (GSM | WCDMA)) {
        return NETWORK_MODE_WCDMA_PREF;
    } else if (adjustedRaf == GSM) {
        return NETWORK_MODE_GSM_ONLY;
    } else if (adjustedRaf == WCDMA) {
        return NETWORK_MODE_WCDMA_ONLY;
    } else if (adjustedRaf == (CDMA | EVDO)) {
        return NETWORK_MODE_CDMA;
    } else if (adjustedRaf == (LTE | CDMA | EVDO)) {
        return NETWORK_MODE_LTE_CDMA_EVDO;
    } else if (adjustedRaf == (LTE | GSM | WCDMA)) {
        return NETWORK_MODE_LTE_GSM_WCDMA;
    } else if (adjustedRaf == (LTE | CDMA | EVDO | GSM | WCDMA)) {
        return NETWORK_MODE_LTE_CDMA_EVDO_GSM_WCDMA;
    } else if (adjustedRaf == LTE) {
        return NETWORK_MODE_LTE_ONLY;
    } else if (adjustedRaf == (LTE | WCDMA)) {
        return NETWORK_MODE_LTE_WCDMA;
    } else if (adjustedRaf == CDMA) {
        return NETWORK_MODE_CDMA_NO_EVDO;
    } else if (adjustedRaf == EVDO) {
        return NETWORK_MODE_EVDO_NO_CDMA;
    } else if (adjustedRaf == (GSM | WCDMA | CDMA | EVDO)) {
        return NETWORK_MODE_GLOBAL;
    } else if (adjustedRaf == RAF_TD_SCDMA) {
        return NETWORK_MODE_TDSCDMA_ONLY;
    } else if (adjustedRaf == (RAF_TD_SCDMA | WCDMA)) {
        return NETWORK_MODE_TDSCDMA_WCDMA;
    } else if (adjustedRaf == (LTE | RAF_TD_SCDMA)) {
        return NETWORK_MODE_LTE_TDSCDMA;
    } else if (adjustedRaf == (RAF_TD_SCDMA | GSM)) {
        return NETWORK_MODE_TDSCDMA_GSM;
    } else if (adjustedRaf == (LTE | RAF_TD_SCDMA | GSM)) {
        return NETWORK_MODE_LTE_TDSCDMA_GSM;
    } else if (adjustedRaf == (RAF_TD_SCDMA | GSM | WCDMA)) {
        return NETWORK_MODE_TDSCDMA_GSM_WCDMA;
    } else if (adjustedRaf == (LTE | RAF_TD_SCDMA | WCDMA)) {
        return NETWORK_MODE_LTE_TDSCDMA_WCDMA;
    } else if (adjustedRaf == (LTE | RAF_TD_SCDMA | GSM | WCDMA)) {
        return NETWORK_MODE_LTE_TDSCDMA_GSM_WCDMA;
    } else if (adjustedRaf == (RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA)) {
        return NETWORK_MODE_TDSCDMA_CDMA_EVDO_GSM_WCDMA;
    } else if (adjustedRaf == (LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA)) {
        return NETWORK_MODE_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA;
    } else if (adjustedRaf == (NR)) {
        return NETWORK_MODE_NR_ONLY;
    } else if (adjustedRaf == (NR | LTE)) {
        return NETWORK_MODE_NR_LTE;
    } else if (adjustedRaf == (NR | LTE | CDMA | EVDO)) {
        return NETWORK_MODE_NR_LTE_CDMA_EVDO;
    } else if (adjustedRaf == (NR | LTE | GSM | WCDMA)) {
        return NETWORK_MODE_NR_LTE_GSM_WCDMA;
    } else if (adjustedRaf == (NR | LTE | CDMA | EVDO | GSM | WCDMA)) {
        return NETWORK_MODE_NR_LTE_CDMA_EVDO_GSM_WCDMA;
    } else if (adjustedRaf == (NR | LTE | WCDMA)) {
        return NETWORK_MODE_NR_LTE_WCDMA;
    } else if (adjustedRaf == (NR | LTE | RAF_TD_SCDMA)) {
        return NETWORK_MODE_NR_LTE_TDSCDMA;
    } else if (adjustedRaf == (NR | LTE | RAF_TD_SCDMA | GSM)) {
        return NETWORK_MODE_NR_LTE_TDSCDMA_GSM;
    } else if (adjustedRaf == (NR | LTE | RAF_TD_SCDMA | WCDMA)) {
        return NETWORK_MODE_NR_LTE_TDSCDMA_WCDMA;
    } else if (adjustedRaf == (NR | LTE | RAF_TD_SCDMA | GSM | WCDMA)) {
        return NETWORK_MODE_NR_LTE_TDSCDMA_GSM_WCDMA;
    } else if (adjustedRaf == (NR | LTE | RAF_TD_SCDMA | CDMA | EVDO | GSM | WCDMA)) {
        return NETWORK_MODE_NR_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA;
    } else {
        return NETWORK_MODE_WCDMA_PREF;
    }
}

static void requestGetPreferredNetType(RIL_SOCKET_ID socket_id, void *data,
                                       size_t datalen, RIL_Token t,
                                       int *response) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int type = -1;
    char prop[PROPERTY_VALUE_MAX] = {0};

    property_get(ENGTEST_ENABLE_PROP, prop, "false");
    RLOGD("ENGTEST_ENABLE_PROP is %s", prop);

    if (0 == strcmp(prop, "true")) {  // request by engineer mode
        switch (s_workMode[socket_id]) {
            case TD_LTE:
                type = NT_TD_LTE;
                break;
            case LTE_FDD:
                type = NT_LTE_FDD;
                break;
            case TD_LTE_AND_LTE_FDD:
                type = NT_LTE_FDD_TD_LTE;
                break;
            case LTE_FDD_AND_W_AND_GSM:
                type = NT_LTE_FDD_WCDMA_GSM;
                break;
            case TD_LTE_AND_W_AND_GSM:
                type = NT_TD_LTE_WCDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_AND_W_AND_GSM:
                type = NT_LTE_FDD_TD_LTE_WCDMA_GSM;
                break;
            case TD_LTE_AND_TD_AND_GSM:
                type = NT_TD_LTE_TDSCDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_AND_TD_AND_GSM:
                type = NT_LTE_FDD_TD_LTE_TDSCDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM:
                type = NT_LTE_FDD_TD_LTE_WCDMA_TDSCDMA_GSM;
                break;
            case PRIMARY_GSM_ONLY:
            case GSM_ONLY:
                type = NT_GSM;
                break;
            case WCDMA_ONLY:
            case PRIMARY_WCDMA_ONLY:
                type = NT_WCDMA;
                break;
            case TD_ONLY:
                type = NT_TDSCDMA;
                break;
            case TD_AND_GSM:
                type = NT_TDSCDMA_GSM;
                break;
            case WCDMA_AND_GSM:
            case PRIMARY_WCDMA_AND_GSM:
                type = NT_WCDMA_GSM;
                break;
            case PRIMARY_TD_AND_WCDMA:
                type = NT_WCDMA_TDSCDMA_EVDO_CDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_AND_GSM:
                type = NT_LTE_FDD_TD_LTE_GSM;
                break;
            case TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM:
                type = NT_WCDMA_TDSCDMA_EVDO_CDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM:
                type = NT_LTE_WCDMA_TDSCDMA_EVDO_CDMA_GSM;
                break;
            case TD_LTE_AND_LTE_FDD_WCDMA:
                type = NT_LTE_FDD_TD_LTE_WCDMA;
                break;
            case EVDO_AND_CDMA:
                type = NT_EVDO_CDMA;
                break;
            case EVDO_ONLY:
                type = NT_EVDO;
                break;
            case CDMA_ONLY:
                type = NT_CDMA;
                break;
            case NR_ONLY:
                type = NT_NR;
                break;
            case NR_AND_TD_LTE_AND_LTE_FDD:
                type = NT_NR_LTE_FDD_TD_LTE;
                break;
            case NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM:
                type = NT_NR_LTE_FDD_TD_LTE_GSM_WCDMA;
                break;
            default:
                break;
        }
    } else {
        switch (s_workMode[socket_id]) {
            case TD_LTE_AND_LTE_FDD_AND_W_AND_TD_AND_GSM:
            case TD_LTE_AND_LTE_FDD_AND_W_AND_GSM:
            case TD_LTE_AND_TD_AND_GSM:
                type = NETWORK_MODE_LTE_GSM_WCDMA;
                break;
            case TD_LTE_AND_LTE_FDD_AND_GSM:
                type = NETWORK_MODE_LTE_GSM;
                break;
            case PRIMARY_TD_AND_WCDMA:
            case WCDMA_AND_GSM:
            case PRIMARY_WCDMA_AND_GSM:
            case TD_AND_GSM:
                type = NETWORK_MODE_WCDMA_PREF;
                break;
            case PRIMARY_GSM_ONLY:
            case GSM_ONLY:
                type = NETWORK_MODE_GSM_ONLY;
                break;
            case TD_LTE:
            case LTE_FDD:
            case TD_LTE_AND_LTE_FDD:
                type = NETWORK_MODE_LTE_ONLY;
                break;
            case WCDMA_ONLY:
            case PRIMARY_WCDMA_ONLY:
                type = NETWORK_MODE_WCDMA_ONLY;
                break;
            case TD_LTE_AND_LTE_FDD_WCDMA:
                type = NETWORK_MODE_LTE_WCDMA;
                break;
            case TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM:
                type = NETWORK_MODE_TDSCDMA_CDMA_EVDO_GSM_WCDMA;
                break;
            case TD_LTE_AND_LTE_FDD_AND_TD_AND_WCDMA_AND_EVDO_AND_CDMA_AND_GSM:
                type = NETWORK_MODE_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA;
                break;
            case NR_AND_TD_LTE_AND_LTE_FDD_AND_WCDMA_AND_GSM:
                type = NETWORK_MODE_NR_LTE_GSM_WCDMA;
                break;
            default:
                break;
        }
    }
    if (type < 0) {
        RLOGD("GetPreferredNetType: incorrect workmode %d",
              s_workMode[socket_id]);
        goto error;
    }

    if (t != NULL) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &type, sizeof(type));
    } else if (response != NULL) {
        *response = type;
    }
    return;

error:
    if (t != NULL) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else if (response != NULL) {
        *response = -1;
    }
}

static void requestNeighboaringCellIds(RIL_SOCKET_ID socket_id, void *data,
                                       size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = 0;
    int cellIdNumber = 0;
    int current = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_NeighboringCell *NeighboringCell = NULL;
    RIL_NeighboringCell **NeighboringCellList = NULL;
    //for vts cases
    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("requestNeighboaringCellIds: card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    err = at_send_command_singleline(socket_id, "AT+Q2GNCELL",
                                     "+Q2GNCELL:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {  // only in 2G
        char *sskip = NULL;
        int skip;

        err = at_tok_nextstr(&line, &sskip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &cellIdNumber);
        if (err < 0 || cellIdNumber == 0) {
            goto error;
        }
        NeighboringCellList = (RIL_NeighboringCell **)
                alloca(cellIdNumber * sizeof(RIL_NeighboringCell *));

        NeighboringCell = (RIL_NeighboringCell *)
                alloca(cellIdNumber * sizeof(RIL_NeighboringCell));

        for (current = 0; at_tok_hasmore(&line), current < cellIdNumber;
                current++) {
            err = at_tok_nextstr(&line, &(NeighboringCell[current].cid));
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &(NeighboringCell[current].rssi));
            if (err < 0) goto error;

            RLOGD("Neighbor cell_id %s = %d",
                  NeighboringCell[current].cid, NeighboringCell[current].rssi);

            NeighboringCellList[current] = &NeighboringCell[current];
        }
    } else {
        AT_RESPONSE_FREE(p_response);
        err = at_send_command_singleline(socket_id, "AT+Q3GNCELL",
                                         "+Q3GNCELL:", &p_response);
        if (err != 0 || p_response->success == 0) {
            goto error;
        }

        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        if (at_tok_hasmore(&line)) {  // only in 3G
            char *sskip = NULL;
            int skip;

            err = at_tok_nextstr(&line, &sskip);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &cellIdNumber);
            if (err < 0 || cellIdNumber == 0) {
                goto error;
            }
            NeighboringCellList = (RIL_NeighboringCell **)
                    alloca(cellIdNumber * sizeof(RIL_NeighboringCell *));

            NeighboringCell = (RIL_NeighboringCell *)
                    alloca(cellIdNumber * sizeof(RIL_NeighboringCell));

            for (current = 0; at_tok_hasmore(&line), current < cellIdNumber;
                    current++) {
                err = at_tok_nextstr(&line, &(NeighboringCell[current].cid));
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &(NeighboringCell[current].rssi));
                if (err < 0) goto error;

                RLOGD("Neighbor cell_id %s = %d", NeighboringCell[current].cid,
                      NeighboringCell[current].rssi);

                NeighboringCellList[current] = &NeighboringCell[current];
            }
        } else {
            AT_RESPONSE_FREE(p_response);
            err = at_send_command_singleline(socket_id,
                    "AT+SPQ4GNCELL", "+SPQ4GNCELL:", &p_response);
            if (err != 0 || p_response->success == 0)
                goto error;

            line = p_response->p_intermediates->line;

            err = at_tok_start(&line);
            if (err < 0) goto error;
            if (at_tok_hasmore(&line)) {  // only in 4G
                char *sskip = NULL;
                int skip;

                err = at_tok_nextstr(&line, &sskip);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &skip);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &cellIdNumber);
                if (err < 0 || cellIdNumber == 0) {
                    goto error;
                }
                NeighboringCellList = (RIL_NeighboringCell **)
                        alloca(cellIdNumber * sizeof(RIL_NeighboringCell *));

                NeighboringCell = (RIL_NeighboringCell *)
                        alloca(cellIdNumber * sizeof(RIL_NeighboringCell));

                for (current = 0; at_tok_hasmore(&line),
                     current < cellIdNumber; current++) {
                    err = at_tok_nextstr(&line, &sskip);
                    if (err < 0) goto error;

                    err = at_tok_nextstr(&line,
                            &(NeighboringCell[current].cid));
                    if (err < 0) goto error;

                    err = at_tok_nextint(&line,
                            &(NeighboringCell[current].rssi));
                    if (err < 0) goto error;

                    RLOGD("Neighbor cell_id %s = %d",
                          NeighboringCell[current].cid,
                          NeighboringCell[current].rssi);

                    NeighboringCellList[current] = &NeighboringCell[current];
                }
            } else {
                goto error;
            }
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NeighboringCellList,
                          cellIdNumber * sizeof(RIL_NeighboringCell *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetCellInfoList(RIL_SOCKET_ID socket_id, void *data,
                                   size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int mcc = 0, mnc = 0, mnc_digit = 0, lac = 0;
    int sig2G = 0, sig3G = 0;
    int pci = 0;
    long cid = 0;
    int arfcn = INT_MAX, bsic = INT_MAX, psc =INT_MAX;
    int rsrp = 0, rsrq = 0;
    int sinr = 0, csirsrp = 0, csirsrq = 0, csisinr = 0;
    int commas = 0, sskip = 0, registered = 0;
    int netType = 0, cellType = 0, biterr2G = 0, biterr3G = 0;
    int cellIdNumber = 0, current = 0, signalStrength = 0;
    int totalNumber = 0;
    char *line =  NULL, *p = NULL, *skip = NULL, *plmn = NULL;

    ATResponse *p_response = NULL;
    ATResponse *p_newResponse = NULL;

    RIL_CellInfo_v1_4 *response = NULL;

    // for mcc & mnc
    err = at_send_command_singleline(socket_id, "AT+COPS?",
                                     "+COPS:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &sskip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &sskip);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &plmn);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &netType);
    if (err < 0) goto error;

    if (plmn != NULL) {
        mnc_digit = strlen(plmn) - 3;
        if (strlen(plmn) == 5) {
            mcc = atoi(plmn) / 100;
            mnc = atoi(plmn) - mcc * 100;
        } else if (strlen(plmn) == 6) {
            mcc = atoi(plmn) / 1000;
            mnc = atoi(plmn) - mcc * 1000;
        } else {
            RLOGE("Invalid plmn");
        }
    }

    if (netType == 10 || netType == 11) {
            cellType = RIL_CELL_INFO_TYPE_NR;
    } else if (netType == 7 || netType == 16 || netType == 13) {
        cellType = RIL_CELL_INFO_TYPE_LTE;
    } else if (netType == 0 || netType == 1 || netType == 3) {
        cellType = RIL_CELL_INFO_TYPE_GSM;
    } else {
        cellType = RIL_CELL_INFO_TYPE_WCDMA;
    }

    AT_RESPONSE_FREE(p_response);

    // For net type, tac
    if (cellType == RIL_CELL_INFO_TYPE_NR) {
        err = at_send_command_singleline(socket_id, "AT+C5GREG?",
                                         "+C5GREG:", &p_response);
    } else if (cellType == RIL_CELL_INFO_TYPE_LTE) {
        err = at_send_command_singleline(socket_id, "AT+CEREG?",
                                         "+CEREG:", &p_response);
    } else {
        err = at_send_command_singleline(socket_id, "AT+CREG?",
                                         "+CREG:", &p_response);
    }
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    for (p = line; *p != '\0'; p++) {
        if (*p == ',') commas++;
    }
    if (commas > 3) {
        char *endptr;
        err = at_tok_nextint(&line, &sskip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &registered);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &skip);  // 2/3G:s_lac  4G:tac
        if (err < 0) goto error;

        lac = strtol(skip, &endptr, 16);
        err = at_tok_nextstr(&line, &skip);  // 2/3G:s_lac  4G:tac
        if (err < 0) goto error;

        cid = strtol(skip, &endptr, 16);
    }

    AT_RESPONSE_FREE(p_response);

    err = at_send_command_singleline(socket_id, "AT+CESQ",
                                     "+CESQ:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    cesq_execute_cmd_rsp(socket_id, p_response, &p_newResponse);
    if (p_newResponse == NULL) goto error;

    line = p_newResponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &sig2G);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &biterr2G);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &sig3G);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &biterr3G);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &rsrq);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &rsrp);
    if (err < 0) goto error;

    AT_RESPONSE_FREE(p_response);

    // For cellinfo
    if (cellType == RIL_CELL_INFO_TYPE_NR) {
        err = at_send_command_singleline(socket_id,
                        "AT+SPQ5GNCELLEX", "+SPQ5GNCELL", &p_response);
        if (err < 0 || p_response->success == 0) {
           goto error;
        }

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        /* AT< +SPQ5GNCELL: Serv_Cell_Nrarfcn, Serv_Cell_Pci, Serv_Cell_ssRsrp, Serv_Cell_ssRsrq, Serv_Cell_ssSinr,
         *                               Serv_Cell_csiRsrp, Serv_Cell_csiRsrq, Serv_Cell_csiSinr, Ncell_num,
         * Neighbor_Cell_Nrarfcn, Neighbor_Cell_Pci, Neighbor_Cell_ssRsrp, Neighbor_Cell_ssRsrq, Neighbor_Cell_ssSinr,
         *                               Neighbor_Cell_csiRsrp, Neighbor_Cell_csiRsrq, Neighbor_Cell_csiSinr
         * ...
         */

        err = at_tok_nextint(&line, &arfcn);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &pci);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rsrp);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rsrq);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &sinr);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &csirsrp);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &csirsrq);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &csisinr);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &cellIdNumber);
        if (err < 0) goto error;

        totalNumber = cellIdNumber + 1;
        response = (RIL_CellInfo_v1_4 *)
                calloc(totalNumber, sizeof(RIL_CellInfo_v1_4));
        if (response == NULL) {
            RLOGE("Failed to calloc memory for response");
            goto error;
        }

        response[0].CellInfo.nr.cellidentity.mnc = mnc;
        response[0].CellInfo.nr.cellidentity.mnc_digit = mnc_digit;
        response[0].CellInfo.nr.cellidentity.mcc = mcc;
        response[0].CellInfo.nr.cellidentity.nci = cid;
        response[0].CellInfo.nr.cellidentity.pci = pci;
        response[0].CellInfo.nr.cellidentity.tac = lac;
        response[0].CellInfo.nr.cellidentity.nrarfcn= arfcn;

        response[0].CellInfo.nr.signalStrength.ssRsrp = rsrp / (-100);
        response[0].CellInfo.nr.signalStrength.ssRsrq = rsrq / (-100);
        response[0].CellInfo.nr.signalStrength.ssSinr = sinr;
        response[0].CellInfo.nr.signalStrength.csiRsrp = csirsrp / (-100);
        response[0].CellInfo.nr.signalStrength.csiRsrq = csirsrq / (-100);
        response[0].CellInfo.nr.signalStrength.csiSinr = csisinr;

        for (current = 0; at_tok_hasmore(&line), current < cellIdNumber; current++) {
            err = at_tok_nextint(&line, &arfcn);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &pci);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &rsrp);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &rsrq);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &sinr);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csirsrp);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csirsrq);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csisinr);
            if (err < 0) goto error;

            response[current + 1].CellInfo.nr.cellidentity.mcc = INT_MAX;
            response[current + 1].CellInfo.nr.cellidentity.mnc = INT_MAX;
            response[current + 1].CellInfo.nr.cellidentity.nci = LONG_MAX;
            response[current + 1].CellInfo.nr.cellidentity.tac = INT_MAX;
            response[current + 1].CellInfo.nr.cellidentity.pci = pci;
            response[current + 1].CellInfo.nr.cellidentity.nrarfcn = arfcn;

            response[current + 1].CellInfo.nr.signalStrength.ssRsrp = rsrp / (-100);
            response[current + 1].CellInfo.nr.signalStrength.ssRsrq = rsrq / (-100);
            response[current + 1].CellInfo.nr.signalStrength.ssSinr = sinr;
            response[current + 1].CellInfo.nr.signalStrength.csiRsrp = csirsrp / (-100);
            response[current + 1].CellInfo.nr.signalStrength.csiRsrq = csirsrq / (-100);
            response[current + 1].CellInfo.nr.signalStrength.csiSinr = csisinr;

            response[current + 1].isRegistered = 0;
            response[current + 1].cellInfoType = cellType;
            response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
            response[current + 1].timeStamp = INT_MAX;
        }
    } else if (cellType == RIL_CELL_INFO_TYPE_LTE) {
        err = at_send_command_singleline(socket_id,
                "AT+SPQ4GNCELLEX", "+SPQ4GNCELL", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        // AT> +SPQ4GNCELLEX
        // AT< +SPQ4GNCELL: Serv_Cell_Earfcn, Serv_Cell_Pci, Serv_Cell_Rsrp, Serv_Cell_Rsrq, Ncell_num,
        // Neighbor_Cell_Earfcn, Neighbor_Cell_Pci, Neighbor_Cell_Rsrp, Neighbor_Cell_Rsrq,
        // ...

        err = at_tok_nextint(&line, &arfcn);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &pci);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rsrp);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rsrq);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &cellIdNumber);
        if (err < 0) goto error;

        totalNumber = s_nrStatusConnected[socket_id] ? (cellIdNumber + 2) : (cellIdNumber + 1);
        response = (RIL_CellInfo_v1_4 *)
                calloc(totalNumber, sizeof(RIL_CellInfo_v1_4));
        if (response == NULL) {
            RLOGE("Failed to calloc memory for response");
            goto error;
        }

        response[0].CellInfo.lte.base.cellIdentityLte.mnc = mnc;
        response[0].CellInfo.lte.base.cellIdentityLte.mnc_digit = mnc_digit;
        response[0].CellInfo.lte.base.cellIdentityLte.mcc = mcc;
        response[0].CellInfo.lte.base.cellIdentityLte.ci  = cid;
        response[0].CellInfo.lte.base.cellIdentityLte.pci = pci;
        response[0].CellInfo.lte.base.cellIdentityLte.tac = lac;
        response[0].CellInfo.lte.base.cellIdentityLte.earfcn = arfcn;
        response[0].CellInfo.lte.base.cellIdentityLte.bandwidth = INT_MAX;

        response[0].CellInfo.lte.base.signalStrengthLte.cqi = INT_MAX;
        response[0].CellInfo.lte.base.signalStrengthLte.rsrp = rsrp / (-100);
        response[0].CellInfo.lte.base.signalStrengthLte.rsrq = rsrq / (-100);
        response[0].CellInfo.lte.base.signalStrengthLte.rssnr = INT_MAX;
        response[0].CellInfo.lte.base.signalStrengthLte.signalStrength = rsrp / 100 + 140;
        response[0].CellInfo.lte.base.signalStrengthLte.timingAdvance  = INT_MAX;

        for (current = 0; at_tok_hasmore(&line), current < cellIdNumber;
             current++) {
            err = at_tok_nextint(&line, &arfcn);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &pci);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &rsrp);
            if (err < 0) goto error;


            err = at_tok_nextint(&line, &rsrq);
            if (err < 0) goto error;

            response[current + 1].CellInfo.lte.base.cellIdentityLte.mcc = INT_MAX;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.mnc = INT_MAX;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.ci = INT_MAX;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.tac = INT_MAX;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.pci = pci;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.earfcn = arfcn;
            response[current + 1].CellInfo.lte.base.cellIdentityLte.bandwidth = INT_MAX;

            response[current + 1].CellInfo.lte.base.signalStrengthLte.cqi = INT_MAX;
            response[current + 1].CellInfo.lte.base.signalStrengthLte.rsrp = rsrp / (-100);
            response[current + 1].CellInfo.lte.base.signalStrengthLte.rsrq = rsrq / (-100);
            response[current + 1].CellInfo.lte.base.signalStrengthLte.rssnr = INT_MAX;
            response[current + 1].CellInfo.lte.base.signalStrengthLte.signalStrength = rsrp / 100 + 140;
            response[current + 1].CellInfo.lte.base.signalStrengthLte.timingAdvance  = INT_MAX;

            response[current + 1].isRegistered = 0;
            response[current + 1].cellInfoType = cellType;
            response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
            response[current + 1].timeStamp = INT_MAX;
        }

        AT_RESPONSE_FREE(p_response);
        if (s_nrStatusConnected[socket_id]) {
            err = at_send_command_singleline(socket_id,
                            "AT+SPQ5GNCELLEX", "+SPQ5GNCELL", &p_response);
            if (err < 0 || p_response->success == 0) {
                goto error;
            }

            line = p_response->p_intermediates->line;
            err = at_tok_start(&line);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &arfcn);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &pci);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &rsrp);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &rsrq);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &sinr);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csirsrp);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csirsrq);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &csisinr);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &cellIdNumber);
            if (err < 0) goto error;

            if (cellIdNumber > 0) {
                totalNumber += cellIdNumber;
                response = (RIL_CellInfo_v1_4 *)
                        realloc(response, totalNumber * sizeof(RIL_CellInfo_v1_4));
            }
            if (response == NULL) {
                RLOGE("Failed to calloc memory for response");
                goto error;
            }

            response[current + 1].CellInfo.nr.cellidentity.mcc = mcc;
            response[current + 1].CellInfo.nr.cellidentity.mnc_digit = mnc_digit;
            response[current + 1].CellInfo.nr.cellidentity.mnc = mnc;
            response[current + 1].CellInfo.nr.cellidentity.nci = LONG_MAX;
            response[current + 1].CellInfo.nr.cellidentity.tac = INT_MAX;
            response[current + 1].CellInfo.nr.cellidentity.pci = pci;
            response[current + 1].CellInfo.nr.cellidentity.nrarfcn = arfcn;

            // pointer initialization was required after realloc
            response[current + 1].CellInfo.nr.cellidentity.operatorNames.alphaLong = "";
            response[current + 1].CellInfo.nr.cellidentity.operatorNames.alphaShort = "";

            response[current + 1].CellInfo.nr.signalStrength.ssRsrp = rsrp / (-100);
            response[current + 1].CellInfo.nr.signalStrength.ssRsrq = rsrq / (-100);
            response[current + 1].CellInfo.nr.signalStrength.ssSinr = sinr;
            response[current + 1].CellInfo.nr.signalStrength.csiRsrp = csirsrp / (-100);
            response[current + 1].CellInfo.nr.signalStrength.csiRsrq = csirsrq / (-100);
            response[current + 1].CellInfo.nr.signalStrength.csiSinr = csisinr;

            response[current + 1].isRegistered = 0;
            response[current + 1].cellInfoType = RIL_CELL_INFO_TYPE_NR;
            response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
            response[current + 1].timeStamp = INT_MAX;

            for (int i = 0; at_tok_hasmore(&line), i < cellIdNumber; i++) {
                current = current + 1;
                err = at_tok_nextint(&line, &arfcn);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &pci);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &rsrp);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &rsrq);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &sinr);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &csirsrp);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &csirsrq);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &csisinr);
                if (err < 0) goto error;

                response[current + 1].CellInfo.nr.cellidentity.mcc = INT_MAX;
                response[current + 1].CellInfo.nr.cellidentity.mnc = INT_MAX;
                response[current + 1].CellInfo.nr.cellidentity.nci = LONG_MAX;
                response[current + 1].CellInfo.nr.cellidentity.tac = INT_MAX;
                response[current + 1].CellInfo.nr.cellidentity.pci = pci;
                response[current + 1].CellInfo.nr.cellidentity.nrarfcn = arfcn;

                // pointer initialization was required after realloc
                response[current + 1].CellInfo.nr.cellidentity.operatorNames.alphaLong = "";
                response[current + 1].CellInfo.nr.cellidentity.operatorNames.alphaShort = "";

                response[current + 1].CellInfo.nr.signalStrength.ssRsrp = rsrp / (-100);
                response[current + 1].CellInfo.nr.signalStrength.ssRsrq = rsrq / (-100);
                response[current + 1].CellInfo.nr.signalStrength.ssSinr = sinr;
                response[current + 1].CellInfo.nr.signalStrength.csiRsrp = csirsrp / (-100);
                response[current + 1].CellInfo.nr.signalStrength.csiRsrq = csirsrq / (-100);
                response[current + 1].CellInfo.nr.signalStrength.csiSinr = csisinr;

                response[current + 1].isRegistered = 0;
                response[current + 1].cellInfoType = RIL_CELL_INFO_TYPE_NR;
                response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
                response[current + 1].timeStamp = INT_MAX;
            }
        }
    } else if (cellType == RIL_CELL_INFO_TYPE_GSM) {
        err = at_send_command_singleline(socket_id,
                "AT+SPQ2GNCELL", "+SPQ2GNCELL", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        // AT+SPQ2GNCELL:
        // Serv_Cell_Lac+Cid, Serv_Cell_SignalStrength, Serv_Cell_Arfcn, Serv_Cell_Bsic, Ncell_num,
        // Neighbor_Cell_Lac+Cid, Neighbor_Cell_SignalStrength, Neighbor_Cell_Arfcn, Neighbor_Cell_Bsic,
        // ...

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &sskip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &arfcn);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &bsic);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &cellIdNumber);
        if (err < 0) goto error;

        totalNumber = cellIdNumber + 1;
        response = (RIL_CellInfo_v1_4 *)
                calloc(totalNumber, sizeof(RIL_CellInfo_v1_4));
        if (response == NULL) {
            RLOGE("Failed to calloc memory for response");
            goto error;
        }

        response[0].CellInfo.gsm.cellIdentityGsm.mcc = mcc;
        response[0].CellInfo.gsm.cellIdentityGsm.mnc = mnc;
        response[0].CellInfo.gsm.cellIdentityGsm.mnc_digit = mnc_digit;
        response[0].CellInfo.gsm.cellIdentityGsm.lac = lac;
        response[0].CellInfo.gsm.cellIdentityGsm.cid = cid;
        response[0].CellInfo.gsm.cellIdentityGsm.arfcn = arfcn;
        response[0].CellInfo.gsm.cellIdentityGsm.bsic = bsic;

        response[0].CellInfo.gsm.signalStrengthGsm.bitErrorRate = biterr2G;
        response[0].CellInfo.gsm.signalStrengthGsm.signalStrength = sig2G;
        response[0].CellInfo.gsm.signalStrengthGsm.timingAdvance = INT_MAX;

        for (current = 0; at_tok_hasmore(&line), current < cellIdNumber;
             current++) {
            err = at_tok_nextstr(&line, &skip);
            if (err < 0) goto error;

            sscanf(skip, "%04x%4lx", &lac, &cid);
            RLOGD("2glac = %d, 2gcid = %ld", lac, cid);

            err = at_tok_nextint(&line, &sig2G);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &arfcn);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &bsic);
            if (err < 0) goto error;

            signalStrength = (sig2G + 3) / 2;

            response[current + 1].CellInfo.gsm.cellIdentityGsm.mcc = INT_MAX;
            response[current + 1].CellInfo.gsm.cellIdentityGsm.mnc = INT_MAX;
            response[current + 1].CellInfo.gsm.cellIdentityGsm.lac = lac;
            response[current + 1].CellInfo.gsm.cellIdentityGsm.cid = cid;
            response[current + 1].CellInfo.gsm.cellIdentityGsm.arfcn = arfcn;
            response[current + 1].CellInfo.gsm.cellIdentityGsm.bsic = bsic;

            response[current + 1].CellInfo.gsm.signalStrengthGsm.bitErrorRate = INT_MAX;
            response[current + 1].CellInfo.gsm.signalStrengthGsm.signalStrength =
                    signalStrength > 31 ? 31 : (signalStrength < 0 ? 0 : signalStrength);
            response[current + 1].CellInfo.gsm.signalStrengthGsm.timingAdvance = INT_MAX;

            response[current + 1].isRegistered = 0;
            response[current + 1].cellInfoType = cellType;
            response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
            response[current + 1].timeStamp = INT_MAX;
        }
    } else {
        err = at_send_command_singleline(socket_id,
                "AT+SPQ3GNCELLEX=4,3", "+SPQ3GNCELL", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }

        // AT> +SPQ3GNCELLEX: 4,3
        // AT< +SPQ3GNCELLEX:
        // "00000000", Serv_Cell_SignalStrength, Serv_Cell_Uarfcn, Serv_Cell_Psc, Ncell_num,
        // Neighbor_Cell_Uarfcn, Neighbor_Cell_Psc, Neighbor_Cell_SignalStrength,
        // ...

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &skip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &sskip);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &arfcn);
        if (err < 0) goto error;

        if (at_tok_hasmore(&line)) {
            err = at_tok_nextint(&line, &psc);
            if (err < 0) goto error;

            err = at_tok_nextint(&line, &cellIdNumber);
            if (err < 0) goto error;
        }

        totalNumber = cellIdNumber + 1;
        response = (RIL_CellInfo_v1_4 *)
                calloc(totalNumber, sizeof(RIL_CellInfo_v1_4));
        if (response == NULL) {
            RLOGE("Failed to calloc memory for response");
            goto error;
        }

        response[0].CellInfo.wcdma.cellIdentityWcdma.mcc = mcc;
        response[0].CellInfo.wcdma.cellIdentityWcdma.mnc = mnc;
        response[0].CellInfo.wcdma.cellIdentityWcdma.mnc_digit = mnc_digit;
        response[0].CellInfo.wcdma.cellIdentityWcdma.lac = lac;
        response[0].CellInfo.wcdma.cellIdentityWcdma.cid = cid;
        response[0].CellInfo.wcdma.cellIdentityWcdma.psc = psc;
        response[0].CellInfo.wcdma.cellIdentityWcdma.uarfcn = arfcn;

        response[0].CellInfo.wcdma.signalStrengthWcdma.bitErrorRate = biterr3G;
        response[0].CellInfo.wcdma.signalStrengthWcdma.signalStrength = sig3G;

        if (at_tok_hasmore(&line)) {
            for (current = 0; at_tok_hasmore(&line), current < cellIdNumber;
                    current++) {
                err = at_tok_nextint(&line, &arfcn);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &psc);
                if (err < 0) goto error;

                err = at_tok_nextint(&line, &sig3G);
                if (err < 0) goto error;

                signalStrength = (sig3G - 3) / 2;

                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.mcc = INT_MAX;
                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.mnc = INT_MAX;
                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.lac = INT_MAX;
                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.cid = INT_MAX;
                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.psc = psc;
                response[current + 1].CellInfo.wcdma.cellIdentityWcdma.uarfcn = arfcn;

                response[current + 1].CellInfo.wcdma.signalStrengthWcdma.bitErrorRate = INT_MAX;
                response[current + 1].CellInfo.wcdma.signalStrengthWcdma.signalStrength =
                        signalStrength > 31 ? 31 : (signalStrength < 0 ? 0 : signalStrength);

                response[current + 1].isRegistered = 0;
                response[current + 1].cellInfoType = cellType;
                response[current + 1].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
                response[current + 1].timeStamp = INT_MAX;
            }
        }
    }

    uint64_t curTime = ril_nano_time();
    if (registered == 1 || registered == 5) {
        registered = 1;
    }
    response[0].isRegistered = registered;
    response[0].cellInfoType = cellType;
    response[0].timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
    response[0].timeStamp = curTime - 1000;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
            totalNumber * sizeof(RIL_CellInfo_v1_4));

    at_response_free(p_response);
    at_response_free(p_newResponse);
    free(response);
    return;

error:
    at_response_free(p_response);
    at_response_free(p_newResponse);
    free(response);

    RIL_onRequestComplete(t, RIL_E_NO_NETWORK_FOUND, NULL, 0);
}

static void requestShutdown(RIL_SOCKET_ID socket_id,  void *data,
                            size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    if (all_calls(socket_id, 1)) {
        at_send_command(socket_id, "ATH", NULL);
    }

    if (s_radioState[socket_id] != RADIO_STATE_OFF) {
        at_send_command(socket_id, "AT+SFUN=5", NULL);
    }

    at_send_command(socket_id, "AT+SPFLUSHNV", NULL);

    at_send_command(socket_id, "AT+SFUN=3", NULL);

    setRadioState(socket_id, RADIO_STATE_UNAVAILABLE);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
}

static void requestGetRadioCapability(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    RIL_RadioCapability *rc = (RIL_RadioCapability *)calloc(1,
            sizeof(RIL_RadioCapability));
    rc->version = RIL_RADIO_CAPABILITY_VERSION;
    rc->session = 0;
    rc->phase = RC_PHASE_CONFIGURED;
    rc->rat = getRadioFeatures(socket_id);
    rc->status = RC_STATUS_NONE;
    if (socket_id == s_multiModeSim) {
        strncpy(rc->logicalModemUuid, "com.unisoc.modem_multiMode",
                sizeof("com.unisoc.modem_multiMode"));
    } else {
        strncpy(rc->logicalModemUuid, "com.unisoc.modem_singleMode",
                sizeof("com.unisoc.modem_singleMode"));
    }
    RLOGD("getRadioCapability rat = %d, logicalModemUuid = %s", rc->rat,
            rc->logicalModemUuid);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, rc, sizeof(RIL_RadioCapability));
    free(rc);
}

void setWorkMode() {
    int workMode = 0;
    int singleModeSim = 0;
    char numToStr[ARRAY_SIZE] = {0};

#if (SIM_COUNT == 2)
    if (s_multiModeSim == RIL_SOCKET_1) {
        singleModeSim = RIL_SOCKET_2;
    } else if (s_multiModeSim == RIL_SOCKET_2) {
        singleModeSim = RIL_SOCKET_1;
    }
#endif

    pthread_mutex_lock(&s_workModeMutex);

    workMode = s_workMode[singleModeSim];
    s_workMode[singleModeSim] = s_workMode[s_multiModeSim];
    s_workMode[s_multiModeSim] = workMode;

#if defined (ANDROID_MULTI_SIM)
#if (SIM_COUNT == 2)
    snprintf(numToStr, sizeof(numToStr), "%d,%d", s_workMode[RIL_SOCKET_1],
            s_workMode[RIL_SOCKET_2]);
#endif
#else
    snprintf(numToStr, sizeof(numToStr), "%d,10", s_workMode[RIL_SOCKET_1]);
#endif

    RLOGD("setWorkMode: %s", numToStr);
    property_set(MODEM_WORKMODE_PROP, numToStr);
    pthread_mutex_unlock(&s_workModeMutex);
}

#if defined (ANDROID_MULTI_SIM)
/*
 * return :  0: set radio capability failed;
 *           1: send AT SPTESTMODE,wait for async unsol response;
 *           2: no change,return success
 */
static int applySetRadioCapability(RIL_RadioCapability *rc,
                                   RIL_SOCKET_ID socket_id, RIL_Token t) {
    int err = -1;
    int retryTimes = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    char numToStr[ARRAY_SIZE] = {0};
    ATResponse *p_response = NULL;

    if (isMaxRat(rc->rat)) {
        if (s_multiModeSim != socket_id) {
            s_multiModeSim = socket_id;
        } else {
            return 2;
        }
    } else {
#if (SIM_COUNT == 2)
        if (s_multiModeSim == socket_id) {
            s_multiModeSim = 1 - socket_id;
        } else {
            return 2;
        }
#endif
    }
    if (SIM_COUNT <= 2) {
        snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,1", s_multiModeSim);
        at_send_command(socket_id, cmd, NULL);
    }
    snprintf(numToStr, sizeof(numToStr), "%d", s_multiModeSim);
    property_set(PRIMARY_SIM_PROP, numToStr);
    RLOGD("applySetRadioCapability: multiModeSim %d", s_multiModeSim);

    setWorkMode();
#if (SIM_COUNT == 2)
    snprintf(cmd, sizeof(cmd), "AT+SPTESTMODE=%d,%d", s_workMode[RIL_SOCKET_1],
            s_workMode[RIL_SOCKET_2]);
#elif (SIM_COUNT == 1)
    snprintf(cmd, sizeof(cmd), "AT+SPTESTMODE=%d,10", s_workMode[RIL_SOCKET_1]);
#endif
    if (s_modemConfig == NRLWG_LWG) {
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd), ",%d", s_multiModeSim);
    }
    const char *respCmd = "+SPTESTMODE:";
    RIL_RadioCapability *responseRc = (RIL_RadioCapability *)malloc(
            sizeof(RIL_RadioCapability));
    memcpy(responseRc, rc, sizeof(RIL_RadioCapability));

again:
    enqueueAsyncCmdMessage(socket_id, t, respCmd, responseRc,
            asyncCmdTimedCallback, 120);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        if (retryTimes < 3 && p_response != NULL &&
                strcmp(p_response->finalResponse, "+CME ERROR: 3") == 0) {
            RLOGE("AT+SPTESTMODE return +CME ERROR: 3, try again");
            retryTimes++;
            AT_RESPONSE_FREE(p_response);
            removeAsyncCmdMessage(t);
            sleep(3);
            goto again;
        } else {
            removeAsyncCmdMessage(t);
            goto error;
        }
    }
    AT_RESPONSE_FREE(p_response);
    return 1;

error:
    AT_RESPONSE_FREE(p_response);
    return 0;
}

static void requestSetRadioCapability(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    RIL_RadioCapability rc;
    memcpy(&rc, data, sizeof(RIL_RadioCapability));
    RLOGD("requestSetRadioCapability : %d, %d, %d, %d, %s, %d, rild:%d",
            rc.version, rc.session, rc.phase, rc.rat, rc.logicalModemUuid,
            rc.status, socket_id);

    RIL_RadioCapability *responseRc = (RIL_RadioCapability *)malloc(
            sizeof(RIL_RadioCapability));
    memcpy(responseRc, &rc, sizeof(RIL_RadioCapability));

    switch (rc.phase) {
        case RC_PHASE_START:
            s_sessionId[socket_id] = rc.session;
            s_radioAccessFamily[socket_id] = rc.rat;
            RLOGD("requestSetRadioCapability RC_PHASE_START");
            responseRc->status = RC_STATUS_SUCCESS;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, responseRc,
                    sizeof(RIL_RadioCapability));
            break;
        case RC_PHASE_FINISH:
            RLOGD("requestSetRadioCapability RC_PHASE_FINISH");
            s_sessionId[socket_id] = 0;
            responseRc->phase = RC_PHASE_CONFIGURED;
            responseRc->status = RC_STATUS_SUCCESS;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, responseRc,
                    sizeof(RIL_RadioCapability));
            break;
        case RC_PHASE_APPLY: {
            int simId = 0;
            int ret = -1;
            s_sessionId[socket_id] = rc.session;
            responseRc->status = RC_STATUS_SUCCESS;
            s_requestSetRC[socket_id] = 1;
            for (simId = 0; simId < SIM_COUNT; simId++) {
                if (s_requestSetRC[simId] != 1) {
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseRc,
                            sizeof(RIL_RadioCapability));
                    goto exit;
                }
            }
            for (simId = 0; simId < SIM_COUNT; simId++) {
                s_requestSetRC[simId] = 0;
            }

#if (SIM_COUNT == 2)
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_1]);
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
            ret = applySetRadioCapability(responseRc, socket_id, t);
            if (ret <= 0) {
#if (SIM_COUNT == 2)
                pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_1]);
                pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, responseRc,
                        sizeof(RIL_RadioCapability));
            } else if (ret == 2) {
#if (SIM_COUNT == 2)
                pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_1]);
                pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
                RIL_onRequestComplete(t, RIL_E_SUCCESS, responseRc,
                                      sizeof(RIL_RadioCapability));
                sendUnsolRadioCapability();
            }

            break;
        }
        default:
            s_sessionId[socket_id] = rc.session;
            responseRc->status = RC_STATUS_FAIL;
            RIL_onRequestComplete(t, RIL_E_INVALID_STATE, NULL, 0);
            break;
    }

exit:
    free(responseRc);
}
#endif

void requestUpdateOperatorName(RIL_SOCKET_ID socket_id, void *data,
                               size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    char *plmn = (char *)data;
    char operatorName[ARRAY_SIZE] = {0};

    memset(operatorName, 0, sizeof(operatorName));
    err = updatePlmn(socket_id, -1, (const char *)plmn,
                     operatorName, sizeof(operatorName));
    RLOGD("updated plmn = %s, operatorName = %s", plmn, operatorName);
    if (err == 0) {
        MUTEX_ACQUIRE(s_operatorInfoListMutex[socket_id]);
        OperatorInfoList *pList = s_operatorInfoList[socket_id].next;
        OperatorInfoList *next;
        while (pList != &s_operatorInfoList[socket_id]) {
            next = pList->next;
            if (strcmp(plmn, pList->plmn) == 0) {
                RLOGD("find the plmn, remove it from s_operatorInfoList[%d]!",
                        socket_id);
                pList->next->prev = pList->prev;
                pList->prev->next = pList->next;
                pList->next = NULL;
                pList->prev = NULL;

                free(pList->plmn);
                free(pList->longName);
                free(pList->shortName);
                free(pList);
                break;
            }
            pList = next;
        }
        MUTEX_RELEASE(s_operatorInfoListMutex[socket_id]);
        addToOperatorInfoList(operatorName, operatorName, plmn,
                              &s_operatorInfoList[socket_id],
                              &s_operatorInfoListMutex[socket_id]);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

void requestStartNetworkScan(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    uint32_t i, j, k;
    int type, interval, access, err;
    int pOffset = 0;
    char bands[AT_COMMAND_LEN] = {0};
    char channels[AT_COMMAND_LEN] = {0};
    char cmd[AT_COMMAND_LEN] = {0};
    char extcmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    memset(cmd, 0, sizeof(cmd));

    RIL_NetworkScanRequest_v1_2 *p_scanRequest = (RIL_NetworkScanRequest_v1_2 *)data;
    RIL_RadioAccessSpecifier *p_accessSpecifier = NULL;

    type = p_scanRequest->type;
    interval = p_scanRequest->interval;

    for (i = 0; i < p_scanRequest->specifiers_length; i++) {
        memset(bands, 0, sizeof(bands));
        memset(channels, 0, sizeof(channels));
        access = p_scanRequest->specifiers[i].radio_access_network;
        p_accessSpecifier = &(p_scanRequest->specifiers[i]);
        if (p_accessSpecifier->bands_length <= 0) {
            snprintf(bands, sizeof(bands), "");
        } else {
            switch (access) {
            case GERAN:
                snprintf(bands, sizeof(bands), "%d", p_accessSpecifier->bands.geran_bands[0]);
                for (j = 1; j < p_accessSpecifier->bands_length; j++) {
                    pOffset = strlen(bands);
                    snprintf(bands + pOffset, sizeof(bands) - pOffset, ",%d",
                            p_accessSpecifier->bands.geran_bands[j]);
                }
                break;
            case UTRAN:
                snprintf(bands, sizeof(bands), "%d", p_accessSpecifier->bands.utran_bands[0]);
                for (j = 1; j < p_accessSpecifier->bands_length; j++) {
                    pOffset = strlen(bands);
                    snprintf(bands + pOffset, sizeof(bands) - pOffset, ",%d",
                            p_accessSpecifier->bands.utran_bands[j]);
                }
                break;
            case EUTRAN:
                snprintf(bands, sizeof(bands), "%d", p_accessSpecifier->bands.eutran_bands[0]);
                for (j = 1; j < p_accessSpecifier->bands_length; j++) {
                    pOffset = strlen(bands);
                    snprintf(bands + pOffset, sizeof(bands) - pOffset, ",%d",
                            p_accessSpecifier->bands.eutran_bands[j]);
                }
                break;
            default:
                break;
            }
        }
        if (p_accessSpecifier->channels_length <= 0) {
            snprintf(channels, sizeof(channels), "");
        } else {
            snprintf(channels, sizeof(channels), "%d", p_accessSpecifier->channels[0]);
            for (k = 1; k < p_accessSpecifier->channels_length; k++) {
                pOffset = strlen(channels);
                snprintf(channels + pOffset, sizeof(channels) - pOffset, ",%d",
                        p_accessSpecifier->channels[k]);
            }
        }
        if (i > 0) {
            pOffset = strlen(cmd);
            snprintf(cmd + pOffset, sizeof(cmd) - pOffset, ",%d,\"%s\",\"%s\"", access,
                     bands, channels);
        } else {
            snprintf(cmd, sizeof(cmd), "AT+SPFREQSCAN=%d,\"%s\",\"%s\"", access, bands, channels);
        }
    }

    s_isScanningNetwork = true;
    if (s_modemConfig == NRLWG_LWG) {
        snprintf(extcmd, sizeof(extcmd), "%s, 4,\"\", \"\"", cmd);
        err = at_send_command(socket_id, extcmd, &p_response);
    } else {
        err = at_send_command(socket_id, cmd, &p_response);
    }
    if (err < 0 || p_response->success == 0) {
        s_isScanningNetwork = false;
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    AT_RESPONSE_FREE(p_response);
}

void requestStopNetworkScan(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                            RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    at_send_command(socket_id, "AT+SAC", NULL);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    s_isScanningNetwork = false;
}

void requestSetLocationUpdates(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                               RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int enable = ((int *)data)[0];
    if (enable == 0) {
        at_send_command(socket_id, "AT+CREG=1", NULL);
    } else {
        at_send_command(socket_id, "AT+CREG=2", NULL);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

void requestVoiceRadioTech(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                           RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    int response = 0;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+SPCTEC?",
                                     "+SPCTEC:", &p_response);
    if (err < 0 || p_response->success == 0) {
        at_response_free(p_response);
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }

    char *line = p_response->p_intermediates->line;
    err = at_tok_start(&line);

    if (err >= 0) {
        err = at_tok_nextint(&line, &response);
    }

    setPhoneType(response, socket_id);
    setCESQValue(socket_id, s_isCDMAPhone[socket_id]);
    if (s_isCDMAPhone[socket_id]) {
        response = RADIO_TECH_1xRTT;
    } else {
        response = RADIO_TECH_GSM;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
}

void requestSetEmergencyOnly(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int value = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    bool isRadioStateOn = false;
    ATResponse *p_response = NULL;

    value = ((int *)data)[0];
    snprintf(cmd, sizeof(cmd), "AT+SPECCMOD=%d", value);

    if (value == 1 || value == 0) {
        err = at_send_command(socket_id, cmd, &p_response);
    } else {
        RLOGE("Invalid param value: %d", value);
        goto error;
    }

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    if (isRadioOn(socket_id) == 1) {
        isRadioStateOn = true;
    }

    if (value == 0 && isRadioStateOn) {
        RLOGD("restart protocol stack");
        at_send_command(socket_id, "AT+SFUN=5", NULL);
        at_send_command(socket_id, "AT+SFUN=4", NULL);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetImsUserAgent(RIL_SOCKET_ID socket_id, void *data,
                                   size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    char *sipUserAgent = (char *)data;

    if (data == NULL) {
        RLOGE("sipUserAgent is NULL, return");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    changeEscapeCharacterToSpace(sipUserAgent, strlen(sipUserAgent));
    snprintf(cmd, sizeof(cmd), "AT+SPENGMDVOLTE=22,1,\"%s\"", sipUserAgent);
    err = at_send_command(socket_id, cmd, &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestSetSignalStrengthReportingCriteria(RIL_SOCKET_ID socket_id,
                            void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int ret = -1, n = 0, pOffset = 0;
    int hysteresisMs = 0;
    int hysteresisDb = 0;
    int thresholdsDbmNumber = 0;
    int *thresholdsDbm = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    char tmpCmd[AT_COMMAND_LEN] = {0};
    RIL_RadioAccessNetworks_v1_2 accessNetworks = 0;
    RIL_SignalStrengthReportingCriteria *pCriteria = NULL;

    if (data == NULL) goto done;
    pCriteria = (RIL_SignalStrengthReportingCriteria *)data;

    hysteresisMs = pCriteria->hysteresisMs;
    hysteresisDb = pCriteria->hysteresisDb;
    thresholdsDbmNumber = pCriteria->thresholdsDbmNumber;
    accessNetworks = pCriteria->accessNetwork;

    if (pCriteria->thresholdsDbm == NULL) {
        /* set for vts test case setSignalStrengthReportingCriteria_EmptyParams, which
         * requires function returns success, when thresholdsDbm is equal to NULL */
        RLOGE("thresholdsDbm is NULL");
        ret = 0;
        goto done;
    }
    snprintf(tmpCmd, sizeof(tmpCmd), "%d,%d,%d", hysteresisMs, hysteresisDb, thresholdsDbmNumber);
    thresholdsDbm = pCriteria->thresholdsDbm;
    while (n < thresholdsDbmNumber) {
        /* To judge hysteresisDb，which must be smaller than the smallest threshold delta. */
        if (n > 0) {
            if (hysteresisDb >= *(thresholdsDbm + n) - *(thresholdsDbm + n - 1)
                || hysteresisDb <= 0) {
                RLOGE("invalid hysteresisDb value");
                goto done;
            }
        }
        pOffset = strlen(tmpCmd);
        snprintf(tmpCmd + pOffset, sizeof(tmpCmd) - pOffset, ",%d", *(thresholdsDbm + n));
        n++;
    }

    switch (accessNetworks) {
        case RADIO_ACCESS_NET_GERAN:
            snprintf(cmd, sizeof(cmd), "AT+SPGASDUMMY=\"set gas signal rule\",%s", tmpCmd);
            break;
        case RADIO_ACCESS_NET_UTRAN:
            snprintf(cmd, sizeof(cmd), "AT+SPWASDUMMY=\"set was signal rule\",%s", tmpCmd);
            ret = at_send_command(socket_id, cmd, NULL);
            if (ret < 0) goto done;
            snprintf(cmd, sizeof(cmd), "AT+SPTASDUMMY=\"set tas signal rule\",%s", tmpCmd);
            break;
        case RADIO_ACCESS_NET_EUTRAN:
            snprintf(cmd, sizeof(cmd), "AT+SPLASDUMMY=\"set las signal rule\",%s", tmpCmd);
            break;
        case RADIO_ACCESS_NET_CDMA2000:
            /* set for vts test case setSignalStrengthReportingCriteria_Cdma2000, which
             * requires function returns success */
            ret = 0;
            goto done;
        default:
            goto done;
    }
    ret = at_send_command(socket_id, cmd, NULL);

done:
    if (ret < 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
}

void getLteSpeedAndSignalStrength(RIL_SOCKET_ID socket_id, void *data,
                                    size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1, i = 0;
    char *line = NULL;
    char *resp[ARRAY_SIZE] = {0};
    char temp[ARRAY_SIZE] = {0};
    ATResponse *p_response = NULL;
    RIL_LTE_SpeedAndSignalStrength *response = (RIL_LTE_SpeedAndSignalStrength *)
                calloc(1, sizeof(RIL_LTE_SpeedAndSignalStrength));

    response->rsrp = s_rsrp[socket_id];

    err = at_send_command_numeric(socket_id, "AT+SPENGMD=0,0,6", &p_response);
    if (err < 0 || p_response->success == 0) {
        err = -1;
        goto done;
    }

    line = p_response->p_intermediates->line;
    resp[i] = strsep(&line, "-");
    while(resp[i] != NULL) {
      i++;
      if (strStartsWith(line, "-")) {
          strsep(&line, "-");
          if (i == 2) {
              snprintf(temp, sizeof(temp), "%s%s", "-", strsep(&line, "-"));
              resp[i] = temp;
          } else {
              resp[i] = strsep(&line, "-");
          }
      } else {
          resp[i] = strsep(&line, "-");
      }
      if (line == NULL) break;
    }

    err = at_tok_nextint(&resp[2], &response->snr);
    if (err < 0) goto done;
    err = at_tok_nextint(&resp[10], &response->txSpeed);
    if (err < 0) goto done;
    err = at_tok_nextint(&resp[10], &response->rxSpeed);
    if (err < 0) goto done;
    RLOGD("rsrp snr txSpeed rxSpeed : %d, %d, %d, %d", response->rsrp, response->snr,
            response->txSpeed, response->rxSpeed);

done:
    if (err < 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(RIL_LTE_SpeedAndSignalStrength));
    }
    at_response_free(p_response);
    free(response);
}

void setBit(const unsigned int mode, int flag) {

    pthread_mutex_lock(&s_nrSwitchStatusMutex);
    if (flag) {
        s_NrSwitchStatus |= (unsigned int)(1 << (mode-1));  // mode to 1
    } else {
        s_NrSwitchStatus &= ~(unsigned int)(1 << (mode-1));  // mode to 0
    }
    pthread_mutex_unlock(&s_nrSwitchStatusMutex);
}

void sendEnableNrSwitchCommand(RIL_SOCKET_ID socket_id, int mode, int enable) {
     char cmd[AT_COMMAND_LEN] = {0};

     RLOGD("sendEnableNrSwitchCommand: mode: %d, enable: %d, s_NrSwitchStatus: %d",
             mode, enable, s_NrSwitchStatus);

     setBit(mode, enable);

     RLOGD("sendEnableNrSwitchCommand: s_NrSwitchStatus: %d", s_NrSwitchStatus);
     if ((s_NrSwitchStatus & 0x04) != 0x04) {  // Temperature mode to forced close 5G
         snprintf(cmd, sizeof(cmd), "AT+SPNASDUMMY=\"set enable nr\", 2");
     } else if (s_NrSwitchStatus & 0x20) {  // SPEEDTEST mode to forced open 5G
         snprintf(cmd, sizeof(cmd), "AT+SPNASDUMMY=\"set enable nr\", 3");
     } else if (s_NrSwitchStatus == 0xDF) {
         snprintf(cmd, sizeof(cmd), "AT+SPNASDUMMY=\"set enable nr\", 1");
         if (s_smart5GEnable == 1) {
             //SignalStrength Reporting Criteria for commend to 1
             sendSignalStrengthCriteriaCommend(socket_id, 1);
         }
     } else {
         snprintf(cmd, sizeof(cmd), "AT+SPNASDUMMY=\"set enable nr\", 0");
         if (s_smart5GEnable == 1) {
             //SignalStrength Reporting Criteria for commend to 2
             sendSignalStrengthCriteriaCommend(socket_id, 2);
         }
     }

     at_send_command(socket_id, cmd, NULL);
}

void requestSetStandAlone(int socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+SPCAPABILITY=51,1,%d", ((int *)data)[0]);

    int err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

void requestGetStandAlone(int socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    RIL_UNUSED_PARM(data);

    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    int response = 0;
    int skip = 0;

    snprintf(cmd, sizeof(cmd), "AT+SPCAPABILITY=51,0");

    int err = at_send_command_singleline(socket_id, cmd,
                                         "+SPCAPABILITY:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    char *line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/*
 * this AT command is used to query cesq version to adapt convert3GValueTodBm
 */
void queryCesqVersion(RIL_SOCKET_ID socket_id) {
    char *line = NULL;
    int err = -1;
    int response = 0;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+SPCESQV?",
                                        "+SPCESQV:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0) goto error;

    if (response == 0) {
        s_isCesqNewVersion = false;
    } else {
        s_isCesqNewVersion = true;
    }

error:
    at_response_free(p_response);
}

int processNetworkRequests(int request, void *data, size_t datalen,
                           RIL_Token t, RIL_SOCKET_ID socket_id) {
    int err = -1;

    switch (request) {
        case RIL_REQUEST_SIGNAL_STRENGTH: {
            requestSignalStrength(socket_id, data, datalen, t);
            break;
        }
        case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        case RIL_REQUEST_DATA_REGISTRATION_STATE:
        case RIL_REQUEST_IMS_REGISTRATION_STATE:
            if (s_isCDMAPhone[socket_id] && (request != RIL_REQUEST_IMS_REGISTRATION_STATE)) {
                requestRegistrationStateCDMA(socket_id, request, data, datalen, t);
            } else {
                requestRegistrationState(socket_id, request, data, datalen, t);
            }
            break;
        case RIL_REQUEST_VOICE_RADIO_TECH:
            if (s_isCDMAPhone[socket_id]) {
                requestVoiceRadioTech(socket_id, data, datalen, t);
            } else {
                requestRegistrationState(socket_id, request, data, datalen, t);
            }
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER: {
            pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
            s_desiredRadioState[socket_id] = ((int *)data)[0];
            requestRadioPower(socket_id, data, datalen, t);
            if (t == NULL) {
                free(data);
            }
            pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
            break;
        }
        case RIL_REQUEST_ENABLE_MODEM: {
            pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
            requestEnableModem(socket_id, data, datalen, t);
            pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
            break;
        }
        case RIL_REQUEST_GET_MODEM_STATUS:
            requestGetModemStatus(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            requestSetNetworkSelectionAutomatic(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            requestNetworkRegistration(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS: {
            s_manualSearchNetworkId = socket_id;
            requestNetworkList(socket_id, data, datalen, t);
            s_manualSearchNetworkId = -1;
            break;
        }
        case RIL_REQUEST_RESET_RADIO:
            requestResetRadio(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_BAND_MODE:
            requestSetBandMode(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
            requestGetBandMode(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        case RIL_EXT_REQUEST_SET_PREFERRED_NETWORK_TYPE: {
#if (SIM_COUNT == 2)
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_1]);
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
            err = requestSetPreferredNetType(socket_id, data, datalen, t);
            if (err < 0) {
#if (SIM_COUNT == 2)
            pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_1]);
            pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
            }
            break;
        }
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE_BITMAP: {
#if (SIM_COUNT == 2)
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_1]);
            pthread_mutex_lock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
            int networkMode = getNetworkModeFromRadioAccessFamily(((int *)data)[0]);
            RLOGD("raf = %d, networkMode = %d", ((int *)data)[0], networkMode);
            err = requestSetPreferredNetType(socket_id, (void *)(&networkMode), datalen, t);
            if (err < 0) {
#if (SIM_COUNT == 2)
            pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_1]);
            pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
            }
            break;
        }
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        case RIL_EXT_REQUEST_GET_PREFERRED_NETWORK_TYPE: {
            requestGetPreferredNetType(socket_id, data, datalen, t, NULL);
            break;
        }
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP: {
            int networkMode = -1;
            requestGetPreferredNetType(socket_id, data, datalen, NULL, &networkMode);
            if (networkMode != -1) {
                int raf = getRadioAccessFamilyFromNetworkMode(networkMode);
                RLOGD("networkMode = %d, raf = %d", networkMode, raf);
                RIL_onRequestComplete(t, RIL_E_SUCCESS, &raf, sizeof(raf));
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
            requestNeighboaringCellIds(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_LOCATION_UPDATES:
            requestSetLocationUpdates(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GET_CELL_INFO_LIST:
            requestGetCellInfoList(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        case RIL_REQUEST_SHUTDOWN:
        case RIL_EXT_REQUEST_SHUTDOWN:
            requestShutdown(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GET_RADIO_CAPABILITY:
            requestGetRadioCapability(socket_id, data, datalen, t);
            break;
#if defined (ANDROID_MULTI_SIM)
        case RIL_REQUEST_SET_RADIO_CAPABILITY: {
            char prop[PROPERTY_VALUE_MAX];

            property_get(FIXED_SLOT_PROP, prop, "false");
            if (strcmp(prop, "true") == 0) {
                RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            } else {
                requestSetRadioCapability(socket_id, data, datalen, t);
            }
            break;
        }
#endif
        case RIL_EXT_REQUEST_GET_IMS_BEARER_STATE:
            RIL_onRequestComplete(t, RIL_E_SUCCESS,
                    (void *)&s_imsBearerEstablished[socket_id], sizeof(int));
            break;
        case RIL_EXT_REQUEST_UPDATE_OPERATOR_NAME: {
            requestUpdateOperatorName(socket_id, data, datalen, t);
            break;
        }
        case RIL_REQUEST_START_NETWORK_SCAN: {
            requestStartNetworkScan(socket_id, data, datalen, t);
            break;
        }
        case RIL_REQUEST_STOP_NETWORK_SCAN: {
            requestStopNetworkScan(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_SET_EMERGENCY_ONLY: {
            pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
            requestSetEmergencyOnly(socket_id, data, datalen, t);
            pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
            break;
        }
        case RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA: {
            if(!s_setSignalStrengthReporting) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            } else {
                /* The function is not perfect. It is off by default */
                requestSetSignalStrengthReportingCriteria(socket_id, data, datalen, t);
            }
            break;
        }
        case RIL_EXT_REQUEST_SET_IMS_USER_AGENT: {
            requestSetImsUserAgent(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_LTE_SPEED_AND_SIGNAL_STRENGTH: {
            getLteSpeedAndSignalStrength(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_ENABLE_NR_SWITCH: {
            int mode = ((int *)data)[0];
            int enable = ((int *)data)[1];
            sendEnableNrSwitchCommand(socket_id, mode, enable);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_EXT_REQUEST_SET_STAND_ALONE: {
            requestSetStandAlone(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_GET_STAND_ALONE: {
            requestGetStandAlone(socket_id, data, datalen, t);
            break;
        }
        default:
            return 0;
    }

    return 1;
}

static void radioPowerOnTimeout(void *param) {
    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }
    setRadioState(socket_id, RADIO_STATE_ON);
}

#if 0  // unused function
static void setPropForAsync(void *param) {
    SetPropPara *propPara = (SetPropPara *)param;
    if (propPara == NULL) {
        RLOGE("Invalid param");
        return;
    }
    RLOGD("setprop socketId:%d,name:%s value:%s mutex:%p",
            propPara->socketId, propPara->propName, propPara->propValue, propPara->mutex);
    pthread_mutex_lock(propPara->mutex);
    setProperty(propPara->socketId, propPara->propName, propPara->propValue);
    pthread_mutex_unlock(propPara->mutex);
    RLOGD("setprop complete");
    free(propPara->propName);
    free(propPara->propValue);
    free(propPara);
}
#endif

static void processNetworkName(char *longName, char *shortName, int mcc, int mnc,
                               int mnc_digit, int lac, RIL_SOCKET_ID socket_id) {
    int err = -1;
    char mccmnc[8] = {0};
    char mcc_str[8] = {0};
    char mnc_str[8] = {0};
    char strFormat[8] = {0};
    char longNameTmp[ARRAY_SIZE] = {0};
    char shortNameTmp[ARRAY_SIZE] = {0};
    char newLongName[ARRAY_SIZE] = {0};
    snprintf(mcc_str, sizeof(mcc_str), "%03d", mcc);
    if (mnc_digit == 2 || mnc_digit == 3) {
        snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
        snprintf(mnc_str, mnc_digit + 1, strFormat, mnc);
    }
    snprintf(mccmnc, sizeof(mccmnc), "%s%s", mcc_str, mnc_str);
    if (strcmp(s_nitzOperatorInfo[socket_id], "")) {
        err = matchOperatorInfo(longName, shortName, mccmnc, s_nitzOperatorInfo[socket_id]);
    }
    if (err != 0) {
        pthread_mutex_lock(&s_operatorInfoMutex);
        err = getOperatorName(longName, shortName, mccmnc,
                              &s_operatorXmlInfoList,
                              &s_operatorXmlInfoListMutex);
        if (err != 0) {
            err = RIL_getONS(longName, shortName, mccmnc);
            if (err != 0 && mnc_digit == 3 && (mnc >= 0 && mnc <= 99 )) {
                if (mnc >= 0 && mnc <= 9) {
                    snprintf(mnc_str, sizeof(mnc_str), "%02d", mnc);
                } else if (mnc >= 10 && mnc <= 99) {
                    snprintf(mnc_str, sizeof(mnc_str), "%d", mnc);
                }
                snprintf(strFormat, sizeof(strFormat), "%s%s", mcc_str, mnc_str);
                err = RIL_getONS(longName, shortName, strFormat);
            }
            if (err == 0) {
                addToOperatorInfoList(longName, shortName, mccmnc,
                                      &s_operatorXmlInfoList,
                                      &s_operatorXmlInfoListMutex);
            }
        }
        pthread_mutex_unlock(&s_operatorInfoMutex);
    }
    RLOGD("get network longName: %s, shortName: %s", longName, shortName);

    err = getOperatorName(longNameTmp, shortNameTmp, mccmnc,
                          &s_operatorInfoList[socket_id],
                          &s_operatorInfoListMutex[socket_id]);
    if (err != 0) {
        err = updatePlmn(socket_id, lac, (const char *)mccmnc,
                         newLongName, sizeof(newLongName));
        if (err == 0 && strcmp(newLongName, "")) {
            RLOGD("updated Operator name: %s", newLongName);
            memcpy(longName, newLongName, strlen(newLongName) + 1);
            addToOperatorInfoList(newLongName, newLongName, mccmnc,
                                  &s_operatorInfoList[socket_id],
                                  &s_operatorInfoListMutex[socket_id]);
        }
    } else if (strcmp(longNameTmp, "") && strcmp(shortNameTmp, "")) {
        memcpy(longName, longNameTmp, strlen(longNameTmp) + 1);
        memcpy(longName, shortNameTmp, strlen(shortNameTmp) + 1);
    }
    RLOGD("get network longName = %s, shortName = %s", longName, shortName);
}

static int getPlmnCount(char *str) {
    int num = 0;
    if (str == NULL || *str == '-') {
        return num;
    }
    while (*str != '\0') {
        if (*str == '-' && *(str - 1) != ',') {
            num++;
        }
        str++;
    }
    return num;
}
static void processNetworkScanResults(void *param) {
    int err = -1;
    int rat = -1, cell_num = 0, bsic = 0, mcc = 0, mnc = 0;
    int mnc_digit = 0, lac = 0;
    int current = 0, skip;
    char *tmp = NULL;
    char *endptr = NULL;
    char *ciptr = NULL;
    int rsrp = 0, rsrq = 0;
    RIL_NetworkScanResult_v1_4 *scanResult = NULL;
    CallbackPara *cbPara = (CallbackPara *)param;

    RIL_SOCKET_ID socket_id = cbPara->socket_id;
    tmp = (char *)cbPara->para;
    if (tmp == NULL) {
        RLOGE("Invalid param, return");
        goto out;
    }

    // +SPFREQSCAN: 255 /+SPFREQSCAN: 254 --complete
    // +SPFREQSCAN: 0-2-1,20,9,11,460,44,2,1-5,512,9,11,460,44,2,1
    // +SPFREQSCAN: 1-1-2,10700,200,9,-27,460,44,2,1
    // +SPFREQSCAN: 2-1-1,9596,0,62,-96,460,44,2,1
    // +SPFREQSCAN: 3-3-0,0,38000,-8600,-1500,460,44,2,1-2,2,38200,-12100,-1500,460,44,2,2-6,6,1250,-12100,-1500,460,44,2,2
    // GSM: 0-cell_num-cid,arfcn,basic,rssi,mcc,mnc,mnc_digit,lac-cid,….
    // WCDMA：1-cell_num-cid,uarfcn,psc,rssi,ecio,mcc,mnc,mnc_digital,lac-cid…
    // TD：2-cell_num-cid,uarfcn,psc,rssi,rscp,mcc,mnc,mnc_digit,lac-cid…
    // LTE：3-cell_num-cid,pcid,arfcn, rsrp,rsrq, mcc,mnc,mnc_digit,tac-cid…
    // NR:4-cell_num-cid,pci,arfcn,rsrp,rsrq,mcc,mnc,mnc_digit,tac-cid…
    rat = atoi(tmp);
    RLOGD("network scan rat = %d", rat);
    scanResult = (RIL_NetworkScanResult_v1_4 *)calloc(1, sizeof(RIL_NetworkScanResult_v1_4));
    if (rat < 0 || rat == 255 || rat == 254) {
        scanResult->status = COMPLETE;
        s_isScanningNetwork = false;
        RIL_onUnsolicitedResponse(RIL_UNSOL_NETWORK_SCAN_RESULT, scanResult,
                                  sizeof(RIL_NetworkScanResult_v1_4), socket_id);
        goto out;
    }
    strsep(&tmp, "-");
    if (tmp == NULL) {
        RLOGE("network scan param error");
        scanResult->status = COMPLETE;
        RIL_onUnsolicitedResponse(RIL_UNSOL_NETWORK_SCAN_RESULT, scanResult,
                                  sizeof(RIL_NetworkScanResult_v1_4), socket_id);
        goto out;
    } else {
        cell_num = getPlmnCount(tmp);
        RLOGD("network scan cell_num = %d", cell_num);
        if (cell_num == 0) {
            RLOGE("network scan plmn count error");
            goto out;
        }
    }
    scanResult->status = PARTIAL;
    scanResult->networkInfos = (RIL_CellInfo_v1_4 *)calloc(cell_num, sizeof(RIL_CellInfo_v1_4));
    scanResult->network_infos_length = cell_num;
    char *longName = (char *)alloca(cell_num * sizeof(char) * ARRAY_SIZE);
    char *shortName = (char *)alloca(cell_num * sizeof(char) * ARRAY_SIZE);
    memset(longName, 0, cell_num * sizeof(char) * ARRAY_SIZE);
    memset(shortName, 0, cell_num * sizeof(char) * ARRAY_SIZE);
    switch (rat) {
        case 0:  // GSM
            for (current = 0; current < cell_num; current++) {
                tmp = strchr(tmp, '-');
                if (tmp == NULL) goto out;
                tmp++;
                scanResult->networkInfos[current].cellInfoType = RIL_CELL_INFO_TYPE_GSM;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.cid);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.arfcn);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &bsic);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.bsic = (uint8_t)bsic;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.gsm.signalStrengthGsm.signalStrength);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &mcc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.mcc = mcc;

                err = at_tok_nextint(&tmp, &mnc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.mnc = mnc;

                err = at_tok_nextint(&tmp, &mnc_digit);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.mnc_digit = mnc_digit;
                if (tmp == NULL) goto out;
                lac = atoi(tmp);
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.lac = lac;

                processNetworkName(longName, shortName, mcc, mnc, mnc_digit, lac, socket_id);
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.operatorNames.alphaLong = longName;
                scanResult->networkInfos[current].CellInfo.gsm.cellIdentityGsm.operatorNames.alphaShort = shortName;
                longName += ARRAY_SIZE;
                shortName += ARRAY_SIZE;
            }
            break;
        case 1:  // WCDMA
            for (current = 0; current < cell_num; current++) {
                scanResult->networkInfos[current].cellInfoType = RIL_CELL_INFO_TYPE_WCDMA;
                tmp = strchr(tmp, '-');
                if (tmp == NULL) goto out;
                tmp++;
                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.cid);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.uarfcn);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.psc);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.wcdma.signalStrengthWcdma.signalStrength);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp,&skip);  // ecio
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &mcc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.mcc = mcc;

                err = at_tok_nextint(&tmp, &mnc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.mnc = mnc;

                err = at_tok_nextint(&tmp, &mnc_digit);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.mnc_digit = mnc_digit;
                if (tmp == NULL) goto out;
                lac = atoi(tmp);
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.lac = lac;

                processNetworkName(longName, shortName, mcc, mnc, mnc_digit, lac, socket_id);
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.operatorNames.alphaLong = longName;
                scanResult->networkInfos[current].CellInfo.wcdma.cellIdentityWcdma.operatorNames.alphaShort = shortName;
                longName += ARRAY_SIZE;
                shortName += ARRAY_SIZE;
            }
            break;
        case 2:  // TD
            for (current = 0; current < cell_num; current++) {
                scanResult->networkInfos[current].cellInfoType = RIL_CELL_INFO_TYPE_TD_SCDMA;
                tmp = strchr(tmp, '-');
                if (tmp == NULL) goto out;
                tmp++;
                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.cid);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &skip);  // uarfcn
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &skip);  // psc
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &skip);  // rssi
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, (int *)&scanResult->networkInfos[current].CellInfo.tdscdma.signalStrengthTdscdma.rscp);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &mcc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.mcc = mcc;

                err = at_tok_nextint(&tmp, &mnc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.mnc = mnc;

                err = at_tok_nextint(&tmp, &mnc_digit);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.mnc_digit = mnc_digit;
                if (tmp == NULL) goto out;
                lac = atoi(tmp);
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.lac = lac;

                processNetworkName(longName, shortName, mcc, mnc, mnc_digit, lac, socket_id);
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.operatorNames.alphaLong = longName;
                scanResult->networkInfos[current].CellInfo.tdscdma.cellIdentityTdscdma.operatorNames.alphaShort = shortName;
                longName += ARRAY_SIZE;
                shortName += ARRAY_SIZE;
            }
            break;
        case 3: //LTE
            for (current = 0; current < cell_num; current++) {
                scanResult->networkInfos[current].cellInfoType = RIL_CELL_INFO_TYPE_LTE;
                tmp = strchr(tmp, '-');
                if (tmp == NULL) goto out;
                tmp++;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.ci);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.pci);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.earfcn);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &rsrp);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.lte.base.signalStrengthLte.rsrp = rsrp / (-100);

                err = at_tok_nextint(&tmp, &rsrq);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.lte.base.signalStrengthLte.rsrq = rsrq / (-100);

                err = at_tok_nextint(&tmp, &mcc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.mcc = mcc;

                err = at_tok_nextint(&tmp, &mnc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.mnc = mnc;

                err = at_tok_nextint(&tmp, &mnc_digit);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.mnc_digit = mnc_digit;

                scanResult->networkInfos[current].CellInfo.lte.base.signalStrengthLte.rssnr = INT_MAX;
                if (tmp == NULL) goto out;
                lac = atoi(tmp);
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.tac = lac;

                processNetworkName(longName, shortName, mcc, mnc, mnc_digit, lac, socket_id);
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.operatorNames.alphaLong = longName;
                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.operatorNames.alphaShort = shortName;
                longName += ARRAY_SIZE;
                shortName += ARRAY_SIZE;

                scanResult->networkInfos[current].CellInfo.lte.base.cellIdentityLte.bandwidth = INT_MAX;
            }
            break;
        case 4: //NR
            for (current = 0; current < cell_num; current++) {
                scanResult->networkInfos[current].cellInfoType = RIL_CELL_INFO_TYPE_NR;
                tmp = strchr(tmp, '-');
                if (tmp == NULL) goto out;
                tmp++;

                err = at_tok_nextstr(&tmp, &ciptr);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.nci = strtol(ciptr, &endptr, 16);

                err = at_tok_nextint(&tmp, (int *)&scanResult->networkInfos[current].CellInfo.nr.cellidentity.pci);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &scanResult->networkInfos[current].CellInfo.nr.cellidentity.nrarfcn);
                if (err < 0) goto out;

                err = at_tok_nextint(&tmp, &rsrp);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.signalStrength.ssRsrp = rsrp / (-100);

                err = at_tok_nextint(&tmp, &rsrq);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.signalStrength.ssRsrq = rsrq / (-100);

                err = at_tok_nextint(&tmp, &mcc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.mcc = mcc;

                err = at_tok_nextint(&tmp, &mnc);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.mnc = mnc;

                err = at_tok_nextint(&tmp, &mnc_digit);
                if (err < 0) goto out;
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.mnc_digit = mnc_digit;

                if (tmp == NULL) goto out;
                lac = atoi(tmp);
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.tac = lac;

                processNetworkName(longName, shortName, mcc, mnc, mnc_digit, lac, socket_id);
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.operatorNames.alphaLong = longName;
                scanResult->networkInfos[current].CellInfo.nr.cellidentity.operatorNames.alphaShort = shortName;
                longName += ARRAY_SIZE;
                shortName += ARRAY_SIZE;
            }
            break;
        default:
            break;
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_NETWORK_SCAN_RESULT, scanResult,
                              sizeof(RIL_NetworkScanResult_v1_4), socket_id);

out:
    if (scanResult != NULL) {
        FREEMEMORY(scanResult->networkInfos);
        FREEMEMORY(scanResult);
    }
    FREEMEMORY(cbPara->para);
    FREEMEMORY(cbPara);
}

static void skipNextLeftBracket(char **p_cur) {
    if (*p_cur == NULL) return;

    while (**p_cur != '\0' && **p_cur != '(') {
        (*p_cur)++;
    }

    if (**p_cur == '(') {
        (*p_cur)++;
    }
}

static void processPhysicalChannelConfigs(char *line, RIL_SOCKET_ID socket_id) {
    int err = -1;
    int bracket, i = 0;
    int range = -1,rat = -1;
    char *p = NULL;
    RIL_PhysicalChannelConfig_v1_4 *config = NULL;

    if (line == NULL) {
        RLOGE("processPhysicalChannelConfigs Invalid param, return");
        return;
    }

    /**
     * 1. only LTE cell: +SPNRCHANNEL:(lte_phycellid,lte_arfcn,lte_rat,lte_bandwidth)
     * 2. only NR cell:  +SPNRCHANNEL:(nr_phycellid,nr_arfcn,nr_rat,nr_bandwidth)
     * 3. LTE primary cell && NR secondary cell:
     *        +SPNRCHANNEL:(lte_phycellid,lte_arfcn,lte_rat,lte_bandwidth),(nr_phycellid,nr_arfcn,nr_rat,nr_bandwdith)
     **/
    bracket = 0;
    for (p = line; *p != '\0'; p++) {
        if (*p == '(') bracket++;
    }

    config = (RIL_PhysicalChannelConfig_v1_4 *)calloc(bracket, sizeof(RIL_PhysicalChannelConfig_v1_4));
    p = line;
    for (i = 0; i < bracket; i++) {
        skipNextLeftBracket(&p);

        config[i].base.status = (i == 1) ? 2 : 1; // "1" means PRIMARY_SERVING, and "2" means SECONDARY_SERVING

        err = at_tok_nextint(&p, (int *)&(config[i].physicalCellId));
        if (err < 0) goto out;

        err = at_tok_nextint(&p, &range);
        if (err < 0) goto out;

        err = at_tok_nextint(&p, &rat);
        if (err < 0) goto out;
        config[i].rat = mapCgregResponse(rat);

        // only nr mode need to convert
        if (config[i].rat == RADIO_TECH_NR) {
            config[i].rfInfo.range = convertToRrequencyRange(range);
        } else {
            config[i].rfInfo.range = FREQUENCY_RANGE_LOW;
        }

        err = at_tok_nextint(&p, &(config[i].base.cellBandwidthDownlink));
        if (err < 0) goto out;
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_PHYSICAL_CHANNEL_CONFIG, config,
            bracket * sizeof(RIL_PhysicalChannelConfig_v1_4), socket_id);
out:
    free(config);
}

int processNetworkUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    char *line = NULL;
    int err;

    if (strStartsWith(s, "+CESQ:")) {
        RIL_SignalStrength_v1_4 responseV1_4;
        char *tmp;
        int response[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
        char newLine[AT_COMMAND_LEN];

        line = strdup(s);
        tmp = line;

        triggerSignalProcess();
        err = cesq_unsol_rsp(tmp, socket_id, newLine);
        if (err == 0) {
            RIL_SIGNALSTRENGTH_INIT_1_4(responseV1_4);

            tmp = newLine;
            at_tok_start(&tmp);

            err = at_tok_nextint(&tmp, &response[0]);
            if (err < 0) goto out;
            err = at_tok_nextint(&tmp, &response[1]);
            if (err < 0) goto out;
            err = at_tok_nextint(&tmp, &response[2]);
            if (err < 0) goto out;
            err = at_tok_nextint(&tmp, &response[3]);
            if (err < 0) goto out;
            err = at_tok_nextint(&tmp, &response[4]);
            if (err < 0) goto out;
            err = at_tok_nextint(&tmp, &response[5]);
            if (err < 0) goto out;

            if (s_modemConfig == NRLWG_LWG) {
                err = at_tok_nextint(&tmp, &response[6]);
                if (err < 0) goto out;
                err = at_tok_nextint(&tmp, &response[7]);
                if (err < 0) goto out;
                err = at_tok_nextint(&tmp, &response[8]);
                if (err < 0) goto out;
            }

            if (!s_isCDMAPhone[socket_id]) {
                if (response[0] != -1 && response[0] != 99 && response[0] != 255) {
                    responseV1_4.gsm.signalStrength = response[0];
                }
                if (response[2] != -1 && response[2] != 255) {  // response[2] is cp reported 3G value
                    int dBm = convert3GValueTodBm(response[2]);
                    responseV1_4.wcdma.signalStrength = getWcdmaSigStrengthBydBm(dBm);
                    responseV1_4.wcdma.rscp = getWcdmaRscpBydBm(dBm);
                    responseV1_4.wcdma.bitErrorRate = response[1];
                    responseV1_4.wcdma.ecno = response[3];
                }
                if (response[7] != -1 && response[7] != 255 && response[7] != -255) {
                    responseV1_4.nr.ssRsrp = response[7];
                }
            } else {
                if (response[0] != -1 && response[0] != 255) {
                    responseV1_4.cdma.dbm = response[0];
                }
                if (response[1] != -1 && response[1] != 255) {
                    responseV1_4.cdma.ecio = response[1];
                }
                if (response[2] != -1 && response[2] != 255) {
                    responseV1_4.evdo.dbm = response[2];
                }
                if (response[3] != -1 && response[3] != 255) {
                    responseV1_4.evdo.signalNoiseRatio = response[3];
                }
            }
            if (response[5] != -1 && response[5] != 255 && response[5] != -255) {
                responseV1_4.lte.rsrp = response[5];
            }
            if (response[7] != -1 && response[7] != 255 && response[7] != -255) {
                responseV1_4.nr.ssRsrp = response[7];
            }
            RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &responseV1_4,
                                      sizeof(RIL_SignalStrength_v1_4), socket_id);
        }
    } else if (strStartsWith(s, "+CREG:") ||
                strStartsWith(s, "+CCREG:")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);
        if (s_radioOnError[socket_id] && s_radioState[socket_id] == RADIO_STATE_OFF) {
            RLOGD("Radio is on, setRadioState now.");
            s_radioOnError[socket_id] = false;
            RIL_requestTimedCallback(radioPowerOnTimeout,
                                     (void *)&s_socketId[socket_id], NULL);
        }
    } else if (strStartsWith(s, "+CGREG:") ||
               strStartsWith(s, "+CCGREG:")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);
    } else if (strStartsWith(s, "+CEREG:")) {
        char *p, *tmp;
        int lteState;
        int commas = 0;
        int netType = -1;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        for (p = tmp; *p != '\0'; p++) {
            if (*p == ',') commas++;
        }
        err = at_tok_nextint(&tmp, &lteState);
        if (err < 0) goto out;

        if (commas == 0 && lteState == 0) {
            s_in4G[socket_id] = 0;
            if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                s_LTEDetached[socket_id] = true;
            }
        }

        if (lteState == 1 || lteState == 5) {
            if (commas >= 3) {
                skipNextComma(&tmp);
                skipNextComma(&tmp);
                err = at_tok_nextint(&tmp, &netType);
                if (err < 0) goto out;
            }
            is4G(netType, -1, socket_id);
            RLOGD("netType is %d", netType);
            pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
            if (s_PSRegState[socket_id] == STATE_OUT_OF_SERVICE) {
                s_PSRegState[socket_id] = STATE_IN_SERVICE;
            }
            pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            RLOGD("s_PSRegState is IN SERVICE");
        } else {
            pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
            if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                s_PSRegState[socket_id] = STATE_OUT_OF_SERVICE;
            }
            pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
            RLOGD("s_PSRegState is OUT OF SERVICE.");
            cancelTimerOperation(socket_id);
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);
    } else if (strStartsWith(s, "+C5GREG:")) {
        char *p = NULL, *tmp = NULL;
        int regState;
        int commas = 0;
        int netType = -1;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        for (p = tmp; *p != '\0'; p++) {
            if (*p == ',') commas++;
        }

        err = at_tok_nextint(&tmp, &regState);
        if (err < 0) goto out;

        if (regState != 4 && s_isNR) {
            s_isSA[socket_id] = true;

            if (commas == 0 && regState == 0) {
                s_in4G[socket_id] = 0;
                if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                    s_LTEDetached[socket_id] = true;
                }
            }

            if (regState == 1 || regState == 5) {
                if (commas >= 3) {
                    skipNextComma(&tmp);
                    skipNextComma(&tmp);
                    err = at_tok_nextint(&tmp, &netType);
                    if (err < 0) goto out;
                }
                is4G(netType, -1, socket_id);
                RLOGD("netType is %d", netType);
                pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
                if (s_PSRegState[socket_id] == STATE_OUT_OF_SERVICE) {
                    s_PSRegState[socket_id] = STATE_IN_SERVICE;
                }
                pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
                RLOGD("s_PSRegState is IN SERVICE");
            } else {
                pthread_mutex_lock(&s_LTEAttachMutex[socket_id]);
                if (s_PSRegState[socket_id] == STATE_IN_SERVICE) {
                    s_PSRegState[socket_id] = STATE_OUT_OF_SERVICE;
                    cancelTimerOperation(socket_id);
                }
                pthread_mutex_unlock(&s_LTEAttachMutex[socket_id]);
                RLOGD("s_PSRegState is OUT OF SERVICE.");
            }
            RIL_onUnsolicitedResponse(
                                    RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                    NULL, 0, socket_id);
        } else {
            s_isSA[socket_id] = false;
        }

        RLOGD("SA/NSA mode: %d", s_isSA[socket_id]);
    } else if (strStartsWith(s, "+CIREGU:")) {
        int response;
        char *tmp = NULL;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &response);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED,
                                  &response, sizeof(response), socket_id);
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_IMS_NETWORK_STATE_CHANGED,
                                  &response, sizeof(response), socket_id);
    } else if (strStartsWith(s, "^CONN:")) {
        int cid;
        int type;
        int active;
        char *tmp;
        line = strdup(s);
        tmp = line;

        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &cid);
        if (err < 0) {
            RLOGD("get cid fail");
            goto out;
        }
        err = at_tok_nextint(&tmp, &type);
        if (err < 0) {
            RLOGD("get type fail");
            goto out;
        }
        err = at_tok_nextint(&tmp, &active);
        if (err < 0) {
            RLOGD("get active fail");
            goto out;
        }

        if (cid == 11) {
            s_imsBearerEstablished[socket_id] = active;
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_RESPONSE_IMS_BEARER_ESTABLISTED,
                    (void *)&s_imsBearerEstablished[socket_id], sizeof(int),
                    socket_id);
        }
#if 0
    } else if (strStartsWith(s, "+SPPCI:")) {
        char *tmp;
        int cid, propNameLen, propValueLen;
        char phy_cellid[ARRAY_SIZE];
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nexthexint(&tmp, &cid);
        if (err < 0) {
            RLOGD("get physicel cell id fail");
            goto out;
        }
        snprintf(phy_cellid, sizeof(phy_cellid), "%d", cid);
        SetPropPara *cellIdPara = (SetPropPara *)calloc(1, sizeof(SetPropPara));
        propNameLen = strlen(PHYSICAL_CELLID_PROP) + 1;
        propValueLen = strlen(phy_cellid) + 1;
        cellIdPara->socketId = socket_id;
        cellIdPara->propName =
                (char *)calloc(propNameLen, sizeof(char));
        cellIdPara->propValue =
                (char *)calloc(propValueLen, sizeof(char));
        memcpy(cellIdPara->propName, PHYSICAL_CELLID_PROP, propNameLen);
        memcpy(cellIdPara->propValue, phy_cellid, propValueLen);
        cellIdPara->mutex = &s_physicalCellidMutex;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, (void *)setPropForAsync, (void *)cellIdPara);
#endif
    } else if (strStartsWith(s, "+SPNWNAME:")) {
        /* NITZ operator name */
        char *tmp = NULL;
        char *mcc = NULL;
        char *mnc = NULL;
        char *fullName = NULL;
        char *shortName = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &mcc);
        if (err < 0) goto out;

        err = at_tok_nextstr(&tmp, &mnc);
        if (err < 0) goto out;

        err = at_tok_nextstr(&tmp, &fullName);
        if (err < 0) goto out;

        err = at_tok_nextstr(&tmp, &shortName);
        if (err < 0) goto out;

        char nitzOperatorInfo[PROPERTY_VALUE_MAX] = {0};
        char propName[ARRAY_SIZE] = {0};

        if (socket_id == RIL_SOCKET_1) {
            snprintf(propName, sizeof(propName), "%s", NITZ_OPERATOR_PROP);
        } else if (socket_id > RIL_SOCKET_1) {
            snprintf(propName, sizeof(propName), "%s%d", NITZ_OPERATOR_PROP,
                     socket_id);
        }
        snprintf(nitzOperatorInfo, sizeof(nitzOperatorInfo), "%s,%s,%s%s",
                 fullName, shortName, mcc, mnc);
        property_set(propName, nitzOperatorInfo);

        snprintf(s_nitzOperatorInfo[socket_id], sizeof(s_nitzOperatorInfo[socket_id]),
                 "%s%s=%s=%s", mcc, mnc, fullName, shortName);
        RLOGD("s_nitzOperatorInfo[%d]: %s", socket_id, s_nitzOperatorInfo[socket_id]);

        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);
    } else if (strStartsWith(s, "+SPTESTMODE:")) {
        int response;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &response);
        if (err < 0) goto out;

        const char *cmd = "+SPTESTMODE:";
        RIL_Token t = NULL;
        void *data = NULL;

        onCompleteAsyncCmdMessage(socket_id, cmd, &t, &data);
        dispatchSPTESTMODE(t, data, (void *)(&response));
    } else if (strStartsWith(s, "+SPFREQSCAN:")) {
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        skipWhiteSpace(&tmp);
        if (tmp == NULL) {
            RLOGE("network scan param is NULL");
            goto out;
        }

        CallbackPara *cbPara =
                (CallbackPara *)calloc(1, sizeof(CallbackPara));
        if (cbPara != NULL) {
            cbPara->para = strdup(tmp);
            cbPara->socket_id = socket_id;
            RIL_requestTimedCallback(processNetworkScanResults, cbPara, NULL);
        }
    } else if (strStartsWith(s, "+SPCTEC:")) {
        int response;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &response);
        if (err < 0) goto out;

        setPhoneType(response, socket_id);
        setCESQValue(socket_id, s_isCDMAPhone[socket_id]);
        if (s_isCDMAPhone[socket_id]) {
            response = RADIO_TECH_1xRTT;
        } else {
            response = RADIO_TECH_GSM;
        }

        RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                (void*)&response, sizeof(int), socket_id);
    } else if (strStartsWith(s, "+SPPRLVERSION:")) {
        // Called when CDMA PRL (preferred roaming list) changes
        char *tmp = NULL;
        int prl_version;

        line = strdup(s);
        tmp = line;

        err = at_tok_start(&tmp);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &prl_version);
        if (err < 0) goto out;

        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_PRL_CHANGED,
                &prl_version, sizeof(prl_version), socket_id);
    } else if (strStartsWith(s, "+SPNRCHANNEL")) {
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);
        if (err < 0) goto out;

        processPhysicalChannelConfigs(tmp, socket_id);
    } else if (strStartsWith(s, "+CSCON:")) {
        // +CSCON: <mode>
        // +CSCON: <mode>[,<state>]
        // +CSCON: <mode>[,<state>[,<access>]]
        // +CSCON: <mode>[,<state>[,<access>[,<coreNetwork>]]]
        // mode 1 -- connected status, represents data business
        //      0 -- idle status
        char *tmp = NULL;
        int retValue = 1;
        int currentSigConnStatus[SIM_COUNT] = {-1};

        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
                                  NULL, 0, socket_id);

        RIL_SingnalConnStatus *sigConnStatus =
                (RIL_SingnalConnStatus *)alloca(sizeof(RIL_SingnalConnStatus));
        memset(sigConnStatus, 0, sizeof(RIL_SingnalConnStatus));

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &sigConnStatus->mode);
        if (err < 0) goto out;

        if (at_tok_hasmore(&tmp)) {
            at_tok_nextint(&tmp, &sigConnStatus->state);
            if (at_tok_hasmore(&tmp)) {
                at_tok_nextint(&tmp, &sigConnStatus->access);
                if (at_tok_hasmore(&tmp)) {
                    at_tok_nextint(&tmp, &sigConnStatus->coreNetwork);
                }
            }
        }

        // Need to upload the last reported data when timeout reached
        memcpy(&s_sigConnStatus[socket_id], sigConnStatus, sizeof(RIL_SingnalConnStatus));
        RLOGD("s_lastSigConnStatus[%d] = %d", socket_id, s_lastSigConnStatus[socket_id]);
        if (sigConnStatus->mode == 1) {
            // connected status
            currentSigConnStatus[socket_id] = 1;
            if (sigConnStatus->access == 5 && sigConnStatus->coreNetwork == 0) {
                // SCG,need to canncel timeout and unsol
                RLOGD("SCG support 5G, canncel timeout");
                s_nrStatusConnected[socket_id] = true;
                cancelTimerOperation(socket_id);
            } else {
                // timeout waiting,go to out
                if (s_sigConnStatusWait[socket_id]) {
                    s_lastSigConnStatus[socket_id] = currentSigConnStatus[socket_id];
                    s_nrStatusConnected[socket_id] = false;
                    goto out;
                }
                // idle->connected: no wait and idel state support 5G,need to wait
                // connected scg->connected: no wait and SCG,need to wait
                if ((s_lastSigConnStatus[socket_id] == 1 && s_nrStatusConnected[socket_id])
                    || (s_lastSigConnStatus[socket_id] == 0 && !s_sigConnStatusWait[socket_id]
                            && s_nrStatusNotRestricted[socket_id])) {
                    RLOGD("need to start timeout %d", socket_id);
                    s_sigConnStatusWait[socket_id] = true;
                    s_cancelSerial[socket_id] = enqueueTimedMessageCancel(processRequestTimerDelay,
                            (void *)&s_socketId[socket_id], 30 * 1000);
                    RLOGD("connected status: s_cancelSerial[%d], %d", socket_id, s_cancelSerial[socket_id]);
                    retValue = 0;
                }
                s_nrStatusConnected[socket_id] = false;
            }
        } else {
            // idle status
            currentSigConnStatus[socket_id] = 0;
            if (s_sigConnStatusWait[socket_id]) {
                // timeout waiting, idle state support 5G, canncel timeout and unsol
                if (s_nrStatusNotRestricted[socket_id]) {
                    RLOGD("idel canncel timeout");
                    removeTimedMessage(s_cancelSerial[socket_id]);
                    RLOGD("idle status: s_cancelSerial[%d], %d",
                            socket_id, s_cancelSerial[socket_id]);
                    // after canceltimeout to unsol
                    cancelTimerNRStateChanged(socket_id);
                } else {
                    // timeout waiting, idle state not support 5G, need to wait
                    RLOGD("need to wait");
                    goto out;
                }
            // connected -> idel: connected state is SCG and idel not support 5G，need to wait
            } else if (s_lastSigConnStatus[socket_id] == 1 && !s_sigConnStatusWait[socket_id]
                        && !s_nrStatusNotRestricted[socket_id] && s_nrStatusConnected[socket_id]) {
                RLOGD("last state is SCG and idel is Restricted %d", socket_id);
                s_sigConnStatusWait[socket_id] = true;
                s_cancelSerial[socket_id] =  enqueueTimedMessageCancel(processRequestTimerDelay,
                        (void *)&s_socketId[socket_id], 30 * 1000);
                RLOGD("connected -> idel: s_cancelSerial[%d], %d", socket_id, s_cancelSerial[socket_id]);
                retValue = 0;
            }
        }
        s_lastSigConnStatus[socket_id] = currentSigConnStatus[socket_id];
        RLOGD("retValue = %d", retValue);
        if (retValue != 0) {
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIGNAL_CONN_STATUS,
                    sigConnStatus, sizeof(RIL_SingnalConnStatus), socket_id);
        }
    } else if (strStartsWith(s, "+SPNRCFGINFO:")) {
        int response[2] = {-1, -1};
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &response[0]);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &response[1]);
        if (err < 0) goto out;

        char smartNr[PROPERTY_VALUE_MAX] = {0};
        property_get(MODEM_SMART_NR_PROP, smartNr, "");
        RLOGD("SPNRCFGINFO: %d, %s", s_isNR, smartNr);
        RLOGD("SPNRCFGINFO: %d, %d", response[0], response[1]);

        if (s_isNR && strcmp(smartNr, "true") == 0) {
            RLOGD("s_lastNrCfgInfo[%d] = %d", socket_id, s_lastNrCfgInfo[socket_id]);
            s_nrCfgInfo[socket_id][0] = response[0];
            s_nrCfgInfo[socket_id][1] = response[1];
            if (response[1] == 1) {
                removeTimedMessage(s_cancelSerial[socket_id]);
                RLOGD("removeTimedMessage: s_cancelSerial[%d], %d",
                        socket_id, s_cancelSerial[socket_id]);
                cancelTimerNRStateChanged(socket_id);
            } else {
                if (!s_sigConnStatusWait[socket_id] && s_lastNrCfgInfo[socket_id] == 1) {
                    s_sigConnStatusWait[socket_id] = true;
                    s_cancelSerial[socket_id] = enqueueTimedMessageCancel(processRequestTimerDelay,
                            (void *)&s_socketId[socket_id], 30 * 1000);
                    RLOGD("enqueueTimedMessageCancel: s_cancelSerial[%d], %d",
                            socket_id, s_cancelSerial[socket_id]);
                }
            }
            s_lastNrCfgInfo[socket_id] = response[1];
        }
    } else if (strStartsWith(s, "+SPSMART5G:")) {
        int response = -1;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &response);
        if (err < 0) goto out;

        if (response == 1) {
            property_set(MODEM_SMART_NR_PROP, "true");
        } else {
            property_set(MODEM_SMART_NR_PROP, "false");
        }
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SMART_NR_CHANGED,
                &response, sizeof(response), socket_id);
    } else if (strStartsWith(s, "+SPDSMINFOU:")) {
        char *tmp = NULL;
        char rxBytesStr[PROPERTY_VALUE_MAX] = {0};
        char txBytesStr[PROPERTY_VALUE_MAX] = {0};

        line = strdup(s);
        tmp = line;
        err = at_tok_start(&tmp);

        int rxBytes = 0;
        int txBytes = 0;
        int flag = 0;

        err = at_tok_nextint(&tmp, &flag);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &rxBytes);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &txBytes);
        if (err < 0) goto out;

        if (flag == s_UsbShareFlag) {
            pthread_mutex_lock(&s_usbSharedMutex);
            s_rxBytes += rxBytes;
            s_txBytes += txBytes;

            snprintf(rxBytesStr, sizeof(rxBytesStr), "%ld", s_rxBytes);
            snprintf(txBytesStr, sizeof(txBytesStr), "%ld", s_txBytes);
            property_set("ril.sys.usb.tether.rx", rxBytesStr);
            property_set("ril.sys.usb.tether.tx", txBytesStr);
            RLOGD("SPDSMINFOU: s_rxBytes: %ld, s_txBytes: %ld", s_rxBytes, s_txBytes);
            pthread_mutex_unlock(&s_usbSharedMutex);
        }
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}

void dispatchSPTESTMODE(RIL_Token t, void *data, void *resp) {
    if (t == NULL || resp == NULL) {
        return;
    }

    int status = ((int *)resp)[0];

#if (SIM_COUNT == 2)
    pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_1]);
    pthread_mutex_unlock(&s_radioPowerMutex[RIL_SOCKET_2]);
#endif
    if (data != NULL) {
        RIL_RadioCapability *rc = (RIL_RadioCapability *)data;
        if (status == 1) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, rc,
                    sizeof(RIL_RadioCapability));
            /* after applying radio capability success
             * send unsol radio capability.
             */
            sendUnsolRadioCapability();
        } else {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, rc,
                    sizeof(RIL_RadioCapability));
        }
    } else {
        if (status == 1) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        } else {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
    }
    free(data);
}

void onSignalStrengthUnsolResponse(const void *data, RIL_SOCKET_ID socket_id) {
    RIL_SignalStrength_v1_4 responseV1_4;
    RIL_SIGNALSTRENGTH_INIT_1_4(responseV1_4);
    int *response = (int *)data;

    if (!s_isCDMAPhone[socket_id]) {
        if (response[0] != -1 && response[0] != 99 && response[0] != 255) {
            responseV1_4.gsm.signalStrength = response[0];
        }
        if (response[2] != -1 && response[2] != 255) {  // response[2] is cp reported 3G value
            int dBm = convert3GValueTodBm(response[2]);
            responseV1_4.wcdma.signalStrength = getWcdmaSigStrengthBydBm(dBm);
            responseV1_4.wcdma.rscp = getWcdmaRscpBydBm(dBm);
            responseV1_4.wcdma.bitErrorRate = response[1];
            responseV1_4.wcdma.ecno = response[3];
        }
    } else {
        if (response[0] != -1 && response[0] != 255) {
            responseV1_4.cdma.dbm = response[0];
        }
        if (response[1] != -1 && response[1] != 255) {
            responseV1_4.cdma.ecio = response[1];
        }
        if (response[2] != -1 && response[2] != 255) {
            responseV1_4.evdo.dbm = response[2];
        }
        if (response[3] != -1 && response[3] != 255) {
            responseV1_4.evdo.signalNoiseRatio = response[3];
        }
    }
    if (response[5] != -1 && response[5] != 255 && response[5] != -255) {
        responseV1_4.lte.rsrp = response[5];
    }
    if (response[7] != -1 && response[7] != 255 && response[7] != -255) {
        responseV1_4.nr.ssRsrp = response[7];
    }

    RLOGD("simId[%d], sigp+CESQ: %d,%d,%d,%d,%d,%d,-1,%d,-1", socket_id, response[0],
       response[1], response[2], response[3], response[4], response[5], response[7]);
    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &responseV1_4,
            sizeof(RIL_SignalStrength_v1_4), socket_id);
}

/* for +CESQ: unsol response process */
int cesq_unsol_rsp(char *line, RIL_SOCKET_ID socket_id, char *newLine) {
    int err;
    char *atInStr;

    atInStr = line;
    err = at_tok_flag_start(&atInStr, ':');
    if (err < 0) goto error;

    err = at_tok_nextint(&atInStr, &s_rxlev[socket_id]);
    if (err < 0) goto error;
    if (!s_isCDMAPhone[socket_id]) {
        if (s_rxlev[socket_id] <= 61) {
            s_rxlev[socket_id] = (s_rxlev[socket_id] + 2) / 2;
        } else if (s_rxlev[socket_id] > 61 && s_rxlev[socket_id] <= 63) {
            s_rxlev[socket_id] = 31;
        } else if (s_rxlev[socket_id] >= 100 && s_rxlev[socket_id] < 103) {
            s_rxlev[socket_id] = 0;
        } else if (s_rxlev[socket_id] >= 103 && s_rxlev[socket_id] < 165) {
            s_rxlev[socket_id] = ((s_rxlev[socket_id] - 103) + 1) / 2;
        } else if (s_rxlev[socket_id] >= 165 && s_rxlev[socket_id] <= 191) {
            s_rxlev[socket_id] = 31;
        }
    } else {
        // 1x_dBm = rssi - 110, and Android needs positive numbers
        s_rxlev[socket_id] = 110 - s_rxlev[socket_id];
    }

    err = at_tok_nextint(&atInStr, &s_ber[socket_id]);
    if (err < 0) goto error;
    if (s_isCDMAPhone[socket_id]) {
        // 1x_ecio_db = (ecio - 62)/2, and Android needs positive numbers * 10
        s_ber[socket_id] = (62 - s_ber[socket_id]) / 2 * 10;
    }

    err = at_tok_nextint(&atInStr, &s_rscp[socket_id]);
    if (err < 0) goto error;
/*
    if (!s_isCDMAPhone[socket_id]) {
        if (s_rscp[socket_id] >= 100 && s_rscp[socket_id] < 103) {
            s_rscp[socket_id] = 0;
        } else if (s_rscp[socket_id] >= 103 && s_rscp[socket_id] < 165) {
            s_rscp[socket_id] = ((s_rscp[socket_id] - 103) + 1) / 2;
        } else if (s_rscp[socket_id] >= 165 && s_rscp[socket_id] <= 191) {
            s_rscp[socket_id] = 31;
        }
    }
*/
    if (s_isCDMAPhone[socket_id]) {
        // Bug1075130
        // this is evdo rssi, reuse variable name rscp
        // evdo_dBm = rssi - 110, and Android needs positive numbers
        s_rscp[socket_id] = 110 - s_rscp[socket_id];
    }

    err = at_tok_nextint(&atInStr, &s_ecno[socket_id]);
    if (err < 0) goto error;
    if (s_isCDMAPhone[socket_id] && s_ecno[socket_id] != -1 && s_ecno[socket_id] != 255) {
        // TODO: Modem report 0-58, but AP needs 0-8
        // Bug 1025340, Set SNR to 8 before Modem tell RIL how to convert
        s_ecno[socket_id] = 8;
    }

    err = at_tok_nextint(&atInStr, &s_rsrq[socket_id]);
    if (err < 0) goto error;

    err = at_tok_nextint(&atInStr, &s_rsrp[socket_id]);
    if (err < 0) goto error;
    if (s_rsrp[socket_id] == 255) {
        s_rsrp[socket_id] = -255;
    } else {
        s_rsrp[socket_id] = 141 - s_rsrp[socket_id];
    }

    if (s_modemConfig == NRLWG_LWG) {
        err = at_tok_nextint(&atInStr, &s_ss_rsrq[socket_id]);
        if (err < 0) goto error;

        err = at_tok_nextint(&atInStr, &s_ss_rsrp[socket_id]);
        if (err < 0) goto error;
        if (s_ss_rsrp[socket_id] == 255) {
            s_ss_rsrp[socket_id] = -255;
        } else {
            s_ss_rsrp[socket_id] = 157 - s_ss_rsrp[socket_id];
        }

        err = at_tok_nextint(&atInStr, &s_ss_sinr[socket_id]);
        if (err < 0) goto error;
    }

    if (s_psOpened[socket_id] == 1) {
        s_psOpened[socket_id] = 0;
        if (s_modemConfig == NRLWG_LWG) {
            snprintf(newLine, AT_COMMAND_LEN, "+CESQ: %d,%d,%d,%d,%d,%d,%d,%d,%d",
                    s_rxlev[socket_id], s_ber[socket_id], s_rscp[socket_id],
                    s_ecno[socket_id], s_rsrq[socket_id], s_rsrp[socket_id],
                                s_ss_rsrq[socket_id], s_ss_rsrp[socket_id], s_ss_sinr[socket_id]);
        } else {
            snprintf(newLine, AT_COMMAND_LEN, "+CESQ: %d,%d,%d,%d,%d,%d",
                s_rxlev[socket_id], s_ber[socket_id], s_rscp[socket_id],
                s_ecno[socket_id], s_rsrq[socket_id], s_rsrp[socket_id]);
        }
        return AT_RESULT_OK;
    }

error:
    return AT_RESULT_NG;
}

/* for AT+CESQ execute command response process */
void cesq_execute_cmd_rsp(RIL_SOCKET_ID socket_id, ATResponse *p_response, ATResponse **p_newResponse) {
    char *line;
    int err, len;
    int rxlev = 0, ber = 0, rscp = 0, ecno = 0, rsrq = 0, rsrp = 0,ss_rsrq = 0, ss_rsrp = 0, ss_sinr = 0;
    char respStr[AT_RESPONSE_LEN] = {0};

    line = p_response->p_intermediates->line;
    len = strlen(line);
    if (findInBuf(line, len, "+CESQ")) {
        err = at_tok_flag_start(&line, ':');
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rxlev);
        if (err < 0) goto error;
        if (!s_isCDMAPhone[socket_id]) {
            if (rxlev <= 61) {
                rxlev = (rxlev + 2) / 2;
            } else if (rxlev > 61 && rxlev <= 63) {
                rxlev = 31;
            } else if (rxlev >= 100 && rxlev < 103) {
                rxlev = 0;
            } else if (rxlev >= 103 && rxlev < 165) {
                rxlev = ((rxlev - 103) + 1) / 2;  // add 1 for compensation
            } else if (rxlev >= 165 && rxlev <= 191) {
                rxlev = 31;
            }
        } else {
            // this is 1x rssi, reuse variable name rxlev
            // 1x_dBm = rssi - 110, and Android needs positive numbers
            rxlev = 110 - rxlev;
        }

        err = at_tok_nextint(&line, &ber);
        if (err < 0) goto error;
        if (s_isCDMAPhone[socket_id]) {
            // 1x_ecio_db = (ecio - 62)/2, and Android needs positive numbers * 10
            ber = (62 - ber) / 2 * 10;
        }

        err = at_tok_nextint(&line, &rscp);
        if (err < 0) goto error;
/*
        if (!s_isCDMAPhone[socket_id]) {  // c2k doesn't need to modify this value
            if (rscp >= 100 && rscp < 103) {
                rscp = 0;
            } else if (rscp >= 103 && rscp < 165) {
                rscp = ((rscp - 103) + 1) / 2; // add 1 for compensation
            } else if (rscp >= 165 && rscp <= 191) {
                rscp = 31;
            }
        }
*/
        if (s_isCDMAPhone[socket_id]) {
            // Bug1075130
            // this is evdo rssi, reuse variable name rscp
            // evdo_dBm = rssi - 110, and Android needs positive numbers
            rscp = 110 - rscp;
        }

        err = at_tok_nextint(&line, &ecno);
        if (err < 0) goto error;
        if (s_isCDMAPhone[socket_id] && ecno != -1 && ecno != 255) {
            // TODO: Modem report 0-58, but AP needs 0-8
            // Bug 1025340, Set SNR to 8 before Modem tell RIL how to convert
            ecno = 8;
        }

        err = at_tok_nextint(&line, &rsrq);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &rsrp);
        if (err < 0) goto error;
        if (rsrp == 255) {
            rsrp = -255;
        } else {
            rsrp = 141 - rsrp;  // modified by bug#486220
        }
        if (s_modemConfig == NRLWG_LWG) {
           err = at_tok_nextint(&line, &ss_rsrq);
           if (err < 0) goto error;

           err = at_tok_nextint(&line, &ss_rsrp);
           if (err < 0) goto error;
           if (ss_rsrp == 255) {
               ss_rsrp = -255;
           } else {
               ss_rsrp = 157 - ss_rsrp;
           }

           err = at_tok_nextint(&line, &ss_sinr);
           if (err < 0) goto error;

           snprintf(respStr, sizeof(respStr), "+CESQ: %d,%d,%d,%d,%d,%d,%d,%d,%d",
                   rxlev, ber, rscp, ecno, rsrq, rsrp, ss_rsrq, ss_rsrp, ss_sinr);
        } else {
           snprintf(respStr, sizeof(respStr), "+CESQ: %d,%d,%d,%d,%d,%d",
                   rxlev, ber, rscp, ecno, rsrq, rsrp);
        }

        ATResponse *sp_response = at_response_new();
        reWriteIntermediate(sp_response, respStr);
        if (p_newResponse == NULL) {
            at_response_free(sp_response);
        } else {
            *p_newResponse = sp_response;
        }
    }

error:
    return;
}

int convert3GValueTodBm(int cp3GValue) {
    int dBm = 0;
    if (s_isCesqNewVersion) {
        dBm = cp3GValue - 120;
    } else {
        dBm = cp3GValue - 100 - 116;  // 100 added by L4, 116 added by PHY
    }
    return dBm;
}

// see CellSignalStrength.java#getAsuFromRssiDbm
int getWcdmaSigStrengthBydBm(int dBm) {
    int ss = (dBm + 113) / 2;

    if (ss < 0) {
        ss = 0;
    } else if (ss > 31) {
        ss = 31;
    }
    return ss;
}

// see CellSignalStrength.java#getAsuFromRscpDbm
int getWcdmaRscpBydBm(int dBm) {
    return dBm + 120;
}
