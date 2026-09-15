#include "dev_servo.h"
#include <math.h>
#include <stdlib.h>

// Servo Pins (V1.10 Schematic)
#define SERVO1_PORT      PORTB
#define SERVO1_PIN       PIN13
#define SERVO1_FUNMUX    PORTB_PIN13_PWM0A

#define SERVO2_PORT      PORTB
#define SERVO2_PIN       PIN12
#define SERVO2_FUNMUX    PORTB_PIN12_PWM0B

// Servo Power Enable (PB15)
#define SERVO_EN_GPIO    GPIOB
#define SERVO_EN_PIN     PIN14

#define SERVO_PWM        PWM0
#define SERVO_PWM_MSK    PWM0_MSK

// Software Calibration Offsets (Adjust these to center your servos)
#define SERVO1_OFFSET    0  // Offset for Servo 1 (PB13) in degrees
#define SERVO2_OFFSET    0  // Offset for Servo 2 (PB12) in degrees

static float s_curr_angle[2] = {90, 90};
static float s_target_angle[2] = {90, 90};
static float s_step_speed[2] = {2.0f, 2.0f};

/**
 * @brief Initialize PWM for Dual Servos (50Hz)
 */
void servo_init(void)
{
    // 1. Enable Servo Power (PB14)
    GPIO_Init(SERVO_EN_GPIO, SERVO_EN_PIN, 1, 0, 0, 0);
    GPIO_SetBit(SERVO_EN_GPIO, SERVO_EN_PIN);

    // 2. Configure GPIO pins as PWM output
    PORT_Init(SERVO1_PORT, SERVO1_PIN, SERVO1_FUNMUX, 0);
    PORT_Init(SERVO2_PORT, SERVO2_PIN, SERVO2_FUNMUX, 0);

    // 3. Configure PWM parameters
    PWM_InitStructure  PWM_initStruct;
    PWM_initStruct.Mode = PWM_EDGE_ALIGNED;
    
    extern uint32_t CyclesPerUs;
    
    // F_PWM = SystemCoreClock / Clkdiv = 1,000,000 Hz (1 tick = 1 microsecond)
    PWM_initStruct.Clkdiv = CyclesPerUs;    
    if (PWM_initStruct.Clkdiv == 0) PWM_initStruct.Clkdiv = 1;

    // 50Hz = 20ms = 20,000 microseconds
    PWM_initStruct.Period = 20000; 

    extern uint32_t bat_voltage_read(void);
    srand(SysTick->VAL ^ bat_voltage_read());

    // Initial position: Both servos at 90 degrees for production calibration
    s_curr_angle[1] = 90;
    s_target_angle[1] = 90;
    
    int16_t angle1 = 90 + SERVO1_OFFSET;
    int16_t angle2 = 90 + SERVO2_OFFSET;
    
    if (angle1 < 0) angle1 = 0; if (angle1 > 180) angle1 = 180;
    if (angle2 < 0) angle2 = 0; if (angle2 > 180) angle2 = 180;

    PWM_initStruct.HdutyA = 500 + ((uint32_t)angle1 * 2000 / 180);
    PWM_initStruct.HdutyB = 500 + ((uint32_t)angle2 * 2000 / 180);
    
    // Common settings
    PWM_initStruct.DeadzoneA = 0;
    PWM_initStruct.IdleLevelA = 0;
    PWM_initStruct.IdleLevelAN= 0;
    PWM_initStruct.OutputInvA = 0;
    PWM_initStruct.OutputInvAN= 0;
    PWM_initStruct.DeadzoneB = 0;
    PWM_initStruct.IdleLevelB = 0;
    PWM_initStruct.IdleLevelBN= 0;
    PWM_initStruct.OutputInvB = 0;
    PWM_initStruct.OutputInvBN= 0;
    PWM_initStruct.UpOvfIE    = 0;
    PWM_initStruct.DownOvfIE  = 0;
    PWM_initStruct.UpCmpAIE   = 0;
    PWM_initStruct.DownCmpAIE = 0;
    PWM_initStruct.UpCmpBIE   = 0;
    PWM_initStruct.DownCmpBIE = 0;

    // 4. Initialize and Start PWM
    PWM_Init(SERVO_PWM, &PWM_initStruct);
    PWM_Start(SERVO_PWM_MSK);
}

/**
 * @brief Set the servo angle
 * @param index 0 for Servo 1 (PB13), 1 for Servo 2 (PB12)
 * @param angle Target angle (0 to 180 degrees)
 */
void servo_set_angle(uint8_t index, uint8_t angle)
{
    int16_t mapped_angle;

    // Map logical 0-180 degrees from CI1303 to the physical servo ranges
    if (index == 0) {
        // Servo 1 (Side-to-Side): map 0-180 to 30-150
        // mapped = 30 + angle * (150 - 30) / 180 = 30 + angle * 2 / 3
        mapped_angle = 30 + ((int16_t)angle * 2 / 3);
    } else {
        // Servo 2 (Nodding): use the angle directly, as it's not DOA
        mapped_angle = angle;
    }

    int16_t real_angle = mapped_angle;

    // Apply software offset
    if (index == 0) {
        real_angle += SERVO1_OFFSET;
    } else {
        real_angle += SERVO2_OFFSET;
    }

    // Clamp to physical limits to protect the mechanism
    // This acts as a final safety check after the offset is applied
    if (index == 0) {
        // Servo 1 (Side-to-Side): 30 to 150 degrees
        if (real_angle < 30) real_angle = 30;
        if (real_angle > 150) real_angle = 150;
    } else {
        // Servo 2 (Nodding): 80 to 125 degrees
        if (real_angle < 80) real_angle = 80;
        if (real_angle > 125) real_angle = 125;
    }

    // Map 0-180 degrees to 500 - 2500 microseconds width
    uint32_t target_width_us = 500 + ((uint32_t)real_angle * 2000 / 180);

    PWM_ReloadDis(SERVO_PWM_MSK);
    if (index == 0) {
        PWM_SetHDuty(SERVO_PWM, PWM_CH_A, target_width_us);
    } else {
        PWM_SetHDuty(SERVO_PWM, PWM_CH_B, target_width_us);
    }
    PWM_ReloadEn(SERVO_PWM_MSK);
}

/**
 * @brief Update target angle for smooth movement
 */
void servo_set_target(uint8_t index, uint8_t angle)
{
    if(index > 1) return;
    s_target_angle[index] = angle;
}

/**
 * @brief Smoothly move servo towards target in main loop
 * Call this in main_tick()
 */
void servo_run_tick(void)
{
    // 1. Trigger random nodding (vertical movement)
    // If horizontal servo is moving, set a random nodding target occasionally
    if (fabs(s_curr_angle[0] - s_target_angle[0]) > 2.0f) {
        if ((rand() % 100) < 10) { // 10% probability to trigger a nod
            s_target_angle[1] = 85 + (rand() % 35); // Random angle 85 ~ 120
        }
    }

    // 2. Incrementally move servos towards targets
    for (int i = 0; i < 2; i++) {
        if (fabs(s_curr_angle[i] - s_target_angle[i]) > 0.5f) {
            // Reduced speed for smoother movement (0.3 to 1.0)
            s_step_speed[i] = 0.3f + (float)(rand() % 7) / 10.0f;
            
            // Increase side-to-side (left/right) speed by 20%
            if (i == 0) {
                s_step_speed[i] *= 1.2f;
            }
            
            if (s_curr_angle[i] < s_target_angle[i]) {
                s_curr_angle[i] += s_step_speed[i];
                if (s_curr_angle[i] > s_target_angle[i]) s_curr_angle[i] = s_target_angle[i];
            } else {
                s_curr_angle[i] -= s_step_speed[i];
                if (s_curr_angle[i] < s_target_angle[i]) s_curr_angle[i] = s_target_angle[i];
            }
            // Update hardware PWM width
            servo_set_angle(i, (uint8_t)s_curr_angle[i]);
        }
    }
}
