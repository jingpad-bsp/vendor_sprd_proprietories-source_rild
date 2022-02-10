/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include "impl_ril.h"
#include "ril_network.h"
#include "channel_controller.h"

int s_ATTableSize = 0;

const cmd_table s_ATTimeoutTable[] = {
        {AT_CMD_STR("AT+CREG"), 15},
        {AT_CMD_STR("AT+CGREG"), 15},
        {AT_CMD_STR("AT+CEREG"), 15},
        {AT_CMD_STR("AT+CESQ"), 5},
        {AT_CMD_STR("AT+COPS=0"), 180},
        {AT_CMD_STR("AT+COPS=3"), 10},
        {AT_CMD_STR("AT+COPS?"), 10},
        {AT_CMD_STR("AT+CPIN?"), 8},
        {AT_CMD_STR("AT+CPIN="), 50},
        {AT_CMD_STR("AT+CPWD"), 60},
        {AT_CMD_STR("AT+ECPIN2"), 60},
        {AT_CMD_STR("AT+EUICC"), 60},
        {AT_CMD_STR("AT+CMGS=?"), 5},
        {AT_CMD_STR("AT+CMGS="), 138},
        {AT_CMD_STR("AT+SNVM=1"), 10},
        {AT_CMD_STR("AT+CMGW=?"), 5},
        {AT_CMD_STR("AT+CMGW="), 10},
        {AT_CMD_STR("ATD"), 40},
        {AT_CMD_STR("ATA"), 40},
        {AT_CMD_STR("ATH"), 40},
        {AT_CMD_STR("AT+CLCC"), 5},
        {AT_CMD_STR("AT+SFUN=2"),300},
        {AT_CMD_STR("AT+SFUN=4"), 180},
        {AT_CMD_STR("AT+CTZR"), 5},
        {AT_CMD_STR("AT+CGSN"), 8},
        {AT_CMD_STR("AT+CIMI"), 5},
        {AT_CMD_STR("AT+CGMR"), 8},
        {AT_CMD_STR("AT+CGDCONT?"), 5},
        {AT_CMD_STR("AT+CGDCONT="), 5},
        {AT_CMD_STR("AT+CGDATA="), 155},
        {AT_CMD_STR("AT+CGACT?"), 5},
        {AT_CMD_STR("AT+CGACT=1"), 600},
        {AT_CMD_STR("AT+CGACT=0"), 50},
        {AT_CMD_STR("AT+COPS=?"), 210},
        {AT_CMD_STR("AT+CCWA?"), 30},
        {AT_CMD_STR("AT+CCWA=?"), 5},
        {AT_CMD_STR("AT+CCWA="), 50},
        {AT_CMD_STR("AT+COLP?"), 30},
        {AT_CMD_STR("AT+COLP=?"), 5},
        {AT_CMD_STR("AT+COLP="), 50},
        {AT_CMD_STR("AT+EVTS="), 10},
        {AT_CMD_STR("AT+CRSM"), 60},
        {AT_CMD_STR("AT+CMOD"), 5},
        {AT_CMD_STR("AT+CLIP?"), 30},
        {AT_CMD_STR("AT+CLIP=?"), 5},
        {AT_CMD_STR("AT+CLIP="), 50},
        {AT_CMD_STR("AT+CSSN"), 5},
        {AT_CMD_STR("AT+CUSD?"), 5},
        {AT_CMD_STR("AT+CUSD=?"), 5},
        {AT_CMD_STR("AT+CUSD="), 50},
        {AT_CMD_STR("AT+CMGD"), 5},
        {AT_CMD_STR("AT+CSCA"), 5},
        {AT_CMD_STR("AT+CNMA"),  5},
        {AT_CMD_STR("AT+CNMI"), 5},
        {AT_CMD_STR("AT+CMMS"), 5},
        {AT_CMD_STR("AT+VTS"), 30},
        {AT_CMD_STR("AT+SDTMF"), 30},
        {AT_CMD_STR("AT+VTD"), 5},
        {AT_CMD_STR("AT+CGEQREQ"), 5},
        {AT_CMD_STR("AT+CGEREP"), 5},
        {AT_CMD_STR("AT+COPS=1"), 90},
        {AT_CMD_STR("AT"), 50},  // default 50s timeout
    };

int s_fdModemBlockRead;
int s_fdModemBlockWrite;
ModemState s_modemState = MODEM_OFFLINE;
extern int s_fdReaderLoopWakeupWrite[SIM_COUNT];
extern pthread_mutex_t s_radioStateMutex[SIM_COUNT];
extern pthread_cond_t s_radioStateCond[SIM_COUNT];
extern ReaderThread s_readerThread[SIM_COUNT];

static int doHandShake();
static int openModemDev(char *path);
static int sendHandshakeCmd(int stty_fd, char *atCmd, char *path);

void *detectModemState() {
    int ret = -1;
    int num = 0;
    int filedes[2];
    int nfds = 0;
    int fdModemd = -1;
    fd_set rfds, readFds;
    char buf[ARRAY_SIZE * 5] = {0};
    const char socketName[ARRAY_SIZE] = "modemd";

    s_ATTableSize = NUM_ELEMS(s_ATTimeoutTable);

    if (pipe(filedes) < 0) {
        RLOGE("Error in pipe() errno:%d", errno);
    }
    s_fdModemBlockRead = filedes[0];
    s_fdModemBlockWrite = filedes[1];

    if (fcntl(s_fdModemBlockRead, F_SETFL, O_NONBLOCK) < 0) {
        RLOGE("fcntl on s_fdModemBlockRead failed");
    }

    FD_SET(s_fdModemBlockRead, &readFds);
    if (s_fdModemBlockRead >= nfds) {
        nfds = s_fdModemBlockRead + 1;
    }

RECONNECT:
    {
        int fd = socket_local_client(socketName,
                ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
        while (fd < 0) {
            usleep(500000);
            fd = socket_local_client(socketName,
                    ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
        }
        RLOGD("Connect to modemd socket success!");

        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            RLOGE("fcntl on fdModemd failed");
        }
        FD_SET(fd, &readFds);
        if (fd >= nfds) {
            nfds = fd + 1;
        }

        fdModemd = fd;
    }

    for (;;) {
        do {
            memcpy(&rfds, &readFds, sizeof(fd_set));
            num = select(nfds, &rfds, NULL, NULL, NULL);
        } while (num == -1 && errno == EINTR);

        if (num > 0) {
            if (FD_ISSET(s_fdModemBlockRead, &rfds)) {  // from at_send_command in RIL
                memset(buf, 0, sizeof(buf));
                read(s_fdModemBlockRead, buf, sizeof(buf));
                RLOGE("Modem blocked, info modemd");
                if (fdModemd >= 0) {
                    int ret = write(fdModemd, "Modem Blocked",
                            sizeof("Modem Blocked"));
                    RLOGE("Write %d bytes to client: %d modemd is blocked",
                          ret, fdModemd);
                } else {
                    RLOGE("Failed to connect to modemd, reconnect");
                    goto RECONNECT;
                }
            }
            if (FD_ISSET(fdModemd, &rfds)) {  // from modemd
                RIL_ModemStatusInfo responseMs;
                memset(buf, 0, sizeof(buf));
                int readNum = read(fdModemd, buf, sizeof(buf));
                RLOGE("%s", buf);
                if (readNum <= 0) {
                    close(fdModemd);
                    goto RECONNECT;
                }
                /* RIL don't stop when cm4 assert, which notifies "P-ARM Modem Assert" */
                if (strstr(buf, "Modem Blocked") ||
                    (strstr(buf, "Modem Assert") && !strstr(buf, "P-ARM Modem Assert"))) {
                    /* set modem assert for RIL to unlock pin */
                    property_set(MODEM_ASSERT_PROP, "1,1");
                    s_modemState = MODEM_OFFLINE;
                    RLOGE("Modem Assert or Blocked, Info readerLoop to get out of select");
                    write(s_fdReaderLoopWakeupWrite[RIL_SOCKET_1], " ", 1);

#if (SIM_COUNT >= 2)
                    write(s_fdReaderLoopWakeupWrite[RIL_SOCKET_2], " ", 1);
#endif
                    responseMs.modemState = strstr(buf, "Modem Blocked") ?
                                                   MODEM_STATUS_BLOCKED : MODEM_STATUS_ASSERT;
                } else if (strstr(buf, "Modem Reset")) {
                    /* set modem assert for RIL to unlock pin */
                    property_set(MODEM_ASSERT_PROP, "1,1");
                    if (s_readerThread[RIL_SOCKET_1].readerClosed == 0) {
                        s_modemState = MODEM_OFFLINE;
                        RLOGE("Modem Reset, Info readerLoop to get out of select");
                        write(s_fdReaderLoopWakeupWrite[RIL_SOCKET_1], " ", 1);
                    }
#if (SIM_COUNT >= 2)
                    if (s_readerThread[RIL_SOCKET_2].readerClosed == 0) {
                        write(s_fdReaderLoopWakeupWrite[RIL_SOCKET_2], " ", 1);
                    }
#endif
                    responseMs.modemState = MODEM_STATUS_RESET;
                } else if (strstr(buf, "Modem Alive") ||
                        strstr(buf, "Modem State: Alive")) {
                    if (s_modemState == MODEM_ALIVE) {
                        continue;
                    }

                    RLOGD("Modem Alive, do handshake first");
                    ret = doHandShake();
                    if (ret < 0) {
                        RLOGE("Failed to handshake with modem");
                        s_modemState = MODEM_OFFLINE;
                        continue;
                    }

                    for (int simCount = 0; simCount < SIM_COUNT; simCount++) {
                        pthread_mutex_lock(&s_radioStateMutex[simCount]);
                        s_modemState = MODEM_ALIVE;
                        pthread_cond_broadcast(&s_radioStateCond[simCount]);
                        pthread_mutex_unlock(&s_radioStateMutex[simCount]);
                    }
                    responseMs.modemState = MODEM_STATUS_ALIVE;
                } else {
                    RLOGE("unknown modem state");
                    continue;
                }
                responseMs.assertInfo = buf;
                RIL_SOCKET_ID socket_id = RIL_SOCKET_1;
                RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_MODEM_STATE_CHANGED,
                                          &responseMs, sizeof(RIL_ModemStatusInfo), socket_id);
            }
        }
    }
    return NULL;
}

/* open specified path */
static int openModemDev(char *path) {
    int fd = -1;
    int retryTimes = 10;
    do {
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            RLOGE("Failed to open %s err: %s!\n", path, strerror(errno));
            if ((errno == EINTR || errno == EAGAIN) && retryTimes > 0) {
                retryTimes--;
            } else {
                return -1;
            }
        }
    } while (fd < 0 && retryTimes >= 0);
    return fd;
}

/* send specified AT string */
static int sendHandshakeCmd(int stty_fd, char *atCmd, char *path) {
    int ret = -1, err = -1;
    int count = 0, length = 0;
    char buffer[ARRAY_SIZE] = {0};
    fd_set rfds;
    struct timeval timeout;

    if (atCmd == NULL) {
        RLOGE("AT command is NULL");
        ret = -1;
        goto exit;
    }

    RLOGD("write %s to %s", atCmd, path);
    length = strlen(atCmd);
    for (;;) {
        err = write(stty_fd, atCmd, length);
        if (err != length) {
            RLOGE("write error length = %d  ret = %d\n", length, ret);
            ret = -1;
            goto exit;
        }

WAIT_RESPOSE:
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        FD_ZERO(&rfds);
        FD_SET(stty_fd, &rfds);

        err = select(stty_fd + 1, &rfds, NULL, NULL, &timeout);
        if (err < 0) {
            RLOGE("select error: %s", strerror(errno));
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                ret = -1;
                goto exit;
            }
        } else if (err == 0) {
            RLOGE("select timeout");
            continue;
        } else {
            memset(buffer, 0, sizeof(buffer));
            count = read(stty_fd, buffer, sizeof(buffer) - 1);
            if (count <= 0) {
                RLOGE("read %d return %d, error: %s", stty_fd, count,
                            strerror(errno));
                continue;
            }
            RLOGD("read response %s", buffer);
            if (strstr(buffer, "OK")) {
                ret = 0;
                break;
            } else if (strstr(buffer, "ERROR")) {
                RLOGE("wrong modem state, exit!");
                ret = -1;
                goto exit;
            } else {
                // go on wait
                RLOGD("do nothing, go on wait!");
                goto WAIT_RESPOSE;
            }
        }
    }

exit:
    return ret;
}

static int doHandShake() {
    int fd = -1;
    int ret = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    char path[ARRAY_SIZE] = {0};
    char modemDev[PROPERTY_VALUE_MAX] = {0};

    property_get(MODEM_TTY_PROP, modemDev, "/dev/sdiomux");
    snprintf(path, sizeof(path), "%s1", modemDev);
    RLOGD("open stty dev: %s", path);
    fd = openModemDev(path);
    if (fd < 0) {
        ret = -1;
        RLOGE("Failed to open %s", path);
        goto exit;
    }

#if (SIM_COUNT == 2)
    snprintf(cmd, sizeof(cmd), "AT+SMMSWAP=0\r");
#elif (SIM_COUNT == 3)
    snprintf(cmd, sizeof(cmd), "AT+SMMSWAP=1\r");
#else
    snprintf(cmd, sizeof(cmd), "AT\r");
#endif

    if (sendHandshakeCmd(fd, cmd, path) != 0) {
        ret = -2;
        RLOGE("Failed to write %s to %s", cmd, path);
        goto exit;
    }

    snprintf(cmd, sizeof(cmd), "AT+CMUX=0\r");
    if (sendHandshakeCmd(fd, cmd, path) != 0) {
        ret = -2;
        RLOGE("Failed to write %s to %s", cmd, path);
        goto exit;
    }

exit:
    if (fd != -1) {
        close(fd);
    }
    return ret;
}

/**
 * Returns 1 if found, 0 otherwise. needle must be null-terminated.
 * strstr might not work because WebBox sends garbage before the first OKread
 */
int findInBuf(char *buf, int len, char *needle) {
    int i;
    int needleMatchedPos = 0;

    if (buf == NULL) {
        return 0;
    }

    if (needle[0] == '\0') {
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (needle[needleMatchedPos] == buf[i]) {
            needleMatchedPos++;
            if (needle[needleMatchedPos] == '\0') {
                // Entire needle was found
                return 1;
            }
        } else {
            needleMatchedPos = 0;
        }
    }
    return 0;
}

/* add an intermediate response to sp_response */
void reWriteIntermediate(ATResponse *sp_response, char *newLine) {
    ATLine *p_new;

    p_new = (ATLine *)malloc(sizeof(ATLine));

    p_new->line = strdup(newLine);

    /* note: this adds to the head of the list, so the list
       will be in reverse order of lines received. the order is flipped
       again before passing on to the command issuer */
    p_new->p_next = sp_response->p_intermediates;
    sp_response->p_intermediates = p_new;
}

/**
 * The line reader places the intermediate responses in reverse order
 * here we flip them back
 */
void reverseNewIntermediates(ATResponse *sp_response) {
    ATLine *pcur, *pnext;

    pcur = sp_response->p_intermediates;
    sp_response->p_intermediates = NULL;

    while (pcur != NULL) {
        pnext = pcur->p_next;
        pcur->p_next = sp_response->p_intermediates;
        sp_response->p_intermediates = pcur;
        pcur = pnext;
    }
}

int getATResponseType(char *str) {
    int rspType = AT_RSP_TYPE_MID;
    if (strStartsWith(str, "CONNECT")) {
        rspType = AT_RSP_TYPE_CONNECT;
    } else if (isFinalResponseError(str)) {
        rspType = AT_RSP_TYPE_ERROR;
    } else if (isFinalResponseSuccess(str)) {
        rspType = AT_RSP_TYPE_OK;
    }
    return rspType;
}
