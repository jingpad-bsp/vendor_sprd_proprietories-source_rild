/**
 * ril_network.h --- Network-related requests
 *                process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_NETWORK_H_
#define RIL_NETWORK_H_

#define MODEM_WORKMODE_PROP     "persist.vendor.radio.modem.workmode"
#define MODEM_CAPABILITY        "persist.vendor.radio.modem.capability"
#define OVERSEA_VERSION         "ro.carrier"
#define MODEM_ENABLED_PROP      "persist.vendor.radio.modem_enabled"
#define MODEM_SMART_NR_PROP     "persist.radio.engtest.nr.enable"

// for usb Shared
extern long int s_rxBytes;
extern long int s_txBytes;
extern pthread_mutex_t s_usbSharedMutex ;

/*  LTE PS registration state */
typedef enum {
    STATE_OUT_OF_SERVICE = 0,
    STATE_IN_SERVICE = 1
} LTE_PS_REG_STATE;

/* Nr switch commend source */
typedef enum {
    POWER_SAVING_MODE = 1,
    WIFI_MODE,
    TEMPERRATURE_MODE,
    APPLICATION_MODE,
    SCREEN_STATUS,
    SPEED_TEST
} NrSwitchMode;

typedef enum {
    NETWORK_MODE_WCDMA_PREF         = 0,    /* GSM/WCDMA (WCDMA preferred) */
    NETWORK_MODE_GSM_ONLY           = 1,    /* GSM only */
    NETWORK_MODE_WCDMA_ONLY         = 2,    /* WCDMA only */
    NETWORK_MODE_GSM_UMTS           = 3,    /* GSM/WCDMA (auto mode, according to PRL)
                                               AVAILABLE Application Settings menu */
    NETWORK_MODE_CDMA               = 4,    /* CDMA and EvDo (auto mode, according to PRL)
                                               AVAILABLE Application Settings menu */
    NETWORK_MODE_CDMA_NO_EVDO       = 5,    /* CDMA only */
    NETWORK_MODE_EVDO_NO_CDMA       = 6,    /* EvDo only */
    NETWORK_MODE_GLOBAL             = 7,    /* GSM/WCDMA, CDMA, and EvDo (auto mode, according to PRL)
                                               AVAILABLE Application Settings menu */
    NETWORK_MODE_LTE_CDMA_EVDO      = 8,    /* LTE, CDMA and EvDo */
    NETWORK_MODE_LTE_GSM_WCDMA      = 9,    /* LTE, GSM/WCDMA */
    NETWORK_MODE_LTE_CDMA_EVDO_GSM_WCDMA = 10,  /* LTE, CDMA, EvDo, GSM/WCDMA */
    NETWORK_MODE_LTE_ONLY           = 11,   /* LTE Only mode. */
    NETWORK_MODE_LTE_WCDMA          = 12,   /* LTE/WCDMA */
    NETWORK_MODE_TDSCDMA_ONLY       = 13,   /** TD-SCDMA only */
    NETWORK_MODE_TDSCDMA_WCDMA      = 14,   /** TD-SCDMA and WCDMA */
    NETWORK_MODE_LTE_TDSCDMA        = 15,   /** LTE and TD-SCDMA*/
    NETWORK_MODE_TDSCDMA_GSM        = 16,   /** TD-SCDMA and GSM */
    NETWORK_MODE_LTE_TDSCDMA_GSM    = 17,   /** TD-SCDMA, GSM and LTE */
    NETWORK_MODE_TDSCDMA_GSM_WCDMA  = 18,   /** TD-SCDMA, GSM and WCDMA */
    NETWORK_MODE_LTE_TDSCDMA_WCDMA  = 19,   /** LTE, TD-SCDMA and WCDMA */
    NETWORK_MODE_LTE_TDSCDMA_GSM_WCDMA = 20,    /** LTE, TD-SCDMA, GSM, and WCDMA */
    NETWORK_MODE_TDSCDMA_CDMA_EVDO_GSM_WCDMA = 21,  /** TD-SCDMA, CDMA, EVDO, GSM and WCDMA */
    NETWORK_MODE_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA = 22,  /** LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA */
    NETWORK_MODE_NR_ONLY            = 23,   /** NR 5G only mode */
    NETWORK_MODE_NR_LTE             = 24,   /** NR 5G, LTE */
    NETWORK_MODE_NR_LTE_CDMA_EVDO   = 25,   /** NR 5G, LTE, CDMA and EvDo */
    NETWORK_MODE_NR_LTE_GSM_WCDMA   = 26,   /** NR 5G, LTE, GSM and WCDMA */
    NETWORK_MODE_NR_LTE_CDMA_EVDO_GSM_WCDMA = 27,    /** NR 5G, LTE, CDMA, EvDo, GSM and WCDMA */
    NETWORK_MODE_NR_LTE_WCDMA       = 28,   /** NR 5G, LTE and WCDMA */
    NETWORK_MODE_NR_LTE_TDSCDMA     = 29,   /** NR 5G, LTE and TDSCDMA */
    NETWORK_MODE_NR_LTE_TDSCDMA_GSM = 30,   /** NR 5G, LTE, TD-SCDMA and GSM */
    NETWORK_MODE_NR_LTE_TDSCDMA_WCDMA = 31, /** NR 5G, LTE, TD-SCDMA, WCDMA */
    NETWORK_MODE_NR_LTE_TDSCDMA_GSM_WCDMA = 32,    /** NR 5G, LTE, TD-SCDMA, GSM and WCDMA */
    NETWORK_MODE_NR_LTE_TDSCDMA_CDMA_EVDO_GSM_WCDMA = 33,    /** NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA */

    // UNISOC: Add extended network mode LTE/GSM
    NETWORK_MODE_LTE_GSM             = 34, /* LTE, GSM */

    NETWORK_MODE_BASE = 50,
    NT_TD_LTE = NETWORK_MODE_BASE + 1,
    NT_LTE_FDD = NETWORK_MODE_BASE + 2,
    NT_LTE_FDD_TD_LTE = NETWORK_MODE_BASE + 3,
    NT_LTE_FDD_WCDMA_GSM = NETWORK_MODE_BASE + 4,
    NT_TD_LTE_WCDMA_GSM = NETWORK_MODE_BASE + 5,
    NT_LTE_FDD_TD_LTE_WCDMA_GSM = NETWORK_MODE_BASE + 6,
    NT_TD_LTE_TDSCDMA_GSM = NETWORK_MODE_BASE + 7,
    NT_LTE_FDD_TD_LTE_TDSCDMA_GSM = NETWORK_MODE_BASE + 8,
    NT_LTE_FDD_TD_LTE_WCDMA_TDSCDMA_GSM = NETWORK_MODE_BASE + 9,
    NT_GSM = NETWORK_MODE_BASE + 10,
    NT_WCDMA = NETWORK_MODE_BASE + 11,
    NT_TDSCDMA = NETWORK_MODE_BASE + 12,
    NT_TDSCDMA_GSM = NETWORK_MODE_BASE + 13,
    NT_WCDMA_GSM = NETWORK_MODE_BASE + 14,
    NT_WCDMA_TDSCDMA_EVDO_CDMA_GSM = NETWORK_MODE_BASE + 15,
    NT_LTE_FDD_TD_LTE_GSM = NETWORK_MODE_BASE + 16,
    NT_LTE_WCDMA_TDSCDMA_EVDO_CDMA_GSM = NETWORK_MODE_BASE + 17,
    NT_LTE_FDD_TD_LTE_WCDMA= NETWORK_MODE_BASE + 18,
    NT_NR = NETWORK_MODE_BASE + 19,
    NT_NR_LTE_FDD_TD_LTE = NETWORK_MODE_BASE + 20,
    NT_NR_LTE_FDD_TD_LTE_GSM_WCDMA =  NETWORK_MODE_BASE + 21,
    NT_EVDO_CDMA = NETWORK_MODE_BASE + 22,
    NT_CDMA = NETWORK_MODE_BASE + 23,
    NT_EVDO = NETWORK_MODE_BASE + 24,
} NetworkMode;

typedef struct {
    bool s_sim_busy;
    pthread_mutex_t s_sim_busy_mutex;
    pthread_cond_t s_sim_busy_cond;
} SimBusy;

typedef struct OperatorInfoList {
    char *plmn;
    char *longName;
    char *shortName;
    struct OperatorInfoList *next;
    struct OperatorInfoList *prev;
} OperatorInfoList;

typedef struct SetPropPara {
    int socketId;
    char *propName;
    char *propValue;
    pthread_mutex_t *mutex;
} SetPropPara;

#define RIL_SIGNALSTRENGTH_INVALID 0x7FFFFFFF

#define RIL_SIGNALSTRENGTH_INIT(ril_signalstrength) do {                                               \
    ril_signalstrength.GW_SignalStrength.signalStrength     = 99;                                       \
    ril_signalstrength.GW_SignalStrength.bitErrorRate       = -1;                                       \
    ril_signalstrength.CDMA_SignalStrength.dbm              = -1;                                       \
    ril_signalstrength.CDMA_SignalStrength.ecio             = -1;                                       \
    ril_signalstrength.EVDO_SignalStrength.dbm              = -1;                                       \
    ril_signalstrength.EVDO_SignalStrength.ecio             = -1;                                       \
    ril_signalstrength.EVDO_SignalStrength.signalNoiseRatio = -1;                                       \
    ril_signalstrength.LTE_SignalStrength.signalStrength    = 99;                                       \
    ril_signalstrength.LTE_SignalStrength.rsrp              = RIL_SIGNALSTRENGTH_INVALID;               \
    ril_signalstrength.LTE_SignalStrength.rsrq              = RIL_SIGNALSTRENGTH_INVALID;               \
    ril_signalstrength.LTE_SignalStrength.rssnr             = RIL_SIGNALSTRENGTH_INVALID;               \
    ril_signalstrength.LTE_SignalStrength.cqi               = RIL_SIGNALSTRENGTH_INVALID;               \
    ril_signalstrength.LTE_SignalStrength.timingAdvance     = RIL_SIGNALSTRENGTH_INVALID;               \
    ril_signalstrength.TD_SCDMA_SignalStrength.rscp         = RIL_SIGNALSTRENGTH_INVALID;               \
} while (0);

#define RIL_SIGNALSTRENGTH_INIT_1_4(ril_signalstrength) do {                                            \
    ril_signalstrength.gsm.signalStrength     = 99;                                                     \
    ril_signalstrength.gsm.bitErrorRate       = -1;                                                     \
    ril_signalstrength.gsm.timingAdvance = RIL_SIGNALSTRENGTH_INVALID;                                  \
    ril_signalstrength.cdma.dbm              = -1;                                                      \
    ril_signalstrength.cdma.ecio             = -1;                                                      \
    ril_signalstrength.evdo.dbm              = -1;                                                      \
    ril_signalstrength.evdo.ecio             = -1;                                                      \
    ril_signalstrength.evdo.signalNoiseRatio = -1;                                                      \
    ril_signalstrength.lte.signalStrength    = 99;                                                      \
    ril_signalstrength.lte.rsrp              = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.lte.rsrq              = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.lte.rssnr             = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.lte.cqi               = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.lte.timingAdvance     = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.tdscdma.signalStrength = 99;                                                     \
    ril_signalstrength.tdscdma.bitErrorRate  = -1;                                                      \
    ril_signalstrength.tdscdma.rscp          = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.wcdma.signalStrength  = 99;                                                      \
    ril_signalstrength.wcdma.bitErrorRate    = -1;                                                      \
    ril_signalstrength.wcdma.rscp            = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.wcdma.ecno            = -1;                                                      \
    ril_signalstrength.nr.ssRsrp             = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.nr.ssRsrq             = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.nr.ssSinr             = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.nr.csiRsrp            = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.nr.csiRsrq            = RIL_SIGNALSTRENGTH_INVALID;                              \
    ril_signalstrength.nr.csiSinr            = RIL_SIGNALSTRENGTH_INVALID;                              \
} while (0);

#define RIL_SIGNALSTRENGTH_INIT_LTE(ril_signalstrength) do {                          \
    ril_signalstrength.LTE_SignalStrength.signalStrength = 99;                         \
    ril_signalstrength.LTE_SignalStrength.rsrp           = RIL_SIGNALSTRENGTH_INVALID; \
    ril_signalstrength.LTE_SignalStrength.rsrq           = RIL_SIGNALSTRENGTH_INVALID; \
    ril_signalstrength.LTE_SignalStrength.rssnr          = RIL_SIGNALSTRENGTH_INVALID; \
    ril_signalstrength.LTE_SignalStrength.cqi            = RIL_SIGNALSTRENGTH_INVALID; \
} while (0);

extern int s_presentSIMCount;
extern int s_in4G[SIM_COUNT];
extern int s_in2G[SIM_COUNT];
extern int s_workMode[SIM_COUNT];
extern int s_sessionId[SIM_COUNT];
extern int s_desiredRadioState[SIM_COUNT];
extern int s_imsRegistered[SIM_COUNT];  // 0 == unregistered
extern int s_imsBearerEstablished[SIM_COUNT];
extern int s_rxlev[SIM_COUNT];
extern int s_ber[SIM_COUNT];
extern int s_rscp[SIM_COUNT];
extern int s_ecno[SIM_COUNT];
extern int s_rsrq[SIM_COUNT];
extern int s_rsrp[SIM_COUNT];
extern int s_ss_rsrp[SIM_COUNT];
extern LTE_PS_REG_STATE s_PSRegState[SIM_COUNT];
extern pthread_mutex_t s_LTEAttachMutex[SIM_COUNT];
extern RIL_RegState s_CSRegStateDetail[SIM_COUNT];
extern RIL_RegState s_PSRegStateDetail[SIM_COUNT];
extern pthread_mutex_t s_radioPowerMutex[SIM_COUNT];
extern SimBusy s_simBusy[SIM_COUNT];
extern OperatorInfoList s_operatorInfoList[SIM_COUNT];
extern OperatorInfoList s_operatorXmlInfoList;
extern pthread_cond_t s_sigConnStatusCond[SIM_COUNT];
extern RIL_SingnalConnStatus s_sigConnStatus[SIM_COUNT];
extern bool s_sigConnStatusWait[SIM_COUNT];
extern int s_nrCfgInfo[SIM_COUNT][2];

void onModemReset_Network();
int processNetworkRequests(int request, void *data, size_t datalen,
                           RIL_Token t, RIL_SOCKET_ID socket_id);
int processNetworkUnsolicited(RIL_SOCKET_ID socket_id, const char *s);
uint64_t ril_nano_time();
void initPrimarySim();

void queryCesqVersion(RIL_SOCKET_ID socket_id);
void dispatchSPTESTMODE(RIL_Token t, void *data, void *resp);

/* for AT+CESQ execute command response process */
void cesq_execute_cmd_rsp(RIL_SOCKET_ID socket_id, ATResponse *p_response,
                          ATResponse **p_newResponse);

/* for +CESQ: unsol response process */
int cesq_unsol_rsp(char *line, RIL_SOCKET_ID socket_id, char *newLine);

/* send AT commend for Nr Enable Switch */
void sendEnableNrSwitchCommand(RIL_SOCKET_ID socket_id, int mode, int enable);

extern int updatePlmn(int slotId, int lac, const char *mncmcc, char *resp, size_t respLen);
extern int updateNetworkList(int slotId, char **networkList, size_t datalen,
                             char *resp, size_t respLen);

void onSignalStrengthUnsolResponse(const void *data, RIL_SOCKET_ID socket_id);
int convert3GValueTodBm(int cp3GValue);
int getWcdmaSigStrengthBydBm(int dBm);
int getWcdmaRscpBydBm(int dBm);

#endif  // RIL_NETWORK_H_
