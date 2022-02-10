/*
 * Copyright (c) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "RILC"


#include <android/hardware/radio/1.4/IRadioResponse.h>
#include <android/hardware/radio/1.4/IRadioIndication.h>
#include <android/hardware/radio/1.4/IRadio.h>

#include <android/hardware/radio/deprecated/1.0/IOemHook.h>

#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <telephony/ril.h>
#include <telephony/ril_mnc.h>
#include <ril_service.h>
#include <hidl/HidlTransportSupport.h>
#include <utils/SystemClock.h>
#include <inttypes.h>

#include <vendor/sprd/hardware/radio/1.0/IExtRadio.h>

#define INVALID_HEX_CHAR 16

using namespace android::hardware::radio;
using namespace android::hardware::radio::V1_0;
using namespace android::hardware::radio::deprecated::V1_0;
using ::android::hardware::configureRpcThreadpool;
using ::android::hardware::joinRpcThreadpool;
using ::android::hardware::Return;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_array;
using ::android::hardware::Void;
using ::android::hardware::hidl_bitfield;
using android::CommandInfo;
using android::RequestInfo;
using android::requestToString;
using android::sp;

using namespace vendor::sprd::hardware::radio::V1_0;

#define BOOL_TO_INT(x) (x ? 1 : 0)
#define ATOI_NULL_HANDLED(x) (x ? atoi(x) : -1)
#define ATOI_NULL_HANDLED_DEF(x, defaultVal) (x ? atoi(x) : defaultVal)

typedef enum {
    OEM_SERVICE,
    RADIO_SERVICE,
    RADIOINTERACTOR_SERVICE,
    ATC_SERVICE
} serviceType;

#ifdef OEM_HOOK_DISABLED
constexpr bool kOemHookEnabled = false;
#else
constexpr bool kOemHookEnabled = true;
#endif

RIL_RadioFunctions *s_vendorFunctions = NULL;

#if defined (ANDROID_MULTI_SIM)
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c), (d))
#define CALL_ONREQUEST(a, b, c, d, e) s_vendorFunctions->onRequest((a), (b), (c), (d), (e))
#define CALL_ONSTATEREQUEST(a) s_vendorFunctions->onStateRequest(a)
#else
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c))
#define CALL_ONREQUEST(a, b, c, d, e) s_vendorFunctions->onRequest((a), (b), (c), (d))
#define CALL_ONSTATEREQUEST(a) s_vendorFunctions->onStateRequest()
#endif

static CommandInfo *s_commands;

struct RadioImpl;
struct OemHookImpl;
struct ExtRadioImpl;

#if (SIM_COUNT >= 2)
sp<RadioImpl> radioService[SIM_COUNT];
sp<ExtRadioImpl> extRadioService[SIM_COUNT];
sp<OemHookImpl> oemHookService[SIM_COUNT];
int64_t nitzTimeReceived[SIM_COUNT];
// counter used for synchronization. It is incremented every time response callbacks are updated.
volatile int32_t mCounterRadio[SIM_COUNT];
volatile int32_t mCounterOemHook[SIM_COUNT];
volatile int32_t mCounterExtRadio[SIM_COUNT];
volatile int32_t mCounterAtcRadio[SIM_COUNT];
#else
sp<RadioImpl> radioService[1];
sp<ExtRadioImpl> extRadioService[1];
sp<OemHookImpl> oemHookService[1];
int64_t nitzTimeReceived[1];
// counter used for synchronization. It is incremented every time response callbacks are updated.
volatile int32_t mCounterRadio[1];
volatile int32_t mCounterOemHook[1];
volatile int32_t mCounterExtRadio[1];
volatile int32_t mCounterAtcRadio[1];
#endif

static pthread_rwlock_t radioServiceRwlock = PTHREAD_RWLOCK_INITIALIZER;

#if (SIM_COUNT >= 2)
static pthread_rwlock_t radioServiceRwlock2 = PTHREAD_RWLOCK_INITIALIZER;
#if (SIM_COUNT >= 3)
static pthread_rwlock_t radioServiceRwlock3 = PTHREAD_RWLOCK_INITIALIZER;
#if (SIM_COUNT >= 4)
static pthread_rwlock_t radioServiceRwlock4 = PTHREAD_RWLOCK_INITIALIZER;
#endif
#endif
#endif

void convertRilHardwareConfigListToHal(void *response, size_t responseLen,
        hidl_vec<HardwareConfig>& records);

void convertRilRadioCapabilityToHal(void *response, size_t responseLen, RadioCapability& rc);

void convertRilLceDataInfoToHal(void *response, size_t responseLen, LceDataInfo& lce);

void convertRilSignalStrengthToHal(void *response, size_t responseLen,
        SignalStrength& signalStrength);
void convertRilSignalStrengthToHal_1_2(void *response, size_t responseLen,
        V1_2::SignalStrength& signalStrength);
void convertRilSignalStrengthToHal_1_4(void *response, size_t responseLen,
        V1_4::SignalStrength& signalStrength);

void convertRilDataCallToHal(RIL_SetupDataCallResult_v1_4 *dcResponse,
        SetupDataCallResult& dcResult);
void convertRilDataCallToHal_1_4(RIL_SetupDataCallResult_v1_4 *dcResponse,
        V1_4::SetupDataCallResult& dcResult);

void convertRilDataCallListToHal(void *response, size_t responseLen,
        hidl_vec<SetupDataCallResult>& dcResultList);
void convertRilDataCallListToHal_1_4(void *response, size_t responseLen,
        hidl_vec<V1_4::SetupDataCallResult>& dcResultList);

void convertRilCellInfoListToHal(void *response, size_t responseLen, hidl_vec<CellInfo>& records);
void convertRilCellInfoListToHal_1_2(void *response, size_t responseLen, hidl_vec<V1_2::CellInfo>& records);
void convertRilCellInfoListToHal_1_4(void *response, size_t responseLen, hidl_vec<V1_4::CellInfo>& records);

extern "C" int updatePlmn(int slotId, int lac, const char *mncmcc, char *resp, size_t respLen);
extern "C" int updateNetworkList(int slotId, char **networkList, size_t datalen,
                      char *resp, size_t respLen);

struct RadioImpl : public V1_4::IRadio {
    int32_t mSlotId;
    sp<IRadioResponse> mRadioResponse;
    sp<IRadioIndication> mRadioIndication;
    sp<V1_1::IRadioResponse> mRadioResponseV1_1;
    sp<V1_1::IRadioIndication> mRadioIndicationV1_1;
    sp<V1_2::IRadioResponse> mRadioResponseV1_2;
    sp<V1_2::IRadioIndication> mRadioIndicationV1_2;
    sp<V1_3::IRadioResponse> mRadioResponseV1_3;
    sp<V1_3::IRadioIndication> mRadioIndicationV1_3;
    sp<V1_4::IRadioResponse> mRadioResponseV1_4;
    sp<V1_4::IRadioIndication> mRadioIndicationV1_4;

    Return<void> setResponseFunctions(
            const ::android::sp<IRadioResponse>& radioResponse,
            const ::android::sp<IRadioIndication>& radioIndication);

    Return<void> getIccCardStatus(int32_t serial);

    Return<void> supplyIccPinForApp(int32_t serial, const hidl_string& pin,
            const hidl_string& aid);

    Return<void> supplyIccPukForApp(int32_t serial, const hidl_string& puk,
            const hidl_string& pin, const hidl_string& aid);

    Return<void> supplyIccPin2ForApp(int32_t serial,
            const hidl_string& pin2,
            const hidl_string& aid);

    Return<void> supplyIccPuk2ForApp(int32_t serial, const hidl_string& puk2,
            const hidl_string& pin2, const hidl_string& aid);

    Return<void> changeIccPinForApp(int32_t serial, const hidl_string& oldPin,
            const hidl_string& newPin, const hidl_string& aid);

    Return<void> changeIccPin2ForApp(int32_t serial, const hidl_string& oldPin2,
            const hidl_string& newPin2, const hidl_string& aid);

    Return<void> supplyNetworkDepersonalization(int32_t serial, const hidl_string& netPin);

    Return<void> getCurrentCalls(int32_t serial);

    Return<void> dial(int32_t serial, const Dial& dialInfo);

    Return<void> getImsiForApp(int32_t serial,
            const ::android::hardware::hidl_string& aid);

    Return<void> hangup(int32_t serial, int32_t gsmIndex);

    Return<void> hangupWaitingOrBackground(int32_t serial);

    Return<void> hangupForegroundResumeBackground(int32_t serial);

    Return<void> switchWaitingOrHoldingAndActive(int32_t serial);

    Return<void> conference(int32_t serial);

    Return<void> rejectCall(int32_t serial);

    Return<void> getLastCallFailCause(int32_t serial);

    Return<void> getSignalStrength(int32_t serial);

    Return<void> getSignalStrength_1_4(int32_t serial);

    Return<void> getVoiceRegistrationState(int32_t serial);

    Return<void> getDataRegistrationState(int32_t serial);

    Return<void> getOperator(int32_t serial);

    Return<void> setRadioPower(int32_t serial, bool on);

    Return<void> sendDtmf(int32_t serial,
            const ::android::hardware::hidl_string& s);

    Return<void> sendSms(int32_t serial, const GsmSmsMessage& message);

    Return<void> sendSMSExpectMore(int32_t serial, const GsmSmsMessage& message);

    Return<void> setupDataCall(int32_t serial,
            RadioTechnology radioTechnology,
            const DataProfileInfo& profileInfo,
            bool modemCognitive,
            bool roamingAllowed,
            bool isRoaming);

    Return<void> setupDataCall_1_2(int32_t serial, V1_2::AccessNetwork accessNetwork,
            const V1_0::DataProfileInfo& dataProfileInfo, bool modemCognitive,
            bool roamingAllowed, bool isRoaming, V1_2::DataRequestReason reason,
            const hidl_vec<hidl_string>& addresses, const hidl_vec<hidl_string>& dnses);

    Return<void> setupDataCall_1_4(int32_t serial, V1_4::AccessNetwork accessNetwork,
            const V1_4::DataProfileInfo& dataProfileInfo, bool roamingAllowed, V1_2::DataRequestReason reason,
            const hidl_vec<hidl_string>& addresses, const hidl_vec<hidl_string>& dnses);

    Return<void> iccIOForApp(int32_t serial,
            const IccIo& iccIo);

    Return<void> sendUssd(int32_t serial,
            const ::android::hardware::hidl_string& ussd);

    Return<void> cancelPendingUssd(int32_t serial);

    Return<void> getClir(int32_t serial);

    Return<void> setClir(int32_t serial, int32_t status);

    Return<void> getCallForwardStatus(int32_t serial,
            const CallForwardInfo& callInfo);

    Return<void> setCallForward(int32_t serial,
            const CallForwardInfo& callInfo);

    Return<void> getCallWaiting(int32_t serial, int32_t serviceClass);

    Return<void> setCallWaiting(int32_t serial, bool enable, int32_t serviceClass);

    Return<void> acknowledgeLastIncomingGsmSms(int32_t serial,
            bool success, SmsAcknowledgeFailCause cause);

    Return<void> acceptCall(int32_t serial);

    Return<void> deactivateDataCall(int32_t serial,
            int32_t cid, bool reasonRadioShutDown);

    Return<void> deactivateDataCall_1_2(int32_t serial, int32_t cid,
            V1_2::DataRequestReason reason);

    Return<void> getFacilityLockForApp(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            const ::android::hardware::hidl_string& password,
            int32_t serviceClass,
            const ::android::hardware::hidl_string& appId);

    Return<void> setFacilityLockForApp(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            bool lockState,
            const ::android::hardware::hidl_string& password,
            int32_t serviceClass,
            const ::android::hardware::hidl_string& appId);

    Return<void> setBarringPassword(int32_t serial,
            const ::android::hardware::hidl_string& facility,
            const ::android::hardware::hidl_string& oldPassword,
            const ::android::hardware::hidl_string& newPassword);

    Return<void> getNetworkSelectionMode(int32_t serial);

    Return<void> setNetworkSelectionModeAutomatic(int32_t serial);

    Return<void> setNetworkSelectionModeManual(int32_t serial,
            const ::android::hardware::hidl_string& operatorNumeric);

    Return<void> getAvailableNetworks(int32_t serial);

    Return<void> startNetworkScan(int32_t serial, const V1_1::NetworkScanRequest& request);

    Return<void> startNetworkScan_1_2(int32_t serial, const V1_2::NetworkScanRequest& request);

    Return<void> startNetworkScan_1_4(int32_t serial, const V1_2::NetworkScanRequest& request);

    Return<void> stopNetworkScan(int32_t serial);

    Return<void> startDtmf(int32_t serial,
            const ::android::hardware::hidl_string& s);

    Return<void> stopDtmf(int32_t serial);

    Return<void> getBasebandVersion(int32_t serial);

    Return<void> separateConnection(int32_t serial, int32_t gsmIndex);

    Return<void> setMute(int32_t serial, bool enable);

    Return<void> getMute(int32_t serial);

    Return<void> getClip(int32_t serial);

    Return<void> getDataCallList(int32_t serial);

    Return<void> setSuppServiceNotifications(int32_t serial, bool enable);

    Return<void> writeSmsToSim(int32_t serial,
            const SmsWriteArgs& smsWriteArgs);

    Return<void> deleteSmsOnSim(int32_t serial, int32_t index);

    Return<void> setBandMode(int32_t serial, RadioBandMode mode);

    Return<void> getAvailableBandModes(int32_t serial);

    Return<void> sendEnvelope(int32_t serial,
            const ::android::hardware::hidl_string& command);

    Return<void> sendTerminalResponseToSim(int32_t serial,
            const ::android::hardware::hidl_string& commandResponse);

    Return<void> handleStkCallSetupRequestFromSim(int32_t serial, bool accept);

    Return<void> explicitCallTransfer(int32_t serial);

    Return<void> setPreferredNetworkType(int32_t serial, PreferredNetworkType nwType);

    Return<void> getPreferredNetworkType(int32_t serial);

    Return<void> getNeighboringCids(int32_t serial);

    Return<void> setLocationUpdates(int32_t serial, bool enable);

    Return<void> setCdmaSubscriptionSource(int32_t serial,
            CdmaSubscriptionSource cdmaSub);

    Return<void> setCdmaRoamingPreference(int32_t serial, CdmaRoamingType type);

    Return<void> getCdmaRoamingPreference(int32_t serial);

    Return<void> setTTYMode(int32_t serial, TtyMode mode);

    Return<void> getTTYMode(int32_t serial);

    Return<void> setPreferredVoicePrivacy(int32_t serial, bool enable);

    Return<void> getPreferredVoicePrivacy(int32_t serial);

    Return<void> sendCDMAFeatureCode(int32_t serial,
            const ::android::hardware::hidl_string& featureCode);

    Return<void> sendBurstDtmf(int32_t serial,
            const ::android::hardware::hidl_string& dtmf,
            int32_t on,
            int32_t off);

    Return<void> sendCdmaSms(int32_t serial, const CdmaSmsMessage& sms);

    Return<void> acknowledgeLastIncomingCdmaSms(int32_t serial,
            const CdmaSmsAck& smsAck);

    Return<void> getGsmBroadcastConfig(int32_t serial);

    Return<void> setGsmBroadcastConfig(int32_t serial,
            const hidl_vec<GsmBroadcastSmsConfigInfo>& configInfo);

    Return<void> setGsmBroadcastActivation(int32_t serial, bool activate);

    Return<void> getCdmaBroadcastConfig(int32_t serial);

    Return<void> setCdmaBroadcastConfig(int32_t serial,
            const hidl_vec<CdmaBroadcastSmsConfigInfo>& configInfo);

    Return<void> setCdmaBroadcastActivation(int32_t serial, bool activate);

    Return<void> getCDMASubscription(int32_t serial);

    Return<void> writeSmsToRuim(int32_t serial, const CdmaSmsWriteArgs& cdmaSms);

    Return<void> deleteSmsOnRuim(int32_t serial, int32_t index);

    Return<void> getDeviceIdentity(int32_t serial);

    Return<void> exitEmergencyCallbackMode(int32_t serial);

    Return<void> getSmscAddress(int32_t serial);

    Return<void> setSmscAddress(int32_t serial,
            const ::android::hardware::hidl_string& smsc);

    Return<void> reportSmsMemoryStatus(int32_t serial, bool available);

    Return<void> reportStkServiceIsRunning(int32_t serial);

    Return<void> getCdmaSubscriptionSource(int32_t serial);

    Return<void> requestIsimAuthentication(int32_t serial,
            const ::android::hardware::hidl_string& challenge);

    Return<void> acknowledgeIncomingGsmSmsWithPdu(int32_t serial,
            bool success,
            const ::android::hardware::hidl_string& ackPdu);

    Return<void> sendEnvelopeWithStatus(int32_t serial,
            const ::android::hardware::hidl_string& contents);

    Return<void> getVoiceRadioTechnology(int32_t serial);

    Return<void> getCellInfoList(int32_t serial);

    Return<void> setCellInfoListRate(int32_t serial, int32_t rate);

    Return<void> setInitialAttachApn(int32_t serial, const DataProfileInfo& dataProfileInfo,
            bool modemCognitive, bool isRoaming);

    Return<void> setInitialAttachApn_1_4(int32_t serial, const V1_4::DataProfileInfo& dataProfileInfo);

    Return<void> getImsRegistrationState(int32_t serial);

    Return<void> sendImsSms(int32_t serial, const ImsSmsMessage& message);

    Return<void> iccTransmitApduBasicChannel(int32_t serial, const SimApdu& message);

    Return<void> iccOpenLogicalChannel(int32_t serial,
            const ::android::hardware::hidl_string& aid, int32_t p2);

    Return<void> iccCloseLogicalChannel(int32_t serial, int32_t channelId);

    Return<void> iccTransmitApduLogicalChannel(int32_t serial, const SimApdu& message);

    Return<void> nvReadItem(int32_t serial, NvItem itemId);

    Return<void> nvWriteItem(int32_t serial, const NvWriteItem& item);

    Return<void> nvWriteCdmaPrl(int32_t serial,
            const ::android::hardware::hidl_vec<uint8_t>& prl);

    Return<void> nvResetConfig(int32_t serial, ResetNvType resetType);

    Return<void> setUiccSubscription(int32_t serial, const SelectUiccSub& uiccSub);

    Return<void> setDataAllowed(int32_t serial, bool allow);

    Return<void> getHardwareConfig(int32_t serial);

    Return<void> requestIccSimAuthentication(int32_t serial,
            int32_t authContext,
            const ::android::hardware::hidl_string& authData,
            const ::android::hardware::hidl_string& aid);

    Return<void> setDataProfile(int32_t serial,
            const ::android::hardware::hidl_vec<DataProfileInfo>& profiles, bool isRoaming);

    Return<void> setDataProfile_1_4(int32_t serial, const hidl_vec<V1_4::DataProfileInfo>& profiles);

    Return<void> requestShutdown(int32_t serial);

    Return<void> getRadioCapability(int32_t serial);

    Return<void> setRadioCapability(int32_t serial, const RadioCapability& rc);

    Return<void> startLceService(int32_t serial, int32_t reportInterval, bool pullMode);

    Return<void> stopLceService(int32_t serial);

    Return<void> pullLceData(int32_t serial);

    Return<void> getModemActivityInfo(int32_t serial);

    Return<void> setAllowedCarriers(int32_t serial,
            bool allAllowed,
            const CarrierRestrictions& carriers);

    Return<void> setAllowedCarriers_1_4(int32_t serial,
            const V1_4::CarrierRestrictionsWithPriority& carriers,
            V1_4::SimLockMultiSimPolicy multiSimPolicy);

    Return<void> getAllowedCarriers(int32_t serial);

    Return<void> getAllowedCarriers_1_4(int32_t serial);

    Return<void> sendDeviceState(int32_t serial, DeviceStateType deviceStateType, bool state);

    Return<void> setIndicationFilter(int32_t serial, int32_t indicationFilter);

    Return<void> setIndicationFilter_1_2(int32_t serial,
            ::android::hardware::hidl_bitfield<V1_2::IndicationFilter> indicationFilter);

    Return<void> startKeepalive(int32_t serial, const V1_1::KeepaliveRequest& keepalive);

    Return<void> stopKeepalive(int32_t serial, int32_t sessionHandle);

    Return<void> setSimCardPower(int32_t serial, bool powerUp);
    Return<void> setSimCardPower_1_1(int32_t serial, V1_1::CardPowerState powerUp);

    Return<void> responseAcknowledgement();

    Return<void> setCarrierInfoForImsiEncryption(int32_t serial,
            const V1_1::ImsiEncryptionInfo& message);

    Return<void> setSignalStrengthReportingCriteria(int32_t serial,
            int32_t hysteresisMs, int32_t hysteresisDb,
            const hidl_vec<int32_t>& thresholdsDbm,
            V1_2::AccessNetwork accessNetwork);

    Return<void> setLinkCapacityReportingCriteria(int32_t serial,
            int32_t hysteresisMs, int32_t hysteresisDlKbps, int32_t hysteresisUlKbps,
            const hidl_vec<int32_t>& thresholdsDownlinkKbps,
            const hidl_vec<int32_t>& thresholdsUplinkKbps,
            V1_2::AccessNetwork accessNetwork);

    Return<void> setSystemSelectionChannels(int32_t serial, bool specifyChannels,
            const hidl_vec<V1_1::RadioAccessSpecifier>& specifiers);

    Return<void> enableModem(int32_t serial, bool on);

    Return<void> getModemStackStatus(int32_t serial);

    Return<void> emergencyDial(int32_t serial, const V1_0::Dial& dialInfo,
            hidl_bitfield<V1_4::EmergencyServiceCategory> categories,
            const hidl_vec<hidl_string>& urns, V1_4::EmergencyCallRouting routing,
            bool hasKnownUserIntentEmergency, bool isTesting);

    Return<void> getPreferredNetworkTypeBitmap(int32_t serial);

    Return<void> setPreferredNetworkTypeBitmap(int32_t serial,
            hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap);

    void checkReturnStatus(Return<void>& ret);
};

struct ExtRadioImpl : public IExtRadio {
    int32_t mSlotId;

    sp<IExtRadioResponse> mExtRadioResponse;
    sp<IExtRadioIndication> mExtRadioIndication;

    sp<IAtcRadioResponse> mAtcRadioResponse;
    sp<IAtcRadioIndication> mAtcRadioIndication;

    Return<void> setExtResponseFunctions(
            const sp<IExtRadioResponse>& radioResponse,
            const sp<IExtRadioIndication>& radioIndication);

    Return<void> setAtcResponseFunctions(
            const sp<IAtcRadioResponse>& radioResponse,
            const sp<IAtcRadioIndication>& radioIndication);

    Return<void> sendCmdSync(int32_t phoneId,
            const hidl_string& cmd, sendCmdSync_cb _hidl_cb);

    Return<void> responseAcknowledgement();

    void checkReturnStatus(Return<void>& ret, serviceType srvType);

    Return<void> videoPhoneDial(int32_t serial, const VideoPhoneDial& dialInfo);

    Return<void> videoPhoneCodec(int32_t serial, const VideoPhoneCodec& codec);

    Return<void> videoPhoneFallback(int32_t serial);

    Return<void> videoPhoneString(int32_t serial, const hidl_string& str);

    Return<void> videoPhoneLocalMedia(int32_t serial, int32_t dataType, int32_t sw, bool beReplaceImg);

    Return<void> videoPhoneControlIFrame(int32_t serial, bool isIFrame, bool needIFrame);

    Return<void> setTrafficClass(int32_t serial, int32_t type);

    Return<void> enableLTE(int32_t serial, bool enabled);

    Return<void> attachData(int32_t serial, bool enabled);

    Return<void> forceDeatch(int32_t serial);

    Return<void> getHDVoiceState(int32_t serial);

    Return<void> simmgrSimPower(int32_t serial, bool enabled);

    Return<void> enableRauNotify(int32_t serial);

    Return<void> simGetAtr(int32_t serial);

    Return<void> explicitCallTransferExt(int32_t serial);

    Return<void> getSimCapacity(int32_t serial);

    Return<void> storeSmsToSim(int32_t serial, bool enabled);

    Return<void> querySmsStorageMode(int32_t serial);

    Return<void> getSimlockRemaintimes(int32_t serial, int32_t type);

    Return<void> setFacilityLockForUser(int32_t serial,
            const hidl_string& facility, bool lockState);

    Return<void> getSimlockStatus(int32_t serial, int32_t type);

    Return<void> getSimlockDummys(int32_t serial);

    Return<void> getSimlockWhitelist(int32_t serial, int32_t type);

    Return<void> updateEcclist(int32_t serial, const hidl_string& ecclist);

    Return<void> setSinglePDN(int32_t serial, bool isSinglePDN);

    Return<void> queryColp(int32_t serial);

    Return<void> queryColr(int32_t serial);

    Return<void> updateOperatorName(int32_t serial, const hidl_string& plmn);

    Return<void> simmgrGetSimStatus(int32_t serial);

    Return<void> setXcapIPAddress(int32_t serial, const hidl_string& ifName,
            const hidl_string& ipv4Addr, const hidl_string& ipv6Addr);

    Return<void> sendCmdAsync(int32_t serial, const hidl_string& cmd);

    Return<void> reAttach(int32_t serial);

    Return<void> setPreferredNetworkTypeExt(int32_t serial, ExtPreferredNetworkType nwType);

    Return<void> requestShutdownExt(int32_t serial);

    Return<void> updateCLIP(int32_t serial, int32_t enable);

    Return<void> setTPMRState(int32_t serial, int32_t state);

    Return<void> getTPMRState(int32_t serial);

    Return<void> setVideoResolution(int32_t serial, int32_t resolution);

    Return<void> enableLocalHold(int32_t serial, bool enabled);

    Return<void> enableWiFiParamReport(int32_t serial, bool enabled);

    Return<void> callMediaChangeRequestTimeOut(int32_t serial, int32_t callId);

    Return<void> setLocalTone(int32_t serial, int32_t state);

    Return<void> queryPlmn(int32_t serial, int32_t type);

    Return<void> updatePlmnPriority(int32_t serial, int32_t type, int32_t action,
            int32_t plmn, int32_t act1, int32_t act2, int32_t act3);

    Return<void> setSimPowerReal(int32_t serial, bool enable);

    Return<void> getRadioPreference(int32_t serial, const hidl_string& key);

    Return<void> setRadioPreference(int32_t serial,  const hidl_string& key,
            const hidl_string& value);

    Return<void> getIMSCurrentCalls(int32_t serial);

    Return<void> setIMSVoiceCallAvailability(int32_t serial, int32_t state);

    Return<void> getIMSVoiceCallAvailability(int32_t serial);

    Return<void> initISIM(int32_t serial);

    Return<void> requestVolteCallMediaChange(int32_t serial, int32_t callID,
            int32_t mediaRequest);

    Return<void> responseVolteCallMediaChange(int32_t serial, int32_t callID,
            bool isAccept, int32_t videoCallMediaDirection);

    Return<void> setIMSSmscAddress(int32_t serial, const hidl_string& smsc);

    Return<void> volteCallFallBackToVoice(int32_t serial, int32_t callID);

    Return<void> queryCallForwardStatus(int32_t serial, const CallForwardInfoUri& info);

    Return<void> setCallForwardUri(int32_t serial, const CallForwardInfoUri& info);

    Return<void> IMSInitialGroupCall(int32_t serial, const hidl_string& numbers);

    Return<void> IMSAddGroupCall(int32_t serial, const hidl_string& numbers);

    Return<void> enableIMS(int32_t serial, bool enabled);

    Return<void> getIMSBearerState(int32_t serial);

    Return<void> setExtInitialAttachApn(int32_t serial,
            const ExtDataProfileInfo& dataProfileInfo);

    Return<void> IMSHandover(int32_t serial, int32_t type);

    Return<void> notifyIMSHandoverStatusUpdate(int32_t serial, int32_t status);

    Return<void> notifyIMSNetworkInfoChanged(int32_t serial,
            const ImsNetworkInfo& networkInfo);

    Return<void> notifyIMSCallEnd(int32_t serial, int32_t type);

    Return<void> notifyVoWifiEnable(int32_t serial, bool enable);

    Return<void> notifyVoWifiCallStateChanged(int32_t serial, bool inCall);

    Return<void> notifyDataRouterUpdate(int32_t serial);

    Return<void> IMSHoldSingleCall(int32_t serial, int32_t callId, bool enable);

    Return<void> IMSMuteSingleCall(int32_t serial, int32_t callId, bool enable);

    Return<void> IMSSilenceSingleCall(int32_t serial, int32_t callId, bool enable);

    Return<void> IMSEnableLocalConference(int32_t serial, bool enable);

    Return<void> notifyHandoverCallInfo(int32_t serial, const hidl_string& callInfo);

    Return<void> getSrvccCapbility(int32_t serial);

    Return<void> setIMSPcscfAddress(int32_t serial, const hidl_string& addr);

    Return<void> getIMSPcscfAddress(int32_t serial);

    Return<void> getFacilityLockForAppExt(int32_t serial,
            const hidl_string& facility, const hidl_string& password,
            int32_t serviceClass, const hidl_string& appId);

    Return<void> getImsRegAddress(int32_t serial);

    Return<void> vsimSendCmd(int32_t serial, int32_t phoneId, const hidl_string& cmd);

    Return<void> getPreferredNetworkTypeExt(int32_t serial);

    Return<void> setRadioPowerFallback(int32_t serial, bool enabled);

    Return<void> getCnap(int32_t serial);

    Return<void> setLocationInfo(int32_t serial, const hidl_string& longitude,
            const hidl_string& latitude);

    Return<void> getSpecialRatcap(int32_t serial, int32_t value);

    Return<void> getVideoResolution(int32_t serial);

    Return<void> getImsPaniInfo(int32_t serial);

    Return<void> setEmergencyOnly(int32_t serial, int32_t emergencyOnly);

    Return<void> getSubsidyLockdyStatus(int32_t serial);

    Return<void> setImsUserAgent(int32_t serial, const hidl_string& sipUserAgent);

    Return<void> resetModem(int32_t serial);

    Return<void> getVoLTEAllowedPLMN(int32_t serial);

    Return<void> setSmsBearer(int32_t serial, int32_t type);

    Return<void> getSmsBearer(int32_t serial);

    Return<void> queryRootNode(int32_t serial);

    Return<void> setPsDataOff(int32_t serial, bool onOff, int32_t exemptionInfo);

    Return<void> requestLteSpeedAndSignalStrength(int32_t serial);

    Return<void> enableNrSwitch(int32_t serial, int32_t mode, int32_t enable);

    Return<void> setUsbShareStateSwitch(int32_t serial, bool switchState);

    Return<void> setStandAlone(int32_t serial, int32_t value);

    Return<void> getStandAlone(int32_t serial);

    Return<void> getSmart5GSwitchState(int32_t serial);
};

struct OemHookImpl : public IOemHook {
    int32_t mSlotId;
    sp<IOemHookResponse> mOemHookResponse;
    sp<IOemHookIndication> mOemHookIndication;

    Return<void> setResponseFunctions(
            const ::android::sp<IOemHookResponse>& oemHookResponse,
            const ::android::sp<IOemHookIndication>& oemHookIndication);

    Return<void> sendRequestRaw(int32_t serial,
            const ::android::hardware::hidl_vec<uint8_t>& data);

    Return<void> sendRequestStrings(int32_t serial,
            const hidl_vec<hidl_string>& data);
};

void memsetAndFreeStrings(int numPointers, ...) {
    va_list ap;
    va_start(ap, numPointers);
    for (int i = 0; i < numPointers; i++) {
        char *ptr = va_arg(ap, char *);
        if (ptr) {
#ifdef MEMSET_FREED
#define MAX_STRING_LENGTH 4096
            memset(ptr, 0, strnlen(ptr, MAX_STRING_LENGTH));
#endif
            free(ptr);
        }
    }
    va_end(ap);
}

bool isChangeSlotId(int serviceId, int slotId) {
    RLOGD("isChangeSlotId: reset begin serviceId = %d, slotId = %d", serviceId, slotId);
    bool ret = false;
    if (radioService[serviceId]->mSlotId != slotId) {
        ret = true;
    }
    return ret;
}

void sendErrorResponse(RequestInfo *pRI, RIL_Errno err) {
    if (pRI != NULL && pRI->pCI != NULL) {
        pRI->pCI->responseFunction((int) pRI->socket_id,
                (int) RadioResponseType::SOLICITED, pRI->token, err, NULL, 0);
    }
}

/**
 * Copies over src to dest. If memory allocation fails, responseFunction() is called for the
 * request with error RIL_E_NO_MEMORY. The size() method is used to determine the size of the
 * destination buffer into which the HIDL string is copied. If there is a discrepancy between
 * the string length reported by the size() method, and the length of the string returned by
 * the c_str() method, the function will return false indicating a failure.
 *
 * Returns true on success, and false on failure.
 */
bool copyHidlStringToRil(char **dest, const hidl_string &src, RequestInfo *pRI, bool allowEmpty) {
    size_t len = src.size();
    if (len == 0 && !allowEmpty) {
        *dest = NULL;
        return true;
    }
    *dest = (char *) calloc(len + 1, sizeof(char));
    if (*dest == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    if (strlcpy(*dest, src.c_str(), len + 1) >= (len + 1)) {
        RLOGE("Copy of the HIDL string has been truncated, as "
              "the string length reported by size() does not "
              "match the length of string returned by c_str().");
        free(*dest);
        *dest = NULL;
        sendErrorResponse(pRI, RIL_E_INTERNAL_ERR);
        return false;
    }
    return true;
}

bool copyHidlStringToRil(char **dest, const hidl_string &src, RequestInfo *pRI) {
    return copyHidlStringToRil(dest, src, pRI, false);
}

hidl_string convertCharPtrToHidlString(const char *ptr) {
    hidl_string ret;
    if (ptr != NULL) {
        // TODO: replace this with strnlen
        ret.setToExternal(ptr, strlen(ptr));
    }
    return ret;
}

bool dispatchVoid(int serial, int slotId, int request) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }
    CALL_ONREQUEST(request, NULL, 0, pRI, pRI->socket_id);
    return true;
}

bool dispatchString(int serial, int slotId, int request, const char * str) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    char *pString = NULL;
    if (!copyHidlStringToRil(&pString, str, pRI)) {
        return false;
    }

    size_t dataLen = 0;
    if (pString != NULL) {
        dataLen = strlen(pString) + 1;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen __attribute__((unused))) {
        memsetAndFreeStrings(1, data);
    };
    REQUEST_INFO_PACK(pRI, cb, pString, dataLen);

    CALL_ONREQUEST(request, pString, dataLen, pRI, pRI->socket_id);

    return true;
}

bool dispatchStrings(int serial, int slotId, int request, bool allowEmpty,
        int countStrings, ...) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    char **pStrings;
    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    va_list ap;
    va_start(ap, countStrings);
    for (int i = 0; i < countStrings; i++) {
        const char* str = va_arg(ap, const char *);
        if (!copyHidlStringToRil(&pStrings[i], hidl_string(str), pRI,
                allowEmpty)) {
            va_end(ap);
            for (int j = 0; j < i; j++) {
                memsetAndFreeStrings(1, pStrings[j]);
            }
            free(pStrings);
            return false;
        }
    }
    va_end(ap);

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        int countStrings = dataLen / sizeof(char *);
        char **pStrings = (char **)data;

        if (pStrings != NULL) {
            for (int i = 0; i < countStrings; i++) {
                memsetAndFreeStrings(1, pStrings[i]);
            }
            memset(pStrings, 0, dataLen);
            free(pStrings);
        }
    };
    REQUEST_INFO_PACK(pRI, cb, pStrings, countStrings * sizeof(char *));

    CALL_ONREQUEST(request, pStrings, countStrings * sizeof(char *), pRI, pRI->socket_id);

    return true;
}

bool dispatchStrings(int serial, int slotId, int request, const hidl_vec<hidl_string>& data) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    int countStrings = data.size();
    char **pStrings;
    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    for (int i = 0; i < countStrings; i++) {
        if (!copyHidlStringToRil(&pStrings[i], data[i], pRI)) {
            for (int j = 0; j < i; j++) {
                memsetAndFreeStrings(1, pStrings[j]);
            }
            free(pStrings);
            return false;
        }
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        int countStrings = dataLen / sizeof(char *);
        char **pStrings = (char **)data;

        if (pStrings != NULL) {
            for (int i = 0; i < countStrings; i++) {
                memsetAndFreeStrings(1, pStrings[i]);
            }
            memset(pStrings, 0, dataLen);
            free(pStrings);
        }
    };
    REQUEST_INFO_PACK(pRI, cb, pStrings, countStrings * sizeof(char *));

    CALL_ONREQUEST(request, pStrings, countStrings * sizeof(char *), pRI, pRI->socket_id);

    return true;
}

bool dispatchInts(int serial, int slotId, int request, int countInts, ...) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    int *pInts = (int *)calloc(countInts, sizeof(int));

    if (pInts == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }
    va_list ap;
    va_start(ap, countInts);
    for (int i = 0; i < countInts; i++) {
        pInts[i] = va_arg(ap, int);
    }
    va_end(ap);

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, pInts, countInts * sizeof(int));

    CALL_ONREQUEST(request, pInts, countInts * sizeof(int), pRI, pRI->socket_id);

    return true;
}

bool dispatchCallForwardStatus(int serial, int slotId, int request,
                              const CallForwardInfo& callInfo) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    RIL_CallForwardInfo *cf = (RIL_CallForwardInfo *)calloc(1, sizeof(RIL_CallForwardInfo));
    if (cf == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    cf->status = (int) callInfo.status;
    cf->reason = callInfo.reason;
    cf->serviceClass = callInfo.serviceClass;
    cf->toa = callInfo.toa;
    cf->timeSeconds = callInfo.timeSeconds;

    if (!copyHidlStringToRil(&cf->number, callInfo.number, pRI)) {
        free(cf);
        return false;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CallForwardInfo *pCFI = (RIL_CallForwardInfo *)data;

        memsetAndFreeStrings(1, pCFI->number);
        memset(pCFI, 0, dataLen);
        free(pCFI);
    };
    REQUEST_INFO_PACK(pRI, cb, cf, sizeof(RIL_CallForwardInfo));

    CALL_ONREQUEST(request, cf, sizeof(RIL_CallForwardInfo), pRI, pRI->socket_id);

    return true;
}

bool dispatchRaw(int serial, int slotId, int request, const hidl_vec<uint8_t>& rawBytes) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    uint8_t *uData = NULL;
    size_t len = rawBytes.size();
    if (len != 0) {
        uData = (uint8_t *)calloc(len, sizeof(uint8_t));
        if (uData == NULL) {
            RLOGE("Memory allocation failed for request %s", requestToString(request));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return false;
        }
        memcpy(uData, rawBytes.data(), len);
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, uData, len);

    CALL_ONREQUEST(request, (void *) uData, rawBytes.size(), pRI, pRI->socket_id);

    return true;
}

bool dispatchIccApdu(int serial, int slotId, int request, const SimApdu& message) {
    RequestInfo *pRI = android::addRequestToList(serial, slotId, request);
    if (pRI == NULL) {
        return false;
    }

    RIL_SIM_APDU *apdu = (RIL_SIM_APDU *)calloc(1, sizeof(RIL_SIM_APDU));
    if (apdu == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    apdu->sessionid = message.sessionId;
    apdu->cla = message.cla;
    apdu->instruction = message.instruction;
    apdu->p1 = message.p1;
    apdu->p2 = message.p2;
    apdu->p3 = message.p3;

    if (!copyHidlStringToRil(&apdu->data, message.data, pRI)) {
        free(apdu);
        return false;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_SIM_APDU *pApdu = (RIL_SIM_APDU *)data;

        memsetAndFreeStrings(1, pApdu->data);;
        memset(pApdu, 0, dataLen);
        free(pApdu);
    };
    REQUEST_INFO_PACK(pRI, cb, apdu, sizeof(RIL_SIM_APDU));

    CALL_ONREQUEST(request, apdu, sizeof(RIL_SIM_APDU), pRI, pRI->socket_id);

    return true;
}

int32_t getRadioServiceCounter(int slotId, serviceType srvType) {
    int32_t counter = 0;
    switch (srvType) {
        case RADIO_SERVICE:
            counter = mCounterRadio[slotId];
            break;
        case RADIOINTERACTOR_SERVICE:
            counter = mCounterExtRadio[slotId];
            break;
        case ATC_SERVICE:
            counter = mCounterAtcRadio[slotId];
            break;
        default:
            counter = mCounterOemHook[slotId];
            break;
    }
    return counter;
}

void checkReturnStatus(int32_t slotId, Return<void>& ret, serviceType srvType) {
    if (ret.isOk() == false) {
        RLOGE("checkReturnStatus: unable to call response/indication callback");
        // Remote process hosting the callbacks must be dead. Reset the callback objects;
        // there's no other recovery to be done here. When the client process is back up, it will
        // call setResponseFunctions()

        // Caller should already hold rdlock, release that first
        // note the current counter to avoid overwriting updates made by another thread before
        // write lock is acquired.
        int counter = getRadioServiceCounter(slotId, srvType);

        pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(slotId);
        int ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);

        //Add synchronization between IRadio callbacks and service creation.

        // acquire wrlock
        ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
        assert(ret == 0);

        // make sure the counter value has not changed
        if (counter == getRadioServiceCounter(slotId, srvType)) {
            if (srvType == RADIO_SERVICE) {
                radioService[slotId]->mRadioResponse = NULL;
                radioService[slotId]->mRadioIndication = NULL;
                radioService[slotId]->mRadioResponseV1_1 = NULL;
                radioService[slotId]->mRadioIndicationV1_1 = NULL;
                radioService[slotId]->mRadioResponseV1_2 = NULL;
                radioService[slotId]->mRadioIndicationV1_2 = NULL;
                radioService[slotId]->mRadioResponseV1_3 = NULL;
                radioService[slotId]->mRadioIndicationV1_3 = NULL;
                radioService[slotId]->mRadioResponseV1_4 = NULL;
                radioService[slotId]->mRadioIndicationV1_4 = NULL;

                mCounterRadio[slotId]++;
            } else if (srvType == RADIOINTERACTOR_SERVICE) {
                extRadioService[slotId]->mExtRadioResponse = NULL;
                extRadioService[slotId]->mExtRadioIndication = NULL;

                mCounterExtRadio[slotId]++;
            } else if (srvType == ATC_SERVICE) {
                extRadioService[slotId]->mAtcRadioResponse = NULL;
                extRadioService[slotId]->mAtcRadioIndication = NULL;

                mCounterAtcRadio[slotId]++;
            } else if (srvType == OEM_SERVICE) {
                oemHookService[slotId]->mOemHookResponse = NULL;
                oemHookService[slotId]->mOemHookIndication = NULL;

                mCounterOemHook[slotId]++;
            } else {
                RLOGE("checkReturnStatus: invalid service type %d", srvType);
            }
        } else {
            RLOGE("checkReturnStatus: not resetting responseFunctions as they likely "
                    "got updated on another thread");
        }

        // release wrlock
        ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);

        // Reacquire rdlock
        ret = pthread_rwlock_rdlock(radioServiceRwlockPtr);
        assert(ret == 0);
    }
}

void RadioImpl::checkReturnStatus(Return<void>& ret) {
    ::checkReturnStatus(mSlotId, ret, RADIO_SERVICE);
}

Return<void> RadioImpl::setResponseFunctions(
        const ::android::sp<IRadioResponse>& radioResponseParam,
        const ::android::sp<IRadioIndication>& radioIndicationParam) {
    RLOGD("setResponseFunctions");

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mRadioResponse = radioResponseParam;
    mRadioIndication = radioIndicationParam;
    mRadioResponseV1_1 = V1_1::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_1 = V1_1::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_1 == nullptr || mRadioIndicationV1_1 == nullptr) {
        mRadioResponseV1_1 = nullptr;
        mRadioIndicationV1_1 = nullptr;
    }
    mRadioResponseV1_2 = V1_2::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_2 = V1_2::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_2 == nullptr || mRadioIndicationV1_2 == nullptr) {
        mRadioResponseV1_2 = nullptr;
        mRadioIndicationV1_2 = nullptr;
    }
    mRadioResponseV1_3 = V1_3::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_3 = V1_3::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_3 == nullptr || mRadioIndicationV1_3 == nullptr) {
        mRadioResponseV1_3 = nullptr;
        mRadioIndicationV1_3 = nullptr;
    }
    mRadioResponseV1_4 = V1_4::IRadioResponse::castFrom(mRadioResponse).withDefault(nullptr);
    mRadioIndicationV1_4 = V1_4::IRadioIndication::castFrom(mRadioIndication).withDefault(nullptr);
    if (mRadioResponseV1_4 == nullptr || mRadioIndicationV1_4 == nullptr) {
        mRadioResponseV1_4 = nullptr;
        mRadioIndicationV1_4 = nullptr;
    }

    mCounterRadio[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    // client is connected. Send initial indications.
    android::onNewCommandConnect((RIL_SOCKET_ID) mSlotId);

    return Void();
}

Return<void> RadioImpl::getIccCardStatus(int32_t serial) {
#if VDBG
    RLOGD("getIccCardStatus: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_SIM_STATUS);
    return Void();
}

Return<void> RadioImpl::supplyIccPinForApp(int32_t serial, const hidl_string& pin,
        const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPinForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PIN, true,
            2, pin.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyIccPukForApp(int32_t serial, const hidl_string& puk,
                                           const hidl_string& pin, const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPukForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PUK, true,
            3, puk.c_str(), pin.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyIccPin2ForApp(int32_t serial, const hidl_string& pin2,
                                            const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPin2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PIN2, true,
            2, pin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyIccPuk2ForApp(int32_t serial, const hidl_string& puk2,
                                            const hidl_string& pin2, const hidl_string& aid) {
#if VDBG
    RLOGD("supplyIccPuk2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_SIM_PUK2, true,
            3, puk2.c_str(), pin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::changeIccPinForApp(int32_t serial, const hidl_string& oldPin,
                                           const hidl_string& newPin, const hidl_string& aid) {
#if VDBG
    RLOGD("changeIccPinForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_SIM_PIN, true,
            3, oldPin.c_str(), newPin.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::changeIccPin2ForApp(int32_t serial, const hidl_string& oldPin2,
                                            const hidl_string& newPin2, const hidl_string& aid) {
#if VDBG
    RLOGD("changeIccPin2ForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_SIM_PIN2, true,
            3, oldPin2.c_str(), newPin2.c_str(), aid.c_str());
    return Void();
}

Return<void> RadioImpl::supplyNetworkDepersonalization(int32_t serial,
                                                       const hidl_string& netPin) {
#if VDBG
    RLOGD("supplyNetworkDepersonalization: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION,
            true, 1, netPin.c_str());
    return Void();
}

Return<void> RadioImpl::getCurrentCalls(int32_t serial) {
#if VDBG
    RLOGD("getCurrentCalls: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CURRENT_CALLS);
    return Void();
}

Return<void> RadioImpl::dial(int32_t serial, const Dial& dialInfo) {
#if VDBG
    RLOGD("dial: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_DIAL);
    if (pRI == NULL) {
        return Void();
    }
    RIL_Dial *dial = (RIL_Dial *)calloc(1, sizeof(RIL_Dial));
    if (dial == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    RIL_UUS_Info *uusInfo = (RIL_UUS_Info *)calloc(1, sizeof(RIL_UUS_Info));
    if (uusInfo == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        free(dial);
        return Void();
    }
    if (!copyHidlStringToRil(&dial->address, dialInfo.address, pRI)) {
        free(dial);
        free(uusInfo);
        return Void();
    }
    dial->clir = (int) dialInfo.clir;

    if (dialInfo.uusInfo.size() != 0) {
        uusInfo->uusType = (RIL_UUS_Type) dialInfo.uusInfo[0].uusType;
        uusInfo->uusDcs = (RIL_UUS_DCS) dialInfo.uusInfo[0].uusDcs;

        if (dialInfo.uusInfo[0].uusData.size() == 0) {
            uusInfo->uusData = NULL;
            uusInfo->uusLength = 0;
        } else {
            if (!copyHidlStringToRil(&uusInfo->uusData, dialInfo.uusInfo[0].uusData, pRI)) {
                memsetAndFreeStrings(1, dial->address);
                free(dial);
                free(uusInfo);
                return Void();
            }
            uusInfo->uusLength = dialInfo.uusInfo[0].uusData.size();
        }

        dial->uusInfo = uusInfo;
    } else {
        memset(uusInfo, 0, sizeof(RIL_UUS_Info));
        free(uusInfo);
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_Dial *pDial = (RIL_Dial *)data;
        RIL_UUS_Info *pUusInfo = pDial->uusInfo;

        memsetAndFreeStrings(1, pDial->address);
        if (pUusInfo != NULL) {
            memsetAndFreeStrings(1, pUusInfo->uusData);
            memset(pUusInfo, 0, sizeof(RIL_UUS_Info));
            free(pUusInfo);
        }
        memset(pDial, 0, sizeof(RIL_Dial));
        free(pDial);
    };
    REQUEST_INFO_PACK(pRI, cb, dial, sizeof(RIL_Dial));

    CALL_ONREQUEST(RIL_REQUEST_DIAL, dial, sizeof(RIL_Dial), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::getImsiForApp(int32_t serial, const hidl_string& aid) {
#if VDBG
    RLOGD("getImsiForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_GET_IMSI, false,
            1, aid.c_str());
    return Void();
}

Return<void> RadioImpl::hangup(int32_t serial, int32_t gsmIndex) {
#if VDBG
    RLOGD("hangup: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_HANGUP, 1, gsmIndex);
    return Void();
}

Return<void> RadioImpl::hangupWaitingOrBackground(int32_t serial) {
#if VDBG
    RLOGD("hangupWaitingOrBackground: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND);
    return Void();
}

Return<void> RadioImpl::hangupForegroundResumeBackground(int32_t serial) {
#if VDBG
    RLOGD("hangupForegroundResumeBackground: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND);
    return Void();
}

Return<void> RadioImpl::switchWaitingOrHoldingAndActive(int32_t serial) {
#if VDBG
    RLOGD("switchWaitingOrHoldingAndActive: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE);
    return Void();
}

Return<void> RadioImpl::conference(int32_t serial) {
#if VDBG
    RLOGD("conference: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CONFERENCE);
    return Void();
}

Return<void> RadioImpl::rejectCall(int32_t serial) {
#if VDBG
    RLOGD("rejectCall: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_UDUB);
    return Void();
}

Return<void> RadioImpl::getLastCallFailCause(int32_t serial) {
#if VDBG
    RLOGD("getLastCallFailCause: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_LAST_CALL_FAIL_CAUSE);
    return Void();
}

Return<void> RadioImpl::getSignalStrength(int32_t serial) {
#if VDBG
    RLOGD("getSignalStrength: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SIGNAL_STRENGTH);
    return Void();
}

Return<void> RadioImpl::getSignalStrength_1_4(int32_t serial) {
#if VDBG
    RLOGD("getSignalStrength: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SIGNAL_STRENGTH);
    return Void();
}

Return<void> RadioImpl::getVoiceRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getVoiceRegistrationState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_VOICE_REGISTRATION_STATE);
    return Void();
}

Return<void> RadioImpl::getDataRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getDataRegistrationState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DATA_REGISTRATION_STATE);
    return Void();
}

Return<void> RadioImpl::getOperator(int32_t serial) {
#if VDBG
    RLOGD("getOperator: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_OPERATOR);
    return Void();
}

Return<void> RadioImpl::setRadioPower(int32_t serial, bool on) {
    RLOGD("setRadioPower: serial %d on %d", serial, on);
    dispatchInts(serial, mSlotId, RIL_REQUEST_RADIO_POWER, 1, BOOL_TO_INT(on));
    return Void();
}

Return<void> RadioImpl::sendDtmf(int32_t serial, const hidl_string& s) {
#if VDBG
    RLOGD("sendDtmf: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_DTMF, s.c_str());
    return Void();
}

Return<void> RadioImpl::sendSms(int32_t serial, const GsmSmsMessage& message) {
#if VDBG
    RLOGD("sendSms: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SEND_SMS, false,
            2, message.smscPdu.c_str(), message.pdu.c_str());
    return Void();
}

Return<void> RadioImpl::sendSMSExpectMore(int32_t serial, const GsmSmsMessage& message) {
#if VDBG
    RLOGD("sendSMSExpectMore: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SEND_SMS_EXPECT_MORE, false,
            2, message.smscPdu.c_str(), message.pdu.c_str());
    return Void();
}

static bool convertMvnoTypeToString(MvnoType type, char *&str) {
    switch (type) {
        case MvnoType::IMSI:
            str = (char *)"imsi";
            return true;
        case MvnoType::GID:
            str = (char *)"gid";
            return true;
        case MvnoType::SPN:
            str = (char *)"spn";
            return true;
        case MvnoType::NONE:
            str = (char *)"";
            return true;
    }
    return false;
}

Return<void> RadioImpl::setupDataCall(int32_t serial, RadioTechnology radioTechnology,
                                      const DataProfileInfo& dataProfileInfo, bool modemCognitive,
                                      bool roamingAllowed, bool isRoaming) {
#if VDBG
    RLOGD("setupDataCall: serial %d", serial);
#endif

    if (s_vendorFunctions->version >= 4 && s_vendorFunctions->version <= 14) {
        const hidl_string &protocol =
                (isRoaming ? dataProfileInfo.roamingProtocol : dataProfileInfo.protocol);
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 7,
            std::to_string((int) radioTechnology + 2).c_str(),
            std::to_string((int) dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int) dataProfileInfo.authType).c_str(),
            protocol.c_str());
    } else if (s_vendorFunctions->version >= 15) {
        char *mvnoTypeStr = NULL;
        if (!convertMvnoTypeToString(dataProfileInfo.mvnoType, mvnoTypeStr)) {
            RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                    RIL_REQUEST_SETUP_DATA_CALL);
            if (pRI != NULL) {
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            }
            return Void();
        }
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 15,
            std::to_string((int) radioTechnology + 2).c_str(),
            std::to_string((int) dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int) dataProfileInfo.authType).c_str(),
            dataProfileInfo.protocol.c_str(),
            dataProfileInfo.roamingProtocol.c_str(),
            std::to_string(dataProfileInfo.supportedApnTypesBitmap).c_str(),
            std::to_string(dataProfileInfo.bearerBitmap).c_str(),
            modemCognitive ? "1" : "0",
            std::to_string(dataProfileInfo.mtu).c_str(),
            mvnoTypeStr,
            dataProfileInfo.mvnoMatchData.c_str(),
            roamingAllowed ? "1" : "0");
    } else {
        RLOGE("Unsupported RIL version %d, min version expected 4", s_vendorFunctions->version);
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SETUP_DATA_CALL);
        if (pRI != NULL) {
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
    }
    return Void();
}

Return<void> RadioImpl::setupDataCall_1_2(int32_t serial, V1_2::AccessNetwork accessNetwork,
        const V1_0::DataProfileInfo& dataProfileInfo, bool modemCognitive,
        bool roamingAllowed, bool isRoaming, V1_2::DataRequestReason reason,
        const hidl_vec<hidl_string>& addresses, const hidl_vec<hidl_string>& dnses) {
#if VDBG
    RLOGD("setupDataCall_1_2: serial %d", serial);
#endif

    if (s_vendorFunctions->version >= 4 && s_vendorFunctions->version <= 14) {
        const hidl_string &protocol =
                (isRoaming ? dataProfileInfo.roamingProtocol : dataProfileInfo.protocol);
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 7,
            std::to_string((int)accessNetwork).c_str(),
            std::to_string((int)dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int)dataProfileInfo.authType).c_str(),
            protocol.c_str());
    } else if (s_vendorFunctions->version >= 15) {
        char *mvnoTypeStr = NULL;
        if (!convertMvnoTypeToString(dataProfileInfo.mvnoType, mvnoTypeStr)) {
            RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                    RIL_REQUEST_SETUP_DATA_CALL);
            if (pRI != NULL) {
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            }
            return Void();
        }
        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 15,
            std::to_string((int)accessNetwork).c_str(),
            std::to_string((int)dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int) dataProfileInfo.authType).c_str(),
            dataProfileInfo.protocol.c_str(),
            dataProfileInfo.roamingProtocol.c_str(),
            std::to_string(dataProfileInfo.supportedApnTypesBitmap).c_str(),
            std::to_string(dataProfileInfo.bearerBitmap).c_str(),
            modemCognitive ? "1" : "0",
            std::to_string(dataProfileInfo.mtu).c_str(),
            mvnoTypeStr,
            dataProfileInfo.mvnoMatchData.c_str(),
            roamingAllowed ? "1" : "0");
    } else {
        RLOGE("Unsupported RIL version %d, min version expected 4", s_vendorFunctions->version);
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SETUP_DATA_CALL);
        if (pRI != NULL) {
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
    }
    return Void();
}

void convertPdpTypeToStr(RIL_PdpProtocolType type, char *str, size_t size) {
    const char *ret = NULL;
    switch (type) {
        case PDP_PROTOCOL_TYPE_IP:
            ret = "IP";
            break;
        case PDP_PROTOCOL_TYPE_IPV6:
            ret = "IPV6";
            break;
        case PDP_PROTOCOL_TYPE_IPV4V6:
            ret = "IPV4V6";
            break;
        case PDP_PROTOCOL_TYPE_PPP:
            ret = "PPP";
            break;
        case PDP_PROTOCOL_TYPE_NON_IP:
            ret = "NON_IP";
            break;
        case PDP_PROTOCOL_TYPE_UNSTRUCTURED:
            ret = "UNSTRUCTURED";
            break;
        case PDP_PROTOCOL_TYPE_UNKNOWN:
            ret = "";
            break;
        default:
            ret = "IP";
            break;
    }

    snprintf(str, size, "%s", ret);
}

Return<void> RadioImpl::setupDataCall_1_4(int32_t serial, V1_4::AccessNetwork accessNetwork,
        const V1_4::DataProfileInfo& dataProfileInfo, bool roamingAllowed,
        V1_2::DataRequestReason reason, const hidl_vec<hidl_string>& addresses,
        const hidl_vec<hidl_string>& dnses) {
#if VDBG
    RLOGD("setupDataCall_1_4: serial %d", serial);
#endif

    if (s_vendorFunctions->version >= 4 && s_vendorFunctions->version <= 14) {
        char protocol[32] = {0};
        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.protocol,
                protocol, sizeof(protocol));

        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 7,
            std::to_string((int)accessNetwork).c_str(),
            std::to_string((int)dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int)dataProfileInfo.authType).c_str(),
            protocol);
    } else if (s_vendorFunctions->version >= 15) {
        char protocol[32] = {0};
        char roamingProtocol[32] = {0};

        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.protocol,
                protocol, sizeof(protocol));
        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.roamingProtocol,
                roamingProtocol, sizeof(roamingProtocol));

        dispatchStrings(serial, mSlotId, RIL_REQUEST_SETUP_DATA_CALL, true, 15,
            std::to_string((int)accessNetwork).c_str(),
            std::to_string((int)dataProfileInfo.profileId).c_str(),
            dataProfileInfo.apn.c_str(),
            dataProfileInfo.user.c_str(),
            dataProfileInfo.password.c_str(),
            std::to_string((int)dataProfileInfo.authType).c_str(),
            protocol,
            roamingProtocol,
            std::to_string(dataProfileInfo.supportedApnTypesBitmap).c_str(),
            std::to_string(dataProfileInfo.bearerBitmap).c_str(),
            //modemCognitive ? "1" : "0",
            std::to_string(dataProfileInfo.mtu).c_str(),
            //mvnoTypeStr,
            //dataProfileInfo.mvnoMatchData.c_str(),
            roamingAllowed ? "1" : "0");
    } else {
        RLOGE("Unsupported RIL version %d, min version expected 4", s_vendorFunctions->version);
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SETUP_DATA_CALL);
        if (pRI != NULL) {
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
    }
    return Void();
}

Return<void> RadioImpl::iccIOForApp(int32_t serial, const IccIo& iccIo) {
#if VDBG
    RLOGD("iccIOForApp: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_IO);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SIM_IO_v6 *rilIccIo = (RIL_SIM_IO_v6 *)calloc(1, sizeof(RIL_SIM_IO_v6));
    if (rilIccIo == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    rilIccIo->command = iccIo.command;
    rilIccIo->fileid = iccIo.fileId;
    if (!copyHidlStringToRil(&rilIccIo->path, iccIo.path, pRI)) {
        free(rilIccIo);
        return Void();
    }

    rilIccIo->p1 = iccIo.p1;
    rilIccIo->p2 = iccIo.p2;
    rilIccIo->p3 = iccIo.p3;

    if (!copyHidlStringToRil(&rilIccIo->data, iccIo.data, pRI)) {
        memsetAndFreeStrings(1, rilIccIo->path);
        free(rilIccIo);
        return Void();
    }

    if (!copyHidlStringToRil(&rilIccIo->pin2, iccIo.pin2, pRI)) {
        memsetAndFreeStrings(2, rilIccIo->path, rilIccIo->data);
        free(rilIccIo);
        return Void();
    }

    if (!copyHidlStringToRil(&rilIccIo->aidPtr, iccIo.aid, pRI)) {
        memsetAndFreeStrings(3, rilIccIo->path, rilIccIo->data, rilIccIo->pin2);
        free(rilIccIo);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_SIM_IO_v6 *pSimIO = (RIL_SIM_IO_v6 *)data;
        memsetAndFreeStrings(4, pSimIO->path, pSimIO->data, pSimIO->pin2, pSimIO->aidPtr);
        memset(pSimIO, 0, dataLen);
        free(pSimIO);
    };
    REQUEST_INFO_PACK(pRI, cb, rilIccIo, sizeof(RIL_SIM_IO_v6));

    CALL_ONREQUEST(RIL_REQUEST_SIM_IO, rilIccIo, sizeof(RIL_SIM_IO_v6), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::sendUssd(int32_t serial, const hidl_string& ussd) {
#if VDBG
    RLOGD("sendUssd: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SEND_USSD, ussd.c_str());
    return Void();
}

Return<void> RadioImpl::cancelPendingUssd(int32_t serial) {
#if VDBG
    RLOGD("cancelPendingUssd: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CANCEL_USSD);
    return Void();
}

Return<void> RadioImpl::getClir(int32_t serial) {
#if VDBG
    RLOGD("getClir: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CLIR);
    return Void();
}

Return<void> RadioImpl::setClir(int32_t serial, int32_t status) {
#if VDBG
    RLOGD("setClir: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_CLIR, 1, status);
    return Void();
}

Return<void> RadioImpl::getCallForwardStatus(int32_t serial, const CallForwardInfo& callInfo) {
#if VDBG
    RLOGD("getCallForwardStatus: serial %d", serial);
#endif
    dispatchCallForwardStatus(serial, mSlotId, RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
            callInfo);
    return Void();
}

Return<void> RadioImpl::setCallForward(int32_t serial, const CallForwardInfo& callInfo) {
#if VDBG
    RLOGD("setCallForward: serial %d", serial);
#endif
    dispatchCallForwardStatus(serial, mSlotId, RIL_REQUEST_SET_CALL_FORWARD,
            callInfo);
    return Void();
}

Return<void> RadioImpl::getCallWaiting(int32_t serial, int32_t serviceClass) {
#if VDBG
    RLOGD("getCallWaiting: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_QUERY_CALL_WAITING, 1, serviceClass);
    return Void();
}

Return<void> RadioImpl::setCallWaiting(int32_t serial, bool enable, int32_t serviceClass) {
#if VDBG
    RLOGD("setCallWaiting: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_CALL_WAITING, 2, BOOL_TO_INT(enable),
            serviceClass);
    return Void();
}

Return<void> RadioImpl::acknowledgeLastIncomingGsmSms(int32_t serial,
                                                      bool success, SmsAcknowledgeFailCause cause) {
#if VDBG
    RLOGD("acknowledgeLastIncomingGsmSms: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SMS_ACKNOWLEDGE, 2, BOOL_TO_INT(success),
            cause);
    return Void();
}

Return<void> RadioImpl::acceptCall(int32_t serial) {
#if VDBG
    RLOGD("acceptCall: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_ANSWER);
    return Void();
}

Return<void> RadioImpl::deactivateDataCall(int32_t serial,
                                           int32_t cid, bool reasonRadioShutDown) {
#if VDBG
    RLOGD("deactivateDataCall: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_DEACTIVATE_DATA_CALL, false,
            2, (std::to_string(cid)).c_str(), reasonRadioShutDown ? "1" : "0");
    return Void();
}

Return<void> RadioImpl::deactivateDataCall_1_2(int32_t serial, int32_t cid,
        V1_2::DataRequestReason reason) {
#if VDBG
    RLOGD("deactivateDataCall: serial %d", serial);
#endif

    RIL_DataRequestReason dataRequestReason = (RIL_DataRequestReason)reason;
    const char *reasonStr = NULL;
    switch (dataRequestReason) {
        case DATA_REQ_REASOPN_NORMAL:
            reasonStr = "normal";
            break;
        case DATA_REQ_REASOPN_SHUTDOWN:
            reasonStr = "shutdown";
            break;
        case DATA_REQ_REASOPN_HANDOVER:
            reasonStr = "handover";
            break;
        default:
            reasonStr = "unknown";
            break;
    }

    dispatchStrings(serial, mSlotId, RIL_REQUEST_DEACTIVATE_DATA_CALL, false,
            2, (std::to_string(cid)).c_str(), reasonStr);
    return Void();
}

Return<void> RadioImpl::getFacilityLockForApp(int32_t serial, const hidl_string& facility,
                                              const hidl_string& password, int32_t serviceClass,
                                              const hidl_string& appId) {
#if VDBG
    RLOGD("getFacilityLockForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_QUERY_FACILITY_LOCK, true,
            4, facility.c_str(), password.c_str(),
            (std::to_string(serviceClass)).c_str(), appId.c_str());
    return Void();
}

Return<void> RadioImpl::setFacilityLockForApp(int32_t serial, const hidl_string& facility,
                                              bool lockState, const hidl_string& password,
                                              int32_t serviceClass, const hidl_string& appId) {
#if VDBG
    RLOGD("setFacilityLockForApp: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_SET_FACILITY_LOCK, true,
            5, facility.c_str(), lockState ? "1" : "0", password.c_str(),
            (std::to_string(serviceClass)).c_str(), appId.c_str() );
    return Void();
}

Return<void> RadioImpl::setBarringPassword(int32_t serial, const hidl_string& facility,
                                           const hidl_string& oldPassword,
                                           const hidl_string& newPassword) {
#if VDBG
    RLOGD("setBarringPassword: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CHANGE_BARRING_PASSWORD, true,
            3, facility.c_str(), oldPassword.c_str(), newPassword.c_str());
    return Void();
}

Return<void> RadioImpl::getNetworkSelectionMode(int32_t serial) {
#if VDBG
    RLOGD("getNetworkSelectionMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE);
    return Void();
}

Return<void> RadioImpl::setNetworkSelectionModeAutomatic(int32_t serial) {
#if VDBG
    RLOGD("setNetworkSelectionModeAutomatic: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC);
    return Void();
}

Return<void> RadioImpl::setNetworkSelectionModeManual(int32_t serial,
                                                      const hidl_string& operatorNumeric) {
#if VDBG
    RLOGD("setNetworkSelectionModeManual: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL,
            operatorNumeric.c_str());
    return Void();
}

Return<void> RadioImpl::getAvailableNetworks(int32_t serial) {
#if VDBG
    RLOGD("getAvailableNetworks: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_AVAILABLE_NETWORKS);
    return Void();
}

Return<void> RadioImpl::startNetworkScan(int32_t serial, const V1_1::NetworkScanRequest& request) {
#if VDBG
    RLOGD("startNetworkScan: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_START_NETWORK_SCAN);
    if (pRI == NULL) {
        return Void();
    }

    if (request.specifiers.size() > MAX_RADIO_ACCESS_NETWORKS) {
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_NetworkScanRequest_v1_2 *scan_request =
            (RIL_NetworkScanRequest_v1_2 *)calloc(1, sizeof(RIL_NetworkScanRequest_v1_2));
    if (scan_request == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    scan_request->type = (RIL_ScanType) request.type;
    scan_request->interval = request.interval;
    scan_request->specifiers_length = request.specifiers.size();
    for (size_t i = 0; i < request.specifiers.size(); ++i) {
        if (request.specifiers[i].geranBands.size() > MAX_BANDS ||
            request.specifiers[i].utranBands.size() > MAX_BANDS ||
            request.specifiers[i].eutranBands.size() > MAX_BANDS ||
            request.specifiers[i].channels.size() > MAX_CHANNELS) {
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            free(scan_request);
            return Void();
        }
        const V1_1::RadioAccessSpecifier& ras_from = request.specifiers[i];
        RIL_RadioAccessSpecifier& ras_to = scan_request->specifiers[i];

        ras_to.radio_access_network = (RIL_RadioAccessNetworks) ras_from.radioAccessNetwork;
        ras_to.channels_length = ras_from.channels.size();

        std::copy(ras_from.channels.begin(), ras_from.channels.end(), ras_to.channels);
        const std::vector<uint32_t> * bands = nullptr;
        switch (request.specifiers[i].radioAccessNetwork) {
            case V1_1::RadioAccessNetworks::GERAN:
                ras_to.bands_length = ras_from.geranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.geranBands;
                break;
            case V1_1::RadioAccessNetworks::UTRAN:
                ras_to.bands_length = ras_from.utranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.utranBands;
                break;
            case V1_1::RadioAccessNetworks::EUTRAN:
                ras_to.bands_length = ras_from.eutranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.eutranBands;
                break;
            default:
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                free(scan_request);
                return Void();
        }
        // safe to copy to geran_bands because it's a union member
        for (size_t idx = 0; idx < ras_to.bands_length; ++idx) {
            ras_to.bands.geran_bands[idx] = (RIL_GeranBands) (*bands)[idx];
        }
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_NetworkScanRequest_v1_2 *scanRequest = (RIL_NetworkScanRequest_v1_2 *)data;
        if (scanRequest->mccMncs != NULL) {
            for (size_t i = 0; i < scanRequest->mccMncsNumbers; i++) {
                memsetAndFreeStrings(1, scanRequest->mccMncs[i]);
            }
            free(scanRequest->mccMncs);
        }
        memset(scanRequest, 0, dataLen);
        free(scanRequest);
    };
    REQUEST_INFO_PACK(pRI, cb, scan_request, sizeof(RIL_NetworkScanRequest_v1_2));

    CALL_ONREQUEST(RIL_REQUEST_START_NETWORK_SCAN, scan_request, sizeof(RIL_NetworkScanRequest_v1_2), pRI,
            pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::startNetworkScan_1_2(int32_t serial, const V1_2::NetworkScanRequest& request) {
#if VDBG
    RLOGD("startNetworkScan_1_2: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_START_NETWORK_SCAN);
    if (pRI == NULL) {
        return Void();
    }

    if (request.specifiers.size() > MAX_RADIO_ACCESS_NETWORKS ||
            request.specifiers.size() <= 0 ||
            request.interval < SCAN_INITERVAL_MIN ||
            request.interval > SCAN_INITERVAL_MAX ||
            request.maxSearchTime < MAX_SERACH_TIME_MIN ||
            request.maxSearchTime > MAX_SERACH_TIME_MAX ||
            request.incrementalResultsPeriodicity < INCREMENT_RESULTS_PERIOD_MIN ||
            request.incrementalResultsPeriodicity > INCREMENT_RESULTS_PERIOD_MAX) {
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_NetworkScanRequest_v1_2 *scan_request =
            (RIL_NetworkScanRequest_v1_2 *)calloc(1, sizeof(RIL_NetworkScanRequest_v1_2));
    if (scan_request == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    scan_request->type = (RIL_ScanType)request.type;
    scan_request->interval = request.interval;
    scan_request->specifiers_length = request.specifiers.size();
    scan_request->maxSearchTime = request.maxSearchTime;
    scan_request->incrementalResults = request.incrementalResults;
    scan_request->incrementalResultsPeriodicity = request.incrementalResultsPeriodicity;
    scan_request->mccMncsNumbers = request.mccMncs.size();
    for (size_t i = 0; i < request.specifiers.size(); ++i) {
        if (request.specifiers[i].geranBands.size() > MAX_BANDS ||
            request.specifiers[i].utranBands.size() > MAX_BANDS ||
            request.specifiers[i].eutranBands.size() > MAX_BANDS ||
            request.specifiers[i].channels.size() > MAX_CHANNELS) {
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            free(scan_request);
            return Void();
        }
        const V1_1::RadioAccessSpecifier& ras_from =
                request.specifiers[i];
        RIL_RadioAccessSpecifier& ras_to = scan_request->specifiers[i];

        ras_to.radio_access_network = (RIL_RadioAccessNetworks) ras_from.radioAccessNetwork;
        ras_to.channels_length = ras_from.channels.size();

        std::copy(ras_from.channels.begin(), ras_from.channels.end(), ras_to.channels);
        const std::vector<uint32_t> * bands = nullptr;
        switch (request.specifiers[i].radioAccessNetwork) {
            case V1_1::RadioAccessNetworks::GERAN:
                ras_to.bands_length = ras_from.geranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.geranBands;
                break;
            case V1_1::RadioAccessNetworks::UTRAN:
                ras_to.bands_length = ras_from.utranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.utranBands;
                break;
            case V1_1::RadioAccessNetworks::EUTRAN:
                ras_to.bands_length = ras_from.eutranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.eutranBands;
                break;
            default:
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                free(scan_request);
                return Void();
        }
        // safe to copy to geran_bands because it's a union member
        for (size_t idx = 0; idx < ras_to.bands_length; ++idx) {
            ras_to.bands.geran_bands[idx] = (RIL_GeranBands) (*bands)[idx];
        }
    }

    if (scan_request->mccMncsNumbers != 0) {
        char **pStrings = (char **)calloc(scan_request->mccMncsNumbers, sizeof(char *));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            free(scan_request);
            return Void();
        }
        for (size_t i = 0; i < request.mccMncs.size(); ++i) {
            if (!copyHidlStringToRil(&pStrings[i], hidl_string(request.mccMncs[i]), pRI)) {
                for (size_t j = 0; j < i; j++) {
                    memsetAndFreeStrings(1, pStrings[j]);
                }
                free(pStrings);
                free(scan_request);
                return Void();
            }
        }
        scan_request->mccMncs = pStrings;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_NetworkScanRequest_v1_2 *scanRequest = (RIL_NetworkScanRequest_v1_2 *)data;
        if (scanRequest->mccMncs != NULL) {
            for (size_t i = 0; i < scanRequest->mccMncsNumbers; i++) {
                memsetAndFreeStrings(1, scanRequest->mccMncs[i]);
            }
            free(scanRequest->mccMncs);
        }
        memset(scanRequest, 0, dataLen);
        free(scanRequest);
    };
    REQUEST_INFO_PACK(pRI, cb, scan_request, sizeof(RIL_NetworkScanRequest_v1_2));

    CALL_ONREQUEST(RIL_REQUEST_START_NETWORK_SCAN, scan_request, sizeof(RIL_NetworkScanRequest_v1_2), pRI,
            pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::startNetworkScan_1_4(int32_t serial, const V1_2::NetworkScanRequest& request) {
#if VDBG
    RLOGD("startNetworkScan_1_4: serial %d", serial);
#endif

    startNetworkScan_1_2(serial, request);
    return Void();
}

Return<void> RadioImpl::stopNetworkScan(int32_t serial) {
#if VDBG
    RLOGD("stopNetworkScan: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_STOP_NETWORK_SCAN);
    return Void();
}

Return<void> RadioImpl::startDtmf(int32_t serial, const hidl_string& s) {
#if VDBG
    RLOGD("startDtmf: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_DTMF_START,
            s.c_str());
    return Void();
}

Return<void> RadioImpl::stopDtmf(int32_t serial) {
#if VDBG
    RLOGD("stopDtmf: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DTMF_STOP);
    return Void();
}

Return<void> RadioImpl::getBasebandVersion(int32_t serial) {
#if VDBG
    RLOGD("getBasebandVersion: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_BASEBAND_VERSION);
    return Void();
}

Return<void> RadioImpl::separateConnection(int32_t serial, int32_t gsmIndex) {
#if VDBG
    RLOGD("separateConnection: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SEPARATE_CONNECTION, 1, gsmIndex);
    return Void();
}

Return<void> RadioImpl::setMute(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setMute: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_MUTE, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::getMute(int32_t serial) {
#if VDBG
    RLOGD("getMute: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_MUTE);
    return Void();
}

Return<void> RadioImpl::getClip(int32_t serial) {
#if VDBG
    RLOGD("getClip: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_CLIP);
    return Void();
}

Return<void> RadioImpl::getDataCallList(int32_t serial) {
#if VDBG
    RLOGD("getDataCallList: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DATA_CALL_LIST);
    return Void();
}

Return<void> RadioImpl::setSuppServiceNotifications(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setSuppServiceNotifications: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION, 1,
            BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::writeSmsToSim(int32_t serial, const SmsWriteArgs& smsWriteArgs) {
#if VDBG
    RLOGD("writeSmsToSim: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_WRITE_SMS_TO_SIM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SMS_WriteArgs *args = (RIL_SMS_WriteArgs *)calloc(1, sizeof(RIL_SMS_WriteArgs));
    if (args == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    args->status = (int) smsWriteArgs.status;

    if (!copyHidlStringToRil(&args->pdu, smsWriteArgs.pdu, pRI)) {
        free(args);
        return Void();
    }

    if (!copyHidlStringToRil(&args->smsc, smsWriteArgs.smsc, pRI)) {
        memsetAndFreeStrings(1, args->pdu);
        free(args);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_SMS_WriteArgs *pArgs = (RIL_SMS_WriteArgs *)data;

        memsetAndFreeStrings(2, pArgs->smsc, pArgs->pdu);

        memset(pArgs, 0, dataLen);
        free(pArgs);
    };
    REQUEST_INFO_PACK(pRI, cb, args, sizeof(RIL_SMS_WriteArgs));

    CALL_ONREQUEST(RIL_REQUEST_WRITE_SMS_TO_SIM, args, sizeof(RIL_SMS_WriteArgs), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::deleteSmsOnSim(int32_t serial, int32_t index) {
#if VDBG
    RLOGD("deleteSmsOnSim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_DELETE_SMS_ON_SIM, 1, index);
    return Void();
}

Return<void> RadioImpl::setBandMode(int32_t serial, RadioBandMode mode) {
#if VDBG
    RLOGD("setBandMode: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_BAND_MODE, 1, mode);
    return Void();
}

Return<void> RadioImpl::getAvailableBandModes(int32_t serial) {
#if VDBG
    RLOGD("getAvailableBandModes: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE);
    return Void();
}

Return<void> RadioImpl::sendEnvelope(int32_t serial, const hidl_string& command) {
#if VDBG
    RLOGD("sendEnvelope: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND,
            command.c_str());
    return Void();
}

Return<void> RadioImpl::sendTerminalResponseToSim(int32_t serial,
                                                  const hidl_string& commandResponse) {
#if VDBG
    RLOGD("sendTerminalResponseToSim: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE,
            commandResponse.c_str());
    return Void();
}

Return<void> RadioImpl::handleStkCallSetupRequestFromSim(int32_t serial, bool accept) {
#if VDBG
    RLOGD("handleStkCallSetupRequestFromSim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM,
            1, BOOL_TO_INT(accept));
    return Void();
}

Return<void> RadioImpl::explicitCallTransfer(int32_t serial) {
#if VDBG
    RLOGD("explicitCallTransfer: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_EXPLICIT_CALL_TRANSFER);
    return Void();
}

Return<void> RadioImpl::setPreferredNetworkType(int32_t serial, PreferredNetworkType nwType) {
#if VDBG
    RLOGD("setPreferredNetworkType: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE, 1, nwType);
    return Void();
}

Return<void> RadioImpl::getPreferredNetworkType(int32_t serial) {
#if VDBG
    RLOGD("getPreferredNetworkType: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE);
    return Void();
}

Return<void> RadioImpl::getNeighboringCids(int32_t serial) {
#if VDBG
    RLOGD("getNeighboringCids: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_NEIGHBORING_CELL_IDS);
    return Void();
}

Return<void> RadioImpl::setLocationUpdates(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setLocationUpdates: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_LOCATION_UPDATES, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::setCdmaSubscriptionSource(int32_t serial, CdmaSubscriptionSource cdmaSub) {
#if VDBG
    RLOGD("setCdmaSubscriptionSource: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_SUBSCRIPTION_SOURCE, 1, cdmaSub);
    return Void();
}

Return<void> RadioImpl::setCdmaRoamingPreference(int32_t serial, CdmaRoamingType type) {
#if VDBG
    RLOGD("setCdmaRoamingPreference: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_ROAMING_PREFERENCE, 1, type);
    return Void();
}

Return<void> RadioImpl::getCdmaRoamingPreference(int32_t serial) {
#if VDBG
    RLOGD("getCdmaRoamingPreference: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_QUERY_ROAMING_PREFERENCE);
    return Void();
}

Return<void> RadioImpl::setTTYMode(int32_t serial, TtyMode mode) {
#if VDBG
    RLOGD("setTTYMode: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_TTY_MODE, 1, mode);
    return Void();
}

Return<void> RadioImpl::getTTYMode(int32_t serial) {
#if VDBG
    RLOGD("getTTYMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_QUERY_TTY_MODE);
    return Void();
}

Return<void> RadioImpl::setPreferredVoicePrivacy(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setPreferredVoicePrivacy: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SET_PREFERRED_VOICE_PRIVACY_MODE,
            1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> RadioImpl::getPreferredVoicePrivacy(int32_t serial) {
#if VDBG
    RLOGD("getPreferredVoicePrivacy: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_QUERY_PREFERRED_VOICE_PRIVACY_MODE);
    return Void();
}

Return<void> RadioImpl::sendCDMAFeatureCode(int32_t serial, const hidl_string& featureCode) {
#if VDBG
    RLOGD("sendCDMAFeatureCode: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_CDMA_FLASH,
            featureCode.c_str());
    return Void();
}

Return<void> RadioImpl::sendBurstDtmf(int32_t serial, const hidl_string& dtmf, int32_t on,
                                      int32_t off) {
#if VDBG
    RLOGD("sendBurstDtmf: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_CDMA_BURST_DTMF, false,
            3, dtmf.c_str(), (std::to_string(on)).c_str(),
            (std::to_string(off)).c_str());
    return Void();
}

void constructCdmaSms(RIL_CDMA_SMS_Message &rcsm, const CdmaSmsMessage& sms) {
    rcsm.uTeleserviceID = sms.teleserviceId;
    rcsm.bIsServicePresent = BOOL_TO_INT(sms.isServicePresent);
    rcsm.uServicecategory = sms.serviceCategory;
    rcsm.sAddress.digit_mode = (RIL_CDMA_SMS_DigitMode) sms.address.digitMode;
    rcsm.sAddress.number_mode = (RIL_CDMA_SMS_NumberMode) sms.address.numberMode;
    rcsm.sAddress.number_type = (RIL_CDMA_SMS_NumberType) sms.address.numberType;
    rcsm.sAddress.number_plan = (RIL_CDMA_SMS_NumberPlan) sms.address.numberPlan;

    rcsm.sAddress.number_of_digits = sms.address.digits.size();
    int digitLimit= MIN((rcsm.sAddress.number_of_digits), RIL_CDMA_SMS_ADDRESS_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.sAddress.digits[i] = sms.address.digits[i];
    }

    rcsm.sSubAddress.subaddressType = (RIL_CDMA_SMS_SubaddressType) sms.subAddress.subaddressType;
    rcsm.sSubAddress.odd = BOOL_TO_INT(sms.subAddress.odd);

    rcsm.sSubAddress.number_of_digits = sms.subAddress.digits.size();
    digitLimit= MIN((rcsm.sSubAddress.number_of_digits), RIL_CDMA_SMS_SUBADDRESS_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.sSubAddress.digits[i] = sms.subAddress.digits[i];
    }

    rcsm.uBearerDataLen = sms.bearerData.size();
    digitLimit= MIN((rcsm.uBearerDataLen), RIL_CDMA_SMS_BEARER_DATA_MAX);
    for (int i = 0; i < digitLimit; i++) {
        rcsm.aBearerData[i] = sms.bearerData[i];
    }
}

Return<void> RadioImpl::sendCdmaSms(int32_t serial, const CdmaSmsMessage& sms) {
#if VDBG
    RLOGD("sendCdmaSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_CDMA_SEND_SMS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_Message *rcsm = (RIL_CDMA_SMS_Message *)calloc(1, sizeof(RIL_CDMA_SMS_Message));
    if (rcsm == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    constructCdmaSms(*rcsm, sms);

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, rcsm, sizeof(RIL_CDMA_SMS_Message));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rcsm, sizeof(RIL_CDMA_SMS_Message), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::acknowledgeLastIncomingCdmaSms(int32_t serial, const CdmaSmsAck& smsAck) {
#if VDBG
    RLOGD("acknowledgeLastIncomingCdmaSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_CDMA_SMS_ACKNOWLEDGE);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_Ack *rcsa = (RIL_CDMA_SMS_Ack *)calloc(1, sizeof(RIL_CDMA_SMS_Ack));
    if (rcsa == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    rcsa->uErrorClass = (RIL_CDMA_SMS_ErrorClass) smsAck.errorClass;
    rcsa->uSMSCauseCode = smsAck.smsCauseCode;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, rcsa, sizeof(RIL_CDMA_SMS_Ack));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rcsa, sizeof(RIL_CDMA_SMS_Ack), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::getGsmBroadcastConfig(int32_t serial) {
#if VDBG
    RLOGD("getGsmBroadcastConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG);
    return Void();
}

Return<void> RadioImpl::setGsmBroadcastConfig(int32_t serial,
                                              const hidl_vec<GsmBroadcastSmsConfigInfo>&
                                              configInfo) {
#if VDBG
    RLOGD("setGsmBroadcastConfig: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG);
    if (pRI == NULL) {
        return Void();
    }

    int num = configInfo.size();
    RIL_GSM_BroadcastSmsConfigInfo **gsmBci = (RIL_GSM_BroadcastSmsConfigInfo **)
            calloc(num, sizeof(RIL_GSM_BroadcastSmsConfigInfo *));
    if (gsmBci == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    int i = 0;
    for (i = 0 ; i < num; i++) {
        gsmBci[i] = (RIL_GSM_BroadcastSmsConfigInfo *)
                calloc(1, sizeof(RIL_GSM_BroadcastSmsConfigInfo));
    }

    for (i = 0 ; i < num ; i++ ) {
        gsmBci[i]->fromServiceId = configInfo[i].fromServiceId;
        gsmBci[i]->toServiceId = configInfo[i].toServiceId;
        gsmBci[i]->fromCodeScheme = configInfo[i].fromCodeScheme;
        gsmBci[i]->toCodeScheme = configInfo[i].toCodeScheme;
        gsmBci[i]->selected = BOOL_TO_INT(configInfo[i].selected);
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_GSM_BroadcastSmsConfigInfo **ppGsmBci =
                (RIL_GSM_BroadcastSmsConfigInfo **)data;
        int num = dataLen / sizeof(RIL_GSM_BroadcastSmsConfigInfo *);

        int i = 0;
        for (i = 0; i < num; i++) {
            memset(ppGsmBci[i], 0, sizeof(RIL_GSM_BroadcastSmsConfigInfo));
            free(ppGsmBci[i]);
        }

        memset(ppGsmBci, 0, dataLen);
        free(ppGsmBci);
    };
    REQUEST_INFO_PACK(pRI, cb, gsmBci, (num * sizeof(RIL_GSM_BroadcastSmsConfigInfo *)));

    CALL_ONREQUEST(pRI->pCI->requestNumber, gsmBci,
            num * sizeof(RIL_GSM_BroadcastSmsConfigInfo *), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setGsmBroadcastActivation(int32_t serial, bool activate) {
#if VDBG
    RLOGD("setGsmBroadcastActivation: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION,
            1, BOOL_TO_INT(!activate));
    return Void();
}

Return<void> RadioImpl::getCdmaBroadcastConfig(int32_t serial) {
#if VDBG
    RLOGD("getCdmaBroadcastConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_GET_BROADCAST_SMS_CONFIG);
    return Void();
}

Return<void> RadioImpl::setCdmaBroadcastConfig(int32_t serial,
                                               const hidl_vec<CdmaBroadcastSmsConfigInfo>&
                                               configInfo) {
#if VDBG
    RLOGD("setCdmaBroadcastConfig: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_CDMA_SET_BROADCAST_SMS_CONFIG);
    if (pRI == NULL) {
        return Void();
    }

    int num = configInfo.size();
    RIL_CDMA_BroadcastSmsConfigInfo **cdmaBci = (RIL_CDMA_BroadcastSmsConfigInfo **)
            calloc(num, sizeof(RIL_CDMA_BroadcastSmsConfigInfo *));
    if (cdmaBci == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    int i = 0;
    for (i = 0; i < num; i++) {
        cdmaBci[i] = (RIL_CDMA_BroadcastSmsConfigInfo *)
                calloc(1, sizeof(RIL_CDMA_BroadcastSmsConfigInfo));
    }

    for (i = 0 ; i < num ; i++ ) {
        cdmaBci[i]->service_category = configInfo[i].serviceCategory;
        cdmaBci[i]->language = configInfo[i].language;
        cdmaBci[i]->selected = BOOL_TO_INT(configInfo[i].selected);
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CDMA_BroadcastSmsConfigInfo **ppCdmaBci =
                (RIL_CDMA_BroadcastSmsConfigInfo **)data;
        int num = dataLen / sizeof(RIL_CDMA_BroadcastSmsConfigInfo *);

        int i = 0;
        for (i = 0; i < num; i++) {
            memset(ppCdmaBci[i], 0, sizeof(RIL_CDMA_BroadcastSmsConfigInfo));
            free(ppCdmaBci[i]);
        }

        memset(ppCdmaBci, 0, dataLen);
        free(ppCdmaBci);
    };
    REQUEST_INFO_PACK(pRI, cb, cdmaBci, (num * sizeof(RIL_CDMA_BroadcastSmsConfigInfo *)));

    CALL_ONREQUEST(pRI->pCI->requestNumber, cdmaBci,
            num * sizeof(RIL_CDMA_BroadcastSmsConfigInfo *), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setCdmaBroadcastActivation(int32_t serial, bool activate) {
#if VDBG
    RLOGD("setCdmaBroadcastActivation: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_SMS_BROADCAST_ACTIVATION,
            1, BOOL_TO_INT(!activate));
    return Void();
}

Return<void> RadioImpl::getCDMASubscription(int32_t serial) {
#if VDBG
    RLOGD("getCDMASubscription: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_SUBSCRIPTION);
    return Void();
}

Return<void> RadioImpl::writeSmsToRuim(int32_t serial, const CdmaSmsWriteArgs& cdmaSms) {
#if VDBG
    RLOGD("writeSmsToRuim: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_CDMA_WRITE_SMS_TO_RUIM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CDMA_SMS_WriteArgs *rcsw = (RIL_CDMA_SMS_WriteArgs *)
            calloc(1, sizeof(RIL_CDMA_SMS_WriteArgs));
    if (rcsw == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    rcsw->status = (int) cdmaSms.status;
    constructCdmaSms(rcsw->message, cdmaSms.message);

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, rcsw, sizeof(RIL_CDMA_SMS_WriteArgs));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rcsw, sizeof(RIL_CDMA_SMS_WriteArgs), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::deleteSmsOnRuim(int32_t serial, int32_t index) {
#if VDBG
    RLOGD("deleteSmsOnRuim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_CDMA_DELETE_SMS_ON_RUIM, 1, index);
    return Void();
}

Return<void> RadioImpl::getDeviceIdentity(int32_t serial) {
#if VDBG
    RLOGD("getDeviceIdentity: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_DEVICE_IDENTITY);
    return Void();
}

Return<void> RadioImpl::exitEmergencyCallbackMode(int32_t serial) {
#if VDBG
    RLOGD("exitEmergencyCallbackMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE);
    return Void();
}

Return<void> RadioImpl::getSmscAddress(int32_t serial) {
#if VDBG
    RLOGD("getSmscAddress: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_SMSC_ADDRESS);
    return Void();
}

Return<void> RadioImpl::setSmscAddress(int32_t serial, const hidl_string& smsc) {
#if VDBG
    RLOGD("setSmscAddress: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_SET_SMSC_ADDRESS,
            smsc.c_str());
    return Void();
}

Return<void> RadioImpl::reportSmsMemoryStatus(int32_t serial, bool available) {
#if VDBG
    RLOGD("reportSmsMemoryStatus: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_REPORT_SMS_MEMORY_STATUS, 1,
            BOOL_TO_INT(available));
    return Void();
}

Return<void> RadioImpl::reportStkServiceIsRunning(int32_t serial) {
#if VDBG
    RLOGD("reportStkServiceIsRunning: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING);
    return Void();
}

Return<void> RadioImpl::getCdmaSubscriptionSource(int32_t serial) {
#if VDBG
    RLOGD("getCdmaSubscriptionSource: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_CDMA_GET_SUBSCRIPTION_SOURCE);
    return Void();
}

Return<void> RadioImpl::requestIsimAuthentication(int32_t serial, const hidl_string& challenge) {
#if VDBG
    RLOGD("requestIsimAuthentication: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_ISIM_AUTHENTICATION,
            challenge.c_str());
    return Void();
}

Return<void> RadioImpl::acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool success,
                                                         const hidl_string& ackPdu) {
#if VDBG
    RLOGD("acknowledgeIncomingGsmSmsWithPdu: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU,
            false, 2, success ? "1" : "0", ackPdu.c_str());
    return Void();
}

Return<void> RadioImpl::sendEnvelopeWithStatus(int32_t serial, const hidl_string& contents) {
#if VDBG
    RLOGD("sendEnvelopeWithStatus: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS,
            contents.c_str());
    return Void();
}

Return<void> RadioImpl::getVoiceRadioTechnology(int32_t serial) {
#if VDBG
    RLOGD("getVoiceRadioTechnology: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_VOICE_RADIO_TECH);
    return Void();
}

Return<void> RadioImpl::getCellInfoList(int32_t serial) {
#if VDBG
    RLOGD("getCellInfoList: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CELL_INFO_LIST);
    return Void();
}

Return<void> RadioImpl::setCellInfoListRate(int32_t serial, int32_t rate) {
#if VDBG
    RLOGD("setCellInfoListRate: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE, 1, rate);
    return Void();
}

Return<void> RadioImpl::setInitialAttachApn(int32_t serial, const DataProfileInfo& dataProfileInfo,
                                            bool modemCognitive, bool isRoaming) {
#if VDBG
    RLOGD("setInitialAttachApn: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_INITIAL_ATTACH_APN);
    if (pRI == NULL) {
        return Void();
    }

    if (s_vendorFunctions->version <= 14) {
        RIL_InitialAttachApn *iaa = (RIL_InitialAttachApn *)
                calloc(1, sizeof(RIL_InitialAttachApn));
        if (iaa == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->apn, dataProfileInfo.apn, pRI, true)) {
            free(iaa);
            return Void();
        }

        const hidl_string &protocol =
                (isRoaming ? dataProfileInfo.roamingProtocol : dataProfileInfo.protocol);

        if (!copyHidlStringToRil(&iaa->protocol, protocol, pRI)) {
            memsetAndFreeStrings(1, iaa->apn);
            free(iaa);
            return Void();
        }
        iaa->authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa->username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(2, iaa->apn, iaa->protocol);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(3, iaa->apn, iaa->protocol, iaa->username);
            free(iaa);
            return Void();
        }

        // free memory when RIL_onRequestComplete
        auto cb = [](void *data, size_t dataLen) {
            RIL_InitialAttachApn *pIAA = (RIL_InitialAttachApn *)data;

            memsetAndFreeStrings(4, pIAA->apn, pIAA->protocol, pIAA->username, pIAA->password);
            memset(data, 0, dataLen);
            free(pIAA);
        };
        REQUEST_INFO_PACK(pRI, cb, iaa, sizeof(RIL_InitialAttachApn));

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, iaa, sizeof(RIL_InitialAttachApn), pRI, pRI->socket_id);
    } else {
        RIL_InitialAttachApn_v15 *iaa = (RIL_InitialAttachApn_v15 *)
                calloc(1, sizeof(RIL_InitialAttachApn_v15));
        if (iaa == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->apn, dataProfileInfo.apn, pRI, true)) {
            free(iaa);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->protocol, dataProfileInfo.protocol, pRI)) {
            memsetAndFreeStrings(1, iaa->apn);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->roamingProtocol, dataProfileInfo.roamingProtocol, pRI)) {
            memsetAndFreeStrings(2, iaa->apn, iaa->protocol);
            free(iaa);
            return Void();
        }
        iaa->authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa->username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(3, iaa->apn, iaa->protocol, iaa->roamingProtocol);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(4, iaa->apn, iaa->protocol, iaa->roamingProtocol, iaa->username);
            free(iaa);
            return Void();
        }
        iaa->supportedTypesBitmask = dataProfileInfo.supportedApnTypesBitmap;
        iaa->bearerBitmask = dataProfileInfo.bearerBitmap;
        iaa->modemCognitive = BOOL_TO_INT(modemCognitive);
        iaa->mtu = dataProfileInfo.mtu;

        if (!convertMvnoTypeToString(dataProfileInfo.mvnoType, iaa->mvnoType)) {
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            memsetAndFreeStrings(5, iaa->apn, iaa->protocol, iaa->roamingProtocol, iaa->username,
                    iaa->password);
            free(iaa);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->mvnoMatchData, dataProfileInfo.mvnoMatchData, pRI)) {
            memsetAndFreeStrings(5, iaa->apn, iaa->protocol, iaa->roamingProtocol, iaa->username,
                    iaa->password);
            free(iaa);
            return Void();
        }

        // free memory when RIL_onRequestComplete
        auto cb = [](void *data, size_t dataLen) {
            RIL_InitialAttachApn_v15 *pIAA = (RIL_InitialAttachApn_v15 *)data;

            memsetAndFreeStrings(7, pIAA->apn, pIAA->protocol, pIAA->roamingProtocol,
                    pIAA->username, pIAA->password, pIAA->mvnoType, pIAA->mvnoMatchData);
            memset(data, 0, dataLen);
            free(pIAA);
        };
        REQUEST_INFO_PACK(pRI, cb, iaa, sizeof(RIL_InitialAttachApn_v15));

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, iaa, sizeof(RIL_InitialAttachApn_v15), pRI, pRI->socket_id);
    }

    return Void();
}

Return<void> RadioImpl::setInitialAttachApn_1_4(int32_t serial,
        const V1_4::DataProfileInfo& dataProfileInfo) {
#if VDBG
    RLOGD("setInitialAttachApn_1_4: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_INITIAL_ATTACH_APN);
    if (pRI == NULL) {
        return Void();
    }

    if (s_vendorFunctions->version <= 14) {
        RIL_InitialAttachApn *iaa = (RIL_InitialAttachApn *)
                calloc(1, sizeof(RIL_InitialAttachApn));
        if (iaa == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->apn, dataProfileInfo.apn, pRI, true)) {
            free(iaa);
            return Void();
        }

        iaa->protocol = (char *)calloc(32, sizeof(char));
        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.protocol,
                iaa->protocol, 32);

        iaa->authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa->username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(2, iaa->apn, iaa->protocol);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(3, iaa->apn, iaa->protocol, iaa->username);
            free(iaa);
            return Void();
        }

        // free memory when RIL_onRequestComplete
        auto cb = [](void *data, size_t dataLen) {
            RIL_InitialAttachApn *pIAA = (RIL_InitialAttachApn *)data;

            memsetAndFreeStrings(4, pIAA->apn, pIAA->protocol, pIAA->username, pIAA->password);
            memset(data, 0, dataLen);
            free(pIAA);
        };
        REQUEST_INFO_PACK(pRI, cb, iaa, sizeof(RIL_InitialAttachApn));

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, iaa, sizeof(RIL_InitialAttachApn), pRI, pRI->socket_id);
    } else {
        RIL_InitialAttachApn_v15 *iaa = (RIL_InitialAttachApn_v15 *)
                calloc(1, sizeof(RIL_InitialAttachApn_v15));
        if (iaa == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->apn, dataProfileInfo.apn, pRI, true)) {
            free(iaa);
            return Void();
        }

        iaa->protocol = (char *)calloc(32, sizeof(char));
        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.protocol,
                iaa->protocol, 32);

        iaa->roamingProtocol = (char *)calloc(32, sizeof(char));
        convertPdpTypeToStr((RIL_PdpProtocolType)dataProfileInfo.roamingProtocol,
                iaa->roamingProtocol, 32);

        iaa->authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa->username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(3, iaa->apn, iaa->protocol, iaa->roamingProtocol);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(4, iaa->apn, iaa->protocol, iaa->roamingProtocol, iaa->username);
            free(iaa);
            return Void();
        }
        iaa->supportedTypesBitmask = dataProfileInfo.supportedApnTypesBitmap;
        iaa->bearerBitmask = dataProfileInfo.bearerBitmap;
        //iaa->modemCognitive = BOOL_TO_INT(modemCognitive);
        iaa->mtu = dataProfileInfo.mtu;

        // free memory when RIL_onRequestComplete
        auto cb = [](void *data, size_t dataLen) {
            RIL_InitialAttachApn_v15 *pIAA = (RIL_InitialAttachApn_v15 *)data;

            memsetAndFreeStrings(7, pIAA->apn, pIAA->protocol, pIAA->roamingProtocol,
                    pIAA->username, pIAA->password, pIAA->mvnoType, pIAA->mvnoMatchData);
            memset(data, 0, dataLen);
            free(pIAA);
        };
        REQUEST_INFO_PACK(pRI, cb, iaa, sizeof(RIL_InitialAttachApn_v15));

        CALL_ONREQUEST(RIL_REQUEST_SET_INITIAL_ATTACH_APN, iaa,
                sizeof(RIL_InitialAttachApn_v15), pRI, pRI->socket_id);
    }

    return Void();
}

Return<void> RadioImpl::getImsRegistrationState(int32_t serial) {
#if VDBG
    RLOGD("getImsRegistrationState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_IMS_REGISTRATION_STATE);
    return Void();
}

bool dispatchImsGsmSms(const ImsSmsMessage& message, RequestInfo *pRI) {
    RIL_IMS_SMS_Message *rism =
            (RIL_IMS_SMS_Message *)calloc(1, sizeof(RIL_IMS_SMS_Message));
    if (rism == NULL) {
        RLOGE("dispatchImsGsmSms: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    char **pStrings;
    int countStrings = 2;
    int dataLen = sizeof(char *) * countStrings;

    rism->tech = RADIO_TECH_3GPP;
    rism->retry = BOOL_TO_INT(message.retry);
    rism->messageRef = message.messageRef;

    if (message.gsmMessage.size() != 1) {
        RLOGE("dispatchImsGsmSms: Invalid len %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        free(rism);
        return false;
    }

    pStrings = (char **)calloc(countStrings, sizeof(char *));
    if (pStrings == NULL) {
        RLOGE("dispatchImsGsmSms: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        free(rism);
        return false;
    }

    if (!copyHidlStringToRil(&pStrings[0], message.gsmMessage[0].smscPdu, pRI)) {
#ifdef MEMSET_FREED
        memset(pStrings, 0, dataLen);
#endif
        free(pStrings);
        free(rism);
        return false;
    }

    if (!copyHidlStringToRil(&pStrings[1], message.gsmMessage[0].pdu, pRI)) {
        memsetAndFreeStrings(1, pStrings[0]);
#ifdef MEMSET_FREED
        memset(pStrings, 0, dataLen);
#endif
        free(pStrings);
        free(rism);
        return false;
    }

    rism->message.gsmMessage = pStrings;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_IMS_SMS_Message *pRism = (RIL_IMS_SMS_Message *)data;
        int countStrings = (dataLen - sizeof(RIL_RadioTechnologyFamily) -
                sizeof(uint8_t) - sizeof(int32_t)) / sizeof(char *);
        for (int i = 0; i < countStrings; i++) {
            memsetAndFreeStrings(1, pRism->message.gsmMessage[i]);
        }
        free(pRism->message.gsmMessage);
        free(pRism);
    };
    REQUEST_INFO_PACK(pRI, cb, rism, (sizeof(RIL_RadioTechnologyFamily) +
            sizeof(uint8_t) + sizeof(int32_t) + dataLen));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rism, sizeof(RIL_RadioTechnologyFamily) +
            sizeof(uint8_t) + sizeof(int32_t) + dataLen, pRI, pRI->socket_id);

    return true;
}

bool dispatchImsCdmaSms(const ImsSmsMessage& message, RequestInfo *pRI) {
    ImsCdmaSms *temp = (ImsCdmaSms *)calloc(1, sizeof(ImsCdmaSms));
    if (temp == NULL) {
        RLOGE("dispatchImsGsmSms: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return false;
    }

    if (message.cdmaMessage.size() != 1) {
        RLOGE("dispatchImsCdmaSms: Invalid len %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        free(temp);
        return false;
    }

    temp->imsSms.tech = RADIO_TECH_3GPP2;
    temp->imsSms.retry = BOOL_TO_INT(message.retry);
    temp->imsSms.messageRef = message.messageRef;
    temp->imsSms.message.cdmaMessage = &temp->cdmaSms;

    constructCdmaSms(temp->cdmaSms, message.cdmaMessage[0]);

    // Vendor code expects payload length to include actual msg payload
    // (sizeof(RIL_CDMA_SMS_Message)) instead of (RIL_CDMA_SMS_Message *) + size of other fields in
    // RIL_IMS_SMS_Message
    int payloadLen = sizeof(RIL_RadioTechnologyFamily) + sizeof(uint8_t) + sizeof(int32_t)
            + sizeof(RIL_CDMA_SMS_Message);

    RIL_IMS_SMS_Message *pImsSms =
                (RIL_IMS_SMS_Message *)calloc(1, sizeof(RIL_IMS_SMS_Message));
    memcpy(pImsSms, &temp->imsSms, sizeof(RIL_IMS_SMS_Message));

    pImsSms->message.cdmaMessage =
            (RIL_CDMA_SMS_Message *)calloc(1, sizeof(RIL_CDMA_SMS_Message));
    memcpy(pImsSms->message.cdmaMessage, &temp->cdmaSms, sizeof(RIL_CDMA_SMS_Message));

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_IMS_SMS_Message *pImsSms = (RIL_IMS_SMS_Message *)data;
        free(pImsSms->message.cdmaMessage);
        free(pImsSms);
    };
    REQUEST_INFO_PACK(pRI, cb, pImsSms, payloadLen);

    CALL_ONREQUEST(pRI->pCI->requestNumber, pImsSms, payloadLen, pRI, pRI->socket_id);

    free(temp);
    return true;
}

Return<void> RadioImpl::sendImsSms(int32_t serial, const ImsSmsMessage& message) {
#if VDBG
    RLOGD("sendImsSms: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_IMS_SEND_SMS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_RadioTechnologyFamily format = (RIL_RadioTechnologyFamily) message.tech;

    if (RADIO_TECH_3GPP == format) {
        dispatchImsGsmSms(message, pRI);
    } else if (RADIO_TECH_3GPP2 == format) {
        dispatchImsCdmaSms(message, pRI);
    } else {
        RLOGE("sendImsSms: Invalid radio tech %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
    }
    return Void();
}

Return<void> RadioImpl::iccTransmitApduBasicChannel(int32_t serial, const SimApdu& message) {
#if VDBG
    RLOGD("iccTransmitApduBasicChannel: serial %d", serial);
#endif
    dispatchIccApdu(serial, mSlotId, RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC, message);
    return Void();
}

Return<void> RadioImpl::iccOpenLogicalChannel(int32_t serial, const hidl_string& aid, int32_t p2) {
#if VDBG
    RLOGD("iccOpenLogicalChannel: serial %d", serial);
#endif
//    if (s_vendorFunctions->version < 15) {
//        dispatchString(serial, mSlotId, RIL_REQUEST_SIM_OPEN_CHANNEL, aid.c_str());
//    } else {
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_OPEN_CHANNEL);
    if (pRI == NULL) {
        return Void();
    }

    RIL_OpenChannelParams *params = (RIL_OpenChannelParams *)calloc(1, sizeof(RIL_OpenChannelParams));
    if (params == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    params->p2 = p2;

    if (!copyHidlStringToRil(&params->aidPtr, aid, pRI)) {
        free(params);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_OpenChannelParams *pParams = (RIL_OpenChannelParams *)data;

        memsetAndFreeStrings(1, pParams->aidPtr);

        memset(pParams, 0, dataLen);
        free(pParams);
    };
    REQUEST_INFO_PACK(pRI, cb, params, sizeof(RIL_OpenChannelParams));

    CALL_ONREQUEST(pRI->pCI->requestNumber, params, sizeof(RIL_OpenChannelParams), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::iccCloseLogicalChannel(int32_t serial, int32_t channelId) {
#if VDBG
    RLOGD("iccCloseLogicalChannel: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SIM_CLOSE_CHANNEL, 1, channelId);
    return Void();
}

Return<void> RadioImpl::iccTransmitApduLogicalChannel(int32_t serial, const SimApdu& message) {
#if VDBG
    RLOGD("iccTransmitApduLogicalChannel: serial %d", serial);
#endif
    dispatchIccApdu(serial, mSlotId, RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL, message);
    return Void();
}

Return<void> RadioImpl::nvReadItem(int32_t serial, NvItem itemId) {
#if VDBG
    RLOGD("nvReadItem: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_NV_READ_ITEM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_NV_ReadItem *nvri = (RIL_NV_ReadItem *)calloc(1, sizeof(RIL_NV_ReadItem));
    if (nvri == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    nvri->itemID = (RIL_NV_Item) itemId;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, nvri, sizeof(RIL_NV_ReadItem));

    CALL_ONREQUEST(pRI->pCI->requestNumber, nvri, sizeof(RIL_NV_ReadItem), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::nvWriteItem(int32_t serial, const NvWriteItem& item) {
#if VDBG
    RLOGD("nvWriteItem: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_NV_WRITE_ITEM);
    if (pRI == NULL) {
        return Void();
    }

    RIL_NV_WriteItem *nvwi = (RIL_NV_WriteItem *)calloc(1, sizeof(RIL_NV_WriteItem));
    if (nvwi == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    nvwi->itemID = (RIL_NV_Item) item.itemId;

    if (!copyHidlStringToRil(&nvwi->value, item.value, pRI)) {
        free(nvwi);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_NV_WriteItem *pNvwi = (RIL_NV_WriteItem *)data;

        memsetAndFreeStrings(1, pNvwi->value);

        memset(pNvwi, 0, dataLen);
        free(pNvwi);
    };
    REQUEST_INFO_PACK(pRI, cb, nvwi, sizeof(RIL_NV_WriteItem));

    CALL_ONREQUEST(pRI->pCI->requestNumber, nvwi, sizeof(RIL_NV_WriteItem), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::nvWriteCdmaPrl(int32_t serial, const hidl_vec<uint8_t>& prl) {
#if VDBG
    RLOGD("nvWriteCdmaPrl: serial %d", serial);
#endif
    dispatchRaw(serial, mSlotId, RIL_REQUEST_NV_WRITE_CDMA_PRL, prl);
    return Void();
}

Return<void> RadioImpl::nvResetConfig(int32_t serial, ResetNvType resetType) {
    int rilResetType = -1;
#if VDBG
    RLOGD("nvResetConfig: serial %d", serial);
#endif
    /* Convert ResetNvType to RIL.h values
     * RIL_REQUEST_NV_RESET_CONFIG
     * 1 - reload all NV items
     * 2 - erase NV reset (SCRTN)
     * 3 - factory reset (RTN)
     */
    switch(resetType) {
      case ResetNvType::RELOAD:
        rilResetType = 1;
        break;
      case ResetNvType::ERASE:
        rilResetType = 2;
        break;
      case ResetNvType::FACTORY_RESET:
        rilResetType = 3;
        break;
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_NV_RESET_CONFIG, 1, rilResetType);
    return Void();
}

Return<void> RadioImpl::setUiccSubscription(int32_t serial, const SelectUiccSub& uiccSub) {
#if VDBG
    RLOGD("setUiccSubscription: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_UICC_SUBSCRIPTION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SelectUiccSub *rilUiccSub = (RIL_SelectUiccSub *)calloc(1, sizeof(RIL_SelectUiccSub));
    if (rilUiccSub == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    rilUiccSub->slot = uiccSub.slot;
    rilUiccSub->app_index = uiccSub.appIndex;
    rilUiccSub->sub_type = (RIL_SubscriptionType) uiccSub.subType;
    rilUiccSub->act_status = (RIL_UiccSubActStatus) uiccSub.actStatus;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, rilUiccSub, sizeof(RIL_SelectUiccSub));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rilUiccSub, sizeof(RIL_SelectUiccSub), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setDataAllowed(int32_t serial, bool allow) {
#if VDBG
    RLOGD("setDataAllowed: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_ALLOW_DATA, 1, BOOL_TO_INT(allow));
    return Void();
}

Return<void> RadioImpl::getHardwareConfig(int32_t serial) {
#if VDBG
    RLOGD("getHardwareConfig: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_HARDWARE_CONFIG);
    return Void();
}

Return<void> RadioImpl::requestIccSimAuthentication(int32_t serial, int32_t authContext,
        const hidl_string& authData, const hidl_string& aid) {
#if VDBG
    RLOGD("requestIccSimAuthentication: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SIM_AUTHENTICATION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_SimAuthentication *pf = (RIL_SimAuthentication *)calloc(1, sizeof(RIL_SimAuthentication));
    if (pf == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    pf->authContext = authContext;

    if (!copyHidlStringToRil(&pf->authData, authData, pRI)) {
        free(pf);
        return Void();
    }

    if (!copyHidlStringToRil(&pf->aid, aid, pRI)) {
        memsetAndFreeStrings(1, pf->authData);
        free(pf);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_SimAuthentication *pSimAuth = (RIL_SimAuthentication *)data;

        memsetAndFreeStrings(2, pSimAuth->authData, pSimAuth->aid);

        memset(pSimAuth, 0, dataLen);
        free(pSimAuth);
    };
    REQUEST_INFO_PACK(pRI, cb, pf, sizeof(RIL_SimAuthentication));

    CALL_ONREQUEST(pRI->pCI->requestNumber, pf, sizeof(RIL_SimAuthentication), pRI, pRI->socket_id);

    return Void();
}

/**
 * @param numProfiles number of data profile
 * @param dataProfiles the pointer to the actual data profiles. The acceptable type is
          RIL_DataProfileInfo or RIL_DataProfileInfo_v15.
 * @param dataProfilePtrs the pointer to the pointers that point to each data profile structure
 * @param numfields number of string-type member in the data profile structure
 * @param ... the variadic parameters are pointers to each string-type member
 **/
template <typename T>
void freeSetDataProfileData(int numProfiles, T *dataProfiles, T **dataProfilePtrs,
                            int numfields, ...) {
    va_list args;
    va_start(args, numfields);

    // Iterate through each string-type field that need to be free.
    for (int i = 0; i < numfields; i++) {
        // Iterate through each data profile and free that specific string-type field.
        // The type 'char *T::*' is a type of pointer to a 'char *' member inside T structure.
        char *T::*ptr = va_arg(args, char *T::*);
        for (int j = 0; j < numProfiles; j++) {
            memsetAndFreeStrings(1, dataProfiles[j].*ptr);
        }
    }

    va_end(args);

#ifdef MEMSET_FREED
    memset(dataProfiles, 0, numProfiles * sizeof(T));
    memset(dataProfilePtrs, 0, numProfiles * sizeof(T *));
#endif
    free(dataProfiles);
    free(dataProfilePtrs);
}

RIL_PdpProtocolType convertStrToPdpType(const char *str) {
    if (str == NULL) {
        return PDP_PROTOCOL_TYPE_UNKNOWN;
    }

    if (strcmp(str, "IP") == 0) {
        return PDP_PROTOCOL_TYPE_IP;
    } else if (strcmp(str, "IPV6") == 0) {
        return PDP_PROTOCOL_TYPE_IPV6;
    } else if (strcmp(str, "IPV4V6") == 0) {
        return PDP_PROTOCOL_TYPE_IPV4V6;
    } else if (strcmp(str, "PPP") == 0) {
        return PDP_PROTOCOL_TYPE_PPP;
    } else if (strcmp(str, "NON_IP") == 0) {
        return PDP_PROTOCOL_TYPE_NON_IP;
    } else if (strcmp(str, "UNSTRUCTURED") == 0) {
        return PDP_PROTOCOL_TYPE_UNSTRUCTURED;
    } else {
        return PDP_PROTOCOL_TYPE_IP;
    }
}

Return<void> RadioImpl::setDataProfile(int32_t serial, const hidl_vec<DataProfileInfo>& profiles,
                                       bool isRoaming) {
#if VDBG
    RLOGD("setDataProfile: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_DATA_PROFILE);
    if (pRI == NULL) {
        return Void();
    }

    size_t num = profiles.size();
    bool success = false;

    RIL_DataProfileInfo_v1_4 *dataProfiles =
        (RIL_DataProfileInfo_v1_4 *)calloc(num, sizeof(RIL_DataProfileInfo_v1_4));
    if (dataProfiles == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    RIL_DataProfileInfo_v1_4 **dataProfilePtrs =
        (RIL_DataProfileInfo_v1_4 **)calloc(num, sizeof(RIL_DataProfileInfo_v1_4 *));
    if (dataProfilePtrs == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        free(dataProfiles);
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    for (size_t i = 0; i < num; i++) {
        dataProfilePtrs[i] = &dataProfiles[i];

        success = copyHidlStringToRil(&dataProfiles[i].apn, profiles[i].apn,
                pRI, true);

        if (success && !copyHidlStringToRil(&dataProfiles[i].user,
                profiles[i].user, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&dataProfiles[i].password,
                profiles[i].password,
                pRI, true)) {
            success = false;
        }

        if (!success) {
            freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 3,
                &RIL_DataProfileInfo_v1_4::apn, &RIL_DataProfileInfo_v1_4::user,
                &RIL_DataProfileInfo_v1_4::password);
            return Void();
        }

        dataProfiles[i].profileId = (RIL_DataProfile)profiles[i].profileId;
        dataProfiles[i].protocol = convertStrToPdpType(profiles[i].protocol.c_str());
        dataProfiles[i].roamingProtocol = convertStrToPdpType(profiles[i].roamingProtocol.c_str());
        dataProfiles[i].authType = (RIL_ApnAuthType)profiles[i].authType;
        dataProfiles[i].type = (RIL_DataProfileInfoType)profiles[i].type;
        dataProfiles[i].maxConnsTime = profiles[i].maxConnsTime;
        dataProfiles[i].maxConns = profiles[i].maxConns;
        dataProfiles[i].waitTime = profiles[i].waitTime;
        dataProfiles[i].enabled = profiles[i].enabled;
        dataProfiles[i].bearerBitmap = profiles[i].bearerBitmap;
        dataProfiles[i].mtu = profiles[i].mtu;
        dataProfiles[i].preferred = profiles[i].mtu;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_DataProfileInfo_v1_4 **dataProfiles = (RIL_DataProfileInfo_v1_4 **)data;
        int num = dataLen / sizeof(RIL_DataProfileInfo_v1_4 *);
        int i = 0;
        for (i = 0; i < num; i++) {
            memsetAndFreeStrings(3, dataProfiles[i]->apn, dataProfiles[i]->user,
                    dataProfiles[i]->password);
        }
        free(dataProfiles[0]);

        memset(dataProfiles, 0, dataLen);
        free(dataProfiles);
    };
    REQUEST_INFO_PACK(pRI, cb, dataProfilePtrs, (num * sizeof(RIL_DataProfileInfo_v1_4 *)));

    CALL_ONREQUEST(RIL_REQUEST_SET_DATA_PROFILE, dataProfilePtrs,
            num * sizeof(RIL_DataProfileInfo_v1_4 *), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setDataProfile_1_4(int32_t serial, const hidl_vec<V1_4::DataProfileInfo>& profiles) {
#if VDBG
    RLOGD("setDataProfile_1_4: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_DATA_PROFILE);
    if (pRI == NULL) {
        return Void();
    }

    size_t num = profiles.size();
    bool success = false;

    RIL_DataProfileInfo_v1_4 *dataProfiles =
        (RIL_DataProfileInfo_v1_4 *)calloc(num, sizeof(RIL_DataProfileInfo_v1_4));
    if (dataProfiles == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    RIL_DataProfileInfo_v1_4 **dataProfilePtrs =
        (RIL_DataProfileInfo_v1_4 **)calloc(num, sizeof(RIL_DataProfileInfo_v1_4 *));
    if (dataProfilePtrs == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        free(dataProfiles);
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    for (size_t i = 0; i < num; i++) {
        dataProfilePtrs[i] = &dataProfiles[i];

        success = copyHidlStringToRil(&dataProfiles[i].apn, profiles[i].apn,
                pRI, true);

        if (success && !copyHidlStringToRil(&dataProfiles[i].user,
                profiles[i].user, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&dataProfiles[i].password,
                profiles[i].password,
                pRI, true)) {
            success = false;
        }

        if (!success) {
            freeSetDataProfileData(num, dataProfiles, dataProfilePtrs, 3,
                &RIL_DataProfileInfo_v1_4::apn, &RIL_DataProfileInfo_v1_4::user,
                &RIL_DataProfileInfo_v1_4::password);
            return Void();
        }

        dataProfiles[i].profileId = (RIL_DataProfile)profiles[i].profileId;
        dataProfiles[i].protocol = (RIL_PdpProtocolType)profiles[i].protocol;
        dataProfiles[i].roamingProtocol = (RIL_PdpProtocolType)profiles[i].roamingProtocol;
        dataProfiles[i].authType = (RIL_ApnAuthType)profiles[i].authType;
        dataProfiles[i].type = (RIL_DataProfileInfoType)profiles[i].type;
        dataProfiles[i].maxConnsTime = profiles[i].maxConnsTime;
        dataProfiles[i].maxConns = profiles[i].maxConns;
        dataProfiles[i].waitTime = profiles[i].waitTime;
        dataProfiles[i].enabled = profiles[i].enabled;
        dataProfiles[i].bearerBitmap = profiles[i].bearerBitmap;
        dataProfiles[i].mtu = profiles[i].mtu;
        dataProfiles[i].preferred = profiles[i].mtu;
        dataProfiles[i].persistent = profiles[i].persistent;
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_DataProfileInfo_v1_4 **dataProfiles = (RIL_DataProfileInfo_v1_4 **)data;
        int num = dataLen / sizeof(RIL_DataProfileInfo_v1_4 *);
        int i = 0;
        for (i = 0; i < num; i++) {
            memsetAndFreeStrings(3, dataProfiles[i]->apn, dataProfiles[i]->user,
                    dataProfiles[i]->password);
        }
        free(dataProfiles[0]);

        memset(dataProfiles, 0, dataLen);
        free(dataProfiles);
    };
    REQUEST_INFO_PACK(pRI, cb, dataProfilePtrs, (num * sizeof(RIL_DataProfileInfo_v1_4 *)));

    CALL_ONREQUEST(RIL_REQUEST_SET_DATA_PROFILE, dataProfilePtrs,
            num * sizeof(RIL_DataProfileInfo_v1_4 *), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::requestShutdown(int32_t serial) {
#if VDBG
    RLOGD("requestShutdown: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_SHUTDOWN);
    return Void();
}

Return<void> RadioImpl::getRadioCapability(int32_t serial) {
#if VDBG
    RLOGD("getRadioCapability: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_RADIO_CAPABILITY);
    return Void();
}

Return<void> RadioImpl::setRadioCapability(int32_t serial, const RadioCapability& rc) {
#if VDBG
    RLOGD("setRadioCapability: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_RADIO_CAPABILITY);
    if (pRI == NULL) {
        return Void();
    }

    RIL_RadioCapability *rilRc = (RIL_RadioCapability *)calloc(1, sizeof(RIL_RadioCapability));
    if (rilRc == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    // TODO : set rilRc.version using HIDL version ?
    rilRc->session = rc.session;
    rilRc->phase = (int) rc.phase;
    rilRc->rat = (int) rc.raf;
    rilRc->status = (int) rc.status;
    strlcpy(rilRc->logicalModemUuid, rc.logicalModemUuid.c_str(), sizeof(rilRc->logicalModemUuid));

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, rilRc, sizeof(RIL_RadioCapability));

    CALL_ONREQUEST(pRI->pCI->requestNumber, rilRc, sizeof(RIL_RadioCapability), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::startLceService(int32_t serial, int32_t reportInterval, bool pullMode) {
#if VDBG
    RLOGD("startLceService: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_START_LCE, 2, reportInterval,
            BOOL_TO_INT(pullMode));
    return Void();
}

Return<void> RadioImpl::stopLceService(int32_t serial) {
#if VDBG
    RLOGD("stopLceService: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_STOP_LCE);
    return Void();
}

Return<void> RadioImpl::pullLceData(int32_t serial) {
#if VDBG
    RLOGD("pullLceData: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_PULL_LCEDATA);
    return Void();
}

Return<void> RadioImpl::getModemActivityInfo(int32_t serial) {
#if VDBG
    RLOGD("getModemActivityInfo: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_ACTIVITY_INFO);
    return Void();
}

Return<void> RadioImpl::setAllowedCarriers(int32_t serial, bool allAllowed,
                                           const CarrierRestrictions& carriers) {
#if VDBG
    RLOGD("setAllowedCarriers: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_CARRIER_RESTRICTIONS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CarrierRestrictions_v1_4 *cr =
            (RIL_CarrierRestrictions_v1_4 *)calloc(1, sizeof(RIL_CarrierRestrictions_v1_4));
    if (cr == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    cr->allowedCarriersPrioritized = false;
    cr->simPolicy = SIM_POLICY_NO_MULTISIM_POLICY;

    RIL_Carrier *allowedCarriers = NULL;
    RIL_Carrier *excludedCarriers = NULL;

    cr->allowedCarriersNumber = carriers.allowedCarriers.size();
    allowedCarriers = (RIL_Carrier *)
            calloc(cr->allowedCarriersNumber, sizeof(RIL_Carrier));
    if (allowedCarriers == NULL) {
        RLOGE("setAllowedCarriers_1_4: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        free(cr);
        return Void();
    }
    cr->allowedCarriers = allowedCarriers;

    cr->excludedCarriersNumber = carriers.excludedCarriers.size();
    excludedCarriers = (RIL_Carrier *)
            calloc(cr->excludedCarriersNumber, sizeof(RIL_Carrier));
    if (excludedCarriers == NULL) {
        RLOGE("setAllowedCarriers_1_4: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
#ifdef MEMSET_FREED
        memset(allowedCarriers, 0, cr.len_allowed_carriers * sizeof(RIL_Carrier));
#endif
        free(allowedCarriers);
        free(cr);
        return Void();
    }
    cr->excludedCarriers = excludedCarriers;

    bool success = false;

    for (uint32_t i = 0; i < cr->allowedCarriersNumber; i++) {
        allowedCarriers[i].match_type =
                (RIL_CarrierMatchType)carriers.allowedCarriers[i].matchType;
        success = copyHidlStringToRil(&allowedCarriers[i].mcc,
                carriers.allowedCarriers[i].mcc, pRI, true);
        if (success && !copyHidlStringToRil(&allowedCarriers[i].mnc,
                carriers.allowedCarriers[i].mnc, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&allowedCarriers[i].match_data,
                carriers.allowedCarriers[i].matchData, pRI, true)) {
            success = false;
        }

        if (!success) {
            for (uint32_t j = 0; j < i; j++) {
                memsetAndFreeStrings(3, allowedCarriers[j].mcc,
                        allowedCarriers[j].mnc, allowedCarriers[j].match_data);
            }
            free(cr->allowedCarriers);
            free(cr->excludedCarriers);
            free(cr);
            return Void();
        }
    }

    success = false;
    for (uint32_t i = 0; i < cr->excludedCarriersNumber; i++) {
        allowedCarriers[i].match_type =
                (RIL_CarrierMatchType)carriers.excludedCarriers[i].matchType;
        success = copyHidlStringToRil(&excludedCarriers[i].mcc,
                carriers.excludedCarriers[i].mcc, pRI, true);
        if (success && !copyHidlStringToRil(&excludedCarriers[i].mnc,
                carriers.excludedCarriers[i].mnc, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&excludedCarriers[i].match_data,
                carriers.excludedCarriers[i].matchData, pRI, true)) {
            success = false;
        }

        if (!success) {
            for (uint32_t j = 0; j < i; j++) {
                memsetAndFreeStrings(3, excludedCarriers[j].mcc,
                        excludedCarriers[j].mnc, excludedCarriers[j].match_data);
            }
            for (uint32_t j = 0; j < cr->allowedCarriersNumber; j++) {
                memsetAndFreeStrings(3, allowedCarriers[j].mcc,
                        allowedCarriers[j].mnc, allowedCarriers[j].match_data);
            }
            free(cr->allowedCarriers);
            free(cr->excludedCarriers);
            free(cr);
            return Void();
        }
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CarrierRestrictions_v1_4 *pCR = (RIL_CarrierRestrictions_v1_4 *)data;

        for (size_t i = 0; i < pCR->allowedCarriersNumber; i++) {
            memsetAndFreeStrings(3, pCR->allowedCarriers[i].mcc,
                    pCR->allowedCarriers[i].mnc, pCR->allowedCarriers[i].match_data);
        }
        for (size_t i = 0; i < pCR->excludedCarriersNumber; i++) {
            memsetAndFreeStrings(3, pCR->excludedCarriers[i].mcc,
                    pCR->excludedCarriers[i].mnc, pCR->excludedCarriers[i].match_data);
        }

        memset(pCR->allowedCarriers, 0,
                pCR->allowedCarriersNumber * sizeof(RIL_Carrier));
        memset(pCR->excludedCarriers, 0,
                pCR->excludedCarriersNumber * sizeof(RIL_Carrier));

        free(pCR->allowedCarriers);
        free(pCR->excludedCarriers);

        memset(pCR, 0, dataLen);
        free(pCR);
    };
    REQUEST_INFO_PACK(pRI, cb, cr, sizeof(RIL_CarrierRestrictions_v1_4));

    CALL_ONREQUEST(pRI->pCI->requestNumber, cr, sizeof(RIL_CarrierRestrictions_v1_4), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setAllowedCarriers_1_4(int32_t serial,
        const V1_4::CarrierRestrictionsWithPriority& carriers,
        V1_4::SimLockMultiSimPolicy multiSimPolicy) {
#if VDBG
    RLOGD("setAllowedCarriers_1_4: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_CARRIER_RESTRICTIONS);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CarrierRestrictions_v1_4 *cr =
            (RIL_CarrierRestrictions_v1_4 *)calloc(1, sizeof(RIL_CarrierRestrictions_v1_4));
    if (cr == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    cr->allowedCarriersPrioritized = carriers.allowedCarriersPrioritized;
    cr->simPolicy = (RIL_SimLockMultiSimPolicy)multiSimPolicy;

    RIL_Carrier *allowedCarriers = NULL;
    RIL_Carrier *excludedCarriers = NULL;

    cr->allowedCarriersNumber = carriers.allowedCarriers.size();
    allowedCarriers = (RIL_Carrier *)
            calloc(cr->allowedCarriersNumber, sizeof(RIL_Carrier));
    if (allowedCarriers == NULL) {
        RLOGE("setAllowedCarriers_1_4: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        free(cr);
        return Void();
    }
    cr->allowedCarriers = allowedCarriers;

    cr->excludedCarriersNumber = carriers.excludedCarriers.size();
    excludedCarriers = (RIL_Carrier *)
            calloc(cr->excludedCarriersNumber, sizeof(RIL_Carrier));
    if (excludedCarriers == NULL) {
        RLOGE("setAllowedCarriers_1_4: Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
#ifdef MEMSET_FREED
        memset(allowedCarriers, 0, cr.len_allowed_carriers * sizeof(RIL_Carrier));
#endif
        free(allowedCarriers);
        free(cr);
        return Void();
    }
    cr->excludedCarriers = excludedCarriers;

    bool success = false;

    for (uint32_t i = 0; i < cr->allowedCarriersNumber; i++) {
        allowedCarriers[i].match_type =
                (RIL_CarrierMatchType)carriers.allowedCarriers[i].matchType;
        success = copyHidlStringToRil(&allowedCarriers[i].mcc,
                carriers.allowedCarriers[i].mcc, pRI, true);
        if (success && !copyHidlStringToRil(&allowedCarriers[i].mnc,
                carriers.allowedCarriers[i].mnc, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&allowedCarriers[i].match_data,
                carriers.allowedCarriers[i].matchData, pRI, true)) {
            success = false;
        }

        if (!success) {
            for (size_t j = 0; j < i; j++) {
                memsetAndFreeStrings(3, allowedCarriers[j].mcc,
                        allowedCarriers[j].mnc, allowedCarriers[j].match_data);
            }
            free(cr->allowedCarriers);
            free(cr->excludedCarriers);
            free(cr);
            return Void();
        }
    }

    success = false;
    for (uint32_t i = 0; i < cr->excludedCarriersNumber; i++) {
        allowedCarriers[i].match_type =
                (RIL_CarrierMatchType)carriers.excludedCarriers[i].matchType;
        success = copyHidlStringToRil(&excludedCarriers[i].mcc,
                carriers.excludedCarriers[i].mcc, pRI, true);
        if (success && !copyHidlStringToRil(&excludedCarriers[i].mnc,
                carriers.excludedCarriers[i].mnc, pRI, true)) {
            success = false;
        }
        if (success && !copyHidlStringToRil(&excludedCarriers[i].match_data,
                carriers.excludedCarriers[i].matchData, pRI, true)) {
            success = false;
        }

        if (!success) {
            for (uint32_t j = 0; j < i; j++) {
                memsetAndFreeStrings(3, excludedCarriers[j].mcc,
                        excludedCarriers[j].mnc, excludedCarriers[j].match_data);
            }
            for (uint32_t j = 0; j < cr->allowedCarriersNumber; j++) {
                memsetAndFreeStrings(3, allowedCarriers[j].mcc,
                        allowedCarriers[j].mnc, allowedCarriers[j].match_data);
            }
            free(cr->allowedCarriers);
            free(cr->excludedCarriers);
            free(cr);
            return Void();
        }
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CarrierRestrictions_v1_4 *pCR = (RIL_CarrierRestrictions_v1_4 *)data;

        for (size_t i = 0; i < pCR->allowedCarriersNumber; i++) {
            memsetAndFreeStrings(3, pCR->allowedCarriers[i].mcc,
                    pCR->allowedCarriers[i].mnc, pCR->allowedCarriers[i].match_data);
        }
        for (size_t i = 0; i < pCR->excludedCarriersNumber; i++) {
            memsetAndFreeStrings(3, pCR->excludedCarriers[i].mcc,
                    pCR->excludedCarriers[i].mnc, pCR->excludedCarriers[i].match_data);
        }

        memset(pCR->allowedCarriers, 0,
                pCR->allowedCarriersNumber * sizeof(RIL_Carrier));
        memset(pCR->excludedCarriers, 0,
                pCR->excludedCarriersNumber * sizeof(RIL_Carrier));

        free(pCR->allowedCarriers);
        free(pCR->excludedCarriers);

        memset(pCR, 0, dataLen);
        free(pCR);
    };
    REQUEST_INFO_PACK(pRI, cb, cr, sizeof(RIL_CarrierRestrictions_v1_4));

    CALL_ONREQUEST(pRI->pCI->requestNumber, cr, sizeof(RIL_CarrierRestrictions_v1_4), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::getAllowedCarriers(int32_t serial) {
#if VDBG
    RLOGD("getAllowedCarriers: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CARRIER_RESTRICTIONS);
    return Void();
}

Return<void> RadioImpl::getAllowedCarriers_1_4(int32_t serial) {
#if VDBG
    RLOGD("getAllowedCarriers_1_4: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_CARRIER_RESTRICTIONS);
    return Void();
}

Return<void> RadioImpl::sendDeviceState(int32_t serial, DeviceStateType deviceStateType,
                                        bool state) {
#if VDBG
    RLOGD("sendDeviceState: serial %d", serial);
#endif
    if (s_vendorFunctions->version < 15) {
        if (deviceStateType ==  DeviceStateType::LOW_DATA_EXPECTED) {
            RLOGD("sendDeviceState: calling screen state %d", BOOL_TO_INT(!state));
            dispatchInts(serial, mSlotId, RIL_REQUEST_SCREEN_STATE, 1, BOOL_TO_INT(!state));
        } else {
            RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                    RIL_REQUEST_SEND_DEVICE_STATE);
            sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        }
        return Void();
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_SEND_DEVICE_STATE, 2, (int) deviceStateType,
            BOOL_TO_INT(state));
    return Void();
}

Return<void> RadioImpl::setIndicationFilter(int32_t serial, int32_t indicationFilter) {
#if VDBG
    RLOGD("setIndicationFilter: serial %d", serial);
#endif
    if (s_vendorFunctions->version < 15) {
        RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
                RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER);
        sendErrorResponse(pRI, RIL_E_REQUEST_NOT_SUPPORTED);
        return Void();
    }
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER, 1, indicationFilter);
    return Void();
}

Return<void> RadioImpl::setIndicationFilter_1_2(int32_t serial,
        ::android::hardware::hidl_bitfield<V1_2::IndicationFilter> indicationFilter) {
#if VDBG
    RLOGD("setIndicationFilter_1_2: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER, 1, indicationFilter);
    return Void();
}

Return<void> RadioImpl::setSimCardPower(int32_t serial, bool powerUp) {
#if VDBG
    RLOGD("setSimCardPower: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SIM_CARD_POWER, 1, BOOL_TO_INT(powerUp));
    return Void();
}

Return<void> RadioImpl::setSimCardPower_1_1(int32_t serial, V1_1::CardPowerState powerUp) {
#if VDBG
    RLOGD("setSimCardPower_1_1: serial %d state %d", serial, powerUp);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_SIM_CARD_POWER, 1, powerUp);
    return Void();
}

Return<void> RadioImpl::setCarrierInfoForImsiEncryption(int32_t serial,
        const V1_1::ImsiEncryptionInfo& data) {
#if VDBG
    RLOGD("setCarrierInfoForImsiEncryption: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(
            serial, mSlotId, RIL_REQUEST_SET_CARRIER_INFO_IMSI_ENCRYPTION);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CarrierInfoForImsiEncryption *imsiEncryption =
            (RIL_CarrierInfoForImsiEncryption *)calloc(1, sizeof(RIL_CarrierInfoForImsiEncryption));
    if (imsiEncryption == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    if (!copyHidlStringToRil(&imsiEncryption->mnc, data.mnc, pRI)) {
        free(imsiEncryption);
        return Void();
    }
    if (!copyHidlStringToRil(&imsiEncryption->mcc, data.mcc, pRI)) {
        memsetAndFreeStrings(1, imsiEncryption->mnc);
        free(imsiEncryption);
        return Void();
    }
    if (!copyHidlStringToRil(&imsiEncryption->keyIdentifier, data.keyIdentifier, pRI)) {
        memsetAndFreeStrings(2, imsiEncryption->mnc, imsiEncryption->mcc);
        free(imsiEncryption);
        return Void();
    }
    imsiEncryption->carrierKeyLength = data.carrierKey.size();
    imsiEncryption->carrierKey = new uint8_t[imsiEncryption->carrierKeyLength];
    memcpy(imsiEncryption->carrierKey, data.carrierKey.data(),
            imsiEncryption->carrierKeyLength);
    imsiEncryption->expirationTime = data.expirationTime;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CarrierInfoForImsiEncryption *info =
                (RIL_CarrierInfoForImsiEncryption *)data;

        memsetAndFreeStrings(3, info->mcc, info->mnc, info->keyIdentifier);
        free(info->carrierKey);

        memset(info, 0, dataLen);
        free(info);
    };
    REQUEST_INFO_PACK(pRI, cb, imsiEncryption, sizeof(RIL_CarrierInfoForImsiEncryption));

    CALL_ONREQUEST(pRI->pCI->requestNumber, imsiEncryption,
            sizeof(RIL_CarrierInfoForImsiEncryption), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::startKeepalive(int32_t serial, const V1_1::KeepaliveRequest& keepalive) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_START_KEEPALIVE);
    if (pRI == NULL) {
        return Void();
    }

    RIL_KeepaliveRequest *kaReq =
            (RIL_KeepaliveRequest *)calloc(1, sizeof(RIL_KeepaliveRequest));
    if (kaReq == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    kaReq->type = static_cast<RIL_KeepaliveType>(keepalive.type);
    switch(kaReq->type) {
        case NATT_IPV4:
            if (keepalive.sourceAddress.size() != 4 ||
                    keepalive.destinationAddress.size() != 4) {
                RLOGE("Invalid address for keepalive!");
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                free(kaReq);
                return Void();
            }
            break;
        case NATT_IPV6:
            if (keepalive.sourceAddress.size() != 16 ||
                    keepalive.destinationAddress.size() != 16) {
                RLOGE("Invalid address for keepalive!");
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                free(kaReq);
                return Void();
            }
            break;
        default:
            RLOGE("Unknown packet keepalive type!");
            sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
            free(kaReq);
            return Void();
    }

    ::memcpy(kaReq->sourceAddress, keepalive.sourceAddress.data(), keepalive.sourceAddress.size());
    kaReq->sourcePort = keepalive.sourcePort;

    ::memcpy(kaReq->destinationAddress,
            keepalive.destinationAddress.data(), keepalive.destinationAddress.size());
    kaReq->destinationPort = keepalive.destinationPort;

    kaReq->maxKeepaliveIntervalMillis = keepalive.maxKeepaliveIntervalMillis;
    kaReq->cid = keepalive.cid; // This is the context ID of the data call

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, kaReq, sizeof(RIL_KeepaliveRequest));

    CALL_ONREQUEST(pRI->pCI->requestNumber, kaReq, sizeof(RIL_KeepaliveRequest), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::stopKeepalive(int32_t serial, int32_t sessionHandle) {
#if VDBG
    RLOGD("stopKeepalive: %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_STOP_KEEPALIVE, 1, sessionHandle);
    return Void();
}

Return<void> RadioImpl::setSignalStrengthReportingCriteria(int32_t serial,
        int32_t hysteresisMs, int32_t hysteresisDb,
        const hidl_vec<int32_t>& thresholdsDbm,
        V1_2::AccessNetwork accessNetwork) {
#if VDBG
    RLOGD("setSignalStrengthReportingCriteria: %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_SIGNAL_STRENGTH_REPORTING_CRITERIA);
    if (pRI == NULL) {
        return Void();
    }

    if (hysteresisDb >= 10) {  // TODO: for vts. hysteresisDb range not checked
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_SignalStrengthReportingCriteria *criteria = (RIL_SignalStrengthReportingCriteria *)
            calloc(1, sizeof(RIL_SignalStrengthReportingCriteria));
    if (criteria == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    criteria->hysteresisMs = hysteresisMs;
    criteria->hysteresisDb = hysteresisDb;
    criteria->thresholdsDbmNumber = thresholdsDbm.size();
    criteria->thresholdsDbm = new int32_t[criteria->thresholdsDbmNumber];
    memcpy(criteria->thresholdsDbm, thresholdsDbm.data(),
            criteria->thresholdsDbmNumber * sizeof(int32_t));
    criteria->accessNetwork = (RIL_RadioAccessNetworks_v1_2)accessNetwork;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_SignalStrengthReportingCriteria *pCriteria =
                (RIL_SignalStrengthReportingCriteria *)data;

        free(pCriteria->thresholdsDbm);
        memset(pCriteria, 0, dataLen);
        free(pCriteria);
    };
    REQUEST_INFO_PACK(pRI, cb, criteria, sizeof(RIL_SignalStrengthReportingCriteria));

    CALL_ONREQUEST(pRI->pCI->requestNumber, criteria,
            sizeof(RIL_SignalStrengthReportingCriteria), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setLinkCapacityReportingCriteria(int32_t serial,
        int32_t hysteresisMs, int32_t hysteresisDlKbps, int32_t hysteresisUlKbps,
        const hidl_vec<int32_t>& thresholdsDownlinkKbps,
        const hidl_vec<int32_t>& thresholdsUplinkKbps,
        V1_2::AccessNetwork accessNetwork) {
#if VDBG
    RLOGD("setLinkCapacityReportingCriteria: %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_REQUEST_SET_LINK_CAPACITY_REPORTING_CRITERIA);
    if (pRI == NULL) {
        return Void();
    }
    // TODO: for vts. hysteresisDlKbps and hysteresisUlKbps range not confirmed
    if (hysteresisDlKbps >= 5000 || hysteresisUlKbps >= 1000) {
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_LinkCapacityReportingCriteria *criteria = (RIL_LinkCapacityReportingCriteria *)
            calloc(1, sizeof(RIL_LinkCapacityReportingCriteria));
    if (criteria == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    criteria->hysteresisMs = hysteresisMs;
    criteria->hysteresisDlKbps = hysteresisDlKbps;
    criteria->hysteresisUlKbps = hysteresisUlKbps;
    criteria->thresholdsDownlinkKbpsLength = thresholdsDownlinkKbps.size();
    criteria->thresholdsUplinkKbpsLength = thresholdsUplinkKbps.size();
    criteria->thresholdsDownlinkKbps = new int32_t[criteria->thresholdsDownlinkKbpsLength];
    criteria->thresholdsUplinkKbps = new int32_t[criteria->thresholdsUplinkKbpsLength];
    memcpy(criteria->thresholdsDownlinkKbps, thresholdsDownlinkKbps.data(),
            criteria->thresholdsDownlinkKbpsLength * sizeof(int32_t));
    memcpy(criteria->thresholdsUplinkKbps, thresholdsUplinkKbps.data(),
            criteria->thresholdsUplinkKbpsLength * sizeof(int32_t));
    criteria->accessNetwork = (RIL_RadioAccessNetworks_v1_2)accessNetwork;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_LinkCapacityReportingCriteria *pCriteria =
                (RIL_LinkCapacityReportingCriteria *)data;

        free(pCriteria->thresholdsDownlinkKbps);
        free(pCriteria->thresholdsUplinkKbps);
        memset(pCriteria, 0, dataLen);
        free(pCriteria);
    };
    REQUEST_INFO_PACK(pRI, cb, criteria, sizeof(RIL_LinkCapacityReportingCriteria));

    CALL_ONREQUEST(pRI->pCI->requestNumber, criteria,
            sizeof(RIL_LinkCapacityReportingCriteria), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::setSystemSelectionChannels(int32_t serial, bool specifyChannels,
        const hidl_vec<V1_1::RadioAccessSpecifier>& specifiers) {
#if VDBG
    RLOGD("setSystemSelectionChannels: %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_SET_SYSTEM_SELECTION_CHANNELS);
    if (pRI == NULL) {
        return Void();
    }

    if (specifiers.size() > RIL_RADIO_ACCESS_SPECIFIER_MAX_SIZE) {
        sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
        return Void();
    }

    RIL_SystemSelectionChannels *sysSelectionChannels =
            (RIL_SystemSelectionChannels *)calloc(1, sizeof(RIL_SystemSelectionChannels));
    if (sysSelectionChannels == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }
    sysSelectionChannels->specifyChannels = specifyChannels;
    sysSelectionChannels->specifiers_length = specifiers.size();
    for (size_t i = 0; i < specifiers.size(); ++i) {
        const V1_1::RadioAccessSpecifier& ras_from = specifiers[i];
        RIL_RadioAccessSpecifier &ras_to = sysSelectionChannels->specifiers[i];

        ras_to.radio_access_network = (RIL_RadioAccessNetworks)ras_from.radioAccessNetwork;
        ras_to.channels_length = ras_from.channels.size();

        std::copy(ras_from.channels.begin(), ras_from.channels.end(), ras_to.channels);
        const std::vector<uint32_t> * bands = nullptr;
        switch (specifiers[i].radioAccessNetwork) {
            case V1_1::RadioAccessNetworks::GERAN:
                ras_to.bands_length = ras_from.geranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.geranBands;
                break;
            case V1_1::RadioAccessNetworks::UTRAN:
                ras_to.bands_length = ras_from.utranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.utranBands;
                break;
            case V1_1::RadioAccessNetworks::EUTRAN:
                ras_to.bands_length = ras_from.eutranBands.size();
                bands = (std::vector<uint32_t> *) &ras_from.eutranBands;
                break;
            default: {
                sendErrorResponse(pRI, RIL_E_INVALID_ARGUMENTS);
                free(sysSelectionChannels);
                return Void();
            }
        }
        // safe to copy to geran_bands because it's a union member
        for (size_t idx = 0; idx < ras_to.bands_length; ++idx) {
            ras_to.bands.geran_bands[idx] = (RIL_GeranBands)(*bands)[idx];
        }
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, sysSelectionChannels, sizeof(RIL_SystemSelectionChannels));

    CALL_ONREQUEST(RIL_REQUEST_SET_SYSTEM_SELECTION_CHANNELS, sysSelectionChannels,
            sizeof(RIL_SystemSelectionChannels), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::enableModem(int32_t serial, bool on) {
#if VDBG
    RLOGD("enableModem: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_ENABLE_MODEM, 1, BOOL_TO_INT(on));
    return Void();
}

Return<void> RadioImpl::getModemStackStatus(int32_t serial) {
#if VDBG
    RLOGD("getModemStackStatus: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_MODEM_STATUS);
    return Void();
}

Return<void> RadioImpl::emergencyDial(int32_t serial, const V1_0::Dial& dialInfo,
            hidl_bitfield<V1_4::EmergencyServiceCategory> categories,
            const hidl_vec<hidl_string>& urns, V1_4::EmergencyCallRouting routing,
            bool hasKnownUserIntentEmergency, bool isTesting) {
#if VDBG
    RLOGD("emergencyDial: serial %d", serial);
#endif

    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_REQUEST_EMERGENCY_DIAL);
    if (pRI == NULL) {
        return Void();
    }

    if (isTesting == true) {
        sendErrorResponse(pRI, RIL_E_SUCCESS);
        RLOGD("Just for testing purpose, return");
        return Void();
    }

    RIL_EmergencyNumber *dial = (RIL_EmergencyNumber *)calloc(1, sizeof(RIL_EmergencyNumber));

    if (!copyHidlStringToRil(&dial->number, dialInfo.address, pRI)) {
        free(dial);
        return Void();
    }
    dial->clir = (int)dialInfo.clir;
    dial->categories = (RIL_EmergencyServiceCategory)categories;
    dial->urnsNumber = urns.size();
    if (dial->urnsNumber != 0) {
        char **pStrings = (char **)calloc(dial->urnsNumber, sizeof(char *));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            free(dial->number);
            free(dial);
            return Void();
        }
        for (uint32_t i = 0; i < dial->urnsNumber; i++) {
            if (!copyHidlStringToRil(&pStrings[i], hidl_string(urns[i]), pRI)) {
                for (uint32_t j = 0; j < i; j++) {
                    memsetAndFreeStrings(1, pStrings[j]);
                }
                free(pStrings);
                free(dial->number);
                free(dial);
                return Void();
            }
        }
        dial->urns = pStrings;
    }
    dial->routing = (RIL_EmergencyCallRouting)routing;
    dial->hasKnownUserIntentEmergency = hasKnownUserIntentEmergency;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_EmergencyNumber *pDial = (RIL_EmergencyNumber *)data;

        memsetAndFreeStrings(3, pDial->number, pDial->mcc, pDial->mnc);

        if (pDial->urns != NULL) {
            for (size_t i = 0; i < pDial->urnsNumber; i++) {
                memsetAndFreeStrings(1, pDial->urns[i]);
            }
            free(pDial->urns);
        }
        memset(pDial, 0, dataLen);
        free(pDial);
    };
    REQUEST_INFO_PACK(pRI, cb, dial, sizeof(RIL_EmergencyNumber));

    CALL_ONREQUEST(RIL_REQUEST_EMERGENCY_DIAL, dial, sizeof(RIL_EmergencyNumber), pRI, pRI->socket_id);

    return Void();
}

Return<void> RadioImpl::getPreferredNetworkTypeBitmap(int32_t serial) {
#if VDBG
    RLOGD("getPreferredNetworkTypeBitmap: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE_BITMAP);
    return Void();
}

Return<void> RadioImpl::setPreferredNetworkTypeBitmap(int32_t serial,
        hidl_bitfield<V1_4::RadioAccessFamily> networkTypeBitmap) {
#if VDBG
    RLOGD("setPreferredNetworkTypeBitmap: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE_BITMAP,
            1, (int)networkTypeBitmap);
    return Void();
}

Return<void> RadioImpl::responseAcknowledgement() {
    android::releaseWakeLock();
    return Void();
}

Return<void> OemHookImpl::setResponseFunctions(
        const ::android::sp<IOemHookResponse>& oemHookResponseParam,
        const ::android::sp<IOemHookIndication>& oemHookIndicationParam) {
#if VDBG
    RLOGD("OemHookImpl::setResponseFunctions");
#endif

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mOemHookResponse = oemHookResponseParam;
    mOemHookIndication = oemHookIndicationParam;
    mCounterOemHook[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    return Void();
}

Return<void> OemHookImpl::sendRequestRaw(int32_t serial, const hidl_vec<uint8_t>& data) {
#if VDBG
    RLOGD("OemHookImpl::sendRequestRaw: serial %d", serial);
#endif
    dispatchRaw(serial, mSlotId, RIL_REQUEST_OEM_HOOK_RAW, data);
    return Void();
}

Return<void> OemHookImpl::sendRequestStrings(int32_t serial,
        const hidl_vec<hidl_string>& data) {
#if VDBG
    RLOGD("OemHookImpl::sendRequestStrings: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_REQUEST_OEM_HOOK_STRINGS, data);
    return Void();
}
/***************************************************************************************************
 * RESPONSE FUNCTIONS
 * Functions above are used for requests going from framework to vendor code. The ones below are
 * responses for those requests coming back from the vendor code.
 **************************************************************************************************/

void radio::acknowledgeRequest(int slotId, int serial) {
    if (radioService[slotId]->mRadioResponse != NULL) {
        Return<void> retStatus = radioService[slotId]->mRadioResponse->acknowledgeRequest(serial);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeRequest: radioService[%d]->mRadioResponse == NULL", slotId);
    }
}

void populateResponseInfo(RadioResponseInfo& responseInfo, int serial, int responseType,
                         RIL_Errno e) {
    responseInfo.serial = serial;
    switch (responseType) {
        case RESPONSE_SOLICITED:
            responseInfo.type = RadioResponseType::SOLICITED;
            break;
        case RESPONSE_SOLICITED_ACK_EXP:
            responseInfo.type = RadioResponseType::SOLICITED_ACK_EXP;
            break;
    }
    responseInfo.error = (RadioError) e;
}

int responseIntOrEmpty(RadioResponseInfo& responseInfo, int serial, int responseType, RIL_Errno e,
               void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL && responseLen == 0) {
        // Earlier RILs did not send a response for some cases although the interface
        // expected an integer as response. Do not return error if response is empty. Instead
        // Return -1 in those cases to maintain backward compatibility.
    } else if (response == NULL || responseLen != sizeof(int)) {
        RLOGE("responseIntOrEmpty: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

int responseInt(RadioResponseInfo& responseInfo, int serial, int responseType, RIL_Errno e,
               void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL || responseLen != sizeof(int)) {
        RLOGE("responseInt: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

void fillCardStatusFromRILCardStatus_1_4(V1_0::CardStatus &cardStatus,
        RIL_CardStatus_v1_4 *response) {
    RIL_CardStatus_v1_4 *p_cur = (RIL_CardStatus_v1_4 *)response;

    cardStatus.cardState = (CardState) p_cur->base.base.card_state;
    cardStatus.universalPinState = (PinState) p_cur->base.base.universal_pin_state;
    cardStatus.gsmUmtsSubscriptionAppIndex = p_cur->base.base.gsm_umts_subscription_app_index;
    cardStatus.cdmaSubscriptionAppIndex = p_cur->base.base.cdma_subscription_app_index;
    cardStatus.imsSubscriptionAppIndex = p_cur->base.base.ims_subscription_app_index;

    RIL_AppStatus *rilAppStatus = p_cur->base.base.applications;
    cardStatus.applications.resize(p_cur->base.base.num_applications);
    AppStatus *appStatus = cardStatus.applications.data();

    for (int i = 0; i < p_cur->base.base.num_applications; i++) {
        appStatus[i].appType = (AppType) rilAppStatus[i].app_type;
        appStatus[i].appState = (AppState) rilAppStatus[i].app_state;
        appStatus[i].persoSubstate = (PersoSubstate) rilAppStatus[i].perso_substate;
        appStatus[i].aidPtr = convertCharPtrToHidlString(rilAppStatus[i].aid_ptr);
        appStatus[i].appLabelPtr = convertCharPtrToHidlString(
                rilAppStatus[i].app_label_ptr);
        appStatus[i].pin1Replaced = rilAppStatus[i].pin1_replaced;
        appStatus[i].pin1 = (PinState) rilAppStatus[i].pin1;
        appStatus[i].pin2 = (PinState) rilAppStatus[i].pin2;
    }
}

int radio::getIccCardStatusResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_2 != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        RIL_CardStatus_v1_4 *p_cur = (RIL_CardStatus_v1_4 *)response;
#if VDBG
        RLOGD("getIccCardStatusResponse");
#endif
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            V1_4::CardStatus cardStatus = {};
            if (response == NULL || responseLen != sizeof(RIL_CardStatus_v1_4)
                    || p_cur->base.base.gsm_umts_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.cdma_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.ims_subscription_app_index >= p_cur->base.base.num_applications) {
                RLOGE("getIccCardStatusResponse_1_4: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                fillCardStatusFromRILCardStatus_1_4(cardStatus.base.base, p_cur);

                cardStatus.base.physicalSlotId = p_cur->base.physicalSlotId;
                cardStatus.base.atr = convertCharPtrToHidlString(p_cur->base.atr);
                cardStatus.base.iccid = convertCharPtrToHidlString(p_cur->base.iccid);
                cardStatus.eid = convertCharPtrToHidlString(p_cur->eid);
            }

            retStatus = radioService[slotId]->mRadioResponseV1_4->
                    getIccCardStatusResponse_1_4(responseInfo, cardStatus);
        }  else if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
            V1_2::CardStatus cardStatus = {};
            if (response == NULL || responseLen != sizeof(RIL_CardStatus_v1_4)
                    || p_cur->base.base.gsm_umts_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.cdma_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.ims_subscription_app_index >= p_cur->base.base.num_applications) {
                RLOGE("getIccCardStatusResponse_1_2: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                fillCardStatusFromRILCardStatus_1_4(cardStatus.base, p_cur);

                cardStatus.physicalSlotId = p_cur->base.physicalSlotId;
                cardStatus.atr = convertCharPtrToHidlString(p_cur->base.atr);
                cardStatus.iccid = convertCharPtrToHidlString(p_cur->base.iccid);
            }

            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getIccCardStatusResponse_1_2(responseInfo, cardStatus);
        } else {
            CardStatus cardStatus = {CardState::ABSENT, PinState::UNKNOWN, -1, -1, -1, {}};
            if (response == NULL || responseLen != sizeof(RIL_CardStatus_v1_4)
                    || p_cur->base.base.gsm_umts_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.cdma_subscription_app_index >= p_cur->base.base.num_applications
                    || p_cur->base.base.ims_subscription_app_index >= p_cur->base.base.num_applications) {
                RLOGE("getIccCardStatusResponse: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
             } else {
                 fillCardStatusFromRILCardStatus_1_4(cardStatus, p_cur);
             }

            retStatus = radioService[slotId]->mRadioResponse->
                    getIccCardStatusResponse(responseInfo, cardStatus);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getIccCardStatusResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::supplyIccPinForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPinForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPinForAppResponse(responseInfo, ret);
        RLOGE("supplyIccPinForAppResponse: amit ret %d", ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPinForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPukForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPukForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->supplyIccPukForAppResponse(
                responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPukForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPin2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPin2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPin2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPin2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyIccPuk2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyIccPuk2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyIccPuk2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyIccPuk2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::changeIccPinForAppResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("changeIccPinForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                changeIccPinForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("changeIccPinForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::changeIccPin2ForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("changeIccPin2ForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                changeIccPin2ForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("changeIccPin2ForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::supplyNetworkDepersonalizationResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("supplyNetworkDepersonalizationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                supplyNetworkDepersonalizationResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("supplyNetworkDepersonalizationResponse: radioService[%d]->mRadioResponse == "
                "NULL", slotId);
    }

    return 0;
}

void stripNumberFromSipAddress(const char *sipAddress, char *number, int len) {
    if (sipAddress == NULL || strlen(sipAddress) == 0
            || number == NULL || len <= 0) {
        return;
    }

    memset(number, 0, len * sizeof(char));

    char delim[] = ":;@";
    char *strDupSipAddr = strdup(sipAddress);
    char *s = strDupSipAddr;
    char *token = strsep(&s, delim);
    if (token != NULL) {
        if (strlen(token) == strlen(sipAddress)) {
            strncpy(number, sipAddress, len);
            goto EXIT;
        }
        token = strsep(&s, delim);
        if (token == NULL) {
            strncpy(number, sipAddress, len);
            goto EXIT;
        } else {
            strncpy(number, token, len);
            goto EXIT;
        }
    }
    strncpy(number, sipAddress, len);

EXIT:
    if (s != NULL) {
        free(strDupSipAddr);
        strDupSipAddr = NULL;
    }
}

int radio::getCurrentCallsResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCurrentCallsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
            radioService[slotId]->mRadioResponseV1_2 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        char *numberTmp = NULL;
        Return<void> retStatus;

        if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
            hidl_vec<V1_2::Call> calls;
            if ((response == NULL && responseLen != 0)
                    || (responseLen % sizeof(RIL_Call_v1_2 *)) != 0) {
                RLOGE("getCurrentCallsResponse: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                int num = responseLen / sizeof(RIL_Call_v1_2 *);
                calls.resize(num);

                for (int i = 0 ; i < num ; i++) {
                    RIL_Call_v1_2 *p_cur = ((RIL_Call_v1_2 **)response)[i];
                    /* each call info */
                    calls[i].base.state = (CallState) p_cur->state;
                    calls[i].base.index = p_cur->index;
                    calls[i].base.toa = p_cur->toa;
                    calls[i].base.isMpty = p_cur->isMpty;
                    calls[i].base.isMT = p_cur->isMT;
                    calls[i].base.als = p_cur->als;
                    calls[i].base.isVoice = p_cur->isVoice;
                    calls[i].base.isVoicePrivacy = p_cur->isVoicePrivacy;
                    if (p_cur->number != NULL) {
                        numberTmp = strdup(p_cur->number);
                        stripNumberFromSipAddress(p_cur->number, numberTmp,
                                strlen(numberTmp) * sizeof(char));
                        calls[i].base.number = convertCharPtrToHidlString(numberTmp);
                    } else {
                        calls[i].base.number = convertCharPtrToHidlString(p_cur->number);
                    }
                    calls[i].base.numberPresentation = (CallPresentation) p_cur->numberPresentation;
                    calls[i].base.name = convertCharPtrToHidlString(p_cur->name);
                    calls[i].base.namePresentation = (CallPresentation) p_cur->namePresentation;
                    if (p_cur->uusInfo != NULL && p_cur->uusInfo->uusData != NULL) {
                        RIL_UUS_Info *uusInfo = p_cur->uusInfo;
                        calls[i].base.uusInfo.resize(1);
                        calls[i].base.uusInfo[0].uusType = (UusType) uusInfo->uusType;
                        calls[i].base.uusInfo[0].uusDcs = (UusDcs) uusInfo->uusDcs;
                        // convert uusInfo->uusData to a null-terminated string
                        char *nullTermStr = strndup(uusInfo->uusData, uusInfo->uusLength);
                        calls[i].base.uusInfo[0].uusData = nullTermStr;
                        free(nullTermStr);
                    }
                    calls[i].audioQuality = (V1_2::AudioQuality)p_cur->audioQuality;
                }
            }

            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getCurrentCallsResponse_1_2(responseInfo, calls);
        } else {
            hidl_vec<Call> calls;
            if ((response == NULL && responseLen != 0)
                    || (responseLen % sizeof(RIL_Call *)) != 0) {
                RLOGE("getCurrentCallsResponse: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                int num = responseLen / sizeof(RIL_Call *);
                calls.resize(num);

                for (int i = 0 ; i < num ; i++) {
                    RIL_Call *p_cur = ((RIL_Call **) response)[i];
                    /* each call info */
                    calls[i].state = (CallState) p_cur->state;
                    calls[i].index = p_cur->index;
                    calls[i].toa = p_cur->toa;
                    calls[i].isMpty = p_cur->isMpty;
                    calls[i].isMT = p_cur->isMT;
                    calls[i].als = p_cur->als;
                    calls[i].isVoice = p_cur->isVoice;
                    calls[i].isVoicePrivacy = p_cur->isVoicePrivacy;
                    if (p_cur->number != NULL) {
                        numberTmp = strdup(p_cur->number);
                        stripNumberFromSipAddress(p_cur->number, numberTmp,
                                strlen(numberTmp) * sizeof(char));
                        calls[i].number = convertCharPtrToHidlString(numberTmp);
                    } else {
                        calls[i].number = convertCharPtrToHidlString(p_cur->number);
                    }
                    calls[i].numberPresentation = (CallPresentation) p_cur->numberPresentation;
                    calls[i].name = convertCharPtrToHidlString(p_cur->name);
                    calls[i].namePresentation = (CallPresentation) p_cur->namePresentation;
                    if (p_cur->uusInfo != NULL && p_cur->uusInfo->uusData != NULL) {
                        RIL_UUS_Info *uusInfo = p_cur->uusInfo;
                        calls[i].uusInfo.resize(1);
                        calls[i].uusInfo[0].uusType = (UusType) uusInfo->uusType;
                        calls[i].uusInfo[0].uusDcs = (UusDcs) uusInfo->uusDcs;
                        // convert uusInfo->uusData to a null-terminated string
                        char *nullTermStr = strndup(uusInfo->uusData, uusInfo->uusLength);
                        calls[i].uusInfo[0].uusData = nullTermStr;
                        free(nullTermStr);
                    }
                }
            }

            retStatus = radioService[slotId]->mRadioResponse->
                    getCurrentCallsResponse(responseInfo, calls);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
        free(numberTmp);
    } else {
        RLOGE("getCurrentCallsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::dialResponse(int slotId,
                       int responseType, int serial, RIL_Errno e, void *response,
                       size_t responseLen) {
#if VDBG
    RLOGD("dialResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->dialResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("dialResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getIMSIForAppResponse(int slotId,
                                int responseType, int serial, RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("getIMSIForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getIMSIForAppResponse(
                responseInfo, convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getIMSIForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupConnectionResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("hangupConnectionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->hangupConnectionResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupConnectionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupWaitingOrBackgroundResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("hangupWaitingOrBackgroundResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->hangupWaitingOrBackgroundResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupWaitingOrBackgroundResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::hangupForegroundResumeBackgroundResponse(int slotId, int responseType, int serial,
                                                    RIL_Errno e, void *response,
                                                    size_t responseLen) {
#if VDBG
    RLOGD("hangupWaitingOrBackgroundResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->hangupWaitingOrBackgroundResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hangupWaitingOrBackgroundResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::switchWaitingOrHoldingAndActiveResponse(int slotId, int responseType, int serial,
                                                   RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("switchWaitingOrHoldingAndActiveResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->switchWaitingOrHoldingAndActiveResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("switchWaitingOrHoldingAndActiveResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::conferenceResponse(int slotId, int responseType,
                             int serial, RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("conferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->conferenceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("conferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::rejectCallResponse(int slotId, int responseType,
                             int serial, RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("rejectCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->rejectCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("rejectCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getLastCallFailCauseResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("getLastCallFailCauseResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        LastCallFailCauseInfo info = {};
        info.vendorCause = hidl_string();
        if (response == NULL) {
            RLOGE("getCurrentCallsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else if (responseLen == sizeof(int)) {
            int *pInt = (int *) response;
            info.causeCode = (LastCallFailCause) pInt[0];
        } else if (responseLen == sizeof(RIL_LastCallFailCauseInfo))  {
            RIL_LastCallFailCauseInfo *pFailCauseInfo = (RIL_LastCallFailCauseInfo *) response;
            info.causeCode = (LastCallFailCause) pFailCauseInfo->cause_code;
            info.vendorCause = convertCharPtrToHidlString(pFailCauseInfo->vendor_cause);
        } else {
            RLOGE("getCurrentCallsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getLastCallFailCauseResponse(
                responseInfo, info);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getLastCallFailCauseResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSignalStrengthResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getSignalStrengthResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_2 != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            V1_4::SignalStrength signalStrength = {};
            if (response == NULL || responseLen != sizeof(RIL_SignalStrength_v1_4)) {
                RLOGE("getSignalStrengthResponse_1_4: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilSignalStrengthToHal_1_4(response, responseLen, signalStrength);
            }

            retStatus = radioService[slotId]->mRadioResponseV1_4->
                    getSignalStrengthResponse_1_4(responseInfo, signalStrength);
        } else if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
            V1_2::SignalStrength signalStrength = {};
            if (response == NULL || responseLen != sizeof(RIL_SignalStrength_v1_4)) {
                RLOGE("getSignalStrengthResponse_1_2: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilSignalStrengthToHal_1_2(response, responseLen, signalStrength);
            }

            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getSignalStrengthResponse_1_2(responseInfo, signalStrength);
        } else {
            SignalStrength signalStrength = {};
            if (response == NULL || responseLen != sizeof(RIL_SignalStrength_v1_4)) {
                RLOGE("getSignalStrengthResponse: Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilSignalStrengthToHal(response, responseLen, signalStrength);
            }

            retStatus = radioService[slotId]->mRadioResponse->
                    getSignalStrengthResponse(responseInfo, signalStrength);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getSignalStrengthResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

RIL_CellInfoType getCellInfoTypeRadioTechnology(char *rat) {
    if (rat == NULL) {
        return RIL_CELL_INFO_TYPE_NONE;
    }

    int radioTech = atoi(rat);

    switch(radioTech) {

        case RADIO_TECH_GPRS:
        case RADIO_TECH_EDGE:
        case RADIO_TECH_GSM: {
            return RIL_CELL_INFO_TYPE_GSM;
        }

        case RADIO_TECH_UMTS:
        case RADIO_TECH_HSDPA:
        case RADIO_TECH_HSUPA:
        case RADIO_TECH_HSPA:
        case RADIO_TECH_HSPAP: {
            return RIL_CELL_INFO_TYPE_WCDMA;
        }

        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_1xRTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD: {
            return RIL_CELL_INFO_TYPE_CDMA;
        }

        case RADIO_TECH_LTE:
        case RADIO_TECH_LTE_CA: {
            return RIL_CELL_INFO_TYPE_LTE;
        }

        case RADIO_TECH_TD_SCDMA: {
            return RIL_CELL_INFO_TYPE_TD_SCDMA;
        }

        default: {
            break;
        }
    }

    return RIL_CELL_INFO_TYPE_NONE;

}

void fillCellIdentityResponse(CellIdentity &cellIdentity, RIL_CellIdentity_v16 &rilCellIdentity) {

    cellIdentity.cellIdentityGsm.resize(0);
    cellIdentity.cellIdentityWcdma.resize(0);
    cellIdentity.cellIdentityCdma.resize(0);
    cellIdentity.cellIdentityTdscdma.resize(0);
    cellIdentity.cellIdentityLte.resize(0);
    cellIdentity.cellInfoType = (CellInfoType)rilCellIdentity.cellInfoType;
    switch(rilCellIdentity.cellInfoType) {

        case RIL_CELL_INFO_TYPE_GSM: {
            cellIdentity.cellIdentityGsm.resize(1);
            cellIdentity.cellIdentityGsm[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityGsm.mcc);
            cellIdentity.cellIdentityGsm[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityGsm.mnc);

            if (cellIdentity.cellIdentityGsm[0].mcc == "-1") {
                cellIdentity.cellIdentityGsm[0].mcc = "";
            }

            cellIdentity.cellIdentityGsm[0].lac = rilCellIdentity.cellIdentityGsm.lac;
            cellIdentity.cellIdentityGsm[0].cid = rilCellIdentity.cellIdentityGsm.cid;
            cellIdentity.cellIdentityGsm[0].arfcn = rilCellIdentity.cellIdentityGsm.arfcn;
            cellIdentity.cellIdentityGsm[0].bsic = rilCellIdentity.cellIdentityGsm.bsic;
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            cellIdentity.cellIdentityWcdma.resize(1);
            cellIdentity.cellIdentityWcdma[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityWcdma.mcc);
            cellIdentity.cellIdentityWcdma[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityWcdma.mnc);

            if (cellIdentity.cellIdentityWcdma[0].mcc == "-1") {
                cellIdentity.cellIdentityWcdma[0].mcc = "";
            }

            cellIdentity.cellIdentityWcdma[0].lac = rilCellIdentity.cellIdentityWcdma.lac;
            cellIdentity.cellIdentityWcdma[0].cid = rilCellIdentity.cellIdentityWcdma.cid;
            cellIdentity.cellIdentityWcdma[0].psc = rilCellIdentity.cellIdentityWcdma.psc;
            cellIdentity.cellIdentityWcdma[0].uarfcn = rilCellIdentity.cellIdentityWcdma.uarfcn;
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA: {
            cellIdentity.cellIdentityCdma.resize(1);
            cellIdentity.cellIdentityCdma[0].networkId = rilCellIdentity.cellIdentityCdma.networkId;
            cellIdentity.cellIdentityCdma[0].systemId = rilCellIdentity.cellIdentityCdma.systemId;
            cellIdentity.cellIdentityCdma[0].baseStationId =
                    rilCellIdentity.cellIdentityCdma.basestationId;
            cellIdentity.cellIdentityCdma[0].longitude = rilCellIdentity.cellIdentityCdma.longitude;
            cellIdentity.cellIdentityCdma[0].latitude = rilCellIdentity.cellIdentityCdma.latitude;
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE: {
            cellIdentity.cellIdentityLte.resize(1);
            cellIdentity.cellIdentityLte[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityLte.mcc);
            cellIdentity.cellIdentityLte[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityLte.mnc);

            if (cellIdentity.cellIdentityLte[0].mcc == "-1") {
                cellIdentity.cellIdentityLte[0].mcc = "";
            }

            cellIdentity.cellIdentityLte[0].ci = rilCellIdentity.cellIdentityLte.ci;
            cellIdentity.cellIdentityLte[0].pci = rilCellIdentity.cellIdentityLte.pci;
            cellIdentity.cellIdentityLte[0].tac = rilCellIdentity.cellIdentityLte.tac;
            cellIdentity.cellIdentityLte[0].earfcn = rilCellIdentity.cellIdentityLte.earfcn;
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA: {
            cellIdentity.cellIdentityTdscdma.resize(1);
            cellIdentity.cellIdentityTdscdma[0].mcc =
                    std::to_string(rilCellIdentity.cellIdentityTdscdma.mcc);
            cellIdentity.cellIdentityTdscdma[0].mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityTdscdma.mnc);

            if (cellIdentity.cellIdentityTdscdma[0].mcc == "-1") {
                cellIdentity.cellIdentityTdscdma[0].mcc = "";
            }

            cellIdentity.cellIdentityTdscdma[0].lac = rilCellIdentity.cellIdentityTdscdma.lac;
            cellIdentity.cellIdentityTdscdma[0].cid = rilCellIdentity.cellIdentityTdscdma.cid;
            cellIdentity.cellIdentityTdscdma[0].cpid = rilCellIdentity.cellIdentityTdscdma.cpid;
            break;
        }

        default: {
            break;
        }
    }
}

void fillCellIdentityResponse_1_2(V1_2::CellIdentity &cellIdentity, RIL_CellIdentity_v1_2 &rilCellIdentity) {
    cellIdentity.cellIdentityGsm.resize(0);
    cellIdentity.cellIdentityWcdma.resize(0);
    cellIdentity.cellIdentityCdma.resize(0);
    cellIdentity.cellIdentityTdscdma.resize(0);
    cellIdentity.cellIdentityLte.resize(0);
    cellIdentity.cellInfoType = (CellInfoType)rilCellIdentity.cellInfoType;
    switch(rilCellIdentity.cellInfoType) {

        case RIL_CELL_INFO_TYPE_GSM: {
            cellIdentity.cellIdentityGsm.resize(1);
            cellIdentity.cellIdentityGsm[0].base.mcc =
                    std::to_string(rilCellIdentity.cellIdentityGsm.mcc);
            cellIdentity.cellIdentityGsm[0].base.mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityGsm.mnc);

            if (cellIdentity.cellIdentityGsm[0].base.mcc == "-1") {
                cellIdentity.cellIdentityGsm[0].base.mcc = "";
            }

            cellIdentity.cellIdentityGsm[0].base.lac = rilCellIdentity.cellIdentityGsm.lac;
            cellIdentity.cellIdentityGsm[0].base.cid = rilCellIdentity.cellIdentityGsm.cid;
            cellIdentity.cellIdentityGsm[0].base.arfcn = rilCellIdentity.cellIdentityGsm.arfcn;
            cellIdentity.cellIdentityGsm[0].base.bsic = rilCellIdentity.cellIdentityGsm.bsic;
            cellIdentity.cellIdentityGsm[0].operatorNames.alphaLong =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityGsm.operatorNames.alphaLong);
            cellIdentity.cellIdentityGsm[0].operatorNames.alphaShort =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityGsm.operatorNames.alphaShort);
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            cellIdentity.cellIdentityWcdma.resize(1);
            cellIdentity.cellIdentityWcdma[0].base.mcc =
                    std::to_string(rilCellIdentity.cellIdentityWcdma.mcc);
            cellIdentity.cellIdentityWcdma[0].base.mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityWcdma.mnc);

            if (cellIdentity.cellIdentityWcdma[0].base.mcc == "-1") {
                cellIdentity.cellIdentityWcdma[0].base.mcc = "";
            }

            cellIdentity.cellIdentityWcdma[0].base.lac = rilCellIdentity.cellIdentityWcdma.lac;
            cellIdentity.cellIdentityWcdma[0].base.cid = rilCellIdentity.cellIdentityWcdma.cid;
            cellIdentity.cellIdentityWcdma[0].base.psc = rilCellIdentity.cellIdentityWcdma.psc;
            cellIdentity.cellIdentityWcdma[0].base.uarfcn = rilCellIdentity.cellIdentityWcdma.uarfcn;
            cellIdentity.cellIdentityWcdma[0].operatorNames.alphaLong =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityWcdma.operatorNames.alphaLong);
            cellIdentity.cellIdentityWcdma[0].operatorNames.alphaShort =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityWcdma.operatorNames.alphaShort);
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA: {
            cellIdentity.cellIdentityCdma.resize(1);
            cellIdentity.cellIdentityCdma[0].base.networkId = rilCellIdentity.cellIdentityCdma.networkId;
            cellIdentity.cellIdentityCdma[0].base.systemId = rilCellIdentity.cellIdentityCdma.systemId;
            cellIdentity.cellIdentityCdma[0].base.baseStationId =
                    rilCellIdentity.cellIdentityCdma.basestationId;
            cellIdentity.cellIdentityCdma[0].base.longitude = rilCellIdentity.cellIdentityCdma.longitude;
            cellIdentity.cellIdentityCdma[0].base.latitude = rilCellIdentity.cellIdentityCdma.latitude;
            cellIdentity.cellIdentityCdma[0].operatorNames.alphaLong =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityCdma.operatorNames.alphaLong);
            cellIdentity.cellIdentityCdma[0].operatorNames.alphaShort =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityCdma.operatorNames.alphaShort);
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE: {
            cellIdentity.cellIdentityLte.resize(1);
            cellIdentity.cellIdentityLte[0].base.mcc =
                    std::to_string(rilCellIdentity.cellIdentityLte.mcc);
            cellIdentity.cellIdentityLte[0].base.mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityLte.mnc);

            if (cellIdentity.cellIdentityLte[0].base.mcc == "-1") {
                cellIdentity.cellIdentityLte[0].base.mcc = "";
            }

            cellIdentity.cellIdentityLte[0].base.ci = rilCellIdentity.cellIdentityLte.ci;
            cellIdentity.cellIdentityLte[0].base.pci = rilCellIdentity.cellIdentityLte.pci;
            cellIdentity.cellIdentityLte[0].base.tac = rilCellIdentity.cellIdentityLte.tac;
            cellIdentity.cellIdentityLte[0].base.earfcn = rilCellIdentity.cellIdentityLte.earfcn;
            cellIdentity.cellIdentityLte[0].operatorNames.alphaLong =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityLte.operatorNames.alphaLong);
            cellIdentity.cellIdentityLte[0].operatorNames.alphaShort =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityLte.operatorNames.alphaShort);
            cellIdentity.cellIdentityLte[0].bandwidth = rilCellIdentity.cellIdentityLte.bandwidth;
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA: {
            cellIdentity.cellIdentityTdscdma.resize(1);
            cellIdentity.cellIdentityTdscdma[0].base.mcc =
                    std::to_string(rilCellIdentity.cellIdentityTdscdma.mcc);
            cellIdentity.cellIdentityTdscdma[0].base.mnc =
                    ril::util::mnc::decode(rilCellIdentity.cellIdentityTdscdma.mnc);

            if (cellIdentity.cellIdentityTdscdma[0].base.mcc == "-1") {
                cellIdentity.cellIdentityTdscdma[0].base.mcc = "";
            }

            cellIdentity.cellIdentityTdscdma[0].base.lac = rilCellIdentity.cellIdentityTdscdma.lac;
            cellIdentity.cellIdentityTdscdma[0].base.cid = rilCellIdentity.cellIdentityTdscdma.cid;
            cellIdentity.cellIdentityTdscdma[0].base.cpid = rilCellIdentity.cellIdentityTdscdma.cpid;
            cellIdentity.cellIdentityTdscdma[0].uarfcn = rilCellIdentity.cellIdentityTdscdma.uarfcn;
            cellIdentity.cellIdentityTdscdma[0].operatorNames.alphaLong =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaLong);
            cellIdentity.cellIdentityTdscdma[0].operatorNames.alphaShort =
                    convertCharPtrToHidlString(rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaShort);
            break;
        }

        default: {
            break;
        }
    }
}

int convertResponseStringEntryToInt(char **response, int index, int numStrings) {
    if ((response != NULL) &&  (numStrings > index) && (response[index] != NULL)) {
        return atoi(response[index]);
    }

    return -1;
}
int convertResponseHexStringEntryToInt(char **response, int index, int numStrings) {
    const int hexBase = 16;
    if ((response != NULL) &&  (numStrings > index) && (response[index] != NULL)) {
        return strtol(response[index], NULL, hexBase);
    }

    return -1;
}

/* Fill Cell Identity info from Voice Registration State Response.
 * This fucntion is applicable only for RIL Version < 15.
 * Response is a  "char **".
 * First and Second entries are in hex string format
 * and rest are integers represented in ascii format. */
void fillCellIdentityFromVoiceRegStateResponseString(CellIdentity &cellIdentity,
        int numStrings, char** response) {
    int strLen = 0;
    long tmpcid = 0;

    RIL_CellIdentity_v16 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v16));

    if (rilCellIdentity.cellInfoType != RIL_CELL_INFO_TYPE_NR && response[2] != NULL) {
        sscanf(response[2], "%lx", &tmpcid);
        strLen = strlen(response[2]) + 1;
        if (tmpcid > 0x7fffffff) {
            tmpcid &= 0x7fffffff;
            snprintf(response[2], strLen, "%lx", tmpcid);
            RLOGD("response str: %s", response[2]);
        }
    }

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);
    switch(rilCellIdentity.cellInfoType) {

        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            rilCellIdentity.cellIdentityWcdma.psc =
                    convertResponseStringEntryToInt(response, 14, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA:{
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA:{
            rilCellIdentity.cellIdentityCdma.basestationId =
                    convertResponseStringEntryToInt(response, 4, numStrings);
            /* Order of Lat. and Long. swapped between RIL and HIDL interface versions. */
            rilCellIdentity.cellIdentityCdma.latitude =
                    convertResponseStringEntryToInt(response, 5, numStrings);
            rilCellIdentity.cellIdentityCdma.longitude =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityCdma.systemId =
                    convertResponseStringEntryToInt(response, 8, numStrings);
            rilCellIdentity.cellIdentityCdma.networkId =
                    convertResponseStringEntryToInt(response, 9, numStrings);
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE:{
            /* valid TAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            break;
        }

        default: {
            break;
        }
    }

    fillCellIdentityResponse(cellIdentity, rilCellIdentity);
}

void fillCellIdentityFromVoiceRegStateResponseString_1_2(V1_2::CellIdentity &cellIdentity,
        int numStrings, char** response) {
    int strLen = 0;
    long tmpcid = 0;
    RIL_CellIdentity_v1_2 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v1_2));

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);

    if (rilCellIdentity.cellInfoType != RIL_CELL_INFO_TYPE_NR && response[2] != NULL) {
        sscanf(response[2], "%lx", &tmpcid);
        strLen = strlen(response[2]) + 1;
        if (tmpcid > 0x7fffffff) {
            tmpcid &= 0x7fffffff;
            snprintf(response[2], strLen, "%lx", tmpcid);
            RLOGD("response str: %s", response[2]);
        }
    }

    switch(rilCellIdentity.cellInfoType) {
        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            // TODO
            rilCellIdentity.cellIdentityGsm.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityGsm.operatorNames.alphaShort = NULL;
            break;
        }

        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            rilCellIdentity.cellIdentityWcdma.psc =
                    convertResponseStringEntryToInt(response, 14, numStrings);

            rilCellIdentity.cellIdentityWcdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityWcdma.operatorNames.alphaShort = NULL;
            break;
        }

        case RIL_CELL_INFO_TYPE_TD_SCDMA:{
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaShort = NULL;
            break;
        }

        case RIL_CELL_INFO_TYPE_CDMA:{
            rilCellIdentity.cellIdentityCdma.basestationId =
                    convertResponseStringEntryToInt(response, 4, numStrings);
            /* Order of Lat. and Long. swapped between RIL and HIDL interface versions. */
            rilCellIdentity.cellIdentityCdma.latitude =
                    convertResponseStringEntryToInt(response, 5, numStrings);
            rilCellIdentity.cellIdentityCdma.longitude =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityCdma.systemId =
                    convertResponseStringEntryToInt(response, 8, numStrings);
            rilCellIdentity.cellIdentityCdma.networkId =
                    convertResponseStringEntryToInt(response, 9, numStrings);

            rilCellIdentity.cellIdentityCdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityCdma.operatorNames.alphaShort = NULL;
            break;
        }

        case RIL_CELL_INFO_TYPE_LTE:{
            /* valid TAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);
            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);
            rilCellIdentity.cellIdentityLte.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityLte.operatorNames.alphaShort = NULL;
            rilCellIdentity.cellIdentityLte.bandwidth = INT_MAX;
            break;
        }
        default: {
            break;
        }
    }

    fillCellIdentityResponse_1_2(cellIdentity, rilCellIdentity);
}

/* Fill Cell Identity info from Data Registration State Response.
 * This fucntion is applicable only for RIL Version < 15.
 * Response is a  "char **".
 * First and Second entries are in hex string format
 * and rest are integers represented in ascii format. */
void fillCellIdentityFromDataRegStateResponseString(CellIdentity &cellIdentity,
        int numStrings, char** response) {
    int strLen = 0;
    long tmpcid = 0;

    RIL_CellIdentity_v16 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v16));

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);

    if (rilCellIdentity.cellInfoType != RIL_CELL_INFO_TYPE_NR && response[2] != NULL) {
        sscanf(response[2], "%lx", &tmpcid);
        strLen = strlen(response[2]) + 1;
        if (tmpcid > 0x7fffffff) {
            tmpcid &= 0x7fffffff;
            snprintf(response[2], strLen, "%lx", tmpcid);
            RLOGD("response str: %s", response[2]);
        }
    }

    switch(rilCellIdentity.cellInfoType) {
        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityGsm.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityGsm.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityGsm.mnc = 0;
                rilCellIdentity.cellIdentityGsm.mcc = 0;
            }
            break;
        }
        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityWcdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityWcdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityWcdma.mnc = 0;
                rilCellIdentity.cellIdentityWcdma.mcc = 0;
            }

            break;
        }
        case RIL_CELL_INFO_TYPE_TD_SCDMA:{
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityTdscdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityTdscdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityTdscdma.mnc = 0;
                rilCellIdentity.cellIdentityTdscdma.mcc = 0;
            }
            break;
        }
        case RIL_CELL_INFO_TYPE_LTE: {
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityLte.pci =
                    convertResponseStringEntryToInt(response, 7, numStrings);
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseStringEntryToInt(response, 8, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityLte.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityLte.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityLte.mnc = 0;
                rilCellIdentity.cellIdentityLte.mcc = 0;
            }
            break;
        }
        // TODO add CDMA
        default: {
            break;
        }
    }

    fillCellIdentityResponse(cellIdentity, rilCellIdentity);
}

void fillCellIdentityFromDataRegStateResponseString_1_2(V1_2::CellIdentity &cellIdentity,
        int numStrings, char** response) {
    int strLen = 0;
    long tmpcid = 0;
    RIL_CellIdentity_v1_2 rilCellIdentity;
    memset(&rilCellIdentity, -1, sizeof(RIL_CellIdentity_v1_2));
    RLOGD("response[2]fxy:%s",response[2]);
    if (rilCellIdentity.cellInfoType != RIL_CELL_INFO_TYPE_NR && response[2] != NULL) {
        sscanf(response[2], "%lx", &tmpcid);
        strLen = strlen(response[2]) + 1;
        if (tmpcid > 0x7fffffff) {
            tmpcid &= 0x7fffffff;
            snprintf(response[2], strLen, "%lx", tmpcid);
            RLOGD("response str: %s", response[2]);
        }
    }

    rilCellIdentity.cellInfoType = getCellInfoTypeRadioTechnology(response[3]);
    switch(rilCellIdentity.cellInfoType) {
        case RIL_CELL_INFO_TYPE_GSM: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityGsm.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityGsm.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityGsm.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityGsm.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityGsm.mnc = 0;
                rilCellIdentity.cellIdentityGsm.mcc = 0;
            }

            // TODO
            rilCellIdentity.cellIdentityGsm.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityGsm.operatorNames.alphaShort = NULL;
            break;
        }
        case RIL_CELL_INFO_TYPE_WCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityWcdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityWcdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityWcdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityWcdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityWcdma.mnc = 0;
                rilCellIdentity.cellIdentityWcdma.mcc = 0;
            }

            rilCellIdentity.cellIdentityWcdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityWcdma.operatorNames.alphaShort = NULL;
            break;
        }
        case RIL_CELL_INFO_TYPE_TD_SCDMA: {
            /* valid LAC are hexstrings in the range 0x0000 - 0xffff */
            rilCellIdentity.cellIdentityTdscdma.lac =
                    convertResponseHexStringEntryToInt(response, 1, numStrings);

            /* valid CID are hexstrings in the range 0x00000000 - 0xffffffff */
            rilCellIdentity.cellIdentityTdscdma.cid =
                    convertResponseHexStringEntryToInt(response, 2, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityTdscdma.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityTdscdma.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityTdscdma.mnc = 0;
                rilCellIdentity.cellIdentityTdscdma.mcc = 0;
            }

            rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityTdscdma.operatorNames.alphaShort = NULL;
            break;
        }
        case RIL_CELL_INFO_TYPE_LTE: {
            rilCellIdentity.cellIdentityLte.tac =
                    convertResponseStringEntryToInt(response, 6, numStrings);
            rilCellIdentity.cellIdentityLte.pci =
                    convertResponseStringEntryToInt(response, 7, numStrings);
            rilCellIdentity.cellIdentityLte.ci =
                    convertResponseStringEntryToInt(response, 8, numStrings);

            if (numStrings >= 13) {
                rilCellIdentity.cellIdentityLte.mcc =
                        convertResponseStringEntryToInt(response, 11, numStrings);

                rilCellIdentity.cellIdentityLte.mnc =
                        convertResponseStringEntryToInt(response, 12, numStrings);
            } else {
                /* vts check the mcc [0, 999] and mnc [0, 999]. */
                rilCellIdentity.cellIdentityLte.mnc = 0;
                rilCellIdentity.cellIdentityLte.mcc = 0;
            }

            rilCellIdentity.cellIdentityLte.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityLte.operatorNames.alphaShort = NULL;
            break;
        }
        case RIL_CELL_INFO_TYPE_CDMA: {
            rilCellIdentity.cellIdentityCdma.operatorNames.alphaLong = NULL;
            rilCellIdentity.cellIdentityCdma.operatorNames.alphaShort = NULL;
            break;
        }
        default: {
            break;
        }
    }

    fillCellIdentityResponse_1_2(cellIdentity, rilCellIdentity);
}

int radio::getVoiceRegistrationStateResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVoiceRegistrationStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
            radioService[slotId]->mRadioResponseV1_2 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
            V1_2::VoiceRegStateResult voiceRegResponse = {};
            if (response == NULL) {
                   RLOGE("getVoiceRegistrationStateResponse_1_2 Invalid response: NULL");
                   if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else if (s_vendorFunctions->version <= 14) {
                int numStrings = responseLen / sizeof(char *);
                if (numStrings != 15) {
                    RLOGE("getVoiceRegistrationStateResponse_1_2 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    char **resp = (char **) response;
                    voiceRegResponse.regState = (RegState) ATOI_NULL_HANDLED_DEF(resp[0], 4);
                    // Modify for Bug833309: set default rat value to RADIO_TECH_UNKNOWN
                    voiceRegResponse.rat = ATOI_NULL_HANDLED_DEF(resp[3], 0);
                    voiceRegResponse.cssSupported = ATOI_NULL_HANDLED_DEF(resp[7], 0);
                    voiceRegResponse.roamingIndicator = ATOI_NULL_HANDLED(resp[10]);
                    voiceRegResponse.systemIsInPrl = ATOI_NULL_HANDLED_DEF(resp[11], 0);
                    voiceRegResponse.defaultRoamingIndicator = ATOI_NULL_HANDLED_DEF(resp[12], 0);
                    voiceRegResponse.reasonForDenial = ATOI_NULL_HANDLED_DEF(resp[13], 0);
                    fillCellIdentityFromVoiceRegStateResponseString_1_2(
                            voiceRegResponse.cellIdentity, numStrings, resp);
                }
            } else {
                RIL_VoiceRegStateResult_v1_2 *voiceRegState =
                        (RIL_VoiceRegStateResult_v1_2 *)response;

                if (responseLen != sizeof(RIL_VoiceRegStateResult_v1_2)) {
                    RLOGE("getVoiceRegistrationStateResponse_1_2 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    voiceRegResponse.regState = (RegState) voiceRegState->regState;
                    voiceRegResponse.rat = voiceRegState->rat;;
                    voiceRegResponse.cssSupported = voiceRegState->cssSupported;
                    voiceRegResponse.roamingIndicator = voiceRegState->roamingIndicator;
                    voiceRegResponse.systemIsInPrl = voiceRegState->systemIsInPrl;
                    voiceRegResponse.defaultRoamingIndicator = voiceRegState->defaultRoamingIndicator;
                    voiceRegResponse.reasonForDenial = voiceRegState->reasonForDenial;
                    fillCellIdentityResponse_1_2(voiceRegResponse.cellIdentity,
                            voiceRegState->cellIdentity);
                }
            }

            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getVoiceRegistrationStateResponse_1_2(responseInfo, voiceRegResponse);
        } else {
            VoiceRegStateResult voiceRegResponse = {};
            int numStrings = responseLen / sizeof(char *);
            if (response == NULL) {
                   RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
                   if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else if (s_vendorFunctions->version <= 14) {
                if (numStrings != 15) {
                    RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    char **resp = (char **)response;
                    voiceRegResponse.regState = (RegState) ATOI_NULL_HANDLED_DEF(resp[0], 4);
                    // Modify for Bug833309: set default rat value to RADIO_TECH_UNKNOWN
                    voiceRegResponse.rat = ATOI_NULL_HANDLED_DEF(resp[3], 0);
                    voiceRegResponse.cssSupported = ATOI_NULL_HANDLED_DEF(resp[7], 0);
                    voiceRegResponse.roamingIndicator = ATOI_NULL_HANDLED(resp[10]);
                    voiceRegResponse.systemIsInPrl = ATOI_NULL_HANDLED_DEF(resp[11], 0);
                    voiceRegResponse.defaultRoamingIndicator = ATOI_NULL_HANDLED_DEF(resp[12], 0);
                    voiceRegResponse.reasonForDenial = ATOI_NULL_HANDLED_DEF(resp[13], 0);
                    fillCellIdentityFromVoiceRegStateResponseString(voiceRegResponse.cellIdentity,
                            numStrings, resp);
                }
            } else {
                RIL_VoiceRegistrationStateResponse *voiceRegState =
                        (RIL_VoiceRegistrationStateResponse *)response;

                if (responseLen != sizeof(RIL_VoiceRegistrationStateResponse)) {
                    RLOGE("getVoiceRegistrationStateResponse Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    voiceRegResponse.regState = (RegState) voiceRegState->regState;
                    voiceRegResponse.rat = voiceRegState->rat;;
                    voiceRegResponse.cssSupported = voiceRegState->cssSupported;
                    voiceRegResponse.roamingIndicator = voiceRegState->roamingIndicator;
                    voiceRegResponse.systemIsInPrl = voiceRegState->systemIsInPrl;
                    voiceRegResponse.defaultRoamingIndicator = voiceRegState->defaultRoamingIndicator;
                    voiceRegResponse.reasonForDenial = voiceRegState->reasonForDenial;
                    fillCellIdentityResponse(voiceRegResponse.cellIdentity,
                            voiceRegState->cellIdentity);
                }
            }

            retStatus = radioService[slotId]->mRadioResponse->
                    getVoiceRegistrationStateResponse(responseInfo, voiceRegResponse);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getVoiceRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getDataRegistrationStateResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getDataRegistrationStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
            radioService[slotId]->mRadioResponseV1_2 != NULL ||
            radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            V1_4::DataRegStateResult dataRegResponse = {};
            if (response == NULL) {
                RLOGE("getDataRegistrationStateResponse_1_4 Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else if (s_vendorFunctions->version <= 14) {
                int numStrings = responseLen / sizeof(char *);
                // UNISOC add for 5G
                if ((numStrings != 6) && (numStrings != 11) && (numStrings != 13) && (numStrings != 12)) {
                    RLOGE("getDataRegistrationStateResponse_1_4 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    char **resp = (char **) response;
                    int rat = ATOI_NULL_HANDLED_DEF(resp[3], 0);
                    dataRegResponse.base.regState = (RegState)ATOI_NULL_HANDLED_DEF(resp[0], 4);
                    dataRegResponse.base.rat = rat;
                    dataRegResponse.base.reasonDataDenied = ATOI_NULL_HANDLED(resp[4]);
                    dataRegResponse.base.maxDataCalls = ATOI_NULL_HANDLED_DEF(resp[5], 1);
                    fillCellIdentityFromDataRegStateResponseString_1_2(dataRegResponse.base.cellIdentity,
                                 numStrings, resp);

                    if (rat == RADIO_TECH_LTE || rat == RADIO_TECH_LTE_CA ||
                        rat == RADIO_TECH_NR) {
                        V1_4::LteVopsInfo lteVopsInfo;
                        lteVopsInfo.isVopsSupported = false;  // TODO
                        lteVopsInfo.isEmcBearerSupported = false;
                        dataRegResponse.vopsInfo.lteVopsInfo(lteVopsInfo);
                    }
                    dataRegResponse.nrIndicators.isEndcAvailable = (ATOI_NULL_HANDLED_DEF(resp[9], 0) == 1);
                    dataRegResponse.nrIndicators.isDcNrRestricted = (ATOI_NULL_HANDLED_DEF(resp[10], 0) == 1);
                    dataRegResponse.nrIndicators.isNrAvailable = (ATOI_NULL_HANDLED_DEF(resp[11], 0) == 1);
                }
            } else {
                RIL_DataRegStateResult_v1_4 *dataRegState =
                        (RIL_DataRegStateResult_v1_4 *) response;

                if (responseLen != sizeof(RIL_DataRegStateResult_v1_4)) {
                    RLOGE("getDataRegistrationStateResponse_1_4 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS)
                        responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    dataRegResponse.base.regState = (RegState) dataRegState->base.regState;
                    dataRegResponse.base.rat = dataRegState->base.rat;
                    dataRegResponse.base.reasonDataDenied = dataRegState->base.reasonDataDenied;
                    dataRegResponse.base.maxDataCalls = dataRegState->base.maxDataCalls;
                    fillCellIdentityResponse_1_2(dataRegResponse.base.cellIdentity,
                            dataRegState->base.cellIdentity);

                    if (dataRegState->base.rat == RADIO_TECH_LTE ||
                        dataRegState->base.rat == RADIO_TECH_LTE_CA ||
                        dataRegState->base.rat == RADIO_TECH_NR) {
                        V1_4::LteVopsInfo lteVopsInfo;
                        lteVopsInfo.isVopsSupported = dataRegState->vopsInfo.lteVopsInfo.isVopsSupported;
                        lteVopsInfo.isEmcBearerSupported = dataRegState->vopsInfo.lteVopsInfo.isEmcBearerSupported;
                        dataRegResponse.vopsInfo.lteVopsInfo(lteVopsInfo);
                    }

                    dataRegResponse.nrIndicators.isEndcAvailable = dataRegState->nrIndicators.isEndcAvailable;
                    dataRegResponse.nrIndicators.isDcNrRestricted = dataRegState->nrIndicators.isDcNrRestricted;
                    dataRegResponse.nrIndicators.isEndcAvailable = dataRegState->nrIndicators.isNrAvailable;
                }
            }

            retStatus = radioService[slotId]->mRadioResponseV1_4->
                    getDataRegistrationStateResponse_1_4(responseInfo, dataRegResponse);
        } else if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
            V1_2::DataRegStateResult dataRegResponse = {};
            if (response == NULL) {
                RLOGE("getDataRegistrationStateResponse_1_2 Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else if (s_vendorFunctions->version <= 14) {
                int numStrings = responseLen / sizeof(char *);
                if ((numStrings != 6) && (numStrings != 11) && (numStrings != 13) && (numStrings != 12)) {
                    RLOGE("getDataRegistrationStateResponse_1_2 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    char **resp = (char **)response;
                    dataRegResponse.regState = (RegState)ATOI_NULL_HANDLED_DEF(resp[0], 4);
                    dataRegResponse.rat =  ATOI_NULL_HANDLED_DEF(resp[3], 0);
                    dataRegResponse.reasonDataDenied =  ATOI_NULL_HANDLED(resp[4]);
                    dataRegResponse.maxDataCalls =  ATOI_NULL_HANDLED_DEF(resp[5], 1);
                    fillCellIdentityFromDataRegStateResponseString_1_2(dataRegResponse.cellIdentity,
                            numStrings, resp);
                }
            } else {
                RIL_DataRegStateResult_v1_2 *dataRegState =
                        (RIL_DataRegStateResult_v1_2 *) response;

                if (responseLen != sizeof(RIL_DataRegStateResult_v1_2)) {
                    RLOGE("getDataRegistrationStateResponse_1_2 Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    dataRegResponse.regState = (RegState) dataRegState->regState;
                    dataRegResponse.rat = dataRegState->rat;;
                    dataRegResponse.reasonDataDenied = dataRegState->reasonDataDenied;
                    dataRegResponse.maxDataCalls = dataRegState->maxDataCalls;
                    fillCellIdentityResponse_1_2(dataRegResponse.cellIdentity, dataRegState->cellIdentity);
                }
            }

            retStatus = radioService[slotId]->mRadioResponseV1_2->
                    getDataRegistrationStateResponse_1_2(responseInfo, dataRegResponse);
        } else {
            DataRegStateResult dataRegResponse = {};
            if (response == NULL) {
                RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else if (s_vendorFunctions->version <= 14) {
                int numStrings = responseLen / sizeof(char *);
                if ((numStrings != 6) && (numStrings != 11) && (numStrings != 13) && (numStrings != 12)) {
                    RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    char **resp = (char **) response;
                    dataRegResponse.regState = (RegState) ATOI_NULL_HANDLED_DEF(resp[0], 4);
                    dataRegResponse.rat =  ATOI_NULL_HANDLED_DEF(resp[3], 0);
                    dataRegResponse.reasonDataDenied =  ATOI_NULL_HANDLED(resp[4]);
                    dataRegResponse.maxDataCalls =  ATOI_NULL_HANDLED_DEF(resp[5], 1);
                    fillCellIdentityFromDataRegStateResponseString(dataRegResponse.cellIdentity,
                            numStrings, resp);
                }
            } else {
                RIL_DataRegistrationStateResponse *dataRegState =
                        (RIL_DataRegistrationStateResponse *)response;

                if (responseLen != sizeof(RIL_DataRegistrationStateResponse)) {
                    RLOGE("getDataRegistrationStateResponse Invalid response: NULL");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    dataRegResponse.regState = (RegState) dataRegState->regState;
                    dataRegResponse.rat = dataRegState->rat;;
                    dataRegResponse.reasonDataDenied = dataRegState->reasonDataDenied;
                    dataRegResponse.maxDataCalls = dataRegState->maxDataCalls;
                    fillCellIdentityResponse(dataRegResponse.cellIdentity, dataRegState->cellIdentity);
                }
            }

            retStatus = radioService[slotId]->mRadioResponse->
                    getDataRegistrationStateResponse(responseInfo, dataRegResponse);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getDataRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getOperatorResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getOperatorResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_string longName;
        hidl_string shortName;
        hidl_string numeric;
        int numStrings = responseLen / sizeof(char *);
        if (response == NULL || numStrings != 3) {
            RLOGE("getOperatorResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **) response;
            longName = convertCharPtrToHidlString(resp[0]);
            shortName = convertCharPtrToHidlString(resp[1]);
            numeric = convertCharPtrToHidlString(resp[2]);
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getOperatorResponse(
                responseInfo, longName, shortName, numeric);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getOperatorResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setRadioPowerResponse(int slotId,
                                int responseType, int serial, RIL_Errno e, void *response,
                                size_t responseLen) {
    RLOGD("setRadioPowerResponse: serial %d", serial);

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setRadioPowerResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setRadioPowerResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendDtmfResponse(int slotId,
                           int responseType, int serial, RIL_Errno e, void *response,
                           size_t responseLen) {
#if VDBG
    RLOGD("sendDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendDtmfResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendDtmfResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

SendSmsResult makeSendSmsResult(RadioResponseInfo& responseInfo, int serial, int responseType,
                                RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    SendSmsResult result = {};

    if (response == NULL || responseLen != sizeof(RIL_SMS_Response)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        result.ackPDU = hidl_string();
    } else {
        RIL_SMS_Response *resp = (RIL_SMS_Response *) response;
        result.messageRef = resp->messageRef;
        result.ackPDU = convertCharPtrToHidlString(resp->ackPDU);
        result.errorCode = resp->errorCode;
    }
    return result;
}

int radio::sendSmsResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("sendSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendSmsResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendSMSExpectMoreResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("sendSMSExpectMoreResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendSMSExpectMoreResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSMSExpectMoreResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setupDataCallResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setupDataCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            V1_4::SetupDataCallResult result = {};
            if (response == NULL || (responseLen % sizeof(RIL_SetupDataCallResult_v1_4)) != 0) {
                if (response != NULL) {
                    RLOGE("setupDataCallResponse_1_4: Invalid response");
                    if (e == RIL_E_SUCCESS)
                        responseInfo.error = RadioError::INVALID_RESPONSE;
                }
                result.cause = V1_4::DataCallFailCause::ERROR_UNSPECIFIED;
                result.type = V1_4::PdpProtocolType::UNKNOWN;
                result.ifname = hidl_string();
                result.addresses.resize(0);
                result.dnses.resize(0);
                result.gateways.resize(0);
                result.pcscf.resize(0);
            } else {
                convertRilDataCallToHal_1_4((RIL_SetupDataCallResult_v1_4 *)response, result);
            }

            if (e != RIL_E_RADIO_NOT_AVAILABLE) {
                responseInfo.error = RadioError::NONE;
            }
            retStatus = radioService[slotId]->mRadioResponseV1_4->setupDataCallResponse_1_4(
                            responseInfo, result);
        } else {
            SetupDataCallResult result = {};
            if (response == NULL || (responseLen % sizeof(RIL_SetupDataCallResult_v1_4)) != 0) {
                if (response != NULL) {
                    RLOGE("setupDataCallResponse: Invalid response");
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                }
                result.status = DataCallFailCause::ERROR_UNSPECIFIED;
                result.type = hidl_string();
                result.ifname = hidl_string();
                result.addresses = hidl_string();
                result.dnses = hidl_string();
                result.gateways = hidl_string();
                result.pcscf = hidl_string();
            } else {
                convertRilDataCallToHal((RIL_SetupDataCallResult_v1_4 *)response, result);
            }

            if (e != RIL_E_RADIO_NOT_AVAILABLE) {
                responseInfo.error = RadioError::NONE;
            }
            retStatus = radioService[slotId]->mRadioResponse->setupDataCallResponse(
                    responseInfo, result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setupDataCallResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }
    return 0;
}

IccIoResult responseIccIo(RadioResponseInfo& responseInfo, int serial, int responseType,
                           RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    IccIoResult result = {};

    if (response == NULL || responseLen != sizeof(RIL_SIM_IO_Response)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        result.simResponse = hidl_string();
    } else {
        RIL_SIM_IO_Response *resp = (RIL_SIM_IO_Response *) response;
        result.sw1 = resp->sw1;
        result.sw2 = resp->sw2;
        result.simResponse = convertCharPtrToHidlString(resp->simResponse);
    }
    return result;
}

int radio::iccIOForAppResponse(int slotId,
                      int responseType, int serial, RIL_Errno e, void *response,
                      size_t responseLen) {
#if VDBG
    RLOGD("iccIOForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus = radioService[slotId]->mRadioResponse->iccIOForAppResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccIOForAppResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendUssdResponse(int slotId,
                           int responseType, int serial, RIL_Errno e, void *response,
                           size_t responseLen) {
#if VDBG
    RLOGD("sendUssdResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->sendUssdResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendUssdResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::cancelPendingUssdResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("cancelPendingUssdResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->cancelPendingUssdResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cancelPendingUssdResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getClirResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getClirResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        int n = -1, m = -1;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 2) {
            RLOGE("getClirResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            n = pInt[0];
            m = pInt[1];
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getClirResponse(responseInfo,
                n, m);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getClirResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setClirResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("setClirResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setClirResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setClirResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getCallForwardStatusResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCallForwardStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<CallForwardInfo> callForwardInfos;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_CallForwardInfo *) != 0) {
            RLOGE("getCallForwardStatusResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_CallForwardInfo *);
            callForwardInfos.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_CallForwardInfo *resp = ((RIL_CallForwardInfo **) response)[i];
                callForwardInfos[i].status = (CallForwardInfoStatus) resp->status;
                callForwardInfos[i].reason = resp->reason;
                callForwardInfos[i].serviceClass = resp->serviceClass;
                callForwardInfos[i].toa = resp->toa;
                callForwardInfos[i].number = convertCharPtrToHidlString(resp->number);
                callForwardInfos[i].timeSeconds = resp->timeSeconds;
            }
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getCallForwardStatusResponse(
                responseInfo, callForwardInfos);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCallForwardStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCallForwardResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setCallForwardResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setCallForwardResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCallForwardResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getCallWaitingResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("getCallWaitingResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        int serviceClass = -1;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 2) {
            RLOGE("getCallWaitingResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
            serviceClass = pInt[1];
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getCallWaitingResponse(
                responseInfo, enable, serviceClass);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCallWaitingResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setCallWaitingResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("setCallWaitingResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setCallWaitingResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCallWaitingResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::acknowledgeLastIncomingGsmSmsResponse(int slotId,
                                                int responseType, int serial, RIL_Errno e,
                                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeLastIncomingGsmSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus =
                radioService[slotId]->mRadioResponse->acknowledgeLastIncomingGsmSmsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeLastIncomingGsmSmsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::acceptCallResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("acceptCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->acceptCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acceptCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::deactivateDataCallResponse(int slotId,
                                                int responseType, int serial, RIL_Errno e,
                                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("deactivateDataCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->deactivateDataCallResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deactivateDataCallResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getFacilityLockForAppResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getFacilityLockForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                getFacilityLockForAppResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getFacilityLockForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setFacilityLockForAppResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("setFacilityLockForAppResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmpty(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setFacilityLockForAppResponse(responseInfo,
                ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setFacilityLockForAppResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setBarringPasswordResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("acceptCallResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setBarringPasswordResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setBarringPasswordResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getNetworkSelectionModeResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("getNetworkSelectionModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool manual = false;
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("getNetworkSelectionModeResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            manual = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getNetworkSelectionModeResponse(
                responseInfo,
                manual);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getNetworkSelectionModeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setNetworkSelectionModeAutomaticResponse(int slotId, int responseType, int serial,
                                                    RIL_Errno e, void *response,
                                                    size_t responseLen) {
#if VDBG
    RLOGD("setNetworkSelectionModeAutomaticResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setNetworkSelectionModeAutomaticResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setNetworkSelectionModeAutomaticResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::setNetworkSelectionModeManualResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setNetworkSelectionModeManualResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setNetworkSelectionModeManualResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acceptCallResponse: radioService[%d]->setNetworkSelectionModeManualResponse "
                "== NULL", slotId);
    }

    return 0;
}

int convertOperatorStatusToInt(const char *str) {
    if (strncmp("unknown", str, 9) == 0) {
        return (int) OperatorStatus::UNKNOWN;
    } else if (strncmp("available", str, 9) == 0) {
        return (int) OperatorStatus::AVAILABLE;
    } else if (strncmp("current", str, 9) == 0) {
        return (int) OperatorStatus::CURRENT;
    } else if (strncmp("forbidden", str, 9) == 0) {
        return (int) OperatorStatus::FORBIDDEN;
    } else {
        return -1;
    }
}

int radio::getAvailableNetworksResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("getAvailableNetworksResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<OperatorInfo> networks;
        if ((response == NULL && responseLen != 0)
                || responseLen % (4 * sizeof(char *))!= 0) {
            RLOGE("getAvailableNetworksResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **) response;
            int numStrings = responseLen / sizeof(char *);
            networks.resize(numStrings/4);
            for (int i = 0, j = 0; i < numStrings; i = i + 4, j++) {
                networks[j].alphaLong = convertCharPtrToHidlString(resp[i]);
                networks[j].alphaShort = convertCharPtrToHidlString(resp[i + 1]);
                networks[j].operatorNumeric = convertCharPtrToHidlString(resp[i + 2]);
                int status = convertOperatorStatusToInt(resp[i + 3]);
                if (status == -1) {
                    if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
                } else {
                    networks[j].status = (OperatorStatus) status;
                }
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getAvailableNetworksResponse(responseInfo,
                networks);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAvailableNetworksResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::startDtmfResponse(int slotId,
                            int responseType, int serial, RIL_Errno e,
                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("startDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->startDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::stopDtmfResponse(int slotId,
                           int responseType, int serial, RIL_Errno e,
                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->stopDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getBasebandVersionResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getBasebandVersionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getBasebandVersionResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getBasebandVersionResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::separateConnectionResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("separateConnectionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->separateConnectionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("separateConnectionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setMuteResponse(int slotId,
                          int responseType, int serial, RIL_Errno e,
                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("setMuteResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setMuteResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setMuteResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getMuteResponse(int slotId,
                          int responseType, int serial, RIL_Errno e, void *response,
                          size_t responseLen) {
#if VDBG
    RLOGD("getMuteResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("getMuteResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getMuteResponse(responseInfo,
                enable);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getMuteResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getClipResponse(int slotId,
                          int responseType, int serial, RIL_Errno e,
                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getClipResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getClipResponse(responseInfo,
                (ClipStatus) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getClipResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getDataCallListResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("getDataCallListResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            hidl_vec<V1_4::SetupDataCallResult> ret;
            if ((response == NULL && responseLen != 0) ||
                 responseLen % sizeof(RIL_SetupDataCallResult_v1_4) != 0) {
                RLOGE("getDataCallListResponse_1_4: invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilDataCallListToHal_1_4(response, responseLen, ret);
            }

            retStatus = radioService[slotId]->mRadioResponseV1_4->getDataCallListResponse_1_4(
                            responseInfo, ret);
        } else {
            hidl_vec<SetupDataCallResult> ret;
            if ((response == NULL && responseLen != 0) ||
                 responseLen % sizeof(RIL_SetupDataCallResult_v1_4) != 0) {
                RLOGE("getDataCallListResponse: invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                convertRilDataCallListToHal(response, responseLen, ret);
            }

            retStatus = radioService[slotId]->mRadioResponse->getDataCallListResponse(
                            responseInfo, ret);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getDataCallListResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setSuppServiceNotificationsResponse(int slotId,
                                              int responseType, int serial, RIL_Errno e,
                                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSuppServiceNotificationsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setSuppServiceNotificationsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setSuppServiceNotificationsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::deleteSmsOnSimResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("deleteSmsOnSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->deleteSmsOnSimResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deleteSmsOnSimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setBandModeResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setBandModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setBandModeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setBandModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::writeSmsToSimResponse(int slotId,
                                int responseType, int serial, RIL_Errno e,
                                void *response, size_t responseLen) {
#if VDBG
    RLOGD("writeSmsToSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->writeSmsToSimResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("writeSmsToSimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getAvailableBandModesResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e, void *response,
                                        size_t responseLen) {
#if VDBG
    RLOGD("getAvailableBandModesResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<RadioBandMode> modes;
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(int) != 0) {
            RLOGE("getAvailableBandModesResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            int numInts = responseLen / sizeof(int);
            modes.resize(numInts);
            for (int i = 0; i < numInts; i++) {
                modes[i] = (RadioBandMode) pInt[i];
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getAvailableBandModesResponse(responseInfo,
                modes);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAvailableBandModesResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendEnvelopeResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendEnvelopeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendEnvelopeResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendEnvelopeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendTerminalResponseToSimResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendTerminalResponseToSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendTerminalResponseToSimResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendTerminalResponseToSimResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::handleStkCallSetupRequestFromSimResponse(int slotId,
                                                   int responseType, int serial,
                                                   RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("handleStkCallSetupRequestFromSimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->handleStkCallSetupRequestFromSimResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("handleStkCallSetupRequestFromSimResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::explicitCallTransferResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("explicitCallTransferResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->explicitCallTransferResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("explicitCallTransferResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setPreferredNetworkTypeResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredNetworkTypeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setPreferredNetworkTypeResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setPreferredNetworkTypeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}


int radio::getPreferredNetworkTypeResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredNetworkTypeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getPreferredNetworkTypeResponse(
                responseInfo, (PreferredNetworkType) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getPreferredNetworkTypeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getNeighboringCidsResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getNeighboringCidsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<NeighboringCell> cells;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_NeighboringCell *) != 0) {
            RLOGE("getNeighboringCidsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_NeighboringCell *);
            cells.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_NeighboringCell *resp = ((RIL_NeighboringCell **) response)[i];
                cells[i].cid = convertCharPtrToHidlString(resp->cid);
                cells[i].rssi = resp->rssi;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getNeighboringCidsResponse(responseInfo,
                cells);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getNeighboringCidsResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setLocationUpdatesResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("setLocationUpdatesResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setLocationUpdatesResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setLocationUpdatesResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaSubscriptionSourceResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaSubscriptionSourceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaSubscriptionSourceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaSubscriptionSourceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaRoamingPreferenceResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaRoamingPreferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaRoamingPreferenceResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaRoamingPreferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaRoamingPreferenceResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaRoamingPreferenceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaRoamingPreferenceResponse(
                responseInfo, (CdmaRoamingType) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaRoamingPreferenceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setTTYModeResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setTTYModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setTTYModeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setTTYModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getTTYModeResponse(int slotId,
                             int responseType, int serial, RIL_Errno e,
                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("getTTYModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getTTYModeResponse(responseInfo,
                (TtyMode) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getTTYModeResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setPreferredVoicePrivacyResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredVoicePrivacyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setPreferredVoicePrivacyResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setPreferredVoicePrivacyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getPreferredVoicePrivacyResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredVoicePrivacyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 1) {
            RLOGE("getPreferredVoicePrivacyResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            enable = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getPreferredVoicePrivacyResponse(
                responseInfo, enable);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getPreferredVoicePrivacyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendCDMAFeatureCodeResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendCDMAFeatureCodeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendCDMAFeatureCodeResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendCDMAFeatureCodeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendBurstDtmfResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendBurstDtmfResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendBurstDtmfResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendBurstDtmfResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::sendCdmaSmsResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("sendCdmaSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendCdmaSmsResponse(responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendCdmaSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::acknowledgeLastIncomingCdmaSmsResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeLastIncomingCdmaSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->acknowledgeLastIncomingCdmaSmsResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeLastIncomingCdmaSmsResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::getGsmBroadcastConfigResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getGsmBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<GsmBroadcastSmsConfigInfo> configs;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_GSM_BroadcastSmsConfigInfo *) != 0) {
            RLOGE("getGsmBroadcastConfigResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_GSM_BroadcastSmsConfigInfo *);
            configs.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_GSM_BroadcastSmsConfigInfo *resp =
                        ((RIL_GSM_BroadcastSmsConfigInfo **) response)[i];
                configs[i].fromServiceId = resp->fromServiceId;
                configs[i].toServiceId = resp->toServiceId;
                configs[i].fromCodeScheme = resp->fromCodeScheme;
                configs[i].toCodeScheme = resp->toCodeScheme;
                configs[i].selected = resp->selected == 1 ? true : false;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getGsmBroadcastConfigResponse(responseInfo,
                configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getGsmBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setGsmBroadcastConfigResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("setGsmBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setGsmBroadcastConfigResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setGsmBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setGsmBroadcastActivationResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("setGsmBroadcastActivationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setGsmBroadcastActivationResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setGsmBroadcastActivationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaBroadcastConfigResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e,
                                         void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<CdmaBroadcastSmsConfigInfo> configs;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_CDMA_BroadcastSmsConfigInfo *) != 0) {
            RLOGE("getCdmaBroadcastConfigResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_CDMA_BroadcastSmsConfigInfo *);
            configs.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_CDMA_BroadcastSmsConfigInfo *resp =
                        ((RIL_CDMA_BroadcastSmsConfigInfo **) response)[i];
                configs[i].serviceCategory = resp->service_category;
                configs[i].language = resp->language;
                configs[i].selected = resp->selected == 1 ? true : false;
            }
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaBroadcastConfigResponse(responseInfo,
                configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaBroadcastConfigResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e,
                                         void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaBroadcastConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaBroadcastConfigResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaBroadcastConfigResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCdmaBroadcastActivationResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setCdmaBroadcastActivationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCdmaBroadcastActivationResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCdmaBroadcastActivationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCDMASubscriptionResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("getCDMASubscriptionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        int numStrings = responseLen / sizeof(char *);
        hidl_string emptyString;
        if (response == NULL || numStrings != 5) {
            RLOGE("getOperatorResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getCDMASubscriptionResponse(
                    responseInfo, emptyString, emptyString, emptyString, emptyString, emptyString);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            char **resp = (char **) response;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getCDMASubscriptionResponse(
                    responseInfo,
                    convertCharPtrToHidlString(resp[0]),
                    convertCharPtrToHidlString(resp[1]),
                    convertCharPtrToHidlString(resp[2]),
                    convertCharPtrToHidlString(resp[3]),
                    convertCharPtrToHidlString(resp[4]));
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getCDMASubscriptionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::writeSmsToRuimResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("writeSmsToRuimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->writeSmsToRuimResponse(responseInfo, ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("writeSmsToRuimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::deleteSmsOnRuimResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("deleteSmsOnRuimResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->deleteSmsOnRuimResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("deleteSmsOnRuimResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getDeviceIdentityResponse(int slotId,
                                    int responseType, int serial, RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getDeviceIdentityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        int numStrings = responseLen / sizeof(char *);
        hidl_string emptyString;
        if (response == NULL || numStrings != 4) {
            RLOGE("getDeviceIdentityResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getDeviceIdentityResponse(responseInfo,
                    emptyString, emptyString, emptyString, emptyString);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            char **resp = (char **) response;
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->getDeviceIdentityResponse(responseInfo,
                    convertCharPtrToHidlString(resp[0]),
                    convertCharPtrToHidlString(resp[1]),
                    convertCharPtrToHidlString(resp[2]),
                    convertCharPtrToHidlString(resp[3]));
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("getDeviceIdentityResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::exitEmergencyCallbackModeResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("exitEmergencyCallbackModeResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->exitEmergencyCallbackModeResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("exitEmergencyCallbackModeResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSmscAddressResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("getSmscAddressResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getSmscAddressResponse(responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getSmscAddressResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setSmscAddressResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSmscAddressResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setSmscAddressResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setSmscAddressResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::reportSmsMemoryStatusResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("reportSmsMemoryStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->reportSmsMemoryStatusResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("reportSmsMemoryStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::reportStkServiceIsRunningResponse(int slotId,
                                             int responseType, int serial, RIL_Errno e,
                                             void *response, size_t responseLen) {
#if VDBG
    RLOGD("reportStkServiceIsRunningResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->
                reportStkServiceIsRunningResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("reportStkServiceIsRunningResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCdmaSubscriptionSourceResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCdmaSubscriptionSourceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getCdmaSubscriptionSourceResponse(
                responseInfo, (CdmaSubscriptionSource) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCdmaSubscriptionSourceResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::requestIsimAuthenticationResponse(int slotId,
                                            int responseType, int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestIsimAuthenticationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestIsimAuthenticationResponse(
                responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestIsimAuthenticationResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::acknowledgeIncomingGsmSmsWithPduResponse(int slotId,
                                                   int responseType,
                                                   int serial, RIL_Errno e, void *response,
                                                   size_t responseLen) {
#if VDBG
    RLOGD("acknowledgeIncomingGsmSmsWithPduResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->acknowledgeIncomingGsmSmsWithPduResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("acknowledgeIncomingGsmSmsWithPduResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::sendEnvelopeWithStatusResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e, void *response,
                                         size_t responseLen) {
#if VDBG
    RLOGD("sendEnvelopeWithStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendEnvelopeWithStatusResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendEnvelopeWithStatusResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getVoiceRadioTechnologyResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVoiceRadioTechnologyResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getVoiceRadioTechnologyResponse(
                responseInfo, (RadioTechnology) ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getVoiceRadioTechnologyResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCellInfoListResponse(int slotId,
                                   int responseType,
                                   int serial, RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("getCellInfoListResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_2 != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if ((response == NULL && responseLen != 0) ||
             responseLen % sizeof(RIL_CellInfo_v1_4) != 0) {
            RLOGE("getCellInfoListResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
                hidl_vec<V1_4::CellInfo> ret;
                retStatus = radioService[slotId]->mRadioResponseV1_4->
                        getCellInfoListResponse_1_4(responseInfo, ret);
            } else if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
                hidl_vec<V1_2::CellInfo> ret;
                retStatus = radioService[slotId]->mRadioResponseV1_2->
                        getCellInfoListResponse_1_2(responseInfo, ret);
            } else {
                hidl_vec<CellInfo> ret;
                retStatus = radioService[slotId]->mRadioResponse->
                        getCellInfoListResponse(responseInfo, ret);
            }
        } else {
            if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
                hidl_vec<V1_4::CellInfo> ret;
                convertRilCellInfoListToHal_1_4(response, responseLen, ret);
                retStatus = radioService[slotId]->mRadioResponseV1_4->
                        getCellInfoListResponse_1_4(responseInfo, ret);
            } else if (radioService[slotId]->mRadioResponseV1_2 != NULL) {
                hidl_vec<V1_2::CellInfo> ret;
                convertRilCellInfoListToHal_1_2(response, responseLen, ret);
                retStatus = radioService[slotId]->mRadioResponseV1_2->
                        getCellInfoListResponse_1_2(responseInfo, ret);
            } else {
                hidl_vec<CellInfo> ret;
                convertRilCellInfoListToHal(response, responseLen, ret);
                retStatus = radioService[slotId]->mRadioResponse->
                        getCellInfoListResponse(responseInfo, ret);
            }
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getCellInfoListResponse: radioService[%d]->mRadioResponse == NULL || "
                "radioService[%d]->mRadioResponseV1_2 != NULL ||"
                "radioService[%d]->mRadioResponseV1_4 != NULL", slotId, slotId, slotId);
    }

    return 0;
}

int radio::setCellInfoListRateResponse(int slotId,
                                       int responseType,
                                       int serial, RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("setCellInfoListRateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setCellInfoListRateResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCellInfoListRateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setInitialAttachApnResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("setInitialAttachApnResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setInitialAttachApnResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setInitialAttachApnResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getImsRegistrationStateResponse(int slotId,
                                           int responseType, int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("getImsRegistrationStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool isRegistered = false;
        int ratFamily = 0;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 2) {
            RLOGE("getImsRegistrationStateResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *) response;
            isRegistered = pInt[0] == 1 ? true : false;
            ratFamily = pInt[1];
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getImsRegistrationStateResponse(
                responseInfo, isRegistered, (RadioTechnologyFamily) ratFamily);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getImsRegistrationStateResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendImsSmsResponse(int slotId,
                              int responseType, int serial, RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("sendImsSmsResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        SendSmsResult result = makeSendSmsResult(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendImsSmsResponse(responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendSmsResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::iccTransmitApduBasicChannelResponse(int slotId,
                                               int responseType, int serial, RIL_Errno e,
                                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccTransmitApduBasicChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccTransmitApduBasicChannelResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccTransmitApduBasicChannelResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::iccOpenLogicalChannelResponse(int slotId,
                                         int responseType, int serial, RIL_Errno e, void *response,
                                         size_t responseLen) {
#if VDBG
    RLOGD("iccOpenLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        int channelId = -1;
        hidl_vec<int8_t> selectResponse;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("iccOpenLogicalChannelResponse Invalid response: NULL");
            if (response != NULL) {
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            }
        } else {
            int *pInt = (int *) response;
            channelId = pInt[0];
            selectResponse.resize(numInts - 1);
            for (int i = 1; i < numInts; i++) {
                selectResponse[i - 1] = (int8_t) pInt[i];
            }
        }
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccOpenLogicalChannelResponse(responseInfo,
                channelId, selectResponse);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccOpenLogicalChannelResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::iccCloseLogicalChannelResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccCloseLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccCloseLogicalChannelResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccCloseLogicalChannelResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::iccTransmitApduLogicalChannelResponse(int slotId,
                                                 int responseType, int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("iccTransmitApduLogicalChannelResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->iccTransmitApduLogicalChannelResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("iccTransmitApduLogicalChannelResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::nvReadItemResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvReadItemResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->nvReadItemResponse(
                responseInfo,
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvReadItemResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvWriteItemResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvWriteItemResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvWriteItemResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvWriteItemResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvWriteCdmaPrlResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvWriteCdmaPrlResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvWriteCdmaPrlResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvWriteCdmaPrlResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::nvResetConfigResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("nvResetConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->nvResetConfigResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nvResetConfigResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setUiccSubscriptionResponse(int slotId,
                                       int responseType, int serial, RIL_Errno e,
                                       void *response, size_t responseLen) {
#if VDBG
    RLOGD("setUiccSubscriptionResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setUiccSubscriptionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setUiccSubscriptionResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setDataAllowedResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("setDataAllowedResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setDataAllowedResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setDataAllowedResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getHardwareConfigResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getHardwareConfigResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        hidl_vec<HardwareConfig> result;
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_HardwareConfig) != 0) {
            RLOGE("hardwareConfigChangedInd: invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            convertRilHardwareConfigListToHal(response, responseLen, result);
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->getHardwareConfigResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getHardwareConfigResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::requestIccSimAuthenticationResponse(int slotId,
                                               int responseType, int serial, RIL_Errno e,
                                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestIccSimAuthenticationResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        IccIoResult result = responseIccIo(responseInfo, serial, responseType, e, response,
                responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestIccSimAuthenticationResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestIccSimAuthenticationResponse: radioService[%d]->mRadioResponse "
                "== NULL", slotId);
    }

    return 0;
}

int radio::setDataProfileResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("setDataProfileResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setDataProfileResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setDataProfileResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::requestShutdownResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestShutdownResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->requestShutdownResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("requestShutdownResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

void responseRadioCapability(RadioResponseInfo& responseInfo, int serial,
        int responseType, RIL_Errno e, void *response, size_t responseLen, RadioCapability& rc) {
    populateResponseInfo(responseInfo, serial, responseType, e);

    if (response == NULL || responseLen != sizeof(RIL_RadioCapability)) {
        RLOGE("responseRadioCapability: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        rc.logicalModemUuid = hidl_string();
    } else {
        convertRilRadioCapabilityToHal(response, responseLen, rc);
    }
}

int radio::getRadioCapabilityResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("getRadioCapabilityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        RadioCapability result = {};
        responseRadioCapability(responseInfo, serial, responseType, e, response, responseLen,
                result);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->getRadioCapabilityResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getRadioCapabilityResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setRadioCapabilityResponse(int slotId,
                                     int responseType, int serial, RIL_Errno e,
                                     void *response, size_t responseLen) {
#if VDBG
    RLOGD("setRadioCapabilityResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        RadioCapability result = {};
        responseRadioCapability(responseInfo, serial, responseType, e, response, responseLen,
                result);
        Return<void> retStatus = radioService[slotId]->mRadioResponse->setRadioCapabilityResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setRadioCapabilityResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

LceStatusInfo responseLceStatusInfo(RadioResponseInfo& responseInfo, int serial, int responseType,
                                    RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfo(responseInfo, serial, responseType, e);
    LceStatusInfo result = {};

    if (response == NULL || responseLen != sizeof(RIL_LceStatusInfo)) {
        RLOGE("Invalid response: NULL");
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        RIL_LceStatusInfo *resp = (RIL_LceStatusInfo *) response;
        result.lceStatus = (LceStatus) resp->lce_status;
        result.actualIntervalMs = (uint8_t) resp->actual_interval_ms;
    }
    return result;
}

int radio::startLceServiceResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("startLceServiceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        LceStatusInfo result = responseLceStatusInfo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->startLceServiceResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startLceServiceResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::stopLceServiceResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopLceServiceResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        LceStatusInfo result = responseLceStatusInfo(responseInfo, serial, responseType, e,
                response, responseLen);

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->stopLceServiceResponse(responseInfo,
                result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopLceServiceResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::pullLceDataResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("pullLceDataResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        LceDataInfo result = {};
        if (response == NULL || responseLen != sizeof(RIL_LceDataInfo)) {
            RLOGE("pullLceDataResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            convertRilLceDataInfoToHal(response, responseLen, result);
        }

        Return<void> retStatus = radioService[slotId]->mRadioResponse->pullLceDataResponse(
                responseInfo, result);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("pullLceDataResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getModemActivityInfoResponse(int slotId,
                                        int responseType, int serial, RIL_Errno e,
                                        void *response, size_t responseLen) {
#if VDBG
    RLOGD("getModemActivityInfoResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        ActivityStatsInfo info;
        if (response == NULL || responseLen != sizeof(RIL_ActivityStatsInfo)) {
            RLOGE("getModemActivityInfoResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            RIL_ActivityStatsInfo *resp = (RIL_ActivityStatsInfo *)response;
            info.sleepModeTimeMs = resp->sleep_mode_time_ms;
            info.idleModeTimeMs = resp->idle_mode_time_ms;
            for(int i = 0; i < RIL_NUM_TX_POWER_LEVELS; i++) {
                info.txmModetimeMs[i] = resp->tx_mode_time_ms[i];
            }
            info.rxModeTimeMs = resp->rx_mode_time_ms;
        }

        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->getModemActivityInfoResponse(responseInfo,
                info);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getModemActivityInfoResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setAllowedCarriersResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("setAllowedCarriersResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            retStatus = radioService[slotId]->mRadioResponseV1_4->setAllowedCarriersResponse_1_4(responseInfo);
        } else {
            retStatus = radioService[slotId]->mRadioResponse->setAllowedCarriersResponse(responseInfo,
                ret);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setAllowedCarriersResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getAllowedCarriersResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("getAllowedCarriersResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL ||
        radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus;

        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            V1_4::CarrierRestrictionsWithPriority carrierInfo = {};
            V1_4::SimLockMultiSimPolicy multiSimPolicy = V1_4::SimLockMultiSimPolicy::NO_MULTISIM_POLICY;

            if (response == NULL) {
#if VDBG
                RLOGD("getAllowedCarriersResponse_1_4 response is NULL: all allowed");
#endif
                carrierInfo.allowedCarriers.resize(0);
                carrierInfo.excludedCarriers.resize(0);
                carrierInfo.allowedCarriersPrioritized = false;
            } else {
                RIL_CarrierRestrictions_v1_4 *pCr = (RIL_CarrierRestrictions_v1_4 *)response;

                carrierInfo.allowedCarriersPrioritized = pCr->allowedCarriersPrioritized;
                multiSimPolicy = (V1_4::SimLockMultiSimPolicy)pCr->simPolicy;

                carrierInfo.allowedCarriers.resize(pCr->allowedCarriersNumber);
                for (uint32_t i = 0; i < pCr->allowedCarriersNumber; i++) {
                    RIL_Carrier *carrier = pCr->allowedCarriers + i;
                    carrierInfo.allowedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                    carrierInfo.allowedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                    carrierInfo.allowedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                    carrierInfo.allowedCarriers[i].matchData =
                            convertCharPtrToHidlString(carrier->match_data);
                }

                carrierInfo.excludedCarriers.resize(pCr->excludedCarriersNumber);
                for(uint32_t i = 0; i < pCr->excludedCarriersNumber; i++) {
                    RIL_Carrier *carrier = pCr->excludedCarriers + i;
                    carrierInfo.excludedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                    carrierInfo.excludedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                    carrierInfo.excludedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                    carrierInfo.excludedCarriers[i].matchData =
                            convertCharPtrToHidlString(carrier->match_data);
                }
            }

            retStatus = radioService[slotId]->mRadioResponseV1_4->getAllowedCarriersResponse_1_4(responseInfo,
                    carrierInfo, multiSimPolicy);
        } else {
            CarrierRestrictions carrierInfo = {};
            bool allAllowed = true;
            if (response == NULL) {
#if VDBG
                RLOGD("getAllowedCarriersResponse response is NULL: all allowed");
#endif
                carrierInfo.allowedCarriers.resize(0);
                carrierInfo.excludedCarriers.resize(0);
            } else if (responseLen != sizeof(RIL_CarrierRestrictions_v1_4)) {
                RLOGE("getAllowedCarriersResponse Invalid response");
                if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
            } else {
                RIL_CarrierRestrictions_v1_4 *pCr = (RIL_CarrierRestrictions_v1_4 *)response;
                if (pCr->allowedCarriersNumber > 0 || pCr->excludedCarriersNumber > 0) {
                    allAllowed = false;
                }

                carrierInfo.allowedCarriers.resize(pCr->allowedCarriersNumber);
                for(uint32_t i = 0; i < pCr->allowedCarriersNumber; i++) {
                    RIL_Carrier *carrier = pCr->allowedCarriers + i;
                    carrierInfo.allowedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                    carrierInfo.allowedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                    carrierInfo.allowedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                    carrierInfo.allowedCarriers[i].matchData =
                            convertCharPtrToHidlString(carrier->match_data);
                }

                carrierInfo.excludedCarriers.resize(pCr->excludedCarriersNumber);
                for(uint32_t i = 0; i < pCr->excludedCarriersNumber; i++) {
                    RIL_Carrier *carrier = pCr->excludedCarriers + i;
                    carrierInfo.excludedCarriers[i].mcc = convertCharPtrToHidlString(carrier->mcc);
                    carrierInfo.excludedCarriers[i].mnc = convertCharPtrToHidlString(carrier->mnc);
                    carrierInfo.excludedCarriers[i].matchType = (CarrierMatchType) carrier->match_type;
                    carrierInfo.excludedCarriers[i].matchData =
                            convertCharPtrToHidlString(carrier->match_data);
                }
            }

            retStatus = radioService[slotId]->mRadioResponse->getAllowedCarriersResponse(responseInfo,
                    allAllowed, carrierInfo);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getAllowedCarriersResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendDeviceStateResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendDeviceStateResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->sendDeviceStateResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("sendDeviceStateResponse: radioService[%d]->mRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setCarrierInfoForImsiEncryptionResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
    RLOGD("setCarrierInfoForImsiEncryptionResponse: serial %d", serial);
    if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_1->
                setCarrierInfoForImsiEncryptionResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setCarrierInfoForImsiEncryptionResponse: radioService[%d]->mRadioResponseV1_1 == "
                "NULL", slotId);
    }
    return 0;
}

int radio::setIndicationFilterResponse(int slotId,
                              int responseType, int serial, RIL_Errno e,
                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setIndicationFilterResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponse->setIndicationFilterResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setIndicationFilterResponse: radioService[%d]->mRadioResponse == NULL",
                slotId);
    }

    return 0;
}


int radio::setSimCardPowerResponse(int slotId,
                                   int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSimCardPowerResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponse != NULL
            || radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
            Return<void> retStatus = radioService[slotId]->mRadioResponseV1_1->
                    setSimCardPowerResponse_1_1(responseInfo);
            radioService[slotId]->checkReturnStatus(retStatus);
        } else {
            RLOGD("setSimCardPowerResponse: radioService[%d]->mRadioResponseV1_1 == NULL",
                    slotId);
            Return<void> retStatus
                    = radioService[slotId]->mRadioResponse->setSimCardPowerResponse(responseInfo);
            radioService[slotId]->checkReturnStatus(retStatus);
        }
    } else {
        RLOGE("setSimCardPowerResponse: radioService[%d]->mRadioResponse == NULL && "
                "radioService[%d]->mRadioResponseV1_1 == NULL", slotId, slotId);
    }
    return 0;
}

int radio::startNetworkScanResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("startNetworkScanResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_1 != NULL ||
            radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus;
        if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
            retStatus = radioService[slotId]->mRadioResponseV1_4->
                    startNetworkScanResponse_1_4(responseInfo);
        } else {
            retStatus = radioService[slotId]->mRadioResponseV1_1->
                    startNetworkScanResponse(responseInfo);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("startNetworkScanResponse: radioService[%d]->mRadioResponseV1_1 == NULL or "
                "radioService[%d]->mRadioResponseV1_4 == NULL", slotId, slotId);
    }

    return 0;
}

int radio::stopNetworkScanResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                   void *response, size_t responseLen) {
#if VDBG
    RLOGD("stopNetworkScanResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_1 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponseV1_1->stopNetworkScanResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stopNetworkScanResponse: radioService[%d]->mRadioResponseV1_1 == NULL", slotId);
    }

    return 0;
}

void convertRilKeepaliveStatusToHal(const RIL_KeepaliveStatus *rilStatus,
        V1_1::KeepaliveStatus& halStatus) {
    halStatus.sessionHandle = rilStatus->sessionHandle;
    halStatus.code = static_cast<V1_1::KeepaliveStatusCode>(rilStatus->code);
}

int radio::startKeepaliveResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    // If we don't have a radio service, there's nothing we can do
    if (radioService[slotId]->mRadioResponseV1_1 == NULL) {
        RLOGE("%s: radioService[%d]->mRadioResponseV1_1 == NULL", __FUNCTION__, slotId);
        return 0;
    }

    V1_1::KeepaliveStatus ks = {};
    if (response == NULL || responseLen != sizeof(V1_1::KeepaliveStatus)) {
        RLOGE("%s: invalid response - %d", __FUNCTION__, static_cast<int>(e));
        if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
    } else {
        convertRilKeepaliveStatusToHal(static_cast<RIL_KeepaliveStatus*>(response), ks);
    }

    Return<void> retStatus =
            radioService[slotId]->mRadioResponseV1_1->startKeepaliveResponse(responseInfo, ks);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::stopKeepaliveResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                    void *response, size_t responseLen) {
#if VDBG
    RLOGD("%s(): %d", __FUNCTION__, serial);
#endif
    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    // If we don't have a radio service, there's nothing we can do
    if (radioService[slotId]->mRadioResponseV1_1 == NULL) {
        RLOGE("%s: radioService[%d]->mRadioResponseV1_1 == NULL", __FUNCTION__, slotId);
        return 0;
    }

    Return<void> retStatus =
            radioService[slotId]->mRadioResponseV1_1->stopKeepaliveResponse(responseInfo);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::setSignalStrengthReportingCriteriaResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response,
        size_t responseLen) {
#if VDBG
    RLOGD("setSignalStrengthReportingCriteriaResponse: %d", serial);
#endif
    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    if (radioService[slotId]->mRadioResponseV1_2 == NULL) {
        RLOGE("radioService[%d]->mRadioResponseV1_2 == NULL", slotId);
        return 0;
    }

    Return<void> retStatus = radioService[slotId]->mRadioResponseV1_2->
            setSignalStrengthReportingCriteriaResponse(responseInfo);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::setLinkCapacityReportingCriteriaResponse(int slotId,
        int responseType, int serial, RIL_Errno e, void *response,
        size_t responseLen) {
#if VDBG
    RLOGD("setLinkCapacityReportingCriteriaResponse: %d", serial);
#endif

    RadioResponseInfo responseInfo = {};
    populateResponseInfo(responseInfo, serial, responseType, e);

    if (radioService[slotId]->mRadioResponseV1_2 == NULL) {
        RLOGE("radioService[%d]->mRadioResponseV1_2 == NULL", slotId);
        return 0;
    }

    Return<void> retStatus = radioService[slotId]->mRadioResponseV1_2->
            setLinkCapacityReportingCriteriaResponse(responseInfo);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::setSystemSelectionChannelsResponse(int slotId, int responseType,
                                              int serial, RIL_Errno e,
                                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSystemSelectionChannelsResponse: %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_3 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_3->
                setSystemSelectionChannelsResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioService[%d]->mRadioResponseV1_3 == NULL", slotId);
    }

    return 0;
}

int radio::enableModemResponse(int slotId, int responseType, int serial,
                               RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("enableModemResponse: %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_3 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_3->
                enableModemResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioService[%d]->mRadioResponseV1_3 == NULL", slotId);
    }

    return 0;
}

int radio::getModemStackStatusResponse(int slotId, int responseType, int serial,
                                       RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getModemStackStatusResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_3 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        bool enable = false;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 1) {
            RLOGE("getModemStackStatusResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *)response;
            enable = pInt[0] == 1 ? true : false;
        }
        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_3->
                getModemStackStatusResponse(responseInfo, enable);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getModemStackStatusResponse: radioService[%d]->mRadioResponseV1_3 == NULL",
                slotId);
    }

    return 0;
}

int radio::emergencyDialResponse(int slotId, int responseType, int serial,
                                 RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("emergencyDialResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);

        Return<void> retStatus = radioService[slotId]->mRadioResponseV1_4->
                emergencyDialResponse(responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("emergencyDialResponse: radioService[%d]->mRadioResponseV1_4 == NULL",
                slotId);
    }

    return 0;
}

int radio::getPreferredNetworkTypeBitmapResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredNetworkTypeBitmapResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        int ret = responseInt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponseV1_4->getPreferredNetworkTypeBitmapResponse(
                responseInfo, (const hidl_bitfield<V1_4::RadioAccessFamily>)ret);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("getPreferredNetworkTypeBitmapResponse: radioService[%d]->mRadioResponseV1_4 == NULL",
                slotId);
    }

    return 0;
}

int radio::setPreferredNetworkTypeBitmapResponse(int slotId,
                                 int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredNetworkTypeBitmapResponse: serial %d", serial);
#endif

    if (radioService[slotId]->mRadioResponseV1_4 != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = radioService[slotId]->mRadioResponseV1_4->setPreferredNetworkTypeBitmapResponse(
                responseInfo);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("setPreferredNetworkTypeBitmapResponse: radioService[%d]->mRadioResponseV1_4 == NULL",
                slotId);
    }

    return 0;
}
int radio::sendRequestRawResponse(int slotId,
                                  int responseType, int serial, RIL_Errno e,
                                  void *response, size_t responseLen) {
#if VDBG
   RLOGD("sendRequestRawResponse: serial %d", serial);
#endif
    if (!kOemHookEnabled) return 0;

    if (oemHookService[slotId]->mOemHookResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<uint8_t> data;

        if (response == NULL) {
            RLOGE("sendRequestRawResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            data.setToExternal((uint8_t *) response, responseLen);
        }
        Return<void> retStatus = oemHookService[slotId]->mOemHookResponse->
                sendRequestRawResponse(responseInfo, data);
        checkReturnStatus(slotId, retStatus, OEM_SERVICE);
    } else {
        RLOGE("sendRequestRawResponse: oemHookService[%d]->mOemHookResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendRequestStringsResponse(int slotId,
                                      int responseType, int serial, RIL_Errno e,
                                      void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendRequestStringsResponse: serial %d", serial);
#endif
    if (!kOemHookEnabled) return 0;
    if (oemHookService[slotId]->mOemHookResponse != NULL) {
        RadioResponseInfo responseInfo = {};
        populateResponseInfo(responseInfo, serial, responseType, e);
        hidl_vec<hidl_string> data;

        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(char *) != 0) {
            RLOGE("sendRequestStringsResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = RadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **) response;
            int numStrings = responseLen / sizeof(char *);
            data.resize(numStrings);
            for (int i = 0; i < numStrings; i++) {
                data[i] = convertCharPtrToHidlString(resp[i]);
            }
        }
        Return<void> retStatus
                = oemHookService[slotId]->mOemHookResponse->sendRequestStringsResponse(
                responseInfo, data);
        checkReturnStatus(slotId, retStatus, OEM_SERVICE);
    } else {
        RLOGE("sendRequestStringsResponse: oemHookService[%d]->mOemHookResponse == "
                "NULL", slotId);
    }

    return 0;
}

/***************************************************************************************************
 * INDICATION FUNCTIONS
 * The below function handle unsolicited messages coming from the Radio
 * (messages for which there is no pending request)
 **************************************************************************************************/

RadioIndicationType convertIntToRadioIndicationType(int indicationType) {
    return indicationType == RESPONSE_UNSOLICITED ? (RadioIndicationType::UNSOLICITED) :
            (RadioIndicationType::UNSOLICITED_ACK_EXP);
}

int radio::radioStateChangedInd(int slotId,
                                 int indicationType, int token, RIL_Errno e, void *response,
                                 size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        RadioState radioState =
                (RadioState) CALL_ONSTATEREQUEST((RIL_SOCKET_ID)radioService[slotId]->mSlotId);
        RLOGD("radioStateChangedInd: radioState %d", radioState);
        Return<void> retStatus = radioService[slotId]->mRadioIndication->radioStateChanged(
                convertIntToRadioIndicationType(indicationType), radioState);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioStateChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::callStateChangedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("callStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->callStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("callStateChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::networkStateChangedInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("networkStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->networkStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("networkStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

uint8_t hexCharToInt(uint8_t c) {
    if (c >= '0' && c <= '9') return (c - '0');
    if (c >= 'A' && c <= 'F') return (c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (c - 'a' + 10);

    return INVALID_HEX_CHAR;
}

uint8_t * convertHexStringToBytes(void *response, size_t responseLen) {
    if (response == NULL || responseLen % 2 != 0) {
        RLOGE("convertHexStringToBytes: invalid response");
        return NULL;
    }

    uint8_t *bytes = (uint8_t *)calloc(responseLen/2, sizeof(uint8_t));
    if (bytes == NULL) {
        RLOGE("convertHexStringToBytes: cannot allocate memory for bytes string");
        return NULL;
    }
    uint8_t *hexString = (uint8_t *)response;

    for (size_t i = 0; i < responseLen; i += 2) {
        uint8_t hexChar1 = hexCharToInt(hexString[i]);
        uint8_t hexChar2 = hexCharToInt(hexString[i + 1]);

        if (hexChar1 == INVALID_HEX_CHAR || hexChar2 == INVALID_HEX_CHAR) {
            RLOGE("convertHexStringToBytes: invalid hex char %d %d",
                    hexString[i], hexString[i + 1]);
            free(bytes);
            return NULL;
        }
        bytes[i/2] = ((hexChar1 << 4) | hexChar2);
    }

    return bytes;
}

int radio::newSmsInd(int slotId, int indicationType,
                     int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newSmsInd: invalid response");
            return 0;
        }

        uint8_t *bytes = convertHexStringToBytes(response, responseLen);
        if (bytes == NULL) {
            RLOGE("newSmsInd: convertHexStringToBytes failed");
            return 0;
        }

        hidl_vec<uint8_t> pdu;
        pdu.setToExternal(bytes, responseLen/2);
#if VDBG
        RLOGD("newSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSms(
                convertIntToRadioIndicationType(indicationType), pdu);
        radioService[slotId]->checkReturnStatus(retStatus);
        free(bytes);
    } else {
        RLOGE("newSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newSmsStatusReportInd(int slotId,
                                 int indicationType, int token, RIL_Errno e, void *response,
                                 size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newSmsStatusReportInd: invalid response");
            return 0;
        }

        uint8_t *bytes = convertHexStringToBytes(response, responseLen);
        if (bytes == NULL) {
            RLOGE("newSmsStatusReportInd: convertHexStringToBytes failed");
            return 0;
        }

        hidl_vec<uint8_t> pdu;
        pdu.setToExternal(bytes, responseLen/2);
#if VDBG
        RLOGD("newSmsStatusReportInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSmsStatusReport(
                convertIntToRadioIndicationType(indicationType), pdu);
        radioService[slotId]->checkReturnStatus(retStatus);
        free(bytes);
    } else {
        RLOGE("newSmsStatusReportInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newSmsOnSimInd(int slotId, int indicationType,
                          int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("newSmsOnSimInd: invalid response");
            return 0;
        }
        int32_t recordNumber = ((int32_t *) response)[0];
#if VDBG
        RLOGD("newSmsOnSimInd: slotIndex %d", recordNumber);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newSmsOnSim(
                convertIntToRadioIndicationType(indicationType), recordNumber);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("newSmsOnSimInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::onUssdInd(int slotId, int indicationType,
                     int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != 2 * sizeof(char *)) {
            RLOGE("onUssdInd: invalid response");
            return 0;
        }
        char **strings = (char **) response;
        char *mode = strings[0];
        hidl_string msg = convertCharPtrToHidlString(strings[1]);
        UssdModeType modeType = (UssdModeType) atoi(mode);
#if VDBG
        RLOGD("onUssdInd: mode %s", mode);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->onUssd(
                convertIntToRadioIndicationType(indicationType), modeType, msg);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("onUssdInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::nitzTimeReceivedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("nitzTimeReceivedInd: invalid response");
            return 0;
        }
        hidl_string nitzTime = convertCharPtrToHidlString((char *) response);
#if VDBG
        RLOGD("nitzTimeReceivedInd: nitzTime %s receivedTime %" PRId64, nitzTime.c_str(),
                nitzTimeReceived[slotId]);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->nitzTimeReceived(
                convertIntToRadioIndicationType(indicationType), nitzTime,
                nitzTimeReceived[slotId]);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("nitzTimeReceivedInd: radioService[%d]->mRadioIndication == NULL", slotId);
        return -1;
    }

    return 0;
}

void convertRilSignalStrengthToHal(void *response, size_t responseLen,
        SignalStrength& signalStrength) {
    RIL_SignalStrength_v1_4 *rilSignalStrength = (RIL_SignalStrength_v1_4 *)response;

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->lte.signalStrength == -1) {
        rilSignalStrength->lte.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->lte.rsrp == -1) {
        rilSignalStrength->lte.rsrp = INT_MAX;
    } else if (rilSignalStrength->lte.rsrp < -1) {
        rilSignalStrength->lte.rsrp = -rilSignalStrength->lte.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->lte.rsrq == -1) {
        rilSignalStrength->lte.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->lte.cqi == -1) {
        rilSignalStrength->lte.cqi = INT_MAX;
    }

    signalStrength.gw.signalStrength = rilSignalStrength->gsm.signalStrength;
    signalStrength.gw.bitErrorRate = rilSignalStrength->gsm.bitErrorRate;
    // RIL_SignalStrength_v10 not support gw.timingAdvance. Set to INT_MAX as
    // invalid value.
    signalStrength.gw.timingAdvance = INT_MAX;

    signalStrength.cdma.dbm = rilSignalStrength->cdma.dbm;
    signalStrength.cdma.ecio = rilSignalStrength->cdma.ecio;
    signalStrength.evdo.dbm = rilSignalStrength->evdo.dbm;
    signalStrength.evdo.ecio = rilSignalStrength->evdo.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->evdo.signalNoiseRatio;
    signalStrength.lte.signalStrength = rilSignalStrength->lte.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->lte.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->lte.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->lte.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->lte.cqi;
    signalStrength.lte.timingAdvance = rilSignalStrength->lte.timingAdvance;
    signalStrength.tdScdma.rscp = rilSignalStrength->tdscdma.rscp;
}

void convertRilSignalStrengthToHal_1_2(void *response, size_t responseLen,
        V1_2::SignalStrength& signalStrength) {
    RIL_SignalStrength_v1_4 *rilSignalStrength = (RIL_SignalStrength_v1_4 *)response;

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->lte.signalStrength == -1) {
        rilSignalStrength->lte.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->lte.rsrp == -1) {
        rilSignalStrength->lte.rsrp = INT_MAX;
    } else if (rilSignalStrength->lte.rsrp < -1) {
        rilSignalStrength->lte.rsrp = -rilSignalStrength->lte.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->lte.rsrq == -1) {
        rilSignalStrength->lte.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->lte.cqi == -1) {
        rilSignalStrength->lte.cqi = INT_MAX;
    }

    signalStrength.gsm.signalStrength = rilSignalStrength->gsm.signalStrength;
    signalStrength.gsm.bitErrorRate = rilSignalStrength->gsm.bitErrorRate;
    signalStrength.gsm.timingAdvance = rilSignalStrength->gsm.timingAdvance;

    signalStrength.cdma.dbm = rilSignalStrength->cdma.dbm;
    signalStrength.cdma.ecio = rilSignalStrength->cdma.ecio;

    signalStrength.evdo.dbm = rilSignalStrength->evdo.dbm;
    signalStrength.evdo.ecio = rilSignalStrength->evdo.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->evdo.signalNoiseRatio;

    signalStrength.lte.signalStrength = rilSignalStrength->lte.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->lte.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->lte.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->lte.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->lte.cqi;
    signalStrength.lte.timingAdvance = rilSignalStrength->lte.timingAdvance;

//    signalStrength.tdScdma.signalStrength = rilSignalStrength->tdScdma.signalStrength;
//    signalStrength.tdScdma.bitErrorRate = rilSignalStrength->tdScdma.bitErrorRate;
    signalStrength.tdScdma.rscp = rilSignalStrength->tdscdma.rscp;

    signalStrength.wcdma.base.signalStrength = rilSignalStrength->wcdma.signalStrength;
    signalStrength.wcdma.base.bitErrorRate = rilSignalStrength->wcdma.bitErrorRate;
    signalStrength.wcdma.rscp = rilSignalStrength->wcdma.rscp;
    signalStrength.wcdma.ecno = rilSignalStrength->wcdma.ecno;
}

void convertRilSignalStrengthToHal_1_4(void *response, size_t responseLen,
        V1_4::SignalStrength& signalStrength) {
    RIL_SignalStrength_v1_4 *rilSignalStrength = (RIL_SignalStrength_v1_4 *)response;

    // Fixup LTE for backwards compatibility
    // signalStrength: -1 -> 99
    if (rilSignalStrength->lte.signalStrength == -1) {
        rilSignalStrength->lte.signalStrength = 99;
    }
    // rsrp: -1 -> INT_MAX all other negative value to positive.
    // So remap here
    if (rilSignalStrength->lte.rsrp == -1) {
        rilSignalStrength->lte.rsrp = INT_MAX;
    } else if (rilSignalStrength->lte.rsrp < -1) {
        rilSignalStrength->lte.rsrp = -rilSignalStrength->lte.rsrp;
    }
    // rsrq: -1 -> INT_MAX
    if (rilSignalStrength->lte.rsrq == -1) {
        rilSignalStrength->lte.rsrq = INT_MAX;
    }
    // Not remapping rssnr is already using INT_MAX
    // cqi: -1 -> INT_MAX
    if (rilSignalStrength->lte.cqi == -1) {
        rilSignalStrength->lte.cqi = INT_MAX;
    }

    signalStrength.gsm.signalStrength = rilSignalStrength->gsm.signalStrength;
    signalStrength.gsm.bitErrorRate = rilSignalStrength->gsm.bitErrorRate;
    signalStrength.gsm.timingAdvance = rilSignalStrength->gsm.timingAdvance;

    signalStrength.cdma.dbm = rilSignalStrength->cdma.dbm;
    signalStrength.cdma.ecio = rilSignalStrength->cdma.ecio;

    signalStrength.evdo.dbm = rilSignalStrength->evdo.dbm;
    signalStrength.evdo.ecio = rilSignalStrength->evdo.ecio;
    signalStrength.evdo.signalNoiseRatio =
            rilSignalStrength->evdo.signalNoiseRatio;

    signalStrength.lte.signalStrength = rilSignalStrength->lte.signalStrength;
    signalStrength.lte.rsrp = rilSignalStrength->lte.rsrp;
    signalStrength.lte.rsrq = rilSignalStrength->lte.rsrq;
    signalStrength.lte.rssnr = rilSignalStrength->lte.rssnr;
    signalStrength.lte.cqi = rilSignalStrength->lte.cqi;
    signalStrength.lte.timingAdvance = rilSignalStrength->lte.timingAdvance;

    signalStrength.tdscdma.signalStrength = rilSignalStrength->tdscdma.signalStrength;
    signalStrength.tdscdma.bitErrorRate = rilSignalStrength->tdscdma.bitErrorRate;
    signalStrength.tdscdma.rscp = rilSignalStrength->tdscdma.rscp;

    signalStrength.wcdma.base.signalStrength = rilSignalStrength->wcdma.signalStrength;
    signalStrength.wcdma.base.bitErrorRate = rilSignalStrength->wcdma.bitErrorRate;
    signalStrength.wcdma.rscp = rilSignalStrength->wcdma.rscp;
    signalStrength.wcdma.ecno = rilSignalStrength->wcdma.ecno;

    signalStrength.nr.ssRsrp = rilSignalStrength->nr.ssRsrp;
    signalStrength.nr.ssRsrq = rilSignalStrength->nr.ssRsrq;
    signalStrength.nr.ssSinr = rilSignalStrength->nr.ssSinr;
    signalStrength.nr.csiRsrp = rilSignalStrength->nr.csiRsrp;
    signalStrength.nr.csiRsrq = rilSignalStrength->nr.csiRsrq;
    signalStrength.nr.csiSinr = rilSignalStrength->nr.csiSinr;
}

int radio::currentSignalStrengthInd(int slotId,
                                    int indicationType, int token, RIL_Errno e,
                                    void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && (radioService[slotId]->mRadioIndication != NULL ||
        radioService[slotId]->mRadioIndicationV1_2 != NULL ||
        radioService[slotId]->mRadioIndicationV1_4 != NULL)) {
        if (response == NULL || responseLen != sizeof(RIL_SignalStrength_v1_4)) {
            RLOGE("currentSignalStrengthInd: invalid response");
            return 0;
        }

#if VDBG
        RLOGD("currentSignalStrengthInd");
#endif
        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_4 != NULL) {
            V1_4::SignalStrength signalStrength = { };
            convertRilSignalStrengthToHal_1_4(response, responseLen, signalStrength);

            retStatus = radioService[slotId]->mRadioIndicationV1_4->currentSignalStrength_1_4(
                    convertIntToRadioIndicationType(indicationType), signalStrength);
        } else if (radioService[slotId]->mRadioIndicationV1_2 != NULL) {
            V1_2::SignalStrength signalStrength = {};
            convertRilSignalStrengthToHal_1_2(response, responseLen, signalStrength);

            retStatus = radioService[slotId]->mRadioIndicationV1_2->currentSignalStrength_1_2(
                    convertIntToRadioIndicationType(indicationType), signalStrength);
        } else {
            SignalStrength signalStrength = {};
            convertRilSignalStrengthToHal(response, responseLen, signalStrength);

            retStatus = radioService[slotId]->mRadioIndication->currentSignalStrength(
                    convertIntToRadioIndicationType(indicationType), signalStrength);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("currentSignalStrengthInd: radioService[%d]->mRadioIndication == NULL"
                " || radioService[%d]->mRadioIndicationV1_2/4 == NULL",
                slotId, slotId);
    }

    return 0;
}

void convertRilDataCallToHal(RIL_SetupDataCallResult_v1_4 *dcResponse,
        SetupDataCallResult& dcResult) {
    const int LIST_SIZE = 180;
    char *separator = (char *)(" ");
    char ipList[LIST_SIZE] = {0};
    char dnsList[LIST_SIZE] = {0};
    char pcscfList[LIST_SIZE] = {0};
    char type[64] = {0};

    dcResult.status = (DataCallFailCause)dcResponse->cause;
    dcResult.suggestedRetryTime = dcResponse->suggestedRetryTime;
    dcResult.cid = dcResponse->cid;
    dcResult.active = dcResponse->active;

    if (dcResponse->type == PDP_PROTOCOL_TYPE_IP) {
        strncpy(type, "IP", sizeof("IP"));
    } else if (dcResponse->type == PDP_PROTOCOL_TYPE_IPV6) {
        strncpy(type, "IPV6", sizeof("IPV6"));
    } else if (dcResponse->type == PDP_PROTOCOL_TYPE_IPV4V6) {
        strncpy(type, "IPV4V6", sizeof("IPV4V6"));
    }
    dcResult.type = type;

    dcResult.ifname = convertCharPtrToHidlString(dcResponse->ifname);

    /* Append the IP address. for Fallback, change two net interface to one */
    for (uint32_t i = 0; i < dcResponse->addressesNumber; i++) {
        strlcat(ipList, dcResponse->addresses[i], LIST_SIZE);
        strlcat(ipList, separator, LIST_SIZE);
    }
    dcResult.addresses = ipList;
    dcResult.gateways = ipList;

    /* Append the DNS address */
    for (uint32_t i = 0; i < dcResponse->dnsesNumber; i++) {
        strlcat(dnsList, dcResponse->dnses[i], LIST_SIZE);
        strlcat(dnsList, separator, LIST_SIZE);
    }
    dcResult.dnses = dnsList;

    for (uint32_t i = 0; i < dcResponse->pcscfNumber; i++) {
        strlcat(pcscfList, dcResponse->pcscf[i], LIST_SIZE);
        strlcat(pcscfList, separator, LIST_SIZE);
    }
    dcResult.pcscf = pcscfList;

    dcResult.mtu = dcResponse->mtu;
}

void convertRilDataCallToHal_1_4(RIL_SetupDataCallResult_v1_4 *dcResponse,
        V1_4::SetupDataCallResult& dcResult) {
    dcResult.cause = (V1_4::DataCallFailCause)dcResponse->cause;
    dcResult.suggestedRetryTime = dcResponse->suggestedRetryTime;
    dcResult.cid = dcResponse->cid;
    dcResult.active = (V1_4::DataConnActiveStatus)dcResponse->active;
    dcResult.type = (V1_4::PdpProtocolType)(dcResponse->type);
    dcResult.ifname = convertCharPtrToHidlString(dcResponse->ifname);

    dcResult.addresses.resize(dcResponse->addressesNumber);
    for (uint32_t i = 0; i < dcResponse->addressesNumber; i++) {
        dcResult.addresses[i] = convertCharPtrToHidlString(dcResponse->addresses[i]);
    }
    dcResult.dnses.resize(dcResponse->dnsesNumber);
    for (uint32_t i = 0; i < dcResponse->dnsesNumber; i++) {
        dcResult.dnses[i] = convertCharPtrToHidlString(dcResponse->dnses[i]);
    }
    dcResult.gateways.resize(dcResponse->gatewaysNumber);
    for (uint32_t i = 0; i < dcResponse->gatewaysNumber; i++) {
        dcResult.gateways[i] = convertCharPtrToHidlString(dcResponse->gateways[i]);
    }
    dcResult.pcscf.resize(dcResponse->pcscfNumber);
    for (uint32_t i = 0; i < dcResponse->pcscfNumber; i++) {
        dcResult.pcscf[i] = convertCharPtrToHidlString(dcResponse->pcscf[i]);
    }
    dcResult.mtu = dcResponse->mtu;
}

void convertRilDataCallListToHal(void *response, size_t responseLen,
        hidl_vec<SetupDataCallResult>& dcResultList) {
    int num = responseLen / sizeof(RIL_SetupDataCallResult_v1_4);

    if (response == NULL) return;
    RIL_SetupDataCallResult_v1_4 *dcResponse = (RIL_SetupDataCallResult_v1_4 *) response;
    dcResultList.resize(num);
    for (int i = 0; i < num; i++) {
        convertRilDataCallToHal(&dcResponse[i], dcResultList[i]);
    }
}

void convertRilDataCallListToHal_1_4(void *response, size_t responseLen,
        hidl_vec<V1_4::SetupDataCallResult>& dcResultList) {
    int num = responseLen / sizeof(RIL_SetupDataCallResult_v1_4);

    if (response == NULL) return;
    RIL_SetupDataCallResult_v1_4 *dcResponse = (RIL_SetupDataCallResult_v1_4 *) response;
    dcResultList.resize(num);
    for (int i = 0; i < num; i++) {
        convertRilDataCallToHal_1_4(&dcResponse[i], dcResultList[i]);
    }
}

int radio::dataCallListChangedInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && (radioService[slotId]->mRadioIndication != NULL ||
        radioService[slotId]->mRadioIndicationV1_4 != NULL)) {
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_SetupDataCallResult_v1_4) != 0) {
            RLOGE("dataCallListChangedInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("dataCallListChangedInd");
#endif
        Return<void> retStatus;
        if (radioService[slotId]->mRadioIndicationV1_4 != NULL) {
            hidl_vec<V1_4::SetupDataCallResult> dcList;
            convertRilDataCallListToHal_1_4(response, responseLen, dcList);

            retStatus = radioService[slotId]->mRadioIndicationV1_4->dataCallListChanged_1_4(
                convertIntToRadioIndicationType(indicationType), dcList);
        } else {
            hidl_vec<SetupDataCallResult> dcList;
            convertRilDataCallListToHal(response, responseLen, dcList);

            retStatus = radioService[slotId]->mRadioIndication->dataCallListChanged(
                convertIntToRadioIndicationType(indicationType), dcList);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("dataCallListChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::suppSvcNotifyInd(int slotId, int indicationType,
                            int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_SuppSvcNotification)) {
            RLOGE("suppSvcNotifyInd: invalid response");
            return 0;
        }

        SuppSvcNotification suppSvc = {};
        RIL_SuppSvcNotification *ssn = (RIL_SuppSvcNotification *) response;
        suppSvc.isMT = ssn->notificationType;
        suppSvc.code = ssn->code;
        suppSvc.index = ssn->index;
        suppSvc.type = ssn->type;
        suppSvc.number = convertCharPtrToHidlString(ssn->number);

#if VDBG
        RLOGD("suppSvcNotifyInd: isMT %d code %d index %d type %d",
                suppSvc.isMT, suppSvc.code, suppSvc.index, suppSvc.type);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->suppSvcNotify(
                convertIntToRadioIndicationType(indicationType), suppSvc);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("suppSvcNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkSessionEndInd(int slotId, int indicationType,
                            int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("stkSessionEndInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkSessionEnd(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkSessionEndInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkProactiveCommandInd(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkProactiveCommandInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkProactiveCommandInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkProactiveCommand(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkProactiveCommandInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkEventNotifyInd(int slotId, int indicationType,
                             int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkEventNotifyInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkEventNotifyInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkEventNotify(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkEventNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkCallSetupInd(int slotId, int indicationType,
                           int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("stkCallSetupInd: invalid response");
            return 0;
        }
        int32_t timeout = ((int32_t *) response)[0];
#if VDBG
        RLOGD("stkCallSetupInd: timeout %d", timeout);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkCallSetup(
                convertIntToRadioIndicationType(indicationType), timeout);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkCallSetupInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simSmsStorageFullInd(int slotId,
                                int indicationType, int token, RIL_Errno e, void *response,
                                size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("simSmsStorageFullInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simSmsStorageFull(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simSmsStorageFullInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simRefreshInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_SimRefreshResponse_v7)) {
            RLOGE("simRefreshInd: invalid response");
            return 0;
        }

        SimRefreshResult refreshResult = {};
        RIL_SimRefreshResponse_v7 *simRefreshResponse = ((RIL_SimRefreshResponse_v7 *) response);
        refreshResult.type =
                (V1_0::SimRefreshType) simRefreshResponse->result;
        refreshResult.efId = simRefreshResponse->ef_id;
        refreshResult.aid = convertCharPtrToHidlString(simRefreshResponse->aid);

#if VDBG
        RLOGD("simRefreshInd: type %d efId %d", refreshResult.type, refreshResult.efId);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simRefresh(
                convertIntToRadioIndicationType(indicationType), refreshResult);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simRefreshInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

void convertRilCdmaSignalInfoRecordToHal(RIL_CDMA_SignalInfoRecord *signalInfoRecord,
        CdmaSignalInfoRecord& record) {
    record.isPresent = signalInfoRecord->isPresent;
    record.signalType = signalInfoRecord->signalType;
    record.alertPitch = signalInfoRecord->alertPitch;
    record.signal = signalInfoRecord->signal;
}

int radio::callRingInd(int slotId, int indicationType,
                       int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        bool isGsm;
        CdmaSignalInfoRecord record = {};
        if (response == NULL || responseLen == 0) {
            isGsm = true;
        } else {
            isGsm = false;
            if (responseLen != sizeof (RIL_CDMA_SignalInfoRecord)) {
                RLOGE("callRingInd: invalid response");
                return 0;
            }
            convertRilCdmaSignalInfoRecordToHal((RIL_CDMA_SignalInfoRecord *) response, record);
        }

#if VDBG
        RLOGD("callRingInd: isGsm %d", isGsm);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->callRing(
                convertIntToRadioIndicationType(indicationType), isGsm, record);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("callRingInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simStatusChangedInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("simStatusChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->simStatusChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("simStatusChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaNewSmsInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_SMS_Message)) {
            RLOGE("cdmaNewSmsInd: invalid response");
            return 0;
        }

        CdmaSmsMessage msg = {};
        RIL_CDMA_SMS_Message *rilMsg = (RIL_CDMA_SMS_Message *) response;
        msg.teleserviceId = rilMsg->uTeleserviceID;
        msg.isServicePresent = rilMsg->bIsServicePresent;
        msg.serviceCategory = rilMsg->uServicecategory;
        msg.address.digitMode =
                (V1_0::CdmaSmsDigitMode) rilMsg->sAddress.digit_mode;
        msg.address.numberMode =
                (V1_0::CdmaSmsNumberMode) rilMsg->sAddress.number_mode;
        msg.address.numberType =
                (V1_0::CdmaSmsNumberType) rilMsg->sAddress.number_type;
        msg.address.numberPlan =
                (V1_0::CdmaSmsNumberPlan) rilMsg->sAddress.number_plan;

        int digitLimit = MIN((rilMsg->sAddress.number_of_digits), RIL_CDMA_SMS_ADDRESS_MAX);
        msg.address.digits.setToExternal(rilMsg->sAddress.digits, digitLimit);

        msg.subAddress.subaddressType = (V1_0::CdmaSmsSubaddressType)
                rilMsg->sSubAddress.subaddressType;
        msg.subAddress.odd = rilMsg->sSubAddress.odd;

        digitLimit= MIN((rilMsg->sSubAddress.number_of_digits), RIL_CDMA_SMS_SUBADDRESS_MAX);
        msg.subAddress.digits.setToExternal(rilMsg->sSubAddress.digits, digitLimit);

        digitLimit = MIN((rilMsg->uBearerDataLen), RIL_CDMA_SMS_BEARER_DATA_MAX);
        msg.bearerData.setToExternal(rilMsg->aBearerData, digitLimit);

#if VDBG
        RLOGD("cdmaNewSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaNewSms(
                convertIntToRadioIndicationType(indicationType), msg);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaNewSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::newBroadcastSmsInd(int slotId,
                              int indicationType, int token, RIL_Errno e, void *response,
                              size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("newBroadcastSmsInd: invalid response");
            return 0;
        }

        hidl_vec<uint8_t> data;
        data.setToExternal((uint8_t *) response, responseLen);
#if VDBG
        RLOGD("newBroadcastSmsInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->newBroadcastSms(
                convertIntToRadioIndicationType(indicationType), data);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("newBroadcastSmsInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaRuimSmsStorageFullInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("cdmaRuimSmsStorageFullInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaRuimSmsStorageFull(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaRuimSmsStorageFullInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::restrictedStateChangedInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("restrictedStateChangedInd: invalid response");
            return 0;
        }
        int32_t state = ((int32_t *) response)[0];
#if VDBG
        RLOGD("restrictedStateChangedInd: state %d", state);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->restrictedStateChanged(
                convertIntToRadioIndicationType(indicationType), (PhoneRestrictedState) state);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("restrictedStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::enterEmergencyCallbackModeInd(int slotId,
                                         int indicationType, int token, RIL_Errno e, void *response,
                                         size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("enterEmergencyCallbackModeInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->enterEmergencyCallbackMode(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("enterEmergencyCallbackModeInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaCallWaitingInd(int slotId,
                              int indicationType, int token, RIL_Errno e, void *response,
                              size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_CallWaiting_v6)) {
            RLOGE("cdmaCallWaitingInd: invalid response");
            return 0;
        }

        CdmaCallWaiting callWaitingRecord = {};
        RIL_CDMA_CallWaiting_v6 *callWaitingRil = ((RIL_CDMA_CallWaiting_v6 *) response);
        callWaitingRecord.number = convertCharPtrToHidlString(callWaitingRil->number);
        callWaitingRecord.numberPresentation =
                (CdmaCallWaitingNumberPresentation) callWaitingRil->numberPresentation;
        callWaitingRecord.name = convertCharPtrToHidlString(callWaitingRil->name);
        convertRilCdmaSignalInfoRecordToHal(&callWaitingRil->signalInfoRecord,
                callWaitingRecord.signalInfoRecord);
        callWaitingRecord.numberType = (CdmaCallWaitingNumberType) callWaitingRil->number_type;
        callWaitingRecord.numberPlan = (CdmaCallWaitingNumberPlan) callWaitingRil->number_plan;

#if VDBG
        RLOGD("cdmaCallWaitingInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaCallWaiting(
                convertIntToRadioIndicationType(indicationType), callWaitingRecord);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaCallWaitingInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaOtaProvisionStatusInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("cdmaOtaProvisionStatusInd: invalid response");
            return 0;
        }
        int32_t status = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaOtaProvisionStatusInd: status %d", status);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaOtaProvisionStatus(
                convertIntToRadioIndicationType(indicationType), (CdmaOtaProvisionStatus) status);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaOtaProvisionStatusInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaInfoRecInd(int slotId,
                          int indicationType, int token, RIL_Errno e, void *response,
                          size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_CDMA_InformationRecords)) {
            RLOGE("cdmaInfoRecInd: invalid response");
            return 0;
        }

        CdmaInformationRecords records = {};
        RIL_CDMA_InformationRecords *recordsRil = (RIL_CDMA_InformationRecords *) response;

        char* string8 = NULL;
        int num = MIN(recordsRil->numberOfInfoRecs, RIL_CDMA_MAX_NUMBER_OF_INFO_RECS);
        if (recordsRil->numberOfInfoRecs > RIL_CDMA_MAX_NUMBER_OF_INFO_RECS) {
            RLOGE("cdmaInfoRecInd: received %d recs which is more than %d, dropping "
                    "additional ones", recordsRil->numberOfInfoRecs,
                    RIL_CDMA_MAX_NUMBER_OF_INFO_RECS);
        }
        records.infoRec.resize(num);
        for (int i = 0 ; i < num ; i++) {
            CdmaInformationRecord *record = &records.infoRec[i];
            RIL_CDMA_InformationRecord *infoRec = &recordsRil->infoRec[i];
            record->name = (CdmaInfoRecName) infoRec->name;
            // All vectors should be size 0 except one which will be size 1. Set everything to
            // size 0 initially.
            record->display.resize(0);
            record->number.resize(0);
            record->signal.resize(0);
            record->redir.resize(0);
            record->lineCtrl.resize(0);
            record->clir.resize(0);
            record->audioCtrl.resize(0);
            switch (infoRec->name) {
                case RIL_CDMA_DISPLAY_INFO_REC:
                case RIL_CDMA_EXTENDED_DISPLAY_INFO_REC: {
                    if (infoRec->rec.display.alpha_len > CDMA_ALPHA_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d", (int) infoRec->rec.display.alpha_len,
                                CDMA_ALPHA_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.display.alpha_len + 1) * sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.display.alpha_buf, infoRec->rec.display.alpha_len);
                    string8[(int)infoRec->rec.display.alpha_len] = '\0';

                    record->display.resize(1);
                    record->display[0].alphaBuf = string8;
                    free(string8);
                    string8 = NULL;
                    break;
                }

                case RIL_CDMA_CALLED_PARTY_NUMBER_INFO_REC:
                case RIL_CDMA_CALLING_PARTY_NUMBER_INFO_REC:
                case RIL_CDMA_CONNECTED_NUMBER_INFO_REC: {
                    if (infoRec->rec.number.len > CDMA_NUMBER_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d", (int) infoRec->rec.number.len,
                                CDMA_NUMBER_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.number.len + 1) * sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.number.buf, infoRec->rec.number.len);
                    string8[(int)infoRec->rec.number.len] = '\0';

                    record->number.resize(1);
                    record->number[0].number = string8;
                    free(string8);
                    string8 = NULL;
                    record->number[0].numberType = infoRec->rec.number.number_type;
                    record->number[0].numberPlan = infoRec->rec.number.number_plan;
                    record->number[0].pi = infoRec->rec.number.pi;
                    record->number[0].si = infoRec->rec.number.si;
                    break;
                }

                case RIL_CDMA_SIGNAL_INFO_REC: {
                    record->signal.resize(1);
                    record->signal[0].isPresent = infoRec->rec.signal.isPresent;
                    record->signal[0].signalType = infoRec->rec.signal.signalType;
                    record->signal[0].alertPitch = infoRec->rec.signal.alertPitch;
                    record->signal[0].signal = infoRec->rec.signal.signal;
                    break;
                }

                case RIL_CDMA_REDIRECTING_NUMBER_INFO_REC: {
                    if (infoRec->rec.redir.redirectingNumber.len >
                                                  CDMA_NUMBER_INFO_BUFFER_LENGTH) {
                        RLOGE("cdmaInfoRecInd: invalid display info response length %d "
                                "expected not more than %d\n",
                                (int)infoRec->rec.redir.redirectingNumber.len,
                                CDMA_NUMBER_INFO_BUFFER_LENGTH);
                        return 0;
                    }
                    string8 = (char*) malloc((infoRec->rec.redir.redirectingNumber.len + 1) *
                            sizeof(char));
                    if (string8 == NULL) {
                        RLOGE("cdmaInfoRecInd: Memory allocation failed for "
                                "responseCdmaInformationRecords");
                        return 0;
                    }
                    memcpy(string8, infoRec->rec.redir.redirectingNumber.buf,
                            infoRec->rec.redir.redirectingNumber.len);
                    string8[(int)infoRec->rec.redir.redirectingNumber.len] = '\0';

                    record->redir.resize(1);
                    record->redir[0].redirectingNumber.number = string8;
                    free(string8);
                    string8 = NULL;
                    record->redir[0].redirectingNumber.numberType =
                            infoRec->rec.redir.redirectingNumber.number_type;
                    record->redir[0].redirectingNumber.numberPlan =
                            infoRec->rec.redir.redirectingNumber.number_plan;
                    record->redir[0].redirectingNumber.pi = infoRec->rec.redir.redirectingNumber.pi;
                    record->redir[0].redirectingNumber.si = infoRec->rec.redir.redirectingNumber.si;
                    record->redir[0].redirectingReason =
                            (CdmaRedirectingReason) infoRec->rec.redir.redirectingReason;
                    break;
                }

                case RIL_CDMA_LINE_CONTROL_INFO_REC: {
                    record->lineCtrl.resize(1);
                    record->lineCtrl[0].lineCtrlPolarityIncluded =
                            infoRec->rec.lineCtrl.lineCtrlPolarityIncluded;
                    record->lineCtrl[0].lineCtrlToggle = infoRec->rec.lineCtrl.lineCtrlToggle;
                    record->lineCtrl[0].lineCtrlReverse = infoRec->rec.lineCtrl.lineCtrlReverse;
                    record->lineCtrl[0].lineCtrlPowerDenial =
                            infoRec->rec.lineCtrl.lineCtrlPowerDenial;
                    break;
                }

                case RIL_CDMA_T53_CLIR_INFO_REC: {
                    record->clir.resize(1);
                    record->clir[0].cause = infoRec->rec.clir.cause;
                    break;
                }

                case RIL_CDMA_T53_AUDIO_CONTROL_INFO_REC: {
                    record->audioCtrl.resize(1);
                    record->audioCtrl[0].upLink = infoRec->rec.audioCtrl.upLink;
                    record->audioCtrl[0].downLink = infoRec->rec.audioCtrl.downLink;
                    break;
                }

                case RIL_CDMA_T53_RELEASE_INFO_REC:
                    RLOGE("cdmaInfoRecInd: RIL_CDMA_T53_RELEASE_INFO_REC: INVALID");
                    return 0;

                default:
                    RLOGE("cdmaInfoRecInd: Incorrect name value");
                    return 0;
            }
        }

#if VDBG
        RLOGD("cdmaInfoRecInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaInfoRec(
                convertIntToRadioIndicationType(indicationType), records);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaInfoRecInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::indicateRingbackToneInd(int slotId,
                                   int indicationType, int token, RIL_Errno e, void *response,
                                   size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("indicateRingbackToneInd: invalid response");
            return 0;
        }
        bool start = ((int32_t *) response)[0];
#if VDBG
        RLOGD("indicateRingbackToneInd: start %d", start);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->indicateRingbackTone(
                convertIntToRadioIndicationType(indicationType), start);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("indicateRingbackToneInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::resendIncallMuteInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("resendIncallMuteInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->resendIncallMute(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("resendIncallMuteInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cdmaSubscriptionSourceChangedInd(int slotId,
                                            int indicationType, int token, RIL_Errno e,
                                            void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("cdmaSubscriptionSourceChangedInd: invalid response");
            return 0;
        }
        int32_t cdmaSource = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaSubscriptionSourceChangedInd: cdmaSource %d", cdmaSource);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->
                cdmaSubscriptionSourceChanged(convertIntToRadioIndicationType(indicationType),
                (CdmaSubscriptionSource) cdmaSource);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaSubscriptionSourceChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::cdmaPrlChangedInd(int slotId,
                             int indicationType, int token, RIL_Errno e, void *response,
                             size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("cdmaPrlChangedInd: invalid response");
            return 0;
        }
        int32_t version = ((int32_t *) response)[0];
#if VDBG
        RLOGD("cdmaPrlChangedInd: version %d", version);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->cdmaPrlChanged(
                convertIntToRadioIndicationType(indicationType), version);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cdmaPrlChangedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::exitEmergencyCallbackModeInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("exitEmergencyCallbackModeInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->exitEmergencyCallbackMode(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("exitEmergencyCallbackModeInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::rilConnectedInd(int slotId,
                           int indicationType, int token, RIL_Errno e, void *response,
                           size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        RLOGD("rilConnectedInd");
        Return<void> retStatus = radioService[slotId]->mRadioIndication->rilConnected(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("rilConnectedInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::voiceRadioTechChangedInd(int slotId,
                                    int indicationType, int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("voiceRadioTechChangedInd: invalid response");
            return 0;
        }
        int32_t rat = ((int32_t *) response)[0];
#if VDBG
        RLOGD("voiceRadioTechChangedInd: rat %d", rat);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->voiceRadioTechChanged(
                convertIntToRadioIndicationType(indicationType), (RadioTechnology) rat);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("voiceRadioTechChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilCellInfoListToHal(void *response, size_t responseLen, hidl_vec<CellInfo>& records) {
    int num = responseLen / sizeof(RIL_CellInfo_v1_4);
    int mnc_digit = 0;
    char mcc_str[32] = {0};
    char mnc_str[32] = {0};
    char strFormat[32] = {0};
    records.resize(num);

    if (response == NULL) return;
    RIL_CellInfo_v1_4 *rillCellInfo = (RIL_CellInfo_v1_4 *) response;
    for (int i = 0; i < num; i++) {
        records[i].cellInfoType = (CellInfoType) rillCellInfo->cellInfoType;
        records[i].registered = rillCellInfo->isRegistered;
        records[i].timeStampType = (TimeStampType) rillCellInfo->timeStampType;
        records[i].timeStamp = rillCellInfo->timeStamp;
        // All vectors should be size 0 except one which will be size 1. Set everything to
        // size 0 initially.
        records[i].gsm.resize(0);
        records[i].wcdma.resize(0);
        records[i].cdma.resize(0);
        records[i].lte.resize(0);
        records[i].tdscdma.resize(0);
        switch(rillCellInfo->cellInfoType) {
            case RIL_CELL_INFO_TYPE_GSM: {
                records[i].gsm.resize(1);
                CellInfoGsm *cellInfoGsm = &records[i].gsm[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.mcc);
                cellInfoGsm->cellIdentityGsm.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                    // Unisoc modify for bug928371
                    cellInfoGsm->cellIdentityGsm.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList gsm mnc digit is invalid %d", mnc_digit);
                    cellInfoGsm->cellIdentityGsm.mnc =
                            std::to_string(rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                }
                cellInfoGsm->cellIdentityGsm.lac =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.lac;
                cellInfoGsm->cellIdentityGsm.cid =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.cid;
                cellInfoGsm->cellIdentityGsm.arfcn =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.arfcn;
                cellInfoGsm->cellIdentityGsm.bsic =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.bsic;
                cellInfoGsm->signalStrengthGsm.signalStrength =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.signalStrength;
                cellInfoGsm->signalStrengthGsm.bitErrorRate =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.bitErrorRate;
                cellInfoGsm->signalStrengthGsm.timingAdvance =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_WCDMA: {
                records[i].wcdma.resize(1);
                CellInfoWcdma *cellInfoWcdma = &records[i].wcdma[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mcc);
                cellInfoWcdma->cellIdentityWcdma.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoWcdma->cellIdentityWcdma.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList wcdma mnc digit is invalid %d", mnc_digit);
                    cellInfoWcdma->cellIdentityWcdma.mnc =
                            std::to_string(rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                }
                cellInfoWcdma->cellIdentityWcdma.lac =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.lac;
                cellInfoWcdma->cellIdentityWcdma.cid =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.cid;
                cellInfoWcdma->cellIdentityWcdma.psc =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.psc;
                cellInfoWcdma->cellIdentityWcdma.uarfcn =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.uarfcn;
                cellInfoWcdma->signalStrengthWcdma.signalStrength =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.signalStrength;
                cellInfoWcdma->signalStrengthWcdma.bitErrorRate =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate;
                break;
            }

            case RIL_CELL_INFO_TYPE_CDMA: {
                records[i].cdma.resize(1);
                CellInfoCdma *cellInfoCdma = &records[i].cdma[0];
                cellInfoCdma->cellIdentityCdma.networkId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.networkId;
                cellInfoCdma->cellIdentityCdma.systemId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.systemId;
                cellInfoCdma->cellIdentityCdma.baseStationId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.basestationId;
                cellInfoCdma->cellIdentityCdma.longitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.longitude;
                cellInfoCdma->cellIdentityCdma.latitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.latitude;
                cellInfoCdma->signalStrengthCdma.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.dbm;
                cellInfoCdma->signalStrengthCdma.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.ecio;
                cellInfoCdma->signalStrengthEvdo.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.dbm;
                cellInfoCdma->signalStrengthEvdo.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.ecio;
                cellInfoCdma->signalStrengthEvdo.signalNoiseRatio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.signalNoiseRatio;
                break;
            }

            case RIL_CELL_INFO_TYPE_LTE: {
                records[i].lte.resize(1);
                CellInfoLte *cellInfoLte = &records[i].lte[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.mcc);
                cellInfoLte->cellIdentityLte.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                    // Unisoc modify for bug928371
                    cellInfoLte->cellIdentityLte.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList lte mnc digit is invalid %d", mnc_digit);
                    cellInfoLte->cellIdentityLte.mnc =
                            std::to_string(rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                }
                cellInfoLte->cellIdentityLte.ci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.ci;
                cellInfoLte->cellIdentityLte.pci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.pci;
                cellInfoLte->cellIdentityLte.tac =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.tac;
                cellInfoLte->cellIdentityLte.earfcn =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.earfcn;
                cellInfoLte->signalStrengthLte.signalStrength =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.signalStrength;
                cellInfoLte->signalStrengthLte.rsrp =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrp;
                cellInfoLte->signalStrengthLte.rsrq =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrq;
                cellInfoLte->signalStrengthLte.rssnr =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rssnr;
                cellInfoLte->signalStrengthLte.cqi =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.cqi;
                cellInfoLte->signalStrengthLte.timingAdvance =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_TD_SCDMA: {
                records[i].tdscdma.resize(1);
                CellInfoTdscdma *cellInfoTdscdma = &records[i].tdscdma[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mcc);
                cellInfoTdscdma->cellIdentityTdscdma.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoTdscdma->cellIdentityTdscdma.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList td mnc digit is invalid %d", mnc_digit);
                    cellInfoTdscdma->cellIdentityTdscdma.mnc =
                            std::to_string(rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                }
                cellInfoTdscdma->cellIdentityTdscdma.lac =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.lac;
                cellInfoTdscdma->cellIdentityTdscdma.cid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cid;
                cellInfoTdscdma->cellIdentityTdscdma.cpid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cpid;
                cellInfoTdscdma->signalStrengthTdscdma.rscp =
                        rillCellInfo->CellInfo.tdscdma.signalStrengthTdscdma.rscp;
                break;
            }
            default: {
                break;
            }
        }
        rillCellInfo += 1;
    }
}

void convertRilCellInfoListToHal_1_2(void *response, size_t responseLen, hidl_vec<V1_2::CellInfo>& records) {
    int num = responseLen / sizeof(RIL_CellInfo_v1_4);
    int mnc_digit = 0;
    char mcc_str[32] = {0};
    char mnc_str[32] = {0};
    char strFormat[32] = {0};
    records.resize(num);

    if (response == NULL) return;
    RIL_CellInfo_v1_4 *rillCellInfo = (RIL_CellInfo_v1_4 *)response;
    for (int i = 0; i < num; i++) {
        records[i].cellInfoType = (CellInfoType) rillCellInfo->cellInfoType;
        records[i].registered = rillCellInfo->isRegistered;
        records[i].timeStampType = (TimeStampType)rillCellInfo->timeStampType;
        records[i].timeStamp = rillCellInfo->timeStamp;
        records[i].connectionStatus = (V1_2::CellConnectionStatus)rillCellInfo->connectionStatus;

        // All vectors should be size 0 except one which will be size 1. Set everything to
        // size 0 initially.
        records[i].gsm.resize(0);
        records[i].wcdma.resize(0);
        records[i].cdma.resize(0);
        records[i].lte.resize(0);
        records[i].tdscdma.resize(0);
        switch(rillCellInfo->cellInfoType) {
            case RIL_CELL_INFO_TYPE_GSM: {
                records[i].gsm.resize(1);
                V1_2::CellInfoGsm *cellInfoGsm = &records[i].gsm[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.mcc);
                cellInfoGsm->cellIdentityGsm.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                    // Unisoc modify for bug928371
                    cellInfoGsm->cellIdentityGsm.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList gsm mnc digit is invalid %d", mnc_digit);
                    cellInfoGsm->cellIdentityGsm.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                }
                cellInfoGsm->cellIdentityGsm.base.lac =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.lac;
                cellInfoGsm->cellIdentityGsm.base.cid =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.cid;
                cellInfoGsm->cellIdentityGsm.base.arfcn =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.arfcn;
                cellInfoGsm->cellIdentityGsm.base.bsic =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.bsic;
                cellInfoGsm->cellIdentityGsm.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.gsm.
                                cellIdentityGsm.operatorNames.alphaLong);
                cellInfoGsm->cellIdentityGsm.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.gsm.
                                cellIdentityGsm.operatorNames.alphaShort);

                cellInfoGsm->signalStrengthGsm.signalStrength =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.signalStrength;
                cellInfoGsm->signalStrengthGsm.bitErrorRate =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.bitErrorRate;
                cellInfoGsm->signalStrengthGsm.timingAdvance =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_WCDMA: {
                records[i].wcdma.resize(1);
                V1_2::CellInfoWcdma *cellInfoWcdma = &records[i].wcdma[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mcc);
                cellInfoWcdma->cellIdentityWcdma.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoWcdma->cellIdentityWcdma.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList wcdma mnc digit is invalid %d", mnc_digit);
                    cellInfoWcdma->cellIdentityWcdma.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                }
                cellInfoWcdma->cellIdentityWcdma.base.lac =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.lac;
                cellInfoWcdma->cellIdentityWcdma.base.cid =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.cid;
                cellInfoWcdma->cellIdentityWcdma.base.psc =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.psc;
                cellInfoWcdma->cellIdentityWcdma.base.uarfcn =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.uarfcn;
                cellInfoWcdma->cellIdentityWcdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.wcdma.
                                cellIdentityWcdma.operatorNames.alphaLong);
                cellInfoWcdma->cellIdentityWcdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.wcdma.
                                cellIdentityWcdma.operatorNames.alphaShort);

                cellInfoWcdma->signalStrengthWcdma.base.signalStrength =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.signalStrength;
                cellInfoWcdma->signalStrengthWcdma.base.bitErrorRate =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate;
                cellInfoWcdma->signalStrengthWcdma.rscp =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.rscp;
                cellInfoWcdma->signalStrengthWcdma.ecno =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.ecno;
                break;
            }

            case RIL_CELL_INFO_TYPE_CDMA: {
                records[i].cdma.resize(1);
                V1_2::CellInfoCdma *cellInfoCdma = &records[i].cdma[0];
                cellInfoCdma->cellIdentityCdma.base.networkId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.networkId;
                cellInfoCdma->cellIdentityCdma.base.systemId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.systemId;
                cellInfoCdma->cellIdentityCdma.base.baseStationId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.basestationId;
                cellInfoCdma->cellIdentityCdma.base.longitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.longitude;
                cellInfoCdma->cellIdentityCdma.base.latitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.latitude;
                cellInfoCdma->cellIdentityCdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.cdma.
                                cellIdentityCdma.operatorNames.alphaLong);
                cellInfoCdma->cellIdentityCdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.cdma.
                                cellIdentityCdma.operatorNames.alphaShort);

                cellInfoCdma->signalStrengthCdma.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.dbm;
                cellInfoCdma->signalStrengthCdma.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.ecio;
                cellInfoCdma->signalStrengthEvdo.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.dbm;
                cellInfoCdma->signalStrengthEvdo.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.ecio;
                cellInfoCdma->signalStrengthEvdo.signalNoiseRatio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.signalNoiseRatio;
                break;
            }

            case RIL_CELL_INFO_TYPE_LTE: {
                records[i].lte.resize(1);
                V1_2::CellInfoLte *cellInfoLte = &records[i].lte[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.mcc);
                cellInfoLte->cellIdentityLte.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                    // Unisoc modify for bug928371
                    cellInfoLte->cellIdentityLte.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList lte mnc digit is invalid %d", mnc_digit);
                    cellInfoLte->cellIdentityLte.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                }
                cellInfoLte->cellIdentityLte.base.ci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.ci;
                cellInfoLte->cellIdentityLte.base.pci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.pci;
                cellInfoLte->cellIdentityLte.base.tac =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.tac;
                cellInfoLte->cellIdentityLte.base.earfcn =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.earfcn;
                cellInfoLte->cellIdentityLte.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.lte.base.
                                cellIdentityLte.operatorNames.alphaLong);
                cellInfoLte->cellIdentityLte.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.lte.base.
                                cellIdentityLte.operatorNames.alphaShort);
                cellInfoLte->cellIdentityLte.bandwidth =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.bandwidth;

                cellInfoLte->signalStrengthLte.signalStrength =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.signalStrength;
                cellInfoLte->signalStrengthLte.rsrp =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrp;
                cellInfoLte->signalStrengthLte.rsrq =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrq;
                cellInfoLte->signalStrengthLte.rssnr =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rssnr;
                cellInfoLte->signalStrengthLte.cqi =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.cqi;
                cellInfoLte->signalStrengthLte.timingAdvance =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.timingAdvance;
                break;
            }

            case RIL_CELL_INFO_TYPE_TD_SCDMA: {
                records[i].tdscdma.resize(1);
                V1_2::CellInfoTdscdma *cellInfoTdscdma = &records[i].tdscdma[0];
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mcc);
                cellInfoTdscdma->cellIdentityTdscdma.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoTdscdma->cellIdentityTdscdma.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList td mnc digit is invalid %d", mnc_digit);
                    cellInfoTdscdma->cellIdentityTdscdma.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                }
                cellInfoTdscdma->cellIdentityTdscdma.base.lac =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.lac;
                cellInfoTdscdma->cellIdentityTdscdma.base.cid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cid;
                cellInfoTdscdma->cellIdentityTdscdma.base.cpid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cpid;
                cellInfoTdscdma->cellIdentityTdscdma.uarfcn =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.uarfcn;
                cellInfoTdscdma->cellIdentityTdscdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.tdscdma.
                                cellIdentityTdscdma.operatorNames.alphaLong);
                cellInfoTdscdma->cellIdentityTdscdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.tdscdma.
                                cellIdentityTdscdma.operatorNames.alphaShort);

                cellInfoTdscdma->signalStrengthTdscdma.rscp =
                        rillCellInfo->CellInfo.tdscdma.signalStrengthTdscdma.rscp;
                break;
            }
            default: {
                break;
            }
        }
        rillCellInfo += 1;
    }
}

void convertRilCellInfoListToHal_1_4(void *response, size_t responseLen, hidl_vec<V1_4::CellInfo>& records) {
    int num = responseLen / sizeof(RIL_CellInfo_v1_4);
    int mnc_digit = 0;
    char mcc_str[32] = {0};
    char mnc_str[32] = {0};
    char strFormat[32] = {0};
    records.resize(num);

    if (response == NULL) return;
    RIL_CellInfo_v1_4 *rillCellInfo = (RIL_CellInfo_v1_4 *)response;
    for (int i = 0; i < num; i++) {
        records[i].isRegistered = rillCellInfo->isRegistered;
        records[i].connectionStatus = (V1_2::CellConnectionStatus)rillCellInfo->connectionStatus;

        switch (rillCellInfo->cellInfoType) {
            case RIL_CELL_INFO_TYPE_GSM: {
                V1_2::CellInfoGsm cellInfoGsm;
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.mcc);
                cellInfoGsm.cellIdentityGsm.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0",
                            mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                    // Unisoc modify for bug928371
                    cellInfoGsm.cellIdentityGsm.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList gsm mnc digit is invalid %d",
                            mnc_digit);
                    cellInfoGsm.cellIdentityGsm.base.mnc = std::to_string(
                            rillCellInfo->CellInfo.gsm.cellIdentityGsm.mnc);
                }
                cellInfoGsm.cellIdentityGsm.base.lac =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.lac;
                cellInfoGsm.cellIdentityGsm.base.cid =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.cid;
                cellInfoGsm.cellIdentityGsm.base.arfcn =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.arfcn;
                cellInfoGsm.cellIdentityGsm.base.bsic =
                        rillCellInfo->CellInfo.gsm.cellIdentityGsm.bsic;
                cellInfoGsm.cellIdentityGsm.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.gsm.
                                                   cellIdentityGsm.operatorNames.alphaLong);
                cellInfoGsm.cellIdentityGsm.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.gsm.
                                                   cellIdentityGsm.operatorNames.alphaShort);

                cellInfoGsm.signalStrengthGsm.signalStrength =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.signalStrength;
                cellInfoGsm.signalStrengthGsm.bitErrorRate =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.bitErrorRate;
                cellInfoGsm.signalStrengthGsm.timingAdvance =
                        rillCellInfo->CellInfo.gsm.signalStrengthGsm.timingAdvance;
                records[i].info.gsm(cellInfoGsm);
                break;
            }

            case RIL_CELL_INFO_TYPE_WCDMA: {
                V1_2::CellInfoWcdma cellInfoWcdma;
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mcc);
                cellInfoWcdma.cellIdentityWcdma.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                            rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoWcdma.cellIdentityWcdma.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList wcdma mnc digit is invalid %d", mnc_digit);
                    cellInfoWcdma.cellIdentityWcdma.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.mnc);
                }
                cellInfoWcdma.cellIdentityWcdma.base.lac =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.lac;
                cellInfoWcdma.cellIdentityWcdma.base.cid =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.cid;
                cellInfoWcdma.cellIdentityWcdma.base.psc =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.psc;
                cellInfoWcdma.cellIdentityWcdma.base.uarfcn =
                        rillCellInfo->CellInfo.wcdma.cellIdentityWcdma.uarfcn;
                cellInfoWcdma.cellIdentityWcdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.wcdma.
                                                   cellIdentityWcdma.operatorNames.alphaLong);
                cellInfoWcdma.cellIdentityWcdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.wcdma.
                                                   cellIdentityWcdma.operatorNames.alphaShort);

                cellInfoWcdma.signalStrengthWcdma.base.signalStrength =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.signalStrength;
                cellInfoWcdma.signalStrengthWcdma.base.bitErrorRate =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate;
                cellInfoWcdma.signalStrengthWcdma.rscp =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.rscp;
                cellInfoWcdma.signalStrengthWcdma.ecno =
                        rillCellInfo->CellInfo.wcdma.signalStrengthWcdma.ecno;
                records[i].info.wcdma(cellInfoWcdma);
                break;
            }

            case RIL_CELL_INFO_TYPE_CDMA: {
                V1_2::CellInfoCdma cellInfoCdma;
                cellInfoCdma.cellIdentityCdma.base.networkId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.networkId;
                cellInfoCdma.cellIdentityCdma.base.systemId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.systemId;
                cellInfoCdma.cellIdentityCdma.base.baseStationId =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.basestationId;
                cellInfoCdma.cellIdentityCdma.base.longitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.longitude;
                cellInfoCdma.cellIdentityCdma.base.latitude =
                        rillCellInfo->CellInfo.cdma.cellIdentityCdma.latitude;
                cellInfoCdma.cellIdentityCdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.cdma.
                                                   cellIdentityCdma.operatorNames.alphaLong);
                cellInfoCdma.cellIdentityCdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.cdma.
                                                   cellIdentityCdma.operatorNames.alphaShort);

                cellInfoCdma.signalStrengthCdma.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.dbm;
                cellInfoCdma.signalStrengthCdma.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthCdma.ecio;
                cellInfoCdma.signalStrengthEvdo.dbm =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.dbm;
                cellInfoCdma.signalStrengthEvdo.ecio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.ecio;
                cellInfoCdma.signalStrengthEvdo.signalNoiseRatio =
                        rillCellInfo->CellInfo.cdma.signalStrengthEvdo.signalNoiseRatio;
                records[i].info.cdma(cellInfoCdma);
                break;
            }

            case RIL_CELL_INFO_TYPE_LTE: {
                V1_4::CellInfoLte cellInfoLte;
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.mcc);
                cellInfoLte.base.cellIdentityLte.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                    // Unisoc modify for bug928371
                    cellInfoLte.base.cellIdentityLte.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList lte mnc digit is invalid %d", mnc_digit);
                    cellInfoLte.base.cellIdentityLte.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.lte.base.cellIdentityLte.mnc);
                }
                cellInfoLte.base.cellIdentityLte.base.ci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.ci;
                cellInfoLte.base.cellIdentityLte.base.pci =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.pci;
                cellInfoLte.base.cellIdentityLte.base.tac =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.tac;
                cellInfoLte.base.cellIdentityLte.base.earfcn =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.earfcn;
                cellInfoLte.base.cellIdentityLte.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.lte.
                                                   base.cellIdentityLte.operatorNames.alphaLong);
                cellInfoLte.base.cellIdentityLte.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.lte.
                                                   base.cellIdentityLte.operatorNames.alphaShort);

                cellInfoLte.base.cellIdentityLte.bandwidth =
                        rillCellInfo->CellInfo.lte.base.cellIdentityLte.bandwidth;

                cellInfoLte.base.signalStrengthLte.signalStrength =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.signalStrength;
                cellInfoLte.base.signalStrengthLte.rsrp =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrp;
                cellInfoLte.base.signalStrengthLte.rsrq =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rsrq;
                cellInfoLte.base.signalStrengthLte.rssnr =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.rssnr;
                cellInfoLte.base.signalStrengthLte.cqi =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.cqi;
                cellInfoLte.base.signalStrengthLte.timingAdvance =
                        rillCellInfo->CellInfo.lte.base.signalStrengthLte.timingAdvance;

                cellInfoLte.cellConfig.isEndcAvailable =
                        rillCellInfo->CellInfo.lte.cellConfig.isEndcAvailable;
                records[i].info.lte(cellInfoLte);
                break;
            }

            case RIL_CELL_INFO_TYPE_TD_SCDMA: {
                V1_2::CellInfoTdscdma cellInfoTdscdma;
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mcc);
                cellInfoTdscdma.cellIdentityTdscdma.base.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                    // Unisoc modify for bug928371
                    cellInfoTdscdma.cellIdentityTdscdma.base.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList td mnc digit is invalid %d", mnc_digit);
                    cellInfoTdscdma.cellIdentityTdscdma.base.mnc =
                            std::to_string(rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.mnc);
                }
                cellInfoTdscdma.cellIdentityTdscdma.base.lac =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.lac;
                cellInfoTdscdma.cellIdentityTdscdma.base.cid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cid;
                cellInfoTdscdma.cellIdentityTdscdma.base.cpid =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.cpid;
                cellInfoTdscdma.cellIdentityTdscdma.uarfcn =
                        rillCellInfo->CellInfo.tdscdma.cellIdentityTdscdma.uarfcn;
                cellInfoTdscdma.cellIdentityTdscdma.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.tdscdma.
                                                   cellIdentityTdscdma.operatorNames.alphaLong);
                cellInfoTdscdma.cellIdentityTdscdma.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.tdscdma.
                                                   cellIdentityTdscdma.operatorNames.alphaShort);

                cellInfoTdscdma.signalStrengthTdscdma.rscp =
                        rillCellInfo->CellInfo.tdscdma.signalStrengthTdscdma.rscp;
                records[i].info.tdscdma(cellInfoTdscdma);
                break;
            }
            case RIL_CELL_INFO_TYPE_NR: {
                V1_4::CellInfoNr cellInfoNr;
                snprintf(mcc_str, sizeof(mcc_str), "%03d",
                        rillCellInfo->CellInfo.nr.cellidentity.mcc);
                cellInfoNr.cellidentity.mcc = mcc_str;
                mnc_digit = rillCellInfo->CellInfo.nr.cellidentity.mnc_digit;
                if (mnc_digit == 2 || mnc_digit == 3) {
                    snprintf(strFormat, sizeof(strFormat), "%s%dd", "%0", mnc_digit);
                    snprintf(mnc_str, mnc_digit + 1, strFormat,
                           rillCellInfo->CellInfo.nr.cellidentity.mnc);
                    cellInfoNr.cellidentity.mnc = mnc_str;
                } else {
                    RLOGE("cellInfoList nr mnc digit is invalid %d", mnc_digit);
                    cellInfoNr.cellidentity.mnc =
                            std::to_string(rillCellInfo->CellInfo.nr.cellidentity.mnc);
                }
                cellInfoNr.cellidentity.nci =
                        rillCellInfo->CellInfo.nr.cellidentity.nci;
                cellInfoNr.cellidentity.pci =
                        rillCellInfo->CellInfo.nr.cellidentity.pci;
                cellInfoNr.cellidentity.tac =
                        rillCellInfo->CellInfo.nr.cellidentity.tac;
                cellInfoNr.cellidentity.nrarfcn =
                        rillCellInfo->CellInfo.nr.cellidentity.nrarfcn;
                cellInfoNr.cellidentity.operatorNames.alphaLong =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.nr.
                                                   cellidentity.operatorNames.alphaLong);
                cellInfoNr.cellidentity.operatorNames.alphaShort =
                        convertCharPtrToHidlString(rillCellInfo->CellInfo.nr.
                                                   cellidentity.operatorNames.alphaShort);

                cellInfoNr.signalStrength.ssRsrp =
                        rillCellInfo->CellInfo.nr.signalStrength.ssRsrp;
                cellInfoNr.signalStrength.ssRsrq =
                        rillCellInfo->CellInfo.nr.signalStrength.ssRsrq;
                cellInfoNr.signalStrength.ssSinr =
                        rillCellInfo->CellInfo.nr.signalStrength.ssSinr;
                cellInfoNr.signalStrength.csiRsrp =
                        rillCellInfo->CellInfo.nr.signalStrength.csiRsrp;
                cellInfoNr.signalStrength.csiRsrq =
                        rillCellInfo->CellInfo.nr.signalStrength.csiRsrq;
                cellInfoNr.signalStrength.csiSinr =
                        rillCellInfo->CellInfo.nr.signalStrength.csiSinr;
                records[i].info.nr(cellInfoNr);
                break;
            }
            default: {
                break;
            }
        }
        rillCellInfo += 1;
    }
}

int radio::cellInfoListInd(int slotId, int indicationType, int token,
                           RIL_Errno e, void *response,
                           size_t responseLen) {
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndication != NULL ||
             radioService[slotId]->mRadioIndicationV1_2 != NULL ||
             radioService[slotId]->mRadioIndicationV1_4 != NULL)) {
        if ((response == NULL && responseLen != 0) ||
             responseLen % sizeof(RIL_CellInfo_v1_4) != 0) {
            RLOGE("cellInfoListInd: invalid response");
            return 0;
        }

#if VDBG
        RLOGD("cellInfoListInd");
#endif
        Return<void> retStatus;

        if (radioService[slotId]->mRadioIndicationV1_4 != NULL) {
            hidl_vec<V1_4::CellInfo> records;
            convertRilCellInfoListToHal_1_4(response, responseLen, records);

            retStatus = radioService[slotId]->mRadioIndicationV1_4->cellInfoList_1_4(
                    convertIntToRadioIndicationType(indicationType), records);
        } else if (radioService[slotId]->mRadioIndicationV1_2 != NULL) {
            hidl_vec<V1_2::CellInfo> records;
            convertRilCellInfoListToHal_1_2(response, responseLen, records);

            retStatus = radioService[slotId]->mRadioIndicationV1_2->cellInfoList_1_2(
                    convertIntToRadioIndicationType(indicationType), records);
        } else {
            hidl_vec<CellInfo> records;
            convertRilCellInfoListToHal(response, responseLen, records);

            retStatus = radioService[slotId]->mRadioIndication->cellInfoList(
                    convertIntToRadioIndicationType(indicationType), records);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("cellInfoListInd: radioService[%d]->mRadioIndication || V1_2 || V1_4 == NULL", slotId);
    }

    return 0;
}

int radio::imsNetworkStateChangedInd(int slotId,
                                     int indicationType, int token, RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
#if VDBG
        RLOGD("imsNetworkStateChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->imsNetworkStateChanged(
                convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("imsNetworkStateChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::subscriptionStatusChangedInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("subscriptionStatusChangedInd: invalid response");
            return 0;
        }
        bool activate = ((int32_t *) response)[0];
#if VDBG
        RLOGD("subscriptionStatusChangedInd: activate %d", activate);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->subscriptionStatusChanged(
                convertIntToRadioIndicationType(indicationType), activate);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("subscriptionStatusChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::srvccStateNotifyInd(int slotId,
                               int indicationType, int token, RIL_Errno e, void *response,
                               size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("srvccStateNotifyInd: invalid response");
            return 0;
        }
        int32_t state = ((int32_t *) response)[0];
#if VDBG
        RLOGD("srvccStateNotifyInd: rat %d", state);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->srvccStateNotify(
                convertIntToRadioIndicationType(indicationType), (SrvccState) state);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("srvccStateNotifyInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

void convertRilHardwareConfigListToHal(void *response, size_t responseLen,
        hidl_vec<HardwareConfig>& records) {
    int num = responseLen / sizeof(RIL_HardwareConfig);
    records.resize(num);

    if (response == NULL) return;
    RIL_HardwareConfig *rilHardwareConfig = (RIL_HardwareConfig *) response;
    for (int i = 0; i < num; i++) {
        records[i].type = (HardwareConfigType) rilHardwareConfig[i].type;
        records[i].uuid = convertCharPtrToHidlString(rilHardwareConfig[i].uuid);
        records[i].state = (HardwareConfigState) rilHardwareConfig[i].state;
        switch (rilHardwareConfig[i].type) {
            case RIL_HARDWARE_CONFIG_MODEM: {
                records[i].modem.resize(1);
                records[i].sim.resize(0);
                HardwareConfigModem *hwConfigModem = &records[i].modem[0];
                hwConfigModem->rat = rilHardwareConfig[i].cfg.modem.rat;
                hwConfigModem->maxVoice = rilHardwareConfig[i].cfg.modem.maxVoice;
                hwConfigModem->maxData = rilHardwareConfig[i].cfg.modem.maxData;
                hwConfigModem->maxStandby = rilHardwareConfig[i].cfg.modem.maxStandby;
                break;
            }

            case RIL_HARDWARE_CONFIG_SIM: {
                records[i].sim.resize(1);
                records[i].modem.resize(0);
                records[i].sim[0].modemUuid =
                        convertCharPtrToHidlString(rilHardwareConfig[i].cfg.sim.modemUuid);
                break;
            }
        }
    }
}

int radio::hardwareConfigChangedInd(int slotId,
                                    int indicationType, int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if ((response == NULL && responseLen != 0)
                || responseLen % sizeof(RIL_HardwareConfig) != 0) {
            RLOGE("hardwareConfigChangedInd: invalid response");
            return 0;
        }

        hidl_vec<HardwareConfig> configs;
        convertRilHardwareConfigListToHal(response, responseLen, configs);

#if VDBG
        RLOGD("hardwareConfigChangedInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->hardwareConfigChanged(
                convertIntToRadioIndicationType(indicationType), configs);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("hardwareConfigChangedInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilRadioCapabilityToHal(void *response, size_t responseLen, RadioCapability& rc) {
    RIL_RadioCapability *rilRadioCapability = (RIL_RadioCapability *) response;
    rc.session = rilRadioCapability->session;
    rc.phase = (V1_0::RadioCapabilityPhase) rilRadioCapability->phase;
    rc.raf = rilRadioCapability->rat;
    rc.logicalModemUuid = convertCharPtrToHidlString(rilRadioCapability->logicalModemUuid);
    rc.status = (V1_0::RadioCapabilityStatus) rilRadioCapability->status;
}

int radio::radioCapabilityIndicationInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_RadioCapability)) {
            RLOGE("radioCapabilityIndicationInd: invalid response");
            return 0;
        }

        RadioCapability rc = {};
        convertRilRadioCapabilityToHal(response, responseLen, rc);

#if VDBG
        RLOGD("radioCapabilityIndicationInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->radioCapabilityIndication(
                convertIntToRadioIndicationType(indicationType), rc);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("radioCapabilityIndicationInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

bool isServiceTypeCfQuery(RIL_SsServiceType serType, RIL_SsRequestType reqType) {
    if ((reqType == SS_INTERROGATION) &&
        (serType == SS_CFU ||
         serType == SS_CF_BUSY ||
         serType == SS_CF_NO_REPLY ||
         serType == SS_CF_NOT_REACHABLE ||
         serType == SS_CF_ALL ||
         serType == SS_CF_ALL_CONDITIONAL)) {
        return true;
    }
    return false;
}

int radio::onSupplementaryServiceIndicationInd(int slotId,
                                               int indicationType, int token, RIL_Errno e,
                                               void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_StkCcUnsolSsResponse)) {
            RLOGE("onSupplementaryServiceIndicationInd: invalid response");
            return 0;
        }

        RIL_StkCcUnsolSsResponse *rilSsResponse = (RIL_StkCcUnsolSsResponse *) response;
        StkCcUnsolSsResult ss = {};
        ss.serviceType = (SsServiceType) rilSsResponse->serviceType;
        ss.requestType = (SsRequestType) rilSsResponse->requestType;
        ss.teleserviceType = (SsTeleserviceType) rilSsResponse->teleserviceType;
        ss.serviceClass = rilSsResponse->serviceClass;
        ss.result = (RadioError) rilSsResponse->result;

        if (isServiceTypeCfQuery(rilSsResponse->serviceType, rilSsResponse->requestType)) {
#if VDBG
            RLOGD("onSupplementaryServiceIndicationInd CF type, num of Cf elements %d",
                    rilSsResponse->cfData.numValidIndexes);
#endif
            if (rilSsResponse->cfData.numValidIndexes > SERVICE_CLASSES_NUM) {
                RLOGE("onSupplementaryServiceIndicationInd numValidIndexes is greater than "
                        "max value %d, truncating it to max value", SERVICE_CLASSES_NUM);
                rilSsResponse->cfData.numValidIndexes = SERVICE_CLASSES_NUM;
            }

            ss.cfData.resize(1);
            ss.ssInfo.resize(0);

            /* number of call info's */
            ss.cfData[0].cfInfo.resize(rilSsResponse->cfData.numValidIndexes);

            for (int i = 0; i < rilSsResponse->cfData.numValidIndexes; i++) {
                 RIL_CallForwardInfo cf = rilSsResponse->cfData.cfInfo[i];
                 CallForwardInfo *cfInfo = &ss.cfData[0].cfInfo[i];

                 cfInfo->status = (CallForwardInfoStatus) cf.status;
                 cfInfo->reason = cf.reason;
                 cfInfo->serviceClass = cf.serviceClass;
                 cfInfo->toa = cf.toa;
                 cfInfo->number = convertCharPtrToHidlString(cf.number);
                 cfInfo->timeSeconds = cf.timeSeconds;
#if VDBG
                 RLOGD("onSupplementaryServiceIndicationInd: "
                        "Data: %d,reason=%d,cls=%d,toa=%d,num=%s,tout=%d],", cf.status,
                        cf.reason, cf.serviceClass, cf.toa, (char*)cf.number, cf.timeSeconds);
#endif
            }
        } else {
            ss.ssInfo.resize(1);
            ss.cfData.resize(0);

            /* each int */
            ss.ssInfo[0].ssInfo.resize(MAX_SS_INFO);
            for (int i = 0; i < MAX_SS_INFO; i++) {
#if VDBG
                 RLOGD("onSupplementaryServiceIndicationInd: Data: %d",
                        rilSsResponse->ssInfo[i]);
#endif
                 ss.ssInfo[0].ssInfo[i] = rilSsResponse->ssInfo[i];
            }
        }

#if VDBG
        RLOGD("onSupplementaryServiceIndicationInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->
                onSupplementaryServiceIndication(convertIntToRadioIndicationType(indicationType),
                ss);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("onSupplementaryServiceIndicationInd: "
                "radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::stkCallControlAlphaNotifyInd(int slotId,
                                        int indicationType, int token, RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("stkCallControlAlphaNotifyInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("stkCallControlAlphaNotifyInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->stkCallControlAlphaNotify(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("stkCallControlAlphaNotifyInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

void convertRilLceDataInfoToHal(void *response, size_t responseLen, LceDataInfo& lce) {
    RIL_LceDataInfo *rilLceDataInfo = (RIL_LceDataInfo *)response;
    lce.lastHopCapacityKbps = rilLceDataInfo->last_hop_capacity_kbps;
    lce.confidenceLevel = rilLceDataInfo->confidence_level;
    lce.lceSuspended = rilLceDataInfo->lce_suspended;
}

int radio::lceDataInd(int slotId,
                      int indicationType, int token, RIL_Errno e, void *response,
                      size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_LceDataInfo)) {
            RLOGE("lceDataInd: invalid response");
            return 0;
        }

        LceDataInfo lce = {};
        convertRilLceDataInfoToHal(response, responseLen, lce);
#if VDBG
        RLOGD("lceDataInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->lceData(
                convertIntToRadioIndicationType(indicationType), lce);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("lceDataInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::pcoDataInd(int slotId,
                      int indicationType, int token, RIL_Errno e, void *response,
                      size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_PCO_Data)) {
            RLOGE("pcoDataInd: invalid response");
            return 0;
        }

        PcoDataInfo pco = {};
        RIL_PCO_Data *rilPcoData = (RIL_PCO_Data *)response;
        pco.cid = rilPcoData->cid;
        pco.bearerProto = convertCharPtrToHidlString(rilPcoData->bearer_proto);
        pco.pcoId = rilPcoData->pco_id;
        pco.contents.setToExternal((uint8_t *) rilPcoData->contents, rilPcoData->contents_length);

#if VDBG
        RLOGD("pcoDataInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->pcoData(
                convertIntToRadioIndicationType(indicationType), pco);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("pcoDataInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::modemResetInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("modemResetInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("modemResetInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndication->modemReset(
                convertIntToRadioIndicationType(indicationType),
                convertCharPtrToHidlString((char *) response));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("modemResetInd: radioService[%d]->mRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::networkScanResultInd(int slotId,
                                int indicationType, int token, RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("networkScanResultInd");
#endif
    if (radioService[slotId] != NULL &&
            (radioService[slotId]->mRadioIndicationV1_1 != NULL ||
             radioService[slotId]->mRadioIndicationV1_2 != NULL ||
             radioService[slotId]->mRadioIndicationV1_4 != NULL)) {
        if (response == NULL || responseLen == 0) {
            RLOGE("networkScanResultInd: invalid response");
            return 0;
        }

        Return<void> retStatus;
        RIL_NetworkScanResult_v1_4 *networkScanResult = (RIL_NetworkScanResult_v1_4 *)response;
        if (radioService[slotId]->mRadioIndicationV1_4 != NULL) {
            V1_4::NetworkScanResult result;
            result.status = (V1_1::ScanStatus)networkScanResult->status;
            result.error = (RadioError)networkScanResult->error;
            convertRilCellInfoListToHal_1_4(
                    networkScanResult->networkInfos,
                    networkScanResult->network_infos_length * sizeof(RIL_CellInfo_v1_4),
                    result.networkInfos);

            retStatus = radioService[slotId]->mRadioIndicationV1_4->networkScanResult_1_4(
                    convertIntToRadioIndicationType(indicationType), result);
        } else if (radioService[slotId]->mRadioIndicationV1_2 != NULL) {
            V1_2::NetworkScanResult result;
            result.status = (V1_1::ScanStatus)networkScanResult->status;
            result.error = (RadioError) networkScanResult->error;
            convertRilCellInfoListToHal_1_2(
                    networkScanResult->networkInfos,
                    networkScanResult->network_infos_length * sizeof(RIL_CellInfo_v1_4),
                    result.networkInfos);

            retStatus = radioService[slotId]->mRadioIndicationV1_2->networkScanResult_1_2(
                    convertIntToRadioIndicationType(indicationType), result);
        } else {
            V1_1::NetworkScanResult result;
            result.status = (V1_1::ScanStatus)networkScanResult->status;
            result.error = (RadioError)networkScanResult->error;
            convertRilCellInfoListToHal(
                    networkScanResult->networkInfos,
                    networkScanResult->network_infos_length * sizeof(RIL_CellInfo_v1_4),
                    result.networkInfos);

            retStatus = radioService[slotId]->mRadioIndicationV1_1->networkScanResult(
                    convertIntToRadioIndicationType(indicationType), result);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("networkScanResultInd: radioService[%d]->mRadioIndicationV1_1 == NULL ||"
                " radioService[%d]->mRadioIndicationV1_2/4 == NULL", slotId, slotId);
    }
    return 0;
}

int radio::carrierInfoForImsiEncryption(int slotId,
                                  int indicationType, int token, RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndicationV1_1 != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("carrierInfoForImsiEncryption: invalid response");
            return 0;
        }
        RLOGD("carrierInfoForImsiEncryption");
        Return<void> retStatus = radioService[slotId]->mRadioIndicationV1_1->
                carrierInfoForImsiEncryption(convertIntToRadioIndicationType(indicationType));
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("carrierInfoForImsiEncryption: radioService[%d]->mRadioIndicationV1_1 == NULL",
                slotId);
    }

    return 0;
}

int radio::keepaliveStatusInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
#if VDBG
    RLOGD("keepaliveStatusInd: token=%d", token);
#endif
    if (radioService[slotId] == NULL || radioService[slotId]->mRadioIndication == NULL) {
        RLOGE("%s: radioService[%d]->mRadioIndication == NULL", __FUNCTION__, slotId);
        return 0;
    }

    auto ret = V1_1::IRadioIndication::castFrom(
        radioService[slotId]->mRadioIndication);
    if (!ret.isOk()) {
        RLOGE("%s: ret.isOk() == false for radioService[%d]", __FUNCTION__, slotId);
        return 0;
    }
    sp<V1_1::IRadioIndication> radioIndicationV1_1 = ret;

    if (response == NULL || responseLen != sizeof(V1_1::KeepaliveStatus)) {
        RLOGE("%s: invalid response", __FUNCTION__);
        return 0;
    }

    V1_1::KeepaliveStatus ks;
    convertRilKeepaliveStatusToHal(static_cast<RIL_KeepaliveStatus*>(response), ks);

    Return<void> retStatus = radioIndicationV1_1->keepaliveStatus(
            convertIntToRadioIndicationType(indicationType), ks);
    radioService[slotId]->checkReturnStatus(retStatus);
    return 0;
}

int radio::currentLinkCapacityEstimateInd(int slotId, int indicationType,
        int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndicationV1_2 != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_LinkCapacityEstimate)) {
            RLOGE("currentLinkCapacityEstimateInd: invalid response");
            return 0;
        }

        V1_2::LinkCapacityEstimate linkCapacity = {};
        RIL_LinkCapacityEstimate *info = (RIL_LinkCapacityEstimate *)response;
        linkCapacity.downlinkCapacityKbps = info->downlinkCapacityKbps;
        linkCapacity.uplinkCapacityKbps = info->uplinkCapacityKbps;

#if VDBG
        RLOGD("currentLinkCapacityEstimateInd");
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndicationV1_2->currentLinkCapacityEstimate(
                convertIntToRadioIndicationType(indicationType), linkCapacity);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("currentLinkCapacityEstimateInd: radioService[%d]->mRadioIndicationV1_2 == NULL", slotId);
    }

    return 0;
}

int radio::currentPhysicalChannelConfigsInd(int slotId, int indicationType,
        int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && (radioService[slotId]->mRadioIndicationV1_2 != NULL ||
        radioService[slotId]->mRadioIndicationV1_4 != NULL)) {
        if (response == NULL || responseLen % sizeof(RIL_PhysicalChannelConfig_v1_4 *) != 0) {
            RLOGE("currentPhysicalChannelConfigsInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("currentPhysicalChannelConfigsInd");
#endif
        Return<void> retStatus;
        int numConfig = responseLen / sizeof(RIL_PhysicalChannelConfig_v1_4 *);
        RIL_PhysicalChannelConfig_v1_4 **pConfig = (RIL_PhysicalChannelConfig_v1_4 **)response;

        if (radioService[slotId]->mRadioIndicationV1_4 != NULL) {
            hidl_vec<V1_4::PhysicalChannelConfig> config;
            config.resize(numConfig);
            for (int i = 0; i < numConfig; i++) {
                config[i].base.status = (V1_2::CellConnectionStatus)pConfig[i]->base.status;
                config[i].base.cellBandwidthDownlink = pConfig[i]->base.cellBandwidthDownlink;
                config[i].rat = (V1_4::RadioTechnology)pConfig[i]->rat;
                config[i].rfInfo.range((V1_4::FrequencyRange)pConfig[i]->rfInfo.range);
                config[i].rfInfo.channelNumber(pConfig[i]->rfInfo.channelNumber);
                config[i].contextIds.resize(pConfig[i]->contextIdsLen);
                for (int j = 0; j < pConfig[i]->contextIdsLen; j++) {
                    config[i].contextIds[j] = pConfig[i]->contextIds[j];
                }
                config[i].physicalCellId = pConfig[i]->physicalCellId;
            }

            retStatus = radioService[slotId]->mRadioIndicationV1_4->currentPhysicalChannelConfigs_1_4(
                    convertIntToRadioIndicationType(indicationType), config);
        } else {
            hidl_vec<V1_2::PhysicalChannelConfig> config;
            config.resize(numConfig);
            for (int i = 0; i < numConfig; i++) {
                config[i].status = (V1_2::CellConnectionStatus)pConfig[i]->base.status;
                config[i].cellBandwidthDownlink = pConfig[i]->base.cellBandwidthDownlink;
            }

            retStatus = radioService[slotId]->mRadioIndicationV1_2->currentPhysicalChannelConfigs(
                    convertIntToRadioIndicationType(indicationType), config);
        }
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("currentPhysicalChannelConfigsInd: radioService[%d]->mRadioIndicationV1_2/4 == NULL", slotId);
    }

    return 0;
}

int radio::currentEmergencyNumberListInd(int slotId, int indicationType,
        int token, RIL_Errno e, void *response, size_t responseLen) {
    if (radioService[slotId] != NULL && radioService[slotId]->mRadioIndicationV1_4 != NULL) {
        if (response == NULL || responseLen % sizeof(RIL_EmergencyNumber) != 0) {
            RLOGE("currentEmergencyNumberListInd: invalid response");
            return 0;
        }

        hidl_vec<V1_4::EmergencyNumber> eccList;
        int eccListLen = responseLen / sizeof(RIL_EmergencyNumber);
        RIL_EmergencyNumber *pEccList = (RIL_EmergencyNumber *)response;
        eccList.resize(eccListLen);
        for (int i = 0; i < eccListLen; i++) {
            eccList[i].number = pEccList[i].number;
            eccList[i].mcc = pEccList[i].mcc;
            eccList[i].mnc = pEccList[i].mnc;
            eccList[i].categories = (int)pEccList[i].categories;
            if (pEccList[i].urns == NULL) {
                eccList[i].urns.resize(1);
                eccList[i].urns[0] = "";
            } else {
                eccList[i].urns.resize(pEccList[i].urnsNumber);
                for (uint32_t j = 0; j < pEccList[i].urnsNumber; j++) {
                    eccList[i].urns[j] = convertCharPtrToHidlString(pEccList[i].urns[j]);
                }
            }
            eccList[i].sources = (int)pEccList[i].sources;
        }

#if VDBG
        RLOGD("currentEmergencyNumberListInd, eccListLen[%d] = %d", slotId, eccListLen);
#endif
        Return<void> retStatus = radioService[slotId]->mRadioIndicationV1_4->currentEmergencyNumberList(
                convertIntToRadioIndicationType(indicationType), eccList);
        radioService[slotId]->checkReturnStatus(retStatus);
    } else {
        RLOGE("currentEmergencyNumberListInd: radioService[%d]->mRadioIndicationV1_4 == NULL", slotId);
    }

    return 0;

}

int radio::oemHookRawInd(int slotId,
                         int indicationType, int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (!kOemHookEnabled) return 0;

    if (oemHookService[slotId] != NULL && oemHookService[slotId]->mOemHookIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("oemHookRawInd: invalid response");
            return 0;
        }

        hidl_vec<uint8_t> data;
        data.setToExternal((uint8_t *) response, responseLen);
#if VDBG
        RLOGD("oemHookRawInd");
#endif
        Return<void> retStatus = oemHookService[slotId]->mOemHookIndication->oemHookRaw(
                convertIntToRadioIndicationType(indicationType), data);
        checkReturnStatus(slotId, retStatus, OEM_SERVICE);
    } else {
        RLOGE("oemHookRawInd: oemHookService[%d]->mOemHookIndication == NULL", slotId);
    }

    return 0;
}

int rilSlotMapping(int logicSlotId) {
    /**
     * slot id should be the id in slot mapping
     */
    char slotIdDefault[ARRAY_SIZE] = {0};
    char slotId[ARRAY_SIZE] = {0};
    snprintf(slotIdDefault, sizeof(slotIdDefault), "%d", logicSlotId);
    android::getProperty((RIL_SOCKET_ID)logicSlotId, SIM_SLOT_MAPPING_PROP, slotId, slotIdDefault);
    RLOGD("slotIdDefault = %s, slotId = %s", slotIdDefault, slotId);

    return atoi(slotId);
}

void radio::registerService(RIL_RadioFunctions *callbacks, CommandInfo *commands) {
    using namespace android::hardware;
    int simCount = 1;
    const char *serviceNames[] = {
            android::RIL_getServiceName()
            #if (SIM_COUNT >= 2)
            , RIL2_SERVICE_NAME
            #if (SIM_COUNT >= 3)
            , RIL3_SERVICE_NAME
            #if (SIM_COUNT >= 4)
            , RIL4_SERVICE_NAME
            #endif
            #endif
            #endif
            };

    #if (SIM_COUNT >= 2)
    simCount = SIM_COUNT;
    #endif

    s_vendorFunctions = callbacks;
    s_commands = commands;

    configureRpcThreadpool(1, true /* callerWillJoin */);
    for (int i = 0; i < simCount; i++) {
        pthread_rwlock_t *radioServiceRwlockPtr = getRadioServiceRwlock(i);
        int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
        assert(ret == 0);

        int slotId = rilSlotMapping(i);

        radioService[i] = new RadioImpl;
        radioService[i]->mSlotId = slotId;
        radioService[i]->mRadioResponse = NULL;
        radioService[i]->mRadioIndication = NULL;
        radioService[i]->mRadioResponseV1_1 = NULL;
        radioService[i]->mRadioIndicationV1_1 = NULL;
        radioService[i]->mRadioResponseV1_2 = NULL;
        radioService[i]->mRadioIndicationV1_2 = NULL;
        radioService[i]->mRadioResponseV1_3 = NULL;
        radioService[i]->mRadioIndicationV1_3 = NULL;
        radioService[i]->mRadioResponseV1_4 = NULL;
        radioService[i]->mRadioIndicationV1_4 = NULL;

        extRadioService[i] = new ExtRadioImpl;
        extRadioService[i]->mSlotId = slotId;
        extRadioService[i]->mExtRadioResponse = NULL;
        extRadioService[i]->mExtRadioIndication = NULL;
        extRadioService[i]->mAtcRadioResponse = NULL;
        extRadioService[i]->mAtcRadioIndication = NULL;

        RLOGD("registerService: starting android::hardware::radio::V1_4::IRadio %s",
                serviceNames[i]);
        android::status_t status = radioService[i]->registerAsService(serviceNames[i]);
        RLOGD("radioService registerService: status %d", status);
        status = extRadioService[i]->registerAsService(serviceNames[i]);
        RLOGD("extRadioService registerService: status %d", status);

        if (kOemHookEnabled) {
            oemHookService[i] = new OemHookImpl;
            oemHookService[i]->mSlotId = slotId;
            status = oemHookService[i]->registerAsService(serviceNames[i]);
        }
        ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
        assert(ret == 0);
    }
}

void rilc_thread_pool() {
    joinRpcThreadpool();
}

pthread_rwlock_t * radio::getRadioServiceRwlock(int slotId) {
    pthread_rwlock_t *radioServiceRwlockPtr = &radioServiceRwlock;

    #if (SIM_COUNT >= 2)
    if (slotId == 1) radioServiceRwlockPtr = &radioServiceRwlock2;
    #if (SIM_COUNT >= 3)
    if (slotId == 2) radioServiceRwlockPtr = &radioServiceRwlock3;
    #if (SIM_COUNT >= 4)
    if (slotId == 3) radioServiceRwlockPtr = &radioServiceRwlock4;
    #endif
    #endif
    #endif

    return radioServiceRwlockPtr;
}

// should acquire write lock for the corresponding service before calling this
void radio::setNitzTimeReceived(int slotId, long timeReceived) {
    nitzTimeReceived[slotId] = timeReceived;
}

/******************************************************************************/
/*                  UNISOC extended interfaces' implementation                */
/******************************************************************************/
Return<void> ExtRadioImpl::setExtResponseFunctions(
        const sp<IExtRadioResponse>& radioResponse,
        const sp<IExtRadioIndication>& radioIndication) {
    RLOGD("setExtResponseFunctions");

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mExtRadioResponse = radioResponse;
    mExtRadioIndication = radioIndication;
    mCounterExtRadio[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    // client is connected. Send initial indications.
    RIL_UNSOL_RESPONSE(RIL_EXT_UNSOL_RIL_CONNECTED, NULL, 0, (RIL_SOCKET_ID)mSlotId);
    RIL_UNSOL_RESPONSE(RIL_EXT_UNSOL_SIMMGR_SIM_STATUS_CHANGED,
                       NULL, 0, (RIL_SOCKET_ID)mSlotId);
    return Void();
}

Return<void> ExtRadioImpl::setAtcResponseFunctions(
        const sp<IAtcRadioResponse>& radioResponse,
        const sp<IAtcRadioIndication>& radioIndication) {
    RLOGD("setAtcResponseFunctions");

    pthread_rwlock_t *radioServiceRwlockPtr = radio::getRadioServiceRwlock(mSlotId);
    int ret = pthread_rwlock_wrlock(radioServiceRwlockPtr);
    assert(ret == 0);

    mAtcRadioResponse = radioResponse;
    mAtcRadioIndication = radioIndication;
    mCounterAtcRadio[mSlotId]++;

    ret = pthread_rwlock_unlock(radioServiceRwlockPtr);
    assert(ret == 0);

    return Void();
}

Return<void> ExtRadioImpl::sendCmdSync(int32_t phoneId,
        const hidl_string& cmd, sendCmdSync_cb _hidl_cb) {
#if VDBG
    RLOGD("sendCmdSync: %s", cmd.c_str());
#endif

    char response[4096] = {0};
    int responseLen = sizeof(response);
    char *atCmd = NULL;

    if (!copyHidlStringToRil(&atCmd, cmd.c_str(), NULL)) {
        return Void();
    }
    s_vendorFunctions->sendCmdSync(mSlotId, atCmd, response, responseLen);

    hidl_string ret = convertCharPtrToHidlString(response);

    _hidl_cb(ret);

    memsetAndFreeStrings(1, atCmd);
    return Void();
}

Return<void> ExtRadioImpl::responseAcknowledgement() {
    android::releaseWakeLock();
    return Void();
}

void ExtRadioImpl::checkReturnStatus(Return<void>& ret, serviceType srvType) {
    ::checkReturnStatus(mSlotId, ret, srvType);
}

Return<void> ExtRadioImpl::videoPhoneDial(int32_t serial, const VideoPhoneDial& dialInfo) {
#if VDBG
    RLOGD("videPhoneDial: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_DIAL);
    if (pRI == NULL) {
        return Void();
    }

    RIL_VideoPhone_Dial *vpDial = (RIL_VideoPhone_Dial *)calloc(1, sizeof(RIL_VideoPhone_Dial));
    if (vpDial == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    if (!copyHidlStringToRil(&vpDial->address, dialInfo.address, pRI)) {
        free(vpDial);
        return Void();
    }

    if (!copyHidlStringToRil(&vpDial->sub_address, dialInfo.subAddress, pRI)) {
        memsetAndFreeStrings(1, vpDial->address);
        free(vpDial);
        return Void();
    }

    vpDial->clir = dialInfo.clir;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_VideoPhone_Dial *pDial = (RIL_VideoPhone_Dial *)data;

        memsetAndFreeStrings(2, pDial->address, pDial->sub_address);
        memset(pDial, 0, dataLen);
        free(pDial);
    };
    REQUEST_INFO_PACK(pRI, cb, vpDial, sizeof(RIL_VideoPhone_Dial));

    CALL_ONREQUEST(pRI->pCI->requestNumber, vpDial, sizeof(RIL_VideoPhone_Dial), pRI, pRI->socket_id);

    return Void();
}

Return<void> ExtRadioImpl::videoPhoneCodec(int32_t serial, const VideoPhoneCodec& codec) {
#if VDBG
    RLOGD("videoPhoneCodec: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_CODEC);
    if (pRI == NULL) {
        return Void();
    }

    RIL_VideoPhone_Codec *vpCodec = (RIL_VideoPhone_Codec *)calloc(1, sizeof(RIL_VideoPhone_Codec));
    if (vpCodec == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    vpCodec->type = codec.type;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        memset(data, 0, dataLen);
        free(data);
    };
    REQUEST_INFO_PACK(pRI, cb, vpCodec, sizeof(RIL_VideoPhone_Codec));

    CALL_ONREQUEST(pRI->pCI->requestNumber, vpCodec, sizeof(RIL_VideoPhone_Codec), pRI, pRI->socket_id);

    return Void();
}

Return<void> ExtRadioImpl::videoPhoneFallback(int32_t serial) {
#if VDBG
    RLOGD("videoPhoneFallback: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_FALLBACK);
    return Void();
}

Return<void> ExtRadioImpl::videoPhoneString(int32_t serial,
                                         const ::android::hardware::hidl_string& str) {
#if VDBG
    RLOGD("videoPhoneString: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_STRING,
            str.c_str());
    return Void();
}

Return<void> ExtRadioImpl::videoPhoneLocalMedia(int32_t serial, int32_t dataType,
                                             int32_t sw, bool beReplaceImg) {
#if VDBG
    RLOGD("videoPhoneLocalMedia: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_LOCAL_MEDIA, 3,
            dataType, sw, BOOL_TO_INT(beReplaceImg));
    return Void();
}

Return<void> ExtRadioImpl::videoPhoneControlIFrame(int32_t serial, bool isIFrame,
                                                bool needIFrame) {
#if VDBG
    RLOGD("videoPhoneControlIFrame: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_VIDEOPHONE_CONTROL_IFRAME, 2,
            BOOL_TO_INT(isIFrame), BOOL_TO_INT(needIFrame));
    return Void();
}

Return<void> ExtRadioImpl::setTrafficClass(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("setTrafficClass: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_TRAFFIC_CLASS, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::enableLTE(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("enableLTE: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_LTE, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::attachData(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("attachData: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ATTACH_DATA, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::forceDeatch(int32_t serial) {
#if VDBG
    RLOGD("forceDeatch: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_FORCE_DETACH);
    return Void();
}

Return<void> ExtRadioImpl::getHDVoiceState(int32_t serial) {
#if VDBG
    RLOGD("getHDVoiceState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_HD_VOICE_STATE);
    return Void();
}

Return<void> ExtRadioImpl::simmgrSimPower(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("simmgrSimPower: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SIMMGR_SIM_POWER, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::enableRauNotify(int32_t serial) {
#if VDBG
    RLOGD("enableRauNotify: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_RAU_NOTIFY);
    return Void();
}

Return<void> ExtRadioImpl::simGetAtr(int32_t serial) {
#if VDBG
    RLOGD("simGetAtr: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_SIM_GET_ATR);
    return Void();
}

Return<void> ExtRadioImpl::explicitCallTransferExt(int32_t serial) {
#if VDBG
    RLOGD("explicitCallTransferExt: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_EXPLICIT_CALL_TRANSFER);
    return Void();
}

Return<void> ExtRadioImpl::getSimCapacity(int32_t serial) {
#if VDBG
    RLOGD("getSimCapacity: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_SIM_CAPACITY);
    return Void();
}

Return<void> ExtRadioImpl::storeSmsToSim(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("storeSmsToSim: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_STORE_SMS_TO_SIM, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::querySmsStorageMode(int32_t serial) {
#if VDBG
    RLOGD("querySmsStorageMode: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_QUERY_SMS_STORAGE_MODE);
    return Void();
}

Return<void> ExtRadioImpl::getSimlockRemaintimes(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("getSimlockRemaintimes: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_GET_SIMLOCK_REMAIN_TIMES, 1,
            type);
    return Void();
}

Return<void> ExtRadioImpl::setFacilityLockForUser(int32_t serial,
        const ::android::hardware::hidl_string& facility, bool lockState) {
#if VDBG
    RLOGD("setFacilityLockForUser: serial %d", serial);
#endif

    const char *state = lockState ? "1" : "0";
    dispatchStrings(serial, mSlotId, RIL_EXT_REQUEST_SET_FACILITY_LOCK_FOR_USER,
            true, 2, facility.c_str(), state);
    return Void();
}

Return<void> ExtRadioImpl::getSimlockStatus(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("getSimlockStatus: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_GET_SIMLOCK_STATUS, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::getSimlockDummys(int32_t serial) {
#if VDBG
    RLOGD("getSimlockDummys: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_SIMLOCK_DUMMYS);
    return Void();
}

Return<void> ExtRadioImpl::getSimlockWhitelist(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("getSimlockWhitelist: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_GET_SIMLOCK_WHITE_LIST, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::updateEcclist(int32_t serial,
        const ::android::hardware::hidl_string& ecclist) {
#if VDBG
    RLOGD("updateEcclist: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_UPDATE_ECCLIST,
            ecclist.c_str());
    return Void();
}

Return<void> ExtRadioImpl::setSinglePDN(int32_t serial, bool isSinglePDN) {
#if VDBG
    RLOGD("setSinglePDN: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_SINGLE_PDN, 1,
            BOOL_TO_INT(isSinglePDN));
    return Void();
}

Return<void> ExtRadioImpl::queryColp(int32_t serial) {
#if VDBG
    RLOGD("queryColp: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_QUERY_COLP);
    return Void();
}

Return<void> ExtRadioImpl::queryColr(int32_t serial) {
#if VDBG
    RLOGD("queryColr: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_QUERY_COLR);
    return Void();
}

Return<void> ExtRadioImpl::updateOperatorName(int32_t serial,
        const ::android::hardware::hidl_string& plmn) {
#if VDBG
    RLOGD("updateOperatorName: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_UPDATE_OPERATOR_NAME, plmn.c_str());
    return Void();
}

Return<void> ExtRadioImpl::simmgrGetSimStatus(int32_t serial) {
#if VDBG
    RLOGD("simmgrGetSimStatus: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_SIMMGR_GET_SIM_STATUS);
    return Void();
}

Return<void> ExtRadioImpl::setXcapIPAddress(int32_t serial,
        const hidl_string& ifName, const hidl_string& ipv4Addr,
        const hidl_string& ipv6Addr) {
#if VDBG
    RLOGD("setXcapIPAddress: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_EXT_REQUEST_SET_XCAP_IP_ADDR, true,
            3, ifName.c_str(), ipv4Addr.c_str(), ipv6Addr.c_str());
    return Void();
}

Return<void> ExtRadioImpl::sendCmdAsync(int32_t serial,
        const ::android::hardware::hidl_string& cmd) {
#if VDBG
    RLOGD("sendCmdAsync: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_SEND_CMD,
            cmd.c_str());
    return Void();
}

Return<void> ExtRadioImpl::reAttach(int32_t serial) {
#if VDBG
    RLOGD("reAttach: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_REATTACH);
    return Void();
}

Return<void> ExtRadioImpl::setPreferredNetworkTypeExt(int32_t serial,
        ExtPreferredNetworkType nwType) {
#if VDBG
    RLOGD("setPreferredNetworkTypeExt: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_PREFERRED_NETWORK_TYPE, 1, nwType);
    return Void();
}

Return<void> ExtRadioImpl::requestShutdownExt(int32_t serial) {
#if VDBG
    RLOGD("requestShutdownExt: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_SHUTDOWN);
    return Void();
}

Return<void> ExtRadioImpl::updateCLIP(int32_t serial, int32_t enable) {
#if VDBG
    RLOGD("updateCLIP: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_UPDATE_CLIP, 1, enable);
    return Void();
}

Return<void> ExtRadioImpl::setTPMRState(int32_t serial, int32_t state) {
#if VDBG
    RLOGD("setTPMRState: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_TPMR_STATE, 1, state);
    return Void();
}

Return<void> ExtRadioImpl::getTPMRState(int32_t serial) {
#if VDBG
    RLOGD("getTPMRState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_TPMR_STATE);
    return Void();
}

Return<void> ExtRadioImpl::setVideoResolution(int32_t serial, int32_t resolution) {
#if VDBG
    RLOGD("setVideoResolution: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_VIDEO_RESOLUTION, 1, resolution);
    return Void();
}

Return<void> ExtRadioImpl::enableLocalHold(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("enableLocalHold: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_LOCAL_HOLD, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::enableWiFiParamReport(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("enableWiFiParamReport: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_WIFI_PARAM_REPORT, 1,
            BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::callMediaChangeRequestTimeOut(int32_t serial,
        int32_t callId) {
#if VDBG
    RLOGD("callMediaChangeRequestTimeOut: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId,
            RIL_EXT_REQUEST_CALL_MEDIA_CHANGE_REQUEST_TIMEOUT, 1, callId);
    return Void();
}

Return<void> ExtRadioImpl::setLocalTone(int32_t serial, int32_t state) {
#if VDBG
    RLOGD("setLocalTone: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_LOCAL_TONE, 1, state);
    return Void();
}

Return<void> ExtRadioImpl::queryPlmn(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("queryPlmn: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_QUERY_PLMN, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::updatePlmnPriority(int32_t serial, int32_t type,
        int32_t action, int32_t plmn, int32_t act1, int32_t act2, int32_t act3) {
#if VDBG
    RLOGD("updatePlmn: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_UPDATE_PLMN, 6, type, action,
            plmn, act1, act2, act3);
    return Void();
}

Return<void> ExtRadioImpl::setSimPowerReal(int32_t serial, bool enable) {
#if VDBG
    RLOGD("setSimPowerReal: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SIM_POWER_REAL, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> ExtRadioImpl::getRadioPreference(int32_t serial,
        const hidl_string& key) {
#if VDBG
    RLOGD("getRadioPreference: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_GET_RADIO_PREFERENCE, key.c_str());
    return Void();
}

Return<void> ExtRadioImpl::setRadioPreference(int32_t serial,
        const hidl_string& key, const hidl_string& value) {
#if VDBG
    RLOGD("setRadioPrefernce: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_EXT_REQUEST_SET_RADIO_PREFERENCE, true,
            2, key.c_str(), value.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getIMSCurrentCalls(int32_t serial) {
#if VDBG
    RLOGD("getIMSCurrentCalls: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_CURRENT_CALLS);
    return Void();
}

Return<void> ExtRadioImpl::setIMSVoiceCallAvailability(int32_t serial, int32_t state) {
#if VDBG
    RLOGD("setIMSVoiceCallAvailability: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_IMS_VOICE_CALL_AVAILABILITY, 1, state);
    return Void();
}

Return<void> ExtRadioImpl::getIMSVoiceCallAvailability(int32_t serial) {
#if VDBG
    RLOGD("getIMSVoiceCallAvailability: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_VOICE_CALL_AVAILABILITY);
    return Void();
}

Return<void> ExtRadioImpl::initISIM(int32_t serial) {
#if VDBG
    RLOGD("initISIM: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_INIT_ISIM);
    return Void();
}

Return<void> ExtRadioImpl::requestVolteCallMediaChange(int32_t serial,
        int32_t callID, int32_t mediaRequest) {
#if VDBG
    RLOGD("requestVolteCallMediaChange: serial %d, callID %d, mediaRequest %d",
            serial, callID, mediaRequest);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_CALL_REQUEST_MEDIA_CHANGE, 2,
            callID, mediaRequest);
    return Void();
}

Return<void> ExtRadioImpl::responseVolteCallMediaChange(int32_t serial,
        int32_t callID, bool isAccept, int32_t videoCallMediaDirection) {
#if VDBG
    RLOGD("responseVolteCallMediaChange: serial %d, callID %d,isAccept %d, videoCallMediaDirection %d",
            serial, callID, isAccept, videoCallMediaDirection);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_CALL_RESPONSE_MEDIA_CHANGE, 3,
            callID, BOOL_TO_INT(isAccept), videoCallMediaDirection);
    return Void();
}

Return<void> ExtRadioImpl::setIMSSmscAddress(int32_t serial, const hidl_string& smsc) {
#if VDBG
    RLOGD("setIMSSmscAddress: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_SET_IMS_SMSC, smsc.c_str());
    return Void();
}

Return<void> ExtRadioImpl::volteCallFallBackToVoice(int32_t serial, int32_t callID) {
#if VDBG
    RLOGD("volteCallFallBackToVoice: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_CALL_FALL_BACK_TO_VOICE, 1, callID);
    return Void();
}

Return<void> ExtRadioImpl::queryCallForwardStatus(int32_t serial,
        const CallForwardInfoUri& info) {
#if VDBG
    RLOGD("queryCallForwardStatus: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
           RIL_EXT_REQUEST_QUERY_CALL_FORWARD_STATUS_URI);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CallForwardInfoUri *cfInfo =
            (RIL_CallForwardInfoUri *)calloc(1, sizeof(RIL_CallForwardInfoUri));
    if (cfInfo == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    cfInfo->status = info.status;
    cfInfo->reason = info.reason;
    cfInfo->numberType = info.numberType;
    cfInfo->ton = info.ton;
    if (!copyHidlStringToRil(&cfInfo->number, info.number, pRI)) {
        free(cfInfo);
        return Void();
    }
    cfInfo->serviceClass = info.serviceClass;
    if (!copyHidlStringToRil(&cfInfo->ruleset, info.ruleset, pRI)) {
        memsetAndFreeStrings(1, cfInfo->number);
        free(cfInfo);
        return Void();
    }
    cfInfo->timeSeconds = info.timeSeconds;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CallForwardInfoUri *pCFInfo = (RIL_CallForwardInfoUri *)data;

        memsetAndFreeStrings(2, pCFInfo->number, pCFInfo->ruleset);
        memset(data, 0, dataLen);
        free(pCFInfo);
    };
    REQUEST_INFO_PACK(pRI, cb, cfInfo, sizeof(RIL_CallForwardInfoUri));

    CALL_ONREQUEST(RIL_EXT_REQUEST_QUERY_CALL_FORWARD_STATUS_URI, cfInfo,
            sizeof(RIL_CallForwardInfoUri), pRI, pRI->socket_id);

    return Void();
}

Return<void> ExtRadioImpl::setCallForwardUri(int32_t serial, const CallForwardInfoUri& info) {
#if VDBG
    RLOGD("setCallForwardUri: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_EXT_REQUEST_SET_CALL_FORWARD_URI);
    if (pRI == NULL) {
        return Void();
    }

    RIL_CallForwardInfoUri *cfInfo =
            (RIL_CallForwardInfoUri *)calloc(1, sizeof(RIL_CallForwardInfoUri));
    if (cfInfo == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    cfInfo->status = info.status;
    cfInfo->reason = info.reason;
    cfInfo->numberType = info.numberType;
    cfInfo->ton = info.ton;
    if (!copyHidlStringToRil(&cfInfo->number, info.number, pRI)) {
        free(cfInfo);
        return Void();
    }
    cfInfo->serviceClass = info.serviceClass;
    if (!copyHidlStringToRil(&cfInfo->ruleset, info.ruleset, pRI)) {
        memsetAndFreeStrings(1, cfInfo->number);
        free(cfInfo);
        return Void();
    }
    cfInfo->timeSeconds = info.timeSeconds;

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        RIL_CallForwardInfoUri *pCFInfo = (RIL_CallForwardInfoUri *)data;

        memsetAndFreeStrings(2, pCFInfo->number, pCFInfo->ruleset);
        memset(data, 0, dataLen);
        free(pCFInfo);
    };
    REQUEST_INFO_PACK(pRI, cb, cfInfo, sizeof(RIL_CallForwardInfoUri));

    CALL_ONREQUEST(RIL_EXT_REQUEST_SET_CALL_FORWARD_URI, cfInfo,
            sizeof(RIL_CallForwardInfoUri), pRI, pRI->socket_id);

    return Void();
}

Return<void> ExtRadioImpl::IMSInitialGroupCall(int32_t serial,
        const hidl_string& numbers) {
#if VDBG
    RLOGD("IMSInitialGroupCall: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_IMS_INITIAL_GROUP_CALL, numbers.c_str());
    return Void();
}

Return<void> ExtRadioImpl::IMSAddGroupCall(int32_t serial,
        const hidl_string& numbers) {
#if VDBG
    RLOGD("IMSAddGroupCall: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_IMS_ADD_TO_GROUP_CALL, numbers.c_str());
    return Void();
}

Return<void> ExtRadioImpl::enableIMS(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("enableIMS: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_IMS, 1, BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::getIMSBearerState(int32_t serial) {
#if VDBG
    RLOGD("getIMSBearerState: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_BEARER_STATE);
    return Void();
}

Return<void> ExtRadioImpl::setExtInitialAttachApn(int32_t serial,
        const ExtDataProfileInfo& dataProfileInfo) {
#if VDBG
    RLOGD("setExtInitialAttachApn: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_EXT_REQUEST_SET_INITIAL_ATTACH_APN);
    if (pRI == NULL) {
        return Void();
    }

    if (s_vendorFunctions->version <= 14) {
        RIL_InitialAttachApn *iaa = (RIL_InitialAttachApn *)calloc(1, sizeof(RIL_InitialAttachApn));
        if (iaa == NULL) {
            RLOGE("Memory allocation failed for request %s",
                    requestToString(pRI->pCI->requestNumber));
            sendErrorResponse(pRI, RIL_E_NO_MEMORY);
            return Void();
        }

        if (!copyHidlStringToRil(&iaa->apn, dataProfileInfo.apn, pRI)) {
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->protocol, dataProfileInfo.protocol, pRI)) {
            memsetAndFreeStrings(1, iaa->apn);
            free(iaa);
            return Void();
        }
        iaa->authtype = (int) dataProfileInfo.authType;
        if (!copyHidlStringToRil(&iaa->username, dataProfileInfo.user, pRI)) {
            memsetAndFreeStrings(2, iaa->apn, iaa->protocol);
            free(iaa);
            return Void();
        }
        if (!copyHidlStringToRil(&iaa->password, dataProfileInfo.password, pRI)) {
            memsetAndFreeStrings(3, iaa->apn, iaa->protocol, iaa->username);
            free(iaa);
            return Void();
        }
        iaa->apnTypes = dataProfileInfo.supportedApnTypesBitmap;

        // free memory when RIL_onRequestComplete
        auto cb = [](void *data, size_t dataLen) {
            RIL_InitialAttachApn *pIAA = (RIL_InitialAttachApn *)data;

            memsetAndFreeStrings(4, pIAA->apn, pIAA->protocol, pIAA->username,
                                 pIAA->password);
            memset(data, 0, dataLen);
            free(pIAA);
        };
        REQUEST_INFO_PACK(pRI, cb, iaa, sizeof(RIL_InitialAttachApn));

        CALL_ONREQUEST(RIL_EXT_REQUEST_SET_INITIAL_ATTACH_APN, iaa,
                sizeof(RIL_InitialAttachApn), pRI, pRI->socket_id);
    } else {  // TODO:

    }

    return Void();
}

Return<void> ExtRadioImpl::IMSHandover(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("IMSHandover: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_HANDOVER, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::notifyIMSHandoverStatusUpdate(int32_t serial, int32_t status) {
#if VDBG
    RLOGD("notifyIMSHandoverStatusUpdate: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_HANDOVER_STATUS_UPDATE, 1, status);
    return Void();
}

Return<void> ExtRadioImpl::notifyIMSNetworkInfoChanged(int32_t serial,
        const ImsNetworkInfo& networkInfo) {
#if VDBG
    RLOGD("notifyIMSNetworkInfoChanged: serial %d", serial);
#endif
    RequestInfo *pRI = android::addRequestToList(serial, mSlotId,
            RIL_EXT_REQUEST_IMS_NETWORK_INFO_CHANGE);
    if (pRI == NULL) {
        return Void();
    }

    IMS_NetworkInfo *nwInfo = (IMS_NetworkInfo *)calloc(1, sizeof(IMS_NetworkInfo));
    if (nwInfo == NULL) {
        RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
        sendErrorResponse(pRI, RIL_E_NO_MEMORY);
        return Void();
    }

    nwInfo->type = networkInfo.type;
    if (!copyHidlStringToRil(&nwInfo->info, networkInfo.info, pRI)) {
        free(nwInfo);
        return Void();
    }

    // free memory when RIL_onRequestComplete
    auto cb = [](void *data, size_t dataLen) {
        IMS_NetworkInfo *pNwInfo = (IMS_NetworkInfo *)data;

        memsetAndFreeStrings(1, pNwInfo->info);
        memset(data, 0, dataLen);
        free(pNwInfo);
    };
    REQUEST_INFO_PACK(pRI, cb, nwInfo, sizeof(IMS_NetworkInfo));

    CALL_ONREQUEST(RIL_EXT_REQUEST_IMS_NETWORK_INFO_CHANGE, nwInfo,
            sizeof(IMS_NetworkInfo), pRI, pRI->socket_id);

    return Void();
}

Return<void> ExtRadioImpl::notifyIMSCallEnd(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("notifyIMSCallEnd: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_HANDOVER_CALL_END, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::notifyVoWifiEnable(int32_t serial, bool enable) {
#if VDBG
    RLOGD("notifyVoWifiEnable: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_WIFI_ENABLE, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> ExtRadioImpl::notifyVoWifiCallStateChanged(int32_t serial, bool inCall) {
#if VDBG
    RLOGD("notifyVoWifiCallStateChanged: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_WIFI_CALL_STATE_CHANGE, 1, BOOL_TO_INT(inCall));
    return Void();
}

Return<void> ExtRadioImpl::notifyDataRouterUpdate(int32_t serial) {
#if VDBG
    RLOGD("notifyDataRouterUpdate: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_IMS_UPDATE_DATA_ROUTER);
    return Void();
}

Return<void> ExtRadioImpl::IMSHoldSingleCall(int32_t serial, int32_t callId, bool enable) {
#if VDBG
    RLOGD("IMSHoldSingleCall: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_HOLD_SINGLE_CALL, 2,
            callId, BOOL_TO_INT(enable));
    return Void();
}

Return<void> ExtRadioImpl::IMSMuteSingleCall(int32_t serial, int32_t callId, bool enable) {
#if VDBG
    RLOGD("IMSMuteSingleCall: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_MUTE_SINGLE_CALL, 2,
            callId, BOOL_TO_INT(enable));
    return Void();
}


Return<void> ExtRadioImpl::IMSSilenceSingleCall(int32_t serial, int32_t callId, bool enable) {
#if VDBG
    RLOGD("IMSSilenceSingleCall: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_SILENCE_SINGLE_CALL, 2,
            callId, BOOL_TO_INT(enable));
    return Void();
}

Return<void> ExtRadioImpl::IMSEnableLocalConference(int32_t serial, bool enable) {
#if VDBG
    RLOGD("IMSEnableLocalConference: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_IMS_ENABLE_LOCAL_CONFERENCE, 1, BOOL_TO_INT(enable));
    return Void();
}

Return<void> ExtRadioImpl::notifyHandoverCallInfo(int32_t serial,
        const ::android::hardware::hidl_string& callInfo) {
#if VDBG
    RLOGD("notifyHandoverCallInfo: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_IMS_NOTIFY_HANDOVER_CALL_INFO, callInfo.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getSrvccCapbility(int32_t serial) {
#if VDBG
    RLOGD("getSrvccCapbility: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_SRVCC_CAPBILITY);
    return Void();
}

Return<void> ExtRadioImpl::getIMSPcscfAddress(int32_t serial) {
#if VDBG
    RLOGD("getIMSPcscfAddress: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_PCSCF_ADDR);
    return Void();
}
Return<void> ExtRadioImpl::setIMSPcscfAddress(int32_t serial,
        const ::android::hardware::hidl_string& addr) {
#if VDBG
    RLOGD("setIMSPcscfAddress: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_SET_IMS_PCSCF_ADDR, addr.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getFacilityLockForAppExt(int32_t serial, const hidl_string& facility,
                                                 const hidl_string& password, int32_t serviceClass,
                                                 const hidl_string& appId) {
#if VDBG
    RLOGD("getFacilityLockForAppExt: serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_EXT_REQUEST_QUERY_FACILITY_LOCK, true,
            4, facility.c_str(), password.c_str(),
            (std::to_string(serviceClass)).c_str(), appId.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getImsRegAddress(int32_t serial) {
#if VDBG
    RLOGD("getImsRegAddress: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_IMS_REGADDR);
    return Void();
}

Return<void> ExtRadioImpl::vsimSendCmd(int32_t serial, int32_t phoneId,
        const ::android::hardware::hidl_string& cmd) {
#if VDBG
    RLOGD("vsimSendCmd: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_ATC_REQUEST_VSIM_SEND_CMD, cmd.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getPreferredNetworkTypeExt(int32_t serial) {
#if VDBG
    RLOGD("getPreferredNetworkTypeExt : serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_PREFERRED_NETWORK_TYPE);
    return Void();
}

Return<void> ExtRadioImpl::setRadioPowerFallback(int32_t serial, bool enabled) {
#if VDBG
    RLOGD("setRadioPowerFallback: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_RADIO_POWER_FALLBACK, 1, BOOL_TO_INT(enabled));
    return Void();
}

Return<void> ExtRadioImpl::getCnap(int32_t serial) {
#if VDBG
    RLOGD("getCNAP: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_CNAP);
    return Void();
}

Return<void> ExtRadioImpl::setLocationInfo(int32_t serial,
        const hidl_string& longitude,
        const hidl_string& latitude) {
#if VDBG
    RLOGD("setLocationInfo : serial %d", serial);
#endif
    dispatchStrings(serial, mSlotId, RIL_EXT_REQUEST_SET_LOCATION_INFO, true,
            2, longitude.c_str(), latitude.c_str());
    return Void();
}

Return<void> ExtRadioImpl::getSpecialRatcap(int32_t serial, int32_t value) {
#if VDBG
    RLOGD("setSpecialRatcap: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_GET_SPECIAL_RATCAP, 1, value);
    return Void();
}

Return<void> ExtRadioImpl::getVideoResolution(int32_t serial) {
#if VDBG
    RLOGD("setVideoResolution: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_VIDEO_RESOLUTION);
    return Void();
}

Return<void> ExtRadioImpl::getImsPaniInfo(int32_t serial) {
#if VDBG
    RLOGD("getImsPaniInfo: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_IMS_PANI_INFO);
    return Void();
}

Return<void> ExtRadioImpl::setEmergencyOnly(int32_t serial, int32_t emergencyOnly) {
#if VDBG
    RLOGD("setEmergencyOnly: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_EMERGENCY_ONLY, 1, emergencyOnly);
    return Void();
}

Return<void> ExtRadioImpl::getSubsidyLockdyStatus(int32_t serial) {
#if VDBG
    RLOGD("getSubsidyLockdyStatus: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_SUBSIDYLOCK_STATUS);
    return Void();
}

Return<void> ExtRadioImpl::setImsUserAgent(int32_t serial,
                                           const hidl_string& sipUserAgent) {
#if VDBG
    RLOGD("setImsUserAgent: serial %d", serial);
#endif
    dispatchString(serial, mSlotId, RIL_EXT_REQUEST_SET_IMS_USER_AGENT, sipUserAgent.c_str());
    return Void();
}

Return<void> ExtRadioImpl::resetModem(int32_t serial) {
#if VDBG
    RLOGD("resetModem: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_RESET_MODEM);
    return Void();
}

Return<void> ExtRadioImpl::getVoLTEAllowedPLMN(int32_t serial) {
#if VDBG
    RLOGD("getVoLTEAllowedPLMN: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_VOLTE_ALLOWED_PLMN);
    return Void();
}

Return<void> ExtRadioImpl::setSmsBearer(int32_t serial, int32_t type) {
#if VDBG
    RLOGD("setSmsBearer: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_SMS_BEARER, 1, type);
    return Void();
}

Return<void> ExtRadioImpl::getSmsBearer(int32_t serial) {
#if VDBG
    RLOGD("getSmsBearer: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_SMS_BEARER);
    return Void();
}

Return<void> ExtRadioImpl::queryRootNode(int32_t serial) {
#if VDBG
    RLOGD("queryRootNode : serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_QUERY_ROOT_NODE);
    return Void();
}

Return<void> ExtRadioImpl::setPsDataOff(int32_t serial, bool onOff,
        int32_t exemptionInfo) {
#if VDBG
    RLOGD("setPsDataOff: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_PS_DATA_OFF, 2,
            BOOL_TO_INT(onOff), exemptionInfo);
    return Void();
}
Return<void> ExtRadioImpl::requestLteSpeedAndSignalStrength(int32_t serial) {
#if VDBG
    RLOGD("requestLteSpeedAndSingalStrength: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_LTE_SPEED_AND_SIGNAL_STRENGTH);
    return Void();
}

Return<void> ExtRadioImpl::enableNrSwitch(int32_t serial, int32_t mode,
                                       int32_t enable) {
#if VDBG
    RLOGD("enableNrSwitch: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_ENABLE_NR_SWITCH, 2, mode, enable);
    return Void();
}

Return<void> ExtRadioImpl::setUsbShareStateSwitch(int32_t serial, bool switchState) {
#if VDBG
    RLOGD("setUsbShareStateSwitch: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_USBSHARE_SWITCH, 1, BOOL_TO_INT(switchState));
    return Void();
}

Return<void> ExtRadioImpl::setStandAlone(int32_t serial, int32_t value) {
#if VDBG
    RLOGD("setStandAlone: serial %d", serial);
#endif
    dispatchInts(serial, mSlotId, RIL_EXT_REQUEST_SET_STAND_ALONE, 1, value);
    return Void();
}

Return<void> ExtRadioImpl::getStandAlone(int32_t serial) {
#if VDBG
    RLOGD("getStandAlone: serial %d", serial);
#endif
    dispatchVoid(serial, mSlotId, RIL_EXT_REQUEST_GET_STAND_ALONE);
    return Void();
}

/******************************************************************************/
/*       UNISOC extended interfaces' corresponding responseFunction           */
/******************************************************************************/
void populateResponseInfoExt(ExtRadioResponseInfo& responseInfo, int serial,
        int responseType, RIL_Errno e) {
    responseInfo.serial = serial;
    switch (responseType) {
        case RESPONSE_SOLICITED:
            responseInfo.type = ExtRadioResponseType::SOLICITED;
            break;
        case RESPONSE_SOLICITED_ACK_EXP:
            responseInfo.type = ExtRadioResponseType::SOLICITED_ACK_EXP;
            break;
    }
    responseInfo.error = (ExtRadioError) e;
}

int responseIntOrEmptyExt(ExtRadioResponseInfo& responseInfo, int serial, int responseType,
        RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfoExt(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL && responseLen == 0) {
        // Earlier RILs did not send a response for some cases although the interface
        // expected an integer as response. Do not return error if response is empty. Instead
        // Return -1 in those cases to maintain backward compatibility.
    } else if (response == NULL || responseLen != sizeof(int)) {
        RLOGE("responseIntOrEmpty: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

int responseIntExt(ExtRadioResponseInfo& responseInfo, int serial, int responseType,
        RIL_Errno e, void *response, size_t responseLen) {
    populateResponseInfoExt(responseInfo, serial, responseType, e);
    int ret = -1;

    if (response == NULL || responseLen != sizeof(int)) {
        RLOGE("responseInt: Invalid response");
        if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
    } else {
        int *p_int = (int *) response;
        ret = p_int[0];
    }
    return ret;
}

int radio::videoPhoneDialResponse(int slotId, int responseType, int serial,
                                  RIL_Errno e, void *response,
                                  size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneDialResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneDialResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneDialResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::videoPhoneCodecResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneCodecResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneCodecResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneCodecResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::videoPhoneFallbackResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneFallbackResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneFallbackResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneFallbackResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::videoPhoneStringResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneStringResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneStringResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneStringResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::videoPhoneLocalMediaResponse(int slotId, int responseType, int serial,
                                        RIL_Errno e, void *response,
                                        size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneLocalMediaResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneLocalMediaResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneLocalMediaResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::videoPhoneControlIFrameResponse(int slotId, int responseType, int serial,
                                           RIL_Errno e, void *response,
                                           size_t responseLen) {
#if VDBG
    RLOGD("videoPhoneControlIFrameResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                videoPhoneControlIFrameResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneControlIFrameResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setTrafficClassResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("setTrafficClassResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setTrafficClassResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setTrafficClassResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::enableLTEResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response,
                             size_t responseLen) {
#if VDBG
    RLOGD("enableLTEResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                enableLTEResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableLTEResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::attachDataResponse(int slotId, int responseType, int serial,
                              RIL_Errno e, void *response,
                              size_t responseLen) {
#if VDBG
    RLOGD("attachDataResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                attachDataResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("attachDataResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::forceDeatchResponse(int slotId, int responseType, int serial,
                               RIL_Errno e, void *response,
                               size_t responseLen) {
#if VDBG
    RLOGD("forceDeatchResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                forceDeatchResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("forceDeatchResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getHDVoiceStateResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("getHDVoiceStateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getHDVoiceStateResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getHDVoiceStateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::simmgrSimPowerResponse(int slotId, int responseType, int serial,
                                  RIL_Errno e, void *response,
                                  size_t responseLen) {
#if VDBG
    RLOGD("simmgrSimPowerResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                simmgrSimPowerResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simmgrSimPowerResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::enableRauNotifyResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("enableRauNotifyResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                enableRauNotifyResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableRauNotifyResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::simGetAtrResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response,
                             size_t responseLen) {
#if VDBG
    RLOGD("simGetAtrResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                simGetAtrResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simGetAtrResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::explicitCallTransferExtResponse(int slotId, int responseType,
                                           int serial, RIL_Errno e,
                                           void *response, size_t responseLen) {
#if VDBG
    RLOGD("explicitCallTransferExtResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->explicitCallTransferExtResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("explicitCallTransferExtResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSimCapacityResponse(int slotId,  int responseType, int serial,
                                  RIL_Errno e, void *response,
                                  size_t responseLen) {
#if VDBG
    RLOGD("getSimCapacityResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        hidl_vec<hidl_string> data;

        if (response == NULL || responseLen % sizeof(char *) != 0) {
            RLOGE("getSimCapacityResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **)response;
            int numStrings = responseLen / sizeof(char *);
            data.resize(numStrings);
            for (int i = 0; i < numStrings; i++) {
                data[i] = convertCharPtrToHidlString(resp[i]);
            }
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSimCapacityResponse(responseInfo, data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSimCapacityResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::storeSmsToSimResponse(int slotId, int responseType, int serial,
                                 RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("storeSmsToSimResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                storeSmsToSimResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("storeSmsToSimResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::querySmsStorageModeResponse(int slotId, int responseType, int serial,
                                 RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("querySmsStorageModeResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                querySmsStorageModeResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("querySmsStorageModeResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSimlockRemaintimesResponse(int slotId, int responseType, int serial,
                                         RIL_Errno e, void *response,
                                         size_t responseLen) {
#if VDBG
    RLOGD("getSimlockRemaintimesResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSimlockRemaintimesResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSimlockRemaintimesResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setFacilityLockForUserResponse(int slotId, int responseType, int serial,
                                          RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("setFacilityLockForUserResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setFacilityLockForUserResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setFacilityLockForUserResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSimlockStatusResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getSimlockStatusResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSimlockStatusResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSimlockStatusResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSimlockDummysResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getSimlockDummysResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        hidl_vec<int32_t> data;

        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("getSimlockDummysResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *)response;
            int numInts = responseLen / sizeof(int);
            data.resize(numInts);
            for (int i = 0; i < numInts; i++) {
                data[i] = (int32_t)pInt[i];
            }
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSimlockDummysResponse(responseInfo, data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSimlockDummysResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSimlockWhitelistResponse(int slotId, int responseType, int serial,
                                       RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("getSimlockWhitelistResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSimlockWhitelistResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSimlockWhitelistResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::updateEcclistResponse(int slotId, int responseType, int serial,
                                 RIL_Errno e, void *response,
                                 size_t responseLen) {
#if VDBG
    RLOGD("updateEcclistResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                updateEcclistResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("updateEcclistResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setSinglePDNResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("setSinglePDNResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setSinglePDNResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setSinglePDNResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::queryColpResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response,
                             size_t responseLen) {
#if VDBG
    RLOGD("queryColpResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                queryColpResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("queryColpResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::queryColrResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response,
                             size_t responseLen) {
#if VDBG
    RLOGD("queryColrResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                queryColrResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("queryColrResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::updateOperatorNameResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("updateOperatorNameResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                updateOperatorNameResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("updateOperatorNameResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

void fillExtCardStatusFromRILCardStatus_1_4(ExtCardStatus &cardStatus, RIL_CardStatus_v1_4 *response) {
    RIL_CardStatus_v1_4 *p_cur = response;

    cardStatus.cardState = (ExtCardState) p_cur->base.base.card_state;
    cardStatus.universalPinState = (ExtPinState) p_cur->base.base.universal_pin_state;
    cardStatus.gsmUmtsSubscriptionAppIndex = p_cur->base.base.gsm_umts_subscription_app_index;
    cardStatus.cdmaSubscriptionAppIndex = p_cur->base.base.cdma_subscription_app_index;
    cardStatus.imsSubscriptionAppIndex = p_cur->base.base.ims_subscription_app_index;

    RIL_AppStatus *rilAppStatus = p_cur->base.base.applications;
    cardStatus.applications.resize(p_cur->base.base.num_applications);
    ExtAppStatus *appStatus = cardStatus.applications.data();

#if VDBG
    RLOGD("num_applications %d", p_cur->base.base.num_applications);
#endif

    for (int i = 0; i < p_cur->base.base.num_applications; i++) {
        appStatus[i].appType = (ExtAppType) rilAppStatus[i].app_type;
        appStatus[i].appState = (ExtAppState) rilAppStatus[i].app_state;
        appStatus[i].persoSubstate = (ExtPersoSubstate) rilAppStatus[i].perso_substate;
        appStatus[i].aidPtr = convertCharPtrToHidlString(rilAppStatus[i].aid_ptr);
        appStatus[i].appLabelPtr = convertCharPtrToHidlString(
                rilAppStatus[i].app_label_ptr);
        appStatus[i].pin1Replaced = rilAppStatus[i].pin1_replaced;
        appStatus[i].pin1 = (ExtPinState) rilAppStatus[i].pin1;
        appStatus[i].pin2 = (ExtPinState) rilAppStatus[i].pin2;
    }
}

int radio::simmgrGetSimStatusResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        ExtCardStatus cardStatus = {};
        if (response == NULL || responseLen != sizeof(RIL_CardStatus_v1_4)) {
            RLOGE("simmgrGetSimStatusResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            fillExtCardStatusFromRILCardStatus_1_4(cardStatus, (RIL_CardStatus_v1_4 *)response);
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                simmgrGetSimStatusResponse(responseInfo, cardStatus);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simmgrGetSimStatusResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setXcapIPAddressResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("setXcapIPAddressResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setXcapIPAddressResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setXcapIPAddressResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::sendCmdAsyncResponse(int slotId, int responseType, int serial,
                         RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("sendCmdAsyncResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        char **resp = (char **) response;
        hidl_string data = convertCharPtrToHidlString(resp[0]);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                sendCmdAsyncResponse(responseInfo, data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("sendCmdAsyncResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::reAttachResponse(int slotId, int responseType, int serial,
                            RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("reAttachResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                reAttachResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("reAttachResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setPreferredNetworkTypeExtResponse(int slotId, int responseType,
                                              int serial, RIL_Errno e,
                                              void *response, size_t responseLen) {
#if VDBG
    RLOGD("setPreferredNetworkTypeExtResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->setPreferredNetworkTypeExtResponse(
                responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setPreferredNetworkTypeExtResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::requestShutdownExtResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("requestShutdownExtResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->requestShutdownExtResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("requestShutdownExtResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::updateCLIPResponse(int slotId, int responseType, int serial,
                              RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("updateCLIPResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                updateCLIPResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("updateCLIPResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getTPMRStateResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getTPMRStateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                        response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getTPMRStateResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getTPMRStateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setTPMRStateResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setTPMRStateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setTPMRStateResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setTPMRStateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setVideoResolutionResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setVideoResolutionResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setVideoResolutionResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setVideoResolutionResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::enableLocalHoldResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("enableLocalHoldResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                enableLocalHoldResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableLocalHoldResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::enableWiFiParamReportResponse(int slotId, int responseType, int serial,
                                         RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("enableWiFiParamReportResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                enableWiFiParamReportResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableWiFiParamReportResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::callMediaChangeRequestTimeOutResponse(int slotId, int responseType,
                                                 int serial, RIL_Errno e,
                                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("callMediaChangeRequestTimeOutResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                callMediaChangeRequestTimeOutResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("callMediaChangeRequestTimeOutResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setLocalToneResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setLocalToneResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setLocalToneResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setLocalToneResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::updatePlmnPriorityResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("updatePlmnPriorityResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                updatePlmnPriorityResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("updatePlmnPriorityResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::queryPlmnResponse(int slotId, int responseType, int serial,
                         RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("queryPlmnResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                queryPlmnResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("queryPlmnResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setSimPowerRealResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setSimPowerRealResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setSimPowerRealResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simPowerResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getRadioPreferenceResponse(int slotId, int responseType, int serial,
        RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getRadioPreferenceResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getRadioPreferenceResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getRadioPreferenceResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setRadioPreferenceResponse(int slotId, int responseType, int serial,
        RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setRadioPreferenceResponse : serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setRadioPreferenceResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setRadioPreferenceResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getIMSCurrentCallsResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("getIMSCurrentCallsResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);

        char *numberTmp = NULL;
        hidl_vec<CallVoLTE> calls;
        if (response == NULL || (responseLen % sizeof(RIL_Call_VoLTE *)) != 0) {
            RLOGE("getIMSCurrentCallsResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_Call_VoLTE *);
            calls.resize(num);

            for (int i = 0 ; i < num ; i++) {
                RIL_Call_VoLTE *p_cur = ((RIL_Call_VoLTE **) response)[i];
                /* each call info */
                calls[i].index = p_cur->index;
                calls[i].isMT = p_cur->isMT;
                calls[i].negStatusPresent = p_cur->negStatusPresent;
                calls[i].negStatus = p_cur->negStatus;
                calls[i].mediaDescription = convertCharPtrToHidlString(p_cur->mediaDescription);
                calls[i].csMode = p_cur->csMode;
                calls[i].state = (ExtCallState)p_cur->state;
                calls[i].mpty = p_cur->mpty;
                calls[i].numberType = p_cur->numberType;
                calls[i].toa = p_cur->toa;
                if (p_cur->number != NULL) {
                    numberTmp = strdup(p_cur->number);
                    stripNumberFromSipAddress(p_cur->number, numberTmp,
                            strlen(numberTmp) * sizeof(char));
                    calls[i].number = convertCharPtrToHidlString(numberTmp);
                } else {
                    calls[i].number = convertCharPtrToHidlString(p_cur->number);
                }
                calls[i].prioritypresent = p_cur->prioritypresent;
                calls[i].priority = p_cur->priority;
                calls[i].cliValidityPresent = p_cur->CliValidityPresent;
                calls[i].numberPresentation = p_cur->numberPresentation;
                calls[i].als = p_cur->als;
                calls[i].isVoicePrivacy = p_cur->isVoicePrivacy;
                calls[i].name = convertCharPtrToHidlString(p_cur->name);
                calls[i].namePresentation = p_cur->namePresentation;
                calls[i].isLoopBack = p_cur->isLoopBack;

                if (p_cur->uusInfo != NULL && p_cur->uusInfo->uusData != NULL) {
                    RIL_UUS_Info *uusInfo = p_cur->uusInfo;
                    calls[i].uusInfo.uusType = (ExtUusType)uusInfo->uusType;
                    calls[i].uusInfo.uusDcs = (ExtUusDcs)uusInfo->uusDcs;
                    // convert uusInfo->uusData to a null-terminated string
//                    char *nullTermStr = strndup(uusInfo->uusData, uusInfo->uusLength);
//                    calls[i].uusInfo[0].uusData = nullTermStr;
//                    free(nullTermStr);// TODO: right?
                    calls[i].uusInfo.uusData = convertCharPtrToHidlString(uusInfo->uusData);
                }
            }
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getIMSCurrentCallsResponse(responseInfo, calls);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
        free(numberTmp);
    } else {
        RLOGE("getIMSCurrentCallsResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setIMSVoiceCallAvailabilityResponse(int slotId, int responseType, int serial,
                                               RIL_Errno e, void *response,
                                               size_t responseLen) {
#if VDBG
    RLOGD("setIMSVoiceCallAvailabilityResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setIMSVoiceCallAvailabilityResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setIMSVoiceCallAvailabilityResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getIMSVoiceCallAvailabilityResponse(int slotId, int responseType, int serial,
                                               RIL_Errno e, void *response,
                                               size_t responseLen) {
#if VDBG
    RLOGD("getIMSVoiceCallAvailabilityResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getIMSVoiceCallAvailabilityResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getIMSVoiceCallAvailabilityResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::initISIMResponse(int slotId, int responseType, int serial,
                            RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("initISIMResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                initISIMResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("initISIMResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::requestVolteCallMediaChangeResponse(int slotId, int responseType, int serial,
                                               RIL_Errno e, void *response,
                                               size_t responseLen) {
#if VDBG
    RLOGD("requestVolteCallMediaChangeResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                requestVolteCallMediaChangeResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("requestVolteCallMediaChangeResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::responseVolteCallMediaChangeResponse(int slotId, int responseType, int serial,
                                                RIL_Errno e, void *response,
                                                size_t responseLen) {
#if VDBG
    RLOGD("responseVolteCallMediaChangeResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                responseVolteCallMediaChangeResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("responseVolteCallMediaChangeResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setIMSSmscAddressResponse(int slotId, int responseType, int serial,
                                                RIL_Errno e, void *response,
                                                size_t responseLen) {
#if VDBG
    RLOGD("setIMSSmscAddressResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setIMSSmscAddressResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setIMSSmscAddressResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::volteCallFallBackToVoiceResponse(int slotId, int responseType, int serial,
                                            RIL_Errno e, void *response,
                                            size_t responseLen) {
#if VDBG
    RLOGD("volteCallFallBackToVoiceResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                volteCallFallBackToVoiceResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("volteCallFallBackToVoiceResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}



int radio::queryCallForwardStatusResponse(int slotId, int responseType, int serial,
                                          RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("queryCallForwardStatusResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        hidl_vec<CallForwardInfoUri> callForwardInfos;

        if ((response == NULL && responseLen != 0) ||
             responseLen % sizeof(RIL_CallForwardInfoUri *) != 0) {
            RLOGE("queryCallForwardStatusResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            int num = responseLen / sizeof(RIL_CallForwardInfoUri *);
            callForwardInfos.resize(num);
            for (int i = 0 ; i < num; i++) {
                RIL_CallForwardInfoUri *resp = ((RIL_CallForwardInfoUri **) response)[i];
                callForwardInfos[i].status = resp->status;
                callForwardInfos[i].reason = resp->reason;
                callForwardInfos[i].numberType = resp->numberType;
                callForwardInfos[i].ton = resp->ton;
                callForwardInfos[i].number = convertCharPtrToHidlString(resp->number);
                callForwardInfos[i].serviceClass = resp->serviceClass;
                callForwardInfos[i].ruleset = convertCharPtrToHidlString(resp->ruleset);
                callForwardInfos[i].timeSeconds = resp->timeSeconds;
            }
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                queryCallForwardStatusResponse(responseInfo, callForwardInfos);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("queryCallForwardStatusResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setCallForwardUriResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
#if VDBG
    RLOGD("setCallForwardUriResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setCallForwardUriResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setCallForwardUriResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSInitialGroupCallResponse(int slotId, int responseType, int serial,
                                       RIL_Errno e, void *response,
                                       size_t responseLen) {
#if VDBG
    RLOGD("IMSInitialGroupCallResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSInitialGroupCallResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSInitialGroupCallResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSAddGroupCallResponse(int slotId, int responseType, int serial,
                                   RIL_Errno e, void *response,
                                   size_t responseLen) {
#if VDBG
    RLOGD("IMSAddGroupCallResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSAddGroupCallResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSAddGroupCallResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::enableIMSResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("enableIMSResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                enableIMSResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableIMSResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getIMSBearerStateResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
#if VDBG
    RLOGD("getIMSBearerStateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getIMSBearerStateResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getIMSBearerStateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setExtInitialAttachApnResponse(int slotId, int responseType, int serial,
                                          RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("setExtInitialAttachApnResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setExtInitialAttachApnResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setExtInitialAttachApnResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSHandoverResponse(int slotId, int responseType, int serial,
                        RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("IMSHandoverResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSHandoverResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSHandoverResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyIMSHandoverStatusUpdateResponse(int slotId, int responseType, int serial,
                                                 RIL_Errno e, void *response,
                                                 size_t responseLen) {
#if VDBG
    RLOGD("notifyIMSHandoverStatusUpdateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyIMSHandoverStatusUpdateResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyIMSHandoverStatusUpdateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyIMSNetworkInfoChangedResponse(int slotId, int responseType, int serial,
                                               RIL_Errno e, void *response,
                                               size_t responseLen) {
#if VDBG
    RLOGD("notifyIMSNetworkInfoChangedResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyIMSNetworkInfoChangedResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyIMSNetworkInfoChangedResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyIMSCallEndResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("notifyIMSCallEndResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyIMSCallEndResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyIMSCallEndResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyVoWifiEnableResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("notifyVoWifiEnableResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyVoWifiEnableResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyVoWifiEnableResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyVoWifiCallStateChangedResponse(int slotId, int responseType, int serial,
                                                RIL_Errno e, void *response,
                                                size_t responseLen) {
#if VDBG
    RLOGD("notifyVoWifiCallStateChangedResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyVoWifiCallStateChangedResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyVoWifiCallStateChangedResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyDataRouterUpdateResponse(int slotId, int responseType, int serial,
                                          RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("notifyDataRouterUpdateResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyDataRouterUpdateResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyDataRouterUpdateResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSHoldSingleCallResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
#if VDBG
    RLOGD("IMSHoldSingleCallResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSHoldSingleCallResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSHoldSingleCallResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSMuteSingleCallResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
#if VDBG
    RLOGD("IMSMuteSingleCallResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSMuteSingleCallResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSMuteSingleCallResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSSilenceSingleCallResponse(int slotId, int responseType, int serial,
                                        RIL_Errno e, void *response,
                                        size_t responseLen) {
#if VDBG
    RLOGD("IMSSilenceSingleCallResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSSilenceSingleCallResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSSilenceSingleCallResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::IMSEnableLocalConferenceResponse(int slotId, int responseType, int serial,
                                            RIL_Errno e, void *response,
                                            size_t responseLen) {
#if VDBG
    RLOGD("IMSEnableLocalConferenceResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                IMSEnableLocalConferenceResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSEnableLocalConferenceResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::notifyHandoverCallInfoResponse(int slotId, int responseType, int serial,
                                          RIL_Errno e, void *response,
                                          size_t responseLen) {
#if VDBG
    RLOGD("notifyHandoverCallInfoResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                notifyHandoverCallInfoResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("notifyHandoverCallInfoResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSrvccCapbilityResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
#if VDBG
    RLOGD("getSrvccCapbilityResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSrvccCapbilityResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSrvccCapbilityResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getIMSPcscfAddressResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("getIMSPcscfAddressResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getIMSPcscfAddressResponse(responseInfo,
                        convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getIMSPcscfAddressResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setIMSPcscfAddressResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response,
                                      size_t responseLen) {
#if VDBG
    RLOGD("setIMSPcscfAddressResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setIMSPcscfAddressResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setIMSPcscfAddressResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getFacilityLockForAppExtResponse(int slotId, int responseType,
                                            int serial, RIL_Errno e,
                                            void *response, size_t responseLen) {
#if VDBG
    RLOGD("getFacilityLockForAppExtResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int status = 0;
        int serviceClass = 0;

        populateResponseInfoExt(responseInfo, serial, responseType, e);
        if (response == NULL || responseLen != 2 * sizeof(int)) {
            RLOGE("responseInt: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            int *p_int = (int *)response;
            status = p_int[0];
            serviceClass = p_int[1];
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getFacilityLockForAppExtResponse(responseInfo, status, serviceClass);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getFacilityLockForAppExtResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getImsRegAddressResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getImsRegAddressResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        hidl_vec<hidl_string> data;

        if (response == NULL || responseLen % sizeof(char *) != 0) {
            RLOGE("getImsRegAddressResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS)responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            char **resp = (char **)response;
            int numStrings = responseLen / sizeof(char *);
            data.resize(numStrings);
            for (int i = 0; i < numStrings; i++) {
                data[i] = convertCharPtrToHidlString(resp[i]);
            }
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getImsRegAddressResponse(responseInfo, data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getImsRegAddressResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::vsimSendCmdResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("vsimSendCmdResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mAtcRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mAtcRadioResponse->
                vsimSendCmdResponse(responseInfo, convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, ATC_SERVICE);
    } else {
        RLOGE("vsimSendCmdResponse: radioService[%d]->mAtcRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getPreferredNetworkTypeExtResponse(int slotId,
                                          int responseType, int serial, RIL_Errno e,
                                          void *response, size_t responseLen) {
#if VDBG
    RLOGD("getPreferredNetworkTypeExtResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntExt(responseInfo, serial, responseType, e, response, responseLen);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->getPreferredNetworkTypeExtResponse(
                responseInfo, (ExtPreferredNetworkType) ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getPreferredNetworkTypeExtResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setRadioPowerFallbackResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setRadioPowerFallbackResponse: serial %d", serial);
#endif
    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setRadioPowerFallbackResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setRadioPowerFallbackResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getCnapResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getCNAPResponse: serial %d", serial);
#endif


    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        int active = -1, status = -1;
        int numInts = responseLen / sizeof(int);
        if (response == NULL || numInts != 2) {
            RLOGE("getCNAPResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            int *pInt = (int *)response;
            active = pInt[0];
            status = pInt[1];
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->getCnapResponse(
                responseInfo, active, status);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getCNAPResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::setLocationInfoResponse(int slotId,
                               int responseType, int serial, RIL_Errno e,
                               void *response, size_t responseLen) {
#if VDBG
    RLOGD("setLocationInfoResponse : serial %d", serial);
#endif
    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setLocationInfoResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setLocationInfoResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSpecialRatcapResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getSpecialRatcapResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSpecialRatcapResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSpecialRatcapResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getVideoResolutionResponse(int slotId, int responseType, int serial,
                                      RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVideoResolutionResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getVideoResolutionResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getVideoResolutionResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getImsPaniInfoResponse(int slotId, int responseType, int serial,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
#if VDBG
    RLOGD("getImsPaniInfoResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        ImsNetworkInfo resp = {};

        if (response == NULL || responseLen % sizeof(IMS_NetworkInfo) != 0) {
            RLOGE("getImsPaniInfoResponse Invalid response: NULL");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            IMS_NetworkInfo *pNetInfo = (IMS_NetworkInfo *)response;
            resp.type = pNetInfo->type;
            resp.age = pNetInfo->age;
            resp.info = convertCharPtrToHidlString(pNetInfo->info);
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getImsPaniInfoResponse(responseInfo, resp);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getImsPaniInfoResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setEmergencyOnlyResponse(int slotId, int responseType, int serial,
                                 RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
RLOGD("setEmergencyOnlyResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                    setEmergencyOnlyResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setEmergencyOnlyResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::getSubsidyLockdyStatusResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getSubsidyLockdyStatusResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSubsidyLockdyStatusResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSubsidyLockdyStatusResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setImsUserAgentResponse(int slotId, int responseType, int serial,
                              RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setImsUserAgentResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setImsUserAgentResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setImsUserAgentResponse: radioService[%d]->mExtRadioResponse == NULL",
               slotId);
    }

    return 0;
}

int radio::resetModemResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen){
#if VDBG
   RLOGD("resetModemResponse: serial %d", serial);
#endif

   if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                resetModemResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("resetModemResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

   return 0;
}

int radio::getVoLTEAllowedPLMNResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getVoLTEAllowedPLMNResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getVoLTEAllowedPLMNResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getVoLTEAllowedPLMNResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}

int radio::setSmsBearerResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("setSmsBearerResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->setSmsBearerResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setSmsBearerResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getSmsBearerResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
    RLOGD("getSmsBearerResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                                    response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getSmsBearerResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getSmsBearerResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::queryRootNodeResponse(int slotId, int responseType, int serial, RIL_Errno e,
                                 void *response, size_t responseLen) {
#if VDBG
    RLOGD("queryRootNodeResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                queryRootNodeResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("queryRootNodeResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }

    return 0;
}


int radio::setPsDataOffResponse(int slotId, int responseType, int serial,
                                RIL_Errno e, void *response,
                                size_t responseLen) {
#if VDBG
     RLOGD("setPsDataOffResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->setPsDataOffResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setSmsBearerResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::requestLteSpeedAndSignalStrengthResponse(int slotId, int responseType,
        int serial, RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("requestLteSpeedAndSignalStrengthResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        LteSpeedAndSignalStrength speedAndStrength = {};
        RIL_LTE_SpeedAndSignalStrength *rilLteSpeedAndSignalStrength =
                (RIL_LTE_SpeedAndSignalStrength*) response;

        if (response == NULL || responseLen != sizeof(RIL_LTE_SpeedAndSignalStrength)) {
            RLOGE("requestLteSpeedAndSignalStrengthResponse: Invalid response");
            if (e == RIL_E_SUCCESS) responseInfo.error = ExtRadioError::INVALID_RESPONSE;
        } else {
            speedAndStrength.rsrp = rilLteSpeedAndSignalStrength->rsrp;
            speedAndStrength.txSpeed = rilLteSpeedAndSignalStrength->txSpeed;
            speedAndStrength.rxSpeed = rilLteSpeedAndSignalStrength->rxSpeed;
            speedAndStrength.snr = rilLteSpeedAndSignalStrength->snr;
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                requestLteSpeedAndSignalStrengthResponse(responseInfo, speedAndStrength);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("requestLteSpeedAndSignalStrengthResponse: "
                "radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::enableNrSwitchResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("enableNrSwitchResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->enableNrSwitchResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("enableNrSwitchResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }
    return 0;
}

int radio::setUsbShareStateSwitchResponse(int slotId, int responseType, int serial,
                                     RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setUsbShareStateSwitchResponse: serial %d", serial);
#endif
    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
    	ExtRadioResponseInfo responseInfo = {};
    	populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                setUsbShareStateSwitchResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setUsbShareStateSwitchResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }
    return 0;
}

int radio::setStandAloneResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("setStandAloneResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        populateResponseInfoExt(responseInfo, serial, responseType, e);
        Return<void> retStatus
                = extRadioService[slotId]->mExtRadioResponse->setStandAloneResponse(responseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("setStandAloneResponse: radioService[%d]->mExtRadioResponse == NULL", slotId);
    }

    return 0;
}

int radio::getStandAloneResponse(int slotId, int responseType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
#if VDBG
    RLOGD("getStandAloneResponse: serial %d", serial);
#endif

    if (extRadioService[slotId]->mExtRadioResponse != NULL) {
        ExtRadioResponseInfo responseInfo = {};
        int ret = responseIntOrEmptyExt(responseInfo, serial, responseType, e,
                response, responseLen);
        Return<void> retStatus = extRadioService[slotId]->mExtRadioResponse->
                getStandAloneResponse(responseInfo, ret);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("getStandAloneResponse: radioService[%d]->mExtRadioResponse == NULL",
                slotId);
    }
    return 0;
}

/******************************************************************************/
/*    UNISOC extended unsolicited interfaces' corresponding responsFunction   */
/******************************************************************************/
ExtRadioIndicationType convertIntToRadioIndicationTypeExt(int indicationType) {
    return indicationType == RESPONSE_UNSOLICITED ? (ExtRadioIndicationType::UNSOLICITED) :
            (ExtRadioIndicationType::UNSOLICITED_ACK_EXP);
}

extern "C" int updatePlmn(int slotId, int lac, const char *mncmcc, char *resp, size_t respLen) {
    int ret = -1;
    if (slotId >= SIM_COUNT || slotId < 0 || mncmcc == NULL) {
        RLOGE("Invalid params");
        return ret;
    }

    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("updatePlmn");
#endif

        std::string pStr;
        auto cb = [&](hidl_string updatedResp) {
            pStr = updatedResp.c_str();
        };

        Return<void> status  = extRadioService[slotId]->mExtRadioIndication->
                updatePlmn(hidl_string(mncmcc), lac, cb);
        if (!status.isOk()) {
            RLOGE("Failed to updatePlmn");
        } else {
            ret = 0;
            snprintf(resp, respLen, "%s", pStr.c_str());
        }
    } else {
        RLOGE("updatePlmn: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }
    return ret;
}

extern "C" int updateNetworkList(int slotId, char **networkList, size_t datalen,
                      char *resp, size_t respLen) {
    int ret = -1;
    if (slotId >= SIM_COUNT || slotId < 0 || networkList == NULL ||
            datalen % sizeof(char *) != 0) {
        RLOGE("Invalid params");
        return ret;
    }

    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("updateNetworkList");
#endif

        std::string pStr;
        auto cb = [&](hidl_string updatedResp) {
            pStr = updatedResp.c_str();
        };

        hidl_vec<hidl_string> data;
        char **list = (char **)networkList;
        int numStrings = datalen / sizeof(char *);
        data.resize(numStrings);
        for (int i = 0; i < numStrings; i++) {
            data[i] = convertCharPtrToHidlString(list[i]);
        }

        Return<void> status  = extRadioService[slotId]->mExtRadioIndication->
                updateNetworkList(data, cb);
        if (!status.isOk()) {
            RLOGE("Failed to updatePlmn");
        } else {
            ret = 0;
            snprintf(resp, respLen, "%s", pStr.c_str());
        }
    } else {
        RLOGE("updateNetworkList: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }
    return ret;
}

int radio::videoPhoneCodecInd(int slotId, int indicationType, int token,
                              RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("videoPhoneCodecInd: invalid response");
            return 0;
        }

        hidl_vec<int32_t> data;
        int *pInt = (int *)response;
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }
#if VDBG
        RLOGD("videoPhoneCodecInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneCodecInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneCodecInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneDSCIInd(int slotId, int indicationType, int serial,
                             RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_VideoPhone_DSCI)) {
            RLOGE("videoPhoneDSCIInd: invalid response");
            return 0;
        }

        VideoPhoneDSCI vpDSCI = {};
        RIL_VideoPhone_DSCI *pDSCI = (RIL_VideoPhone_DSCI *)response;
        vpDSCI.id = pDSCI->id;
        vpDSCI.idr = pDSCI->idr;
        vpDSCI.stat = pDSCI->stat;
        vpDSCI.type = pDSCI->type;
        vpDSCI.mpty = pDSCI->mpty;
        vpDSCI.number = convertCharPtrToHidlString(pDSCI->number);
        vpDSCI.numType = pDSCI->num_type;
        vpDSCI.bsType = pDSCI->bs_type;
        vpDSCI.cause = pDSCI->cause;
        vpDSCI.location = pDSCI->location;

#if VDBG
        RLOGD("videoPhoneDSCIInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneDSCIInd(convertIntToRadioIndicationTypeExt(indicationType), vpDSCI);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneDSCIInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneStringInd(int slotId, int indicationType, int token,
                               RIL_Errno e, void *response,
                               size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("videoPhoneStringInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("videoPhoneStringInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneStringInd(convertIntToRadioIndicationTypeExt(indicationType),
                                    convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneStringInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneRemoteMediaInd(int slotId, int indicationType, int token,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("videoPhoneRemoteMediaInd: invalid response");
            return 0;
        }

        hidl_vec<int32_t> data;
        int *pInt = (int *)response;
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }
#if VDBG
        RLOGD("videoPhoneRemoteMediaInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneRemoteMediaInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneRemoteMediaInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneMMRingInd(int slotId, int indicationType,
                          int token, RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("videoPhoneMMRingInd: invalid response");
            return 0;
        }
        int32_t data = ((int32_t *)response)[0];
#if VDBG
        RLOGD("videoPhoneMMRingInd: %d", data);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->videoPhoneMMRingInd(
                convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneMMRingInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneReleasingInd(int slotId, int indicationType, int token,
                                  RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("videoPhoneReleasingInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("videoPhoneReleasingInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneReleasingInd(convertIntToRadioIndicationTypeExt(indicationType),
                                       convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneReleasingInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneRecordVideoInd(int slotId, int indicationType,
                                    int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("videoPhoneRecordVideoInd: invalid response");
            return 0;
        }
        int32_t data = ((int32_t *)response)[0];
#if VDBG
        RLOGD("videoPhoneRecordVideoInd: %d", data);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->videoPhoneRecordVideoInd(
                convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneRecordVideoInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoPhoneMediaStartInd(int slotId, int indicationType,
                                    int token, RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("videoPhoneMediaStartInd: invalid response");
            return 0;
        }
        int32_t data = ((int32_t *)response)[0];
#if VDBG
        RLOGD("videoPhoneMediaStartInd: %d", data);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoPhoneMediaStartInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoPhoneMediaStartInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::rauSuccessInd(int slotId, int indicationType, int token,
                         RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("rauSuccessInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                rauSuccessInd(convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("rauSuccessInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::clearCodeFallbackInd(int slotId, int indicationType, int token,
                                RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("clearCodeFallbackInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                clearCodeFallbackInd(convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("clearCodeFallbackInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::extRILConnectedInd(int slotId, int indicationType, int token,
                              RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("extRILConnectedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                rilConnectedInd(convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("extRILConnectedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simlockSimExpiredInd(int slotId, int indicationType,
                                int token, RIL_Errno e, void *response,
                                size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("simlockSimExpiredInd: invalid response");
            return 0;
        }
        int32_t simID = ((int32_t *)response)[0];
#if VDBG
        RLOGD("simlockSimExpiredInd: %d", simID);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                simlockSimExpiredInd(convertIntToRadioIndicationTypeExt(indicationType), simID);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simlockSimExpiredInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::networkErrorCodeInd(int slotId, int indicationType, int token,
                          RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("videoPhoneRemoteMediaInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("networkErrorCodeInd");
#endif
        hidl_vec<int32_t> data;
        int *pInt = (int *)response;
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                networkErrorCodeInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("networkErrorCodeInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::simMgrSimStatusChangedInd(int slotId, int indicationType, int token,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("simMgrSimStatusChangedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                simMgrSimStatusChangedInd(convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("simMgrSimStatusChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::earlyMediaInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("earlyMediaInd: invalid response");
            return 0;
        }
        int32_t id = ((int32_t *)response)[0];
#if VDBG
        RLOGD("earlyMediaInd: %d", id);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                earlyMediaInd(convertIntToRadioIndicationTypeExt(indicationType), id);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("earlyMediaInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::availableNetworksInd(int slotId, int indicationType, int token,
                                        RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(char *) != 0) {
            RLOGE("availableNetworksInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("availableNetworksInd");
#endif
        hidl_vec<hidl_string> data;

        char **resp = (char **)response;
        int numStrings = responseLen / sizeof(char *);
        data.resize(numStrings);
        for (int i = 0; i < numStrings; i++) {
           data[i] = convertCharPtrToHidlString(resp[i]);
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                availableNetworksInd(convertIntToRadioIndicationTypeExt(indicationType),
                        data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("availableNetworksInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSCallStateChangedInd(int slotId, int indicationType, int token,
                                  RIL_Errno e, void *response,
                                  size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
#if VDBG
        RLOGD("IMSCallStateChangedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSCallStateChangedInd(convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSCallStateChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::videoQualityInd(int slotId, int indicationType, int token,
                           RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("videoQualityInd: invalid response");
            return 0;
        }

        hidl_vec<int32_t> data;
        int *pInt = (int *)response;
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }
#if VDBG
        RLOGD("videoQualityInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                videoQualityInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("videoQualityInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSBearerEstablished(int slotId, int indicationType, int token,
                                RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("IMSBearerEstablished: invalid response");
            return 0;
        }
        int32_t state = ((int32_t *)response)[0];
#if VDBG
        RLOGD("IMSBearerEstablished");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSBearerEstablished(convertIntToRadioIndicationTypeExt(indicationType), state);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSBearerEstablished: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSHandoverRequestInd(int slotId, int indicationType, int token,
                                 RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("IMSHandoverRequestInd: invalid response");
            return 0;
        }
        int32_t status = ((int32_t *)response)[0];
#if VDBG
        RLOGD("IMSHandoverRequestInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSHandoverRequestInd(convertIntToRadioIndicationTypeExt(indicationType), status);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSHandoverRequestInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSHandoverStatusChangedInd(int slotId, int indicationType, int token,
                                       RIL_Errno e, void *response,
                                       size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("IMSHandoverStatusChangedInd: invalid response");
            return 0;
        }
        int32_t status = ((int32_t *)response)[0];
#if VDBG
        RLOGD("IMSHandoverStatusChangedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSHandoverStatusChangedInd(convertIntToRadioIndicationTypeExt(indicationType), status);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSHandoverStatusChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSNetworkInfoChangedInd(int slotId, int indicationType, int token,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(IMS_NetworkInfo)) {
            RLOGE("IMSNetworkInfoChangedInd: invalid response");
            return 0;
        }

        ImsNetworkInfo nwInfo = {};
        IMS_NetworkInfo *pInfo = (IMS_NetworkInfo *)response;
        nwInfo.type = pInfo->type;
        nwInfo.info = convertCharPtrToHidlString(pInfo->info);

#if VDBG
        RLOGD("IMSNetworkInfoChangedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSNetworkInfoChangedInd(convertIntToRadioIndicationTypeExt(indicationType), nwInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSNetworkInfoChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSRegisterAddressChangedInd(int slotId, int indicationType, int token,
                                        RIL_Errno e, void *response,
                                        size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(char *) != 0) {
            RLOGE("IMSRegisterAddressChangedInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("IMSRegisterAddressChangedInd");
#endif
        hidl_vec<hidl_string> data;

        char **resp = (char **)response;
        int numStrings = responseLen / sizeof(char *);
        data.resize(numStrings);
        for (int i = 0; i < numStrings; i++) {
           data[i] = convertCharPtrToHidlString(resp[i]);
        }

        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSRegisterAddressChangedInd(convertIntToRadioIndicationTypeExt(indicationType),
                        data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSRegisterAddressChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSWifiParamInd(int slotId, int indicationType, int token,
                           RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("IMSWifiParamInd: invalid response");
            return 0;
        }

        hidl_vec<int32_t> data;
        int *pInt = (int *)response;
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }
#if VDBG
        RLOGD("IMSWifiParamInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSWifiParamInd(convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSWifiParamInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSNetworkStateChangedInd(int slotId, int indicationType, int token,
                                     RIL_Errno e, void *response,
                                     size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("IMSNetworkStateChangedInd: invalid response");
            return 0;
        }
        int32_t resp = ((int32_t *)response)[0];
#if VDBG
        RLOGD("IMSNetworkStateChangedInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSNetworkStateChangedInd(convertIntToRadioIndicationTypeExt(indicationType), resp);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSNetworkStateChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::updateHdStateInd(int slotId, int indicationType,
                         int token, RIL_Errno e, void *response,
                         size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("updateHdStateInd: invalid response");
            return 0;
        }
        int32_t id = ((int32_t *)response)[0];
#if VDBG
        RLOGD("updateHdStateInd: %d", id);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                updateHdStateInd(convertIntToRadioIndicationTypeExt(indicationType), id);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("updateHdStateInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::subsidyLockStatusChangedInd(int slotId, int indicationType,
                                       int token, RIL_Errno e, void *response,
                                       size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(int)) {
            RLOGE("subsidyLockStatusChangedInd: invalid response");
            return 0;
        }
        int32_t status = ((int32_t *)response)[0];
#if VDBG
        RLOGD("subsidyLockStatusChangedInd: %d", status);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                subsidyLockStatusChangedInd(convertIntToRadioIndicationTypeExt(indicationType), status);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("subsidyLockStatusChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSCsfbVendorCauseInd(int slotId, int indicationType, int token,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("IMSCsfbVendorCauseInd: invalid response");
            return 0;
        }

#if VDBG
        RLOGD("IMSCsfbVendorCauseInd");
#endif

        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSCsfbVendorCauseInd(convertIntToRadioIndicationTypeExt(indicationType),
                                      convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSCsfbVendorCauseInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::IMSErrorCauseInd(int slotId, int indicationType, int token,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL ||  responseLen != sizeof(IMS_ErrorCause)) {
            RLOGE("IMSErrorCauseInd: invalid response");
            return 0;
        }
        ImsErrorCauseInfo errorCauseInfo = {};
        IMS_ErrorCause *pInfo = (IMS_ErrorCause *)response;
        errorCauseInfo.type = pInfo->type;
        errorCauseInfo.errCode = pInfo->errCode;
        errorCauseInfo.errDescription = convertCharPtrToHidlString(pInfo->errDescription);

#if VDBG
        RLOGD("IMSErrorCauseInd");
#endif

        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                IMSErrorCauseInd(convertIntToRadioIndicationTypeExt(indicationType),
                        errorCauseInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("IMSErrorCauseInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::cnapInd(int slotId,
                   int indicationType, int token, RIL_Errno e, void *response,
                   size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("cnapInd: invalid name in +CNAP");
            return 0;
        }
#if VDBG
        RLOGD("cnapInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                cnapInd(convertIntToRadioIndicationTypeExt(indicationType),
                        convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("cnapInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::signalConnStatusInd(int slotId, int indicationType, int token,
                                    RIL_Errno e, void *response,
                                    size_t responseLen) {
    if (radioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_SingnalConnStatus)) {
            RLOGE("signalConnStatusInd: invalid response");
            return 0;
        }

        SignalConnStatus sigConnStatus = {};
        RIL_SingnalConnStatus *pInfo = (RIL_SingnalConnStatus *)response;
        sigConnStatus.mode = pInfo->mode;
        sigConnStatus.state = pInfo->state;
        sigConnStatus.access = pInfo->access;
        sigConnStatus.coreNetwork = pInfo->coreNetwork;

#if VDBG
        RLOGD("signalConnStatusInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->
                signalConnStatusInd(convertIntToRadioIndicationTypeExt(indicationType), sigConnStatus);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("signalConnStatusInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::smartNrChangedInd(int slotId,
                             int indicationType, int token, RIL_Errno e, void *response,
                             size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {

#if VDBG
        RLOGD("smart5GStatusInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->smartNrChangedInd(
                convertIntToRadioIndicationTypeExt(indicationType));
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("smart5GStatusInd: radioService[%d]->mExtRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::nrCfgInfoInd(int slotId,
                        int indicationType, int token, RIL_Errno e, void *response,
                        size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen % sizeof(int) != 0) {
            RLOGE("nrConfigInfoInd: invalid response");
            return 0;
        }

        hidl_vec<int32_t> data;
        int* pInt = ((int *) response);
        int numInts = responseLen / sizeof(int);
        data.resize(numInts);
        for (int i = 0; i < numInts; i++) {
            data[i] = (int32_t)pInt[i];
        }

#if VDBG
        RLOGD("nrConfigInfoInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->nrCfgInfoInd(
                convertIntToRadioIndicationTypeExt(indicationType), data);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("nrConfigInfoInd: radioService[%d]->mRadioIndication == NULL",
                slotId);
    }

    return 0;
}

int radio::modemStateChangedInd(int slotId, int indicationType, int token,
                                RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL && extRadioService[slotId]->mExtRadioIndication != NULL) {
        if (response == NULL || responseLen != sizeof(RIL_ModemStatusInfo)) {
            RLOGE("modemStateChangedInd: invalid response");
            return 0;
        }

        ModemStatusInfo msInfo = {};
        RIL_ModemStatusInfo *pInfo = (RIL_ModemStatusInfo *)response;
        msInfo.status = (ModemDetailStatus) pInfo->modemState;
        msInfo.assertInfo = convertCharPtrToHidlString(pInfo->assertInfo);

#if VDBG
        RLOGD("modemStateChangedInd state: %d, info: %s", msInfo.status, pInfo->assertInfo);
#endif
        Return<void> retStatus = extRadioService[slotId]->mExtRadioIndication->modemStateChangedInd(
                convertIntToRadioIndicationTypeExt(indicationType), msInfo);
        extRadioService[slotId]->checkReturnStatus(retStatus, RADIOINTERACTOR_SERVICE);
    } else {
        RLOGE("modemStateChangedInd: radioService[%d]->mExtRadioIndication == NULL", slotId);
    }

    return 0;
}

/******************************************************************************/
/*               UNISOC extended atc interfaces' responsFunction              */
/******************************************************************************/
int radio::vsimRSimReqInd(int slotId, int indicationType, int token,
                       RIL_Errno e, void *response, size_t responseLen) {
    if (extRadioService[slotId] != NULL &&
            extRadioService[slotId]->mAtcRadioIndication != NULL) {
        if (response == NULL || responseLen == 0) {
            RLOGE("vsimRSimReqInd: invalid response");
            return 0;
        }
#if VDBG
        RLOGD("vsimRSimReqInd");
#endif
        Return<void> retStatus = extRadioService[slotId]->mAtcRadioIndication->
                vsimRSimReqInd(convertIntToRadioIndicationTypeExt(indicationType),
                            convertCharPtrToHidlString((char *)response));
        extRadioService[slotId]->checkReturnStatus(retStatus, ATC_SERVICE);
    } else {
        RLOGE("vsimRSimReqInd: radioService[%d]->mAtcRadioIndication == NULL", slotId);
    }

    return 0;
}

int radio::getServiceIdBySocketId(int socket_id) {
    int ret = socket_id;
    for (int i = 0; i < SIM_COUNT; i++) {
        if (radioService[i] != NULL && radioService[i]->mSlotId == socket_id) {
            ret = i;
            break;
        }
    }
    return ret;
}
