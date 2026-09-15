#ifndef __DEV_UART_PROCESS_H__
#define __DEV_UART_PROCESS_H__

#include "stdint.h"

void sdisp_handler();
void sdisp_notify(const uint8_t *data, uint32_t len);
void sdisp_write(uint8_t *buf, int len);


#endif