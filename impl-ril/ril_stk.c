/**
 * ril_stk.c --- STK-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_stk.h"
#include "ril_network.h"
#include "ril_sim.h"
#include "ril_data.h"
#include "ril_public.h"
#include "channel_controller.h"
#include "utils.h"

void getDefaultBearerNetAccessName(RIL_SOCKET_ID socket_id, char *apn, size_t size);

void onModemReset_Stk() {
    resetStkVariables();
}

int sendTRData(RIL_SOCKET_ID socket_id, char *data) {
    int ret = -1;
    int err = -1;
    char *cmd = NULL;
    ATResponse *p_response = NULL;

    ret = asprintf(&cmd, "AT+SPUSATTERMINAL=\"%s\"", data);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }
    err = at_send_command_singleline(socket_id, cmd, "+SPUSATTERMINAL:",
            &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    AT_RESPONSE_FREE(p_response);
    return 1;

error:
    AT_RESPONSE_FREE(p_response);
    return 0;
}

int sendELData(RIL_SOCKET_ID socket_id, char *data) {
    int ret = -1;
    int err = -1;
    char *cmd = NULL;
    ATResponse *p_response = NULL;

    ret = asprintf(&cmd, "AT+SPUSATENVECMD=\"%s\"", data);
    if (ret < 0) {
        RLOGE("Failed to asprintf");
        FREEMEMORY(cmd);
        goto error;
    }
    err = at_send_command_singleline(socket_id, cmd, "+SPUSATENVECMD:",
            &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }
    AT_RESPONSE_FREE(p_response);
    return 1;

error:
    AT_RESPONSE_FREE(p_response);
    return 0;
}

int phoneIsBusy(RIL_SOCKET_ID socket_id) {
    int err = -1;
    int countCalls = 0;
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;
    err = at_send_command_multiline(socket_id, "AT+CLCC",
                                    "+CLCC:", &p_response);
    if (err != 0 || p_response->success == 0) {
        goto done;
    }

    /* total the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        countCalls++;
    }

done:
    at_response_free(p_response);
    return countCalls;
}

int sendDtmfData(RIL_SOCKET_ID socket_id, char *data) {
    int ret = 0;
    char character = ((char *)data)[0];
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+VTS=%c", (int)character);
    ret = at_send_command(socket_id, cmd, &p_response);
    if (ret < 0 || p_response->success == 0) {
        ret = -1;
    }

    AT_RESPONSE_FREE(p_response);
    return ret;
}

void getDefaultBearerNetAccessName(RIL_SOCKET_ID socket_id, char *apn, size_t size) {
    int err = -1;
    char *apnTemp = NULL;
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;
    ATResponse *p_newResponse = NULL;

    err = at_send_command_multiline(socket_id,
                "AT+SPIPCONTEXT?", "+SPIPCONTEXT:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    cgdcont_read_cmd_rsp(p_response, &p_newResponse);
    for (p_cur = p_newResponse->p_intermediates; p_cur != NULL;
            p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int ncid = -1;
        int active = -1;
        int ipType = -1;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &ncid);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &active);
        if (err < 0) goto error;

        if (ncid == 1) {
            err = at_tok_nextstr(&line, &apnTemp);
            if (err < 0) goto error;
            if (emNStrlen(apnTemp) != 0) {
                snprintf(apn, size, "%s", apnTemp);
                break;
            }
            err = at_tok_nextint(&line, &ipType);
            if (err < 0) goto error;

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextstr(&line, &apnTemp);
                if (err < 0) goto error;
            }
            if (emNStrlen(apnTemp) != 0) {
                snprintf(apn, size, "%s", apnTemp);
                break;
            }
        }
    }

    AT_RESPONSE_FREE(p_response);
    AT_RESPONSE_FREE(p_newResponse);
    return;

error:
    AT_RESPONSE_FREE(p_response);
    AT_RESPONSE_FREE(p_newResponse);
}

static void requestSendEnvelopeWithStatus(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
        RIL_Token t) {
    int err = -1, resplen = 0;
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr = {0};
    char cmd[AT_COMMAND_LEN] = {0};

    snprintf(cmd, sizeof(cmd), "AT+CSIM=%d,\"%s\"", (int)(datalen - 1), (char *)data);
    err = at_send_command_singleline(socket_id, cmd, "+CSIM:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &resplen);
    if (err < 0 || resplen < 4) goto error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto error;

    sscanf(&(sr.simResponse[resplen - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[resplen - 4] = '\0';

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

int processStkRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id) {
    RIL_UNUSED_PARM(datalen);

    int err;
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_STK_GET_PROFILE: {
            char *line;
            p_response = NULL;
            err = at_send_command_singleline(socket_id, "AT+SPUSATPROFILE?",
                    "+SPUSATPROFILE:", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                line = p_response->p_intermediates->line;
                RIL_onRequestComplete(t, RIL_E_SUCCESS, line,
                        strlen(line) + 1);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND: {
            char *cmd = NULL;
            int ret;
            if ((char *)(data) == NULL || strlen((char *)data) == 0) {
                RLOGE("data is invalid");
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
                break;
            }
            ret = asprintf(&cmd, "AT+SPUSATENVECMD=\"%s\"", (char *)(data));
            if (ret < 0) {
                RLOGE("Failed to asprintf");
                FREEMEMORY(cmd);
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            err = at_send_command_singleline(socket_id, cmd, "+SPUSATENVECMD:",
                    &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE: {
            char *cmd = NULL;
            int ret;
            if ((char *)(data) == NULL || strlen((char *)data) == 0) {
                RLOGE("data is invalid");
                RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
                break;
            }

            ret = lunchOpenChannelDialog((char *)(data), socket_id);
            if (ret != 1) {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                break;
            }
            ret = asprintf(&cmd, "AT+SPUSATTERMINAL=\"%s\"", (char *)(data));
            if (ret < 0) {
                RLOGE("Failed to asprintf");
                FREEMEMORY(cmd);
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            err = at_send_command_singleline(socket_id, cmd, "+SPUSATTERMINAL:",
                    &p_response);
            free(cmd);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM: {
            int value = ((int *)data)[0];
            if (value == 0) {
                err = at_send_command(socket_id, "AT+SPUSATCALLSETUP=0",
                        &p_response);
            } else {
                err = at_send_command(socket_id, "AT+SPUSATCALLSETUP=1",
                        &p_response);
            }
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING: {
            char lastResponse[AT_RESPONSE_LEN] = {0};
            int retValue = reportStkServiceRunning(socket_id, lastResponse,
                    sizeof(lastResponse));
            if (1 == retValue) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND,
                        lastResponse, strlen(lastResponse) + 1, socket_id);
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                break;
            }

            err = at_send_command_singleline(socket_id, "AT+SPUSATPROFILE?",
                    "+SPUSATPROFILE:", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS:
            requestSendEnvelopeWithStatus(socket_id, data, datalen, t);
            break;
        default:
            return 0;
    }

    return 1;
}

int processStkUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    int err;
    char *line = NULL;

    if (strStartsWith(s, "+SPUSATENDSESSIONIND")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0,
                socket_id);
    } else if (strStartsWith(s, "+SPUSATPROCMDIND:")) {
        char *tmp = NULL;;
        char *response = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextstr(&tmp, &response);
        if (err < 0) {
            RLOGE("%s fail", s);
            goto out;
        }

        int retValue = parseProCmdIndResponse(response, socket_id);
        if (retValue == 1) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, response,
                                      strlen(response) + 1, socket_id);
        }
    } else if (strStartsWith(s, "+SPUSATDISPLAY:")) {
         char *tmp = NULL;
         char *response = NULL;

         line = strdup(s);
         tmp = line;
         at_tok_start(&tmp);
         err = at_tok_nextstr(&tmp, &response);
         if (err < 0) {
             RLOGE("%s fail", s);
             goto out;
         }

         parseDisplayCmdIndResponse(response, socket_id);

         RIL_onUnsolicitedResponse(RIL_UNSOL_STK_EVENT_NOTIFY, response,
                                   strlen(response) + 1, socket_id);
    } else if (strStartsWith(s, "+SPUSATSETUPCALL:")) {
        char *response = NULL;
        char *tmp;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextstr(&tmp, &response);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }

        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, response,
                                  strlen(response) + 1, socket_id);
    } else if (strStartsWith(s, "+SPUSATREFRESH:")) {
        char *tmp;
        int result = 0;
        RIL_SimRefreshResponse_v7 *response = NULL;

        response = (RIL_SimRefreshResponse_v7 *)
                   alloca(sizeof(RIL_SimRefreshResponse_v7));
        if (response == NULL) {
            goto out;
        }
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &result);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->ef_id);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        skipNextComma(&tmp);
        err = at_tok_nextstr(&tmp, &response->aid);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        response->result = result;
        if (SIM_RESET == result) {
            s_imsInitISIM[socket_id] = -1;
            setStkServiceRunning(socket_id, false);
        }
        if (SIM_INIT == result) {
            s_imsInitISIM[socket_id] = -1;
        }
        response->aid = "";
        RIL_onUnsolicitedResponse(RIL_UNSOL_SIM_REFRESH, response,
                sizeof(RIL_SimRefreshResponse_v7), socket_id);
    /* add for alpha identifier display in stk @{ */
    } else if (strStartsWith(s, "+SPUSATCALLCTRL:")) {
        char *tmp;
        RIL_StkCallControlResult *response = NULL;;

        response = (RIL_StkCallControlResult *)alloca(sizeof(RIL_StkCallControlResult));
        if (response == NULL) goto out;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        err = at_tok_nextint(&tmp, &response->call_type);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->result);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->is_alpha);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->alpha_len);
        if (err < 0 || response->alpha_len == 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextstr(&tmp, &response->alpha_data);
        if (err < 0 || strlen(response->alpha_data) == 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->pre_type);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->ton);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->npi);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextint(&tmp, &response->num_len);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        err = at_tok_nextstr(&tmp, &response->number);
        if (err < 0) {
            RLOGD("%s fail", s);
            goto out;
        }
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_CC_ALPHA_NOTIFY,
                                  response->alpha_data,
                                  strlen(response->alpha_data) + 1,
                                  socket_id);
    /* @} */
    } else {
        return 0;
    }

out:
    free(line);
    return 1;
}
