/*
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
#define LOG_TAG "RIL-AT"

#include "atchannel.h"
#include "at_tok.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <utils/Log.h>
#include "misc.h"
#include "channel_controller.h"

#define LOG_NDEBUG              1
#define HANDSHAKE_RETRY_COUNT   8
#define HANDSHAKE_TIMEOUT_MSEC  250
#define NUM_ELEMS(x)            (sizeof(x) / sizeof(x[0]))

int s_fdReaderLoopWakeupRead[SIM_COUNT];
int s_fdReaderLoopWakeupWrite[SIM_COUNT];
int s_atTimeoutCount[MAX_AT_CHANNELS];
struct ATChannels s_ATChannel[MAX_AT_CHANNELS];
static pthread_mutex_t s_ATChannelMutex[MAX_AT_CHANNELS];
static pthread_cond_t s_ATChannelCond[MAX_AT_CHANNELS];

//used for checking that CP is mode changing
bool s_CModChgState[SIM_COUNT] = {false};
pthread_mutex_t s_CModChgMutex[MAX_AT_CHANNELS];
pthread_cond_t s_CModChgCond[MAX_AT_CHANNELS];

extern PDP_INFO pdp_info[MAX_PDP_NUM];
extern int s_psOpened[SIM_COUNT];

ReaderThread s_readerThread[SIM_COUNT];

#if AT_DEBUG
void  AT_DUMP(const char *prefix, const char *buff, int len) {
    if (len < 0) {
        len = strlen(buff);
    }
    RLOGD("%.*s", len, buff);
}
#endif

//static void (*s_onTimeout)(RIL_SOCKET_ID socket_id) = NULL;
static void (*s_onReaderClosed)(RIL_SOCKET_ID socket_id) = NULL;

static void onReaderClosed(RIL_SOCKET_ID socket_id);
static int writeCtrlZ(struct ATChannels *ATch, const char *s);
static int writeline(struct ATChannels *ATch, const char *s);

#define NS_PER_S 1000000000

static void setTimespecRelative(struct timespec *p_ts, long long msec) {
    struct timespec tv;

    clock_gettime(CLOCK_MONOTONIC, &tv);

    p_ts->tv_sec = tv.tv_sec + (msec / 1000);
    p_ts->tv_nsec = tv.tv_nsec + (msec % 1000) * 1000L * 1000L;
    /* assuming tv.tv_usec < 10^6 */
    if (p_ts->tv_nsec >= NS_PER_S) {
        p_ts->tv_sec++;
        p_ts->tv_nsec -= NS_PER_S;
    }
}

#if 0  // unused function
static void sleepMsec(long long msec) {
    struct timespec ts;
    int err;

    ts.tv_sec = (msec / 1000);
    ts.tv_nsec = (msec % 1000) * 1000 * 1000;

    do {
        err = nanosleep(&ts, &ts);
    } while (err < 0 && errno == EINTR);
}
#endif

/* add an intermediate response to sp_response */
static void addIntermediate(struct ATChannels *ATch) {
    ATLine *p_new;

    p_new = (ATLine *)malloc(sizeof(ATLine));

    p_new->line = strdup(ATch->line);

    /* note: this adds to the head of the list, so the list
       will be in reverse order of lines received. the order is flipped
       again before passing on to the command issuer */
    p_new->p_next = ATch->sp_response->p_intermediates;
    ATch->sp_response->p_intermediates = p_new;
}

/**
 * returns 1 if line is a final response indicating error
 * See 27.007 annex B
 * WARNING: NO CARRIER and others are sometimes unsolicited
 */
static const char *s_finalResponsesError[] = {
    "ERROR",
    "+CMS ERROR:",
    "+CME ERROR:",
    "NO CARRIER", /* sometimes! */
    "NO ANSWER",
    "NO DIALTONE",
};

int isFinalResponseError(const char *line) {
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_finalResponsesError); i++) {
        if (strStartsWith(line, s_finalResponsesError[i])) {
            return 1;
        }
    }

    return 0;
}

/**
 * returns 1 if line is a final response indicating success
 * See 27.007 annex B
 * WARNING: NO CARRIER and others are sometimes unsolicited
 */
static const char *s_finalResponsesSuccess[] = {
    "OK",
    "CONNECT"  /* some stacks start up data on another channel */
};

int isFinalResponseSuccess(const char *line) {
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_finalResponsesSuccess); i++) {
        if (strStartsWith(line, s_finalResponsesSuccess[i])) {
            return 1;
        }
    }

    return 0;
}

#if 0  // unused function
/**
 * returns 1 if line is a final response, either  error or success
 * See 27.007 annex B
 * WARNING: NO CARRIER and others are sometimes unsolicited
 */
static int isFinalResponse(const char *line) {
    return isFinalResponseSuccess(line) || isFinalResponseError(line);
}
#endif

/**
 * returns 1 if line is the first line in (what will be) a two-line
 * SMS unsolicited response
 */
static const char *s_smsUnsoliciteds[] = {
    "+CMT:",
    "+CDS:",
    "+CBM:",
    "+CMGR:"
};

static int isSMSUnsolicited(const char *line) {
    size_t i;

    for (i = 0; i < NUM_ELEMS(s_smsUnsoliciteds); i++) {
        if (strStartsWith(line, s_smsUnsoliciteds[i])) {
            return 1;
        }
    }

    return 0;
}

static void at_backup_free(struct ATChannels *ATch) {
    if (ATch != NULL && ATch->s_ATBackup != NULL) {
        free(ATch->s_ATBackup);
        ATch->s_ATBackup = NULL;
        ATch->s_ATBackupLenCur = 0;
    }
}

/* assumes s_commandmutex is held */
static void handleFinalResponse(struct ATChannels *ATch) {
    ATch->sp_response->finalResponse = strdup(ATch->line);
    pthread_cond_signal(&s_ATChannelCond[ATch->channelID]);
}

static void handleUnsolicited(struct ATChannels *ATch) {
    char *line = ATch->line;
    int channelID = ATch->channelID;
    if (ATch->s_unsolHandler != NULL) {
        ATch->s_unsolHandler(channelID, line, NULL);
    }
}

static void processLine(struct ATChannels *ATch) {
    pthread_mutex_lock(&s_ATChannelMutex[ATch->channelID]);

    if (ATch->sp_response == NULL) {
        /* no command pending */
        handleUnsolicited(ATch);
    } else if (isFinalResponseSuccess(ATch->line)) {
        ATch->sp_response->success = 1;
        handleFinalResponse(ATch);
    } else if (isFinalResponseError(ATch->line)) {
        ATch->sp_response->success = 0;
        handleFinalResponse(ATch);
    } else if (ATch->s_smsPDU != NULL && 0 == strcmp(ATch->line, "> ")) {
        /**
         * See eg. TS 27.005 4.3
         * Commands like AT+CMGS have a "> " prompt
         */
        writeCtrlZ(ATch, ATch->s_smsPDU);
        ATch->s_smsPDU = NULL;
    } else switch (ATch->s_type) {
        case NO_RESULT:
            handleUnsolicited(ATch);
            break;
        case NUMERIC:
            if (ATch->sp_response->p_intermediates == NULL &&
                isdigit(ATch->line[0])) {
                addIntermediate(ATch);
            } else {
                /* either we already have an intermediate response or
                   the line doesn't begin with a digit */
                handleUnsolicited(ATch);
            }
            break;
        case SINGLELINE:
            if (ATch->sp_response->p_intermediates == NULL &&
                strStartsWith(ATch->line, ATch->s_responsePrefix)) {
                addIntermediate(ATch);
            } else {
                /* we already have an intermediate response */
                handleUnsolicited(ATch);
            }
            break;
        case MULTILINE:
            if (strStartsWith (ATch->line, ATch->s_responsePrefix) ||
                strchr(ATch->line, ':') == NULL) {
                addIntermediate(ATch);
            } else {
                handleUnsolicited(ATch);
            }
            break;

        default:  /* this should never be reached */
            RLOGE("Unsupported AT command type %d\n", ATch->s_type);
            handleUnsolicited(ATch);
            break;
    }

    pthread_mutex_unlock(&s_ATChannelMutex[ATch->channelID]);
}

/**
 * Returns a pointer to the end of the next line
 * special-cases the "> " SMS prompt
 *
 * returns NULL if there is no complete line
 */
static char *findNextEOL(char *cur) {
    if (cur[0] == '>' && cur[1] == ' ' && cur[2] == '\0') {
        /* SMS prompt character...not \r terminated */
        return cur+2;
    }

    // Find next newline
    while (*cur != '\0' && *cur != '\r' && *cur != '\n') cur++;

    return *cur == '\0' ? NULL : cur;
}


/**
 * Reads a line from the AT channel, returns NULL on timeout.
 * Assumes it has exclusive read access to the FD
 *
 * This line is valid only until the next call to readline
 *
 * This function exists because as of writing, android libc does not
 * have buffered stdio.
 */
static const char *readline(struct ATChannels *ATch) {
    ssize_t count;
    char *p_read = NULL;
    char *p_eol = NULL;
    /* Add for 321528 @{ */
    ssize_t err_count = 0;
    /* @} */

    /* this is a little odd. I use *s_ATBufferCur == 0 to
     * mean "buffer consumed completely". If it points to a character, than
     * the buffer continues until a \0
     */
    if (*ATch->s_ATBufferCur == '\0') {
        /* empty buffer */
        ATch->s_ATBufferCur = ATch->s_ATBuffer;
        *ATch->s_ATBufferCur = '\0';
        p_read = ATch->s_ATBuffer;
    } else {   /* *s_ATBufferCur != '\0' */
        /* there's data in the buffer from the last read */

        // skip over leading newlines
        while (*ATch->s_ATBufferCur == '\r' || *ATch->s_ATBufferCur == '\n') {
            ATch->s_ATBufferCur++;
        }

        p_eol = findNextEOL(ATch->s_ATBufferCur);

        if (p_eol == NULL) {
            /* a partial line. move it up and prepare to read more */
            size_t len;

            len = strlen(ATch->s_ATBufferCur);

            memmove(ATch->s_ATBuffer, ATch->s_ATBufferCur, len + 1);
            p_read = ATch->s_ATBuffer + len;
            ATch->s_ATBufferCur = ATch->s_ATBuffer;
        }
        /* Otherwise, (p_eol !- NULL) there is a complete line  */
        /* that will be returned the while () loop below        */
    }

    while (p_eol == NULL) {
        if (0 == MAX_AT_RESPONSE - (p_read - ATch->s_ATBuffer)) {
            RLOGE("Input line exceeded buffer, realloc memory to store AT line");

            ATch->s_ATBackup = (char *)realloc(ATch->s_ATBackup,
                    ATch->s_ATBackupLenCur + strlen(ATch->s_ATBufferCur));
            memcpy(ATch->s_ATBackup + ATch->s_ATBackupLenCur, ATch->s_ATBufferCur,
                    strlen(ATch->s_ATBufferCur));
            ATch->s_ATBackupLenCur += strlen(ATch->s_ATBufferCur);

            /* ditch buffer and start over again */
            ATch->s_ATBufferCur = ATch->s_ATBuffer;
            *ATch->s_ATBufferCur = '\0';
            p_read = ATch->s_ATBuffer;
        }

        do {
            count = read(ATch->s_fd, p_read,
                         MAX_AT_RESPONSE - (p_read - ATch->s_ATBuffer));
        } while (count < 0 && errno == EINTR);

        if (count > 0) {
            AT_DUMP("<< ", p_read, count);

            p_read[count] = '\0';

            if (*ATch->s_ATBufferCur == '\r' && ATch->s_ATBackup != NULL) {
                p_eol = ATch->s_ATBufferCur;
            } else {
                // skip over leading newlines
                while (*ATch->s_ATBufferCur == '\r' ||
                        *ATch->s_ATBufferCur == '\n') {
                    ATch->s_ATBufferCur++;
                }

                p_eol = findNextEOL(ATch->s_ATBufferCur);
            }
            p_read += count;
            /* Add for 321528 @{ */
            err_count = 0;
            /* @} */
        } else if (count <= 0) {
            /* read error encountered or EOF reached */
            if (count == 0) {
                // RLOGD("atchannel: EOF reached");
                /* Add for 321528 @{ */
                err_count++;
                if (err_count > 10) {
                    RLOGD("atchannel: EOF reached. Sleep 10s");
                    sleep(10);
                } else {
                    RLOGD("atchannel: EOF reached. err_count = %zd", err_count);
                }
                /* @} */
            } else {
                // RLOGD("atchannel: read error %s", strerror(errno));
            }
            return NULL;
        }
    }

    /* a full line in the buffer. Place a \0 over the \r and return */
    *p_eol = '\0';
    if (ATch->s_ATBackup != NULL) {
        ATch->s_ATBackup = (char *)realloc(ATch->s_ATBackup,
                ATch->s_ATBackupLenCur + strlen(ATch->s_ATBufferCur) + 1);
        memcpy(ATch->s_ATBackup + ATch->s_ATBackupLenCur, ATch->s_ATBufferCur,
                strlen(ATch->s_ATBufferCur));
        ATch->s_ATBackupLenCur += strlen(ATch->s_ATBufferCur);
        *(ATch->s_ATBackup + ATch->s_ATBackupLenCur) = '\0';

        ATch->line = ATch->s_ATBackup;
    } else {
        ATch->line = ATch->s_ATBufferCur;
    }
    ATch->s_ATBufferCur = p_eol + 1;  /* this will always be <= p_read,
                                         and there will be a \0 at *p_read */
    if (!ATch->nolog) {
        if (!ATch->name) {
            RLOGD("AT< %s\n", ATch->line);
        } else {
            RLOGD("%s: AT< %s\n", ATch->name, ATch->line);
        }
    }

    return ATch->line;
}

static void onReaderClosed(RIL_SOCKET_ID socket_id) {
    if (s_onReaderClosed != NULL &&
        s_readerThread[socket_id].readerClosed == 0) {
        int channel;
        int firstChannel, lastChannel;

#if defined (ANDROID_MULTI_SIM)
        firstChannel = socket_id * AT_CHANNEL_OFFSET;
        lastChannel = (socket_id + 1) * AT_CHANNEL_OFFSET;
#else
        firstChannel = AT_URC;
        lastChannel = MAX_AT_CHANNELS;
#endif

        s_readerThread[socket_id].readerClosed = 1;

        for (channel = firstChannel; channel < lastChannel; channel++) {
            pthread_mutex_lock(&s_ATChannelMutex[channel]);
            pthread_cond_signal(&s_ATChannelCond[channel]);
            pthread_mutex_unlock(&s_ATChannelMutex[channel]);
        }

        usleep(100 * 1000);
        s_onReaderClosed(socket_id);
    }

    close(s_fdReaderLoopWakeupWrite[socket_id]);
    close(s_fdReaderLoopWakeupRead[socket_id]);
    s_fdReaderLoopWakeupWrite[socket_id] = -1;
    s_fdReaderLoopWakeupRead[socket_id] = -1;

}

static void *readerLoop(void *arg) {
    int channelID = 0;
    int ret;
    fd_set rfds;
    int firstChannel, lastChannel;
    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)arg);

    int open_ATchs = s_readerThread[socket_id].openedATChs;

#if defined (ANDROID_MULTI_SIM)
    firstChannel = socket_id * AT_CHANNEL_OFFSET;
    lastChannel = (socket_id + 1) * AT_CHANNEL_OFFSET;
#else
    firstChannel = AT_URC;
    lastChannel = MAX_AT_CHANNELS;
#endif

    for (;;) {
        do {
            rfds = s_readerThread[socket_id].readerSet;
            ret = select(open_ATchs + 1, &rfds, NULL, NULL, NULL);
        } while (ret == -1 && errno == EINTR);
        if (ret > 0) {
            for (channelID = firstChannel; channelID < lastChannel;
                  channelID++) {
                if (s_ATChannel[channelID].s_fd != -1 &&
                    FD_ISSET(s_ATChannel[channelID].s_fd, &rfds)) {
                    struct ATChannels *ATch = &s_ATChannel[channelID];
                    while (1) {
                        if (!readline(ATch)) {
                            break;
                        }
                        if (ATch->line == NULL) {
                            break;
                        }
                        if (isSMSUnsolicited(ATch->line)) {
                            char *line1;
                            const char *line2;

                            // The scope of string returned by 'readline()'
                            // is valid only till next call to 'readline()'
                            // hence making a copy of line
                            // before calling readline again.
                            line1 = strdup(ATch->line);
                            at_backup_free(ATch);
                            fcntl(ATch->s_fd, F_SETFL, O_RDWR);
                            line2 = readline(ATch);
                            fcntl(ATch->s_fd, F_SETFL, O_RDWR | O_NONBLOCK);

                            if (line2 == NULL) {
                                free(line1);
                                break;
                            }
                            if (ATch->s_unsolHandler != NULL)
                                ATch->s_unsolHandler(channelID, line1, line2);
                            free(line1);
                        } else {
                            processLine(ATch);
                        }
                        at_backup_free(ATch);
                    }
                 }
            }

            if (FD_ISSET(s_fdReaderLoopWakeupRead[socket_id], &rfds)) {
                char buf[ARRAY_SIZE] = {0};
                read(s_fdReaderLoopWakeupRead[socket_id], buf, sizeof(buf));
                RLOGE("Modem Abnormal, stop sim%d readerLoop", socket_id);
                break;
            }
        }
    }

    onReaderClosed(socket_id);

    return NULL;
}

/**
 * Sends string s to the radio with a \r appended.
 * Returns AT_ERROR_* on error, 0 on success
 *
 * This function exists because as of writing, android libc does not
 * have buffered stdio.
 */
static int writeline(struct ATChannels *ATch, const char *s) {
    size_t cur = 0;
    size_t len = strlen(s);
    ssize_t written;
    RIL_SOCKET_ID socket_id = getSocketIdByChannelID(ATch->channelID);

    if (ATch->s_fd < 0 || s_readerThread[socket_id].readerClosed > 0) {
        return AT_ERROR_CHANNEL_CLOSED;
    }

    if (!ATch->nolog) {
        if (ATch->name) {
            RLOGD("%s: AT> %s\n", ATch->name, s);
        } else {
            RLOGD("AT> %s\n", s);
        }
    }

    AT_DUMP(">> ", s, strlen(s));

    /* the main string */
    while (cur < len) {
        do {
            written = write(ATch->s_fd, s + cur, len - cur);
        } while (written < 0 && errno == EINTR);

        /* UNISOC Bug1235268,send modem block event when ETIME happened */

        if (errno == ETIME) {
            return AT_ERROR_MODEM_BLOCKED;
        }

        if (written < 0) {
            return AT_ERROR_GENERIC;
        }

        cur += written;
    }

    /* the \r */
    do {
        written = write(ATch->s_fd, "\r", 1);
    } while ((written < 0 && errno == EINTR) || (written == 0));

    /* UNISOC Bug1235268,send modem block event when ETIME happened */

    if (errno == ETIME) {
        return AT_ERROR_MODEM_BLOCKED;
    }

    if (written < 0) {
        return AT_ERROR_GENERIC;
    }

    return 0;
}

static int writeCtrlZ(struct ATChannels *ATch, const char *s) {
    size_t cur = 0;
    size_t len = strlen(s);
    ssize_t written;
    RIL_SOCKET_ID socket_id = getSocketIdByChannelID(ATch->channelID);

    if (ATch->s_fd < 0 || s_readerThread[socket_id].readerClosed > 0) {
        return AT_ERROR_CHANNEL_CLOSED;
    }

    RLOGD("AT> %s^Z\n", s);

    AT_DUMP(">* ", s, strlen(s));

    /* the main string */
    while (cur < len) {
        do {
            written = write(ATch->s_fd, s + cur, len - cur);
        } while (written < 0 && errno == EINTR);

        if (written < 0) {
            return AT_ERROR_GENERIC;
        }

        cur += written;
    }

    /* the ^Z  */

    do {
        written = write(ATch->s_fd, "\032", 1);
    } while ((written < 0 && errno == EINTR) || (written == 0));

    if (written < 0) {
        return AT_ERROR_GENERIC;
    }

    return 0;
}

static void clearPendingCommand(struct ATChannels *ATch) {
    ATResponse *sp_response = ATch->sp_response;

    if (sp_response != NULL) {
        at_response_free(sp_response);
    }

    ATch->sp_response = NULL;
    ATch->s_responsePrefix = NULL;
    ATch->s_smsPDU = NULL;
}

void init_channels(RIL_SOCKET_ID socket_id) {
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
        s_ATChannel[channel].s_fd = -1;
    }

    FD_ZERO(&(s_readerThread[socket_id].readerSet));

    s_readerThread[socket_id].openedATChs = -1;

    /* for notify readerLoop to get out of select, process modem assert */
    int filedes[2];
    if (pipe(filedes) < 0) {
        RLOGE("Error in pipe() errno:%d", errno);
    }
    s_fdReaderLoopWakeupRead[socket_id] = filedes[0];
    s_fdReaderLoopWakeupWrite[socket_id] = filedes[1];

    if (fcntl(s_fdReaderLoopWakeupRead[socket_id], F_SETFL, O_NONBLOCK) < 0) {
        RLOGE("fcntl on s_fdReaderLoopWakeupRead file failed");
    }

    int *open_ATchs = &s_readerThread[socket_id].openedATChs;

    FD_SET(s_fdReaderLoopWakeupRead[socket_id], &s_readerThread[socket_id].readerSet);
    *open_ATchs = s_fdReaderLoopWakeupRead[socket_id] > (*open_ATchs) ?
            s_fdReaderLoopWakeupRead[socket_id] : (*open_ATchs);
}

/**
 * Starts AT handler on stream "fd'
 * returns 0 on success, -1 on error
 */
struct ATChannels *at_open(int fd, int channelID, char *name,
                             ATUnsolHandler h) {
    struct ATChannels *ATch;
    int channelNums;
    pthread_condattr_t attr;

#if defined (ANDROID_MULTI_SIM)
    channelNums = MAX_AT_CHANNELS;
#else
    channelNums = MAX_AT_CHANNELS;
#endif

    ATch = &s_ATChannel[channelID];

    if (channelID == channelNums) {
        RLOGE("channelID exceeded MAX_CHANNELS in at_open\n");
        return NULL;
    }
    if (name) {
        ATch->name = strdup(name);
    }

    ATch->s_fd = fd;
    ATch->s_responsePrefix = NULL;
    ATch->channelID = channelID;
    ATch->s_smsPDU = NULL;
    ATch->s_unsolHandler = h;
    ATch->sp_response = NULL;
    ATch->nolog = 1;
    memset(ATch->s_ATBuffer, 0, sizeof(ATch->s_ATBuffer));
    ATch->s_ATBufferCur = ATch->s_ATBuffer;

    RIL_SOCKET_ID socket_id = getSocketIdByChannelID(channelID);

    int *open_ATchs = &s_readerThread[socket_id].openedATChs;

    FD_SET(fd, &s_readerThread[socket_id].readerSet);
    *open_ATchs = fd > (*open_ATchs) ? fd : (*open_ATchs);
    pthread_mutex_init(&s_ATChannelMutex[channelID], NULL);

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&s_ATChannelCond[channelID], &attr);

    pthread_mutex_init(&s_CModChgMutex[channelID], NULL);
    pthread_cond_init(&s_CModChgCond[channelID], NULL);

    return ATch;
}

int start_reader(RIL_SOCKET_ID socket_id) {
    int ret;
    pthread_attr_t attr;
    extern const RIL_SOCKET_ID s_socketId[];

    s_readerThread[socket_id].readerClosed = 0;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&s_readerThread[socket_id].readerTid, &attr,
            readerLoop, (void*)&s_socketId[socket_id]);

    if (ret < 0) {
        perror("pthread_create");
        return -1;
    }
    return 0;
}

/* FIXME is it ok to call this from the reader and the command thread? */
void at_close(struct ATChannels *ATch) {
    RIL_SOCKET_ID socket_id = getSocketIdByChannelID(ATch->channelID);

    if (ATch->s_fd >= 0) {
        close(ATch->s_fd);
    }

    FD_CLR(ATch->s_fd, &s_readerThread[socket_id].readerSet);
    if (ATch->name) {
        free(ATch->name);
    }

    ATch->s_fd = -1;
}

void stop_reader(RIL_SOCKET_ID socket_id) {
    int channel;
    int firstChannel, lastChannel;

#if defined (ANDROID_MULTI_SIM)
    firstChannel = socket_id * AT_CHANNEL_OFFSET;
    lastChannel = (socket_id + 1) * AT_CHANNEL_OFFSET;
#else
    firstChannel = AT_URC;
    lastChannel = MAX_AT_CHANNELS;
#endif

    s_readerThread[socket_id].readerClosed = 1;

    for (channel = firstChannel; channel < lastChannel; channel++) {
        pthread_mutex_lock(&s_ATChannelMutex[channel]);
        pthread_cond_signal(&s_ATChannelCond[channel]);
        pthread_mutex_unlock(&s_ATChannelMutex[channel]);
    }

    /* the reader thread should eventually die */
}


ATResponse * at_response_new() {
    return (ATResponse *)calloc(1, sizeof(ATResponse));
}

void at_response_free(ATResponse *p_response) {
    ATLine *p_line;

    if (p_response == NULL) return;

    p_line = p_response->p_intermediates;

    while (p_line != NULL) {
        ATLine *p_toFree;

        p_toFree = p_line;
        p_line = p_line->p_next;

        free(p_toFree->line);
        free(p_toFree);
    }

    free(p_response->finalResponse);
    free(p_response);
}

/**
 * The line reader places the intermediate responses in reverse order
 * here we flip them back
 */
static void reverseIntermediates(struct ATChannels *ATch) {
    ATLine *pcur, *pnext;

    pcur = ATch->sp_response->p_intermediates;
    ATch->sp_response->p_intermediates = NULL;

    while (pcur != NULL) {
        pnext = pcur->p_next;
        pcur->p_next = ATch->sp_response->p_intermediates;
        ATch->sp_response->p_intermediates = pcur;
        pcur = pnext;
    }
}

/**
 * Internal send_command implementation
 * Doesn't lock or call the timeout callback
 *
 * timeoutMsec == 0 means infinite timeout
 */
static int at_send_command_full_nolock(struct ATChannels *ATch,
                    const char *command, ATCommandType type,
                    const char *responsePrefix, const char *smspdu,
                    long long timeoutMsec, ATResponse **pp_outResponse) {
    int err = 0;
    struct timespec ts;
    RIL_SOCKET_ID socket_id = getSocketIdByChannelID(ATch->channelID);

    if (ATch->sp_response != NULL) {
        err = AT_ERROR_COMMAND_PENDING;
        goto error;
    }

    ATch->s_type = type;
    ATch->s_responsePrefix = (char *)responsePrefix;
    ATch->s_smsPDU = (char *)smspdu;
    ATch->sp_response = at_response_new();
    if (ATch->sp_response == NULL) {
        err = AT_ERROR_GENERIC;
        goto error;
    }

    //used for checking that CP is mode changing
    while (s_CModChgState[socket_id]) {
        RLOGD("CModeChange starts and all AT need to wait it: channel is %d", ATch->channelID);
        pthread_mutex_lock(&s_CModChgMutex[ATch->channelID]);
        pthread_cond_wait(&s_CModChgCond[ATch->channelID], &s_CModChgMutex[ATch->channelID]);
        RLOGD("CModeChange finished and all AT need to send: channel is %d", ATch->channelID);
        pthread_mutex_unlock(&s_CModChgMutex[ATch->channelID]);
    }

    err = writeline(ATch, command);
    if (err < 0) {
        goto error;
    }

    if (timeoutMsec != 0) {
        setTimespecRelative(&ts, timeoutMsec);
    }

    while (ATch->sp_response->finalResponse == NULL &&
           s_readerThread[socket_id].readerClosed == 0) {
        if (timeoutMsec != 0) {
            err = pthread_cond_timedwait(&s_ATChannelCond[ATch->channelID],
                    &s_ATChannelMutex[ATch->channelID], &ts);
        } else {
            err = pthread_cond_wait(&s_ATChannelCond[ATch->channelID],
                    &s_ATChannelMutex[ATch->channelID]);
        }

        if (err == ETIMEDOUT) {
            err = AT_ERROR_TIMEOUT;
            goto error;
        }
    }

    if (s_readerThread[socket_id].readerClosed > 0) {
        err = AT_ERROR_CHANNEL_CLOSED;
        goto error;
    }

    if (pp_outResponse == NULL) {
        at_response_free(ATch->sp_response);
    } else {
        /* line reader stores intermediate responses in reverse order */
        reverseIntermediates(ATch);
        *pp_outResponse = ATch->sp_response;
    }

    ATch->sp_response = NULL;

    err = 0;

error:
    clearPendingCommand(ATch);
    return err;
}

/**
 * Internal send_command implementation
 *
 * timeoutMsec == 0 means infinite timeout
 */
static int at_send_command_full(struct ATChannels *ATch,
                                    const char *command, ATCommandType type,
                                    const char *responsePrefix,
                                    const char *smspdu,
                                    long long timeoutMsec,
                                    ATResponse **pp_outResponse) {
    int err, readerNum;

    for (readerNum = 0; readerNum < SIM_COUNT; readerNum++) {
        if (0 != pthread_equal(s_readerThread[readerNum].readerTid,
                pthread_self())) {
            /* cannot be called from reader thread */
            return AT_ERROR_INVALID_THREAD;
        }
    }

    pthread_mutex_lock(&s_ATChannelMutex[ATch->channelID]);

    err = at_send_command_full_nolock(ATch, command, type,
                    responsePrefix, smspdu,
                    timeoutMsec, pp_outResponse);

    pthread_mutex_unlock(&s_ATChannelMutex[ATch->channelID]);

    /* for google android, when one AT timeout, will stop readerLoop*/
//    if (err == AT_ERROR_TIMEOUT && s_onTimeout != NULL) {
//        RIL_SOCKET_ID socket_id = getSocketIdByChannelID(ATch->channelID);
//        s_onTimeout(socket_id);
//    }
    if (err == AT_ERROR_TIMEOUT) {
        s_atTimeoutCount[ATch->channelID] += 1;
        RLOGE("After %lld s, channel%d %s timeout, timeout AT number: %d",
                (timeoutMsec / 1000), ATch->channelID, command,
                s_atTimeoutCount[ATch->channelID]);
        if (s_atTimeoutCount[ATch->channelID] > BLOCKED_MAX_COUNT ||
                strcmp(command, "AT+SFUN=4") == 0) {
            s_atTimeoutCount[ATch->channelID] = 0;
            RLOGE("Info detectModemState to send Modem Blocked to modemd");
            write(s_fdModemBlockWrite, " ", 1);
        }
    } else if (err == AT_ERROR_MODEM_BLOCKED) {
        s_atTimeoutCount[ATch->channelID] = 0;
        RLOGE("Info detectModemState to send Modem Blocked to modemd");
        write(s_fdModemBlockWrite, " ", 1);
    } else {
        s_atTimeoutCount[ATch->channelID] = 0;
    }

    return err;
}

long long getATTimeoutMesc(const char *command) {
    int i;
    long long timeoutMesc = 50 * 1000;

    for (i = 0; i < s_ATTableSize; i++) {
        if (!strncasecmp(s_ATTimeoutTable[i].cmd, command,
                s_ATTimeoutTable[i].len)) {
            timeoutMesc = s_ATTimeoutTable[i].timeout * (long long)1000;
            break;
        }
    }

    return timeoutMesc;
}

/**
 * Issue a single normal AT command with no intermediate response expected
 *
 * "command" should not include \r
 * pp_outResponse can be NULL
 *
 * if non-NULL, the resulting ATResponse * must be eventually freed with
 * at_response_free
 */
int at_send_command(RIL_SOCKET_ID socket_id, const char *command,
                    ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);

    if (!strncasecmp(command, "AT+CFUN=0", sizeof("AT+CFUN=0"))||
        !strncasecmp(command, "AT+SFUN=5", sizeof("AT+SFUN=5"))) {
        int i;
        for (i = 0; i < MAX_PDP_NUM; i++) {
            pdp_info[i].state = PDP_STATE_IDLE;
        }
    } else if (!strncasecmp(command, "AT+SFUN=4", sizeof("AT+SFUN=4"))) {
        s_psOpened[socket_id] = 1;
    }

    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, NO_RESULT,
                               NULL, NULL, timeoutMesc, pp_outResponse);
    putChannel(channelID);

    return err;
}

int at_send_command_snvm(RIL_SOCKET_ID socket_id, const char *command,
                         const char *pdu, const char *responsePrefix,
                         ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);
    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, NO_RESULT,
                               responsePrefix, pdu, timeoutMesc, pp_outResponse);
    putChannel(channelID);

    return err;
}

int at_send_command_singleline(RIL_SOCKET_ID socket_id,
                               const char *command,
                               const char *responsePrefix,
                               ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);
    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, SINGLELINE,
                               responsePrefix, NULL, timeoutMesc, pp_outResponse);
    putChannel(channelID);

    if (err == 0 && pp_outResponse != NULL
        && (*pp_outResponse)->success > 0
        && (*pp_outResponse)->p_intermediates == NULL
    ) {
        /* successful command must have an intermediate response */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        return AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}

int at_send_command_numeric(RIL_SOCKET_ID socket_id, const char *command,
                            ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);

    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, NUMERIC, NULL,
                               NULL, timeoutMesc, pp_outResponse);
    putChannel(channelID);

    if (err == 0 && pp_outResponse != NULL && (*pp_outResponse)->success > 0 &&
       (*pp_outResponse)->p_intermediates == NULL) {
        /* successful command must have an intermediate response */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        return AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}


int at_send_command_sms(RIL_SOCKET_ID socket_id, const char *command,
                        const char *pdu, const char *responsePrefix,
                        ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);
    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, SINGLELINE,
                               responsePrefix, pdu, timeoutMesc, pp_outResponse);
    putChannel(channelID);

    if (err == 0 && pp_outResponse != NULL && (*pp_outResponse)->success > 0 &&
        (*pp_outResponse)->p_intermediates == NULL) {
        /* successful command must have an intermediate response */
        at_response_free(*pp_outResponse);
        *pp_outResponse = NULL;
        return AT_ERROR_INVALID_RESPONSE;
    }

    return err;
}

int at_send_command_multiline(RIL_SOCKET_ID socket_id,
                              const char *command,
                              const char *responsePrefix,
                              ATResponse **pp_outResponse) {
    if (socket_id < 0 || socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return AT_ERROR_INVALID_SOCKET_ID;
    }

    int err = AT_ERROR_GENERIC;
    long long timeoutMesc = 0;

    timeoutMesc = getATTimeoutMesc(command);

    if (!strncasecmp(command, "AT+CFUN=0", sizeof("AT+CFUN=0"))||
        !strncasecmp(command, "AT+SFUN=5", sizeof("AT+SFUN=5"))) {
        int i;
        for (i = 0; i < MAX_PDP_NUM; i++) {
            pdp_info[i].state = PDP_STATE_IDLE;
        }
    } else if (!strncasecmp(command, "AT+SFUN=4", sizeof("AT+SFUN=4"))) {
        s_psOpened[socket_id] = 1;
    }

    int channelID = getChannel(socket_id);
    err = at_send_command_full(s_ATChannels[channelID], command, MULTILINE,
                               responsePrefix, NULL, timeoutMesc, pp_outResponse);
    putChannel(channelID);
    return err;
}


/** This callback is invoked on the command thread */
//void at_set_on_timeout(void (*onTimeout)(RIL_SOCKET_ID socket_id)) {
//    s_onTimeout = onTimeout;
//}

/**
 *  This callback is invoked on the reader thread (like ATUnsolHandler)
 *  when the input stream closes before you call at_close
 *  (not when you call at_close())
 *  You should still call at_close()
 */

void at_set_on_reader_closed(void (*onClose)(RIL_SOCKET_ID socket_id)) {
    s_onReaderClosed = onClose;
}

/**
 * Periodically issue an AT command and wait for a response.
 * Used to ensure channel has start up and is active
 */
int at_handshake(struct ATChannels *ATch) {
    int i;
    int err = 0;
    int readerNum;
    for (readerNum = 0; readerNum < SIM_COUNT; readerNum++) {
        if (0 != pthread_equal(s_readerThread[readerNum].readerTid,
                 pthread_self())) {
            /* cannot be called from reader thread */
            return AT_ERROR_INVALID_THREAD;
        }
    }

    pthread_mutex_lock(&s_ATChannelMutex[ATch->channelID]);

    for (i = 0; i < HANDSHAKE_RETRY_COUNT; i++) {
        /* some stacks start with verbose off */
        err = at_send_command_full_nolock(ATch, "ATE0Q0V1", NO_RESULT, NULL,
                                          NULL, HANDSHAKE_TIMEOUT_MSEC, NULL);
        if (err == 0) {
            break;
        }
    }

    pthread_mutex_unlock(&s_ATChannelMutex[ATch->channelID]);

    return err;
}

/**
 * Returns error code from response
 * Assumes AT+CMEE=1 (numeric) mode
 */
AT_CME_Error at_get_cme_error(const ATResponse *p_response) {
    int ret;
    int err;
    char *p_cur;

    if (p_response->success > 0) {
        return CME_SUCCESS;
    }

    if (p_response->finalResponse == NULL ||
        !strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
        return CME_ERROR_NON_CME;
    }

    p_cur = p_response->finalResponse;
    err = at_tok_start(&p_cur);

    if (err < 0) {
        return CME_ERROR_NON_CME;
    }

    err = at_tok_nextint(&p_cur, &ret);

    if (err < 0) {
        return CME_ERROR_NON_CME;
    }

    return (AT_CME_Error)ret;
}
