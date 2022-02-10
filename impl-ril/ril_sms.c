/**
 * ril_sms.c --- SMS-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_sms.h"
#include "ril_sim.h"
#include "utils.h"

#define LOG_BUF_SIZE            512

void onModemReset_Sms() {
    ;
}

static void requestSendSMS(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                              RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, ret;
    int tpLayerLength;
    char *cmd1 = NULL, *cmd2 = NULL;
    char *line = NULL;
    RIL_SMS_Response response = {0};
    ATResponse *p_response = NULL;
    const char *smsc = ((const char **)data)[0];
    const char *pdu = ((const char **)data)[1];

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }
    tpLayerLength = strlen(pdu) / 2;

    /* "NULL for default SMSC" */
    if (smsc == NULL) {
        smsc = "00";
    }

    ret = asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd1);
        goto error1;
    }
    ret = asprintf(&cmd2, "%s%s", smsc, pdu);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd1);
        FREEMEMORY(cmd2);
        goto error1;
    }

    err = at_send_command_sms(socket_id, cmd1, cmd2, "+CMGS:",
                              &p_response);
    free(cmd1);
    free(cmd2);
    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    /* FIXME fill in messageRef and ackPDU */
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error1;

    err = at_tok_nextint(&line, &response.messageRef);
    if (err < 0) goto error1;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
            sizeof(RIL_SMS_Response));
    at_response_free(p_response);
    return;

error:
    if (p_response == NULL) {
        goto error1;
    }
    line = p_response->finalResponse;
    err = at_tok_start(&line);
    if (err < 0) goto error1;

    err = at_tok_nextint(&line, &response.errorCode);
    if (err < 0) goto error1;

    if (response.errorCode == 313) {
        RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, NULL, 0);
    } else if (response.errorCode == 512  || response.errorCode == 128 ||
               response.errorCode == 254 || response.errorCode == 514 ||
               response.errorCode == 515) {
        RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
    return;

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    return;
}

static void requestSendIMSSMS(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                  RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, ret;
    int tpLayerLength;
    char *pdu = NULL;
    char *line = NULL;
    char *cmd1 = NULL, *cmd2 = NULL;
    const char *smsc = NULL;
    RIL_SMS_Response response;
    ATResponse *p_response = NULL;
    RIL_IMS_SMS_Message *sms = NULL;

    sms = (RIL_IMS_SMS_Message *)data;
    if (sms->tech == RADIO_TECH_3GPP) {
        memset(&response, 0, sizeof(RIL_SMS_Response));

        smsc = ((char **)(sms->message.gsmMessage))[0];
        pdu = ((char **)(sms->message.gsmMessage))[1];
        if (sms->retry > 0) {
            /*
             * per TS 23.040 Section 9.2.3.6:  If TP-MTI SMS-SUBMIT (0x01) type
             * TP-RD (bit 2) is 1 for retry
             * and TP-MR is set to previously failed sms TP-MR
             */
            if (((0x01 & pdu[0]) == 0x01)) {
                pdu[0] |= 0x04;  // TP-RD
                pdu[1] = sms->messageRef;  // TP-MR
            }
        }

        tpLayerLength = strlen(pdu) / 2;
        /* "NULL for default SMSC" */
        if (smsc == NULL) {
            smsc = "00";
        }

        ret = asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
        if (ret < 0) {
            RLOGE("Failed to asprintf");
            FREEMEMORY(cmd1);
            goto error1;
        }
        ret = asprintf(&cmd2, "%s%s", smsc, pdu);
        if (ret < 0) {
            RLOGE("Failed to asprintf");
            FREEMEMORY(cmd1);
            FREEMEMORY(cmd2);
            goto error1;
        }

        err = at_send_command_sms(socket_id, cmd1, cmd2, "+CMGS:",
                                  &p_response);
        free(cmd1);
        free(cmd2);
        if (err != 0 || p_response->success == 0)
            goto error;

        /* FIXME fill in messageRef and ackPDU */
        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error1;

        err = at_tok_nextint(&line, &response.messageRef);
        if (err < 0) goto error1;

        RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                              sizeof(RIL_SMS_Response));
        at_response_free(p_response);
        return;

error:
        if (p_response == NULL) {
            goto error1;
        }
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err < 0) goto error1;

        err = at_tok_nextint(&line, &response.errorCode);
        if (err < 0) goto error1;
        if ((response.errorCode != 313) && (response.errorCode != 512)) {
            goto error1;
        }
        if (response.errorCode == 313) {
            RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, NULL, 0);
        } else if (response.errorCode == 512 || response.errorCode == 128 ||
                    response.errorCode == 254) {
            RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
        }
        at_response_free(p_response);
        return;

error1:
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

static void requestSMSAcknowledge(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int ackSuccess = ((int *)data)[0];;
    int err;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_INVALID_STATE , NULL, 0);
        return;
    }

    if (ackSuccess == 1) {
        err = at_send_command(socket_id, "AT+CNMA=1", NULL);
    } else if (ackSuccess == 0) {
        err = at_send_command(socket_id, "AT+CNMA=2", NULL);
    } else {
        RLOGE("Unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE");
        goto error;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

static void requestWriteSmsToSim(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                     RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int length;
    int err, ret;
    int errorNum;
    int index = 0;
    char *cmd = NULL, *cmd1 = NULL;
    const char *smsc = NULL;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_SMS_WriteArgs *p_args = NULL;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    p_args = (RIL_SMS_WriteArgs *)data;

    length = strlen(p_args->pdu) / 2;

    smsc = (const char *)(p_args->smsc);
    /* "NULL for default SMSC" */
    if (smsc == NULL) {
        smsc = "00";
    }

    ret = asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error1;
    }
    ret = asprintf(&cmd1, "%s%s", smsc, p_args->pdu);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        FREEMEMORY(cmd1);
        goto error1;
    }

    err = at_send_command_sms(socket_id, cmd, cmd1, "+CMGW:",
                              &p_response);
    free(cmd);
    free(cmd1);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &index);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &index, sizeof(index));
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
            strStartsWith(p_response->finalResponse, "+CMS ERROR:")) {
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err < 0) goto error1;

        err = at_tok_nextint(&line, &errorNum);
        if (err < 0) goto error1;

        if (errorNum != 322) {
            goto error1;
        }
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int setSmsBroadcastConfigValue(int value, char *out_value,
        size_t out_len ) {
    if (value < 0 || value > 0xffff) {
        return 0;
    } else {
        snprintf(out_value, out_len, "%d", value);
    }
    return 1;
}

static void setSmsBroadcastConfigData(int data, int idx, int isFirst,
        char *toStr, int *strLength, char *retStr) {
    RIL_UNUSED_PARM(toStr);

    int len = 0;
    char str[10] = {0};
    char comma = 0x2c;  // ,
    char quotes = 0x22;  // "
    char line = 0x2d;  // -

    memset(str, 0, 10);
    if (setSmsBroadcastConfigValue(data, str, 10) > 0) {
        if (idx == 0 && 1 == isFirst) {
            retStr[len] = quotes;
            len += 1;
        } else if (2 == isFirst) {
            retStr[0] = line;
            len += 1;
        } else {
            retStr[0] = comma;
            len += 1;
        }
        memcpy(retStr + len, str, strlen(str));
        len += strlen(str);
    }
    *strLength = len;
    RLOGI("setSmsBroadcastConfigData ret_char %s, len %d", retStr, *strLength);
}

#if 0  // unused functoin
// Add for command SPPWS
static void skipFirstQuotes(char **p_cur) {
    if (*p_cur == NULL) return;

    if (**p_cur == '"') {
        (*p_cur)++;
    }
}
#endif

static void requestSetSmsBroadcastConfig(RIL_SOCKET_ID socket_id, void *data,
                                              size_t datalen, RIL_Token t) {
    int err;
    int ret = -1;
    int enable = 0;
    int i = 0;
    int count = datalen / sizeof(RIL_GSM_BroadcastSmsConfigInfo *);
    int channelLen = 0;
    int langLen = 0;
    int len = 0;
    int serviceId[ARRAY_SIZE] = {0};
    int index = 0;
    char *cmd = NULL;
    char *channel = NULL;
    char *lang = NULL;
    char comma = 0x2c;
    char tmp[20] = {0};
    char quotes = 0x22;
    ATResponse *p_response = NULL;

    RIL_GSM_BroadcastSmsConfigInfo **gsmBciPtrs =
            (RIL_GSM_BroadcastSmsConfigInfo **)data;
    RIL_GSM_BroadcastSmsConfigInfo gsmBci = {0};

    int size = datalen * 16 * sizeof(char);
    channel = (char *)alloca(size);
    lang = (char *)alloca(size);

    memset(channel, 0, datalen * 16);
    memset(lang, 0, datalen * 16);

    for (i = 0; i < count; i++) {
        gsmBci = *(RIL_GSM_BroadcastSmsConfigInfo *)(gsmBciPtrs[i]);
        if (i == 0) {
            enable = gsmBci.selected ? 0 : 1;
        }
        /**
         * AT+CSCB = <mode>, <mids>, <dcss>
         * When mids are all different possible combinations of CBM message
         * identifiers, we send the range to modem. e.g. AT+CSCB=0,"4373-4383"
         */
        memset(tmp, 0, 20);
        setSmsBroadcastConfigData(gsmBci.fromServiceId, i, 1, channel, &len,
                tmp);
        memcpy(channel + channelLen, tmp, strlen(tmp));
        channelLen += len;
        serviceId[index++] = gsmBci.fromServiceId;
        if (gsmBci.fromServiceId != gsmBci.toServiceId) {
            memset(tmp, 0, 20);
            setSmsBroadcastConfigData(gsmBci.toServiceId, i, 2, channel, &len,
                                      tmp);
            memcpy(channel + channelLen, tmp, strlen(tmp));
            channelLen += len;
        }
        serviceId[index++] = gsmBci.toServiceId;

        memset(tmp, 0, 20);
        setSmsBroadcastConfigData(gsmBci.fromCodeScheme, i, 1, lang, &len, tmp);
        memcpy(lang + langLen, tmp, strlen(tmp));
        langLen += len;
        RLOGI("SetSmsBroadcastConfig lang %s, %d", lang, langLen);

        memset(tmp, 0, 20);
        setSmsBroadcastConfigData(gsmBci.toCodeScheme, i, 2, lang, &len, tmp);
        memcpy(lang + langLen, tmp, strlen(tmp));
        langLen += len;
        RLOGI("SetSmsBroadcastConfig lang %s, %d", lang, langLen);
    }
    if (langLen == 0) {
        snprintf(lang, size, "%c", quotes);
    }
    if (channelLen == 1) {
        snprintf(channel + channelLen, size - channelLen, "%c", quotes);
    }
    if (channelLen == 0) {
        snprintf(channel, size, "%c", quotes);
    }

    ret = asprintf(&cmd, "AT+CSCB=%d%c%s%c%c%s%c", enable, comma, channel,
                    quotes, comma, lang, quotes);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    AT_RESPONSE_FREE(p_response);

    /* Add for at command SPPWS @{ */
    if (enable == 0) {
        int cmas = 0;
        int etws = 0;
        int etwsTest = 0;
        int current = 0;

        RLOGD("index %d", index);
        for (current = 0; current < index; current = current + 2) {
            // cmas message under LTE, channel is from 4370 to 6400
            if (serviceId[current] >= 4370 && serviceId[current] <= 6400 &&
                    serviceId[current + 1] >= 4370 &&
                    serviceId[current + 1] <= 6400) {
                cmas++;
            } else if ((serviceId[current] >= 4352 && serviceId[current] <= 4359
                    && serviceId[current + 1] >= 4352 &&
                    serviceId[current + 1] <= 4359) &&
                    (serviceId[current] != 4355 &&
                            serviceId[current + 1] != 4355)) {
                 // etws primary and second message under LTE, channel is from
                 // 4352 to 4359 except 4355.
                 etws++;
            } else if (serviceId[current] == 4355 &&
                    serviceId[current + 1] ==4355) {
                // etws test message under LTE, channel is 4355
                etwsTest++;
            }
        }

        if (0 != etwsTest) {  // enable etws test message
            at_send_command(socket_id, "AT+SPPWS=2,2,1,2", NULL);
        }
        if (0 != cmas && 0 != etws) {  // enable etws and cmas message
            at_send_command(socket_id, "AT+SPPWS=1,1,2,1", NULL);
        } else {
            if (0 != cmas) {  // enable cmas message
                at_send_command(socket_id, "AT+SPPWS=2,2,2,1",
                        NULL);
            } else if (0 != etws) {  // enable etws message
               at_send_command(socket_id, "AT+SPPWS=1,1,2,2",
                       NULL);
            }
        }
    }
    /* }@ Add for at command SPPWS end */
}

static void requestGetSmsBroadcastConfig(RIL_SOCKET_ID socket_id, void *data,
                                              size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    ATResponse *p_response = NULL;
    int err = -1, mode, commas = 0, i = 0;
    char *line = NULL;
    char *serviceIds = NULL, *codeSchemes = NULL, *p = NULL;
    char *serviceId = NULL, *codeScheme = NULL;

    err = at_send_command_singleline(socket_id, "AT+CSCB?",
                                     "+CSCB:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &mode);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &serviceIds);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &codeSchemes);
        if (err < 0) goto error;

        for (p = serviceIds; *p != '\0'; p++) {
            if (*p == ',') {
                commas++;
            }
        }
        RIL_GSM_BroadcastSmsConfigInfo **gsmBciPtrs =
                (RIL_GSM_BroadcastSmsConfigInfo **)alloca((commas + 1) *
                        sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
        memset(gsmBciPtrs, 0, (commas + 1) *
                sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
        for (i = 0; i < commas + 1; i++) {
            gsmBciPtrs[i] = (RIL_GSM_BroadcastSmsConfigInfo *)alloca(
                    sizeof(RIL_GSM_BroadcastSmsConfigInfo));
            memset(gsmBciPtrs[i], 0, sizeof(RIL_GSM_BroadcastSmsConfigInfo));

            err = at_tok_nextstr(&serviceIds, &serviceId);
            if (err < 0) goto error;

            p = NULL;
            p = strsep(&serviceId, "-");
            RLOGD("requestGetSmsBroadcastConfig p %s,serviceId=%s",p,serviceId);
            gsmBciPtrs[i]->fromServiceId = p != NULL ? atoi(p) : 0;

            gsmBciPtrs[i]->toServiceId =
                    serviceId != NULL ?
                            atoi(serviceId) : gsmBciPtrs[i]->fromServiceId;

            err = at_tok_nextstr(&codeSchemes, &codeScheme);
            if (err < 0) goto error;

            p = NULL;
            p = strsep(&codeScheme, "-");
            RLOGD("GetSmsBroadcastConfig p %s,codeScheme=%s",p,codeScheme);
            gsmBciPtrs[i]->fromCodeScheme = p != NULL ? atoi(p) : 0;

            gsmBciPtrs[i]->toCodeScheme =
                    codeScheme != NULL ?
                            atoi(codeScheme) : gsmBciPtrs[i]->fromCodeScheme;
            gsmBciPtrs[i]->selected = mode == 0 ? false : true;
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, gsmBciPtrs, (commas + 1) *
                sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
        at_response_free(p_response);
        return;
    }
error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSmsBroadcastActivation(RIL_SOCKET_ID socket_id, void *data,
                                               size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    int *active = (int *)data;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CSCB=%d", active[0]);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestGetSmscAddress(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err;
    char *sc_line;
    ATResponse *p_response = NULL;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }
    err = at_send_command_singleline(socket_id, "AT+CSCA?",
                                     "+CSCA:", &p_response);
    if (err >= 0 && p_response->success) {
        char *line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err >= 0) {
            err = at_tok_nextstr(&line, &sc_line);
            char *decidata = (char *)calloc((strlen(sc_line) / 2 + 1),
                    sizeof(char));
            convertHexToBin(sc_line, strlen(sc_line), decidata);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, decidata,
                                  strlen(decidata) + 1);
            free(decidata);
        } else {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestGetSIMCapacity(RIL_SOCKET_ID socket_id, void *data,
                                  size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int i;
    int err;
    int response[2] = {-1, -1};
    char *line = NULL, *skip = NULL;
    char *responseStr[2] = {NULL, NULL};
    char res[2][20] = {{0}, {0}};
    ATResponse *p_response = NULL;

    for (i = 0; i < 2; i++) {
        responseStr[i] = res[i];
    }

    err = at_send_command_singleline(socket_id,
                                     "AT+CPMS?", "+CPMS:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &skip);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;

    if (response[0] == -1 || response[1] == -1) {
        goto error;
    }

    snprintf(res[0], sizeof(res[0]), "%d", response[0]);
    snprintf(res[1], sizeof(res[1]), "%d", response[1]);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, 2 * sizeof(char *));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestStoreSmsToSim(RIL_SOCKET_ID socket_id, void *data,
        size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err;
    char *line = NULL;
    char cmd[128] = {0};
    char *memoryRD = NULL;  // memory for read and delete
    char *memoryWS = NULL;  // memory for write and send
    ATResponse *p_response = NULL;
    int value = ((int *)data)[0];

    err = at_send_command_singleline(socket_id, "AT+CPMS?",
                "+CPMS:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &memoryRD);
    if (err < 0) goto error;

    skipNextComma(&line);
    skipNextComma(&line);

    err = at_tok_nextstr(&line, &memoryWS);
    if (err < 0) goto error;

    if (value == 1) {
        at_send_command(socket_id, "AT+CNMI=3,1,2,1,1", NULL);
        snprintf(cmd, sizeof(cmd), "AT+CPMS=\"%s\",\"%s\",\"SM\"", memoryRD,
                memoryWS);
    } else {
        char prop[ARRAY_SIZE];
        property_get(VSIM_PRODUCT_PROP, prop, "0");
        RLOGD("vsim product prop = %s", prop);
        if (strcmp(prop, "1") != 0) {
            at_send_command(socket_id, "AT+CNMI=3,2,2,1,1", NULL);
        } else {
            at_send_command(socket_id, "AT+CNMI=3,0,2,1,1", NULL);
        }
        snprintf(cmd, sizeof(cmd), "AT+CPMS=\"%s\",\"%s\",\"ME\"", memoryRD,
                memoryWS);
    }

    AT_RESPONSE_FREE(p_response);
    err = at_send_command_singleline(socket_id, cmd, "+CPMS:",
                                     &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQuerySmsStorageMode(RIL_SOCKET_ID socket_id, void *data,
        size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int commas = 0;
    char *response = NULL;
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+CPMS?",
            "+CPMS:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    // +CPMS:"ME",0,20,"ME",0,20, "SM",0,20
    // +CPMS:"ME",0,20,"ME",0,20, "ME",0,20
    for (commas = 0; commas < 6; commas++) {
        skipNextComma(&line);
    }
    err = at_tok_nextstr(&line, &response);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, strlen(response) + 1);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

//e.g. : 15 convert to ＂00001111＂
void convertUnsignedCharToBinary(unsigned char ch, unsigned char *bitflow, int len) {
    int i = 0;

    for (i = 0; i < len; i++) {
        bitflow[len - 1 - i] = ch%2;
        ch = ch/2;
    }
}

unsigned char convertBinaryToUnsignedChar(unsigned char* bitflow, int index, int len) {
    unsigned char dec = 0;
    int i = 0;

    for (; i < len; i++) {
        dec += bitflow[index + i] << (len - 1 - i);
    }

    return dec;
}

// 3GPP2 C.S0015-B 3.4 Transport Layer Messages
static void convertCdmaSmsMsgToPDU(RIL_CDMA_SMS_Message *rcsm, char *pdu) {
    int index = 0;
    int bits = 0;
    int tpLayerLength = 0;
    unsigned char addrContent[128] = {0};

    memcpy(pdu, "00", 2);//message type
    tpLayerLength = 1;

    //the ID(00) LEN(02) and value of TeleserviceID
    snprintf(pdu + tpLayerLength * 2, 5, "%s", "0002");
    tpLayerLength += 2;
    snprintf(pdu + tpLayerLength * 2, 5, "%04x", rcsm->uTeleserviceID);
    tpLayerLength += 2;

    if (rcsm->bIsServicePresent) {
        //the ID(01) LEN(02) and value of ServiceCategory
        snprintf(pdu + tpLayerLength * 2, 5, "%s", "0102");
        tpLayerLength += 2;

        snprintf(pdu + tpLayerLength * 2, 5, "%04x", rcsm->uServicecategory);
        tpLayerLength += 2;
        RLOGD("add rcsm->uServicecategory!");
    }

    addrContent[bits++] = rcsm->sAddress.digit_mode;
    addrContent[bits++] = rcsm->sAddress.number_mode;

    if (rcsm->sAddress.digit_mode == RIL_CDMA_SMS_DIGIT_MODE_8_BIT) {
        convertUnsignedCharToBinary(rcsm->sAddress.number_type, addrContent + bits, 3);
        bits += 3;
        RLOGD("add rcsm->sAddress.number_type!");
        if (rcsm->sAddress.number_mode == RIL_CDMA_SMS_NUMBER_MODE_NOT_DATA_NETWORK) {
            convertUnsignedCharToBinary(rcsm->sAddress.number_plan, addrContent + bits, 4);
            bits += 4;
            RLOGD("add rcsm->sAddress.number_plan!");
        }
    }

    convertUnsignedCharToBinary(rcsm->sAddress.number_of_digits, addrContent + bits, 8);
    bits += 8;

    for (index = 0; index < rcsm->sAddress.number_of_digits; index++) {
        if (rcsm->sAddress.digit_mode == RIL_CDMA_SMS_DIGIT_MODE_8_BIT) {
            convertUnsignedCharToBinary(rcsm->sAddress.digits[index], addrContent + bits, 8);
            bits += 8;
            RLOGD("digit is 8 bits!");
        } else {
            convertUnsignedCharToBinary(rcsm->sAddress.digits[index], addrContent + bits, 4);
            bits += 4;
        }
    }

    //RESERVED bit, fill with 0
    for (index = 0; index < 7; index++) {
        if (bits % 8 == 0) break;
        addrContent[bits++] = 0;
    }

    snprintf(pdu + tpLayerLength * 2, 3, "%s", "04"); //addr id
    tpLayerLength += 1;

    snprintf(pdu + tpLayerLength * 2, 3, "%02x", bits / 8);// addr len
    tpLayerLength += 1;

    RLOGD("printf cdma pdu1 = %s", pdu);

    index = 0;
    while (bits > 0) {
        unsigned char temp = convertBinaryToUnsignedChar(addrContent, index, 4);
        snprintf(pdu + tpLayerLength * 2 + index / 4, 2, "%x", temp);
        index += 4;
        bits -= 4;
    }

    tpLayerLength += index / 8; //addrContent len
    RLOGD("printf cdma pdu2 = %s", pdu);

    //add RIL_CDMA_SMS_BEARER_RPLY_OPT filed
    snprintf(pdu + tpLayerLength * 2, 7, "%s", "060100");
    tpLayerLength += 3;

    //message type ID
    snprintf(pdu + tpLayerLength * 2, 3, "%s", "08");
    tpLayerLength += 1;

    snprintf(pdu + tpLayerLength * 2, 3, "%02x", rcsm->uBearerDataLen);
    tpLayerLength += 1;

    RLOGD("printf cdma pdu3 = %s", pdu);
    convertBinToHex(((char *)rcsm->aBearerData), rcsm->uBearerDataLen,
            (unsigned char *)(pdu + tpLayerLength * 2));
    RLOGD("printf cdma pdu = %s", pdu);
}

static void requestCdmaSendSMS(RIL_SOCKET_ID socket_id, void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int err, ret;
    char pdu[1024] = {0};
    char *cmd = NULL;
    char *line = NULL;
    RIL_SMS_Response response;
    RIL_CDMA_SMS_Message *rcsm = (RIL_CDMA_SMS_Message *)data;
    ATResponse *p_response = NULL;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    RLOGD("requestCdmaSendSMS datalen = %zu, sizeof(RIL_CDMA_SMS_Message) = %zu",
            datalen, sizeof(RIL_CDMA_SMS_Message));

    memset(&response, 0, sizeof(RIL_SMS_Response));
    RLOGD("TeleserviceID = %d, bIsServicePresent = %d, \
            uServicecategory = %d, sAddress.digit_mode = %d, \
            sAddress.Number_mode = %d, sAddress.number_type = %d",
            rcsm->uTeleserviceID, rcsm->bIsServicePresent,
            rcsm->uServicecategory, rcsm->sAddress.digit_mode,
            rcsm->sAddress.number_mode, rcsm->sAddress.number_type);

    convertCdmaSmsMsgToPDU(rcsm, pdu);
    ret = asprintf(&cmd, "AT+CMGS=%d", (int)(strlen(pdu) / 2));

    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }

    err = at_send_command_sms(socket_id, cmd, pdu, "+CMGS:",
                              &p_response);
    FREEMEMORY(cmd);

    if (err != 0 || p_response->success == 0) {
        goto error;
    }

    /* FIXME fill in messageRef and ackPDU */
    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error1;

    err = at_tok_nextint(&line, &response.messageRef);
    if (err < 0) goto error1;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    if (p_response == NULL) {
        goto error1;
    }

    line = p_response->finalResponse;
    err = at_tok_start(&line);
    if (err < 0) goto error1;

    err = at_tok_nextint(&line, &response.errorCode);
    if (err < 0) goto error1;

    if (response.errorCode == 313) {
        RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, NULL, 0);
    } else if (response.errorCode == 512  || response.errorCode == 128 ||
               response.errorCode == 254 || response.errorCode == 514 ||
               response.errorCode == 515) {
        RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }

    at_response_free(p_response);
    return;

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestCdmaSMSAcknowledge(RIL_SOCKET_ID socket_id, void *data,
                                      size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    RIL_CDMA_SMS_Ack* rcsm;
    int ackSuccess;
    int err;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("CDMA: card is absent");
        RIL_onRequestComplete(t, RIL_E_NO_SMS_TO_ACK, NULL, 0);
        return;
    }
    rcsm = (RIL_CDMA_SMS_Ack *)data;
    ackSuccess = (int)rcsm->uErrorClass;

    if (ackSuccess == 0) {
        //AT+CNMA=1 : SUCCESS; AT+CNMA=2 : FAIL
        err = at_send_command(socket_id, "AT+CNMA=1", NULL);
    } else if (ackSuccess == 1) {
        err = at_send_command(socket_id, "AT+CNMA=2", NULL);
    } else {
        RLOGE("CDMA: Unsupported arg to RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE\n");
        goto error;
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/* 3GPP2 C.S0015-B 3.4.33 Address Parameters */
void decodeCdmaSmsAddress(unsigned char* bytePdu, RIL_CDMA_SMS_Address* address, int length) {
    int index = 0;
    int chari_len = 4;
    int i = 0;
    unsigned char* bitflow = (unsigned char*) malloc(length * 8);

    for (i = 0; i < length; i ++) {
        convertUnsignedCharToBinary(bytePdu[i], bitflow + i * 8, 8);
    }

    address->digit_mode = bitflow[index++];
    address->number_mode = bitflow[index++];

    if (address->digit_mode == RIL_CDMA_SMS_DIGIT_MODE_8_BIT) {
        //change an address digit or character into 8bits
        chari_len = 8;
        RLOGD("change an address digit or character into 8bits");

        address->number_type = convertBinaryToUnsignedChar(bitflow, index, 3);
        index += 3;
        RLOGD("decodeCdmaSmsAddress: address->number_type = %d", address->number_type);

        if (address->number_mode == RIL_CDMA_SMS_NUMBER_MODE_NOT_DATA_NETWORK) {
            address->number_plan = convertBinaryToUnsignedChar(bitflow, index, 4);
            index += 4;
            RLOGD("decodeCdmaSmsAddress: address->number_plan = %d", address->number_plan);
        }
    }

    address->number_of_digits = convertBinaryToUnsignedChar(bitflow, index, 8);
    index += 8;
    RLOGD("decodeCdmaSmsAddress: number_of_digits: %d", address->number_of_digits);

    for (i = 0; i < address->number_of_digits; i ++) {
        address->digits[i] = convertBinaryToUnsignedChar(bitflow, index, chari_len);
        index += chari_len;
        RLOGD("address->digits[%d] = 0x%2x", i, address->digits[i]);
    }

    RLOGD("chari_len = %d", chari_len);
    free(bitflow);
}

/*3GPP2 C.S0015-B*/
RIL_CDMA_SMS_Message buildCdmaSmsMessage(const char *sms_pdu) {
    RIL_CDMA_SMS_Message sms = {0};
    int index = 2; // +CMT first 0000 don't used
    int param_id, param_len;
    unsigned char bytePdu[AT_RESPONSE_LEN] = {0};
    int length = 0;

    convertHexToBin(sms_pdu, strlen(sms_pdu), (char *)bytePdu);
    length = strlen(sms_pdu) / 2;
    while (index < length) {
        param_id = bytePdu[index++];
        param_len = bytePdu[index++];

        switch (param_id) {
        case TeleserviceIdentifier:
            if (param_len != 2) {
                RLOGE("buildCdmaSmsMessage: get TeleserviceIdentifier failed");
            } else {
                sms.uTeleserviceID = (((unsigned short)bytePdu[index]) << 8)
                                    + (unsigned short)bytePdu[index + 1];
                RLOGD("sms.uTeleserviceID: 0x%x\n", sms.uTeleserviceID);
            }
            break;
        case ServiceCategory:
            if (param_len != 2) {
                RLOGE("buildCdmaSmsMessage: get ServiceCategory failed");
            } else {
                sms.uServicecategory = (((unsigned short)bytePdu[index]) << 8)
                                    + (unsigned short)bytePdu[index + 1];
                RLOGD("sms.uServicecategory: 0x%x\n", sms.uServicecategory);
            }
            break;
        case DestinationAddress:
        case OriginatingAddress:
            decodeCdmaSmsAddress(bytePdu+index, &(sms.sAddress), param_len);
            break;
        case OriginatingSubaddress:
        case DestinationSubaddress:
            RLOGE("buildCdmaSmsMessage: get SubAddress do not supported now...");
            break;
        case BearerReplyOption:
            RLOGE("buildCdmaSmsMessage: didn't decode BearerReplyOption");
            break;
        case CauseCodes:
            RLOGE("buildCdmaSmsMessage: didn't decode CauseCodes");
            break;
        case BearerData:
            sms.uBearerDataLen = param_len;
            memcpy(sms.aBearerData, bytePdu + index, sms.uBearerDataLen);
            break;
        default:
            break;
        }
        index += param_len;
    }

    return sms;
}

static void requestWriteCdmaSmsToRuim(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                                      RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, ret;
    int index = 0;
    char pdu[1024] = {0};
    char *cmd = NULL;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_CDMA_SMS_WriteArgs *p_args = (RIL_CDMA_SMS_WriteArgs *)data;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    convertCdmaSmsMsgToPDU(&(p_args->message), pdu);
    ret = asprintf(&cmd, "AT+CMGW=%d,%d", (int)(strlen(pdu) / 2), p_args->status);

    if (ret < 0) {
        RLOGE("Failed to allocate memory");
        FREEMEMORY(cmd);
        goto error;
    }

    err = at_send_command_sms(socket_id, cmd, pdu, "+CMGW:",
                              &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &index);
    if (err < 0) goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &index, sizeof(index));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetSmsBearer(RIL_SOCKET_ID socket_id, void *data,
                                  size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int err = -1;
    int response = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id,
                                     "AT+CASIMS?", "+CASIMS:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) {
        goto error;
    }
    err = at_tok_nextint(&line, &response);
    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

int processSmsRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id) {
    int err;
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_SEND_SMS:
        case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
            requestSendSMS(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_IMS_SEND_SMS:
            requestSendIMSSMS(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM:
        case RIL_REQUEST_DELETE_SMS_ON_SIM: {
            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("card is absent");
                RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
                break;
            }
            char cmd[AT_COMMAND_LEN] = {0};
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", ((int *)data)[0]);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
            requestSmsBroadcastActivation(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
            requestSetSmsBroadcastConfig(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
            requestGetSmsBroadcastConfig(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_GET_SMSC_ADDRESS:
            requestGetSmscAddress(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_SMSC_ADDRESS: {
            char *cmd = NULL;
            int ret;
            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("card is absent");
                RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
                break;
            }
            if (data == NULL || strlen(data) == 0) {
                RLOGE("SET_SMSC_ADDRESS invalid adress: %s", data);
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            int hexLen = strlen(data) * 2 + 1;
            unsigned char *hexData =
                    (unsigned char *)calloc(hexLen, sizeof(unsigned char));
            convertBinToHex(data, strlen(data), hexData);

            ret = asprintf(&cmd, "AT+CSCA=\"%s\"", hexData);
            if (ret < 0) {
                RLOGE("Failed to asprintf");
                FREEMEMORY(hexData);
                FREEMEMORY(cmd);
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            err = at_send_command(socket_id, cmd, &p_response);
            free(cmd);
            free(hexData);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS: {
            char cmd[AT_COMMAND_LEN] = {0};
            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("card is absent");
                RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
                break;
            }
            snprintf(cmd, sizeof(cmd), "AT+SPSMSFULL=%d", !((int *)data)[0]);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        /* IMS request @{ */
        case RIL_EXT_REQUEST_SET_IMS_SMSC: {
            char *cmd = NULL;
            int ret;
            p_response = NULL;
            RLOGD("[sms]RIL_EXT_REQUEST_SET_IMS_SMSC (%s)", (char *)(data));
            ret = asprintf(&cmd, "AT+PSISMSC=\"%s\"", (char *)(data));
            if (ret < 0) {
                RLOGE("Failed to asprintf!");
                FREEMEMORY(cmd);
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            err = at_send_command(socket_id, cmd, &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        /* }@ */
        case RIL_EXT_REQUEST_GET_SIM_CAPACITY: {
            requestGetSIMCapacity(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_STORE_SMS_TO_SIM:
            requestStoreSmsToSim(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_QUERY_SMS_STORAGE_MODE:
            requestQuerySmsStorageMode(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_SEND_SMS:
            requestCdmaSendSMS(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE:
            requestCdmaSMSAcknowledge(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM:
            requestWriteCdmaSmsToRuim(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_SET_SMS_BEARER:{
            p_response = NULL;
            int n = ((int *)data)[0];
            char cmd[AT_COMMAND_LEN] = {0};
            snprintf(cmd, sizeof(cmd), "AT+CASIMS=%d", n);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_GET_SMS_BEARER: {
            /* add for bug1181272  */
            requestGetSmsBearer(socket_id, data, datalen, t);
            break;
        }
        default:
            return 0;
    }

    return 1;
}

int processSmsUnsolicited(RIL_SOCKET_ID socket_id, const char *s,
                          const char *sms_pdu) {
    char *line = NULL;
    int err;

    if (strStartsWith(s, "+CMT:")) {
        if (!s_isCDMAPhone[socket_id]) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS, sms_pdu,
                                      strlen(sms_pdu), socket_id);
        } else {
            RIL_CDMA_SMS_Message sms = buildCdmaSmsMessage(sms_pdu);
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CDMA_NEW_SMS,
                    &sms, sizeof(RIL_CDMA_SMS_Message), socket_id);
        }
    } else if (strStartsWith(s, "+CDS:")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
                                  sms_pdu, strlen(sms_pdu), socket_id);
    } else if (strStartsWith(s, "+CMGR:")) {
        if (sms_pdu != NULL) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS, sms_pdu,
                                      strlen(sms_pdu), socket_id);
        } else {
            RLOGD("[cmgr] sms_pdu is NULL");
        }
    } else if (strStartsWith(s, "+CMTI:")) {
        /* can't issue AT commands here -- call on main thread */
        int location;
        char *response = NULL;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &response);
        if (err < 0) {
            RLOGD("sms request fail");
            goto out;
        }
        if (strcmp(response, "SM")) {
            RLOGD("sms request arrive but it is not a new sms");
            goto out;
        }

        /* Read the memory location of the sms */
        err = at_tok_nextint(&tmp, &location);
        if (err < 0) {
            RLOGD("error parse location");
            goto out;
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM, &location,
                                  sizeof(location), socket_id);
    } else if (strStartsWith(s, "+CBM:")) {
        int smsPDULen = (int)strlen(sms_pdu);
        char *pdu_bin = NULL;

        RLOGD("\"%s\" len = %d, sms_pdu len = %d", s, (int)strlen(s), smsPDULen);
        pdu_bin = (char *)calloc(smsPDULen / 2 + 1, sizeof(char));
        if (!convertHexToBin(sms_pdu, smsPDULen, pdu_bin)) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
                                      pdu_bin, smsPDULen / 2, socket_id);
        } else {
            int i;
            int segments = smsPDULen / LOG_BUF_SIZE;
            char smsPDUTmp[LOG_BUF_SIZE + 1] = {0};

            RLOGE("Convert hex to bin failed for SMSCB");
            for (i = 0; i <= segments; i++) {
                snprintf(smsPDUTmp, LOG_BUF_SIZE + 1, "%s", sms_pdu + LOG_BUF_SIZE * i);
                RLOGE("%s", smsPDUTmp);
            }
        }
        free(pdu_bin);
    } else if (strStartsWith(s, "+SPLWRN:")) {
        //  +SPLWRN:<segment_id>,<total_segments>,<length>,<CR><LF><data>
        int skip;
        int segmentId;
        int totalSegments;
        static int count = 0;
        static int dataLen = 0;

        static char **pdus = NULL;
        char *msg = NULL;
        char *tmp = NULL;
        char *data = NULL;
        char *binData = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &segmentId);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &totalSegments);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &skip);
        if (err < 0) goto out;

        err = at_tok_nextstr(&tmp, &data);
        if (err < 0) goto out;

        /* Max length of SPWRN message is
         * 9600 byte and each time ATC can only send 1k. When 9600 is divided
         * by 1024, the quotient is 9 with a remainder of 1.
         */

        if (totalSegments < 10 && count == 0) {
            pdus = (char **)calloc(totalSegments, sizeof(char *));
        }
        if (pdus == NULL) {
            RLOGE("pdus is NULL");
            goto out;
        }
        if (segmentId <= totalSegments) {
            pdus[segmentId -1] =
                    (char *)calloc(strlen(data) + 1, sizeof(char));
            snprintf(pdus[segmentId -1], strlen(data) + 1, "%s", data);

            count++;
            dataLen += strlen(data);
        }

        // To make sure no missing pages, then concat all pages.
        if (count == totalSegments) {
            msg = (char *)calloc(dataLen + 1, sizeof(char));
            int index = 0;
            for (; index < count; index++) {
                if (pdus[index] != NULL) {
                    strncat(msg, pdus[index], strlen(pdus[index]));
                }
            }
            RLOGD("concat pdu: %s", msg);
            /* +SPLWRN:1,N,<xx>,<data1>
             * +SPLWRN:2,N,<xx>,<data2>
             * ...
             * +SPLWRN:N,N,<xx>,<dataN>
             * Response data1 + data2 + ... + dataN to framework
             */
            binData = (char *)calloc(strlen(msg) / 2 + 1, sizeof(char));
            if (!convertHexToBin(msg, strlen(msg), binData)) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
                        binData, strlen(msg) / 2, socket_id);
            } else {
                RLOGD("Convert hex to bin failed for SPLWRN");
            }
            free(msg);
            free(binData);
            for (index = 0; index < count; index++) {
                free(pdus[index]);
            }
            free(pdus);
            pdus = NULL;
            dataLen = 0;
            count = 0;
        }
    } else if (strStartsWith(s, "^SMOF:")) {
        int value;
        char *tmp;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &value);
        if (err < 0) goto out;

        if (value == 2) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_SMS_STORAGE_FULL, NULL, 0,
                                      socket_id);
        }
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}

