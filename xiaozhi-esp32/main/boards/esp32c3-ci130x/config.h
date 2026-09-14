#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// CI130x using UART0 Controller on GPIO 0/1 (Clean pins)
#define CI130X_UART_NUM          UART_NUM_0
#define CI130X_UART_BAUD_RATE    921600
#define CI130X_UART_GPIO_TX      GPIO_NUM_0
#define CI130X_UART_GPIO_RX      GPIO_NUM_1

// ML307 4G using UART1 Controller on GPIO 2/3 (Clean pins)
#define ML307_UART_NUM           UART_NUM_1
#define ML307_TX_PIN             GPIO_NUM_2
#define ML307_RX_PIN             GPIO_NUM_3
#define ML307_DTR_PIN            GPIO_NUM_NC

#define BUILTIN_LED_GPIO         GPIO_NUM_8 // Default for some C3 boards, adjust if needed
#define BOOT_BUTTON_GPIO         GPIO_NUM_9 // Default for C3 Boot button

#endif // _BOARD_CONFIG_H_
