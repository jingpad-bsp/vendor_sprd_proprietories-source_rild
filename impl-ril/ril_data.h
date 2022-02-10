/**
 * ril_data.h --- Data-related requests
 *                process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_DATA_H_
#define RIL_DATA_H_

#define MAX_PDP                     6
#define MAX_PDP_CP                  11
#define MINIMUM_APN_LEN             19
#define MAX_ETH                     8

#define DATA_ACTIVE_FALLBACK_FAILED             -2
#define DATA_ACTIVE_FAILED                      -1
#define DATA_ACTIVE_SUCCESS                     0
#define DATA_ACTIVE_NEED_RETRY                  1
#define DATA_ACTIVE_NEED_FALLBACK               2
#define DATA_ACTIVE_NEED_RETRY_FOR_ANOTHER_CID  3
#define DATA_ACTIVE_NEED_RETRY_AFTER_DELAY_TIME 4

#define TRAFFIC_CLASS_DEFAULT       2
#define UNUSABLE_CID                0

#define IP_ADDR_SIZE                16
#define IPV6_ADDR_SIZE              64
#define IP_ADDR_MAX                 128
#define MAX_PDP_NUM                 12
#define PROPERTY_NAME_MAX           32
#define FILE_BUFFER_LENGTH          1024
#define NET_INTERFACE_LENGTH        128

#define PDP_STATE_IDLE              1
#define PDP_STATE_ACTING            2
#define PDP_STATE_CONNECT           3
#define PDP_STATE_ESTING            4
#define PDP_STATE_ACTIVE            5
#define PDP_STATE_DESTING           6
#define PDP_STATE_DEACTING          7
#define PDP_STATE_ACT_ERROR         8
#define PDP_STATE_EST_ERROR         9
#define PDP_STATE_EST_UP_ERROR      10

#define SYS_NET_ADDR                "vendor.data.net.addr"
#define SYS_NET_ACTIVATING_TYPE     "vendor.data.activating.type"
#define SYS_IPV6_LINKLOCAL          "vendor.data.ipv6.linklocal"
#define DEFAULT_PUBLIC_DNS2         "204.117.214.10"
// Due to real network limited,
// 2409:8084:8000:0010:2002:4860:4860:8888 maybe not correct
#define DEFAULT_PUBLIC_DNS2_IPV6    "2409:8084:8000:0010:2002:4860:4860:8888"

#define GSPS_IPV4_ADDR_HEADER       "192.168."
#define DHCP_DNSMASQ_LEASES_FILE    "/data/misc/dhcp/dnsmasq.leases"

#define GSPS_ETH_UP_PROP            "vendor.ril.gsps.eth.up"
#define GSPS_ETH_DOWN_PROP          "vendor.ril.gsps.eth.down"
#define ATTACH_ENABLE_PROP          "persist.vendor.sys.attach.enable"
#define ALLOW_DATA_SOCKET_ID        "vendor.ril.allow_data.socket_id"
#define ALLOW_DATA_MODEM_SOCKET_ID  "vendor.ril.allow_data.modem.socket_id"

#define SOCKET_NAME_EXT_DATA        "ext_data"

// Default MTU value
#define DEFAULT_MTU 1500
typedef enum {
    UNKNOWN = 0,
    IPV4    = 1,
    IPV6    = 2,
    IPV4V6  = 3
} IPType;

enum States{
    OPEN,
    REUSE,
    CLOSE,
};

enum EthState {
    ETH_IDLE,
    ETH_BUSY,
};

typedef struct {
    int socketId;
    int cid;
} EthInfo;

enum PDPState {
    PDP_IDLE,
    PDP_BUSY,
};

struct PDPInfo {
    int socketId;
    int cid;
    int secondary_cid;  // for fallback cid
    bool isPrimary;
    enum PDPState state;
    pthread_mutex_t mutex;
};

struct OpenchannelInfo {
    int cid;
    enum States state;
    bool pdpState;
    int count;
};

typedef struct {
    int nCid;
    char strIPType[64];
    char strApn[64];
    char strAttachApn[64];
} PDNInfo;

typedef struct PDP_INFO {
    char dns1addr[IP_ADDR_SIZE];            /* IPV4 Primary MS DNS entries */
    char dns2addr[IP_ADDR_SIZE];            /* IPV4 secondary MS DNS entries */
    char userdns1addr[IP_ADDR_SIZE];        /* IPV4 Primary MS DNS entries */
    char userdns2addr[IP_ADDR_SIZE];        /* IPV4 secondary MS DNS entries */
    char ipladdr[IP_ADDR_SIZE];             /* IPV4 address local */
    char ipraddr[IP_ADDR_SIZE];             /* IPV4 address remote */
    char ipv6dns1addr[IPV6_ADDR_SIZE];      /* IPV6 Primary MS DNS entries */
    char ipv6dns2addr[IPV6_ADDR_SIZE];      /* IPV6 secondary MS DNS entries */
    char ipv6userdns1addr[IPV6_ADDR_SIZE];  /* IPV6 Primary MS DNS entries */
    char ipv6userdns2addr[IPV6_ADDR_SIZE];  /* IPV6 secondary MS DNS entries */
    char ipv6laddr[IPV6_ADDR_SIZE];         /* IPV6 address local */
    char ipv6raddr[IPV6_ADDR_SIZE];         /* IPV6 address remote */
    IPType ip_state;
    int state;
    int cid;
    int manual_dns;
    int error_num;
} PDP_INFO;

typedef enum {
    UNSOLICTED_DATA_CALL,
    GET_DATA_CALL,
    SETUP_DATA_CALL,
} DataCallListType;
/* data call fail cause mapping */
typedef enum {
    MN_GPRS_ERR_OPERATOR_DETERMINE_BAR             = 0x08,
    MN_GPRS_ERR_LLC_SND_FAILURE                    = 0x19,
    MN_GPRS_ERR_INSUFF_RESOURCE                    = 0x1A,
    MN_GPRS_ERR_UNKNOWN_APN                        = 0x1B,
    MN_GPRS_ERR_UNKNOWN_PDP_ADDR_OR_TYPE           = 0x1C,
    MN_GPRS_ERR_AUTHENTICATION_FAILURE             = 0x1D,
    MN_GPRS_ERR_ACTIVATION_REJ_GGSN                = 0x1E,
    MN_GPRS_ERR_ACTIVATION_REJ                     = 0x1F,
    MN_GPRS_ERR_UNSUPPORTED_SERVICE_OPTION         = 0x20,
    MN_GPRS_ERR_UNSUBSCRIBED_SERVICE_OPTION        = 0x21,
    MN_GPRS_ERR_OUT_OF_ORDER_SERVICE_OPTION        = 0x22,
    MN_GPRS_ERR_NSAPI_ALREADY_USED                 = 0x23,
    MN_GPRS_ERR_REGULAR_DEACTIVATION               = 0x24,
    MN_GPRS_ERR_QOS_NOT_ACCEPTED                   = 0x25,
    MN_GPRS_ERR_NETWORK_FAIL                       = 0x26,
    MN_GPRS_ERR_REACTIVATION_REQD                  = 0x27,
    MN_GPRS_ERR_FEATURE_NOT_SUPPORTED              = 0x28,
    MN_GPRS_ERR_SEMANTIC_ERROR_IN_TFT_OP           = 0x29,
    MN_GPRS_ERR_SYNTACTICAL_ERROR_IN_TFT_OP        = 0x2A,
    MN_GPRS_ERR_UNKNOWN_PDP_CONTEXT                = 0x2B,
    MN_GPRS_ERR_SEMANTIC_ERROR_IN_PACKET_FILTER    = 0x2C,
    MN_GPRS_ERR_SYNTAX_ERROR_IN_PACKET_FILTER      = 0x2D,
    MN_GPRS_ERR_PDP_CONTEXT_WO_TFT_ALREADY_ACT     = 0x2E,
    MN_GPRS_ERR_SM_ERR_UNSPECIFIED                 = 0x2F,
    MN_GPRS_ERR_MAX_ACTIVE_PDP_REACHED             = 0x41,
    MN_GPRS_ERR_INVALID_TI                         = 0x51,
    MN_GPRS_ERR_INCORRECT_MSG                      = 0x5F,
    MN_GPRS_ERR_INVALID_MAND_INFO                  = 0x60,
    MN_GPRS_ERR_UNIMPLE_MSG_TYPE                   = 0x61,
    MN_GPRS_ERR_INCOMPAT_MSG_TYP_PROTO_STAT        = 0x62,
    MN_GPRS_ERR_UNIMPLE_IE                         = 0x63,
    MN_GPRS_ERR_CONTEXT_CAUSE_CONDITIONAL_IE_ERROR = 0x64,
    MN_GPRS_ERR_INCOMP_MSG_PROTO_STAT              = 0x65,
    MN_GPRS_ERR_UNSPECIFIED                        = 0x6F,
    MN_GPRS_ERR_STARTUP_FAILURE                    = 0x70,
// error codes for cdma begin
    MN_GPRS_ERR_GENERAL_ERROR                      = 0XA0,
    MN_GPRS_ERR_PGW_REJECT                         = 0XA1,
    MN_GPRS_ERR_RESOURCE_UNAVAILABLE               = 0XA2,
    MN_GPRS_ERR_RECONNECT_NOT_ALLOWED              = 0XA3,
    MN_GPRS_ERR_EAPAKA_FAILURE                     = 0XA4,
    MN_GPRS_ERR_NETWORK_NO_RSP                     = 0XA5,
    MN_GPRS_ERR_PDN_ATTACH_ABORT                   = 0XA6,
    MN_GPRS_ERR_INVALID_PDN_ATTACH_REQ             = 0XA7,
    MN_GPRS_ERR_PDN_REC_FAILURE                    = 0XA8,
    MN_GPRS_ERR_MAIN_CONN_SETUP_FAILURE            = 0XA9,
    MN_GPRS_ERR_BEARER_RESOURCE_UNAVAILABLE        = 0XAA,
    MN_GPRS_ERR_EAPAKA_REJECT                      = 0XAB,
    MN_GPRS_ERR_LCP_NEGO3_FAILURE                  = 0XAC,
    MN_GPRS_ERR_TCH_SETUP_FAILURE                  = 0XAD,
    MN_GPRS_ERR_NW_NO_RSP_IN_LCP                   = 0XAE,
    MN_GPRS_ERR_NW_NO_RSP_INAUTH                   = 0XAF,
    MN_GPRS_ERR_PDN_TIMEOUT                        = 0XB0,
    MN_GPRS_ERR_DEFCONT_FAILURE                    = 0XB1,
    MN_GPRS_ERR_DETACHED                           = 0XB2,
    MN_GPRS_ERR_INTERNAL_FAILURE                   = 0XB3,
    MN_GPRS_ERR_LOCAL_DETACH_DUE_TO_IRAT           = 0XB4,
    MN_GPRS_ERR_UNAUTHORIZED_APN                   = 0XB5,
    MN_GPRS_ERR_PDN_LIMIT_EXCEEDED                 = 0XB6,
    MN_GPRS_ERR_NO_PGW_AVALIABLE                   = 0XB7,
    MN_GPRS_ERR_PGW_UNREACHABLE                    = 0XB8,
    MN_GPRS_ERR_INSUFFICIENT_PARAMETERS            = 0XB9,
    MN_GPRS_ERR_ADMIN_PROHIBITED                   = 0XBA,
    MN_GPRS_ERR_PDNID_ALREADY_INUSED               = 0XBB,
    MN_GPRS_ERR_SUBSCRIPTION_LIMITATION            = 0XBC,
    MN_GPRS_ERR_PDNCONN_ALREADY_EXIST_FORPDN       = 0XBD,
    MN_GPRS_ERR_EMERGENCY_NOT_SUPPORTED            = 0XBE,
    MN_GPRS_ERR_RETRY_TMR_THROTTLING               = 0XBF,
    MN_GPRS_ERR_PDN_LIMIT_EXCEEDED_INUESIDE        = 0XC0,
    MN_GPRS_ERR_PDNID_ALREADY_INUSE_INSESIDE       = 0XC1,
    MN_GPRS_ERR_OP_ABORT_BY_USER                   = 0XC2,
    MN_GPRS_ERR_RTT_DATA_CNNECTED                  = 0XC3,
    MN_GPRS_ERR_ERR_ALREADY_IN_REQUEST_STATE       = 0XC4,
    MN_GPRS_ERR_POWER_DOWN                         = 0XC5,
// error codes for cdma end
    MN_GPRS_ERR_START                              = 0xff,
    MN_GPRS_ERR_PRIMITIVE,
    MN_GPRS_ERR_MEM_ALLOC,
    MN_GPRS_ERR_NO_NSAPI,
    MN_GPRS_ERR_SENDER,
    MN_GPRS_ERR_PDP_TYPE,
    MN_GPRS_ERR_ATC_PARAM,
    MN_GPRS_ERR_PDP_ID,
    MN_GPRS_ERR_SPACE_NOT_ENOUGH,
    MN_GPRS_ERR_ACTIVE_REJCET,
    MN_GPRS_ERR_SAME_PDP_CONTEXT,
    MN_GPRS_ERR_NSAPI,
    MN_GPRS_ERR_MODIFY_REJ,
    MN_GPRS_ERR_READ_TYPE,
    MN_GPRS_ERR_PDP_CONTEXT_ACTIVATED,
    MN_GPRS_ERR_NO_PDP_CONTEXT,
    MN_GPRS_ERR_PERMENANT_PROBLEM,
    MN_GPRS_ERR_TEMPORARILY_BLOCKED,
    MN_GPRS_ERR_RETRYING,
    MN_GPRS_ERR_UNKNOWN_ERROR,
    MN_GPRS_ERR_SERVICE_OPTION_NOT_SUPPORTED,
    MN_GPRS_ERR_REQUEST_SERVICE_OPTION_NOT_SUBSCRIBED,
    MN_GPRS_ERR_SERVICE_OPTION_OUTOF_ORDER,
    MN_GPRS_ERR_PDP_AUTHENTICATION_FAILED,
    MN_GPRS_ERR_MISSING_OR_UNKOWN_APN,
    MN_GPRS_ERR_OPERATION_NOT_ALLOWED,
    MN_GPRS_ERR_NO_SATISFIED_RESOURCE,
} RIL_CP_DataCallFailCause;

extern int s_GSCid;
extern int s_ethOnOff;
extern bool s_isGCFTest;
extern int s_dataAllowed[SIM_COUNT];
extern int s_ddsOnModem;
extern int s_manualSearchNetworkId;
extern bool s_LTEDetached[SIM_COUNT];
extern struct PDPInfo s_PDP[SIM_COUNT][MAX_PDP];

void onModemReset_Data();
void putPDP(RIL_SOCKET_ID socket_id, int cid);
int isExistActivePdp(RIL_SOCKET_ID socket_id);
int processDataRequest(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
int processDataUnsolicited(RIL_SOCKET_ID socket_ID, const char *s);

void ps_service_init();

int requestSetupDataConnection(RIL_SOCKET_ID socket_id, void *data, size_t datalen);
void requestDeactiveDataConnection(RIL_SOCKET_ID socket_id, void *data, size_t datalen);
void getEthNameByCid(RIL_SOCKET_ID socket_id, int cid, char *ethName, size_t len);
void sendPsDataOffToExtData(RIL_SOCKET_ID socket_id, int exemptionInfo, int port);

/* for AT+CGACT=0 set command response process */
void cgact_deact_cmd_rsp(int cid, RIL_SOCKET_ID socket_id);

/* for AT+CGDATA= set command process */
int cgdata_set_cmd_req(char *cgdataCmd);

/* for AT+CGDCONT= set command response process */
int cgdcont_set_cmd_req(char *cmd, char *newCmd);

/* for AT+CGDCONT? read response process */
void cgdcont_read_cmd_rsp(ATResponse *p_response, ATResponse **pp_outResponse);

/* for AT+CGDATA= set command response process */
int cgdata_set_cmd_rsp(ATResponse *p_response, int pdpIndex, int primaryCid,
                       RIL_SOCKET_ID socket_id);

void sendCmdToExtData(char cmd[]);

/*for GCF test only */
void startGSPS(void *param);

#endif  // RIL_DATA_H_
