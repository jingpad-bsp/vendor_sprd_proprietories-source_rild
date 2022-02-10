/**
 * Copyright (C) 2019 UNISOC Technologies Co.,Ltd.
 */

#ifndef RIL_THERMAL_H_
#define RIL_THERMAL_H_

#ifdef __cplusplus
extern "C" {
#endif

void setCPUFrequency(bool enable);
void setThermal(bool enable);

#ifdef __cplusplus
}
#endif

#endif  // RIL_THERMAL_H_
