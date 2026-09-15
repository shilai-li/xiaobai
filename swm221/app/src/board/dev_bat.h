#ifndef __DEV_BAT_H__
#define __DEV_BAT_H__

#include "SWM221.h"

void bat_monitor_init(void);
void bat_monitor_enable(void);
void bat_monitor_disable(void);
uint32_t bat_voltage_read(void);
void bat_voltage_task(void);

#endif // __DEV_BAT_H__
