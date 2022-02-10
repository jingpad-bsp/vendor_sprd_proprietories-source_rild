/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#define LOG_TAG "RIL_SE"

#include <android/hardware/secure_element/1.0/ISecureElement.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/ProcessState.h>
#include <se_service.h>
#include <inttypes.h>

using namespace android::hardware::secure_element;
using namespace android::hardware::secure_element::V1_0;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using android::sp;

#define FREE(data)    \
{                            \
        free(data);          \
        data = NULL;         \
}

SE_Functions *s_seFunctions = NULL;
struct SecureElementImpl;

#if (SIM_COUNT >= 2)
sp<SecureElementImpl> secureElementService[SIM_COUNT];
#else
sp<SecureElementImpl> secureElementService[1];;
#endif

extern int rilSlotMapping(int logicSlotId);

uint8_t *convertHexStringToBytes(void *response, size_t responseLen);

struct SecureElementImpl: public ISecureElement {
    int32_t mSlotId;

    Return<void> init(const ::android::sp<ISecureElementHalCallback>& clientCallback);

    Return<void> getAtr(getAtr_cb _hidl_cb);

    Return<bool> isCardPresent();

    Return<void> transmit(const ::android::hardware::hidl_vec<uint8_t>& data,
                          transmit_cb _hidl_cb);

    Return<void> openLogicalChannel(
            const ::android::hardware::hidl_vec<uint8_t>& aid, uint8_t p2,
            openLogicalChannel_cb _hidl_cb);

    Return<void> openBasicChannel(
            const ::android::hardware::hidl_vec<uint8_t>& aid, uint8_t p2,
            openBasicChannel_cb _hidl_cb);

    Return<SecureElementStatus> closeChannel(uint8_t channelNumber);
};

Return<void> SecureElementImpl::init(const sp<ISecureElementHalCallback>& clientCallback) {
#if VDBG
    RLOGD("secure element init: mSlotId %d", mSlotId);
#endif
    bool ret = true;  // bug1063494, need to return true
    // ret = s_seFunctions->initForSeService(mSlotId);
    Return<void> status = clientCallback->onStateChange(ret);
    if (!status.isOk()) {
        RLOGE("se client has died");
    }

    return Void();
}

Return<void> SecureElementImpl::getAtr(getAtr_cb _hidl_cb) {
#if VDBG
    RLOGD("secure element getAtr");
#endif
    hidl_vec<uint8_t> response;
    uint8_t *bytes = NULL;
    char resp[4096] = {0};
    int respLen = 0;

    s_seFunctions->getAtrForSeService(mSlotId, resp, &respLen);
    bytes = convertHexStringToBytes(resp, respLen);
    if (bytes == NULL) {
        RLOGE("SE transmit convertHexStringToBytes failed");
        goto done;
    }
    response.setToExternal(bytes, respLen / 2);

done:
    _hidl_cb(response);
    FREE(bytes);
    return Void();
}

Return<bool> SecureElementImpl::isCardPresent() {
#if VDBG
    RLOGD("secure element isCardPresent: mSlotId %d", mSlotId);
#endif
    bool ret = false;
    ret = s_seFunctions->isCardPresentForSeService(mSlotId);
    return (bool)ret;
}

Return<void> SecureElementImpl::transmit(const hidl_vec<uint8_t>& data, transmit_cb _hidl_cb) {
#if VDBG
    RLOGD("secure element transmit: mSlotId %d", mSlotId);
#endif
    hidl_vec<uint8_t> response;
    uint8_t *bytes = NULL;
    SE_APDU apdu, resp;
    memset(&apdu, 0, sizeof(SE_APDU));
    memset(&resp,0, sizeof(SE_APDU));

    apdu.len = data.size();
    if (apdu.len == 0) {
        RLOGE("SE transmit data length is zero");
        goto done;
    }

    apdu.data = (uint8_t *)calloc(apdu.len, sizeof(uint8_t));
    if (apdu.data == NULL) {
        RLOGE("SE transmit calloc failed");
        goto done;
    }
    memcpy(apdu.data, data.data(), apdu.len);

    s_seFunctions->transmitForSeService(mSlotId, &apdu, &resp);
    bytes = convertHexStringToBytes(resp.data, resp.len);
    if (bytes == NULL) {
        RLOGE("SE transmit convertHexStringToBytes failed");
        goto done;
    }
    response.setToExternal(bytes, resp.len / 2);

done:
    _hidl_cb(response);
    FREE(apdu.data);
    FREE(resp.data);
    FREE(bytes);
    return Void();
}

Return<void> SecureElementImpl::openLogicalChannel(const hidl_vec<uint8_t>& aid,
        uint8_t p2, openLogicalChannel_cb _hidl_cb) {
#if VDBG
    RLOGD("secure element openLogicalChannel: mSlotId %d", mSlotId);
#endif
    SE_OpenChannelParams params = {};
    uint8_t *uData = NULL;
    int resp[4096] = {0};
    int respLen = 0;
    int numInts = 0;
    size_t aidLen = aid.size();
    SE_Status stat = FAILED;
    LogicalChannelResponse response = {};

    if (aidLen == 0) {
        RLOGD("SE openLogicalChannel aid length is zero");
        // stat = NO_SUCH_ELEMENT_ERROR;  // bug1059826
        // goto done;  // bug1059702, allow aid len == 0
    } else {
        uData = (uint8_t *)calloc(aidLen, sizeof(uint8_t));
        if (uData == NULL) {
            RLOGE("Memory allocation failed for openLogicalChannel");
            goto done;
        }
        memcpy(uData, aid.data(), aidLen);
    }

    params.aidPtr = uData;
    params.p2 = p2;
    params.len = aidLen;
    stat = s_seFunctions->openLogicalChannelForSeService(mSlotId, &params, resp, &respLen);

    numInts = respLen / sizeof(int);
    if (numInts < 1) {
        RLOGE("SE openLogicalChannel failed, final status is %d", stat);
        goto done;
    }

    response.channelNumber = (uint8_t)resp[0];
    response.selectResponse.resize(numInts - 1);
    for (int i = 1; i < numInts; i++) {
        response.selectResponse[i - 1] = (uint8_t) resp[i];
    }

done:
    _hidl_cb(response, (SecureElementStatus)stat);
    FREE(uData);
    return Void();
}

Return<void> SecureElementImpl::openBasicChannel(const hidl_vec<uint8_t>& aid, uint8_t p2, openBasicChannel_cb _hidl_cb) {
    hidl_vec<uint8_t> response;
    SE_Status stat = CHANNEL_NOT_AVAILABLE;

    _hidl_cb(response, (SecureElementStatus)stat);
    return Void();
}

Return<SecureElementStatus> SecureElementImpl::closeChannel(uint8_t channelNumber) {  // channelNumber to be closed
#if VDBG
    RLOGD("secure element closeChannel: mSlotId %d", mSlotId);
#endif
    SE_Status stat = SUCCESS;
    stat = s_seFunctions->closeChannelForSeService(mSlotId, channelNumber);
    return (SecureElementStatus)stat;
}

void secureElement::registerService(SE_Functions *seCallbacks) {
    using namespace android::hardware;
    int simCount = 1;
    const char *serviceNames[] = {
        android::SE_getServiceName()
        #if (SIM_COUNT >= 2)
        , SE_ON_SIM2_SERVICE_NAME
        #if (SIM_COUNT >= 3)
        , SE_ON_SIM3_SERVICE_NAME
        #if (SIM_COUNT >= 4)
        , SE_ON_SIM4_SERVICE_NAME
        #endif
        #endif
        #endif
    };

    #if (SIM_COUNT >= 2)
    simCount = SIM_COUNT;
    #endif

    s_seFunctions = seCallbacks;

    for (int i = 0; i < simCount; i++) {
        int slotId = rilSlotMapping(i);
        secureElementService[i] = new SecureElementImpl;
        secureElementService[i]->mSlotId = slotId;

        RLOGD("SE registerService: starting android::hardware::secure_element::V1_0::ISecureElement %s",
                serviceNames[i]);
        android::status_t status = secureElementService[i]->registerAsService(serviceNames[i]);
        RLOGD("secureElementService registerService: status %d", status);
    }
}
