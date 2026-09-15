#include "Analysis.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "../CSL/CMSIS/DeviceSupport/SWM221.h"
#include "synwit_ui_framework/synwit_ui.h"
#include "dev_uart.h"
#include "board/dev_servo.h"
#include "ui_src/appkit/screen_id.h"

int buf_long;
#define TEMP_BUFFER_SIZE 32 

#define CRC8_POLY 0x31 
#define CRC8_INIT 0x00
#define CRC8_XOROUT 0x00 
#define REFLECT_IN 1  
#define REFLECT_OUT 1  

#define CI1303_STATE_ONLINE    2211U
#define CI1303_STATE_LISTENING 2212U
#define CI1303_STATE_IDLE      2213U
#define CI1303_STATE_SPEAKING  2214U
#define CI1303_STATE_USER_SPEAKING 2215U
#define CI1303_STATE_TOPIC_SPEAKING 2216U
#define CONNECTION_SUCCESS_DISPLAY_MS 2000

char *UUID = NULL;
uint16_t UUID_length = 0;
uint8_t AI_WakeUp_Sta = 0;
uint8_t rec_info_flag = 0;
volatile char check_UUID_success = 0; 
static bool Start_Sys = false;
static uint8_t Get_Cnt;
static int32_t face_timer = 0; // Timer in ms
static uint16_t face_timeout_target = 100;


DeviceInfo device = {
    .evt = { .tch = 0, .sen = 0 },
    .bat = 0, .charge_sta = 0 , .net_sta = 0, .Module_sta = 0
};
volatile uint16_t esp32_status = 0;

uint8_t ctrl_step = 0;
uint8_t Analysis_sleep_flag = false;

char android_buf_in(struct android_cbuf *android_bp, uint8_t *android_dp)
{
    if ((android_bp->android_datain + 1) % android_MaxLen == android_bp->android_dataout) {
        return 1;
    } else {
        android_bp->android_dat[android_bp->android_datain] = *android_dp;
        android_bp->android_datain = ((android_bp->android_datain) + 1) % android_MaxLen; 
        return 0;
    }
}

char android_buf_Out(struct android_cbuf *android_bp, char *android_dp)
{
    if (android_bp->android_dataout == android_bp->android_datain) {
        return 1;
    } else {
        *android_dp = android_bp->android_dat[android_bp->android_dataout];
        android_bp->android_dataout = ((android_bp->android_dataout) + 1) % android_MaxLen; 
        return 0;
    }
}

const uint8_t Start_Speaking_Protocol[4] = {0x01,0x03,0xF1,0x88};
const uint8_t End_Speaking_Protocol[4] = {0x01,0x03,0xF2,0x88};

void Get_Net_Dev_State(void)
{
    state_message *agreement_data_received_P = (state_message *)android_Buf_send;
    static uint8_t last_Module_sta = 0;
    device.Module_sta = agreement_data_received_P->dev_state;
    device.net_sta = agreement_data_received_P->net_state;
    if(device.Module_sta==0x06) error_cnt = 0;
    else if(device.Module_sta==0x04 && last_Module_sta==0x06 && error_cnt<4) device.Module_sta=0x06;
    last_Module_sta = device.Module_sta;
}

void Get_AI_WakeUp_State(void)
{
    AI_wakeup_message *agreement_data_received_P = (AI_wakeup_message *)android_Buf_send;
    if(agreement_data_received_P->AI_state==0x03) AI_WakeUp_Sta = 1;
}

void Get_Expression(uint16_t len)
{
    face_message *agreement_data_received_P = (face_message *)android_Buf_send;
    if(agreement_data_received_P->face_num >= 0x01 && agreement_data_received_P->face_num <= 0x03) finish_play_flag = 1;
}

void Get_UUID(uint16_t len)
{
    uint16_t temp_i = 0;
    uint8_t temp_buffer[TEMP_BUFFER_SIZE];
    uint16_t length = ((len & 0xFF) << 8) | ((len >> 8) & 0xFF);
    for (uint16_t i = 6; i < length+2; i++) temp_buffer[temp_i++] = android_Buf_send[i];
    if (UUID != NULL) { free(UUID); UUID = NULL; }
    UUID_length = temp_i;
    UUID = (char *)malloc(UUID_length * sizeof(uint8_t));
    if (UUID != NULL) memcpy(UUID, temp_buffer, UUID_length);
}

static uint8_t reflect_byte(uint8_t byte) {
    byte = ((byte & 0xF0) >> 4) | ((byte & 0x0F) << 4);
    byte = ((byte & 0xCC) >> 2) | ((byte & 0x33) << 2);
    byte = ((byte & 0xAA) >> 1) | ((byte & 0x55) << 1);
    return byte;
}

uint8_t crc8_maxim(const uint8_t *data, uint8_t len) {
    uint8_t crc = CRC8_INIT;
    for (uint8_t i = 6; i < len; i++) {
        uint8_t byte = data[i];
        if (REFLECT_IN) byte = reflect_byte(byte);
        crc ^= byte;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ CRC8_POLY;
            else crc <<= 1;
        }
    }
    if (REFLECT_OUT) crc = reflect_byte(crc);
    return crc ^ CRC8_XOROUT;
}

uint8_t crc8_maxim_send(const uint8_t *data, uint8_t len) {
    uint8_t crc = CRC8_INIT;
    for (uint8_t i = 6; i < len+6; i++) {
        uint8_t byte = data[i];
        if (REFLECT_IN) byte = reflect_byte(byte);
        crc ^= byte;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ CRC8_POLY;
            else crc <<= 1;
        }
    }
    if (REFLECT_OUT) crc = reflect_byte(crc);
    return crc ^ CRC8_XOROUT;
}

uint16_t parse_protocol_data(uint8_t *protocol_data) {
    if (!protocol_data || protocol_data[0] != 0xAA || protocol_data[1] != 0x55) return 0;
    uint16_t frame_length_field = (protocol_data[2] << 8) | protocol_data[3];
    uint16_t protocol_len = frame_length_field+4;
    if (protocol_data[protocol_len - 2] != crc8_maxim(protocol_data, protocol_len - 2)) return 0;
    return protocol_len;
}

uint8_t send_buffer[256];

int pack_protocol(uint8_t cmd) {
    uint8_t actual_size = 0;
    static char music_change = 0;
    if(cmd==0x78 || cmd==0x80 || cmd==0x82 || cmd==0x87) {
        uint8_t orig_cmd = cmd; cmd = 0x90; send_buffer[6] = orig_cmd; actual_size = 1;
    } else if(cmd==0x77 || cmd==0x89) {
        send_buffer[6] = 0x01; actual_size = 1;
    } else if(cmd==0x79 || cmd==0x85) {
        send_buffer[6] = device.evt.sta_now; actual_size = 1;
    } else if(cmd==0x81) {
        music_change = (music_change % 3) + 1; send_buffer[6] = music_change; actual_size = 1;
    } else if(cmd==0x86) {
        actual_size = snprintf((char*)&send_buffer[6], 250, "{\"uid\":\"%s\",\"evt\":{\"tch\":%d,\"sen\":%d},\"bat\":%d,\"sta\":%d}", UUID, device.evt.tch, device.evt.sen, device.bat, device.charge_sta);
    }
    int index = 0;
    send_buffer[index++] = 0xAA; send_buffer[index++] = 0x55;
    send_buffer[index++] = ((actual_size+4) >> 8) & 0xFF; send_buffer[index++] = (actual_size+4) & 0xFF;
    send_buffer[index++] = cmd; send_buffer[index++] = 0x00;
    index += actual_size;
    send_buffer[index++] = crc8_maxim_send(send_buffer, actual_size);
    send_buffer[index++] = 0x88;
    return index;
}

int pack_protocol_reply(uint8_t cmd, uint8_t *buffer, int buffer_size) { return 0; }

int send_UUID_msg(void) {
    static uint16_t check_UUID_time = 0;
    static char check_times = 0;
    if(check_UUID_success==0 && check_UUID_time++==0 && check_times<4) {
        int data_len = pack_protocol(0x77);
        for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
    } else if(check_UUID_success==1) return 1;
    else if(check_UUID_time>300 && check_UUID_success==0 && check_times<3) { check_UUID_time=0; check_times++; }
    else { ctrl_step=2; }
    return 0;
}

void send_music_msg(void) {
    int data_len = pack_protocol(0x81);
    for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
}

void send_servive_msg(void) {
    int data_len = pack_protocol(0x86);
    for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
    device.evt.sen = 0; device.evt.tch = 0;
}

void send_sleep_msg(void) {
    int data_len = pack_protocol(0x89);
    for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
    device.evt.sen = 0; device.evt.tch = 0;
}

volatile char mpu_play_flag;

void send_touch_msg(void) {
    static int cnt_touch_time = 0;
    if(device.Module_sta==0x06 || device.Module_sta==0x04) cnt_touch_time = 0;
    else if(ctrl_step==2 && cnt_touch_time++>6000) {
        cnt_touch_time = 0; finish_play_flag = 1; send_sleep_msg(); ctrl_step = 3;
    }
}

void send_wakeup_way_msg(void) {
    int data_len = pack_protocol(0x79);
    for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
    device.evt.sta_now = 0;
}

void SG_agreement_parsing(void) {
    if(parse_protocol_data((uint8_t *)android_Buf_send) == 0) return;
    header *agreement_data_received_P = (header *)android_Buf_send;
    uint8_t cmd = agreement_data_received_P->command;
    bool reply = false;
    if(cmd == 0x78) { Get_UUID(agreement_data_received_P->len); check_UUID_success = 1; reply = true; }
    else if(cmd == 0x80) { Get_Net_Dev_State(); reply = true; }
    else if(cmd == 0x82 || cmd == 0x87) reply = true;
    else if(cmd == 0x90) { /* handle reply */ }
    
    if(reply) {
        int data_len = pack_protocol(cmd);
        for(int i = 0; i < data_len; i++) { USART_Write(USART0, send_buffer[i]); while(!USART_INTStat(USART0, USART_IT_TX_EMPTY)); }
    }
}

bool Mod_Flag = false;
void send_msg(void) {
    static int wakeup_time = 0;
    static char wakeup_flag = 1;
    if(wakeup_flag==1) {
        wakeup_time++;
        if(wakeup_time >= 25) { wakeup_time = 0; wakeup_flag = 0; }
    }
    if(ctrl_step==0) {
        if(send_UUID_msg()==1) { ctrl_step=2; Mod_Flag = true; }
    } else if(ctrl_step==2 || ctrl_step==3) {
        send_touch_msg();
        if(ctrl_step==0) wakeup_flag = 1;
    }
}

static uint32_t last_face_systick = 0;

extern volatile uint8_t test_driver_change;
extern int synwit_ug_load_screen(uint32_t screen_index);
void trigger_face_by_id(uint16_t face_id);

static void load_screen_with_direction(uint32_t screen_id)
{
    /* Never blank the backlight for a screen change: the panel redraws line by
     * line from QSPI flash, so hiding it costs a full dark frame plus the
     * settling delay on every 100 <-> 301 transition. Only power-on hides. */
    // 100/301 retain the original mirrored setup; status animations use the same direction.
    test_driver_change = (screen_id == SCREEN100 || screen_id == SCREEN301) ? 0 : 1;
    synwit_ug_load_screen(screen_id);
}

void face_timeout_task(void) {
    extern int synwit_ug_load_screen(uint32_t screen_index);
    extern uint32_t systick_get_tick(void);
    
    uint32_t current_systick = systick_get_tick();
    
    if (face_timer > 0) {
        uint32_t delta = 0;
        if (last_face_systick != 0) {
            delta = current_systick - last_face_systick;
            if (delta > 1000) delta = 10; // Cap to prevent huge jumps if interrupted
        } else {
            delta = 10;
        }
        
        face_timer -= delta;
        if (face_timer <= 0) {
            uint16_t target_face = face_timeout_target;
            face_timer = 0;
            trigger_face_by_id(target_face);
            printf("UI: Auto timeout -> Face %u\r\n", target_face);
        }
    }
    last_face_systick = current_systick;
}


void send_audio_play_cmd(uint16_t voice_id) {
    printf("SWM221 sending audio cmd 0x03 to CI1303, ID: %d\r\n", voice_id);
    USART_Write(USART0, 0x03);
    while(!USART_INTStat(USART0, USART_IT_TX_EMPTY));
    USART_Write(USART0, (uint8_t)(voice_id >> 8));
    while(!USART_INTStat(USART0, USART_IT_TX_EMPTY));
    USART_Write(USART0, (uint8_t)(voice_id & 0xFF));
    while(!USART_INTStat(USART0, USART_IT_TX_EMPTY));
    USART_Write(USART0, 0xFF);
    while(!USART_INTStat(USART0, USART_IT_TX_EMPTY));
}

void trigger_face_by_id(uint16_t face_id) {
    extern int synwit_ug_load_screen(uint32_t screen_index);

    /* Touch actions keep their CI1303 prompt IDs even when the matching
     * display animations are not included in the reduced UI build. */
    if (face_id >= 401 && face_id <= 405) {
        send_audio_play_cmd(face_id);
    }

    switch (face_id) {
        case 100:
            device.Module_sta = 0x00;
            face_timer = 0;
            face_timeout_target = 100;
            load_screen_with_direction(SCREEN100);
            break;
        case 301:
            device.Module_sta = 0x06;
            face_timer = 5000;
            face_timeout_target = 100;
            load_screen_with_direction(SCREEN301);
            break;
        default:
            return;
    }

    printf("UI: Cmd %d (Sta: 0x%02X)\r\n", face_id, device.Module_sta);
}

static bool handle_esp32_status(uint16_t status_id) {
    uint32_t screen_id;

    switch (status_id) {
        case ESP32_STATUS_REGISTERING:
            screen_id = SCREEN_REGISTERING;
            break;
        case ESP32_STATUS_REGISTER_SUCCESS:
            screen_id = SCREEN_REGISTRATION_SUCCESS;
            break;
        case ESP32_STATUS_REGISTER_FAILED:
            screen_id = SCREEN_REGISTRATION_FAILED;
            break;
        case ESP32_STATUS_ACTIVATING:
            screen_id = SCREEN_ACTIVATION_TITLE;
            break;
        case ESP32_STATUS_ACTIVATE_SUCCESS:
            screen_id = SCREEN_ACTIVATION_SUCCESS;
            break;
        case ESP32_STATUS_ACTIVATE_FAILED:
            screen_id = SCREEN_ACTIVATION_FAILED;
            break;
        case ESP32_STATUS_CONNECTING:
            screen_id = SCREEN_CONNECTING;
            break;
        case ESP32_STATUS_CONNECT_SUCCESS:
            screen_id = SCREEN_CONNECTION_SUCCESS;
            break;
        case ESP32_STATUS_CONNECT_FAILED:
            screen_id = SCREEN_CONNECTION_FAILED;
            break;
        default:
            return false;
    }

    esp32_status = status_id;
    face_timer = 0;
    if (status_id == ESP32_STATUS_CONNECT_SUCCESS) {
        face_timer = CONNECTION_SUCCESS_DISPLAY_MS;
        face_timeout_target = 301;
    }
    load_screen_with_direction(screen_id);
    printf("ESP32 status updated: %u -> screen %lu\r\n",
           status_id, (unsigned long)screen_id);
    return true;
}

static bool handle_ci1303_state(uint16_t state_id) {
    if (state_id < CI1303_STATE_ONLINE || state_id > CI1303_STATE_TOPIC_SPEAKING) {
        return false;
    }

    hibernation_notify_ci1303_state(state_id);
    if (state_id == CI1303_STATE_LISTENING) {
        // A wake-up enters listening mode; replace any setup/status screen with face 301.
        trigger_face_by_id(301);
    }
    printf("CI1303 state updated: %u\r\n", state_id);
    return true;
}

void android_chu_li(void) {
    uint16_t i = 0; char ch = 0;
    static uint8_t startBit = 0, num = 0;
    for (i = 0; i < 128; i++) {
        if (android_buf_Out(&android_receive_buf, &ch) != 0) break;
        if (startBit == 0) {
            if ((uint8_t)ch == 0xAA) { startBit = 1; num = 0; android_Buf_send[num++] = (uint8_t)ch; }
            else if ((uint8_t)ch == 0x01) { startBit = 2; num = 0; android_Buf_send[num++] = (uint8_t)ch; }
            else if ((uint8_t)ch == 0x00) { startBit = 3; num = 0; android_Buf_send[num++] = (uint8_t)ch; }
            else if ((uint8_t)ch == 0x02) { startBit = 4; num = 0; android_Buf_send[num++] = (uint8_t)ch; }
            else if ((uint8_t)ch == 0x04) { startBit = 5; num = 0; android_Buf_send[num++] = (uint8_t)ch; }
        } else if (startBit == 1) {
            android_Buf_send[num++] = (uint8_t)ch;
            if (num == 2 && (uint8_t)android_Buf_send[1] != 0x55) { if ((uint8_t)android_Buf_send[1] == 0xAA) num = 1; else startBit = 0; }
            else if (num == 4) buf_long = (android_Buf_send[2] << 8) | android_Buf_send[3];
            else if (num >= 6 && num == (buf_long + 4)) { SG_agreement_parsing(); startBit = 0; }
            if (num >= 64) startBit = 0;
        } else if (startBit == 2) {
            android_Buf_send[num++] = (uint8_t)ch;
            if (num == 4) {
                if ((uint8_t)android_Buf_send[3] == 0xFF) {
                    uint16_t angle = (android_Buf_send[1] << 8) | android_Buf_send[2];
                    // servo_set_angle(0, (uint8_t)angle);
                    servo_set_target(0, (uint8_t)angle);
                }
                startBit = 0;
            }
        } else if (startBit == 3) {
            android_Buf_send[num++] = (uint8_t)ch;
            if (num == 4) {
                if ((uint8_t)android_Buf_send[3] == 0xFF) {
                    uint16_t face_id = (android_Buf_send[1] << 8) | android_Buf_send[2];
                    if (handle_esp32_status(face_id)) {
                        /* Status is available through esp32_status for UI and motion logic. */
                    } else if (handle_ci1303_state(face_id)) {
                        /* Voice activity state also drives the hibernation guard. */
                    } else {
                        trigger_face_by_id(face_id);
                    }
                }
                startBit = 0;
            }
        } else if (startBit == 4) {
            android_Buf_send[num++] = (uint8_t)ch;
            if (num == 4) {
                if ((uint8_t)android_Buf_send[3] == 0xFF) {
                    uint16_t seconds = (android_Buf_send[1] << 8) |
                                       android_Buf_send[2];
                    hibernation_set_timeout_seconds(seconds);
                }
                startBit = 0;
            }
        } else if (startBit == 5) {
            android_Buf_send[num++] = (uint8_t)ch;
            if (num == 4) {
                if ((uint8_t)android_Buf_send[3] == 0xFF) {
                    uint16_t seconds = (android_Buf_send[1] << 8) |
                                       android_Buf_send[2];
                    hibernation_set_wakeup_window_seconds(seconds);
                }
                startBit = 0;
            }
        }
    }
}
