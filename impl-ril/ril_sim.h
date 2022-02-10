/**
 * ril_sim.h --- SIM-related requests
 *               process functions/struct/variables declaration and definition
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_SIM_H_
#define RIL_SIM_H_

#define SIM_ENABLED_PROP         "persist.radio.sim_enabled"
#define PHONE_COUNT_PROP         "persist.vendor.radio.phone_count"
#define SIM_SLOT_MAPPING_PROP    "persist.vendor.radio.sim.slot.mapping"

typedef enum {
    UNLOCK_PIN   = 0,
    UNLOCK_PIN2  = 1,
    UNLOCK_PUK   = 2,
    UNLOCK_PUK2  = 3
} SimUnlockType;

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2,  /* SIM_READY means radio state is RADIO_STATE_SIM_READY */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    RUIM_ABSENT = 6,
    RUIM_NOT_READY = 7,
    RUIM_READY = 8,
    RUIM_PIN = 9,
    RUIM_PUK = 10,
    RUIM_NETWORK_PERSONALIZATION = 11,
    EXT_SIM_STATUS_BASE = 11,
    SIM_NETWORK_SUBSET_PERSONALIZATION = EXT_SIM_STATUS_BASE + 1,
    SIM_SERVICE_PROVIDER_PERSONALIZATION = EXT_SIM_STATUS_BASE + 2,
    SIM_CORPORATE_PERSONALIZATION = EXT_SIM_STATUS_BASE + 3,
    SIM_SIM_PERSONALIZATION = EXT_SIM_STATUS_BASE + 4,
    SIM_NETWORK_PUK = EXT_SIM_STATUS_BASE + 5,
    SIM_NETWORK_SUBSET_PUK = EXT_SIM_STATUS_BASE + 6,
    SIM_SERVICE_PROVIDER_PUK = EXT_SIM_STATUS_BASE + 7,
    SIM_CORPORATE_PUK = EXT_SIM_STATUS_BASE + 8,
    SIM_SIM_PUK = EXT_SIM_STATUS_BASE + 9,
    SIM_SIMLOCK_FOREVER = EXT_SIM_STATUS_BASE + 10,
    SIM_PERM_BLOCK = EXT_SIM_STATUS_BASE + 11
} SimStatus;

typedef enum {
    ABSENT = 0,
    PRESENT = 1,
    SIM_UNKNOWN = 2
} SimPresentState;

// UNISOC Add for AT+SPCRSM
typedef enum {
    SPCRSM_APPTYPE_UNKNOWN   = 0,
    SPCRSM_APPTYPE_USIM  = 1,
    SPCRSM_APPTYPE_ISIM   = 2,
    SPCRSM_APPTYPE_CSIM  = 3
} SpcrsmType;

extern int s_imsInitISIM[SIM_COUNT];
extern RIL_AppType s_appType[SIM_COUNT];
extern pthread_mutex_t s_CglaCrsmMutex[SIM_COUNT];
extern pthread_mutex_t s_SPCCHOMutex[SIM_COUNT];

void onSimAbsent(void *param);
void onModemReset_Sim();
int initISIM(RIL_SOCKET_ID socket_id);
int processSimRequests(int request, void *data, size_t datalen, RIL_Token t,
                       RIL_SOCKET_ID socket_id);
int processSimUnsolicited(RIL_SOCKET_ID socket_id, const char *s);
SimStatus getSIMStatus(int request, RIL_SOCKET_ID socket_id);
RIL_AppType getSimType(RIL_SOCKET_ID socket_id);
void dispatchCLCK(RIL_Token t, void *data, void *resp);
void setSimPresent(RIL_SOCKET_ID socket_id, int hasSim);
int isSimPresent(RIL_SOCKET_ID socket_id);
void initSIMPresentState();
#endif  // RIL_SIM_H_
