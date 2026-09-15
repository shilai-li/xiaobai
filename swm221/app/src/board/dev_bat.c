#include "dev_bat.h"

extern void send_audio_play_cmd(uint16_t voice_id);
extern void request_power_off(void);
extern void systick_delay_ms(const uint32_t ms);
extern uint32_t systick_get_tick(void);

#define LOW_BAT_SHUTDOWN_SECONDS 10
#define LOW_BAT_WARN_SECONDS 10
#define LOW_BAT_REMINDER_MS (5UL * 60UL * 1000UL)
#define BAT_ADC_INVALID_VALUE 0xFFFFFFFFUL

static bool s_initialized = false;

void bat_monitor_init(void)
{
    // PA14 as bat_adc (ADC0_CH0)
    PORT_Init(PORTA, PIN14, PORTA_PIN14_ADC0_CH0, 0);

    ADC_InitStructure ADC_initStruct = {0};
    ADC_initStruct.clkdiv = 16;
    ADC_initStruct.samplAvg = ADC_AVG_SAMPLE2; 
    ADC_initStruct.refsrc = ADC_REF_VDD;
    ADC_Init(ADC0, &ADC_initStruct);

    static uint8_t adc_channels[] = {ADC_CH0, 0xF}; // Channel list, terminated by 0xF
    ADC_SEQ_InitStructure ADC_SEQ_initStruct = {0};
    ADC_SEQ_initStruct.channels = adc_channels;
    ADC_SEQ_initStruct.trig_src = ADC_TRIGGER_SW;
    ADC_SEQ_initStruct.conv_cnt = 1;
    ADC_SEQ_initStruct.samp_tim = 20;
    ADC_SEQ_Init(ADC0, ADC_SEQ0, &ADC_SEQ_initStruct);

    ADC_Open(ADC0);
    s_initialized = true;
}

uint32_t bat_voltage_read(void)
{
    ADC_Stop(ADC_SEQ0, 0);
    ADC_Start(ADC_SEQ0, 0);
    uint32_t start_tick = systick_get_tick();
    while(!ADC_DataAvailable(ADC0, ADC_CH0)) {
        if (systick_get_tick() - start_tick >= 5) {
            ADC_Stop(ADC_SEQ0, 0);
            return BAT_ADC_INVALID_VALUE;
        }
    }
    uint32_t adc_val = ADC_Read(ADC0, ADC_CH0);
    ADC_Stop(ADC_SEQ0, 0);
    return adc_val;
}

/* Raw thresholds measured on the target board under normal load. The warning
 * latch rearms above the observed noise band to prevent repeated prompt 501. */
#define BAT_ADC_WARN_THRESHOLD       2146U  /* 3.50 V: observed 2143-2146 */
#define BAT_ADC_WARN_RESET_THRESHOLD 2160U
#define BAT_ADC_SHUTDOWN_THRESHOLD   2091U  /* 3.20 V: observed 2087-2091 */

void bat_voltage_task(void)
{
    static uint32_t last_check_tick = 0;
    static int print_counter = 0;
    static int low_bat_warn_count = 0;
    static int low_bat_shutdown_count = 0;
    static bool low_bat_warned = false;
    static uint32_t low_bat_last_warn_tick = 0;
    
    if (!s_initialized) return;

    uint32_t current_tick = systick_get_tick();
    if (current_tick - last_check_tick >= 1000) { // Check exactly every 1000ms
        last_check_tick = current_tick;
        
        uint32_t adc_val = bat_voltage_read();

        if (adc_val == BAT_ADC_INVALID_VALUE) {
            printf("[BAT DEBUG] ADC conversion timeout; sample ignored\r\n");
            return;
        }
        
        print_counter++;
        if (print_counter >= 5) { // Log every 5 seconds
            print_counter = 0;
            printf("Bat ADC: %d\r\n", adc_val);
        }

        if (adc_val <= BAT_ADC_SHUTDOWN_THRESHOLD) {
            low_bat_shutdown_count++;
            low_bat_warn_count = 0;
            printf("[BAT DEBUG] critical_bat_count: %d (ADC: %d)\r\n",
                   low_bat_shutdown_count, adc_val);
            if (low_bat_shutdown_count == LOW_BAT_SHUTDOWN_SECONDS) {
                printf("Battery critical! ADC: %d. Requesting power-off sequence...\r\n",
                       adc_val);
                request_power_off();
            }
        } else if (adc_val <= BAT_ADC_WARN_THRESHOLD) {
            low_bat_shutdown_count = 0;
            if (!low_bat_warned) {
                low_bat_warn_count++;
                printf("[BAT DEBUG] low_bat_warn_count: %d (ADC: %d)\r\n",
                       low_bat_warn_count, adc_val);
                if (low_bat_warn_count == LOW_BAT_WARN_SECONDS) {
                    printf("Battery low! ADC: %d. Sending audio cmd 501...\r\n", adc_val);
                    send_audio_play_cmd(501); // Play low battery audio
                    low_bat_warned = true;
                    low_bat_last_warn_tick = current_tick;
                }
            } else if ((uint32_t)(current_tick - low_bat_last_warn_tick) >=
                       LOW_BAT_REMINDER_MS) {
                printf("Battery still low! ADC: %d. Repeating audio cmd 501...\r\n",
                       adc_val);
                send_audio_play_cmd(501);
                low_bat_last_warn_tick = current_tick;
            }
        } else if (adc_val >= BAT_ADC_WARN_RESET_THRESHOLD) {
            if (low_bat_warn_count > 0 || low_bat_shutdown_count > 0) {
                printf("[BAT DEBUG] Battery recovered (ADC: %d). Counters reset to 0!\r\n",
                       adc_val);
            }
            low_bat_warn_count = 0;
            low_bat_shutdown_count = 0;
            low_bat_warned = false;
            low_bat_last_warn_tick = 0;
        } else {
            low_bat_warn_count = 0;
            low_bat_shutdown_count = 0;
        }
    }
}
