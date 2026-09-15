#ifndef __ANALYSIS_H__
#define __ANALYSIS_H__

#include <stdint.h>
#include <stdbool.h>
#define android_MaxLen 256

extern struct android_cbuf android_receive_buf; 
extern char android_Buf_send[64];	

extern uint8_t send_cmd;
extern volatile int finish_play_flag;//????????
extern int  error_cnt;

extern uint8_t ctrl_step;
extern uint8_t Analysis_sleep_flag;
extern bool Mod_Flag;

struct android_cbuf
{
    uint16_t android_datain;  
    uint16_t android_dataout; 
    char android_dat[android_MaxLen];
};

typedef struct {
    struct {
        int tch;    // touch
        int sen;    // sensor
				uint8_t sta_now;
    } evt;
    
    int bat;       			
		int charge_sta;
    uint8_t net_sta;        // 0,1
		uint8_t Module_sta;			//

} DeviceInfo;

extern DeviceInfo device;

typedef enum {
    ESP32_STATUS_REGISTERING = 2220,
    ESP32_STATUS_REGISTER_SUCCESS = 2221,
    ESP32_STATUS_REGISTER_FAILED = 2222,
    ESP32_STATUS_ACTIVATING = 2223,
    ESP32_STATUS_ACTIVATE_SUCCESS = 2224,
    ESP32_STATUS_ACTIVATE_FAILED = 2225,
    ESP32_STATUS_CONNECTING = 2226,
    ESP32_STATUS_CONNECT_SUCCESS = 2227,
    ESP32_STATUS_CONNECT_FAILED = 2228,
} Esp32Status;

extern volatile uint16_t esp32_status;

typedef struct {
    uint8_t id;
    const char* string;
} Expression;

typedef struct __attribute__((packed))
{
    uint16_t header;     
    uint16_t len;         
    uint8_t command;      
    uint8_t liu_shui_hao; 
} header;           

typedef struct __attribute__((packed))
{
    uint16_t header;         //    AA 55
    uint16_t len;            //
    uint8_t command;         //   0x80
    uint8_t liu_shui_hao;    //   0x00
    uint8_t net_state;    		//   0x0A||0x0B
    uint8_t dev_state;    		//  	00 01 02 03
		uint8_t crc;
    uint8_t tail;             //   88
} state_message;      //

typedef struct __attribute__((packed))
{
    uint16_t header;         //    AA 55
    uint16_t len;            //
    uint8_t command;         //   0x80
    uint8_t liu_shui_hao;    //   0x00
    uint8_t cmd;    	
		uint8_t crc;
    uint8_t tail;             //   88
} reply_message;      //

typedef struct __attribute__((packed))
{
    uint16_t header;         //    AA 55
    uint16_t len;            //
    uint8_t command;         //   0x80
    uint8_t liu_shui_hao;    //   0x00
    uint8_t AI_state;    		//   0x0A||0x0B
		uint8_t crc;
    uint8_t tail;             //   88
} AI_wakeup_message;      //

typedef struct __attribute__((packed))
{
    uint16_t header;         //    AA 55
    uint16_t len;            //
    uint8_t command;         //   0x80
    uint8_t liu_shui_hao;    //   0x00
    uint8_t face_num;    		//   0x0A||0x0B
		uint8_t crc;
    uint8_t tail;             //   88
} face_message;    

char android_buf_in(struct android_cbuf *android_bp, uint8_t *android_dp);
void send_sleep_msg(void);
void SG_agreement_parsing(void);
void android_chu_li(void);
void hibernation_notify_ci1303_state(uint16_t state);
void hibernation_set_timeout_seconds(uint16_t seconds);
void hibernation_set_wakeup_window_seconds(uint16_t seconds);
void send_msg(void);
// uint8_t calculate_crc8(const uint8_t *data, uint8_t len);
uint16_t parse_protocol_data(uint8_t *protocol_data);
uint8_t generate_protocol_data(const uint8_t *original_data, uint8_t data_len, uint8_t *output_data);
uint8_t crc8_maxim(const uint8_t *data, uint8_t len);
#endif
