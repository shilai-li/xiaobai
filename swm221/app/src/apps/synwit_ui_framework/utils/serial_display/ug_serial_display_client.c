#include <stdio.h>
#include "ug_serial_display_client.h"

#define CRC16_ENABLED   1

#define TAKE(a, n)  (uint8_t)(((a) >> (n)) & 0xFF)

static uint8_t* sd_frame_buf;
static uint32_t sd_frame_buf_remaining;

static uint32_t sd_frame_len;
static uint8_t sd_is_getter;

static uint16_t crc16_ccitt(const uint8_t* msg, uint32_t len)
{
    uint16_t crc = 0x0000;
    uint16_t polynomial = 0x1021;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (msg[i] << 8);
        for (unsigned char j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ polynomial;
            }
            else {
                crc <<= 1;
            }
        }
    }
    return crc & 0xFFFF;
}

static void _synwit_sdcmd_begin(uint8_t* buffer, uint32_t size, uint8_t msgid, uint8_t is_getter)
{
    if (buffer == NULL || size < SD_FRAME_INSTRU_START_POS) {
        sd_frame_buf = NULL;
        sd_frame_buf_remaining = 0;
        sd_frame_len = 0;
        return;
    }

    sd_frame_buf = buffer;
    sd_frame_buf_remaining = size;

    sd_frame_len = 0;
    sd_is_getter = is_getter;
    /* Header */
    if (is_getter) {
        sd_frame_buf[sd_frame_len++] = 0xFA;
        sd_frame_buf[sd_frame_len++] = 0x81;
    }
    else {
        sd_frame_buf[sd_frame_len++] = 0xFA;
        sd_frame_buf[sd_frame_len++] = 0x80;
    }
    
    /* Data len */
    sd_frame_buf[sd_frame_len++] = 0;
    sd_frame_buf[sd_frame_len++] = 0;

    /* Msg id */
    sd_frame_buf[sd_frame_len++] = msgid;

    /* Num of instruction(s) */
    sd_frame_buf[sd_frame_len++] = 0;

    sd_frame_buf_remaining -= sd_frame_len;
}

void synwit_ug_sdcmd_setter_begin(uint8_t* buffer, uint32_t size, uint8_t msgid)
{
    _synwit_sdcmd_begin(buffer, size, msgid, 0);
}

//void synwit_sdcmd_getter_begin(uint8_t* buffer, uint32_t size, uint8_t msgid)
//{
//    _synwit_sdcmd_begin(buffer, size, msgid, 1);
//}

static void append8(uint8_t val)
{
    sd_frame_buf[sd_frame_len++] = val;
}

static void append16(uint32_t val)
{
    sd_frame_buf[sd_frame_len++] = TAKE(val, 0);
    sd_frame_buf[sd_frame_len++] = TAKE(val, 8);
}

static void append_str(const char* str)
{
    char* dst;
    int len;

    len = 0;
    dst = (char *)&sd_frame_buf[sd_frame_len + 2];
    while (*str) {
        *dst++ = *str++;
        len++;
    }
    *dst = 0;

    sd_frame_buf[sd_frame_len++] = TAKE(len, 0);
    sd_frame_buf[sd_frame_len++] = TAKE(len, 8);
    sd_frame_len += len + 1;
}

static void increase_num_of_instructions()
{
    sd_frame_buf[SD_FRAME_CMD_NUM_POS]++;
}

void sdcmd_obj_show(uint32_t obj_id)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_obj_show;
    append16(obj_id);
    increase_num_of_instructions();
}

void sdcmd_obj_hide(uint32_t obj_id)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_obj_hide;
    append16(obj_id);
    increase_num_of_instructions();
}

void sdcmd_load_screen(uint32_t screen_id)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_load_screen;
    append16(screen_id);
    increase_num_of_instructions();
}

void sdcmd_set_pos(uint32_t obj_id, int16_t x, int16_t y)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_set_pos;
    append16(obj_id);
    append16(x);
    append16(y);
    increase_num_of_instructions();
}

void sdcmd_set_focus(uint32_t obj_id)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_set_focus;
    append16(obj_id);
    increase_num_of_instructions();
}

void sdcmd_set_focus_style(uint8_t style, 
                            uint16_t color, uint8_t opa, 
                            uint8_t line_width, 
                            uint8_t padding_top, uint8_t padding_bottom,
                            uint8_t padding_left, uint8_t padding_right)
{
    /* Sd code */
    sd_frame_buf[sd_frame_len++] = sdcode_set_focus_style;
    append8(style);
    append16(color);
    append8(opa);
    append8(line_width);
    append8(padding_top);
    append8(padding_bottom);
    append8(padding_left);
    append8(padding_right);
    increase_num_of_instructions();
}

void sdcmd_move_focus(uint8_t dir)
{
    sd_frame_buf[sd_frame_len++] = sdcode_move_focus;
    append8(dir);
    increase_num_of_instructions();
}

void sdcmd_levelimg_set_level(uint32_t obj_id, uint16_t level)
{
    sd_frame_buf[sd_frame_len++] = sdcode_levelimg_set_level;
    append16(obj_id);
    append16(level);
    increase_num_of_instructions();
}

void sdcmd_levelimg_set_recolor(uint32_t obj_id, uint16_t color)
{
    sd_frame_buf[sd_frame_len++] = sdcode_levelimg_set_recolor;
    append16(obj_id);
    append16(color);
    increase_num_of_instructions();
}

void sdcmd_levelimg_start_anim(uint32_t obj_id)
{
    sd_frame_buf[sd_frame_len++] = sdcode_levelimg_start_anim;
    append16(obj_id);
    increase_num_of_instructions();
}

void sdcmd_levelimg_stop_anim(uint32_t obj_id)
{
    sd_frame_buf[sd_frame_len++] = sdcode_levelimg_stop_anim;
    append16(obj_id);
    increase_num_of_instructions();
}

void sdcmd_label_set_text_color(uint32_t obj_id, uint16_t color)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_text_color;
    append16(obj_id);
    append16(color);
    increase_num_of_instructions();
}

void sdcmd_label_set_bg_color(uint32_t obj_id, uint16_t color)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_bg_color;
    append16(obj_id);
    append16(color);
    increase_num_of_instructions();
}

void sdcmd_label_set_bg_end_color(uint32_t obj_id, uint16_t color)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_bg_end_color;
    append16(obj_id);
    append16(color);
    increase_num_of_instructions();
}

void sdcmd_label_set_bg_grad_dir(uint32_t obj_id, uint8_t dir)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_bg_grad_dir;
    append16(obj_id);
    append8(dir);
    increase_num_of_instructions();
}

void sdcmd_label_set_bg_opa(uint32_t obj_id, uint8_t opa)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_bg_opa;
    append16(obj_id);
    append8(opa);
    increase_num_of_instructions();
}

void sdcmd_label_set_text(uint32_t obj_id, const char* str)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_text;
    append16(obj_id);
    append_str(str);
    increase_num_of_instructions();
}

void sdcmd_label_set_text_align(uint32_t obj_id, uint8_t align)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_text_align;
    append16(obj_id);
    append8(align);
    increase_num_of_instructions();
}

void sdcmd_label_set_long_mode(uint32_t obj_id, uint8_t longmode)
{
    sd_frame_buf[sd_frame_len++] = sdcode_label_set_long_mode;
    append16(obj_id);
    append8(longmode);
    increase_num_of_instructions();
}

void sdcmd_imgbtn_set_text(uint32_t obj_id, const char* str)
{
    sd_frame_buf[sd_frame_len++] = sdcode_imgbtn_set_text;
    append16(obj_id);
    append_str(str);
    increase_num_of_instructions();
}

void sdcmd_imgbtn_set_toggled(uint32_t obj_id, uint8_t toggled)
{
    sd_frame_buf[sd_frame_len++] = sdcode_imgbtn_set_toggled;
    append16(obj_id);
    append8(toggled);
    increase_num_of_instructions();
}

void sdcmd_btn_set_text(uint32_t obj_id, const char* str)
{
    sd_frame_buf[sd_frame_len++] = sdcode_btn_set_text;
    append16(obj_id);
    append_str(str);
    increase_num_of_instructions();
}

void sdcmd_btn_set_toggled(uint32_t obj_id, uint8_t toggled)
{
    sd_frame_buf[sd_frame_len++] = sdcode_btn_set_toggled;
    append16(obj_id);
    append8(toggled);
    increase_num_of_instructions();
}

void sdcmd_img_set_recolor(uint32_t obj_id, uint16_t color)
{
    sd_frame_buf[sd_frame_len++] = sdcode_img_set_recolor;
    append16(obj_id);
    append16(color);
    increase_num_of_instructions();
}

void sdcmd_bar_set_value(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_bar_set_value;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

void sdcmd_bar_set_value_max(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_bar_set_value_max;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

void sdcmd_bar_set_value_min(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_bar_set_value_min;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

void sdcmd_arc_set_value(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_arc_set_value;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

void sdcmd_arc_set_value_max(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_arc_set_value_max;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

void sdcmd_arc_set_value_min(uint32_t obj_id, int16_t value)
{
    sd_frame_buf[sd_frame_len++] = sdcode_arc_set_value_min;
    append16(obj_id);
    append16(value);
    increase_num_of_instructions();
}

uint8_t* synwit_ug_sdcmd_end(uint32_t* len)
{
    /* Update data len field */
    uint16_t data_len = (uint16_t)(sd_frame_len - 4);
    sd_frame_buf[2] = TAKE(data_len, 0);
    sd_frame_buf[3] = TAKE(data_len, 8);

    uint32_t extra_len = 0;
#if (CRC16_ENABLED == 1)
    uint16_t crc16 = crc16_ccitt(sd_frame_buf, sd_frame_len);
    sd_frame_buf[sd_frame_len] = TAKE(crc16, 0);
    sd_frame_buf[sd_frame_len + 1] = TAKE(crc16, 8);
    extra_len = 2;
#endif

    if (len) {
        *len = sd_frame_len + extra_len;
    }
    return sd_frame_buf;
}

