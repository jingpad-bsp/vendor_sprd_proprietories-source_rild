/**
 * ril_se.h --- secure element-related requests process functions declaration
 *
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_SE_H_
#define RIL_SE_H_

bool initForSeService(int simId);
void getAtrForSeService(int simId, void *response, int *responseLen);
bool isCardPresentForSeService(int simId);
void transmitForSeService(int simId, void *data, void *response);
SE_Status openLogicalChannelForSeService(int simId, void *data, void *response, int *responseLen);
SE_Status openBasicChannelForSeService(int simId, void *data, void *response);
SE_Status closeChannelForSeService(int simId, uint8_t channelNumber);

#endif  // RIL_SE_H_
