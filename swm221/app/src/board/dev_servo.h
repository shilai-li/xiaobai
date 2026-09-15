#ifndef __DEV_SERVO_H__
#define __DEV_SERVO_H__

#include "SWM221.h"

/**
 * @brief Initialize dual servos on PB13 (PWM0A) and PB12 (PWM0B)
 */
void servo_init(void);

/**
 * @brief Set servo angle
 * @param index 0 for Servo 1 (PB13), 1 for Servo 2 (PB12)
 * @param angle Target angle (0 to 180 degrees)
 */
void servo_set_angle(uint8_t index, uint8_t angle);
void servo_set_target(uint8_t index, uint8_t angle);
void servo_run_tick(void);

#endif // __DEV_SERVO_H__
