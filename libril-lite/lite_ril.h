/**
 * lite_ril.h --- embms and mdt service marco definitions and
 *                structs / functions declaration
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef LITE_RIL_H_
#define LITE_RIL_H_

#define MAX_NAME_LENGTH                 32
#define MAX_BUFFER_BYTES                (8 * 1024)
#define MUX_NUM                         2

#define ERROR_GENERIC                  -1
#define ERROR_TIMEOUT                  -2
#define ERROR_CHANNEL_CLOSED           -3
#define ERROR_COMMAND_PENDING          -4
#define ERROR_INVALID_THREAD           -5
#define ARRAY_SIZE                      128
#define NUM_ELEMS(x)                    (sizeof(x) / sizeof(x[0]))
// EMBMS
#define MUX_EMBMS_NUM                   2
#define MUX_EMBMS_INDEX                 15

// MDT
#define MUX_MDT_NUM                     2
#define MUX_MDT_INDEX                   13

#define ETH_ALEN                        6

extern "C" {
#endif

int at_tok_start(char **p_cur);
int at_tok_nextint(char **p_cur, int *p_out);
int at_tok_nexthexint(char **p_cur, int *p_out);

int at_tok_nextbool(char **p_cur, char *p_out);
int at_tok_nextstr(char **p_cur, char **out);

int at_tok_hasmore(char **p_cur);
void skipNextComma(char **p_cur);
void skipWhiteSpace(char **p_cur);
int at_tok_flag_start(char **p_cur, char start_flag);

int service_init(const char *client_Name);

#ifdef __cplusplus
}

typedef enum {
    EMBMS,
    MDT,
    MAX_SERVICE_NUM
} SERVICE_ID;

typedef struct RespLine { /* a singly-lined list of intermediate responses */
    struct RespLine *p_next;
    char *line;
} RespLine;

typedef struct {
    int success;                /* true if final response indicates success (eg "OK") */
    char *finalResponse;        /* eg OK, ERROR */
    RespLine *p_intermediates;    /* any intermediate responses */
} CmdResponse;

typedef struct ChannelInfo {
    int s_fd;
    int channelID;
    char name[128];
    char s_respBuffer[MAX_BUFFER_BYTES + 1];
    char *s_respBufferCur;
    char *line; /* current line */
    char *s_responsePrefix;
    CmdResponse *sp_response;
    char *p_read;
    char *p_eol;
} ChannelInfo;

#endif  // LITE_RIL_H_
