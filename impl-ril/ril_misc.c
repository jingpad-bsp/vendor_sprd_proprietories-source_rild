/**
 * ril_misc.c --- Any other requests besides data/sim/call...
 *                process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */
#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_misc.h"
#include "ril_data.h"
#include "ril_network.h"
#include "ril_sim.h"
#include "channel_controller.h"

#include "ril_thermal.h"

/* Fast Dormancy disable property */
#define RADIO_FD_DISABLE_PROP "persist.vendor.radio.fd.disable"
/* PROP_FAST_DORMANCY value is "a,b". a is screen_off value, b is on value */
#define PROP_FAST_DORMANCY    "persist.vendor.radio.fastdormancy"
/* for sleep log */
#define BUFFER_SIZE     (12 * 1024 * 4)
#define CONSTANT_DIVIDE 32768.0

/* single channel call, no need to distinguish sim1 and sim2*/
int s_maybeAddCall = 0;
int s_screenState = 1;
int s_vsimClientFd = -1;
int s_vsimServerFd = -1;
bool s_vsimListenLoop = false;
bool s_vsimInitFlag[SIM_COUNT];
pthread_mutex_t s_vsimSocketMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_vsimSocketCond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_screenMutex = PTHREAD_MUTEX_INITIALIZER;
struct timeval s_timevalCloseVsim = {60, 0};

// for SignalStrength Reporting Criteria to default
RIL_Ext_SignalStrengthReportingCriteria s_GSMDefault[3] = {
        {0,    1, 0, (int*)-1, 1, 1, 1},
        {2000, 3, 0, (int*)-1, 1, 1, 1},
        {3000, 3, 0, (int*)-1, 1, 1, 1}
};

RIL_Ext_SignalStrengthReportingCriteria s_CDMADefault[3] = {
        {0,    3, 0, (int*)-1, 1, 1, 2},
        {2000, 3, 0, (int*)-1, 1, 1, 2},
        {3000, 3, 0, (int*)-1, 1, 1, 2}
};

RIL_Ext_SignalStrengthReportingCriteria s_LTEDefault[3] = {
        {0,    0, 0, (int*)-1, 1, 1, 3},
        {2000, 3, 0, (int*)-1, 1, 1, 3},
        {3000, 3, 0, (int*)-1, 1, 1, 3}
};

RIL_Ext_SignalStrengthReportingCriteria s_NRDefault[3] = {
        {0,    3, 0, (int*)-1, 1, 6, 6},
        {2000, 3, 0, (int*)-1, 1, 6, 6},
        {3000, 3, 0, (int*)-1, 1, 6, 6}
};

static void requestBasebandVersion(RIL_SOCKET_ID socket_id, void *data,
                                   size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int i = 0, err = -1;
    char response[ARRAY_SIZE] = {0};
    char *line = NULL;
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_multiline(socket_id, "AT+CGMR", "",
                                    &p_response);
    memset(response, 0, sizeof(response));
    if (err != 0 || p_response->success == 0) {
        RLOGE("requestBasebandVersion:Send command error!");
        goto error;
    }
    for (i = 0, p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next, i++) {
        line = p_cur->line;
        if (i < 2) continue;
        if (i < 4) {
            if (at_tok_start(&line) == 0) {
                skipWhiteSpace(&line);
                strncat(response, line, strlen(line));
                strncat(response, "|", strlen("|"));
            } else {
                continue;
            }
        } else {
            skipWhiteSpace(&line);
            strncat(response, line, strlen(line));
        }
    }
    if (strlen(response) == 0) {
        RLOGE("requestBasebandVersion: Parameter parse error!");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, strlen(response) + 1);
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestGetIMEISV(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                             RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = 0;
    int sv = 0;
    char *line = NULL;
    char response[ARRAY_SIZE] = {0};  // 10 --> ARRAY_SIZE  debug
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+SGMR=0,0,2",
                                     "+SGMR:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &sv);
    if (err < 0) goto error;

    if (sv >= 0 && sv < 10) {
        snprintf(response, sizeof(response), "0%d", sv);
    } else {
        snprintf(response, sizeof(response), "%d", sv);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, strlen(response) + 1);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestDeviceIdentify(RIL_SOCKET_ID socket_id, void *data,
                                  size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = 0;
    int sv = 0;
    char *line = NULL;
    char *response[4] = {NULL, NULL, NULL, NULL};
    char buf[4][ARRAY_SIZE];
    ATResponse *p_response = NULL;

    memset(buf, 0, 4 * ARRAY_SIZE);

    // get IMEI
    err = at_send_command_numeric(socket_id, "AT+CGSN",
                                  &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    snprintf(buf[0], ARRAY_SIZE, "%s", p_response->p_intermediates->line);
    response[0] = buf[0];

    AT_RESPONSE_FREE(p_response);

    // get IMEISV
    err = at_send_command_singleline(socket_id, "AT+SGMR=0,0,2",
                                     "+SGMR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto done;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto done;

    err = at_tok_nextint(&line, &sv);
    if (err < 0) goto done;

    if (sv >= 0 && sv < 10) {
        snprintf(buf[1], ARRAY_SIZE, "0%d", sv);
    } else {
        snprintf(buf[1], ARRAY_SIZE, "%d", sv);
    }
    response[1] = buf[1];

    // UNISOC Add For CDMA MEID & ESN:
    if (s_isModemSupportCDMA) {
        AT_RESPONSE_FREE(p_response);
        // get ESN
        err = at_send_command_singleline(socket_id, "AT+SPESN?",
                "+SPESN:",&p_response);
        if (err < 0 || p_response->success == 0) {
            goto done;
        }

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto done;

        snprintf(buf[2], ARRAY_SIZE, "%s", line);
        response[2] = buf[2];
        AT_RESPONSE_FREE(p_response);

        // get MEID
        err = at_send_command_singleline(socket_id, "AT+SPMEID?",
                "+SPMEID:",&p_response);
        if (err < 0 || p_response->success == 0) {
            goto done;
        }

        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto done;

        snprintf(buf[3], ARRAY_SIZE, "%s", line);
        response[3] = buf[3];
    }


done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, 4 * sizeof(char *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void onQuerySignalStrength(void *param) {
    int err = -1;
    int response[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    char *line = NULL;
    ATResponse *p_response = NULL;
    ATResponse *p_newResponse = NULL;
    RIL_SignalStrength_v1_4 responseV1_4;

    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    RLOGE("query signal strength when screen on");
    RIL_SIGNALSTRENGTH_INIT_1_4(responseV1_4);

    err = at_send_command_singleline(socket_id, "AT+CESQ", "+CESQ:", &p_response);
    if (err < 0 || p_response->success == 0) {
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
            s_ss_rsrp[socket_id] = response[7];
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
    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH, &responseV1_4,
                              sizeof(RIL_SignalStrength_v1_4), socket_id);
    at_response_free(p_response);
    at_response_free(p_newResponse);
    return;

error:
    RLOGE("onQuerySignalStrength fail");
    at_response_free(p_response);
    at_response_free(p_newResponse);
}

void onQuerySingnalConnStatus(void *param) {
    int err = -1;
    int skip = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;

    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    RLOGD("query signal conn status when screen on");
    err = at_send_command_multiline(socket_id, "AT+CSCON?", "+CSCON:",
                                     &p_response);
    if (err < 0 || p_response->success == 0) {
        goto out;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next) {
        line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto out;

        memset(&s_sigConnStatus[socket_id], 0, sizeof(RIL_SingnalConnStatus));

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto out;

        err = at_tok_nextint(&line, &s_sigConnStatus[socket_id].mode);
        if (err < 0) goto out;

        if (at_tok_hasmore(&line)) {
            at_tok_nextint(&line, &s_sigConnStatus[socket_id].state);
            if (at_tok_hasmore(&line)) {
                at_tok_nextint(&line, &s_sigConnStatus[socket_id].access);
                if (at_tok_hasmore(&line)) {
                    at_tok_nextint(&line, &s_sigConnStatus[socket_id].coreNetwork);
                }
            }
        }
    }

    if (!s_sigConnStatusWait[socket_id]) {
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIGNAL_CONN_STATUS,
                &s_sigConnStatus[socket_id], sizeof(RIL_SingnalConnStatus), socket_id);
    }

out:
    at_response_free(p_response);
}

void onQueryNRCfgInfo(void *param) {
    int err = -1;
    int skip = 0;
    int response[2] = {-1, -1};
    char *line = NULL;
    ATResponse *p_response = NULL;

    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    RLOGD("query NR Config info when screen on");
    err = at_send_command_singleline(socket_id, "AT+SPNRCFGINFO?", "+SPNRCFGINFO:",
                                     &p_response);

    if (err < 0 || p_response->success == 0) {
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

    s_nrCfgInfo[socket_id][0] = response[0];
    s_nrCfgInfo[socket_id][1] = response[1];

    if (!s_sigConnStatusWait[socket_id]) {
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_NR_CFG_INFO,
                (void*)&response, sizeof(response), socket_id);
    }
    at_response_free(p_response);
    return;

error:
    RLOGE("onQueryNRCfgInfo fail");
    at_response_free(p_response);
}

int getFastDormancyTime(int screeState) {
    int screenOffValue = 2, screenOnValue = 5, fastDormancyTime = 0;
    int err = -1;
    char fastDormancyPropValue[PROPERTY_VALUE_MAX] = {0};
    char overseaProp[PROPERTY_VALUE_MAX] = {0};
    char *p_fastDormancy = NULL;

    property_get(PROP_FAST_DORMANCY, fastDormancyPropValue, "");
    property_get(OVERSEA_VERSION, overseaProp, "unknown");
    if (!strcmp(overseaProp, "orange")) {
        screenOnValue = 8;
        screenOffValue = 5;
        goto done;
    }
    if (strcmp(fastDormancyPropValue, "")) {
        p_fastDormancy = fastDormancyPropValue;
        err = at_tok_nextint(&p_fastDormancy, &screenOffValue);
        if (err < 0) {
            goto done;
        }
        err = at_tok_nextint(&p_fastDormancy, &screenOnValue);
        if (err < 0) {
            goto done;
        }
    }
done:
    screeState ? (fastDormancyTime = screenOnValue) :
                 (fastDormancyTime = screenOffValue);
    return fastDormancyTime;
}

int setupVPIfNeeded() {
    char agps_active[PROPERTY_VALUE_MAX] = {0};
    char mms_active[PROPERTY_VALUE_MAX] = {0};
    int setupVPIfNeeded = 0;

    property_get("vendor.sys.ril.agps.active", agps_active, "0");
    property_get("ril.mms.active", mms_active, "0");

    if (strcmp(agps_active, "1") && strcmp(mms_active, "1") ) {
        setupVPIfNeeded = 1;
    }
    RLOGD("%s,%s, setupVPIfNeeded = %d", agps_active, mms_active, setupVPIfNeeded);

    return setupVPIfNeeded;
}

static void requestScreeState(RIL_SOCKET_ID socket_id, int status, RIL_Token t) {
    char prop[PROPERTY_VALUE_MAX] = {0};

    pthread_mutex_lock(&s_screenMutex);
    property_get(RADIO_FD_DISABLE_PROP, prop, "0");
    RLOGD("RADIO_FD_DISABLE_PROP = %s", prop);
    s_screenState = status;
    setScreenState(s_screenState);

    if (!status) {
        /* Suspend */
        at_send_command(socket_id, "AT+CCED=2,8", NULL);
        if (s_isNR) {
            at_send_command(socket_id, "AT+C5GREG=1", NULL);
        }
        at_send_command(socket_id, "AT+CEREG=1", NULL);
        at_send_command(socket_id, "AT+SPEDDAENABLE=1", NULL);
        at_send_command(socket_id, "AT+CREG=1", NULL);
        at_send_command(socket_id, "AT+CGREG=1", NULL);
        /* 5G network connection,Bug1206134 */
        at_send_command(socket_id, "AT+CSCON=0", NULL);
        at_send_command(socket_id, "AT+SPNRCFGINFO=0", NULL);

        if (s_isVoLteEnable) {
            at_send_command(socket_id, "AT+CIREG=1", NULL);
        }
        if (isExistActivePdp(socket_id) && !strcmp(prop, "0")) {
            char cmd[ARRAY_SIZE] = {0};
            snprintf(cmd, sizeof(cmd), "AT*FDY=1,%d",
                     getFastDormancyTime(status));
            at_send_command(socket_id, cmd, NULL);
        }
        if (setupVPIfNeeded()) {
            at_send_command(socket_id, "AT+SPVOICEPREFER=1", NULL);
        }
    } else {
        /* Resume */
        at_send_command(socket_id, "AT+CCED=1,8", NULL);
        if (s_isNR) {
            at_send_command(socket_id, "AT+C5GREG=2", NULL);
        }
        at_send_command(socket_id, "AT+CEREG=2", NULL);
        at_send_command(socket_id, "AT+SPEDDAENABLE=0", NULL);
        at_send_command(socket_id, "AT+CREG=2", NULL);
        at_send_command(socket_id, "AT+CGREG=2", NULL);
        /* 5G network connection,Bug1206134 */
        at_send_command(socket_id, "AT+CSCON=4", NULL);
        at_send_command(socket_id, "AT+SPNRCFGINFO=1", NULL);

        if (s_isVoLteEnable) {
            at_send_command(socket_id, "AT+CIREG=2", NULL);
        }
        if (isExistActivePdp(socket_id) && !strcmp(prop, "0")) {
            char cmd[ARRAY_SIZE] = {0};
            snprintf(cmd, sizeof(cmd), "AT*FDY=1,%d",
                     getFastDormancyTime(status));
            at_send_command(socket_id, cmd, NULL);
        }

        if (s_radioState[socket_id] == RADIO_STATE_ON &&
                getSIMStatus(-1, socket_id) == SIM_READY) {
            RIL_requestTimedCallback(onQuerySignalStrength,
                                     (void *)&s_socketId[socket_id], NULL);
        }
        at_send_command(socket_id, "AT+SPVOICEPREFER=0", NULL);
        RIL_onUnsolicitedResponse(
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0, socket_id);
        RIL_requestTimedCallback(onQuerySingnalConnStatus,
                (void *)&s_socketId[socket_id], NULL);
        RIL_requestTimedCallback(onQueryNRCfgInfo,
                (void *)&s_socketId[socket_id], NULL);
    }
    pthread_mutex_unlock(&s_screenMutex);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSetUnsolResponseFilter(RIL_SOCKET_ID socket_id,
        int indicationFilter, RIL_Token t) {
    // screen state is off(low data), ignore
    if (s_screenState == 0 || indicationFilter == IND_FILTER_ALL) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
    }

    // !low data
    RLOGD("indFilter is %d, screenState is %d", indicationFilter, s_screenState);
    int shouldTurnOnSignalStrength = (indicationFilter & IND_FILTER_SIGNAL_STRENGTH);
    int shouldTurnOnFullNetworkUpdate = (indicationFilter & IND_FILTER_FULL_NETWORK_STATE);
    int shouldTurnOnDormancyUpdate = (indicationFilter & IND_FILTER_DATA_CALL_DORMANCY_CHANGED);

    if (shouldTurnOnSignalStrength) {
        at_send_command(socket_id, "AT+CCED=1,8", NULL);
    } else {
        at_send_command(socket_id, "AT+CCED=2,8", NULL);
    }

    if (shouldTurnOnFullNetworkUpdate) {
        if (s_isNR) {
            at_send_command(socket_id, "AT+C5GREG=2", NULL);
        }
        at_send_command(socket_id, "AT+CEREG=2", NULL);
        at_send_command(socket_id, "AT+CREG=2", NULL);
        at_send_command(socket_id, "AT+CGREG=2", NULL);
        if (s_isVoLteEnable) {
            at_send_command(socket_id, "AT+CIREG=2", NULL);
        }
        /* 5G network connection status */
        at_send_command(socket_id, "AT+CSCON=4", NULL);
        at_send_command(socket_id, "AT+SPNRCFGINFO=1", NULL);
    } else {
        if (s_isNR) {
            at_send_command(socket_id, "AT+C5GREG=1", NULL);
        }
        at_send_command(socket_id, "AT+CEREG=1", NULL);
        at_send_command(socket_id, "AT+CREG=1", NULL);
        at_send_command(socket_id, "AT+CGREG=1", NULL);
        if (s_isVoLteEnable) {
            at_send_command(socket_id, "AT+CIREG=1", NULL);
        }
        /* 5G network connection */
        at_send_command(socket_id, "AT+CSCON=0", NULL);
        at_send_command(socket_id, "AT+SPNRCFGINFO=0", NULL);
    }

    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get(RADIO_FD_DISABLE_PROP, prop, "0");
    if (isExistActivePdp(socket_id) && !strcmp(prop, "0")) {
        char cmd[ARRAY_SIZE] = { 0 };
        snprintf(cmd, sizeof(cmd), "AT*FDY=1,%d",
                 getFastDormancyTime(shouldTurnOnDormancyUpdate));
        at_send_command(socket_id, cmd, NULL);
    }
    RIL_requestTimedCallback(onQueryNRCfgInfo,
            (void *)&s_socketId[socket_id], NULL);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetHardwareConfig(void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    RIL_HardwareConfig hwCfg = {0};

    hwCfg.type = -1;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &hwCfg, sizeof(hwCfg));
}

int vsimQueryVirtual(RIL_SOCKET_ID socket_id) {
    RLOGD("vsimQueryVirtual");
    int err = -1;
    int vsimMode = -1;
    char *line = NULL;
    ATResponse *p_response = NULL;
    err = at_send_command_singleline(socket_id,
            "AT+VIRTUALSIMINIT?", "+VIRTUALSIMINIT:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGD("vsim query virtual card error");
    } else {
        line = p_response->p_intermediates->line;
        RLOGD("vsim query virtual card resp:%s",line);
        err = at_tok_start(&line);
        err = at_tok_nextint(&line, &vsimMode);
    }
    at_response_free(p_response);
//    if (vsimMode > 0) {
//        at_send_command(socket_id,"AT+RSIMRSP=\"ERRO\",1,", NULL);
//    }

    return vsimMode;
}

void onSimDisabled(RIL_SOCKET_ID socket_id) {
    int i;
    pthread_mutex_lock(&s_radioPowerMutex[socket_id]);
    at_send_command(socket_id, "AT+SFUN=5", NULL);
    pthread_mutex_unlock(&s_radioPowerMutex[socket_id]);
    for (i = 0; i < MAX_PDP; i++) {
        if (s_dataAllowed[socket_id] &&
            s_PDP[socket_id][i].state == PDP_BUSY) {
            RLOGD("s_PDP[%d].state = %d", i, s_PDP[socket_id][i].state);
            putPDP(socket_id, i);
        }
    }
    setRadioState(socket_id, RADIO_STATE_OFF);
}

int closeVirtual(int socket_id) {
    RLOGD("closeVirtual, phoneId: %d", socket_id);
    int err = -1;

    err = at_send_command(socket_id,"AT+RSIMRSP=\"VSIM\",0", NULL);
    onSimDisabled(socket_id);

    return err;
}

int setSignalStrengthReportingCriteria(RIL_SOCKET_ID socket_id,
                            RIL_Ext_SignalStrengthReportingCriteria *data) {
    int err = -1;
    int flag = -1;
    ATResponse *p_response = NULL;
    int n = 0, pOffset = 0;
    int hysteresisMs = 0;
    int hysteresisDb = 0;
    bool isEnabled = false;
    int thresholdsDbmNumber = 0;
    int signalMeasurementType = 0;
    int *thresholdsDbm = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    char tmpCmd[AT_COMMAND_LEN] = {0};
    RIL_Ext_RadioAccessNetworks accessNetworks = 0;
    RIL_Ext_SignalStrengthReportingCriteria *pCriteria = NULL;

    if (data == NULL) goto done;
    pCriteria = data;
    RLOGD("setSignalStrengthReportingCriteria");

    isEnabled = pCriteria->isEnabled;
    hysteresisMs = pCriteria->hysteresisMs;
    hysteresisDb = pCriteria->hysteresisDb;
    thresholdsDbmNumber = pCriteria->thresholdsDbmNumber;
    accessNetworks = pCriteria->accessNetwork;
    signalMeasurementType = (int)pCriteria->signalMeasurement;
    RLOGD("hysteresisMs:%d, hysteresisDb:%d, thresholdsDbmNumber:%d, accessNetworks:%d",
            hysteresisMs, hysteresisDb, thresholdsDbmNumber, accessNetworks);

    if (pCriteria->thresholdsDbm == NULL) {
        /* set for vts test case setSignalStrengthReportingCriteria_EmptyParams, which
         * requires function returns success, when thresholdsDbm is equal to NULL */
        RLOGE("thresholdsDbm is NULL");
        err = 0;
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
        case RADIO_EXT_ACCESS_NET_GERAN:
            if (isEnabled) {
                snprintf(cmd, sizeof(cmd), "AT+SPGASDUMMY=\"set gas signal rule\",%s", tmpCmd);
                err = at_send_command(socket_id, cmd, &p_response);
            } else {
                err = at_send_command(socket_id, "AT+SPGASDUMMY=\"set gas signal rule\"", &p_response);
            }
            break;
        case RADIO_EXT_ACCESS_NET_UTRAN:
            if (isEnabled) {
                snprintf(cmd, sizeof(cmd), "AT+SPWASDUMMY=\"set was signal rule\",%s", tmpCmd);
                err = at_send_command(socket_id, cmd, &p_response);
                if (err < 0 || p_response->success == 0) goto done;

                snprintf(cmd, sizeof(cmd), "AT+SPTASDUMMY=\"set tas signal rule\",%s", tmpCmd);
                err = at_send_command(socket_id, cmd, &p_response);
            } else {
                err = at_send_command(socket_id, "AT+SPWASDUMMY=\"set was signal rule\"", &p_response);
                if (err < 0 || p_response->success == 0) goto done;

                err = at_send_command(socket_id, "AT+SPTASDUMMY=\"set tas signal rule\"", &p_response);
            }
            break;
        case RADIO_EXT_ACCESS_NET_EUTRAN:
            if (isEnabled) {
                snprintf(cmd, sizeof(cmd), "AT+SPLASDUMMY=\"set las signal rule\",%s,%d",
                        tmpCmd, signalMeasurementType);
                err = at_send_command(socket_id, cmd, &p_response);
            } else {
                err = at_send_command(socket_id, "AT+SPLASDUMMY=\"set las signal rule\"", &p_response);
            }
            break;
        case RADIO_EXT_ACCESS_NET_NGRAN:
            if (isEnabled) {
                snprintf(cmd, sizeof(cmd), "AT+SPASENGMD=\"#nras_set_signal_rule\",%d,%s",
                        signalMeasurementType, tmpCmd);
                err = at_send_command(socket_id, cmd, &p_response);
            } else {
                err = at_send_command(socket_id, "AT+SPASENGMD=\"#nras_set_signal_rule\"", &p_response);
            }
            break;
        default:
            break;
    }
done:
    if (err < 0 || p_response->success == 0) {
        flag = 1;
    } else {
        flag = 0;
    }
    at_response_free(p_response);
    return flag;
}

void sendSignalStrengthCriteriaCommend(RIL_SOCKET_ID socket_id, int commend) {
    int err = setSignalStrengthReportingCriteria(socket_id, &s_GSMDefault[commend]);
    RLOGD("sendSignalStrengthCriteriaCommend GSM result %d ", err);
    err = setSignalStrengthReportingCriteria(socket_id, &s_CDMADefault[commend]);
    RLOGD("sendSignalStrengthCriteriaCommend CDMA result %d ",err);
    err = setSignalStrengthReportingCriteria(socket_id, &s_LTEDefault[commend]);
    RLOGD("sendSignalStrengthCriteriaCommend LTE result %d ", err);
    err = setSignalStrengthReportingCriteria(socket_id, &s_NRDefault[commend]);
    RLOGD("sendSignalStrengthCriteriaCommend NR result %d ", err);
}

void requestSendAT(RIL_SOCKET_ID socket_id, const char *data, size_t datalen,
                   RIL_Token t, char *atResp, int responseLen) {
    RIL_UNUSED_PARM(datalen);

    int i = 0, err = -1;
    char *ATcmd = (char *)data;
    char buf[AT_RESPONSE_LEN] = {0};
    char *cmd = NULL;
    char *pdu = NULL;
    char *response[1] = {NULL};
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;

    if (ATcmd == NULL) {
        RLOGE("Invalid AT command");
        goto error;
    }
    RLOGD("ATcmd=%s in %s\n", ATcmd, __FUNCTION__);
    // AT+SNVM=1,2118,01
    if (!strncasecmp(ATcmd, "AT+SNVM=1", strlen("AT+SNVM=1"))) {
        cmd = ATcmd;
        skipNextComma(&ATcmd);
        pdu = strchr(ATcmd, ',');
        if (pdu == NULL) {
            RLOGE("SNVM: cmd is %s pdu is NULL !", cmd);
            strlcat(buf, "\r\n", sizeof(buf));
            response[0] = buf;
            if (t != NULL) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, response,
                                      sizeof(char *));
            } else if (atResp != NULL) {
                snprintf(atResp, responseLen, "ERROR");
            }
            return;
        }

        *pdu = '\0';
        pdu++;
        err = at_send_command_snvm(socket_id, cmd, pdu, "",
                                   &p_response);
    } else if (strStartsWith(ATcmd, "VSIM_INIT")) {
        char *cmd = NULL;
        RLOGD("vsim init");
        cmd = ATcmd;
        at_tok_start(&cmd);
        err = at_send_command(socket_id, cmd, &p_response);
    } else if (strStartsWith(ATcmd, "VSIM_EXIT")) {
        char *cmd = ATcmd;

        //send AT
        at_tok_start(&cmd);
        int simState = getSIMStatus(RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS, socket_id);
        if (simState != SIM_ABSENT) {
            at_send_command(socket_id, "AT+RSIMRSP=\"ERRO\",2", NULL);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err >= 0 && p_response->success != 0) {
                onSimDisabled(socket_id);
            }
        } else {
            RLOGD("no vsim!!");
        }
    }  else if (strStartsWith(ATcmd, "VSIM_TIMEOUT")) {
        int time = -1;
        char *cmd = ATcmd;

        at_tok_start(&cmd);
        err = at_tok_nextint(&cmd, &time);
        RLOGD("VSIM_TIMEOUT:%d", time);
        if (time > 0) {
            s_timevalCloseVsim.tv_sec = time;
        }
    } else if (strStartsWith(ATcmd, "START_PPPD")) {//only for C2K GCF test
        s_GSCid = 1;
        s_ethOnOff = 1;
        s_isGCFTest = true;

        RLOGD("START_PPPD : startGSPS");
        RIL_requestTimedCallback(startGSPS, (void *)&s_socketId[socket_id], NULL);

        RLOGD("START_PPPD : return ok only");
        response[0] = "OK";
        snprintf(atResp, responseLen, "%s", response[0]);
        return;
    } else if (strStartsWith(ATcmd, "STOP_PPPD")) {//only for C2K GCF test
        s_GSCid = 1;
        s_ethOnOff = 0;
        s_isGCFTest = true;

        RLOGD("STOP_PPPD : startGSPS");
        RIL_requestTimedCallback(startGSPS, (void *)&s_socketId[socket_id], NULL);

        RLOGD("STOP_PPPD : return ok only!!!");
        response[0] = "OK";
        snprintf(atResp, responseLen, "%s", response[0]);
        return;
    } else if (!strncasecmp(ATcmd, "SIM Hot Plug In", strlen("SIM Hot Plug In"))) {
        err = at_send_command(socket_id, "AT+SFUN=2", NULL);

        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                  NULL, 0, socket_id);
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED,
                                  NULL, 0, socket_id);
        return;
    } else if (!strncasecmp(ATcmd, "SIM Hot Plug Out", strlen("SIM Hot Plug Out"))) {
        err = at_send_command(socket_id, "AT+SFUN=3", NULL);

        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                  NULL, 0, socket_id);
        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED,
                                  NULL, 0, socket_id);

        // sim hot plug out and set stk to not enable
        setStkServiceRunning(socket_id, false);
        return;
    } else if (!strncasecmp(ATcmd, "SIM File Refresh", strlen("SIM File Refresh"))) {
        extern int s_imsInitISIM[SIM_COUNT];
        RLOGD("Entry sim file refresh");
        // get the result, the last character of the command
        int result = ATcmd[strlen(ATcmd) - 1] - '0';
        RLOGD("result:%d", result);
        RIL_SimRefreshResponse_v7 *response = NULL;

        response = (RIL_SimRefreshResponse_v7 *)
                           alloca(sizeof(RIL_SimRefreshResponse_v7));
        response->result = result;
        response->ef_id = 0;
        response->aid = "";
        if (2 == result) {
            s_imsInitISIM[socket_id] = -1;
            setStkServiceRunning(socket_id, false);
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_REFRESH, response,
                sizeof(RIL_SimRefreshResponse_v7), socket_id);
        return;
    } else if (!strncasecmp(ATcmd, "AT+SPBANDSCAN", strlen("AT+SPBANDSCAN"))
                && (t != NULL)) {  // async AT command, not supported by sendCmdSync
        const char *respCmd = "+SPBANDSCAN:";
        enqueueAsyncCmdMessage(socket_id, t, respCmd, NULL,
                asyncCmdTimedCallback, 400);
        err = at_send_command(socket_id, ATcmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            removeAsyncCmdMessage(t);
        } else {
            RLOGD("wait for +SPBANDSCAN: unsolicited response");
            at_response_free(p_response);
            return;
        }
    } else if (t == NULL &&    // the request comes from libatci
            !strncasecmp(ATcmd, "SP5GMode", strlen("SP5GMode"))) {
        char *cmd = NULL;
        cmd = ATcmd;
        int mode = 0;
        int flag = 0;
        at_tok_start(&cmd);
        err = at_tok_nextint(&cmd, &mode);
        if (err < 0) goto error;

        err = at_tok_nextint(&cmd, &flag);
        if (err < 0) goto error;

        RLOGD("SP5GMode: mode %d, flag %d", mode, flag);
        sendEnableNrSwitchCommand(socket_id, mode, flag);
        snprintf(atResp, responseLen, "OK");
        return;
    } else if (t == NULL &&    // the request comes from libatci
            !strncasecmp(ATcmd, "AT+SPLASDUMMY=", strlen("AT+SPLASDUMMY="))) {
        char *cmd = NULL;
        cmd = ATcmd;

        // send AT
        err = at_send_command_multiline(socket_id, ATcmd, "",
                                        &p_response);

        at_tok_flag_start(&cmd, '=');
        skipNextComma(&cmd);
        if (at_tok_hasmore(&cmd)) {
            err = at_tok_nextint(&cmd, &s_smart5GEnable);
            if (err < 0) goto error;
            RLOGD("Smart5GSwitch: s_smart5GEnable %d", s_smart5GEnable);

            //SignalStrength Reporting Criteria for commend to s_smart5GEnable
            sendSignalStrengthCriteriaCommend(socket_id, s_smart5GEnable);
        }
    } else {
        err = at_send_command_multiline(socket_id, ATcmd, "",
                                        &p_response);
    }

    if (err < 0 || p_response->success == 0) {
        if (err == AT_ERROR_CHANNEL_CLOSED) {
            goto error;
        }
        if (p_response != NULL) {
            strlcat(buf, p_response->finalResponse, sizeof(buf));
            strlcat(buf, "\r\n", sizeof(buf));
            response[0] = buf;
            if (t != NULL) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, response,
                                      sizeof(char *));
            } else if (atResp != NULL) {
                snprintf(atResp, responseLen, "%s", p_response->finalResponse);
            }
        } else {
            goto error;
        }
    } else {
        p_cur = p_response->p_intermediates;
        for (i = 0; p_cur != NULL; p_cur = p_cur->p_next, i++) {
            strlcat(buf, p_cur->line, sizeof(buf));
            strlcat(buf, "\r\n", sizeof(buf));
        }
        strlcat(buf, p_response->finalResponse, sizeof(buf));
        strlcat(buf, "\r\n", sizeof(buf));
        response[0] = buf;
        if (!strncasecmp(ATcmd, "AT+SFUN=5", strlen("AT+SFUN=5"))) {
            setRadioState(socket_id, RADIO_STATE_OFF);
        }
        if (t != NULL) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
        } else if (atResp != NULL) {
            snprintf(atResp, responseLen, "%s", buf);
        }
    }
    at_response_free(p_response);
    return;

error:
    if (t != NULL) {
        memset(buf, 0 ,sizeof(buf));
        strlcat(buf, "ERROR", sizeof(buf));
        strlcat(buf, "\r\n", sizeof(buf));
        response[0] = buf;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, response,
                              sizeof(char *));
    } else if (atResp != NULL) {
        snprintf(atResp, responseLen, "ERROR");
    }
    at_response_free(p_response);
}

void sendCmdSync(int phoneId, char *cmd, char *response, int responseLen) {
    RLOGD("sendCmdSync: simId = %d, cmd = %s", phoneId, cmd);

    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        snprintf(response, responseLen, "ERROR: MODEM_NOT_ALIVE");
        return;
    }

    requestSendAT(phoneId, (const char *)cmd, 0, NULL, response, responseLen);
}

static void requestVsimCmd(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                           RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int i = 0, err = -1;
    char *ATcmd = (char *)data;
    char buf[AT_RESPONSE_LEN] = {0};
    char *response[1] = {NULL};
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;

    if (ATcmd == NULL) {
        RLOGE("Invalid AT command");
        goto error;
    }

    if (strStartsWith(ATcmd, "VSIM_INIT")) {
        char *cmd = NULL;
        RLOGD("vsim init");
        cmd = ATcmd;
        at_tok_start(&cmd);
        err = at_send_command(socket_id, cmd, &p_response);
    } else if (strStartsWith(ATcmd, "VSIM_EXIT")) {
        char *cmd = NULL;

        // send AT
        cmd = ATcmd;
        at_tok_start(&cmd);
        int simState = getSIMStatus(RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS, socket_id);
        if (simState != SIM_ABSENT) {
            at_send_command(socket_id, "AT+RSIMRSP=\"ERRO\",2", NULL);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err >= 0 && p_response->success != 0) {
                onSimDisabled(socket_id);
            }
        } else {
            RLOGD("no vsim!!");
        }
    }  else if (strStartsWith(ATcmd, "VSIM_TIMEOUT")) {
        int time = -1;
        char *cmd = NULL;
        cmd = ATcmd;
        at_tok_start(&cmd);
        err = at_tok_nextint(&cmd, &time);
        RLOGD("VSIM_TIMEOUT:%d",time);
        if (time > 0) {
            s_timevalCloseVsim.tv_sec = time;
        }
    } else {
        err = at_send_command_multiline(socket_id, ATcmd, "",
                                        &p_response);
    }

    if (err < 0 || p_response->success == 0) {
        if (p_response != NULL) {
            strlcat(buf, p_response->finalResponse, sizeof(buf));
            strlcat(buf, "\r\n", sizeof(buf));
            response[0] = buf;
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, response,
                                  sizeof(char *));
        } else {
            goto error;
        }
    } else {
        p_cur = p_response->p_intermediates;
        for (i = 0; p_cur != NULL; p_cur = p_cur->p_next, i++) {
            strlcat(buf, p_cur->line, sizeof(buf));
            strlcat(buf, "\r\n", sizeof(buf));
        }
        strlcat(buf, p_response->finalResponse, sizeof(buf));
        strlcat(buf, "\r\n", sizeof(buf));
        response[0] = buf;

        RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(char *));
        if (!strncasecmp(ATcmd, "AT+SFUN=5", strlen("AT+SFUN=5"))) {
            setRadioState(socket_id, RADIO_STATE_OFF);
        }
    }
    at_response_free(p_response);
    return;

error:
    if (t != NULL) {
        memset(buf, 0 ,sizeof(buf));
        strlcat(buf, "ERROR", sizeof(buf));
        strlcat(buf, "\r\n", sizeof(buf));
        response[0] = buf;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, response,
                              sizeof(char *));
    }
    at_response_free(p_response);
}

static void requestGetVolteAllowedPlmn(RIL_SOCKET_ID socket_id, void *data,
                                       size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    char *line = NULL;
    int skip = 0, delta = 0, type = 0;

    int err = at_send_command_singleline(socket_id, "AT+SPDLTNV?",
            "+SPDLTNV:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    // +SPDLTNV: <n>,[<inf_type>,<inf_detal>],[<inf_typex>,<inf_detalx>]...
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &skip);    // n
    if (err < 0) goto error;

    // inf_type
    // 1: volte white information, to indicate whether current PLMN in volte white list
    err = at_tok_nextint(&line, &type);
    if (err < 0 || type != 1) goto error;

    err = at_tok_nextint(&line, &delta);    // inf_detal
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &delta, sizeof(delta));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetSpecialRATCAP(RIL_SOCKET_ID socket_id, void *data,
                                    size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    char *line = NULL;
    int response = 0;
    int err = -1;
    char cmd[AT_COMMAND_LEN] = {0};

    if (data == NULL) {
        goto error;
    }

    int value = ((int *)data)[0];

    if (-1 == value) {
        snprintf(cmd, sizeof(cmd), "AT+SPOPRCAP=1,2");
    } else {
        snprintf(cmd, sizeof(cmd), "AT+SPOPRCAP=1,2,%d", value);
    }

    err = at_send_command_singleline(socket_id, cmd,
                                    "+SPOPRCAP:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response);
    if (err < 0) goto error;
    err = at_tok_nextint(&line, &response);//only need the second
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}


void downgradeRadioPower(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                         RIL_Token t) {
    ATResponse *p_response = NULL;
    int err = 0;
    int mode = ((int *)data)[0];
    char cmd[AT_COMMAND_LEN] = {0};
    if (1 == mode) {  //turn on power regress/lift function and use setA configuration
        snprintf(cmd, sizeof(cmd), "AT+SPPOWERBFCOM=0,0,1");
        err = at_send_command(socket_id, cmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            goto error;
        }
        AT_RESPONSE_FREE(p_response);
    }
    memset(cmd, 0, sizeof(cmd));
    snprintf(cmd, sizeof(cmd), "AT+SPPOWERFB=%d", mode);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
error:
    at_response_free(p_response);
}

int processMiscRequests(int request, void *data, size_t datalen, RIL_Token t,
                        RIL_SOCKET_ID socket_id) {
    int err = -1;
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_BASEBAND_VERSION:
            requestBasebandVersion(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GET_IMEI: {
            err = at_send_command_numeric(socket_id, "AT+CGSN",
                                          &p_response);

            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(
                    t, RIL_E_SUCCESS, p_response->p_intermediates->line,
                    strlen(p_response->p_intermediates->line) + 1);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_GET_IMEISV:
            requestGetIMEISV(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_DEVICE_IDENTITY:
            requestDeviceIdentify(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_MUTE: {
            char cmd[AT_COMMAND_LEN] = {0};
            snprintf(cmd, sizeof(cmd), "AT+CMUT=%d", ((int *)data)[0]);
            if (s_maybeAddCall == 1 && ((int *)data)[0] == 0) {
                RLOGD("Don't cancel mute when dialing the second call");
                s_maybeAddCall = 0;
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                break;
            }
            at_send_command(socket_id, cmd, NULL);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_GET_MUTE: {
            p_response = NULL;
            int response = 0;
            err = at_send_command_singleline(
                socket_id, "AT+CMUT?", "+CMUT: ", &p_response);
            if (err >= 0 && p_response->success) {
                char *line = p_response->p_intermediates->line;
                err = at_tok_start(&line);
                if (err >= 0) {
                    err = at_tok_nextint(&line, &response);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                                          sizeof(response));
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }

            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_SCREEN_STATE:
            requestScreeState(socket_id, ((int *)data)[0], t);
            break;
        case RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER:
            requestSetUnsolResponseFilter(socket_id, ((int *)data)[0], t);
            break;
        case RIL_REQUEST_GET_HARDWARE_CONFIG:
            requestGetHardwareConfig(data, datalen, t);
            break;
        case RIL_REQUEST_OEM_HOOK_STRINGS: {
            const char **cur = (const char **)data;

            requestSendAT(socket_id, *cur, datalen, t, NULL, 0);
            break;
        }
        case RIL_EXT_REQUEST_SEND_CMD: {
            requestSendAT(socket_id, (const char *)data, datalen, t, NULL, 0);
            break;
        }
        case RIL_ATC_REQUEST_VSIM_SEND_CMD: {
            requestVsimCmd(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_RADIO_POWER_FALLBACK: {
            downgradeRadioPower(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_SET_LOCATION_INFO: {
            p_response = NULL;
            char cmd[64] = {0};
            char *longitude = ((char **)data)[0];
            char *latitude = ((char **)data)[1];
            if ((longitude == NULL) || (latitude == NULL) ||
                (strlen(longitude) > 20) || (strlen(latitude) > 20)) {
                RLOGE("The length of longitude or latitude is too long");
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            snprintf(cmd, sizeof(cmd), "AT+SPLOCINFO=\"%s\",\"%s\"", longitude, latitude);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_GET_SPECIAL_RATCAP:
            requestGetSpecialRATCAP(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_RESET_MODEM: {
            RLOGD("RIL_EXT_REQUEST_RESET_MODEM!");
            at_send_command(socket_id, "AT+RESET=1", NULL);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_EXT_REQUEST_GET_VOLTE_ALLOWED_PLMN: {
            requestGetVolteAllowedPlmn(socket_id, data, datalen, t);
            break;
        }
        default:
            return 0;
    }

    return 1;
}


int isVendorRadioProp(char *key) {
    int i;
    const char *prop_buffer[] = {
            "persist.vendor.radio.",
            "ro.vendor.modem.",
            "ro.vendor.radio.",
            "vendor.radio.",
            "vendor.ril.",
            "vendor.sim.",
            "vendor.data.",
            "vendor.net.",
            "persist.vendor.sys.",
            "vendor.sys.",
            NULL
    };
    for (i = 0; prop_buffer[i]; i++) {
        if (strncmp(prop_buffer[i], key, strlen(prop_buffer[i])) == 0) {
            return 1;
        }
    }
    return 0;
}

static void requestGetRadioPreference(int request, void *data,
        size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    char prop[PROPERTY_VALUE_MAX] = {0};
    char *key = NULL;

    key = ((char *)data);
    if (!isVendorRadioProp(key)) {
        RLOGE("get %s is not vendor radio prop", key);
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    } else {
        property_get(key, prop, "");
        RLOGD("get prop key = %s, value = %s", key, prop);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &prop, sizeof(prop));
    }
}

static void requestSetRadioPreference(int request, void *data,
        size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int ret = 0;
    char key[PROPERTY_VALUE_MAX] = {0};
    char value[PROPERTY_VALUE_MAX] = {0};

    strncpy(key, ((char **)data)[0], strlen(((char **)data)[0]) + 1);
    strncpy(value, ((char **)data)[1], strlen(((char **)data)[1]) + 1);

    if (!isVendorRadioProp(key)) {
        RLOGE("set %s is not vendor radio prop", key);
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    } else {
        RLOGD("set prop key = %s, value = %s", key, value);
        ret = property_set(key, value);
        if (ret < 0) {
            RLOGE("Error while set prop!");
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        }
    }
}

int processPropRequests(int request, void *data, size_t datalen, RIL_Token t) {
    switch (request) {
        case RIL_EXT_REQUEST_GET_RADIO_PREFERENCE: {
            requestGetRadioPreference(request, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_SET_RADIO_PREFERENCE: {
            requestSetRadioPreference(request, data, datalen, t);
            break;
        }
        default:
            return 0;
    }
    return 1;
}

//when high rate mode, open DVFS/Thermal/IPA/lock CUP Freq/RPS
static void handleHighRateMode() {
    RLOGD("handleHighRateMode START!");
    //open RPS
    property_set("ctl.start","vendor.rps_on");

    //CPU Frequency
    setCPUFrequency(true);

    setThermal(true);
    RLOGD("handleHighRateMode DONE!");
}

static void handleNormalRateMode() {
    RLOGD("handleNormalRateMode START!");
    //open RPS
    property_set("ctl.start","vendor.rps_off");

    //need auto CPU Frequency
    setCPUFrequency(false);

    //Thermal
    setThermal(false);
    RLOGD("handleNormalRateMode DONE!");
}

void notifyCModChgOver(RIL_SOCKET_ID socket_id) {
    int channel;
    int firstChannel, lastChannel;

#if defined (ANDROID_MULTI_SIM)
    firstChannel = socket_id * AT_CHANNEL_OFFSET;
    lastChannel = (socket_id + 1) * AT_CHANNEL_OFFSET;
#else
    firstChannel = AT_URC;
    lastChannel = MAX_AT_CHANNELS;
#endif

    for (channel = firstChannel; channel < lastChannel; channel++) {
        pthread_mutex_lock(&s_CModChgMutex[channel]);
        pthread_cond_signal(&s_CModChgCond[channel]);
        pthread_mutex_unlock(&s_CModChgMutex[channel]);
    }

    RLOGD("notify CModechange is over to all channels!");
}

static void onRadioUnavailable(void *param) {
    int off = *((int *)param);
    RLOGD("onRadioUnavailable: %s", off == 0 ? "true" : "false");

    for (int simId = 0; simId < SIM_COUNT; simId++) {
        if (off == 0) {
            setRadioState(simId, RADIO_STATE_UNAVAILABLE);
        } else {
            if (isRadioOn(simId) == 1) {
                setRadioState(simId, RADIO_STATE_ON);
            } else {
                setRadioState(simId, RADIO_STATE_OFF);
            }
        }
    }

    free(param);
}

int processMiscUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    int err = -1;
    char *line = NULL;

    if (strStartsWith(s, "+CTZV:")) {
        /* NITZ time */
        char *response = NULL;
        char *tmp = NULL;
        char *tmp_response = NULL;
        char tmpRsp[ARRAY_SIZE] = {0};

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &tmp_response);
        if (err != 0) {
            RLOGE("invalid NITZ line %s\n", s);
        } else {
            if (strstr(tmp_response, "//,::")) {
                char strTm[ARRAY_SIZE/2] = {0};
                time_t now = time(NULL);
                struct tm *curtime = gmtime(&now);

                strftime(strTm, sizeof(strTm), "%y/%m/%d,%H:%M:%S", curtime);
                snprintf(tmpRsp, sizeof(tmpRsp), "%s%s", strTm, tmp_response + strlen("//,::"));
                response = tmpRsp;
            } else {
                response = tmp_response;
            }
            RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED, response,
                                      strlen(response) + 1, socket_id);
        }
    } else if (strStartsWith(s, "%RSIMREQ:")) {
        char *tmp = NULL;
        char *response = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        skipWhiteSpace(&tmp);
        response = (char *)calloc((strlen(tmp) + 5), sizeof(char));
        snprintf(response, strlen(tmp) + 4, "%d,%s\r\n", socket_id, tmp);
        RIL_onUnsolicitedResponse(RIL_ATC_UNSOL_VSIM_RSIM_REQ, response,
                                  strlen(response) + 1, socket_id);
        free(response);
    } else if (strStartsWith(s, "+SPLTERATEMODE:")) {
       /*
        * +SPLTERATEMODE:<mode>,[max],[rate]
        *
        * <mode>    description
        * 0              Low/Normal rate mode
        * 1              High rate mode
        * <max>      Current band max rate
        * <rate>      Latest detected rate
        */
        int mode = 0, rate = 0, max_rate = 0;
        int err = -1;
        char* tmp = NULL;

        RLOGD("CA NVIOT rate URC: %s", s);
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &mode);
        if (err < 0) {
            RLOGD("CA NVIOT rate -- get mode error");
            goto out;
        }

        if (at_tok_hasmore(&tmp)) {
            err = at_tok_nextint(&tmp, &max_rate);
            if (err < 0) {
                RLOGD("CA NVIOT rate -- get max error");
                goto out;
            }
        }

        if (at_tok_hasmore(&tmp)) {
            err = at_tok_nextint(&tmp, &rate);
            if (err < 0) {
                RLOGD("CA NVIOT rate -- get rate error");
                goto out;
            }
        }

        if (mode) {
            handleHighRateMode();
        } else {
            handleNormalRateMode();
        }
    } else if (strStartsWith(s, "+SPCMODCHG:")) {
        /** used for checking that CP is mode changing.
         * Unsolicited info
         * +SPCMODCHG: <state>
         * OK
         *
         * Parameter:
         * <state>
         * 1  mode changing
         * 2  mode change finish
         */
        int state = 0;
        char* tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &state);
        if (err < 0) {
            RLOGE("check cp is mode changing -- error!");
            goto out;
        }

        if (1 == state) {
            s_CModChgState[socket_id] = true;
        } else if (2 == state) {
            s_CModChgState[socket_id] = false;
            notifyCModChgOver(socket_id);
        }
    } else if (strStartsWith(s, "+SPBANDSCAN:")) {
        int flag = -1;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &flag);

        int maxNum = 20;
        int curDataLen = 0;
        char *endChar = "\r\n";
        static int count = 0;
        static int dataLen = 0;
        static char **bandscanResults = NULL;

        if (count == 0) {
            bandscanResults = (char **)calloc(maxNum, sizeof(char *));
        }

        if (flag != 255 && flag != 254 && count < maxNum) {
            curDataLen = strlen(s) + sizeof("\r\n");
            bandscanResults[count] = (char *)calloc(curDataLen, sizeof(char));
            snprintf(bandscanResults[count], curDataLen, "%s%s", s, endChar);
            count++;
            dataLen += curDataLen;
        } else {
            dataLen += sizeof("+SPBANDSCAN: 255");
            char *response = (char *)calloc(dataLen, sizeof(char));

            int index = 0;
            for (index = 0; index < count; index++) {
                if (bandscanResults[index] != NULL) {
                    strncat(response, bandscanResults[index],
                            strlen(bandscanResults[index]));
                }
            }
            strncat(response, s, sizeof("+SPBANDSCAN: 255"));

            RLOGD("AT+SPBANDSCAN response:\n%s", response);

            const char *cmd = "+SPBANDSCAN:";
            RIL_Token t = NULL;
            void *data = NULL;

            onCompleteAsyncCmdMessage(socket_id, cmd, &t, &data);
            dispatchSPBANDSCAN(t, data, (void *)response);
            for (index = 0; index < count; index++) {
                FREEMEMORY(bandscanResults[index]);
            }
            FREEMEMORY(bandscanResults);
            FREEMEMORY(response);
            count = 0;
            dataLen = 0;
        }
    } else if (strStartsWith(s, "+MODECHAN:")) {
        int mode = -1, err = -1;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &mode);
        if (err < 0) goto out;

        if (mode == 0) {
            RLOGD("+MODECHAN: 0");
            s_isRadioUnavailable = true;
        } else if (mode == 2) {
            RLOGD("+MODECHAN: 2");
            s_isRadioUnavailable = false;
        } else {
            RLOGE("Invalid mode");
            goto out;
        }

        int *off = (int *)calloc(1, sizeof(int));
        *off = mode;
        RIL_requestTimedCallback(onRadioUnavailable, (void *)off, NULL);
    } else if (strStartsWith(s, "+SPRATEMODE:")) {
        /*
         * UNISOC :Bug1239906 auto open rps and gro
         * +SPRATEMODE:<mode>,[max],[rate]
         *
         * <mode>    description
         * 0         rate < 200mbps
         * 1         200mbps <= rate < 500mbps
         * 2         rate >= 500mbps
         * <max>      Current band max rate
         * <rate>      Latest detected rate
         */
        int mode;
        int err;
        char *tmp = NULL;

        RLOGD("CA NVIOT rate URC FOR NR: %s", s);
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &mode);
        if (err < 0) {
            RLOGD("CA NVIOT rate FOR NR -- get mode error");
            goto out;
        }

        RLOGD("CA NVIOT rate FOR NR -- mode = %d", mode);
        if (0 == mode) {
            property_set("ctl.start", "vendor.rps_off");
            property_set("ctl.start", "gro_off");
        } else if (1 == mode) {
            property_set("ctl.start", "vendor.rps_roc_m");
            property_set("ctl.start", "gro_on");
        } else if (2 == mode) {
            property_set("ctl.start", "vendor.rps_roc_h");
            property_set("ctl.start", "gro_on");
        } else {
            RLOGD("CA NVIOT rate FOR NR -- mode err!");
        }
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}

void dispatchSPBANDSCAN(RIL_Token t, void *data, void *resp) {
    if (t == NULL || resp == NULL) {
        return;
    }

    char *results[1] = {NULL};
    results[0] = (char *)resp;

    RLOGD("AT+SPBANDSCAN response complete");
    RIL_onRequestComplete(t, RIL_E_SUCCESS, results, sizeof(char *));
    free(data);
}
