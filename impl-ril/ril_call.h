/**
 * ril_call.h --- Call-related requests
 *                process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_CALL_H_
#define RIL_CALL_H_

#define NUM_ECC_WITHOUT_SIM 8  // 112,911,000,08,110,118,119,999
#define NUM_ECC_WITH_SIM 2  // 112，911

typedef struct ListNode {
    char data;
    struct ListNode *next;
    struct ListNode *prev;
} ListNode;

/* add for VoLTE to handle +CLCCS */
typedef enum {
    VOLTE_CALL_IDEL = 1,
    VOLTE_CALL_CALLING_MO = 2,
    VOLTE_CALL_CONNECTING_MO = 3,
    VOLTE_CALL_ALERTING_MO = 4,
    VOLTE_CALL_ALERTING_MT = 5,
    VOLTE_CALL_ACTIVE = 6,
    VOLTE_CALL_RELEASED_MO = 7,
    VOLTE_CALL_RELEASED_MT = 8,
    VOLTE_CALL_USER_BUSY = 9,
    VOLTE_CALL_USER_DETERMINED_BUSY = 10,
    VOLTE_CALL_WAITING_MO = 11,
    VOLTE_CALL_WAITING_MT = 12,
    VOLTE_CALL_HOLD_MO = 13,
    VOLTE_CALL_HOLD_MT = 14
} RIL_VoLTE_CallState;

/* add for VoLTE to handle SRVCC */
typedef enum {
    SRVCC_PS_TO_CS_START = 0,
    SRVCC_PS_TO_CS_SUCCESS = 1,
    SRVCC_PS_TO_CS_CANCELED = 2,
    SRVCC_PS_TO_CS_FAILED = 3,
    VSRVCC_PS_TO_CS_START = 4,
    VSRVCC_PS_TO_CS_SUCCESS = 5,
    SRVCC_CS_TO_PS_START = 6,
    SRVCC_CS_TO_PS_CANCELED = 7,
    SRVCC_CS_TO_PS_FAILED = 8,
    SRVCC_CS_TO_PS_SUCCESS = 9,
} RIL_VoLTE_SrvccState;

typedef enum {
    MEDIA_REQUEST_DEFAULT = 0,
    MEDIA_REQUEST_AUDIO_UPGRADE_VIDEO_BIDIRECTIONAL = 1,
    MEDIA_REQUEST_AUDIO_UPGRADE_VIDEO_TX = 2,
    MEDIA_REQUEST_AUDIO_UPGRADE_VIDEO_RX = 3,
    MEDIA_REQUEST_VIDEO_TX_UPGRADE_VIDEO_BIDIRECTIONAL = 4,
    MEDIA_REQUEST_VIDEO_RX_UPGRADE_VIDEO_BIDIRECTIONAL = 5,
    MEDIA_REQUEST_VIDEO_BIDIRECTIONAL_DOWNGRADE_AUDIO = 6,
    MEDIA_REQUEST_VIDEO_TX_DOWNGRADE_AUDIO = 7,
    MEDIA_REQUEST_VIDEO_RX_DOWNGRADE_AUDIO = 8,
    MEDIA_REQUEST_VIDEO_BIDIRECTIONAL_DOWNGRADE_VIDEO_TX = 9,
    MEDIA_REQUEST_VIDEO_BIDIRECTIONAL_DOWNGRADE_VIDEO_RX = 10,
} RIL_VoLTE_MEDIA_REQUEST;

typedef enum {
    VIDEO_CALL_MEDIA_DESCRIPTION_INVALID = 1000,
    VIDEO_CALL_MEDIA_DESCRIPTION_SENDRECV = 1001,  // "m=video\a=sendrecv" or "m=video"
    VIDEO_CALL_MEDIA_DESCRIPTION_SENDONLY = 1002,  // "m=video\a=sendonly"
    VIDEO_CALL_MEDIA_DESCRIPTION_RECVONLY = 1003,  // "m=video\a=recvonly"
} RIL_VoLTE_RESPONSE_MEDIA_CHANGE;

// cp name's value is not the same as RIL_CDMA_InfoRecName defined.
// 0  Display_struct
// 1  called_party_number_struct
// 2  calling_party_number_struct
// 3  connected_number_struct
// 4  Signal_struct
// 9  redirecting_number_struct
// 13 line_control_struct
// 15 extended_display_struct
// 16 Clir_struct
// 17 audio_control_struct
typedef enum {
    SPNOTI_CDMA_DISPLAY_INFO_REC = 0,
    SPNOTI_CDMA_CALLED_PARTY_NUMBER_INFO_REC = 1,
    SPNOTI_CDMA_CALLING_PARTY_NUMBER_INFO_REC = 2,
    SPNOTI_CDMA_CONNECTED_NUMBER_INFO_REC = 3,
    SPNOTI_CDMA_SIGNAL_INFO_REC = 4,
    SPNOTI_CDMA_REDIRECTING_NUMBER_INFO_REC = 9,
    SPNOTI_CDMA_LINE_CONTROL_INFO_REC = 13,
    SPNOTI_CDMA_EXTENDED_DISPLAY_INFO_REC = 15,
    SPNOTI_CDMA_T53_CLIR_INFO_REC = 16,
    SPNOTI_CDMA_T53_AUDIO_CONTROL_INFO_REC = 17
} SPNOTI_CDMA_InfoRecName;

typedef struct Srvccpendingrequest {
    char *cmd;
    struct Srvccpendingrequest *p_next;
} SrvccPendingRequest;

extern ListNode s_DTMFList[SIM_COUNT];
extern int s_simEccLen[SIM_COUNT];
extern RIL_EmergencyNumber *s_simEccList[SIM_COUNT];
extern RIL_EmergencyNumber s_defaultEccWithoutSim[NUM_ECC_WITHOUT_SIM];
extern RIL_EmergencyNumber s_defaultEccWithSim[NUM_ECC_WITH_SIM];
extern int s_callCount[SIM_COUNT];

void onModemReset_Call();

int processCallRequest(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
int processCallUnsolicited(RIL_SOCKET_ID socket_id, const char *s);

int all_calls(RIL_SOCKET_ID socket_id, int do_mute);

void list_init(ListNode *node);

void initDefaultEccList();

void sendUnsolEccList(void *param);

void freeSimEcclist(RIL_SOCKET_ID socketId);

#endif  // RIL_CALL_H_
