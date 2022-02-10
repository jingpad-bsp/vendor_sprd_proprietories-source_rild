/**
 * ril_ss.c --- SS-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_ss.h"
#include "ril_sim.h"
#include "utils.h"

#define MO_CALL 0
#define MT_CALL 1

/**
 * add AT SPERROR for ussd
 * for s_ussdError : 0 --- no unsolicited SPERROR
 *                   1 --- unsolicited SPERROR
 * for s_ussdRun : 0 --- ussd to end
 *                 1 --- ussd to start
 */
int s_ussdError[SIM_COUNT];
int s_ussdRun[SIM_COUNT];

void onModemReset_Ss() {
    ;
}

void convertStringToHex(char *outString, char *inString, int len) {
    const char *hex = "0123456789ABCDEF";
    int i = 0;
    while (i < len) {
        *outString++ = hex[inString[i] >> 4];
        *outString++ = hex[inString[i] & 0x0F];
        ++i;
    }
    *outString = '\0';
}

static void requestSendUSSD(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                            RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err, ret, len;
    int errNum = -1, errCode = -1;
    char *cmd = NULL;
    char *line = NULL;
    char *ussdInitialRequest = NULL;
    char *ussdHexRequest = NULL;
    ATResponse *p_response = NULL;

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("card is absent");
        RIL_onRequestComplete(t, RIL_E_INVALID_STATE, NULL, 0);
        return;
    }

    s_ussdRun[socket_id] = 1;
    ussdInitialRequest = (char *)(data);
    len = strlen(ussdInitialRequest);
    ussdHexRequest = (char *)malloc(2 * len + 1);
    convertStringToHex(ussdHexRequest, ussdInitialRequest, len);
    ret = asprintf(&cmd, "AT+CUSD=1,\"%s\",15", ussdHexRequest);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        FREEMEMORY(ussdHexRequest);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    free(ussdHexRequest);
    if (err >= 0) {
        if (strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
            line = p_response->finalResponse;
            errCode = at_tok_start(&line);
            if (errCode >= 0) {
                errCode = at_tok_nextint(&line, &errNum);
            }
        }
    }
    if (errNum == 254) {
        RLOGE("Failed to send ussd by FDN check");
        s_ussdRun[socket_id] = 0;
        RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
    } else if (err < 0 || p_response->success == 0) {
        s_ussdRun[socket_id] = 0;
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

static void requestChangeFacilityLock(RIL_SOCKET_ID socket_id, char **data,
                                      size_t datalen, RIL_Token t) {
    int err = -1;
    int errNum = 0xff;
    int result;
    char *cmd = NULL, *line = NULL;
    ATResponse *p_response = NULL;

    if (datalen != 3 * sizeof(char *) || data[0] == NULL || data[1] == NULL ||
        data[2] == NULL || strlen(data[0]) == 0 ||  strlen(data[1]) == 0 ||
        strlen(data[2]) == 0) {
        RLOGE("CHANGE_BARRING_PASSWORD invalid arguments");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    err = asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", data[0], data[1],
                    data[2]);
    if (err < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }

    err = at_send_command(socket_id, cmd, &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        if (p_response != NULL &&
            strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
            line = p_response->finalResponse;
            err = at_tok_start(&line);
            if (err >= 0) {
                err = at_tok_nextint(&line, &errNum);
            }
        }
        goto error;
    }
    if (*data[1] == '1') {
        result = 1;
    } else {
        result = 0;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &result, sizeof(result));
    at_response_free(p_response);
    return;

error:
    if (err < 0 || errNum == 0xff || errNum == 3 ) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else if (errNum == 70 || errNum == 128 || errNum == 254) {
        RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    }
    at_response_free(p_response);
}

static int forwardFromCCFCLineUri(char *line,
                                      RIL_CallForwardInfoUri *p_forward) {
    int err;
    int i;

    // +CCFCU: <status>,<class1>[,<numbertype>,<ton>,
    // <number>[,<subaddr>,<satype>[,<time>]]]
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->status));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->serviceClass));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextint(&line, &p_forward->numberType);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &p_forward->ton);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &(p_forward->number));

        /* tolerate null here */
        if (err < 0) return 0;

        if (at_tok_hasmore(&line)) {
            for (i = 0; i < 2; i++) {
                skipNextComma(&line);
            }

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &p_forward->timeSeconds);
                if (err < 0) {
                    p_forward->timeSeconds = 0;
                    RLOGE("invalid CCFCU timeSeconds");
                }
            }
            if (at_tok_hasmore(&line)) {
                err = at_tok_nextstr(&line, &p_forward->ruleset);
                if (err < 0) {
                    RLOGE("invalid CCFCU ruleset");
                    p_forward->ruleset = NULL;
                }
            }
        }
    }

    return 0;

error:
    RLOGE("invalid CCFCU line");
    return -1;
}

static void requestCallForwardUri(RIL_SOCKET_ID socket_id, RIL_CallForwardInfoUri *data,
                                  size_t datalen, RIL_Token t) {
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;
    int err;
    int errNum = 0xff;
    char *cmd = NULL, *line = NULL;
    int ret = -1;

    if (datalen != sizeof(*data)) {
        goto error1;
    }

    if (data->serviceClass == 0) {
        if (data->status == 2) {
            ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
                data->reason,
                data->status,
                data->numberType,
                data->ton,
                data->number ? data->number : "",
                data->serviceClass);
        } else {
            if (data->timeSeconds != 0 && data->status == 3) {
                ret = asprintf(&cmd,
                        "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"\",\"\",,%d",
                        data->reason,
                        data->status,
                        data->numberType,
                        data->ton,
                        data->number ? data->number : "",
                        data->serviceClass,
                        data->timeSeconds);
            } else {
                ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
                        data->reason,
                        data->status,
                        data->numberType,
                        data->ton,
                        data->number ? data->number : "",
                        data->serviceClass);
            }
        }
    } else {
        if (data->status == 2) {
            ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
                    data->reason,
                    data->status,
                    data->numberType,
                    data->ton,
                    data->number ? data->number : "",
                    data->serviceClass);
        } else {
            if (data->timeSeconds != 0 && data->status == 3) {
                ret = asprintf(&cmd,
                        "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"%s\",\"\",,%d",
                        data->reason,
                        data->status,
                        data->numberType,
                        data->ton,
                        data->number ? data->number : "",
                        data->serviceClass,
                        data->ruleset ? data->ruleset : "",
                        data->timeSeconds);
            } else {
                ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"%s\"",
                        data->reason,
                        data->status,
                        data->numberType,
                        data->ton,
                        data->number ? data->number : "",
                        data->serviceClass,
                        data->ruleset ? data->ruleset : "");
            }
        }
    }
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error1;
    }
    err = at_send_command_multiline(socket_id, cmd, "+CCFCU:",
                                    &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    if (data->status == 2) {
        RIL_CallForwardInfoUri **forwardList, *forwardPool;
        int forwardCount = 0;
        int validCount = 0;
        int i;

        for (p_cur = p_response->p_intermediates; p_cur != NULL;
             p_cur = p_cur->p_next, forwardCount++) {
        }

        forwardList = (RIL_CallForwardInfoUri **)
            alloca(forwardCount * sizeof(RIL_CallForwardInfoUri *));

        forwardPool = (RIL_CallForwardInfoUri *)
            alloca(forwardCount * sizeof(RIL_CallForwardInfoUri));

        memset(forwardPool, 0, forwardCount * sizeof(RIL_CallForwardInfoUri));

        /* init the pointer array */
        for (i = 0; i < forwardCount; i++) {
            forwardList[i] = &(forwardPool[i]);
        }

        for (p_cur = p_response->p_intermediates; p_cur != NULL;
             p_cur = p_cur->p_next) {
            err = forwardFromCCFCLineUri(p_cur->line, forwardList[validCount]);
            forwardList[validCount]->reason = data->reason;
            if (err == 0) {
                validCount++;
            }
        }

        RIL_onRequestComplete(t, RIL_E_SUCCESS, validCount ? forwardList : NULL,
                              validCount * sizeof(RIL_CallForwardInfoUri *));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
    return;

error:
    if (data->status == 2) {
        if (p_response != NULL &&
            strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
            line = p_response->finalResponse;
            err = at_tok_start(&line);
            if (err < 0) goto error1;
            err = at_tok_nextint(&line, &errNum);
            if (err < 0) goto error1;
            if (errNum == 70 || errNum == 254 || errNum == 128) {
                RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
                at_response_free(p_response);
                return;
            }
        }
    }

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int forwardFromCCFCULine(char *line,
        RIL_CallForwardInfo *p_forward) {
    int err;
    int i;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->status));
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &(p_forward->serviceClass));
    if (err < 0) goto error;

    if (at_tok_hasmore(&line)) {
        int numberType = 0;
        err = at_tok_nextint(&line, &numberType);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &p_forward->toa);
        if (err < 0) goto error;

        err = at_tok_nextstr(&line, &(p_forward->number));

        /* tolerate null here */
        if (err < 0) return 0;

        if (at_tok_hasmore(&line)) {
            for (i = 0; i < 2; i++) {
                skipNextComma(&line);
            }

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &p_forward->timeSeconds);
                if (err < 0) {
                    p_forward->timeSeconds = 0;
                    RLOGE("invalid CCFCU timeSeconds");
                }
            }
        }
    }

    return 0;

error:
    RLOGE("invalid CCFCU line");
    return -1;
}

static void requestSetCallForward(RIL_SOCKET_ID socket_id, RIL_CallForwardInfo *data,
                                  size_t datalen, RIL_Token t) {
    int err;
    int errNum = 0xff;
    int ret = -1;
    char *cmd = NULL, *line = NULL;
    ATResponse *p_response = NULL;

    if (datalen != sizeof(*data) ||
            (data->status == 3 && data->number == NULL)) {
        goto error1;
    }
    if (data->serviceClass == 0) {
        if (data->timeSeconds != 0 && data->status == 3) {
            ret = asprintf(&cmd,
                    "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"\",\"\",,%d",
                    data->reason,
                    data->status,
                    2,
                    data->toa,
                    data->number ? data->number : "",
                    data->serviceClass,
                    data->timeSeconds);

        } else {
            ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
                    data->reason,
                    data->status,
                    2,
                    data->toa,
                    data->number ? data->number : "",
                    data->serviceClass);
        }
    } else {
        if (data->timeSeconds != 0 && data->status == 3) {
            ret = asprintf(&cmd,
                    "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"%s\",\"\",,%d",
                    data->reason,
                    data->status,
                    2,
                    data->toa,
                    data->number ? data->number : "",
                    data->serviceClass,
                    "",
                    data->timeSeconds);
        } else {
            ret = asprintf(&cmd, "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d,\"%s\"",
                    data->reason,
                    data->status,
                    2,
                    data->toa,
                    data->number ? data->number : "",
                    data->serviceClass,
                    "");
        }
    }
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error1;
    }
    err = at_send_command_multiline(socket_id, cmd, "+CCFCU:", &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
        strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err < 0) goto error1;
        err = at_tok_nextint(&line, &errNum);
        if (err < 0) goto error1;
        if (errNum == 70 || errNum == 254 || errNum == 128) {
            RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
            at_response_free(p_response);
            return;
        } else if (errNum == 21) {
            RIL_onRequestComplete(t, RIL_E_INVALID_STATE, NULL, 0);
            at_response_free(p_response);
            return;
        }
    }

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryCallForward(RIL_SOCKET_ID socket_id, RIL_CallForwardInfo *data,
                                    size_t datalen, RIL_Token t) {
    int err;
    int errNum = 0xff;
    int ret = -1;
    char *cmd = NULL, *line = NULL;
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;

    if (datalen != sizeof(*data)) {
        goto error1;
    }

    ret = asprintf(&cmd, "AT+CCFCU=%d,2,%d,%d,\"%s\",%d", data->reason, 2,
            data->toa, data->number ? data->number : "", data->serviceClass);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error1;
    }
    err = at_send_command_multiline(socket_id, cmd, "+CCFCU:",
                                    &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_CallForwardInfo **forwardList, *forwardPool;
    int forwardCount = 0;
    int validCount = 0;
    int i;

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next, forwardCount++) {
    }

    forwardList = (RIL_CallForwardInfo **)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo *));

    forwardPool = (RIL_CallForwardInfo *)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo));

    memset(forwardPool, 0, forwardCount * sizeof(RIL_CallForwardInfo));

    /* init the pointer array */
    for (i = 0; i < forwardCount; i++) {
        forwardList[i] = &(forwardPool[i]);
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        err = forwardFromCCFCULine(p_cur->line, forwardList[validCount]);
        forwardList[validCount]->reason = data->reason;
        if (err == 0) validCount++;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, validCount ? forwardList : NULL,
                          validCount * sizeof (RIL_CallForwardInfo *));
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
        strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
        line = p_response->finalResponse;
        err = at_tok_start(&line);
        if (err < 0) goto error1;
        err = at_tok_nextint(&line, &errNum);
        if (err < 0) goto error1;
        if (errNum == 70 || errNum == 254 || errNum == 128) {
            RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
            at_response_free(p_response);
            return;
        } else if (errNum == 21) {
            RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
            at_response_free(p_response);
            return;
        }
    }

error1:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

void requestQueryCOLP(RIL_SOCKET_ID socket_id, RIL_Token t) {
    int err;
    int response[2] = {0, 0};
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+COLP?",
                                     "+COLP: ", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response[1], sizeof(int));
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

void requestQueryCOLR(RIL_SOCKET_ID socket_id, RIL_Token t) {
    int err;
    int response[2] = {0, 0};
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+COLR?",
                                     "+COLR: ", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[0]);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0) goto error;

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response[1], sizeof(int));
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSetSuppServiceNotifications (RIL_SOCKET_ID socket_id,
        void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);

    int err = 0;
    ATResponse *p_response = NULL;
    int mode = ((int *)data)[0];
    char cmd[AT_COMMAND_LEN] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CSSN=%d,%d", mode, mode);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
}

int processSSRequests(int request, void *data, size_t datalen, RIL_Token t,
                      RIL_SOCKET_ID socket_id) {
    int err;
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_CANCEL_USSD: {
            p_response = NULL;
            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("card is absent");
                RIL_onRequestComplete(t, RIL_E_INVALID_STATE, NULL, 0);
                break;
            }
            err = at_send_command(socket_id, "AT+CUSD=2",
                                  &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_GET_CLIR: {
            int errNum = 0xff;
            int response[2] = {1, 1};
            char *line = NULL;

            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("GET_CLIR: card is absent");
                RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
                break;
            }

            p_response = NULL;
            err = at_send_command_singleline(socket_id,
                                            "AT+CLIR?", "+CLIR: ", &p_response);
            if (err >= 0 && p_response->success) {
                char *line = p_response->p_intermediates->line;

                err = at_tok_start(&line);
                if (err >= 0) {
                    err = at_tok_nextint(&line, &response[0]);

                    if (err >= 0)
                        err = at_tok_nextint(&line, &response[1]);
                }
                if (err >= 0) {
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
                                          sizeof(response));
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            } else {
                if (err >= 0) {
                    if (strStartsWith(p_response->finalResponse,
                                      "+CME ERROR:")) {
                        line = p_response->finalResponse;
                        err = at_tok_start(&line);
                        if (err >= 0) {
                            err = at_tok_nextint(&line, &errNum);
                        }
                    }
                }
                if (err >= 0 && (errNum == 70 || errNum == 128 ||
                                 errNum == 254)) {
                    RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_SET_CLIR: {
            int n = ((int *)data)[0];
            char cmd[AT_COMMAND_LEN] = {0};
            p_response = NULL;
            snprintf(cmd, sizeof(cmd), "AT+CLIR=%d", n);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
            requestQueryCallForward(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_SET_CALL_FORWARD:
            requestSetCallForward(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_CALL_WAITING: {
            int c = ((int *)data)[0];
            int errNum = 0xff;
            int mode, serviceClass;
            int response[2] = {0, 0};
            char cmd[AT_COMMAND_LEN] = {0};
            char *line;
            ATLine *p_cur;
            p_response = NULL;

            if (c == 0) {
                snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2");
            } else {
                snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2,%d", c);
            }
            err = at_send_command_multiline(socket_id, cmd,
                                            "+CCWA: ", &p_response);
            if (err >= 0 && p_response->success) {
                for (p_cur = p_response->p_intermediates; p_cur != NULL;
                     p_cur = p_cur->p_next) {
                    line = p_cur->line;
                    err = at_tok_start(&line);
                    if (err >= 0) {
                        err = at_tok_nextint(&line, &mode);
                        if (err >= 0) {
                            err = at_tok_nextint(&line, &serviceClass);
                            if (err >= 0) {
                                response[0] = mode;
                                response[1] |= serviceClass;
                            }
                        }
                    }
                }
                RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
                                      sizeof(response));
            } else {
                if (p_response != NULL &&
                    strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
                    line = p_response->finalResponse;
                    err = at_tok_start(&line);
                    if (err >= 0) {
                        err = at_tok_nextint(&line, &errNum);
                        if (err >= 0 && (errNum == 70 || errNum == 128 ||
                                         errNum == 254)) {
                            RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE,
                                                  NULL, 0);
                        } else if (err >= 0 && errNum == 0) {
                            RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS,
                                                  NULL, 0);
                        } else {
                            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE,
                                                  NULL, 0);
                        }
                    } else {
                        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL,
                                              0);
                    }
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_SET_CALL_WAITING: {
            p_response = NULL;
            int errNum = -1;
            int errCode = -1;
            char cmd[AT_COMMAND_LEN] = {0};
            char *line = NULL;
            int enable = ((int *)data)[0];
            int c = ((int *)data)[1];

            if (c == 0) {
                snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d", enable);
            } else {
                snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d,%d", enable, c);
            }

            err = at_send_command(socket_id, cmd,  &p_response);
            if (err < 0 || p_response->success == 0) {
                if (p_response != NULL &&
                    strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
                    line = p_response->finalResponse;
                    errCode = at_tok_start(&line);
                    if (errCode >= 0) {
                        errCode = at_tok_nextint(&line, &errNum);
                        if (errCode >= 0 && errNum == 254) {
                            RIL_onRequestComplete(t, RIL_E_FDN_CHECK_FAILURE,
                                                  NULL, 0);
                        } else if (errCode >= 0 && errNum == 176) {
                            RIL_onRequestComplete(t, RIL_E_INVALID_STATE,
                                                  NULL, 0);
                        } else {
                            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE,
                                                  NULL, 0);
                        }
                    } else {
                        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE,
                                NULL, 0);
                    }
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
            requestChangeFacilityLock(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_CLIP: {
            p_response = NULL;
            int skip = 0;
            int response = 0;

            if (s_isSimPresent[socket_id] != PRESENT) {
                RLOGE("QUERY_CLIP: card is absent");
                RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
                break;
            }

            err = at_send_command_singleline(socket_id,
                    "AT+CLIP?", "+CLIP: ", &p_response);
            if (err >= 0 && p_response->success) {
                char *line = p_response->p_intermediates->line;
                err = at_tok_start(&line);
                if (err >= 0) {
                    err = at_tok_nextint(&line, &skip);
                    if (err >= 0) {
                        err = at_tok_nextint(&line, &response);
                    }
                }

                if (err >= 0) {
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response,
                                          sizeof(response));
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }

            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
            requestSetSuppServiceNotifications(socket_id, data, datalen, t);
            break;
        /* IMS request @{ */
        case RIL_EXT_REQUEST_QUERY_CALL_FORWARD_STATUS_URI:
            requestCallForwardUri(socket_id, data, datalen, t);
            break;
        case RIL_EXT_REQUEST_SET_CALL_FORWARD_URI:
            requestCallForwardUri(socket_id, data, datalen, t);
            break;
        /* }@ */
        case RIL_EXT_REQUEST_QUERY_COLP:
            requestQueryCOLP(socket_id, t);
            break;
        case RIL_EXT_REQUEST_QUERY_COLR:
            requestQueryCOLR(socket_id, t);
            break;
        case RIL_EXT_REQUEST_UPDATE_CLIP: {
            int enable = ((int *)data)[0];
            char cmd[AT_COMMAND_LEN] = {0};
            p_response = NULL;

            snprintf(cmd, sizeof(cmd), "AT+CLIP=%d", enable);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_GET_CNAP: {
            int response[2] = {0};
            p_response = NULL;

            err = at_send_command_singleline(socket_id, "AT+CNAP?", "+CNAP: ",
                    &p_response);
            if (err >= 0 && p_response->success) {
                char *line = p_response->p_intermediates->line;
                err = at_tok_start(&line);
                if (err >= 0) {
                    err = at_tok_nextint(&line, &response[0]);
                    if (err >= 0) {
                        err = at_tok_nextint(&line, &response[1]);
                    }
                }
                if (err >= 0) {
                    RLOGD("CNAP respone %d, %d", response[0], response[1]);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
                                          sizeof(response));
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_QUERY_ROOT_NODE: {
            p_response = NULL;
            err = at_send_command(socket_id, "AT+SPUTROOT", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        default:
            return 0;
    }

    return 1;
}

int processSSUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    int err;
    char *line = NULL;

    if (strStartsWith(s, "+CUSD:")) {
        char *response[3] = {NULL, NULL, NULL};
        char *tmp = NULL;
        char *hexStr = NULL;
        char tmpStr[ARRAY_SIZE * 8] = {0};
        char utf8Str[ARRAY_SIZE * 8] = {0};

        s_ussdRun[socket_id] = 0;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &(response[0]));
        if (err < 0) {
            RLOGE("Error code not present");
            goto out;
        }

        err = at_tok_nextstr(&tmp, &hexStr);
        if (err == 0) {
            /* Convert the response, which is in the GSM 03.38 [25]
             * default alphabet, to UTF-8
             * Since the GSM alphabet characters contain a character
             * above 2048 (the Euro symbol)
             * the string can theoretically expand by 3/2 in length
             * when converted to UTF-8, so we allocate a new buffer, twice
             * the size of the one holding the hex string.*/
            err = at_tok_nextstr(&tmp, &(response[2]));
            if (err < 0) {
                RLOGD("%s fail", s);
                goto out;
            }
            /* convert hex string to string */
            convertHexToBin((const char *)hexStr, strlen(hexStr), tmpStr);
            if (strcmp(response[2], "15") == 0) {  // GSM_7BITS_TYPE
                convertGsm7ToUtf8((unsigned char *)tmpStr, strlen(hexStr) / 2,
                                  (unsigned char *)utf8Str);
            } else if (strcmp(response[2], "72") == 0) {  // UCS2_TYPE
                convertUcs2ToUtf8((unsigned char *)tmpStr, strlen(hexStr) / 2,
                                  (unsigned char *)utf8Str);
            }

            response[1] = utf8Str;
            if (strcmp(response[0], "2") == 0) {
                response[0] = "0";
            }
        } else {
            if (s_ussdError[socket_id] == 1) {  /* for ussd */
                RLOGD("+CUSD ussdError");
                // 4: network does not support the current operation
                response[0] = "4";
                response[1] = "";
                s_ussdError[socket_id] = 0;
            }
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_ON_USSD, response,
                                  2 * sizeof(char *), socket_id);
    } else if (strStartsWith(s, "+CSSI:")) {
        RIL_SuppSvcNotification *response = NULL;
        int code = 0;
        int index = 0;
        char *tmp;

        response =(RIL_SuppSvcNotification *)
                malloc(sizeof(RIL_SuppSvcNotification));
        if (response == NULL) {
            goto out;
        }
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &code);
        if (err < 0) {
            RLOGD("%s code fail", s);
            free(response);
            goto out;
        }
        err = at_tok_nextint(&tmp, &index);
        if (err < 0) {
            RLOGD("%s index fail ", s);
            index = 0;
        }
        response->notificationType = MO_CALL;
        response->code = code;
        response->index = index;
        response->type = 0;
        response->number = NULL;

        RIL_onUnsolicitedResponse(RIL_UNSOL_SUPP_SVC_NOTIFICATION, response,
                                  sizeof(RIL_SuppSvcNotification), socket_id);
        free(response);
    } else if (strStartsWith(s, "+CSSU:")) {
        RIL_SuppSvcNotification *response = NULL;
        int code = 0;
        int index = 0;
        char *tmp;

        response = (RIL_SuppSvcNotification *)
                malloc(sizeof(RIL_SuppSvcNotification));
        if (response == NULL) {
            goto out;
        }
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &code);
        if (err < 0) {
            RLOGD("%s code fail", s);
            free(response);
            goto out;
        }
        err = at_tok_nextint(&tmp, &index);
        if (err < 0) {
            RLOGD("%s index fail ", s);
            index = 0;
        }
        response->notificationType = MT_CALL;
        response->code = code;
        response->index = index;
        response->type = 0;
        response->number = NULL;

        RIL_onUnsolicitedResponse(RIL_UNSOL_SUPP_SVC_NOTIFICATION, response,
                                  sizeof(RIL_SuppSvcNotification), socket_id);
        free(response);
    } else if (strStartsWith(s, "+CNAP:")) {
        char *name = NULL;
        char *tmp;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &name);
        if (err < 0) goto out;

        RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_CNAP, name, strlen(name),
                                  socket_id);
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}
