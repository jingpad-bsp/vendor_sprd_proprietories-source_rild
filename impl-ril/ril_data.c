/**
 * ril_data.c --- Data-related requests process functions implementation
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL"

#include <ifaddrs.h>
#include <netutils/ifc.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netlink/msg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include "impl_ril.h"
#include "ril_data.h"
#include "ril_network.h"
#include "ril_call.h"
#include "ril_sim.h"
#include "channel_controller.h"
#include "ril_stk.h"
#include "utils.h"

#include <sys/wait.h>
#include <sys/types.h>

#define DDR_STATUS_PROP         "persist.vendor.sys.ddr.status"
#define REUSE_DEFAULT_PDN       "persist.vendor.sys.pdp.reuse"
#define BIP_OPENCHANNEL         "persist.vendor.radio.openchannel"
#define IS_BOOTS                "persist.vendor.support.boots.retry"
#define USB_TETHER_ENABLE       "net.usbtethering.enable"
#define RIL_USB_TETHER_FLAG     "ril.sys.usb.tether.flag"

int s_dataAllowed[SIM_COUNT];
int s_ddsOnModem;
int s_manualSearchNetworkId = -1;
/* for LTE, attach will occupy a cid for default PDP in CP */
bool s_LTEDetached[SIM_COUNT] = {0};
int s_GSCid;
int s_ethOnOff;
bool s_isGCFTest = false;
bool s_isPPPDStart = false;
static int s_activePDN;
static int s_addedIPCid = -1;  /* for VoLTE additional business */
static int s_pdpType = IPV4V6;

PDP_INFO pdp_info[MAX_PDP_NUM];
pthread_mutex_t s_psServiceMutex = PTHREAD_MUTEX_INITIALIZER;
static int s_extDataFd = -1;
static char s_SavedDns[IP_ADDR_SIZE] = {0};
static char s_SavedDns_IPV6[IP_ADDR_SIZE * 4] ={0};
static int s_swapCard = 0;
pthread_mutex_t s_signalBipPdpMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_signalBipPdpCond = PTHREAD_COND_INITIALIZER;

struct OpenchannelInfo s_openchannelInfo[SIM_COUNT][MAX_PDP];
static int s_openchannelCid = -1;

static int s_curCid[SIM_COUNT];

static int s_fdSocketV4[MAX_PDP];
static int s_fdSocketV6[MAX_PDP];
static int s_ethState[MAX_ETH];

/* Last PDP fail cause, obtained by *ECAV */
static int s_lastPDPFailCause[SIM_COUNT] = {
        PDP_FAIL_ERROR_UNSPECIFIED
#if (SIM_COUNT >= 2)
        ,PDP_FAIL_ERROR_UNSPECIFIED
#if (SIM_COUNT >= 3)
        ,PDP_FAIL_ERROR_UNSPECIFIED
#if (SIM_COUNT >= 4)
        ,PDP_FAIL_ERROR_UNSPECIFIED
#endif
#endif
#endif
};
static int s_trafficClass[SIM_COUNT] = {
        TRAFFIC_CLASS_DEFAULT
#if (SIM_COUNT >= 2)
        ,TRAFFIC_CLASS_DEFAULT
#if (SIM_COUNT >= 3)
        ,TRAFFIC_CLASS_DEFAULT
#if (SIM_COUNT >= 4)
        ,TRAFFIC_CLASS_DEFAULT
#endif
#endif
#endif
};

static int s_singlePDNAllowed[SIM_COUNT] = {
        0
#if (SIM_COUNT >= 2)
        ,0
#if (SIM_COUNT >= 3)
        ,0
#if (SIM_COUNT >= 4)
        ,0
#endif
#endif
#endif
};
struct PDPInfo s_PDP[SIM_COUNT][MAX_PDP] = {
    {{-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},}
#if (SIM_COUNT >= 2)
   ,{{-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},
     {-1, -1, -1, false, PDP_IDLE, PTHREAD_MUTEX_INITIALIZER},}
#endif
};
static PDNInfo s_PDN[MAX_PDP_CP] = {
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""},
    { -1, "", "", ""}
};

static EthInfo s_cidForEth[MAX_ETH] = {
    {-1, -1},
    {-1, -1},
    {-1, -1},
    {-1, -1},
    {-1, -1},
    {-1, -1},
    {-1, -1},
    {-1, -1}
};

static int detachGPRS(RIL_SOCKET_ID socket_id, RIL_Token t);
static bool isApnEqual(char *new, char *old);
static bool isProtocolEqual(char *new, char *old);
static bool isStrEqual(char *new, char *old);
int getEthIndexBySocketId(RIL_SOCKET_ID socket_id, int cid);
int downNetcard(int cid, char *netinterface, RIL_SOCKET_ID socket_id);

#if (SIM_COUNT == 2)
static void switchData(RIL_SOCKET_ID socket_id, bool isSwitchDataToCurrentSim);
#endif

void onModemReset_Data() {
    int i = 0;
    RIL_SOCKET_ID socket_id  = 0;

    s_manualSearchNetworkId =-1;
    s_activePDN = 0;
    s_swapCard = 0;
    s_openchannelCid = -1;
    s_addedIPCid = -1;

    memset(s_fdSocketV4, -1, sizeof(s_fdSocketV4));
    memset(s_fdSocketV6, -1, sizeof(s_fdSocketV6));

    for (socket_id = RIL_SOCKET_1; socket_id < RIL_SOCKET_NUM; socket_id++) {
        s_LTEDetached[socket_id] = 0;
        s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
        for (i = 0; i < MAX_PDP; i++) {
            putPDP(socket_id, i);
            s_openchannelInfo[socket_id][i].cid = -1;
            s_openchannelInfo[socket_id][i].state = CLOSE;
            s_openchannelInfo[socket_id][i].pdpState = false;
            s_openchannelInfo[socket_id][i].count = 0;
        }
    }

    memset(pdp_info, 0, sizeof(pdp_info));
    for (i = 0; i < MAX_PDP_NUM; i++) {
        pdp_info[i].state = PDP_STATE_IDLE;
    }
}

static int getPDP(RIL_SOCKET_ID socket_id) {
    int ret = -1;
    int i;

    for (i = 0; i < MAX_PDP; i++) {
        if ((s_roModemConfig >= LWG_LWG || socket_id == s_multiModeSim) &&
            s_activePDN > 0 && s_PDN[i].nCid == (i + 1)) {
            continue;
        }
        pthread_mutex_lock(&s_PDP[socket_id][i].mutex);
        if (s_PDP[socket_id][i].state == PDP_IDLE && s_PDP[socket_id][i].cid == -1) {
            s_PDP[socket_id][i].state = PDP_BUSY;
            ret = i;
            pthread_mutex_unlock(&s_PDP[socket_id][i].mutex);
            RLOGD("get s_PDP[%d]", ret);
            RLOGD("PDP[0].state = %d, PDP[1].state = %d, PDP[2].state = %d",
                    s_PDP[socket_id][0].state, s_PDP[socket_id][1].state, s_PDP[socket_id][2].state);
            RLOGD("PDP[3].state = %d, PDP[4].state = %d, PDP[5].state = %d",
                    s_PDP[socket_id][3].state, s_PDP[socket_id][4].state, s_PDP[socket_id][5].state);
            return ret;
        }
        pthread_mutex_unlock(&s_PDP[socket_id][i].mutex);
    }
    return ret;
}

void putPDP(RIL_SOCKET_ID socket_id, int cid) {
    if (cid < 0 || cid >= MAX_PDP || socket_id < RIL_SOCKET_1 ||
        socket_id >= SIM_COUNT) {
        return;
    }

    pthread_mutex_lock(&s_PDP[socket_id][cid].mutex);
    if (s_PDP[socket_id][cid].state != PDP_BUSY) {
        goto done;
    }
    s_PDP[socket_id][cid].state = PDP_IDLE;

done:
    if ((s_PDP[socket_id][cid].secondary_cid > 0) &&
        (s_PDP[socket_id][cid].secondary_cid <= MAX_PDP)) {
        s_PDP[socket_id][s_PDP[socket_id][cid].secondary_cid - 1].secondary_cid = -1;
    }
    s_PDP[socket_id][cid].secondary_cid = -1;
    s_PDP[socket_id][cid].cid = -1;
    s_PDP[socket_id][cid].isPrimary = false;
    RLOGD("put s_PDP[%d]", cid);
    pthread_mutex_unlock(&s_PDP[socket_id][cid].mutex);
    pthread_mutex_lock(&s_signalBipPdpMutex);
    if (s_openchannelInfo[socket_id][cid].count != 0) {
        s_openchannelInfo[socket_id][cid].count = 0;
    }
    s_openchannelInfo[socket_id][cid].pdpState = false;
    pthread_mutex_unlock(&s_signalBipPdpMutex);
}

static int getPDPByIndex(RIL_SOCKET_ID socket_id, int index) {
    if (index >= 0 && index < MAX_PDP) {  // cid: 1 ~ MAX_PDP
        pthread_mutex_lock(&s_PDP[socket_id][index].mutex);
        if (s_PDP[socket_id][index].state == PDP_IDLE && s_PDP[socket_id][index].cid != UNUSABLE_CID) {
            s_PDP[socket_id][index].state = PDP_BUSY;
            pthread_mutex_unlock(&s_PDP[socket_id][index].mutex);
            RLOGD("getPDPByIndex[%d]", index);
            RLOGD("PDP[0].state = %d, PDP[1].state = %d, PDP[2].state = %d",
                   s_PDP[socket_id][0].state, s_PDP[socket_id][1].state, s_PDP[socket_id][2].state);
            RLOGD("PDP[3].state = %d, PDP[4].state = %d, PDP[5].state = %d",
                   s_PDP[socket_id][3].state, s_PDP[socket_id][4].state, s_PDP[socket_id][5].state);
            return index;
        }
        pthread_mutex_unlock(&s_PDP[socket_id][index].mutex);
    }
    return -1;
}

void putPDPByIndex(RIL_SOCKET_ID socket_id, int index) {
    if (index < 0 || index >= MAX_PDP) {
        return;
    }
    pthread_mutex_lock(&s_PDP[socket_id][index].mutex);
    if (s_PDP[socket_id][index].state == PDP_BUSY) {
        s_PDP[socket_id][index].state = PDP_IDLE;
    }
    pthread_mutex_unlock(&s_PDP[socket_id][index].mutex);
}

void putUnusablePDPCid(RIL_SOCKET_ID socket_id) {
    int i = 0;
    for (; i < MAX_PDP; i++) {
        pthread_mutex_lock(&s_PDP[socket_id][i].mutex);
        if (s_PDP[socket_id][i].cid == UNUSABLE_CID) {
            RLOGD("putUnusablePDPCid cid = %d", i + 1);
            s_PDP[socket_id][i].cid = -1;
        }
        pthread_mutex_unlock(&s_PDP[socket_id][i].mutex);
    }
}

int updatePDPCid(RIL_SOCKET_ID socket_id, int cid, int state) {
    int index = cid - 1;
    if (cid <= 0 || cid > MAX_PDP) {
        return 0;
    }
    pthread_mutex_lock(&s_PDP[socket_id][index].mutex);
    if (state == 1) {
        s_PDP[socket_id][index].cid = cid;
    } else if (state == 0) {
        s_PDP[socket_id][index].cid = -1;
    } else if (state == -1 && s_PDP[socket_id][index].cid == -1) {
        s_PDP[socket_id][index].cid = UNUSABLE_CID;
    }
    pthread_mutex_unlock(&s_PDP[socket_id][index].mutex);
    return 1;
}

int getPDPCid(RIL_SOCKET_ID socket_id, int index) {
    if (index >= MAX_PDP || index < 0) {
        return -1;
    } else {
        return s_PDP[socket_id][index].cid;
    }
}

enum PDPState getPDPState(RIL_SOCKET_ID socket_id, int index) {
    if (index >= MAX_PDP || index < 0) {
        return PDP_IDLE;
    } else {
        return s_PDP[socket_id][index].state;
    }
}

int getFallbackCid(RIL_SOCKET_ID socket_id, int index) {
    if (index >= MAX_PDP || index < 0) {
        return -1;
    } else {
        return s_PDP[socket_id][index].secondary_cid;
    }
}

int setPDPMapping(RIL_SOCKET_ID socket_id, int primary, int secondary) {
    RLOGD("setPDPMapping primary %d, secondary %d", primary, secondary);
    if (primary < 0 || primary >= MAX_PDP || secondary < 0 ||
        secondary >= MAX_PDP) {
        return 0;
    }
    pthread_mutex_lock(&s_PDP[socket_id][primary].mutex);
    s_PDP[socket_id][primary].cid = primary + 1;
    s_PDP[socket_id][primary].secondary_cid = secondary + 1;
    s_PDP[socket_id][primary].isPrimary = true;
    pthread_mutex_unlock(&s_PDP[socket_id][primary].mutex);

    pthread_mutex_lock(&s_PDP[socket_id][secondary].mutex);
    s_PDP[socket_id][secondary].cid = secondary + 1;
    s_PDP[socket_id][secondary].secondary_cid = primary + 1;
    s_PDP[socket_id][secondary].isPrimary = false;
    pthread_mutex_unlock(&s_PDP[socket_id][secondary].mutex);
    return 1;
}

int isExistActivePdp(RIL_SOCKET_ID socket_id) {
    int cid;
    for (cid = 0; cid < MAX_PDP; cid++) {
        pthread_mutex_lock(&s_PDP[socket_id][cid].mutex);
        if (s_PDP[socket_id][cid].state == PDP_BUSY) {
            pthread_mutex_unlock(&s_PDP[socket_id][cid].mutex);
            RLOGD("PDP[0].state = %d, PDP[1].state = %d, PDP[2].state = %d",
                    s_PDP[socket_id][0].state, s_PDP[socket_id][1].state, s_PDP[socket_id][2].state);
            RLOGD("PDP[%d] is busy now", cid);
            return 1;
        }
        pthread_mutex_unlock(&s_PDP[socket_id][cid].mutex);
    }

    return 0;
}

static bool isBoots() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get(IS_BOOTS, prop, "0");
    RLOGD("isBoots: prop = %s", prop);
    if (strcmp(prop, "0") == 0) {
        return false;
    }
    return true;
}

static void convertFailCause(RIL_SOCKET_ID socket_id, int cause) {
    int failCause = cause;

    switch (failCause) {
        case MN_GPRS_ERR_NO_SATISFIED_RESOURCE:
        case MN_GPRS_ERR_INSUFF_RESOURCE:
        case MN_GPRS_ERR_MEM_ALLOC:
        case MN_GPRS_ERR_LLC_SND_FAILURE:
        case MN_GPRS_ERR_OPERATION_NOT_ALLOWED:
        case MN_GPRS_ERR_SPACE_NOT_ENOUGH:
        case MN_GPRS_ERR_TEMPORARILY_BLOCKED:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_INSUFFICIENT_RESOURCES;
            break;
        case MN_GPRS_ERR_SERVICE_OPTION_OUTOF_ORDER:
        case MN_GPRS_ERR_OUT_OF_ORDER_SERVICE_OPTION:
            s_lastPDPFailCause[socket_id] =
                    PDP_FAIL_SERVICE_OPTION_OUT_OF_ORDER;
            break;
        case MN_GPRS_ERR_PDP_AUTHENTICATION_FAILED:
        case MN_GPRS_ERR_AUTHENTICATION_FAILURE:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_USER_AUTHENTICATION;
            break;
        case MN_GPRS_ERR_NO_NSAPI:
        case MN_GPRS_ERR_PDP_TYPE:
        case MN_GPRS_ERR_PDP_ID:
        case MN_GPRS_ERR_NSAPI:
        case MN_GPRS_ERR_UNKNOWN_PDP_ADDR_OR_TYPE:
        case MN_GPRS_ERR_INVALID_TI:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_UNKNOWN_PDP_ADDRESS_TYPE;
            break;
        case MN_GPRS_ERR_SERVICE_OPTION_NOT_SUPPORTED:
        case MN_GPRS_ERR_UNSUPPORTED_SERVICE_OPTION:
        case MN_GPRS_ERR_FEATURE_NOT_SUPPORTED:
        case MN_GPRS_ERR_QOS_NOT_ACCEPTED:
        case MN_GPRS_ERR_ATC_PARAM:
        case MN_GPRS_ERR_PERMENANT_PROBLEM:
        case MN_GPRS_ERR_READ_TYPE:
        case MN_GPRS_ERR_STARTUP_FAILURE:
            s_lastPDPFailCause[socket_id] =
                    PDP_FAIL_SERVICE_OPTION_NOT_SUPPORTED;
            break;
        case MN_GPRS_ERR_ACTIVE_REJCET:
        case MN_GPRS_ERR_REQUEST_SERVICE_OPTION_NOT_SUBSCRIBED:
        case MN_GPRS_ERR_UNSUBSCRIBED_SERVICE_OPTION:
            s_lastPDPFailCause[socket_id] =
                    PDP_FAIL_SERVICE_OPTION_NOT_SUBSCRIBED;
            break;
        case MN_GPRS_ERR_ACTIVATION_REJ_GGSN:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ACTIVATION_REJECT_GGSN;
            break;
        case MN_GPRS_ERR_ACTIVATION_REJ:
        case MN_GPRS_ERR_MODIFY_REJ:
        case MN_GPRS_ERR_SM_ERR_UNSPECIFIED:
            s_lastPDPFailCause[socket_id] =
                    PDP_FAIL_ACTIVATION_REJECT_UNSPECIFIED;
            break;
        case MN_GPRS_ERR_MISSING_OR_UNKOWN_APN:
        case MN_GPRS_ERR_UNKNOWN_APN:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_MISSING_UKNOWN_APN;
            break;
        case MN_GPRS_ERR_SAME_PDP_CONTEXT:
        case MN_GPRS_ERR_NSAPI_ALREADY_USED:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_NSAPI_IN_USE;
            break;
        case MN_GPRS_ERR_OPERATOR_DETERMINE_BAR:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_OPERATOR_BARRED;
            break;
        case MN_GPRS_ERR_INCORRECT_MSG:
        case MN_GPRS_ERR_SYNTACTICAL_ERROR_IN_TFT_OP:
        case MN_GPRS_ERR_SEMANTIC_ERROR_IN_PACKET_FILTER:
        case MN_GPRS_ERR_SYNTAX_ERROR_IN_PACKET_FILTER:
        case MN_GPRS_ERR_PDP_CONTEXT_WO_TFT_ALREADY_ACT:
        case MN_GPRS_ERR_CONTEXT_CAUSE_CONDITIONAL_IE_ERROR:
        case MN_GPRS_ERR_UNIMPLE_MSG_TYPE:
        case MN_GPRS_ERR_UNIMPLE_IE:
        case MN_GPRS_ERR_INCOMP_MSG_PROTO_STAT:
        case MN_GPRS_ERR_SEMANTIC_ERROR_IN_TFT_OP:
        case MN_GPRS_ERR_INCOMPAT_MSG_TYP_PROTO_STAT:
        case MN_GPRS_ERR_UNKNOWN_PDP_CONTEXT:
        case MN_GPRS_ERR_NO_PDP_CONTEXT:
        case MN_GPRS_ERR_PDP_CONTEXT_ACTIVATED:
        case MN_GPRS_ERR_INVALID_MAND_INFO:
        case MN_GPRS_ERR_PRIMITIVE:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_PROTOCOL_ERRORS;
            break;
        case MN_GPRS_ERR_SENDER:
        case MN_GPRS_ERR_RETRYING:
        case MN_GPRS_ERR_UNKNOWN_ERROR:
        case MN_GPRS_ERR_REGULAR_DEACTIVATION:
        case MN_GPRS_ERR_REACTIVATION_REQD:
        case MN_GPRS_ERR_UNSPECIFIED:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            break;
        case MN_GPRS_ERR_MAX_ACTIVE_PDP_REACHED:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_MAX_ACTIVE_PDP_CONTEXT_REACHED;
            break;
        case MN_GPRS_ERR_GENERAL_ERROR:
        case MN_GPRS_ERR_PGW_REJECT:
        case MN_GPRS_ERR_RESOURCE_UNAVAILABLE:
        case MN_GPRS_ERR_RECONNECT_NOT_ALLOWED:
        case MN_GPRS_ERR_EAPAKA_FAILURE:
        case MN_GPRS_ERR_NETWORK_NO_RSP:
        case MN_GPRS_ERR_PDN_ATTACH_ABORT:
        case MN_GPRS_ERR_INVALID_PDN_ATTACH_REQ:
        case MN_GPRS_ERR_PDN_REC_FAILURE:
        case MN_GPRS_ERR_MAIN_CONN_SETUP_FAILURE:
        case MN_GPRS_ERR_BEARER_RESOURCE_UNAVAILABLE:
        case MN_GPRS_ERR_EAPAKA_REJECT:
        case MN_GPRS_ERR_LCP_NEGO3_FAILURE:
        case MN_GPRS_ERR_TCH_SETUP_FAILURE:
        case MN_GPRS_ERR_NW_NO_RSP_IN_LCP:
        case MN_GPRS_ERR_NW_NO_RSP_INAUTH:
        case MN_GPRS_ERR_PDN_TIMEOUT:
        case MN_GPRS_ERR_DEFCONT_FAILURE:
        case MN_GPRS_ERR_DETACHED:
        case MN_GPRS_ERR_INTERNAL_FAILURE:
        case MN_GPRS_ERR_LOCAL_DETACH_DUE_TO_IRAT:
        case MN_GPRS_ERR_UNAUTHORIZED_APN:
            // need retry
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            break;
        case MN_GPRS_ERR_PDN_LIMIT_EXCEEDED:
        case MN_GPRS_ERR_NO_PGW_AVALIABLE:
        case MN_GPRS_ERR_PGW_UNREACHABLE:
        case MN_GPRS_ERR_INSUFFICIENT_PARAMETERS:
        case MN_GPRS_ERR_ADMIN_PROHIBITED:
        case MN_GPRS_ERR_PDNID_ALREADY_INUSED:
        case MN_GPRS_ERR_SUBSCRIPTION_LIMITATION:
        case MN_GPRS_ERR_PDNCONN_ALREADY_EXIST_FORPDN:
        case MN_GPRS_ERR_EMERGENCY_NOT_SUPPORTED:
        case MN_GPRS_ERR_RETRY_TMR_THROTTLING:
        case MN_GPRS_ERR_PDN_LIMIT_EXCEEDED_INUESIDE:
        case MN_GPRS_ERR_PDNID_ALREADY_INUSE_INSESIDE:
        case MN_GPRS_ERR_OP_ABORT_BY_USER:
        case MN_GPRS_ERR_RTT_DATA_CNNECTED:
        case MN_GPRS_ERR_ERR_ALREADY_IN_REQUEST_STATE:
        case MN_GPRS_ERR_POWER_DOWN:
            // no retry
            s_lastPDPFailCause[socket_id] = PDP_FAIL_PROTOCOL_ERRORS;
            break;
        default:
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            break;
    }
}

/**
 * return  -1: general failed;
 *          0: success;
 *          1: fail cause 288, need retry active;
 *          2: fail cause 128, need fall back;
 *          3: fail cause 253, need retry active for another cid;
 */
static int errorHandlingForCGDATA(RIL_SOCKET_ID socket_id, ATResponse *p_response,
                                  int err, int cid) {
    int failCause;
    int ret = DATA_ACTIVE_SUCCESS;
    char cmd[AT_COMMAND_LEN] = {0};
    char *line = NULL;

    if (err < 0 || p_response->success == 0) {
        ret = DATA_ACTIVE_FAILED;
        if (p_response != NULL &&
                strStartsWith(p_response->finalResponse, "+CME ERROR:")) {
            line = p_response->finalResponse;
            err = at_tok_start(&line);
            if (err >= 0) {
                err = at_tok_nextint(&line, &failCause);
                if (err >= 0) {
                    if (failCause == 288) {
                        ret = DATA_ACTIVE_NEED_RETRY;
                    } else if (failCause == 128 || (isBoots() && (failCause == 35 || failCause == 38))) {  // 128: network reject
                        ret = DATA_ACTIVE_NEED_FALLBACK;
                        if (isBoots()) {
                            if (failCause == 128){
                                s_lastPDPFailCause[socket_id] = PDP_FAIL_UNKNOWN_PDP_ADDRESS_TYPE;
                            } else if (failCause == 38 || failCause == 35){
                                s_lastPDPFailCause[socket_id] = PDP_FAIL_NETWORK_FAILURE;
                            }
                        }
                    } else if (failCause == 253) {
                        ret = DATA_ACTIVE_NEED_RETRY_FOR_ANOTHER_CID;
                    } else if (failCause == 26 || failCause == 47) {
                        ret = DATA_ACTIVE_NEED_RETRY_AFTER_DELAY_TIME;
                    } else {
                        convertFailCause(socket_id, failCause);
                    }
                } else {
                    s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
                }
            }
        } else {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
        }
        // when cgdata timeout then send deactive to modem
        if (err == AT_ERROR_TIMEOUT || (p_response != NULL &&
                strStartsWith(p_response->finalResponse, "ERROR"))) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", cid);
            at_send_command(socket_id, cmd, NULL);
            cgact_deact_cmd_rsp(cid, socket_id);
        }
    }
    return ret;
}

static int getSPACTFBcause(RIL_SOCKET_ID socket_id) {
    int err = 0, cause = -1;
    char *line = NULL;
    ATResponse *p_response = NULL;

    err = at_send_command_singleline(socket_id, "AT+SPACTFB?", "+SPACTFB:",
                                     &p_response);
    if (err < 0 || p_response->success == 0) {
        s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
    } else {
        line = p_response->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &cause);
        if (err < 0) goto error;
    }
error:
    at_response_free(p_response);
    return cause;
}

static void queryAllActivePDNInfos(RIL_SOCKET_ID socket_id) {
    int err = 0;
    int active;
    char *line = NULL;
    ATLine *pCur = NULL;
    PDNInfo *pdns = s_PDN;
    ATResponse *pdnResponse = NULL;

    s_activePDN = 0;
    err = at_send_command_multiline(socket_id, "AT+SPIPCONTEXT?",
                                    "+SPIPCONTEXT:", &pdnResponse);
    if (err < 0 || pdnResponse->success == 0) goto done;
    for (pCur = pdnResponse->p_intermediates; pCur != NULL;
         pCur = pCur->p_next) {
        int i, cid;
        int type;
        char *apn;
        char *attachApn = "";
        line = pCur->line;
        err = at_tok_start(&line);
        if (err < 0) {
            pdns->nCid = -1;
        }
        err = at_tok_nextint(&line, &pdns->nCid);
        if (err < 0) {
            pdns->nCid = -1;
        }
        cid = pdns->nCid;
        i = cid - 1;
        if (pdns->nCid > MAX_PDP || i < 0) {
            continue;
        }
        err = at_tok_nextint(&line, &active);
        if (err < 0 || active == 0) {
            pdns->nCid = -1;
        }
        if (active == 1) {
            s_activePDN++;
        }
        /* apn */
        err = at_tok_nextstr(&line, &apn);
        if (err < 0) {
            s_PDN[i].nCid = -1;
        }
        snprintf(s_PDN[i].strApn, sizeof(s_PDN[i].strApn), "%s", apn);
        /* type */
        err = at_tok_nextint(&line, &type);
        if (err < 0) {
            s_PDN[i].nCid = -1;
        }
        char *strType = NULL;
        switch (type) {
            case IPV4:
                strType = "IP";
                break;
            case IPV6:
                strType = "IPV6";
                break;
            case IPV4V6:
                strType = "IPV4V6";
                break;
            default:
                strType = "IP";
                break;
        }
        snprintf(s_PDN[cid - 1].strIPType, sizeof(s_PDN[i].strIPType), "%s",
                 strType);
        if (at_tok_hasmore(&line)) {
            at_tok_nextstr(&line, &attachApn);
        }
        snprintf(s_PDN[i].strAttachApn, sizeof(s_PDN[i].strAttachApn), "%s",
                 attachApn);

        if (active > 0) {
            RLOGI("active PDN: cid = %d, ipType = %s, apn = %s, attachApn = %s",
                  s_PDN[i].nCid, s_PDN[i].strIPType,
                  s_PDN[i].strApn, s_PDN[i].strAttachApn);
        }
        pdns++;
    }
done:
    at_response_free(pdnResponse);
}

int getPDNCid(int index) {
    if (index >= MAX_PDP_CP || index < 0) {
        return -1;
    } else {
        return s_PDN[index].nCid;
    }
}

char *getPDNIPType(int index) {
    if (index >= MAX_PDP_CP || index < 0) {
        return NULL;
    } else {
        return s_PDN[index].strIPType;
    }
}

char *getPDNAPN(int index) {
    if (index >= MAX_PDP_CP || index < 0) {
        return NULL;
    } else {
        return s_PDN[index].strApn;
    }
}

char *getPDNAttachAPN(int index) {
    if (index >= MAX_PDP_CP || index < 0) {
        return NULL;
    } else {
        return s_PDN[index].strAttachApn;
    }
}

static int checkCmpAnchor(char *apn) {
    if (apn == NULL || strlen(apn) == 0) {
        return 0;
    }

    const int len = strlen(apn);
    char strApn[ARRAY_SIZE] = {0};
    char tmp[ARRAY_SIZE] = {0};
    static char *str[] = {".GPRS", ".MCC", ".MNC"};

    // if the length of apn is less than "mncxxx.mccxxx.gprs",
    // we would not continue to check.
    if (len <= MINIMUM_APN_LEN) {
        return len;
    }

    snprintf(strApn, sizeof(strApn), "%s", apn);
    RLOGD("getOrgApnlen: apn = %s, strApn = %s, len = %d", apn, strApn, len);

    memset(tmp, 0, sizeof(tmp));
    strncpy(tmp, apn + (len - 5), 5);
//    RLOGD("getOrgApnlen: tmp = %s", tmp);
    if (strcasecmp(str[0], tmp)) {
        return len;
    }
    memset(tmp, 0, sizeof(tmp));

    strncpy(tmp, apn + (len - 12), strlen(str[1]));
//    RLOGD("getOrgApnlen: tmp = %s", tmp);
    if (strcasecmp(str[1], tmp)) {
        return len;
    }
    memset(tmp, 0, sizeof(tmp));

    strncpy(tmp, apn + (len - MINIMUM_APN_LEN), strlen(str[2]));
//    RLOGD("getOrgApnlen: tmp = %s", tmp);
    if (strcasecmp(str[2], tmp)) {
        return len;
    }
    return (len - MINIMUM_APN_LEN);
}

static bool isAttachEnable() {
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get(ATTACH_ENABLE_PROP, prop, "true");
    RLOGD("isAttachEnable: prop = %s", prop);
    if (!strcmp(prop, "false")) {
        return false;
    }
    return true;
}

static void cleanCid(int index, bool needDeactive, RIL_SOCKET_ID socket_id) {
    char cmd[AT_COMMAND_LEN] = {0};
    int cid = index + 1;

    cgact_deact_cmd_rsp(cid, socket_id);
    if (needDeactive) {
        snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", cid);
        at_send_command(socket_id, cmd, NULL);
    }
}

/*
 * return : -2: Active Cid success,but isnt fall back cid ip type;
 *          -1: Active Cid failed;
 *           0: Active Cid success;
 *           1: Active Cid failed, error 288, need retry;
 *           2: Active Cid failed, error 128, need do fall back;
 */
static int activeSpeciedCidProcess(RIL_SOCKET_ID socket_id, void *data, int cid,
                                   const char *pdp_type, int primaryCid) {
    int err;
    int ret = -1;
    int ethId = -1;
    IPType ipType = UNKNOWN;
    char cmd[AT_COMMAND_LEN] = {0};
    char ethIdCmd[AT_COMMAND_LEN] = {0};
    char newCmd[AT_COMMAND_LEN] = {0};
    char qosState[PROPERTY_VALUE_MAX] = {0};
    char qosSduErrorRatio[PROPERTY_VALUE_MAX] = {0};
    char qosResidualBitErrorRatio[PROPERTY_VALUE_MAX] = {0};
    const char *apn = NULL, *username = NULL, *password = NULL, *authtype = NULL;

    ATResponse *p_response = NULL;

    apn = ((const char **)data)[2];
    username = ((const char **)data)[3];
    password = ((const char **)data)[4];
    authtype = ((const char **)data)[5];

    if (!strcmp(pdp_type, "IPV4+IPV6")) {
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"IP\",\"%s\",\"\",0,0",
                  cid, apn);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"%s\",\"%s\",\"\",0,0",
                  cid, pdp_type, apn);
    }
    err = cgdcont_set_cmd_req(cmd, newCmd);
    if (err == 0) {
        err = at_send_command(socket_id, newCmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            putPDP(socket_id, cid - 1);
            at_response_free(p_response);
            return ret;
        }
        AT_RESPONSE_FREE(p_response);
    }

    snprintf(cmd, sizeof(cmd), "AT+CGPCO=0,\"%s\",\"%s\",%d,%d", username,
              password, cid, atoi(authtype));
    at_send_command(socket_id, cmd, NULL);
    /* Set required QoS params to default */
    property_get(ENG_QOS_PROP, qosState, "0");
    property_get(QOS_SDU_ERROR_RATIO, qosSduErrorRatio, "1e4");
    property_get(QOS_RESIDUAL_BIT_ERROR_RATIO, qosResidualBitErrorRatio, "0e0");
    if (!strcmp(qosState, "0")) {
        snprintf(cmd, sizeof(cmd),
                  "AT+CGEQREQ=%d,%d,0,0,0,0,2,0,\"%s\",\"%s\",3,0,0",
                  cid, s_trafficClass[socket_id], qosSduErrorRatio, qosResidualBitErrorRatio);
        at_send_command(socket_id, cmd, NULL);
    }

    if (primaryCid > 0) {
        ethId = getEthIndexBySocketId(socket_id, primaryCid);
        snprintf(cmd, sizeof(cmd), "AT+CGDATA=\"M-ETHER\",%d, %d", cid,
                  primaryCid);
    } else {
        ethId = getEthIndexBySocketId(socket_id, cid);
        snprintf(cmd, sizeof(cmd), "AT+CGDATA=\"M-ETHER\",%d", cid);
    }

    snprintf(ethIdCmd, sizeof(ethIdCmd), "AT+SPAPNETID=%d,%d", cid, ethId);

    cgdata_set_cmd_req(cmd);

    at_send_command(socket_id, ethIdCmd, NULL);

    err = at_send_command(socket_id, cmd, &p_response);
    s_curCid[socket_id] = cid;
    cgdata_set_cmd_rsp(p_response, cid - 1, primaryCid, socket_id);
    ret = errorHandlingForCGDATA(socket_id, p_response, err, cid);
    AT_RESPONSE_FREE(p_response);
    if (ret != DATA_ACTIVE_SUCCESS) {
        putPDP(socket_id, cid - 1);
        return ret;
    }

    if (primaryCid > 0) {
        /* Check ip type after fall back  */
        ipType = pdp_info[primaryCid - 1].ip_state;
        RLOGD("Fallback 2 s_PDP: fb_ip = %d", ipType);
        if (ipType != IPV4V6) {
            RLOGD("Fallback s_PDP type mismatch, do deactive");
            cleanCid(cid - 1, true, socket_id);
            putPDP(socket_id, cid - 1);
            ret = DATA_ACTIVE_FALLBACK_FAILED;
        } else {
            setPDPMapping(socket_id, primaryCid - 1, cid - 1);
            ret = DATA_ACTIVE_SUCCESS;
        }
    } else {
        updatePDPCid(socket_id, cid, 1);
        ret = DATA_ACTIVE_SUCCESS;
    }
    s_trafficClass[socket_id] = TRAFFIC_CLASS_DEFAULT;
    return ret;
}

/*
 * return  NULL :  Dont need fallback
 *         other:  FallBack s_PDP type
 */
static const char *checkFallBackType(RIL_SOCKET_ID socket_id, const char *pdp_type,
                                     int cidIndex) {
    int fbCause = 0;
    char *ret = NULL;
    char prop_PdpEnableFallbackCause[PROPERTY_VALUE_MAX] = {0};
    char prop_fallbackCause[PROPERTY_VALUE_MAX] = {0};

    /* Check if need fall back or not */
    int ipType = pdp_info[cidIndex].ip_state;

    char carrier[PROPERTY_VALUE_MAX];
    bool isDualpdpAllowed = false;
    memset(carrier, 0, sizeof(carrier));
    property_get(OVERSEA_VERSION, carrier, "unknown");
    RLOGD("checkFallBackType ro.carrier = %s", carrier);

    if (!strcmp(carrier, "claro") || !strcmp(carrier, "telcel")) {
        isDualpdpAllowed = true;
    }

    if (!strcmp(pdp_type, "IPV4V6") && ipType != IPV4V6) {
        fbCause = getSPACTFBcause(socket_id);
        property_get(ENABLE_SPECIAL_FALLBACK_CAUSE, prop_PdpEnableFallbackCause, "0");
        property_get(SPECIAL_FALLBACK_CAUSE, prop_fallbackCause, "-1");
        RLOGD("fallback cause reported is %d, prop_PdpEnableFallbackCause = %s, prop_fallbackCause = %s",
                fbCause, prop_PdpEnableFallbackCause, prop_fallbackCause);
        if ((fbCause == 52 ||
                (strcmp(prop_PdpEnableFallbackCause, "1") == 0 && fbCause == atoi(prop_fallbackCause))) &&
                (!isDualpdpAllowed || cidIndex == 0)) {
            if (ipType == IPV4) {
                ret = "IPV6";
            } else if (ipType == IPV6) {
                ret = "IP";
            }
        }
    }
    return ret;
}

/*bug1027382 DUT fails to access ipv6 address*/
static void checkFallBack(RIL_SOCKET_ID socket_id, void *data,
                          const char *pdp_type, int cidIndex) {
    const char *tmpType = NULL;
    int isFallback = 0;
    int index = -1;

    /* Check if need fall back or not */
    if (pdp_type != NULL && !strcmp(pdp_type, "IPV4+IPV6")) {
        tmpType = "IPV6";
    } else if (pdp_type != NULL && !strcmp(pdp_type, "IPV4V6")) {
        tmpType = checkFallBackType(socket_id, pdp_type, cidIndex);
        isFallback = 1;
    }
    if (tmpType == NULL) {  // don't need fallback
        return ;
    }
    index = getPDP(socket_id);
    if (index < 0 || getPDPCid(socket_id, index) >= 0) {
        /* just use actived IP */
        return ;
    }
    if (isFallback == 1) {
        activeSpeciedCidProcess(socket_id, data, index + 1, tmpType,
                cidIndex + 1);
    } else {  // IPV4+IPV6
        activeSpeciedCidProcess(socket_id, data, index+1, tmpType, 0);
    }
}

static bool doIPV4_IPV6_Fallback(RIL_SOCKET_ID socket_id, int index, void *data) {
    if (isBoots()) {
        int err = 0;
        char cmd[AT_COMMAND_LEN] = {0};
        char newCmd[AT_COMMAND_LEN] = {0};
        char ethCmd[AT_COMMAND_LEN] = {0};
        const char *apn = NULL;
        bool iPV4Failed = false;
        bool iPV6Failed = false;
        int ipv4Cid = 0;
        ATResponse *p_response = NULL;

        apn = ((const char **)data)[2];

        // active IPV4
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"IP\",\"%s\",\"\",0,0",
                index + 1, apn);
        err = cgdcont_set_cmd_req(cmd, newCmd);
        if (err == 0) {
            err = at_send_command(socket_id, newCmd, &p_response);
        }
        if (err < 0 || p_response->success == 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            at_response_free(p_response);
            iPV4Failed = true;
            goto retryIPV6;
        }
        AT_RESPONSE_FREE(p_response);

        snprintf(ethCmd, sizeof(ethCmd), "AT+SPAPNETID=%d,%d", index + 1,
                getEthIndexBySocketId(socket_id, index + 1));
        at_send_command(socket_id, ethCmd, NULL);

        snprintf(cmd, sizeof(cmd), "AT+CGDATA=\"M-ETHER\",%d", index + 1);
        cgdata_set_cmd_req(cmd);
        err = at_send_command(socket_id, cmd, &p_response);
        cgdata_set_cmd_rsp(p_response, index, 0, socket_id);
        if (errorHandlingForCGDATA(socket_id, p_response, err,index) !=
                DATA_ACTIVE_SUCCESS) {
            at_response_free(p_response);
            iPV4Failed = true;
            goto retryIPV6;
        }
        ipv4Cid = index +1;
        updatePDPCid(socket_id, index + 1, 1);
retryIPV6:
        // active IPV6
        if(!iPV4Failed){
            index = getPDP(socket_id);
        }
        if (index < 0 || getPDPCid(socket_id, index) >= 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            putPDP(socket_id, index);
        } else {
            err = activeSpeciedCidProcess(socket_id, data, index + 1, "IPV6", ipv4Cid);
        }
        if (err != DATA_ACTIVE_SUCCESS){
            iPV6Failed = true;
        }
        RLOGD("doIPV4_IPV6_Fallback: iPV4Failed: %d  iPV6Failed: %d", iPV4Failed, iPV6Failed);
        at_response_free(p_response);
        return !iPV4Failed || !iPV6Failed;
    } else {
        bool ret = false;
        int err = 0;
        char cmd[AT_COMMAND_LEN] = {0};
        char newCmd[AT_COMMAND_LEN] = {0};
        char ethCmd[AT_COMMAND_LEN] = {0};
        const char *apn = NULL;
        ATResponse *p_response = NULL;

        apn = ((const char **)data)[2];

        // active IPV4
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"IP\",\"%s\",\"\",0,0",
                index + 1, apn);
        err = cgdcont_set_cmd_req(cmd, newCmd);
        if (err == 0) {
            err = at_send_command(socket_id, newCmd, &p_response);
        }
        if (err < 0 || p_response->success == 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            goto error;
        }
        AT_RESPONSE_FREE(p_response);

        snprintf(ethCmd, sizeof(ethCmd), "AT+SPAPNETID=%d,%d", index + 1,
                getEthIndexBySocketId(socket_id, index + 1));
        at_send_command(socket_id, ethCmd, NULL);

        snprintf(cmd, sizeof(cmd), "AT+CGDATA=\"M-ETHER\",%d", index + 1);

        cgdata_set_cmd_req(cmd);
        err = at_send_command(socket_id, cmd, &p_response);
        cgdata_set_cmd_rsp(p_response, index, 0, socket_id);
        if (errorHandlingForCGDATA(socket_id, p_response, err,index) !=
                DATA_ACTIVE_SUCCESS) {
            goto error;
        }

        updatePDPCid(socket_id, index + 1, 1);
        // active IPV6
        index = getPDP(socket_id);
        if (index < 0 || getPDPCid(socket_id, index) >= 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            putPDP(socket_id, index);
        } else {
            activeSpeciedCidProcess(socket_id, data, index + 1, "IPV6", 0);
        }
        ret = true;

    error:
        at_response_free(p_response);
        return ret;
    }
}

/*
 * check if IPv6 address is begain with FE80,if yes,return Ipv6 address begin with 0000
 */
void checkIpv6Address(char *oldIpv6Address, char *newIpv6Address, int len) {
    RLOGD("checkIpv6Address: old ipv6 address is: %s", oldIpv6Address);
    if (oldIpv6Address == NULL) {
        snprintf(newIpv6Address, len,"%s",
                "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF");
        return;
    }
    strncpy(newIpv6Address, oldIpv6Address, strlen(oldIpv6Address));
    if (strncasecmp(newIpv6Address, "fe80:", sizeof("fe80:") - 1) == 0) {
    char *temp = strchr(newIpv6Address, ':');
       if (temp != NULL) {
           snprintf(newIpv6Address, len, "0000%s", temp);
       }
    }
    RLOGD("checkIpv6Address: ipv6 address is: %s",newIpv6Address);
}

void setEth(RIL_SOCKET_ID socket_id, int cid) {
    int i;
    int ethIndex = -1;

    ethIndex = getEthIndexBySocketId(socket_id, cid);
    if (ethIndex >= 0 && ethIndex < MAX_ETH) {
        return;
    }

    for (i = 0; i< MAX_ETH; i++ ) {
        if (s_ethState[i] == ETH_IDLE) {
            RLOGD("set ethIndex = %d for socket_id = %d cid = %d",
                    i, socket_id, cid);
            s_ethState[i] = ETH_BUSY;
            s_cidForEth[i].socketId = socket_id;
            s_cidForEth[i].cid = cid;
            return;
        }
    }

    RLOGD("All network interface are busy");
}

int getEthIndexBySocketId(RIL_SOCKET_ID socket_id, int cid) {
    int i;
    int ret = -1;

    for (i = 0; i < MAX_ETH; i++) {
        if (s_cidForEth[i].socketId == (int) socket_id &&
                s_cidForEth[i].cid == cid &&
                s_ethState[i] == ETH_BUSY) {
            RLOGD("ethIndex = %d is already busy for socket_id = %d cid = %d ",
                    i, socket_id, cid);
            ret = i;
            return ret;
        }
    }

    RLOGD("getEthIndexBySocketId -1");
    return ret;
}

void cleanEth(RIL_SOCKET_ID socket_id, int cid) {
    int ethIndex = getEthIndexBySocketId(socket_id, cid);

    if (ethIndex < 0 || ethIndex >= MAX_ETH ||
            cid < 0 || cid >= MAX_PDP) {
        RLOGD("do not need to clean ethIndex = %d", ethIndex);
        return;
    }

    RLOGD("cleanEth: ethIndex = %d", ethIndex);
    s_ethState[ethIndex] = ETH_IDLE;
    s_cidForEth[ethIndex].socketId = -1;
    s_cidForEth[ethIndex].cid = -1;
}

int getCidByEthIndex(RIL_SOCKET_ID socket_id, int ethIndex) {
    int ret = -1;

    if (ethIndex >= 0 && ethIndex < MAX_ETH) {
        if (s_cidForEth[ethIndex].socketId == (int) socket_id) {
            ret = s_cidForEth[ethIndex].cid;
        }
    }
    return ret;
}

void getEthNameByCid(RIL_SOCKET_ID socket_id, int cid, char *ethName, size_t len) {
    if (ethName == NULL) {
        RLOGE("ethName is null");
        return;
    }

    char eth[PROPERTY_VALUE_MAX] = {0};
    int ethIndex = getEthIndexBySocketId(socket_id, cid);

    property_get(MODEM_ETH_PROP, eth, "veth");
    snprintf(ethName, len, "%s%d", eth, ethIndex);

    RLOGD("getEthNameByCid: socket_id = %d, cid = %d, ethName = %s",
            socket_id, cid, ethName);
}

int getCidByEthName(RIL_SOCKET_ID socket_id, char *ethName, size_t len) {
    int cid = -1;
    int ethIndex = 0;
    char eth[PROPERTY_VALUE_MAX] = {0};
    char str[ARRAY_SIZE / 4] = {0};
    property_get(MODEM_ETH_PROP, eth, "veth");

    if (ethName != NULL && len > strlen(eth)) {
        snprintf(str, sizeof(str), "%s", ethName + strlen(eth));
        ethIndex = atoi(str);
        cid = getCidByEthIndex(socket_id, ethIndex);
    }
    RLOGD("getCidByEthName: socket_id = %d, cid = %d, ethName = %s",
            socket_id, cid, ethName);

    return cid;
}

void sendPsDataOffToExtData(RIL_SOCKET_ID socket_id, int exemptionInfo, int port) {
    char cmd[AT_COMMAND_LEN] = {0};
    if (exemptionInfo == -1) {
        snprintf(cmd, sizeof(cmd), "ext_data<dataOffDisable>%d;%d",
                socket_id, port);
    } else {
        snprintf(cmd, sizeof(cmd), "ext_data<dataOffEnable>%d;%d",
                socket_id, port);
    }
    RLOGD("sendPsDataOffToExtData: socket_id = %d, cmd = %s",
                socket_id, cmd);
    sendCmdToExtData(cmd);
}

static void requestOrSendDataCallList(RIL_SOCKET_ID socket_id, int cid,
                                      RIL_Token *t) {
    int err;
    int i = 0;
    int n = 15;
    char *out = NULL;
    char *line = NULL;
    char eth[PROPERTY_VALUE_MAX] = {0};
    ATLine *p_cur = NULL;
    ATResponse *p_response = NULL;
    ATResponse *p_newResponse = NULL;
    RIL_SetupDataCallResult_v1_4 *responses = NULL;
    RIL_SetupDataCallResult_v1_4 *response = NULL;
    DataCallListType type = UNSOLICTED_DATA_CALL;

    if (t == NULL) {
        if (cid <= 0) {
            RLOGD("requestOrSendDataCallList with invalid cid!");
            return;
        } else {
            type = UNSOLICTED_DATA_CALL;
        }
    } else {
        if (cid <= 0) {
            type = GET_DATA_CALL;
        } else {
            type = SETUP_DATA_CALL;
        }
    }
    RLOGD("requestOrSendDataCallList, cid: %d, type: %d", cid, type);
    err = at_send_command_multiline(socket_id, "AT+CGACT?", "+CGACT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL) {
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0,
                                      socket_id);
        }
        at_response_free(p_response);
        return;
    }

    responses = alloca(n * sizeof(RIL_SetupDataCallResult_v1_4));
    for (i = 0; i < n; i++) {
        responses[i].cause = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = PDP_PROTOCOL_TYPE_UNKNOWN;
        responses[i].ifname = "";
        responses[i].addressesNumber = 0;
        responses[i].addresses = NULL;
        responses[i].dnsesNumber = 0;
        responses[i].dnses = NULL;
        responses[i].gatewaysNumber = 0;
        responses[i].gateways = NULL;
        responses[i].pcscfNumber = 0;
        responses[i].pcscf = NULL;
        responses[i].mtu = 0;
    }
    response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        int tmpCid = -1;
        int state = -1;

        line = p_cur->line;
        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &tmpCid);
        if (err < 0) goto error;

        if (tmpCid < 1 || tmpCid > n) continue;
        err = at_tok_nextint(&line, &state);
        if (err < 0) goto error;

        response[tmpCid - 1].cid = tmpCid;
        response[tmpCid - 1].active = (RIL_DataConnActiveStatus)state;
    }
    AT_RESPONSE_FREE(p_response);

    err = at_send_command_multiline(socket_id, "AT+CGDCONT?",
                                    "+CGDCONT:", &p_response);
    if (err != 0 || p_response->success == 0) {
        if (t != NULL) {
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0,
                                      socket_id);
        }
        at_response_free(p_response);
        return;
    }
    cgdcont_read_cmd_rsp(p_response, &p_newResponse);
    for (p_cur = p_newResponse->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char *line = p_cur->line;
        int ncid;
        int nn;
        IPType ipType = UNKNOWN;
        char cmd[AT_COMMAND_LEN] = {0};
        char prop[PROPERTY_VALUE_MAX] = {0};

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &ncid);
        if (err < 0) goto error;

        if (type == SETUP_DATA_CALL && ncid != cid) {
            continue;
        }
        i = ncid - 1;

        if (i >= MAX_PDP) {
            /* details for a context we didn't hear about in the last request */
            continue;
        }
        /* Assume no error */
        responses[i].cause = PDP_FAIL_NONE;

        /* type */
        err = at_tok_nextstr(&line, &out);
        if (err < 0) goto error;

        /* APN ignored for v5 */
        err = at_tok_nextstr(&line, &out);
        if (err < 0) goto error;

        property_get(MODEM_ETH_PROP, eth, "veth");

        int ethIndex = getEthIndexBySocketId(socket_id, ncid);

        snprintf(cmd, sizeof(cmd), "%s%d", eth, ethIndex);
        responses[i].ifname = alloca(strlen(cmd) + 1);
        snprintf(responses[i].ifname, strlen(cmd) + 1, "%s", cmd);

        snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ip_type", eth, ethIndex);
        property_get(cmd, prop, "0");
        ipType = atoi(prop);
        if (responses[i].active > 0) {
            RLOGD("prop = %s, ipType = %d", prop, ipType);
        }

        if (ipType == IPV4) {
            responses[i].type = PDP_PROTOCOL_TYPE_IP;
            responses[i].addressesNumber = 1;
            responses[i].gatewaysNumber = 1;
            responses[i].dnsesNumber = 2;
            responses[i].addresses = (char **)alloca(responses[i].addressesNumber * sizeof(char *));
            responses[i].gateways = (char **)alloca(responses[i].gatewaysNumber * sizeof(char *));
            responses[i].dnses = (char **)alloca(responses[i].dnsesNumber * sizeof(char *));

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ip", eth, ethIndex);
            property_get(cmd, prop, NULL);
            RLOGD("IPV4 cmd=%s, prop = %s", cmd, prop);
            responses[i].addresses[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].addresses[0], strlen(prop) + 1, "%s", prop);

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.gw", eth, ethIndex);
            property_get(cmd, prop, "0.0.0.0");
            RLOGD("IPV4 cmd=%s, prop = %s", cmd, prop);
            responses[i].gateways[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].gateways[0], strlen(prop) + 1, "%s", prop);

            for (nn = 0; nn < 2; nn++) {
                snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.dns%d", eth, ethIndex, nn + 1);
                property_get(cmd, prop, NULL);
                responses[i].dnses[nn] = prop;
                responses[i].dnses[nn] = (char *)alloca(strlen(prop) + 1);
                snprintf(responses[i].dnses[nn], strlen(prop) + 1, "%s", prop);
            }
        } else if (ipType == IPV6) {
            responses[i].type = PDP_PROTOCOL_TYPE_IPV6;
            responses[i].addressesNumber = 1;
            responses[i].gatewaysNumber = 1;
            responses[i].dnsesNumber = 2;
            responses[i].addresses = (char **)alloca(responses[i].addressesNumber * sizeof(char *));
            responses[i].gateways = (char **)alloca(responses[i].gatewaysNumber * sizeof(char *));
            responses[i].dnses = (char **)alloca(responses[i].dnsesNumber * sizeof(char *));

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_ip", eth, ethIndex);
            property_get(cmd, prop, NULL);
            RLOGD("IPV6 cmd=%s, prop = %s", cmd, prop);
            responses[i].addresses[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].addresses[0], strlen(prop) + 1, "%s", prop);
            strtok(prop, "/");
            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_gw", eth, ethIndex);
            property_get(cmd, prop, "::0");
            RLOGD("IPV6 cmd=%s, prop = %s", cmd, prop);
            responses[i].gateways[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].gateways[0], strlen(prop) + 1, "%s", prop);

            for (nn = 0; nn < 2; nn++) {
                snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_dns%d", eth, ethIndex, nn + 1);
                property_get(cmd, prop, NULL);
                responses[i].dnses[nn] = (char *)alloca(strlen(prop) + 1);
                snprintf(responses[i].dnses[nn], strlen(prop) + 1, "%s", prop);
            }
        } else if (ipType == IPV4V6) {
            responses[i].type = PDP_PROTOCOL_TYPE_IPV4V6;
            responses[i].addressesNumber = 2;
            responses[i].gatewaysNumber = 2;
            responses[i].dnsesNumber = 4;
            responses[i].addresses = (char **)alloca(responses[i].addressesNumber * sizeof(char *));
            responses[i].gateways = (char **)alloca(responses[i].gatewaysNumber * sizeof(char *));
            responses[i].dnses = (char **)alloca(responses[i].dnsesNumber * sizeof(char *));

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ip", eth, ethIndex);
            property_get(cmd, prop, NULL);
            RLOGD("IPV4V6 cmd = %s, prop = %s", cmd, prop);
            responses[i].addresses[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].addresses[0], strlen(prop) + 1, "%s", prop);

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.gw", eth, ethIndex);
            property_get(cmd, prop, "0.0.0.0");
            RLOGD("IPV4V6 cmd = %s, prop = %s", cmd, prop);
            responses[i].gateways[0] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].gateways[0], strlen(prop) + 1, "%s", prop);

            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_ip", eth, ethIndex);
            property_get(cmd, prop, NULL);
            RLOGD("IPV4V6 cmd = %s, prop = %s", cmd, prop);
            responses[i].addresses[1] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].addresses[1], strlen(prop) + 1, "%s", prop);
            strtok(prop, "/");
            snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_gw", eth, ethIndex);
            property_get(cmd, prop, "::0");
            RLOGD("IPV4V6 cmd = %s, prop = %s", cmd, prop);
            responses[i].gateways[1] = (char *)alloca(strlen(prop) + 1);
            snprintf(responses[i].gateways[1], strlen(prop) + 1, "%s", prop);

            for (nn = 0; nn < 2; nn++) {
                snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.dns%d", eth, ethIndex, nn + 1);
                property_get(cmd, prop, NULL);
                RLOGD("IPV4V6 cmd = %s, prop = %s", cmd, prop);
                responses[i].dnses[nn] = (char *)alloca(strlen(prop) + 1);
                snprintf(responses[i].dnses[nn], strlen(prop) + 1, "%s", prop);

            }
            for (nn = 0; nn < 2; nn++) {
                snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ipv6_dns%d", eth,
                        ethIndex, nn + 1);
                property_get(cmd, prop, NULL);
                RLOGD("IPV4V6 cmd=%s, prop = %s", cmd, prop);
                responses[i].dnses[nn + 2] = (char *)alloca(strlen(prop) + 1);
                snprintf(responses[i].dnses[nn + 2], strlen(prop) + 1, "%s", prop);
            }
        } else {
            if (responses[i].active > 0) {
                RLOGE("Unknown IP type!");
            }
        }

        if (type == UNSOLICTED_DATA_CALL) {
            if ((cid == i + 1) || ((!responses[i].active) &&
                    responses[i].addresses != NULL &&
                    responses[i].addresses[0] != NULL &&
                    strcmp(responses[i].addresses[0], ""))) {
                responses[i].cause = PDP_FAIL_OPERATOR_BARRED;
                responses[i].type = PDP_PROTOCOL_TYPE_UNKNOWN;
                responses[i].addressesNumber = 0;
                responses[i].gatewaysNumber = 0;
                responses[i].dnsesNumber = 0;
                responses[i].addresses = NULL;
                responses[i].gateways = NULL;
                responses[i].dnses = NULL;
            }
          }

        if (responses[i].active > 0) {
            RLOGD("status = %d", responses[i].cause);
            RLOGD("suggestedRetryTime = %d", responses[i].suggestedRetryTime);
            RLOGD("cid = %d", responses[i].cid);
            RLOGD("active = %d", responses[i].active);
            RLOGD("type = %d", responses[i].type);
            RLOGD("ifname = %s", responses[i].ifname);
            for (uint32_t j = 0; j < responses[i].addressesNumber; j++) {
                RLOGD("address[%d] = %s", j, responses[i].addresses[j]);
            }
            for (uint32_t j = 0; j < responses[i].gatewaysNumber; j++) {
                RLOGD("gateways[%d] = %s", j, responses[i].gateways[j]);
            }
            for (uint32_t j = 0; j < responses[i].dnsesNumber; j++) {
                RLOGD("dns[%d] = %s", j, responses[i].dnses[j]);
            }
        }
    }

    AT_RESPONSE_FREE(p_response);
    AT_RESPONSE_FREE(p_newResponse);

    if (type == SETUP_DATA_CALL) {
        RLOGD("requestOrSendDataCallList is called by SetupDataCall!");
        bool success = false;
        i = cid - 1;
        if (responses[i].cid == cid) {
            RLOGD("SetupDataCall cid: %d", cid);
            if (responses[i].active) {
                if (s_LTEDetached[socket_id]) {
                    RLOGD("Lte detached in the past2.");
                    success = false;
                } else {
                    success = true;
                }
            }
        }
        if (success) {
            RIL_onRequestComplete(*t, RIL_E_SUCCESS, &responses[i],
                    sizeof(RIL_SetupDataCallResult_v1_4));
        } else {
            int fb_cid = getFallbackCid(socket_id, i);  // pdp fallback cid
            RLOGD("SetupDataCall fail fallback cid : %d", fb_cid);
            putPDP(socket_id, fb_cid -1);
            putPDP(socket_id, i);
            s_lastPDPFailCause[socket_id] =
                    PDP_FAIL_ERROR_UNSPECIFIED;

            cleanCid(i, true, socket_id);
            cleanEth(socket_id, cid);

#if (SIM_COUNT == 2)
            if (s_dataAllowed[socket_id] == 0 && (int)socket_id == s_ddsOnModem) {
                switchData(socket_id, false);
            }
#endif

            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        }
        s_LTEDetached[socket_id] = false;
        s_curCid[socket_id] = 0;
    } else if (type == GET_DATA_CALL) {
        RIL_onRequestComplete(*t, RIL_E_SUCCESS, responses,
                n * sizeof(RIL_SetupDataCallResult_v1_4));
    } else if (type == UNSOLICTED_DATA_CALL) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                responses, n * sizeof(RIL_SetupDataCallResult_v1_4), socket_id);
    }
    return;

error:
    if (t != NULL) {
        RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0,
                                  socket_id);
    }
    at_response_free(p_response);
    at_response_free(p_newResponse);
}

/*
 * return : -1: Dont reuse defaulte bearer;
 *           0: Reuse defaulte bearer success;
 *          >0: Reuse failed, the failed cid;
 */
static int getAPNMatchedCidIndex(void *data, RIL_SOCKET_ID socket_id) {
    queryAllActivePDNInfos(socket_id);
    int i, cid;
    char *apn = ((char **)data)[2];
    char *type = ((char **)data)[6];
    if (s_activePDN > 0) {
        for (i = 0; i < MAX_PDP; i++) {
            cid = getPDNCid(i);
            if (cid == (i + 1)) {
                RLOGD("s_PDP[%d].state = %d", i, getPDPState(socket_id, i));
                if (((isApnEqual(apn, getPDNAPN(i))
                        || !strcasecmp(apn, getPDNAttachAPN(i))) &&
                        isProtocolEqual(type, getPDNIPType(i))) ||
                        s_singlePDNAllowed[socket_id] == 1) {
                    return i;
                }
            }
        }
    }
    return -1;
}

static void requestSetupDataCall(RIL_SOCKET_ID socket_id, void *data,
                                 size_t datalen, RIL_Token t) {
    int index = -1;
    int nRetryTimes = 0;
    int nRetryDelayTimes = 0;
    int ret;
    const char *pdpType = "IP";
    RIL_SetupDataCallResult_v1_4 response;
    memset(&response, 0, sizeof(RIL_SetupDataCallResult_v1_4));

RETRY:
    if (datalen > 6 * sizeof(char *)) {
        pdpType = ((const char **)data)[6];
    } else {
        pdpType = "IP";
    }
    if (strcmp(pdpType, "IP") == 0) {
        s_pdpType = IPV4;
    } else if (strcmp(pdpType, "IPV6") == 0) {
        s_pdpType = IPV6;
    } else {
        s_pdpType = IPV4V6;
    }

    s_LTEDetached[socket_id] = false;
    index = getPDPByIndex(socket_id, getAPNMatchedCidIndex(data, socket_id));
    if (index >= 0) {
        cleanCid(index, false, socket_id);
    } else {
        index = getPDP(socket_id);
        if (index < 0 || getPDPCid(socket_id, index) >= 0) {
            s_lastPDPFailCause[socket_id] = PDP_FAIL_ERROR_UNSPECIFIED;
            goto error;
        }
        cleanCid(index, true, socket_id);
    }

    setEth(socket_id, index + 1);
    ret = activeSpeciedCidProcess(socket_id, data, index + 1, pdpType, 0);
    if (ret == DATA_ACTIVE_NEED_RETRY) {
        if (nRetryTimes < 5) {
            nRetryTimes++;
            goto RETRY;
        }
        goto error;
    } else if (ret == DATA_ACTIVE_NEED_RETRY_AFTER_DELAY_TIME &&
            s_dataAllowed[s_ddsOnModem] != 1) {
        if (nRetryDelayTimes < 6) {
            nRetryDelayTimes++;
            sleep(5);
            goto RETRY;
        }
        goto error;
    } else if ( ret == DATA_ACTIVE_NEED_FALLBACK) {
        if (doIPV4_IPV6_Fallback(socket_id, index, data) == false) {
            goto error;
        } else {
            goto done;
        }
    } else if (ret == DATA_ACTIVE_NEED_RETRY_FOR_ANOTHER_CID) {
        updatePDPCid(socket_id, index + 1, -1);
        goto RETRY;
    } else if (ret == DATA_ACTIVE_SUCCESS &&
            (!strcmp(pdpType, "IPV4V6") || !strcmp(pdpType, "IPV4+IPV6"))) {
        checkFallBack(socket_id, data, pdpType, index);
    } else if (ret != DATA_ACTIVE_SUCCESS) {
        goto error;
    }

done:
    putUnusablePDPCid(socket_id);
    pthread_mutex_lock(&s_signalBipPdpMutex);
    s_openchannelCid = index + 1;
    s_openchannelInfo[socket_id][index].count++;
    s_openchannelInfo[socket_id][index].pdpState = true;
    pthread_cond_signal(&s_signalBipPdpCond);
    pthread_mutex_unlock(&s_signalBipPdpMutex);
    RLOGD("s_openchannelInfo[%d] count= %d", socket_id,
            s_openchannelInfo[socket_id][index].count);
    requestOrSendDataCallList(socket_id, index + 1, &t);
    return;

error:
    if (index >= 0) {
        putPDP(socket_id, getFallbackCid(socket_id, index) - 1);
        putPDP(socket_id, index);
        s_openchannelInfo[socket_id][index].pdpState = true;
    }
    pthread_mutex_lock(&s_signalBipPdpMutex);
    pthread_cond_signal(&s_signalBipPdpCond);
    pthread_mutex_unlock(&s_signalBipPdpMutex);
    putUnusablePDPCid(socket_id);

    response.cause = s_lastPDPFailCause[socket_id];
    response.suggestedRetryTime = -1;
    response.cid = -1;

    cleanEth(socket_id, index + 1);

#if (SIM_COUNT == 2)
    if (s_dataAllowed[socket_id] == 0 && (int)socket_id == s_ddsOnModem) {
        switchData(socket_id, false);
     }
#endif

    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response,
            sizeof(RIL_SetupDataCallResult_v1_4));
}

static void deactivateDataConnection(RIL_SOCKET_ID socket_id, void *data,
                                     size_t datalen, RIL_Token t) {
    int cid;
    int secondaryCid = -1;
    const char *p_cid = NULL;
    const char *strReason = NULL;
    ATResponse *p_response = NULL;
    bool error = false;
    bool needToDeactive = true;

    p_cid = ((const char **)data)[0];
    cid = atoi(p_cid);
    if (cid < 1) {
        error = true;
        goto done;
    }
    if (datalen > 1 * sizeof(char *)) {
        strReason = ((const char **)data)[1];
        if (isStrEqual((char *)strReason, "shutdown")) {
            needToDeactive = false;
        }
    }
    RLOGD("deactivateDC s_in4G[%d]=%d, count = %d", socket_id, s_in4G[socket_id],
                s_openchannelInfo[socket_id][cid - 1].count);
    if (s_openchannelInfo[socket_id][cid - 1].count > 0) {
        s_openchannelInfo[socket_id][cid - 1].count--;
    }
    if (getPDPState(socket_id, cid - 1) == PDP_IDLE) {
        RLOGD("deactive done!");
        goto done;
    }
    if (s_openchannelInfo[socket_id][cid - 1].count != 0) {
        error = true;
        goto done;
    }

    secondaryCid = getFallbackCid(socket_id, cid - 1);
    cleanCid(cid - 1, needToDeactive, socket_id);
    AT_RESPONSE_FREE(p_response);
    if (secondaryCid != -1) {
        RLOGD("dual PDP, do CGACT again, fallback cid = %d", secondaryCid);
        cleanCid(secondaryCid - 1, needToDeactive, socket_id);
    }

    putPDP(socket_id, secondaryCid - 1);
    putPDP(socket_id, cid - 1);
    at_response_free(p_response);

done:
    if (needToDeactive) {
        cleanEth(socket_id, cid);
    }

    if (s_isSimPresent[socket_id] != PRESENT) {
        RLOGE("deactivateDataConnection: card is absent");
        RIL_onRequestComplete(t, RIL_E_INVALID_CALL_ID, NULL, 0);
        return;
    }
    if (t != NULL) {
        if (error) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        }
    }
    return;
}

static bool isStrEmpty(char *str) {
    if (NULL == str || strcmp(str, "") == 0) {
        return true;
    }
    return false;
}

static int getPco(RIL_SOCKET_ID socket_id, RIL_InitialAttachApn *response,
                  int cid) {
    ATResponse *pdnResponse = NULL;
    char *line = NULL;
    int skip = 0;
    int err = -1;
    ATLine *pCur = NULL;
    int curr_cid =0;
    char *username = NULL;
    char *password = NULL;

    err = at_send_command_multiline(socket_id, "AT+CGPCO?", "+CGPCO:",
                                    &pdnResponse);
    if (err < 0 || pdnResponse->success == 0) goto done;
    for (pCur = pdnResponse->p_intermediates; pCur != NULL;
         pCur = pCur->p_next) {
        line = pCur->line;
        err = at_tok_start(&line);
        if (err < 0) {
            goto done;
        }
        err = at_tok_nextint(&line, &skip);
        if (err < 0) {
            goto done;
        }
        err = at_tok_nextstr(&line, &username);
        if (err < 0) {
            goto done;
        }
        err = at_tok_nextstr(&line, &password);
        if (err < 0) {
            goto done;
        }
        err = at_tok_nextint(&line, &curr_cid);
        if (err < 0) {
            goto done;
        }
        if (curr_cid != cid) {
            continue;
        }
        snprintf(response->username, ARRAY_SIZE,
                 "%s", username);
        snprintf(response->password, ARRAY_SIZE,
                 "%s", password);
        err = at_tok_nextint(&line, &response->authtype);
    }
done:
    at_response_free(pdnResponse);
    return err;
}

static int getDataProfile(RIL_InitialAttachApn *response,
                          RIL_SOCKET_ID socket_id, int cid) {
    int ret = -1;
    if (cid < 1) {
        return ret;
    }
    queryAllActivePDNInfos(socket_id);
    if (s_PDN[cid - 1].nCid == cid) {
        snprintf(response->apn, ARRAY_SIZE,
                 "%s", s_PDN[cid - 1].strApn);
        snprintf(response->protocol, 16,
                 "%s", s_PDN[cid - 1].strIPType);
        ret = getPco(socket_id, response, cid);
    }
    return ret;
}

static bool isStrEqual(char *new, char *old) {
    bool ret = false;
    if (isStrEmpty(old) && isStrEmpty(new)) {
        ret = true;
    } else if (!isStrEmpty(old) && !isStrEmpty(new)) {
        if (strcasecmp(old, new) == 0) {
            ret = true;
        } else {
            RLOGD("isStrEqual old=%s, new=%s", old, new);
        }
    } else {
        RLOGD("isStrEqual old or new is empty!");
    }
    return ret;
}

static bool isApnEqual(char *new, char *old) {
    char strApnName[ARRAY_SIZE] = {0};
    strncpy(strApnName, old, checkCmpAnchor(old));
    strApnName[strlen(strApnName)] = '\0';
    if (isStrEmpty(new) || isStrEqual(new, old) ||
        isStrEqual(strApnName, new)) {
        return true;
    }
    return false;
}

static bool isProtocolEqual(char *new, char *old) {
    bool ret = false;
    if (strcasecmp(new, "IPV4V6") == 0 ||
        strcasecmp(old, "IPV4V6") == 0 ||
        strcasecmp(new, old) == 0) {
        ret = true;
    }
    return ret;
}

static int compareApnProfile(RIL_InitialAttachApn *new,
                             RIL_InitialAttachApn *old) {
    int ret = -1;
    int AUTH_NONE = 0;
    if (isStrEmpty(new->username) || isStrEmpty(new->password) ||
        new->authtype <= 0) {
        new->authtype = AUTH_NONE;
        if (new->username != NULL) {
            memset(new->username, 0, strlen(new->username));
        }
        if (new->password != NULL) {
            memset(new->password, 0, strlen(new->password));
        }
    }
    if (isStrEmpty(old->username) || isStrEmpty(old->password) ||
        old->authtype <= 0) {
        RLOGD("old profile is empty");
        old->authtype = AUTH_NONE;
        if (old->username != NULL) {
            memset(old->username, 0, strlen(old->username));
        }
        if (old->password != NULL) {
            memset(old->password, 0, strlen(old->password));
        }
    }
    if (isStrEmpty(new->apn) && isStrEmpty(new->protocol)) {
        ret = 2;
    } else if (isApnEqual(new->apn, old->apn) &&
        isStrEqual(new->protocol, old->protocol) &&
        isStrEqual(new->username, old->username) &&
        isStrEqual(new->password, old->password) &&
        new->authtype == old->authtype) {
        ret = 1;
    }
    return ret;
}

static void setDataProfile(RIL_InitialAttachApn *new, int cid,
                           RIL_SOCKET_ID socket_id) {
    char qosState[PROPERTY_VALUE_MAX] = {0};
    char qosSduErrorRatio[PROPERTY_VALUE_MAX] = {0};
    char qosResidualBitErrorRatio[PROPERTY_VALUE_MAX] = {0};
    char cmd[AT_COMMAND_LEN] = {0};
    char newCmd[AT_COMMAND_LEN] = {0};
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%d,\"%s\",\"%s\",\"\",0,0",
             cid, new->protocol, new->apn);
    int err = cgdcont_set_cmd_req(cmd, newCmd);
    if (err == 0) {
        at_send_command(socket_id, newCmd, NULL);
    }

    snprintf(cmd, sizeof(cmd), "AT+CGPCO=0,\"%s\",\"%s\",%d,%d",
             new->username, new->password, cid, new->authtype);
    at_send_command(socket_id, cmd, NULL);

    /* Set required QoS params to default */
    property_get(ENG_QOS_PROP, qosState, "0");
    property_get(QOS_SDU_ERROR_RATIO, qosSduErrorRatio, "1e4");
    property_get(QOS_RESIDUAL_BIT_ERROR_RATIO, qosResidualBitErrorRatio, "0e0");
    if (!strcmp(qosState, "0")) {
        snprintf(cmd, sizeof(cmd),
                  "AT+CGEQREQ=%d,%d,0,0,0,0,2,0,\"%s\",\"%s\",3,0,0",
                  cid, s_trafficClass[socket_id], qosSduErrorRatio, qosResidualBitErrorRatio);
        at_send_command(socket_id, cmd, NULL);
    }
}

static void requestSetInitialAttachAPN(RIL_SOCKET_ID socket_id, void *data,
                                       size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int initialAttachId = 1;
    int ret = -1;
    bool isSetReattach = false;
    char manualAttachProp[PROPERTY_VALUE_MAX] = {0};
    char vsimProp[PROPERTY_VALUE_MAX] = {0};

    RIL_InitialAttachApn *response =
            (RIL_InitialAttachApn *)calloc(1, sizeof(RIL_InitialAttachApn));
    response->apn = (char *)calloc(ARRAY_SIZE, sizeof(char));
    response->protocol = (char *)calloc(16, sizeof(char));
    response->username = (char *)calloc(ARRAY_SIZE, sizeof(char));
    response->password = (char *)calloc(ARRAY_SIZE, sizeof(char));

    // Claro Test APN private failed
    property_get(LTE_MANUAL_ATTACH_PROP, manualAttachProp, "0");
    RLOGD("persist.radio.manual.attach: %s", manualAttachProp);
    if (data != NULL) {
        RIL_InitialAttachApn *pIAApn = (RIL_InitialAttachApn *)data;
        ret = getDataProfile(response, socket_id, initialAttachId);
        ret = compareApnProfile(pIAApn, response);
        if (ret > 0) {
            if (ret == 2) {  // esm flag = 0, both apn and protocol are empty.
                // Claro Test APN private failed
                if (!strcmp(manualAttachProp, "1")
                        && s_isFirstSetAttach[socket_id]
                        && (s_roModemConfig == LWG_LWG
                                || socket_id == s_multiModeSim)) {
                    at_send_command(socket_id, "AT+CGATT=1", NULL);
                    s_isFirstSetAttach[socket_id] = false;
                }
            } else {
                RLOGD("send APN information even though apn is same with network");
                setDataProfile(pIAApn, initialAttachId, socket_id);
            }
            goto done;
        } else {
            setDataProfile(pIAApn, initialAttachId, socket_id);
            if (!strcmp(manualAttachProp, "1")
                    && s_isFirstSetAttach[socket_id]
                    && (s_roModemConfig == LWG_LWG
                            || socket_id == s_multiModeSim)) {
                at_send_command(socket_id, "AT+CGATT=1", NULL);
                s_isFirstSetAttach[socket_id] = false;
                goto done;
            }
        }
        RLOGD("get_data_profile s_PSRegStateDetail=%d, s_in4G=%d",
               s_PSRegStateDetail[socket_id], s_in4G[socket_id]);

        isSetReattach = (socket_id == s_multiModeSim);
        property_get(VSIM_PRODUCT_PROP, vsimProp, "0");
        if (strcmp(vsimProp, "0") != 0) {
            if (isSetReattach && (s_workMode[socket_id] == WCDMA_ONLY
                    || s_workMode[socket_id] == WCDMA_AND_GSM
                    || s_workMode[socket_id] == GSM_ONLY)) {
                isSetReattach = false;
            }
        }
        if (isSetReattach && (s_in4G[socket_id] == 1 ||
            s_PSRegStateDetail[socket_id] == RIL_NOT_REG_AND_NOT_SEARCHING ||
            s_PSRegStateDetail[socket_id] == RIL_NOT_REG_AND_SEARCHING ||
            s_PSRegStateDetail[socket_id] == RIL_UNKNOWN ||
            s_PSRegStateDetail[socket_id] == RIL_REG_DENIED)) {
            at_send_command(socket_id, "AT+SPREATTACH", NULL);
        }
    }
done:
    free(response->apn);
    free(response->protocol);
    free(response->username);
    free(response->password);
    free(response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    s_trafficClass[socket_id] = TRAFFIC_CLASS_DEFAULT;
}

static void requestSetInitialAttachAPNExt(RIL_SOCKET_ID socket_id, void *data,
                                          size_t datalen, RIL_Token t) {
    char cmd[AT_COMMAND_LEN] = {0};
    char newCmd[AT_COMMAND_LEN] = {0};
    char qosState[PROPERTY_VALUE_MAX] = {0};
    char qosSduErrorRatio[PROPERTY_VALUE_MAX] = {0};
    char qosResidualBitErrorRatio[PROPERTY_VALUE_MAX] = {0};
    int initialAttachId = 0;  // use index for sos or ims
    int err = -1;
    RIL_InitialAttachApn *initialAttachApn = NULL;
    if (data == NULL) {
        goto error;
    }
    initialAttachApn = (RIL_InitialAttachApn *)data;

    if ((initialAttachApn->apnTypes & RIL_APN_TYPE_IMS) != 0) {
        initialAttachId = 11;
        RLOGD("RIL_APN_TYPE_IMS");
    } else if ((initialAttachApn->apnTypes & RIL_APN_TYPE_EMERGENCY) != 0) {
        initialAttachId = 9;
        RLOGD("RIL_APN_TYPE_SOS");
    }
    snprintf(cmd, sizeof(cmd),
             "AT+CGDCONT=%d,\"%s\",\"%s\",\"\",0,0",
             initialAttachId,
             initialAttachApn->protocol,
             initialAttachApn->apn);
    err = cgdcont_set_cmd_req(cmd, newCmd);
    if (err == 0) {
        at_send_command(socket_id, newCmd, NULL);
    }

    snprintf(cmd, sizeof(cmd), "AT+CGPCO=0,\"%s\",\"%s\",%d,%d",
             initialAttachApn->username,
             initialAttachApn->password,
             initialAttachId,
             initialAttachApn->authtype);
    err = at_send_command(socket_id, cmd, NULL);

    /* Set required QoS params to default */
    property_get(ENG_QOS_PROP, qosState, "0");
    property_get(QOS_SDU_ERROR_RATIO, qosSduErrorRatio, "1e4");
    property_get(QOS_RESIDUAL_BIT_ERROR_RATIO, qosResidualBitErrorRatio, "0e0");
    if (!strcmp(qosState, "0")) {
        snprintf(cmd, sizeof(cmd),
                 "AT+CGEQREQ=%d,%d,0,0,0,0,2,0,\"%s\",\"%s\",3,0,0",
                 initialAttachId, s_trafficClass[socket_id],
                 qosSduErrorRatio, qosResidualBitErrorRatio);
        err = at_send_command(socket_id, cmd, NULL);
    }
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;
error:
    RLOGE("INITIAL_ATTACH_APN data is null");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/**
 * RIL_REQUEST_LAST_PDP_FAIL_CAUSE
 * Requests the failure cause code for the most recently failed PDP
 * context activate.
 */
void requestLastDataFailCause(RIL_SOCKET_ID socket_id, void *data,
                              size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    int response = PDP_FAIL_ERROR_UNSPECIFIED;
    response = s_lastPDPFailCause[socket_id];
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
}

static void requestDataCallList(RIL_SOCKET_ID socket_id, void *data,
                                size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);

    requestOrSendDataCallList(socket_id, -1, &t);
}

static int attachGPRS(RIL_SOCKET_ID socket_id, RIL_Token t) {
    RIL_UNUSED_PARM(t);

    int err = -1;
    int ret = RIL_E_SUCCESS;
    char cmd[AT_COMMAND_LEN] = {0};
    char propDdsOnModem[PROPERTY_VALUE_MAX] = {0};
    ATResponse *p_response = NULL;

    s_ddsOnModem = socket_id;
    snprintf(propDdsOnModem, sizeof(propDdsOnModem), "%d", s_ddsOnModem);
    property_set(ALLOW_DATA_MODEM_SOCKET_ID, propDdsOnModem);

    RLOGD("s_ddsOnModem = %d", s_ddsOnModem);

    if (s_radioState[socket_id] != RADIO_STATE_ON) {
        ret = RIL_E_RADIO_NOT_AVAILABLE;
        return ret;
    }

    if (s_roModemConfig >= LWG_LWG) {
        snprintf(cmd, sizeof(cmd), "AT+SPSWDATA");
        at_send_command(socket_id, cmd, NULL);
    } else {
        if (s_sessionId[socket_id] != 0) {
            RLOGD("setRadioCapability is on going during attach, return!!");
            ret = RIL_E_GENERIC_FAILURE;
            goto done;
        }
        if (socket_id != s_multiModeSim ) {
            snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,1",
                     socket_id);
            at_send_command(socket_id, cmd, NULL);
            err = at_send_command(socket_id, "AT+CGATT=1", &p_response);
            if (err < 0 || p_response->success == 0) {
                ret = RIL_E_GENERIC_FAILURE;
                goto done;
            }
        } else {
            snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,0",
                     1 - socket_id);
            at_send_command(socket_id, cmd, NULL);
        }
    }

done:
    at_response_free(p_response);
    return ret;
}

static int detachGPRS(RIL_SOCKET_ID socket_id, RIL_Token t) {
    RIL_UNUSED_PARM(t);

    int ret = RIL_E_SUCCESS;
    int cid = -1, i = 0;
    int state = PDP_IDLE;
    int err = -1;
    char cmd[AT_COMMAND_LEN] = {0};

    ATResponse *p_response = NULL;

    if (s_radioState[socket_id] != RADIO_STATE_ON) {
        return ret;
    }

    if (s_roModemConfig < LWG_LWG || t != NULL) {
        for (i = 0; i < MAX_PDP; i++) {
            cid = getPDPCid(socket_id, i);
            state = getPDPState(socket_id, i);
            if (state == PDP_BUSY || cid > 0) {
                 cgact_deact_cmd_rsp(i + 1, socket_id);
                 snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", i + 1);
                 at_send_command(socket_id, cmd, NULL);
                 RLOGD("s_PDP[%d].state = %d", i, state);
                 putPDP(socket_id, i);
                 if (state == PDP_BUSY) {
                    requestOrSendDataCallList(socket_id, i + 1, NULL);
                 }
            }
        }
    }

    if (s_roModemConfig < LWG_LWG) {
        if (s_sessionId[socket_id] != 0) {
            RLOGD("setRadioCapability is on going during detach, return!!");
            ret = RIL_E_GENERIC_FAILURE ;
            goto done;
        }
        if (socket_id != s_multiModeSim) {
            err = at_send_command(socket_id, "AT+SGFD", &p_response);
            if (err < 0 || p_response->success == 0) {
                ret = RIL_E_GENERIC_FAILURE ;
                goto done;
            }
            snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,0",
                     socket_id);
        } else {
            snprintf(cmd, sizeof(cmd), "AT+SPSWITCHDATACARD=%d,1",
                     1 - socket_id);
        }
        err = at_send_command(socket_id, cmd, NULL);
    }

done:
    at_response_free(p_response);
    return ret;
}

#if (SIM_COUNT == 2)
static void switchData(RIL_SOCKET_ID socket_id, bool isSwitchDataToCurrentSim) {
    if (isSwitchDataToCurrentSim) {
        detachGPRS(1 - socket_id, NULL);
        attachGPRS(socket_id, NULL);
    } else {
        detachGPRS(socket_id, NULL);
        attachGPRS(1 - socket_id, NULL);
    }
}
#endif

void saveDataCardProp(int socket_id) {
    char propValue[PROPERTY_VALUE_MAX] = {0};
    snprintf(propValue, sizeof(propValue), "%d", socket_id);
    property_set(ALLOW_DATA_SOCKET_ID, propValue);
    /* bug1009443 PDP could not be activated after airplane mode on @{ */
    if (s_modemConfig == LWG_LWG) {
        property_set(PRIMARY_SIM_PROP, propValue);
        s_multiModeSim = socket_id;
        RLOGD("s_multiModeSim = %d", s_multiModeSim);
    }
    /* }@ */
}

void requestSetPreferredDataModem(RIL_SOCKET_ID socket_id, void *data,
                                  size_t datalen, RIL_Token t) {
    int dds = ((int *)data)[0];
    int retDetach = RIL_E_SUCCESS;
    int retAttach = RIL_E_SUCCESS;

    if (dds < RIL_SOCKET_1 || dds >= SIM_COUNT) {
        RLOGE("Invalid modem id %d", dds);
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }
    saveDataCardProp(dds);
    s_dataAllowed[dds] = 1;
    RLOGD("s_dataAllowed[%d] = %d", dds, s_dataAllowed[dds]);

#if (SIM_COUNT == 2)
    s_dataAllowed[1 - dds] = 0;
#endif

    if (s_presentSIMCount == 1) {
        retAttach = attachGPRS(dds, t);
    } else {
        retDetach = detachGPRS(1 - dds, t);
        retAttach = attachGPRS(dds, t);
    }

    if (retDetach == RIL_E_SUCCESS && retAttach == RIL_E_SUCCESS) {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    } else if (retDetach != RIL_E_SUCCESS) {
        RIL_onRequestComplete(t, retDetach, NULL, 0);
    } else {
        RIL_onRequestComplete(t, retAttach, NULL, 0);
    }
}

void requestImsRegaddr(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                       RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);
    ATResponse *p_response = NULL;
    int err = 0;
    err = at_send_command_singleline(socket_id, "AT+SPIMSREGADDR?",
            "+SPIMSREGADDR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        int count = 2;
        char *imsAddr[2] = {NULL, NULL};
        char *line = p_response->p_intermediates->line;
        if (findInBuf(line, strlen(line), "+SPIMSREGADDR")) {
            err = at_tok_flag_start(&line, ':');
            if (err < 0) goto error;

            err = at_tok_nextstr(&line, &imsAddr[0]);
            if ((err < 0) || (imsAddr[0] == NULL)) goto error;

            err = at_tok_nextstr(&line, &imsAddr[1]);
            if ((err < 0) || (imsAddr[1] == NULL)) goto error;

            RIL_onRequestComplete(t, RIL_E_SUCCESS, imsAddr, count * sizeof(char*));
            at_response_free(p_response);
            return;
        }
    }
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

void requestSetPsDataOff(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
        RIL_Token t) {
    int err = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    if (data == NULL || datalen < 2 * sizeof(int)) {
        goto error;
    }
    int onOff = ((int *)data)[0];
    int exemption = ((int *)data)[1];
    RLOGD("requestSetPsDataOff %d, %d", onOff, exemption);
    sendPsDataOffToExtData(socket_id, exemption, 0);
    snprintf(cmd, sizeof(cmd), "AT+CPSDO=%d", onOff);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    at_response_free(p_response);
}

void requestGetImsPcscfAddr(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                            RIL_Token t) {
    RIL_UNUSED_PARM(data);
    RIL_UNUSED_PARM(datalen);
    ATResponse *p_response = NULL;
    ATLine *p_cur = NULL;
    char *input = NULL;
    char *sskip = NULL;
    char *pcscf_prim_addr = NULL;
    int skip = 0;

    int err = at_send_command_multiline(socket_id, "AT+CGCONTRDP=11",
            "+CGCONTRDP:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        input = p_cur->line;
        err = at_tok_flag_start(&input, ':');
        if (err < 0) goto error;

        err = at_tok_nextint(&input, &skip);  // cid
        if (err < 0) goto error;

        err = at_tok_nextint(&input, &skip);  // bearer_id
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &sskip);  // apn
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &sskip);  // local_addr_and_subnet_mask
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &sskip);  // gw_addr
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &sskip);  // dns_prim_addr
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &sskip);  // dns_sec_addr
        if (err < 0) goto error;

        err = at_tok_nextstr(&input, &pcscf_prim_addr);  // pcscf_prim_addr
        if (err < 0) goto error;

        if (pcscf_prim_addr != NULL) {
            RIL_onRequestComplete(t, RIL_E_SUCCESS, pcscf_prim_addr, strlen(pcscf_prim_addr) + 1);
            AT_RESPONSE_FREE(p_response);
            return;
        }
    }

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    AT_RESPONSE_FREE(p_response);
}

void requestStartKeepAlive(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                           RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int err = -1, len = 0, pOffset = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    char srcAddr[AT_COMMAND_LEN] = {0};
    char destAddr[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;
    RIL_KeepaliveStatus response = {0};
    RIL_KeepaliveRequest *kaReq = (RIL_KeepaliveRequest *)data;
    RLOGD("StartKeepAlive:cid %d,destPort %d,maxInterval %d, srcPort %d,type %d",
            kaReq->cid, kaReq->destinationPort,
            kaReq->maxKeepaliveIntervalMillis, kaReq->sourcePort, kaReq->type);
    if (kaReq->cid < 1 || kaReq->cid > MAX_PDP) {
        goto error;
    }

    len = (kaReq->type == NATT_IPV4) ? 4 : 16;
    snprintf(srcAddr, sizeof(srcAddr), "%u", (unsigned char)kaReq->sourceAddress[0]);
    snprintf(destAddr, sizeof(destAddr), "%u", (unsigned char)kaReq->destinationAddress[0]);
    for (int i = 1; i < len; i++) {
        pOffset = strlen(srcAddr);
        snprintf(srcAddr + pOffset, sizeof(srcAddr) - pOffset, ".%u",
                (unsigned char)kaReq->sourceAddress[i]);
        pOffset = strlen(destAddr);
        snprintf(destAddr + pOffset, sizeof(destAddr) - pOffset, ".%u",
                (unsigned char)kaReq->destinationAddress[i]);
    }
    RLOGD("StartKeepAlive: srcaddr %s, destaddr %s", srcAddr, destAddr);
    //AT+SPKEEPALIVE=enable，session，millis，cid，sourceport，dstport，iptype，“sourceaddr”，“dstaddr”
    snprintf(cmd, sizeof(cmd), "AT+SPKEEPALIVE=1,%d,%d,%d,%d,%d,%d,\"%s\",\"%s\"",
            kaReq->cid, kaReq->maxKeepaliveIntervalMillis, kaReq->cid, kaReq->sourcePort,
            kaReq->destinationPort, kaReq->type, srcAddr, destAddr);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        response.code = KEEPALIVE_INACTIVE;
    } else {
        response.code = KEEPALIVE_ACTIVE;
    }
    response.sessionHandle = kaReq->cid;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    at_response_free(p_response);
}

void requestStopKeepAlive(RIL_SOCKET_ID socket_id, void *data, size_t datalen,
                          RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int err = -1;
    ATResponse *p_response = NULL;
    if (data == NULL) {
        goto error;
    }
    int ssessionHandle = ((int *)data)[0];
    if (ssessionHandle < 1 || ssessionHandle > MAX_PDP) {
        goto error;
    }
    char cmd[AT_COMMAND_LEN] = {0};
    snprintf(cmd, sizeof(cmd), "AT+SPKEEPALIVE=0,%d", ssessionHandle);
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    at_response_free(p_response);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
    at_response_free(p_response);
}

void requestSwitchUsbShare(RIL_SOCKET_ID socket_id, void *data, size_t datalen, RIL_Token t) {
    RIL_UNUSED_PARM(datalen);
    int err;
    ATResponse *p_response = NULL;
    char cmd[AT_COMMAND_LEN] = {0};
    int mode = ((int *)data)[0];

    //  Get the property of usbtethering
    char prop[PROPERTY_VALUE_MAX] = {0};
    property_get(RIL_USB_TETHER_FLAG, prop, "0");

    if (1 == mode && !strcmp(prop, "0")) {  //  turn on usb share
        snprintf(cmd, sizeof(cmd), "AT+SPASENGMD=\"#dsm_usb_share_enable\", 1");
    } else if (0 == mode && !strcmp(prop, "1")) {  //  turn off usb share
        snprintf(cmd, sizeof(cmd), "AT+SPASENGMD=\"#dsm_usb_share_enable\", 0");
    } else {
        RLOGD("usb share is turn on or turn off, mode = %d", mode);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        return;
    }
    err = at_send_command(socket_id, cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        if (1 == mode) {
            property_set(RIL_USB_TETHER_FLAG, "1");
        } else {
            property_set(RIL_USB_TETHER_FLAG, "0");
            pthread_mutex_lock(&s_usbSharedMutex);
            s_rxBytes = 0;
            s_txBytes = 0;
            char rxBytesStr[PROPERTY_VALUE_MAX] = {0};
            char txBytesStr[PROPERTY_VALUE_MAX] = {0};

            snprintf(rxBytesStr, sizeof(rxBytesStr), "%ld", s_rxBytes);
            snprintf(txBytesStr, sizeof(txBytesStr), "%ld", s_txBytes);
            property_set("ril.sys.usb.tether.rx", rxBytesStr);
            property_set("ril.sys.usb.tether.tx", txBytesStr);
            RLOGD("SPDSMINFOU: s_rxBytes: %ld, s_txBytes: %ld", s_rxBytes, s_txBytes);
            pthread_mutex_unlock(&s_usbSharedMutex);
        }
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
    AT_RESPONSE_FREE(p_response);
}

int processDataRequest(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id) {
    int ret = 1;
    int err;
    char cmd[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    ATResponse *p_response = NULL;

    switch (request) {
        case RIL_REQUEST_SETUP_DATA_CALL: {
        //  Get the property of usbtethering
            property_get(USB_TETHER_ENABLE, prop, "0");

            //  Get ril the property of usbtethering
            char ril_prop[PROPERTY_VALUE_MAX] = {0};
            property_get(RIL_USB_TETHER_FLAG, ril_prop, "0");

            if (!strcmp(prop, "1") && !strcmp(ril_prop, "0")) {
                snprintf(cmd, sizeof(cmd), "AT+SPASENGMD=\"#dsm_usb_share_enable\", 1");
                err = at_send_command(socket_id, cmd, &p_response);
                if (err < 0 || p_response->success == 0) {
                    RLOGD("turn on Failed");
                } else {
                    property_set(RIL_USB_TETHER_FLAG, "1");
                    RLOGD("turn on successully");
                }
             } else if (!strcmp(prop, "0") && !strcmp(ril_prop, "1")) {
                 snprintf(cmd, sizeof(cmd), "AT+SPASENGMD=\"#dsm_usb_share_enable\", 0");
                 err = at_send_command(socket_id, cmd, &p_response);
                 if (err < 0 || p_response->success == 0) {
                     RLOGD("turn off Failed");
                 } else {
                     property_set(RIL_USB_TETHER_FLAG, "0");
                     RLOGD("turn off successully");
                     pthread_mutex_lock(&s_usbSharedMutex);
                     s_rxBytes = 0;
                     s_txBytes = 0;
                     char rxBytesStr[PROPERTY_VALUE_MAX] = {0};
                     char txBytesStr[PROPERTY_VALUE_MAX] = {0};

                     snprintf(rxBytesStr, sizeof(rxBytesStr), "%ld", s_rxBytes);
                     snprintf(txBytesStr, sizeof(txBytesStr), "%ld", s_txBytes);
                     property_set("ril.sys.usb.tether.rx", rxBytesStr);
                     property_set("ril.sys.usb.tether.tx", txBytesStr);
                     RLOGD("SPDSMINFOU: s_rxBytes: %ld, s_txBytes: %ld", s_rxBytes, s_txBytes);
                     pthread_mutex_unlock(&s_usbSharedMutex);
                 }
             }
             AT_RESPONSE_FREE(p_response);
            RIL_SetupDataCallResult_v1_4 response;
            memset(&response, 0, sizeof(RIL_SetupDataCallResult_v1_4));
            response.suggestedRetryTime = -1;
            //Datacall will be deactived before GCF Test starting
            //when setup datacall retry, return RIL_E_RADIO_NOT_AVAILABLE to stop it.
            if (s_isGCFTest) {
                RLOGD("SETUP_DATA_CALL attach not enable when GCF test!");
                response.cause = PDP_FAIL_ERROR_UNSPECIFIED;
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response,
                                                sizeof(RIL_SetupDataCallResult_v1_4));
                break;
            }

            if (s_isSimPresent[socket_id] == SIM_ABSENT) {
                RIL_onRequestComplete(t, RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW, NULL, 0);
                break;
            }
            if (s_manualSearchNetworkId >= 0 || s_swapCard != 0) {
                RLOGD("s_manualSearchNetworkId = %d, swapcard = %d", s_manualSearchNetworkId, s_swapCard);
                response.cause = PDP_FAIL_ERROR_UNSPECIFIED;
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response,
                                                sizeof(RIL_SetupDataCallResult_v1_4));
                break;
            }

#if (SIM_COUNT == 2)
            if ((int)socket_id != s_ddsOnModem && s_dataAllowed[socket_id] == 1) {
                RLOGD("s_dataAllowed[%d] = %d, s_ddsOnMode = %d", socket_id, s_dataAllowed[socket_id], s_ddsOnModem);
                response.cause = PDP_FAIL_ERROR_UNSPECIFIED;
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response,
                                                sizeof(RIL_SetupDataCallResult_v1_4));
                break;
            }
#endif

            RLOGD("s_desiredRadioState[socket_id] = %d, isAttachEnable() = %d",
                    s_desiredRadioState[socket_id], s_swapCard);
            if (s_desiredRadioState[socket_id] > 0 && isAttachEnable()) {
                RLOGD("SETUP_DATA_CALL s_PSRegState[%d] = %d", socket_id,
                      s_PSRegState[socket_id]);
#if (SIM_COUNT == 2)
                if (s_dataAllowed[socket_id] == 0) {
                    switchData(socket_id, true);
                 }
#endif
                requestSetupDataCall(socket_id, data, datalen, t);
            } else {
                RLOGD("SETUP_DATA_CALL attach not enable by engineer mode");
                response.cause = PDP_FAIL_ERROR_UNSPECIFIED;
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response,
                                                sizeof(RIL_SetupDataCallResult_v1_4));
            }
            break;
        }
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            deactivateDataConnection(socket_id, data, datalen, t);
#if (SIM_COUNT == 2)
            if (s_dataAllowed[socket_id] == 0 && !isExistActivePdp(socket_id)) {
                switchData(socket_id, false);
            }
#endif
            break;
        case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
            requestLastDataFailCause(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_DATA_CALL_LIST:
            requestDataCallList(socket_id, data, datalen, t);
            break;
        case RIL_REQUEST_ALLOW_DATA: {
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
        }
        case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
            requestSetInitialAttachAPN(socket_id, data, datalen, t);
            break;
        /* IMS request @{ */
        case RIL_EXT_REQUEST_SET_INITIAL_ATTACH_APN:
            requestSetInitialAttachAPNExt(socket_id, data, datalen, t);
            break;
        /* }@ */
        case RIL_EXT_REQUEST_TRAFFIC_CLASS: {
            s_trafficClass[socket_id] = ((int *)data)[0];
            if (s_trafficClass[socket_id] < 0) {
                s_trafficClass[socket_id] = TRAFFIC_CLASS_DEFAULT;
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            break;
        }
        /* For data clear code @{ */
        case RIL_EXT_REQUEST_ENABLE_LTE: {
            int err = -1;
            int value = ((int *)data)[0];
            char cmd[AT_COMMAND_LEN] = {0};
            p_response = NULL;

            snprintf(cmd, sizeof(cmd), "AT+SPEUTRAN=%d", value);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_ATTACH_DATA: {
            int err = -1;
            int value = ((int *)data)[0];
            char cmd[AT_COMMAND_LEN] = {0};
            p_response = NULL;

            snprintf(cmd, sizeof(cmd), "AT+CGATT=%d", value);
            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_FORCE_DETACH: {
            int err;
            p_response = NULL;
            err = at_send_command(socket_id, "AT+CLSSPDT=1", &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_ENABLE_RAU_NOTIFY: {
            // set RAU SUCCESS report to AP
            at_send_command(socket_id, "AT+SPREPORTRAU=1", NULL);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        /* }@ */
        case RIL_EXT_REQUEST_SET_SINGLE_PDN: {
            s_singlePDNAllowed[socket_id] = ((int *)data)[0];
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_EXT_REQUEST_SET_XCAP_IP_ADDR: {
            char *ifname = ((char **)data)[0];
            char *ipv4 = ((char **)data)[1];
            char *ipv6 = ((char **)data)[2];
            char ipv6Address[PROPERTY_VALUE_MAX] = {0};
            RLOGD("ifname = %s", ifname);
            /* send IP for volte addtional business */
            if (ifname != NULL) {
                int cid = getCidByEthName(socket_id, ifname, strlen(ifname));
                if (getPDPCid(socket_id, cid - 1) > 0) {
                    char cmd[AT_COMMAND_LEN] = {0};
                    if (ipv4 == NULL || strlen(ipv4) <= 0) {
                        ipv4 = "0.0.0.0";
                    }
                    if (ipv6 == NULL || strlen(ipv6) <= 0) {
                        ipv6 = "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF";
                    }
                    checkIpv6Address(ipv6, ipv6Address, sizeof(ipv6Address));
                    snprintf(cmd, sizeof(cmd), "AT+XCAPIP=%d,\"%s,[%s]\"",
                            cid, ipv4, ipv6Address);
                    at_send_command(socket_id, cmd, NULL);
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                } else {
                    RLOGD("pdn was already deactived, do nothing.");
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_EXT_REQUEST_GET_IMS_PCSCF_ADDR: {
            requestGetImsPcscfAddr(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_SET_IMS_PCSCF_ADDR: {
            int err;
            char cmd[AT_COMMAND_LEN] = {0};
            const char *strings = (const char *)data;

            if (datalen > 0 && strings != NULL && strlen(strings) > 0) {
                snprintf(cmd, sizeof(cmd), "AT+VOWIFIPCSCF=%s", strings);
                err = at_send_command(socket_id, cmd , NULL);
                if (err != 0) {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                } else {
                    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            }
            break;
        }
        case RIL_EXT_REQUEST_REATTACH: {
            at_send_command(socket_id, "AT+SPREATTACH", NULL);
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_EXT_REQUEST_IMS_REGADDR: {
            requestImsRegaddr(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_UPDATE_PLMN: {
            char cmd[AT_COMMAND_LEN] = {0};
            int type = ((int*)data)[0];
            int action = ((int*)data)[1];
            int plmn = ((int*)data)[2];
            int act1 = ((int*)data)[3];
            int act2 = ((int*)data)[4];
            int act3 = ((int*)data)[5];
            p_response = NULL;
            if (type == 0) {
                if (action == 0 || action == 1) {
                    snprintf(cmd, sizeof(cmd), "AT+CUFP=%d,\"%d\"", action, plmn);
                } else if (action == 2) {
                    snprintf(cmd, sizeof(cmd), "AT+SPSELMODE=1");
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                    break;
                }
            } else if (type == 1) {
                if (action == 1) {
                    snprintf(cmd, sizeof(cmd), "AT+CPOL=0,2,\"%d\",%d,,%d,%d", plmn, act1, act2, act3);
                } else {
                    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                    break;
                }
            } else {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }

            err = at_send_command(socket_id, cmd, &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_EXT_REQUEST_QUERY_PLMN: {
            int i = -1;
            int type = ((int*)data)[0];
            char response[AT_RESPONSE_LEN] = {0};
            char *line = NULL;
            ATLine *p_cur = NULL;
            p_response = NULL;
            if (type != 1) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
                break;
            }
            err = at_send_command_multiline(socket_id, "AT+CPOL?", "+CPOL:",
                                            &p_response);
            if (err < 0 || p_response->success == 0) {
                RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            } else {
                for (i = 0, p_cur = p_response->p_intermediates; p_cur != NULL;
                     p_cur = p_cur->p_next, i++) {
                    line = p_cur->line;
                    at_tok_start(&line);
                    skipWhiteSpace(&line);
                    strlcat(response, line, sizeof(response));
                    strlcat(response, ";", sizeof(response));
                }
                RIL_onRequestComplete(t, RIL_E_SUCCESS, response, strlen(response) + 1);
            }
            at_response_free(p_response);
            break;
        }
        case RIL_REQUEST_CONFIG_SET_PREFER_DATA_MODEM: {
            requestSetPreferredDataModem(socket_id, data, datalen, t);
            break;
        }
        case RIL_REQUEST_SET_DATA_PROFILE: {
            // for radio 1.4 VTS
            RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
            break;
        }
        case RIL_REQUEST_START_KEEPALIVE: {
            requestStartKeepAlive(socket_id, data, datalen, t);
            break;
        }
        case RIL_REQUEST_STOP_KEEPALIVE: {
            requestStopKeepAlive(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_PS_DATA_OFF: {
            requestSetPsDataOff(socket_id, data, datalen, t);
            break;
        }
        case RIL_EXT_REQUEST_SET_USBSHARE_SWITCH: {
            requestSwitchUsbShare(socket_id, data, datalen, t);
            break;
        }
        default :
            ret = 0;
            break;
    }

    return ret;
}

static void onDataCallListChanged(void *param) {
    CallbackPara *cbPara = (CallbackPara *)param;
    if (cbPara == NULL || (int)cbPara->socket_id < 0 ||
            (int)cbPara->socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id");
        return;
    }

    if (cbPara->para == NULL) {
        requestOrSendDataCallList(cbPara->socket_id, -1, NULL);
    } else {
        requestOrSendDataCallList(cbPara->socket_id, *((int *)(cbPara->para)), NULL);
        free(cbPara->para);
    }

    free(cbPara);
}

void startGSPS(void *param) {
    int err = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    char ethCmd[AT_COMMAND_LEN] = {0};
    ATResponse *p_response = NULL;

    RIL_SOCKET_ID socket_id = *((RIL_SOCKET_ID *)param);
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        return;
    }

    RLOGD("startGSPS isGCFTest = %d, cid = %d, eth state = %d", s_isGCFTest,
            s_GSCid, s_ethOnOff);

    if (s_isGCFTest) {
        for (int i = 0; i < MAX_PDP; i++) {
            int cid = getPDPCid(socket_id, i);
            int state = getPDPState(socket_id, i);
            if (state == PDP_BUSY || cid > 0) {
                RLOGD("deactive datacall for cid = %d", cid);
                snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", i + 1);
                at_send_command(socket_id, cmd, &p_response);
                RLOGD("s_PDP[%d].state = %d", i, state);
                putPDP(socket_id, i);
                cgact_deact_cmd_rsp(i + 1, socket_id);

                if (state == PDP_BUSY) {
                    requestOrSendDataCallList(socket_id, i + 1, NULL);
                }
                cleanEth(socket_id, cid);
                AT_RESPONSE_FREE(p_response);
            }
        }
    }

    if (s_ethOnOff) {
        setEth(socket_id, s_GSCid);
        snprintf(ethCmd, sizeof(ethCmd), "AT+SPAPNETID=%d,%d", s_GSCid,
                getEthIndexBySocketId(socket_id, s_GSCid));
        at_send_command(socket_id, ethCmd, NULL);

        property_set(GSPS_ETH_UP_PROP, "1");
        snprintf(cmd, sizeof(cmd), "AT+CGDATA=\"M-ETHER\", %d", s_GSCid);
        cgdata_set_cmd_req(cmd);
    } else {
        cleanEth(socket_id, s_GSCid);

        property_set(GSPS_ETH_DOWN_PROP, "1");
        snprintf(cmd, sizeof(cmd), "AT+CGACT=0, %d", s_GSCid);
    }

    err = at_send_command(socket_id, cmd, &p_response);
    if (s_ethOnOff) {
        cgdata_set_cmd_rsp(p_response, s_GSCid - 1, 0, socket_id);
    } else {
        cgact_deact_cmd_rsp(s_GSCid, socket_id);
        RLOGD("stop pppd end and set isGCFTest = %d", s_isGCFTest);
        s_isGCFTest = false;
    }

    at_response_free(p_response);
}

static void queryVideoCid(void *param) {
    int err = -1;
    int commas = 0, i;
    int cid;
    int *response = NULL;
    char cmd[32] = {0};
    char *line = NULL, *p = NULL;
    ATResponse *p_response = NULL;
    CallbackPara *cbPara = (CallbackPara *)param;

    RIL_SOCKET_ID socket_id = cbPara->socket_id;
    if ((int)socket_id < 0 || (int)socket_id >= SIM_COUNT) {
        RLOGE("Invalid socket_id %d", socket_id);
        FREEMEMORY(cbPara->para);
        FREEMEMORY(cbPara);
        return;
    }

    cid = *((int *)(cbPara->para));

    FREEMEMORY(cbPara->para);
    FREEMEMORY(cbPara);

    snprintf(cmd, sizeof(cmd), "AT+CGEQOSRDP=%d", cid);
    err = at_send_command_singleline(socket_id, cmd, "+CGEQOSRDP:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    for (p = line; *p != '\0'; p++) {
        if (*p == ',') {
            commas++;
        }
    }

    response = (int *)malloc((commas + 1) * sizeof(int));

    /**
     * +CGEQOSRDP: <cid>,<QCI>,<DL_GBR>,<UL_GBR>,<DL_MBR>,
     * <UL_MBR>,<DL_AMBR>,<UL_AMBR>
     */
    for (i = 0; i <= commas; i++) {
        err = at_tok_nextint(&line, &response[i]);
        if (err < 0)  goto error;
    }

    if (commas >= 1) {
        RLOGD("queryVideoCid param = %d, commas = %d, cid = %d, QCI = %d", cid,
              commas, response[0], response[1]);
        if (response[1] == 2) {
            RLOGD("QCI is 2, %d is video cid", response[0]);
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_RESPONSE_VIDEO_QUALITY,
                    response, (commas + 1) * sizeof(int), socket_id);
        }
    }

error:
    at_response_free(p_response);
    free(response);
}

void unsolPcoData(RIL_SOCKET_ID socket_id, const char *s) {
    int num = 0, i = 0, err = 0;
    char *tmp = NULL, *line = NULL;
    RIL_PCO_Data *pcoData = NULL;

    pcoData = (RIL_PCO_Data *) calloc(1, sizeof(RIL_PCO_Data));

    line = strdup(s);
    tmp = line;
    at_tok_start(&tmp);

    err = at_tok_nextint(&tmp, &pcoData->cid);
    if (err < 0) goto error;

    err = at_tok_nextstr(&tmp, &pcoData->bearer_proto);
    if (err < 0) goto error;

    err = at_tok_nextint(&tmp, &num);
    if (err < 0) goto error;

    for (i = 0; i < num; i++) {
        err = at_tok_nextint(&tmp, &pcoData->pco_id);
        if (err < 0) goto error;

        err = at_tok_nextint(&tmp, &pcoData->contents_length);
        if (err < 0) goto error;

        err = at_tok_nextstr(&tmp, &pcoData->contents);
        if (err < 0) goto error;

        RIL_onUnsolicitedResponse(RIL_UNSOL_PCO_DATA, pcoData, sizeof(RIL_PCO_Data), socket_id);
    }

error:
    free(pcoData);
    free(line);
}

int processDataUnsolicited(RIL_SOCKET_ID socket_id, const char *s) {
    int err;
    int ret = 1;
    char *line = NULL;

    if (strStartsWith(s, "+CGEV:")) {
        char *tmp;
        char *pCommaNext = NULL;
        static int activeCid = -1;
        int pdpState = 1;
        int cid = -1;
        int networkChangeReason = -1;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        if (strstr(tmp, "NW PDN ACT")) {
            tmp += strlen(" NW PDN ACT ");
        } else if (strstr(tmp, "NW ACT ")) {
            tmp += strlen(" NW ACT ");
            for (pCommaNext = tmp; *pCommaNext != '\0'; pCommaNext++) {
                if (*pCommaNext == ',') {
                    pCommaNext += 1;
                    break;
                }
            }
            activeCid = atoi(pCommaNext);
            RLOGD("activeCid = %d, networkChangeReason = %d", activeCid,
                  networkChangeReason);
            CallbackPara *cbPara =
                    (CallbackPara *)malloc(sizeof(CallbackPara));
            if (cbPara != NULL) {
                cbPara->para = (int *)malloc(sizeof(int));
                *((int *)(cbPara->para)) = activeCid;
                cbPara->socket_id = socket_id;
                RIL_requestTimedCallback(queryVideoCid, cbPara, NULL);
            }
        } else if (strstr(tmp, "NW PDN DEACT")) {
            tmp += strlen(" NW PDN DEACT ");
            pdpState = 0;
        } else if (strstr(tmp, " NW MODIFY ")) {
            tmp += strlen(" NW MODIFY ");
            activeCid = atoi(tmp);
            for (pCommaNext = tmp; *pCommaNext != '\0'; pCommaNext++) {
                if (*pCommaNext == ',') {
                    pCommaNext += 1;
                    break;
                }
            }
            networkChangeReason = atoi(pCommaNext);
            RLOGD("activeCid = %d, networkChangeReason = %d", activeCid,
                  networkChangeReason);
            if (networkChangeReason == 2 || networkChangeReason == 3) {
                CallbackPara *cbPara =
                        (CallbackPara *)malloc(sizeof(CallbackPara));
                if (cbPara != NULL) {
                    cbPara->para = (int *)malloc(sizeof(int));
                    *((int *)(cbPara->para)) = activeCid;
                    cbPara->socket_id = socket_id;
                    RIL_requestTimedCallback(queryVideoCid, cbPara, NULL);
                }
            /* extends <change_reason>: 8 is IP change, AP need reActive PDP */
            } else if (networkChangeReason == 8) {
                CallbackPara *cbPara = (CallbackPara *) malloc(
                        sizeof(CallbackPara));
                if (cbPara != NULL) {
                    cbPara->para = (int *)malloc(sizeof(int));
                    *((int *)(cbPara->para)) = activeCid;
                    cbPara->socket_id = socket_id;
                    RIL_requestTimedCallback(onDataCallListChanged, cbPara, NULL);
                }
            }
            goto out;
        } else {
            RLOGD("Invalid CGEV");
            goto out;
        }
        cid = atoi(tmp);
        if (cid > 0 && cid <= MAX_PDP) {
            RLOGD("update cid %d ", cid);
            updatePDPCid(socket_id, cid, pdpState);
        }
    } else if (strStartsWith(s, "^CEND:")) {
        int commas;
        int cid =-1;
        int endStatus;
        int ccCause;
        char *p = NULL;
        char *tmp = NULL;
        extern pthread_mutex_t s_callMutex[];
        extern int s_callFailCause[];

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        commas = 0;
        for (p = tmp; *p != '\0'; p++) {
            if (*p == ',') commas++;
        }
        err = at_tok_nextint(&tmp, &cid);
        if (err < 0) goto out;
        skipNextComma(&tmp);
        err = at_tok_nextint(&tmp, &endStatus);
        if (err < 0) goto out;
        err = at_tok_nextint(&tmp, &ccCause);
        if (err < 0) goto out;

        if (commas == 3) {
            pthread_mutex_lock(&s_callMutex[socket_id]);
            s_callFailCause[socket_id] = ccCause;
            pthread_mutex_unlock(&s_callMutex[socket_id]);
            RLOGD("The last call fail cause: %d", s_callFailCause[socket_id]);
        }
        if (commas == 4) {
            if (s_isGCFTest && (cid == 1) && (endStatus == 104)) {
                s_GSCid = 1;
                s_ethOnOff = 0;

                RLOGD("stop pppd from CEND unsl!");
                RIL_requestTimedCallback(startGSPS,
                        (void *)&s_socketId[socket_id], NULL);
            }

            /* GPRS reply 5 parameters */
            /* as endStatus 21 means: PDP reject by network,
             * so we not do onDataCallListChanged */
            if (endStatus != 29 && endStatus != 21) {
                if (endStatus == 104) {
                    if (cid > 0 && cid <= MAX_PDP &&
                        s_PDP[socket_id][cid - 1].state == PDP_BUSY) {
                        RLOGD("cend 104");
                        if (cid == s_curCid[socket_id]) {
                            s_LTEDetached[socket_id] = true;
                        }
                        CallbackPara *cbPara =
                                (CallbackPara *)malloc(sizeof(CallbackPara));
                        if (cbPara != NULL) {
                            cbPara->para = (int *)malloc(sizeof(int));
                            *((int *)(cbPara->para)) = cid;
                            cbPara->socket_id = socket_id;
                        }
                        if (s_openchannelInfo[socket_id][cid - 1].state != CLOSE) {
                            RLOGD("sendEvenLoopThread cid:%d", cid);
                            s_openchannelInfo[socket_id][cid - 1].cid = -1;
                            s_openchannelInfo[socket_id][cid - 1].state = CLOSE;
                            int secondaryCid = getFallbackCid(socket_id, cid - 1);
                            putPDP(socket_id, secondaryCid - 1);
                            putPDP(socket_id, cid - 1);
                            RIL_requestTimedCallback(sendEvenLoopThread, cbPara, NULL);
                        }
                        s_openchannelInfo[socket_id][cid - 1].count = 0;
                        RIL_requestTimedCallback(onDataCallListChanged, cbPara,
                                                 NULL);

                        //modify for bug1594431
                        char prop[PROPERTY_VALUE_MAX] = {0};
                        property_get(MODEM_ETH_PROP, prop, "veth");
                        downNetcard(cid, prop, socket_id);

                        //modify for bug1564183
                        cleanEth(socket_id, cid);
                    }
                } else {
                    CallbackPara *cbPara =
                            (CallbackPara *)malloc(sizeof(CallbackPara));
                    if (cbPara != NULL) {
                        cbPara->para = NULL;
                        cbPara->socket_id = socket_id;
                    }
                    RIL_requestTimedCallback(onDataCallListChanged, cbPara,
                                             NULL);
                }
                RIL_onUnsolicitedResponse(
                        RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0,
                        socket_id);
            }
        }
    } else if (strStartsWith(s, "+SPGS:")) {
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &s_GSCid);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &s_ethOnOff);
        if (err < 0) goto out;

        RIL_requestTimedCallback(startGSPS,
                (void *)&s_socketId[socket_id], NULL);
    } else if (strStartsWith(s,"+SPREPORTRAU:")) {
        char *response = NULL;
        char *tmp = NULL;

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextstr(&tmp, &response);
        if (err < 0)  goto out;

        if (!strcmp(response, "RAU SUCCESS")) {
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_RAU_SUCCESS, NULL, 0,
                                      socket_id);
        }
    } else if (strStartsWith(s, "+SPERROR:")) {
        int type;
        int errCode;
        char *tmp = NULL;
        int response[3] = {0};
        int plmn = 0;
        extern int s_ussdError[SIM_COUNT];
        extern int s_ussdRun[SIM_COUNT];

        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);

        err = at_tok_nextint(&tmp, &type);
        if (err < 0) goto out;

        err = at_tok_nextint(&tmp, &errCode);
        if (err < 0) goto out;

        if (at_tok_hasmore(&tmp)) {
            err = at_tok_nextint(&tmp, &plmn);
            if (err < 0) goto out;
        }

        if (errCode == 336) {
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_CLEAR_CODE_FALLBACK, NULL,
                                      0, socket_id);
        }
        /*if ((type == 5) && (s_ussdRun[socket_id] == 1)) { // 5: for SS
            s_ussdError[socket_id] = 1;
        } else if (type == 10) { // ps business in this sim is rejected by network
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIM_PS_REJECT, NULL, 0,
                    socket_id);
        } else if (type == 1) {
            setProperty(socket_id, "ril.sim.ps.reject", "1");
            if ((errCode == 3) || (errCode == 6) || (errCode == 7)
                    || (errCode == 8) || (errCode == 14)) {
                RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIM_PS_REJECT, NULL, 0,
                        socket_id);
            }
        }*/
        if (type == 5) { // 5: for SS
            if (s_ussdRun[socket_id] == 1) {
                 s_ussdError[socket_id] = 1;
            }
        } else if (type == 15) {
            char imsResponse[32] = {0};
            snprintf(imsResponse, sizeof(imsResponse), "%d", errCode);
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_IMS_CSFB_VENDOR_CAUSE, imsResponse,
                                                      sizeof(imsResponse), socket_id);
        } else {
            if (type == 1) {
                setProperty(socket_id, "ril.ps.reject", "1");
            }
            response[0] = type;
            response[1] = errCode;
            response[2] = plmn;
            RIL_onUnsolicitedResponse(RIL_EXT_UNSOL_SIM_PS_REJECT, response, sizeof(response),
                                            socket_id);
        }
    } else if (strStartsWith(s, "SPUCOPSLIST:")) {
        int i = 0;
        int tok = 0;
        int count = 0;
        char *tmp = NULL;
        char *checkTmp = NULL;
        line = strdup(s);
        tmp = line;
        at_tok_start(&tmp);
        skipWhiteSpace(&tmp);
        checkTmp = tmp;
        int len = strlen(checkTmp);

        while (len--) {
            if (*checkTmp == '(')
                tok++;
            if (*checkTmp  == ')') {
                if (tok == 1) {
                    count++;
                    tok--;
                }
            }
            if (*checkTmp != 0)
                checkTmp++;
        }
        RLOGD("Searched available cops list numbers = %d", count);

        char **responseStr = calloc(count, sizeof(char*));
        for (i=0; i < count; i++) {
            responseStr[i] = calloc(ARRAY_SIZE, sizeof(char));
        }
        i = 0;
        while ((i++ < count) && (tmp = strchr(tmp, '(')) ) {
            int stat1 = 0;
            int stat2 = 0;
            int stat3 = 0;
            char statChr[20] = {0};
            char *strChr = statChr;

            tmp++;
            err = at_tok_nextstr(&tmp, &strChr);
            if (err < 0) continue;

            err = at_tok_nextint(&tmp, &stat1);
            if (err < 0) continue;

            err = at_tok_nextint(&tmp, &stat2);
            if (err < 0) continue;

            err = at_tok_nextint(&tmp, &stat3);
            if (err < 0) continue;

            snprintf(responseStr[i-1], ARRAY_SIZE * sizeof(char), "%s-%d-%d-%d", strChr, stat1, stat2, stat3);
        }
        RIL_onUnsolicitedResponse (RIL_EXT_UNSOL_SPUCOPS_LIST, responseStr, count * sizeof(char *), socket_id);

        for (i = 0; i < count; i++) {
            free(responseStr[i]);
        }
        free(responseStr);
    } else if (strStartsWith(s, "+SPPCODATA:")) {
        unsolPcoData(socket_id, s);
    } else {
        ret = 0;
    }

out:
    free(line);
    return ret;
}

/*
 * phoneserver used to process these AT Commands or its response
 *    # AT+CGACT=0 set command response process
 *    # AT+CGDATA= set command process
 *    # AT+CGDATA= set command response process
 *    # AT+CGDCONT= set command response process
 *    # AT+CGDCONT? read response process
 *
 * since phoneserver has been removed, its function should be realized in RIL,
 * so when used AT+CGACT=0, AT+CGDATA=, AT+CGDATA=, AT+CGDCONT= and AT+CGDCONT?,
 * please make sure to call the corresponding process functions.
 */

void setSockTimeout() {
    struct timeval writetm, recvtm;
    writetm.tv_sec = 1;  // write timeout: 1s
    writetm.tv_usec = 0;
    recvtm.tv_sec = 10;  // recv timeout: 10s
    recvtm.tv_usec = 0;

    if (setsockopt(s_extDataFd, SOL_SOCKET, SO_SNDTIMEO, &writetm,
                     sizeof(writetm)) == -1) {
        RLOGE("WARNING: Cannot set send timeout value on socket: %s",
                 strerror(errno));
    }
    if (setsockopt(s_extDataFd, SOL_SOCKET, SO_RCVTIMEO, &recvtm,
                     sizeof(recvtm)) == -1) {
        RLOGE("WARNING: Cannot set receive timeout value on socket: %s",
                 strerror(errno));
    }
}

void *listenExtDataThread(void) {
    int retryTimes = 0;
    RLOGD("try to connect socket ext_data...");

    do {
        s_extDataFd = socket_local_client(SOCKET_NAME_EXT_DATA,
                ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
        usleep(10 * 1000);  // wait for 10ms, try 10 times
        retryTimes++;
    } while (s_extDataFd < 0 && retryTimes < 10);

    if (s_extDataFd >= 0) {
        RLOGD("connect to ext_data socket success!");
        setSockTimeout();
    } else {
        RLOGE("connect to ext_data socket failed!");
    }
    return NULL;
}

void sendCmdToExtData(char cmd[]) {
    int retryTimes = 0;

    while (s_extDataFd < 0 && retryTimes < 10) {
        s_extDataFd = socket_local_client(SOCKET_NAME_EXT_DATA,
                ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
        usleep(10 * 1000);  // wait for 10ms, try again
        retryTimes++;
        RLOGD("connect to ext_data socket and retrytimes = %d", retryTimes);
    }
    if (s_extDataFd >= 0 && retryTimes != 0) {
        RLOGD("set socket timeout for extdata");
        setSockTimeout();
    }

    if (s_extDataFd >= 0) {
        int len = strlen(cmd) + 1;
        RLOGD("write cmd to extdata!");
        if (TEMP_FAILURE_RETRY(write(s_extDataFd, cmd, len)) != len) {
            RLOGE("Failed to write cmd to ext_data!");
            close(s_extDataFd);
            s_extDataFd = -1;
        }
    }
}

void ps_service_init() {
    int i;
    int ret;
    pthread_t tid;
    pthread_attr_t attr;

    memset(pdp_info, 0x0, sizeof(pdp_info));
    for (i = 0; i < MAX_PDP_NUM; i++) {
        pdp_info[i].state = PDP_STATE_IDLE;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid, &attr, (void *)listenExtDataThread, NULL);
    if (ret < 0) {
        RLOGE("Failed to create listen_ext_data_thread errno: %d", errno);
    }
}

int ifc_set_noarp(const char *ifname) {
    struct ifreq ifr;
    int fd = -1;
    int err = -1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&ifr, 0, sizeof(struct ifreq));
    strlcpy(ifr.ifr_name, ifname, IFNAMSIZ);

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return -1;
    }

    ifr.ifr_flags = ifr.ifr_flags | IFF_NOARP;
    err = ioctl(fd, SIOCSIFFLAGS, &ifr);
    close(fd);
    return err;
}

int getIPV6Addr(const char *prop, int ethIndex, RIL_SOCKET_ID socket_id) {
    char netInterface[NET_INTERFACE_LENGTH] = {0};
    const int maxRetry = 120;  // wait 12s
    int retry = 0;
    int setup_success = 0;
    char cmd[AT_COMMAND_LEN] = {0};
    const int ipv6AddrLen = 32;

    snprintf(netInterface, sizeof(netInterface), "%s%d", prop, ethIndex);
    RLOGD("query interface %s, socket_id= %d, s_LTEDetached[socket_id]= %d", netInterface,
            socket_id, s_LTEDetached[socket_id]);
    while (!setup_success && !s_LTEDetached[socket_id]) {
        char rawaddrstr[INET6_ADDRSTRLEN], addrstr[INET6_ADDRSTRLEN];
        unsigned int prefixlen;
        int i, j;
        char ifname[NET_INTERFACE_LENGTH];  // Currently, IFNAMSIZ = 16.
        FILE *f = fopen("/proc/net/if_inet6", "r");
        if (!f) {
            return -errno;
        }

        // Format:
        // 20010db8000a0001fc446aa4b5b347ed 03 40 00 01    wlan0
        while (fscanf(f, "%32s %*02x %02x %*02x %*02x %63s\n", rawaddrstr,
                &prefixlen, ifname) == 3) {
            // Is this the interface we're looking for?
            if (strcmp(netInterface, ifname)) {
                continue;
            }

            // Put the colons the address
            // and add ':' to separate every 4 addr char
            for (i = 0, j = 0; i < ipv6AddrLen; i++, j++) {
                addrstr[j] = rawaddrstr[i];
                if (i % 4 == 3) {
                    addrstr[++j] = ':';
                }
            }
            addrstr[j - 1] = '\0';
            RLOGD("getipv6addr found ip %s", addrstr);
            // Don't add the link-local address
            if (strncmp(addrstr, "fe80:", sizeof("fe80:") - 1) == 0) {
                RLOGD("getipv6addr found fe80");
                continue;
            }
            snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_ip %s/64", prop,
                    ethIndex, addrstr);
            system(cmd);
            RLOGD("getipv6addr propset %s ", cmd);
            setup_success = 1;
            break;
        }

        fclose(f);
        if (!setup_success) {
            usleep(100 * 1000);
            retry++;
        }
        if (retry == maxRetry) {
            break;
        }
    }
    return setup_success;
}

IPType readIPAddr(char *raw, char *rsp) {
    int comma_count = 0;
    int num = 0, comma4_num = 0, comma16_num = 0;
    int space_num = 0;
    char *buf = raw;
    int len = 0;
    int ip_type = UNKNOWN;

    if (raw != NULL) {
        len = strlen(raw);
        for (num = 0; num < len; num++) {
            if (raw[num] == '.') {
                comma_count++;
            }

            if (raw[num] == ' ') {
                space_num = num;
                break;
            }

            if (comma_count == 4 && comma4_num == 0) {
                comma4_num = num;
            }

            if (comma_count > 7 && comma_count == 16) {
                comma16_num = num;
                break;
            }
        }

        if (space_num > 0) {
            buf[space_num] = '\0';
            ip_type = IPV6;
            memcpy(rsp, buf, strlen(buf) + 1);
        } else if (comma_count >= 7) {
            if (comma_count == 7) {  // ipv4
                buf[comma4_num] = '\0';
                ip_type = IPV4;
            } else {  // ipv6
                buf[comma16_num] = '\0';
                ip_type = IPV6;
            }
            memcpy(rsp, buf, strlen(buf) + 1);
        }
    }

    return ip_type;
}

void resetDNS2(char *out, size_t dataLen) {
    if (strlen(s_SavedDns) > 0) {
        RLOGD("Use saved DNS2 instead.");
        memcpy(out, s_SavedDns, sizeof(s_SavedDns));
    } else {
        RLOGD("Use default DNS2 instead.");
        snprintf(out, dataLen, "%s", DEFAULT_PUBLIC_DNS2);
    }
}

/* for AT+CGDCONT? read response process */
void cgdcont_read_cmd_rsp(ATResponse *p_response, ATResponse **pp_outResponse) {
    int err = -1;
    int tmpCid = 0;
    int respLen = 0;
    char *out = NULL;
    char atCmdStr[AT_RESPONSE_LEN] = {0};
    char ip[IP_ADDR_MAX] = {0}, net[IP_ADDR_MAX] = {0};

    if (p_response == NULL) {
        return;
    }

    char *line = NULL;
    ATLine *p_cur = NULL;
    ATResponse *sp_response = at_response_new();
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        line = p_cur->line;
        respLen = strlen(line);
        snprintf(atCmdStr, sizeof(atCmdStr), "%s", line);
        if (findInBuf(line, respLen, "+CGDCONT")) {
            do {
                err = at_tok_start(&line);
                if (err < 0) break;

                err = at_tok_nextint(&line, &tmpCid);
                if (err < 0) break;

                err = at_tok_nextstr(&line, &out);  // ip
                if (err < 0) break;

                snprintf(ip, sizeof(ip), "%s", out);

                err = at_tok_nextstr(&line, &out);  // cmnet
                if (err < 0) break;

                snprintf(net, sizeof(net), "%s", out);

                if (tmpCid < MAX_PDP_NUM) {
                    if (pdp_info[tmpCid - 1].state == PDP_STATE_ACTIVE) {
                        if (pdp_info[tmpCid - 1].manual_dns == 1) {
                            snprintf(atCmdStr, sizeof(atCmdStr),
                                "+CGDCONT:%d,\"%s\",\"%s\",\"%s\",0,0,\"%s\",\"%s\"\r",
                                tmpCid, ip, net, pdp_info[tmpCid].ipladdr,
                                pdp_info[tmpCid - 1].userdns1addr,
                                pdp_info[tmpCid - 1].userdns2addr);
                        } else {
                            snprintf(atCmdStr, sizeof(atCmdStr),
                                "+CGDCONT:%d,\"%s\",\"%s\",\"%s\",0,0,\"%s\",\"%s\"\r",
                                tmpCid, ip, net, pdp_info[tmpCid - 1].ipladdr,
                                pdp_info[tmpCid - 1].dns1addr,
                                pdp_info[tmpCid - 1].dns2addr);
                        }
                    } else {
                        snprintf(atCmdStr, sizeof(atCmdStr),
                                "+CGDCONT:%d,\"%s\",\"%s\",\"%s\",0,0,\"%s\",\"%s\"\r",
                                tmpCid, ip, net, "0.0.0.0", "0.0.0.0", "0.0.0.0");
                    }
                }
            } while (0);
        }
        reWriteIntermediate(sp_response, atCmdStr);
    }

    if (pp_outResponse == NULL) {
        at_response_free(sp_response);
    } else {
        reverseNewIntermediates(sp_response);
        *pp_outResponse = sp_response;
    }
}

/* for AT+CGDCONT= set command response process */
int cgdcont_set_cmd_req(char *cmd, char *newCmd) {
    int tmpCid = 0;
    char *input = cmd;
    char ip[IP_ADDR_MAX], net[IP_ADDR_MAX],
         ipladdr[IP_ADDR_MAX], hcomp[IP_ADDR_MAX], dcomp[IP_ADDR_MAX];
    char *out = NULL;
    int err = 0;
    int maxPDPNum = MAX_PDP_NUM;
    char prop_PsAttReqIpv4MTU[PROPERTY_VALUE_MAX] = {0};

    if (cmd == NULL || newCmd == NULL) {
        goto error;
    }

    memset(ip, 0, IP_ADDR_MAX);
    memset(net, 0, IP_ADDR_MAX);
    memset(ipladdr, 0, IP_ADDR_MAX);
    memset(hcomp, 0, IP_ADDR_MAX);
    memset(dcomp, 0, IP_ADDR_MAX);

    err = at_tok_flag_start(&input, '=');
    if (err < 0) goto error;

    err = at_tok_nextint(&input, &tmpCid);
    if (err < 0) goto error;

    err = at_tok_nextstr(&input, &out);  // ip
    if (err < 0) goto exit;

    snprintf(ip, sizeof(ip), "%s", out);

    err = at_tok_nextstr(&input, &out);  // cmnet
    if (err < 0) goto exit;

    snprintf(net, sizeof(net), "%s", out);

    err = at_tok_nextstr(&input, &out);  // ipladdr
    if (err < 0) goto exit;

    snprintf(ipladdr, sizeof(ipladdr), "%s", out);

    err = at_tok_nextstr(&input, &out);  // dcomp
    if (err < 0) goto exit;

    snprintf(dcomp, sizeof(dcomp), "%s", out);

    err = at_tok_nextstr(&input, &out);  // hcomp
    if (err < 0) goto exit;

    snprintf(hcomp, sizeof(hcomp), "%s", out);

    // cp dns to pdp_info ?
    if (tmpCid <= maxPDPNum) {
        strncpy(pdp_info[tmpCid - 1].userdns1addr, "0.0.0.0",
                 sizeof("0.0.0.0"));
        strncpy(pdp_info[tmpCid - 1].userdns2addr, "0.0.0.0",
                 sizeof("0.0.0.0"));
        pdp_info[tmpCid - 1].manual_dns = 0;
    }

    // dns1, info used with cgdata
    err = at_tok_nextstr(&input, &out);
    if (err < 0) goto exit;

    if (tmpCid <= maxPDPNum && *out != 0) {
        strncpy(pdp_info[tmpCid - 1].userdns1addr, out,
                sizeof(pdp_info[tmpCid - 1].userdns1addr));
        pdp_info[tmpCid - 1].userdns1addr[
                sizeof(pdp_info[tmpCid - 1].userdns1addr) - 1] = '\0';
    }

    // dns2, info used with cgdata
    err = at_tok_nextstr(&input, &out);
    if (err < 0) goto exit;

    if (tmpCid <= maxPDPNum && *out != 0) {
        strncpy(pdp_info[tmpCid - 1].userdns2addr, out,
                sizeof(pdp_info[tmpCid - 1].userdns2addr));
        pdp_info[tmpCid - 1].userdns2addr[
                sizeof(pdp_info[tmpCid - 1].userdns2addr)- 1] = '\0';
    }

    // cp dns to pdp_info?
exit:
    if (tmpCid <= maxPDPNum) {
        if (strncasecmp(pdp_info[tmpCid - 1].userdns1addr, "0.0.0.0",
                strlen("0.0.0.0"))) {
            pdp_info[tmpCid - 1].manual_dns = 1;
        }
    }
    property_get(PS_ATTACH_REQUEST_IPV4_MTU, prop_PsAttReqIpv4MTU, "0");
    RLOGD("cgdcont_set_cmd_req persist.vendor.set.ipv4mtu: %s, protocol:%s", prop_PsAttReqIpv4MTU, ip);
    if (!strcmp(prop_PsAttReqIpv4MTU, "1")) {
        RLOGD("set ipv4mtu");
        snprintf(newCmd, AT_COMMAND_LEN,
                "AT+CGDCONT=%d,\"%s\",\"%s\",\"%s\",%s,%s,0,0,1\r", tmpCid, ip, net,
                ipladdr, dcomp, hcomp);
    } else {
        snprintf(newCmd, AT_COMMAND_LEN,
                "AT+CGDCONT=%d,\"%s\",\"%s\",\"%s\",%s,%s\r", tmpCid, ip, net,
                ipladdr, dcomp, hcomp);
    }
    return AT_RESULT_OK;

error:
    return AT_RESULT_NG;
}

/* for AT+CGDATA= set command process */
int cgdata_set_cmd_req(char *cgdataCmd) {
    int cid = 0;
    int pdpIndex = 0;
    int err = -1;
    char *cmdStr = NULL;
    char *out = NULL;
    char atBuffer[AT_RESPONSE_LEN] = {0};

    if (cgdataCmd == NULL || strlen(cgdataCmd) <= 0) {
        goto error;
    }

    cmdStr = atBuffer;
    snprintf(cmdStr, sizeof(atBuffer), "%s", cgdataCmd);

    err = at_tok_flag_start(&cmdStr, '=');
    if (err < 0) goto error;

    /* get L2P */
    err = at_tok_nextstr(&cmdStr, &out);
    if (err < 0) goto error;

    /* Get cid */
    err = at_tok_nextint(&cmdStr, &cid);
    if (err < 0) goto error;

    pdpIndex = cid - 1;
    pthread_mutex_lock(&s_psServiceMutex);
    pdp_info[pdpIndex].state = PDP_STATE_ACTING;
    pdp_info[pdpIndex].cid = cid;
    pdp_info[pdpIndex].error_num = -1;
    pthread_mutex_unlock(&s_psServiceMutex);
    return AT_RESULT_OK;

error:
    return AT_RESULT_NG;
}

void init_socket(int index) {
    if (index < 0 || index >= MAX_PDP) {
        RLOGE("Invalid index: %d", index);
        return;
    }

    s_fdSocketV4[index] = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_fdSocketV4[index] < 0) {
        RLOGE("Couldn't create IP socket: errno = %d", errno);
    } else {
        RLOGD("Allocate sock_fd = %d, for cid = %d", s_fdSocketV4[index],
                index + 1);
    }

    s_fdSocketV6[index] = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s_fdSocketV6[index] < 0) {
        RLOGE("Couldn't create IPv6 socket: errno = %d", errno);
    } else {
        RLOGD("Allocate sock6_fd = %d, for cid = %d", s_fdSocketV6[index],
                index + 1);
    }
}

void close_socket(int index) {
    if (index < 0 || index >= MAX_PDP) {
        RLOGE("Invalid index: %d", index);
        return;
    }

    close(s_fdSocketV4[index]);
    close(s_fdSocketV6[index]);
    s_fdSocketV4[index] = -1;
    s_fdSocketV6[index] = -1;
}

void ifupdown(int s, struct ifreq *ifr, int active) {
    if (ioctl(s, SIOCGIFFLAGS, ifr) < 0) {
        RLOGE("get interface state failed");
        goto done;
    }
    RLOGD("set interface state from %d to %d", ifr->ifr_flags, active);
    if (active) {
        if (ifr->ifr_flags & IFF_UP) {
            RLOGD("interface is already up");
        }
        ifr->ifr_flags |= IFF_UP;
    } else {
        if (!(ifr->ifr_flags & IFF_UP)) {
            RLOGD("interface is already down");
        }
        ifr->ifr_flags &= ~IFF_UP;
    }
    RLOGD("set interface state: %d", ifr->ifr_flags);
    if (ioctl(s, SIOCSIFFLAGS, ifr) < 0) {
        RLOGE("Set SIOCSIFFLAGS Error!");
        goto done;
    }
done:
    return;
}

void set_ipv4_addr(int socket, struct ifreq *ifr, const char *addr) {
    RLOGD("set IPv4 adress: %s", addr);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = addr ? inet_addr(addr) : INADDR_ANY;
    if (ioctl(socket, SIOCSIFADDR, ifr) < 0) {
        RLOGE("set_ipv4_addr failed.");
    }
}

void set_ipv6_addr(int socket, struct ifreq *ifr, const char *addr) {
    RLOGD("set IPv6 adress: %s", addr);
    struct in6_ifreq ifr_v6;
    int ret = 0;

    ret = ioctl(socket, SIOCGIFINDEX, ifr);
    if (ret < 0) {
        goto error;
    }

    memset(&ifr_v6, 0, sizeof(ifr_v6));

    if (addr != NULL) {
        inet_pton(AF_INET6, addr, &ifr_v6.ifr6_addr);
        ifr_v6.ifr6_prefixlen = 64;
        ifr_v6.ifr6_ifindex = ifr->ifr_ifindex;
    }

    ret = ioctl(socket, SIOCSIFADDR, &ifr_v6);
    if (ret < 0) {
        goto error;
    }
    return;

error:
    RLOGE("setIpv6AddrToNwInterface failed: %d - %d: %s", ret, errno,
            strerror(errno));
    return;
}

int downNetcard(int cid, char *netinterface, RIL_SOCKET_ID socket_id) {
    int index = cid - 1;
    int isAutoTest = 0;
    char linker[AT_COMMAND_LEN] = {0};
    char cmd[AT_COMMAND_LEN] = {0};
    char gspsprop[PROPERTY_VALUE_MAX] = {0};
    struct ifreq ifr;

    int ethIndex = getEthIndexBySocketId(socket_id, cid);

    if (cid < 1 || cid >= MAX_PDP_NUM || netinterface == NULL ||
            ethIndex < 0 || ethIndex >= MAX_ETH) {
        return 0;
    }

    RLOGD("down cid %d, network interface %s ", cid, netinterface);
    snprintf(linker, sizeof(linker), "%s%d", netinterface, ethIndex);

    property_get(GSPS_ETH_DOWN_PROP, gspsprop, "0");
    isAutoTest = atoi(gspsprop);
    snprintf(cmd, sizeof(cmd), "ext_data<ifdown>%s;%s;%d", linker, "IPV4V6",
             isAutoTest);
    sendCmdToExtData(cmd);

    property_set(GSPS_ETH_DOWN_PROP, "0");

    RLOGD("sleep 400ms before disable the linker(%s),waiting for sockets to be closed \n", linker);
    usleep(400000);

    init_socket(index);
    memset(&ifr, 0, sizeof(struct ifreq));
    sprintf(ifr.ifr_name, "%s", linker);
    ifupdown(s_fdSocketV4[index], &ifr, 0);
    set_ipv4_addr(s_fdSocketV4[index], &ifr, NULL);
    set_ipv6_addr(s_fdSocketV6[index], &ifr, NULL);
    close_socket(index);

    RLOGD("data_off execute done");
    return 1;
}

int dispose_data_fallback(int masterCid, int secondaryCid, RIL_SOCKET_ID socket_id) {
    int master_index = masterCid - 1;
    int secondary_index = secondaryCid - 1;
    int masterEthIndex = -1;
    char cmd[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};

    if (masterCid < 1|| masterCid >= MAX_PDP_NUM || secondaryCid <1 ||
        secondaryCid >= MAX_PDP_NUM) {
    // 1~11 is valid cid
        return 0;
    }
    masterEthIndex = getEthIndexBySocketId(socket_id, masterCid);
    property_get(MODEM_ETH_PROP, prop, "veth");
    RLOGD("master ip type %d, secondary ip type %d",
             pdp_info[master_index].ip_state,
             pdp_info[secondary_index].ip_state);
    // fallback get same type ip with master
    if (pdp_info[master_index].ip_state ==
        pdp_info[secondary_index].ip_state) {
        return 0;
    }
    if (pdp_info[master_index].ip_state == IPV4) {
        // down ipv4, because need set ipv6 firstly
        downNetcard(masterCid, prop, socket_id);
        // copy secondary ppp to master ppp
        memcpy(pdp_info[master_index].ipv6laddr,
                pdp_info[secondary_index].ipv6laddr,
                sizeof(pdp_info[master_index].ipv6laddr));
        memcpy(pdp_info[master_index].ipv6dns1addr,
                pdp_info[secondary_index].ipv6dns1addr,
                sizeof(pdp_info[master_index].ipv6dns1addr));
        memcpy(pdp_info[master_index].ipv6dns2addr,
                pdp_info[secondary_index].ipv6dns2addr,
                sizeof(pdp_info[master_index].ipv6dns2addr));
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_ip %s", prop,
                masterEthIndex, pdp_info[master_index].ipv6laddr);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_dns1 %s", prop,
                masterEthIndex, pdp_info[master_index].ipv6dns1addr);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_dns2 %s", prop,
                masterEthIndex, pdp_info[master_index].ipv6dns2addr);
        system(cmd);
    } else if (pdp_info[master_index].ip_state == IPV6) {
        // copy secondary ppp to master ppp
        memcpy(pdp_info[master_index].ipladdr,
                pdp_info[secondary_index].ipladdr,
                sizeof(pdp_info[master_index].ipladdr));
        memcpy(pdp_info[master_index].dns1addr,
                pdp_info[secondary_index].dns1addr,
                sizeof(pdp_info[master_index].dns1addr));
        memcpy(pdp_info[master_index].dns2addr,
                pdp_info[secondary_index].dns2addr,
                sizeof(pdp_info[master_index].dns2addr));
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip %s", prop,
                masterEthIndex, pdp_info[master_index].ipladdr);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.dns1 %s", prop,
                masterEthIndex, pdp_info[master_index].dns1addr);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.dns2 %s", prop,
                masterEthIndex, pdp_info[secondary_index].dns2addr);
        system(cmd);
    }
    snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip_type %d", prop,
            masterEthIndex, IPV4V6);
    system(cmd);
    pdp_info[master_index].ip_state = IPV4V6;
    return 1;
}

int write_file(const char *path, const char *value) {
    int fd = -1, len = 0;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        RLOGE("open file %s fail: %s", path, strerror(errno));
        return -1;
    }

    len = strlen(value);
    if (write(fd, value, len) != len) {
        RLOGE("write %s to file %s fail: %s", value, path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    RLOGD("write %s to file %s ok", value, path);
    return 0;
}

#if 0
static int get_iface_v6addr(const char *netInterface) {
    const int maxRetry = 120;  // wait 12s
    int retry = 0;
    int setup_success = 0;
    uint32_t ifindex = -1;
    int rt_sockfd = -1;
    char buf[1080] = {0};
    struct iovec iov;
    memset(&iov, 0, sizeof(struct iovec));

    struct {
        struct nlmsghdr nlhdr;
        struct ifaddrmsg addrmsg;
    } msg;

    struct ifaddrmsg *retaddr = NULL;
    struct nlmsghdr *retmsg = NULL;
    uint32_t len = -1;

    RLOGD("query interface %s", netInterface);

    rt_sockfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (rt_sockfd < 0) {
        RLOGE("netlink route create fail: %s", strerror(errno));
        return -errno;
    }
    while (!setup_success) {
        msg.nlhdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
        msg.nlhdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
        msg.nlhdr.nlmsg_type = RTM_GETADDR;
        msg.addrmsg.ifa_family = AF_INET6;

        iov.iov_base = &msg;
        iov.iov_len = sizeof(msg);
        if (writev(rt_sockfd, &iov, 1) < 0) {
            RLOGE("get_iface_addr writev fail: %s", strerror(errno));
            close(rt_sockfd);
            rt_sockfd = -1;
            return -errno;
        }

        len = recv(rt_sockfd, &buf, sizeof(buf), 0);
        retmsg = (struct nlmsghdr *) buf;
        while (NLMSG_OK(retmsg, len)) {
            int attlen = -1;
            struct rtattr *retrta = NULL;

            retaddr = (struct ifaddrmsg *)NLMSG_DATA(retmsg);
            ifindex = if_nametoindex(netInterface);
            if (retaddr->ifa_index == ifindex && retaddr->ifa_scope == RT_SCOPE_UNIVERSE) {
                retrta = (struct rtattr *)IFA_RTA(retaddr);
                attlen = IFA_PAYLOAD(retmsg);
                while (RTA_OK(retrta, attlen)) {
                    if (retrta->rta_type == IFA_ADDRESS) {
                        char pradd[AT_COMMAND_LEN] = {0};
                        char cmd[AT_COMMAND_LEN] = {0};

                        inet_ntop(AF_INET6, RTA_DATA(retrta), pradd, sizeof(pradd));
                        RLOGD("retaddr prefix %d, family %d, index %d, scope %d, pradd %s",
                                 retaddr->ifa_prefixlen, retaddr->ifa_family,
                                 retaddr->ifa_index, retaddr->ifa_scope, pradd);

                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s.ipv6_ip %s/%d",
                                netInterface, pradd, retaddr->ifa_prefixlen);
                        system(cmd);
                        RLOGD("getipv6addr propset %s ", cmd);
                        setup_success = 1;
                    }
                    retrta = RTA_NEXT(retrta, attlen);
                }
            }
            retmsg = NLMSG_NEXT(retmsg, len);
        }
        if (!setup_success) {
            usleep(100 * 1000);
            retry++;
        }
        if (retry == maxRetry) {
            break;
        }
    }

    close(rt_sockfd);
    rt_sockfd = -1;
    return setup_success;
}
#endif

/*
 * return value: 1: success
 *               0: getIpv6 header 64bit failed
 */
static int upNetInterface(int cidIndex, IPType ipType, RIL_SOCKET_ID socket_id) {
    char linker[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    char gspsprop[PROPERTY_VALUE_MAX] = {0};
    char cmd[AT_COMMAND_LEN];
    IPType actIPType = ipType;
    int isAutoTest = 0;
    int err = -1;

    char ip[IP_ADDR_MAX], ip2[IP_ADDR_MAX], dns1[IP_ADDR_MAX], dns2[IP_ADDR_MAX];
    memset(ip, 0, sizeof(ip));
    memset(ip2, 0, sizeof(ip2));
    memset(dns1, 0, sizeof(dns1));
    memset(dns2, 0, sizeof(dns2));
    property_get(MODEM_ETH_PROP, prop, "veth");

    /* set net interface name */
    snprintf(linker, sizeof(linker), "%s%d", prop, getEthIndexBySocketId( socket_id, cidIndex + 1));
    property_set("ril.sys.usb.tether.iface", linker);
    RLOGD("Net interface addr linker = %s", linker);

    property_get(GSPS_ETH_UP_PROP, gspsprop, "0");
    RLOGD("GSPS up prop = %s", gspsprop);
    isAutoTest = atoi(gspsprop);
    RLOGD("ipType = %d", ipType);

    init_socket(cidIndex);
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(struct ifreq));
    sprintf(ifr.ifr_name, "%s", linker);

    if (ipType != IPV4) {
        actIPType = IPV6;
    }
    do {
        snprintf(cmd, sizeof(cmd), "ext_data<preifup>%s;%s;%d", linker,
                  actIPType == IPV4 ? "IPV4" : "IPV6", isAutoTest);
        sendCmdToExtData(cmd);

        if (ifc_set_noarp(linker)) {
            RLOGE("ifc_set_noarp %s fail: %s", linker, strerror(errno));
        }

        //GCF test no need to config ip addr
        if (actIPType != IPV4) {
            ifupdown(s_fdSocketV6[cidIndex], &ifr, 1);
            if (!s_isGCFTest) {
                set_ipv6_addr(s_fdSocketV6[cidIndex], &ifr, pdp_info[cidIndex].ipv6laddr);
            }
        } else {
            ifupdown(s_fdSocketV4[cidIndex], &ifr, 1);
            if (!s_isGCFTest) {
                set_ipv4_addr(s_fdSocketV4[cidIndex], &ifr, pdp_info[cidIndex].ipladdr);
            }
        }

        snprintf(cmd, sizeof(cmd), "ext_data<ifup>%s;%s;%d", linker,
                  actIPType == IPV4 ? "IPV4" : "IPV6", isAutoTest);
        sendCmdToExtData(cmd);

        /* Get IPV6 Header 64bit */
        if (actIPType != IPV4) {
            if (!getIPV6Addr(prop, getEthIndexBySocketId(socket_id, cidIndex + 1), socket_id)) {
                RLOGD("get IPv6 address timeout, actIPType = %d", actIPType);
                if (ipType == IPV4V6) {
                    pdp_info[cidIndex].ip_state = IPV4;
                } else {
                    close_socket(cidIndex);
                    return 0;
                }
            }
        }

        /* if IPV4V6 actived, need set IPV4 again */
        if (ipType == IPV4V6 && actIPType != IPV4) {
            actIPType = IPV4;
        } else {
            break;
        }
    } while (ipType == IPV4V6);

    char bip[PROPERTY_VALUE_MAX];
    memset(bip, 0, sizeof(bip));
    property_get(BIP_OPENCHANNEL, bip, "0");
    if (strcmp(bip, "1") == 0) {
        if (ipType == IPV4) {
            in_addr_t address = inet_addr(pdp_info[cidIndex].ipladdr);
            err = ifc_create_default_route(linker, address);
            RLOGD("ifc_create_default_route address = %d, error = %d", address, err);
        } else if (ipType == IPV6) {
            property_set("persist.vendor.sys.bip.ipv6_addr", pdp_info[cidIndex].ipv6laddr);
            int tableIndex = 0;
            ifc_init();
            RLOGD("linker = %s", linker);
            err = ifc_get_ifindex(linker, &tableIndex);
            RLOGD("index = %d, error = %d", tableIndex, err);
            ifc_close();
            tableIndex = tableIndex + 1000;
            sprintf(cmd, "setprop persist.vendor.sys.bip.table_index %d", tableIndex);
            system(cmd);
            property_set("ctl.start", "vendor.stk");
        }
    }
    close_socket(cidIndex);

    /*call ppp route .sh*/
    if (s_isGCFTest) {
        char startpppd[256] = {0};

        //use default nds for DNS1 when it is empty in instrument test
        if (strlen(pdp_info[cidIndex].dns1addr) < 7) {
            snprintf(startpppd, sizeof(startpppd), "ext_data<startpppd>ttyGS0;192.168.168.1;%s;%s;%s",
                        pdp_info[cidIndex].ipladdr, DEFAULT_PUBLIC_DNS2, pdp_info[cidIndex].dns2addr);
        } else {
            snprintf(startpppd, sizeof(startpppd), "ext_data<startpppd>ttyGS0;192.168.168.1;%s;%s;%s",
                        pdp_info[cidIndex].ipladdr, pdp_info[cidIndex].dns1addr, pdp_info[cidIndex].dns2addr);
        }

        usleep(500 * 1000);
        RLOGD("start pppd! cmd = %s.", startpppd);
        sendCmdToExtData(startpppd);
        s_isPPPDStart = true;

        usleep(500 * 1000);
        //eg:<pppup>seth_lte0;10.10.10.10
        snprintf(cmd, sizeof(cmd), "ext_data<pppup>%s;%s", linker, pdp_info[cidIndex].ipladdr);
        sendCmdToExtData(cmd);

        RLOGD("cmd to set ppp route : [%s]", cmd);
    }

    return 1;
}

int cgcontrdp_set_cmd_rsp(ATResponse *p_response, RIL_SOCKET_ID socket_id) {
    int err = -1;
    char *input = NULL;
    int cid = 0;
    char *local_addr_subnet_mask = NULL;
    char *dns_prim_addr = NULL, *dns_sec_addr = NULL;
    char ip[IP_ADDR_SIZE * 4], dns1[IP_ADDR_SIZE * 4], dns2[IP_ADDR_SIZE * 4];
    char cmd[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    char *sskip = NULL;
    char *tmp = NULL;
    int skip = 0;
    int ip_type_num = 0;
    int ip_type = 0;
    int maxPDPNum = MAX_PDP_NUM;
    ATLine *p_cur = NULL;
    char *gw_addr = NULL;

    if (p_response == NULL) {
        RLOGE("leave cgcontrdp_set_cmd_rsp:AT_RESULT_NG");
        return AT_RESULT_NG;
    }

    memset(ip, 0, sizeof(ip));
    memset(dns1, 0, sizeof(dns1));
    memset(dns2, 0, sizeof(dns2));

    property_get(MODEM_ETH_PROP, prop, "veth");

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        input = p_cur->line;
        if (findInBuf(input, strlen(p_cur->line), "+CGCONTRDP") ||
            findInBuf(input, strlen(p_cur->line), "+SIPCONFIG")) {
            do {
                err = at_tok_flag_start(&input, ':');
                if (err < 0) break;

                err = at_tok_nextint(&input, &cid);  // cid
                if (err < 0) break;

                err = at_tok_nextint(&input, &skip);  // bearer_id
                if (err < 0) break;

                err = at_tok_nextstr(&input, &sskip);  // apn
                if (err < 0) break;

                if (at_tok_hasmore(&input)) {
                    // local_addr_and_subnet_mask
                    err = at_tok_nextstr(&input, &local_addr_subnet_mask);
                    if (err < 0) break;

                    if (at_tok_hasmore(&input)) {
                        err = at_tok_nextstr(&input, &gw_addr);  // gw_addr
                        if (err < 0) break;

                        if (at_tok_hasmore(&input)) {
                            // dns_prim_addr
                            err = at_tok_nextstr(&input, &dns_prim_addr);
                            if (err < 0) break;

                            snprintf(dns1, sizeof(dns1), "%s", dns_prim_addr);

                            if (at_tok_hasmore(&input)) {
                                // dns_sec_addr
                                err = at_tok_nextstr(&input, &dns_sec_addr);
                                if (err < 0) break;

                                snprintf(dns2, sizeof(dns2), "%s", dns_sec_addr);
                            }
                        }
                    }
                }

                if ((cid < maxPDPNum) && (cid >= 1)) {
                    int ethIndex = getEthIndexBySocketId(socket_id, cid);
                    ip_type = readIPAddr(local_addr_subnet_mask, ip);
                    RLOGD("PS:cid = %d,ip_type = %d,ip = %s,dns1 = %s,dns2 = %s",
                             cid, ip_type, ip, dns1, dns2);

                    if (ip_type == IPV6) {  // ipv6
                        if (s_isGCFTest) {
                            RLOGD("GCFTest don't need ipv6 addr, break");
                            break;
                        }

                        RLOGD("cgcontrdp_set_cmd_rsp: IPV6");
                        if (!strncasecmp(ip, "0000:0000:0000:0000",
                                strlen("0000:0000:0000:0000"))) {
                            // incomplete address
                            tmp = strchr(ip, ':');
                            if (tmp != NULL) {
                                snprintf(ip, sizeof(ip), "FE80%s", tmp);
                            }
                        }
                        memcpy(pdp_info[cid - 1].ipv6laddr, ip,
                                sizeof(pdp_info[cid - 1].ipv6laddr));
                        memcpy(pdp_info[cid - 1].ipv6dns1addr, dns1,
                                sizeof(pdp_info[cid - 1].ipv6dns1addr));

                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip_type %d",
                                  prop, ethIndex, IPV6);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_ip %s",
                                  prop, ethIndex, ip);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ipv6_gw %s",
                                  prop, ethIndex, gw_addr);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd),
                                  "setprop vendor.net.%s%d.ipv6_dns1 %s", prop, ethIndex,
                                  dns1);
                        system(cmd);
                        if (strlen(dns2) != 0) {
                            if (!strcmp(dns1, dns2)) {
                                if (strlen(s_SavedDns_IPV6) > 0) {
                                    RLOGD("Use saved DNS2 instead.");
                                    memcpy(dns2, s_SavedDns_IPV6,
                                            sizeof(s_SavedDns_IPV6));
                                } else {
                                    RLOGD("Use default DNS2 instead.");
                                    snprintf(dns2, sizeof(dns2), "%s",
                                              DEFAULT_PUBLIC_DNS2_IPV6);
                                }
                            } else {
                                RLOGD("Backup DNS2");
                                memset(s_SavedDns_IPV6, 0,
                                        sizeof(s_SavedDns_IPV6));
                                memcpy(s_SavedDns_IPV6, dns2, sizeof(dns2));
                            }
                        } else {
                            RLOGD("DNS2 is empty!!");
                            memset(dns2, 0, IP_ADDR_SIZE * 4);
                            snprintf(dns2, sizeof(dns2), "%s",
                                      DEFAULT_PUBLIC_DNS2_IPV6);
                        }
                        memcpy(pdp_info[cid - 1].ipv6dns2addr, dns2,
                                sizeof(pdp_info[cid - 1].ipv6dns2addr));
                        snprintf(cmd, sizeof(cmd),
                                  "setprop vendor.net.%s%d.ipv6_dns2 %s", prop, ethIndex,
                                  dns2);
                        system(cmd);

                        pdp_info[cid - 1].ip_state = IPV6;
                        ip_type_num++;
                    } else if (ip_type == IPV4) {  // ipv4
                        RLOGD("cgcontrdp_set_cmd_rsp: IPV4");
                        memcpy(pdp_info[cid - 1].ipladdr, ip,
                                sizeof(pdp_info[cid - 1].ipladdr));
                        memcpy(pdp_info[cid - 1].dns1addr, dns1,
                                sizeof(pdp_info[cid - 1].dns1addr));

                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip_type %d",
                                  prop, ethIndex, IPV4);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip %s", prop,
                                ethIndex, ip);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.gw %s",
                                  prop, ethIndex, gw_addr);
                        system(cmd);
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.dns1 %s",
                                  prop, ethIndex, dns1);
                        system(cmd);
                        if (strlen(dns2) != 0) {
                            if (!strcmp(dns1, dns2)) {
                                RLOGD("Two DNS are the same, so need to reset"
                                         "dns2!!");
                                resetDNS2(dns2, sizeof(dns2));
                            } else {
                                RLOGD("Backup DNS2");
                                memset(s_SavedDns, 0, sizeof(s_SavedDns));
                                memcpy(s_SavedDns, dns2, IP_ADDR_SIZE);
                            }
                        } else {
                            RLOGD("DNS2 is empty!!");
                            memset(dns2, 0, IP_ADDR_SIZE);
                            resetDNS2(dns2, sizeof(dns2));
                        }
                        memcpy(pdp_info[cid - 1].dns2addr, dns2,
                                sizeof(pdp_info[cid - 1].dns2addr));
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.dns2 %s",
                                  prop, ethIndex, dns2);
                        system(cmd);

                        pdp_info[cid - 1].ip_state = IPV4;
                        ip_type_num++;
                    } else {  // unknown
                        pdp_info[cid - 1].state = PDP_STATE_EST_UP_ERROR;
                        RLOGD("PDP_STATE_EST_UP_ERROR: unknown ip type!");
                    }

                    if (ip_type_num > 1) {
                        RLOGD("cgcontrdp_set_cmd_rsp is IPV4V6, s_pdpType = %d", s_pdpType);
                        pdp_info[cid - 1].ip_state = s_pdpType;
                        snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip_type %d",
                                 prop, ethIndex, s_pdpType);
                        system(cmd);
                        s_pdpType = IPV4V6;
                    }
                    pdp_info[cid - 1].state = PDP_STATE_ACTIVE;
                    RLOGD("PDP_STATE_ACTIVE");
                }
            } while (0);
        }
    }
    return AT_RESULT_OK;
}

void checkIpv6Dns(RIL_SOCKET_ID socket_id, int cid) {
    IPType ipType = UNKNOWN;
    char cmd[AT_COMMAND_LEN] = {0};
    char ethProp[PROPERTY_VALUE_MAX] = {0};
    char ipTypeProp[PROPERTY_VALUE_MAX] = {0};
    char dns1[PROPERTY_VALUE_MAX] = {0};
    char dns2[PROPERTY_VALUE_MAX] = {0};
    property_get(MODEM_ETH_PROP, ethProp, "veth");

    snprintf(cmd, sizeof(cmd), "vendor.net.%s%d.ip_type", ethProp, cid - 1);
    property_get(cmd, ipTypeProp, "0");
    ipType = atoi(ipTypeProp);
    snprintf(cmd, sizeof(cmd),"vendor.net.%s%d.ipv6_dns1", ethProp, cid - 1);
    property_get(cmd, dns1, "");
    snprintf(cmd, sizeof(cmd),"vendor.net.%s%d.ipv6_dns2", ethProp, cid - 1);
    property_get(cmd, dns2, "");

    if (ipType == IPV6 &&
            (!strcmp(dns1, "0000:0000:0000:0000:0000:0000:0000:0000") ||
                    strlen(dns1) == 0) &&
            (!strcmp(dns2, "0000:0000:0000:0000:0000:0000:0000:0000") ||
                    !strcmp(dns2, DEFAULT_PUBLIC_DNS2_IPV6))) {
        RLOGD("dns is invalid sent reattach command");
        at_send_command(socket_id, "AT+SPREATTACH", NULL);
    }
}

/* for AT+CGDATA= set command response process */
int cgdata_set_cmd_rsp(ATResponse *p_response, int pdpIndex, int primaryCid,
                       RIL_SOCKET_ID socket_id) {
    int rspType = 0;
    char atCmdStr[AT_COMMAND_LEN] = {0};
    char *input = NULL;
    int err = -1, error_num = 0;
    ATResponse *p_rdpResponse = NULL;

    if (p_response == NULL) {
        return AT_RESULT_NG;
    }
    if (pdpIndex < 0 || pdpIndex >= MAX_PDP_NUM) {
        return AT_RESULT_NG;
    }

    int cid = pdp_info[pdpIndex].cid;

    pthread_mutex_lock(&s_psServiceMutex);
    rspType = getATResponseType(p_response->finalResponse);
    if (rspType == AT_RSP_TYPE_CONNECT) {
        pdp_info[pdpIndex].state = PDP_STATE_CONNECT;
    } else if (rspType == AT_RSP_TYPE_ERROR) {
        RLOGE("PDP activate error");
        pdp_info[pdpIndex].state = PDP_STATE_ACT_ERROR;
        input = p_response->finalResponse;
        if (strStartsWith(input, "+CME ERROR:")) {
            err = at_tok_flag_start(&input, ':');
            if (err >= 0) {
                err = at_tok_nextint(&input, &error_num);
                if (err >= 0) {
                    if (error_num >= 0)
                        pdp_info[pdpIndex].error_num = error_num;
                }
            }
        }
    } else {
        goto error;
    }

    if (pdp_info[pdpIndex].state != PDP_STATE_CONNECT) {
        RLOGE("PDP activate error: %d", pdp_info[pdpIndex].state);
        // p_response->finalResponse stay unchanged
        pdp_info[pdpIndex].state = PDP_STATE_IDLE;
    } else {  // PDP_STATE_CONNECT
        pdp_info[pdpIndex].state = PDP_STATE_ESTING;
        pdp_info[pdpIndex].manual_dns = 0;

        snprintf(atCmdStr, sizeof(atCmdStr), "AT+CGCONTRDP=%d", cid);
        err = at_send_command_multiline(socket_id, atCmdStr, "+CGCONTRDP:",
                &p_rdpResponse);
        if (err == AT_ERROR_TIMEOUT) {
            AT_RESPONSE_FREE(p_rdpResponse);
            RLOGE("Get IP address timeout");
            pdp_info[pdpIndex].state = PDP_STATE_DEACTING;
            snprintf(atCmdStr, sizeof(atCmdStr), "AT+CGACT=0,%d", cid);
            err = at_send_command(socket_id, atCmdStr, NULL);
            if (err == AT_ERROR_TIMEOUT) {
                RLOGE("PDP deactivate timeout");
                goto error;
            }
        } else {
            cgcontrdp_set_cmd_rsp(p_rdpResponse, socket_id);
            checkIpv6Dns(socket_id, cid);
            AT_RESPONSE_FREE(p_rdpResponse);
        }

        if (pdp_info[pdpIndex].state == PDP_STATE_ACTIVE) {
            RLOGD("PS connected successful");

            // if fallback, need map ipv4 and ipv6 to one net device
            if (dispose_data_fallback(primaryCid, cid, socket_id)) {
                cid = primaryCid;
                pdpIndex = cid - 1;
            }

            RLOGD("PS ip_state = %d, socket_id = %d", pdp_info[pdpIndex].ip_state, socket_id);
            if (upNetInterface(pdpIndex, pdp_info[pdpIndex].ip_state, socket_id) == 0) {
                RLOGE("get IPv6 address timeout ");
                goto error;
            }

            RLOGD("data_on execute done");
        }
    }

    pthread_mutex_unlock(&s_psServiceMutex);
    return AT_RESULT_OK;

error:
    pthread_mutex_unlock(&s_psServiceMutex);
    free(p_response->finalResponse);
    p_response->finalResponse = strdup("ERROR");
    return AT_RESULT_NG;
}

/* for AT+CGACT=0 set command response process */
void cgact_deact_cmd_rsp(int cid, RIL_SOCKET_ID socket_id) {
    char cmd[AT_COMMAND_LEN] = {0};
    char prop[PROPERTY_VALUE_MAX] = {0};
    char ipv6_dhcpcd_cmd[AT_COMMAND_LEN] = {0};
    int ethIndex = getEthIndexBySocketId(socket_id, cid);

    pthread_mutex_lock(&s_psServiceMutex);
    /* deactivate PDP connection */
    pdp_info[cid - 1].state = PDP_STATE_IDLE;

//    usleep(200 * 1000);
    property_get(MODEM_ETH_PROP, prop, "veth");

     if (s_isGCFTest && s_isPPPDStart) { //only for GCF test
        RLOGD("stop pppd!");
        s_isPPPDStart = false;
        sendCmdToExtData("ext_data<stoppppd>");
    }

    downNetcard(cid, prop, socket_id);

    if (pdp_info[cid - 1].ip_state == IPV6 ||
        pdp_info[cid - 1].ip_state == IPV4V6) {
        snprintf(ipv6_dhcpcd_cmd, sizeof(ipv6_dhcpcd_cmd),
                "dhcpcd_ipv6:%s%d", prop, ethIndex);
        property_set("ctl.stop", ipv6_dhcpcd_cmd);
    }

    snprintf(cmd, sizeof(cmd), "setprop vendor.net.%s%d.ip_type %d", prop,
            ethIndex, UNKNOWN);
    system(cmd);

    pthread_mutex_unlock(&s_psServiceMutex);
}

int requestSetupDataConnection(RIL_SOCKET_ID socket_id, void *data,
                               size_t datalen) {
    int i;
    int cid = 0;
    const char *pdpType = "IP";
    const char *apn = NULL;
    apn = ((const char **)data)[2];

    if (s_dataAllowed[socket_id] != 1) {
        return -1;
    }
    if (datalen > 6 * sizeof(char *)) {
        pdpType = ((const char **)data)[6];
    } else {
        pdpType = "IP";
    }
    property_set(BIP_OPENCHANNEL, "1");
    s_openchannelCid = -1;
    queryAllActivePDNInfos(socket_id);
    if (s_activePDN > 0) {
        for (i = 0; i < MAX_PDP; i++) {
            cid = getPDNCid(i);
            if (cid == (i + 1)) {
                RLOGD("s_PDP[%d].state = %d", i, getPDPState(socket_id, i));
                if (getPDPState(socket_id, i) == PDP_BUSY &&
                    isApnEqual((char *)apn, getPDNAPN(i)) &&
                    isProtocolEqual((char *)pdpType, getPDNIPType(i))) {
                    if (!(s_openchannelInfo[socket_id][i].pdpState)) {
                        pthread_mutex_lock(&s_signalBipPdpMutex);
                        pthread_cond_wait(&s_signalBipPdpCond, &s_signalBipPdpMutex);
                        pthread_mutex_unlock(&s_signalBipPdpMutex);
                    }
                    s_openchannelInfo[socket_id][i].cid = cid;
                    s_openchannelInfo[socket_id][i].state = REUSE;
                    s_openchannelInfo[socket_id][i].count++;
                    return s_openchannelInfo[socket_id][i].cid;
                }
            }
        }
    }

    requestSetupDataCall(socket_id, data, datalen, NULL);
    RLOGD("open channel cid = %d", s_openchannelCid);
    if (s_openchannelCid > 0) {
        i = s_openchannelCid -1;
        s_openchannelInfo[socket_id][i].cid = s_openchannelCid;
        s_openchannelInfo[socket_id][i].state = OPEN;
    }
    return s_openchannelCid;
}

void requestDeactiveDataConnection(RIL_SOCKET_ID socket_id, void *data,
                                   size_t datalen) {
    const char *p_cid = ((const char **)data)[0];
    int cid = atoi(p_cid);

    RLOGD("close channel cid = %d", cid);
    property_set(BIP_OPENCHANNEL, "0");
    if (cid > 0) {
        RLOGD("close channel state for socket_id = %d is %d",
                socket_id, s_openchannelInfo[socket_id][cid - 1].state);
        s_openchannelInfo[socket_id][cid - 1].cid = -1;
        s_openchannelInfo[socket_id][cid - 1].state = CLOSE;
        deactivateDataConnection(socket_id, data, datalen, NULL);
    }
}
