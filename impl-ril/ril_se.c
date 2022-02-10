/**
 * ril_se.c --- secure element-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_se.h"
#include "ril_sim.h"
#include "channel_controller.h"

int s_channelNumber[SIM_COUNT] = {0};

bool initForSeService(int simId) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return false;
    }

    bool ret = false;
    int err = -1;
    char *cpinLine = NULL, *cpinResult = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(simId, "AT+CPIN?", "+CPIN:", &p_response);
    if (err < 0 || p_response->success == 0) goto done;

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start(&cpinLine);
    if (err < 0) goto done;

    err = at_tok_nextstr(&cpinLine, &cpinResult);
    if (err < 0) goto done;

    if (strcmp(cpinResult, "READY") == 0) {
        ret = true;
    }

done:
    at_response_free(p_response);
    return ret;
}

void getAtrForSeService(int simId, void *response, int *responseLen) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return;
    }

    int err;
    char *line = NULL;
    char *resp = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(simId, "AT+SPATR?", "+SPATR:", &p_response);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        RLOGD("getAtr AT return failed");
        goto error;
    }

    line = p_response->p_intermediates->line;
    if (at_tok_start(&line) < 0) goto error;
    if (at_tok_nextstr(&line, &resp) < 0) goto error;

    memcpy((char *)response, resp, strlen(resp));
    *responseLen = strlen(resp);
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
}

bool isCardPresentForSeService(int simId) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return false;
    }

    bool ret = false;
    if (s_isSimPresent[simId] != ABSENT) {
        ret = true;
        RLOGD("isCardPresent");
    }
    return ret;
}

void transmitForSeService(int simId, void *data, void *response) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return;
    }

    int err = -1, len = 0, i = 0;
    char *line = NULL;
    char cmd[AT_COMMAND_LEN * 10] = {0};
    char tmp[AT_COMMAND_LEN * 10] = {0};
    char respBuf[71680] = {0};  // 70k
    SE_APDU *apdu = (SE_APDU *)data;
    SE_APDU *resp = (SE_APDU *)response;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));
    pthread_mutex_lock(&s_CglaCrsmMutex[simId]);

    if ((apdu->len == 7) && (apdu->data[4] == 0x00)) {
        // bug1068801, total apdu len is 7 bytes and fifth byte is 0x00
        for (i = 0; i < 5; i++) {  // send first 5 bytes to cp
            snprintf(tmp + i * 2, sizeof(tmp) - i * 2, "%02X", apdu->data[i]);
        }
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,%d,\"%s\"", s_channelNumber[simId], 10, tmp);
    } else if ((apdu->len >= 262) && (apdu->data[4] == 0x00)) {
        // bug1068836, total apdu len is longer than 262 bytes and fifth byte is 0x00
        char commonHead[16] = {0};
        char lastHead[16] = {0};
        int maxAPDULen = 255;
        int removeBytes = 7;
        int commonP3 = 0xFF;
        int lastP3 = (apdu->len - removeBytes) % maxAPDULen;  // length of bytes in Decimal
        int segmentNum = (apdu->len - removeBytes) / maxAPDULen;

        for (int i = 0; i < 4; i++) {
            snprintf(commonHead + i * 2, sizeof(commonHead) - i * 2, "%02X", apdu->data[i]);
        }
        snprintf(lastHead, sizeof(lastHead),  "%s%02X", commonHead, lastP3);
        snprintf(commonHead + i * 2, sizeof(commonHead) - i * 2, "%02X", commonP3);
        RLOGD("lastHead = %s", lastHead);

        for (int i = 0; i < segmentNum; i++) {
            int pOffset = 0;
            for (int j = removeBytes; j < (removeBytes + maxAPDULen); j++) {
                // remove first 7 bytes, loop 255 times
                int index = maxAPDULen * i + j;  // index
                snprintf(tmp + pOffset, sizeof(tmp) - pOffset, "%02X", apdu->data[index]);
                pOffset += 2;
            }
            snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,%d,\"%s%s\"", s_channelNumber[simId],
                    10 + maxAPDULen * 2, commonHead, tmp);
            at_send_command_singleline(simId, cmd, "+CGLA:", NULL);
            memset(tmp, 0, sizeof(tmp));
        }

        for (int i = 0; i < lastP3; i++) {
            int index = removeBytes + maxAPDULen * segmentNum + i;
            snprintf(tmp + i * 2, sizeof(tmp) - i * 2, "%02X", apdu->data[index]);
        }
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,%d,\"%s%s\"", s_channelNumber[simId],
                 10 + lastP3 * 2, lastHead, tmp);
    } else {
        for (i = 0; i < (int)(apdu->len); i++) {
            snprintf(tmp + i * 2, sizeof(tmp) - i * 2, "%02X", apdu->data[i]);
        }
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,%d,\"%s\"", s_channelNumber[simId],
                 (int)((apdu->len) * 2), tmp);
    }

RETRY:
    err = at_send_command_singleline(simId, cmd, "+CGLA:", &p_response);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        RLOGE("transmit(): +CGLA return failed");
        goto error;
    }

    line = p_response->p_intermediates->line;
    if (at_tok_start(&line) < 0 || at_tok_nextint(&line, &len) < 0 ||
        at_tok_nextstr(&line, &(sr.simResponse)) < 0) {
        RLOGE("transmit(): at_tok failed");
        goto error;
    }

    resp->len = strlen(sr.simResponse);
    if (strncmp(sr.simResponse, "6881", resp->len) == 0) {  // bug1063360
        sr.simResponse = "";
    } else if ((resp->len >= 4) &&
                strncmp(sr.simResponse + resp->len - 4, "61", 2) == 0) {  // bug1071112
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,10,\"%02XC00000%s\"", s_channelNumber[simId],
                 apdu->data[0], sr.simResponse + resp->len - 2);
        if (resp->len > 4) {  // bug1060206
            strncat(respBuf, sr.simResponse, resp->len - 4);
        }
        AT_RESPONSE_FREE(p_response);
        goto RETRY;
    } else if ((apdu->len == 4) && (resp->len >= 4) &&
                strncmp(sr.simResponse + resp->len - 4, "6C", 2) == 0) {  // bug1107931
        snprintf(cmd, sizeof(cmd), "AT+CGLA=%d,10,\"%s%s\"", s_channelNumber[simId], tmp,
                 sr.simResponse + resp->len - 2);
        AT_RESPONSE_FREE(p_response);
        goto RETRY;
    }

    strncat(respBuf, sr.simResponse, resp->len);

    resp->len = strlen(respBuf);
    resp->data = (uint8_t *)calloc(resp->len, sizeof(uint8_t));
    memcpy(resp->data, (uint8_t *)respBuf, (resp->len) * sizeof(uint8_t));

error:
    pthread_mutex_unlock(&s_CglaCrsmMutex[simId]);
    AT_RESPONSE_FREE(p_response);
}

SE_Status openLogicalChannelForSeService(int simId, void *data, void *resp, int *responseLen) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return FAILED;
    }

    pthread_mutex_lock(&s_SPCCHOMutex[simId]);  // bug1071409
    int respLen = 1;
    int err, i = 0;
    int response[ARRAY_SIZE * 4] = {0};
    SE_Status errType = FAILED;
    char *line = NULL;
    char cmd[AT_COMMAND_LEN * 10] = {0};
    char tmp[AT_COMMAND_LEN] = {0};
    char *statusWord = NULL;
    char *respData = NULL;
    SE_OpenChannelParams *params = (SE_OpenChannelParams *)data;
    ATResponse *p_response = NULL;

    if (params->len == 0) {  // bug1059702
        errType = NO_SUCH_ELEMENT_ERROR;
        err = at_send_command_singleline(simId,
                "AT+CSIM=10,\"0070000001\"", "+CSIM:", &p_response);
    } else {
        for (i = 0; i < (int)(params->len); i++) {
            snprintf(tmp + i * 2, sizeof(tmp) - i * 2, "%02X", params->aidPtr[i]);
        }
        snprintf(cmd, sizeof(cmd), "AT+SPCCHO=\"%s\",%d", tmp, params->p2);

        err = at_send_command_singleline(simId, cmd, "+SPCCHO:", &p_response);
    }
    pthread_mutex_unlock(&s_SPCCHOMutex[simId]);
    if (err < 0) goto error;
    if (p_response != NULL && p_response->success == 0) {
        if (params->len != 0) {
            if (!strcmp(p_response->finalResponse, "+CME ERROR: 20")) {
                errType = FAILED;
            } else if (!strcmp(p_response->finalResponse, "+CME ERROR: 22")) {
                errType = NO_SUCH_ELEMENT_ERROR;
            } else if (!strcmp(p_response->finalResponse, "+CME ERROR: 100")) {
                // bug1063352
                errType = IOERROR;
            }
        }
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[0]);  // Read channel number
    if (err < 0) goto error;
    if (params->len != 0) {
        s_channelNumber[simId] = response[0];
    }

    if (at_tok_hasmore(&line)) {  // Read select response (if available)
        err = at_tok_nextstr(&line, &statusWord);
        if (err < 0) goto error;
        if (at_tok_hasmore(&line)) {
            err = at_tok_nextstr(&line, &respData);
            if (err < 0) goto error;
            int length = strlen(respData) / 2;
            while (respLen <= length) {
                sscanf(respData, "%02x", &(response[respLen]));
                respLen++;
                respData += 2;
            }
        } else {
            if (params->len == 0) {  // response format: +CSIM: 6,019000
                // 9000 represents success, and it's status code
                if (strlen(statusWord) >= strlen("019000") &&
                   (strncmp(statusWord + 2, "9000", strlen("9000")) == 0)) {
                    sscanf(&(statusWord[0]), "%2x", &(response[0]));
                    s_channelNumber[simId] = response[0];
                    statusWord = statusWord + 2;
                } else {
                    goto error;
                }
            }
        }
        sscanf(statusWord, "%02x%02x", &(response[respLen]), &(response[respLen + 1]));
        respLen = respLen + 2;
    } else {  // no select response, set status word
        response[respLen] = 0x90;
        response[respLen + 1] = 0x00;
        respLen = respLen + 2;
    }
    RLOGD("response[0]=%d, response[1]=0x%02X, response[2]=0x%02X", response[0],
            response[1], response[2]);
    memcpy((int *)resp, response, respLen * sizeof(int));
    *responseLen = respLen * sizeof(int);
    errType = SUCCESS;

error:
    at_response_free(p_response);
    return errType;
}

SE_Status openBasicChannelForSeService(int simId, void *data, void *response) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return UNSUPPORTED_OPERATION;
    }
    /* @@@ TODO */
    RIL_UNUSED_PARM(simId);
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(response);
    return UNSUPPORTED_OPERATION;
}

SE_Status closeChannelForSeService(int simId, uint8_t channelNumber) {
    if (s_modemState != MODEM_ALIVE) {
        RLOGE("Modem is not alive, return radio_not_avaliable");
        return FAILED;
    }

    RLOGD("closeChannel: simId = %d, channelNumber = %d", simId, channelNumber);
    int err = 0, status = SUCCESS;
    int sessionId = (int)channelNumber;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    if (sessionId <= 0) {
        status = FAILED;
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCHC=%d", sessionId);
        err = at_send_command(simId, cmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            status = FAILED;
        }
        at_response_free(p_response);
    }
    s_channelNumber[simId] = 0;
    return status;
}
