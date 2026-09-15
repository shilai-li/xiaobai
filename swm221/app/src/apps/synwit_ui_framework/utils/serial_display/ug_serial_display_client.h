#ifndef SYNWIT_UGUI_SERIAL_DISPLAY_CLIENT_DECLARE_H
#define SYNWIT_UGUI_SERIAL_DISPLAY_CLIENT_DECLARE_H

#include "ug_serial_display_opcodes.h"
#include <stdint.h>

#ifdef WIN32
    #if (defined FRAMEWORK_BUILD)
        #define SD_EXPORT_API __declspec(dllexport)
        #define SD_EXPORT_VAR __declspec(dllexport)
    #else
        #define SD_EXPORT_API __declspec(dllimport)
        #define SD_EXPORT_VAR __declspec(dllimport)
    #endif
#else
    #define SD_EXPORT_API
    #define SD_EXPORT_VAR
#endif

typedef uint16_t    ug_sdcode_t;

/* 协议帧字段位置定义 */
#define SD_FRAME_HEADER_POS                 0   /* 2 bytes */
#define SD_FRAME_LEN_POS                    2   /* 2 bytes */
#define SD_FRAME_MSG_ID_POS                 4   /* 1 byte */
#define SD_FRAME_CMD_NUM_POS                5   /* 1 byte */
#define SD_FRAME_INSTRU_START_POS           6

#ifdef __cplusplus
extern "C" {
#endif


/**@brief 开始构建串口屏Setter操作数据帧
* @param[in]  buffer             数据帧缓存区
* @param[in]  size               数据帧缓存区大小
* @param[in]  msgid              用户自定义msg id，由应答数据携带返回
*/
SD_EXPORT_API void synwit_ug_sdcmd_setter_begin(uint8_t* buffer, uint32_t size, uint8_t msgid);



/**@brief 数据帧构建结束
* @param[out] len                数据帧长度
* @return  调用synwit_sdcmd_begin()时传入的帧地址
*/
SD_EXPORT_API uint8_t* synwit_ug_sdcmd_end(uint32_t *len);


/* Common operations */
SD_EXPORT_API void sdcmd_obj_show(uint32_t obj_id);
SD_EXPORT_API void sdcmd_obj_hide(uint32_t obj_id);
SD_EXPORT_API void sdcmd_load_screen(uint32_t scr_id);
SD_EXPORT_API void sdcmd_set_pos(uint32_t obj_id, int16_t x, int16_t y);
SD_EXPORT_API void sdcmd_set_focus(uint32_t obj_id);
SD_EXPORT_API void sdcmd_set_focus_style(uint8_t style,
                                                uint16_t color, uint8_t opa,
                                                uint8_t line_width,
                                                uint8_t padding_top, uint8_t padding_bottom,
                                                uint8_t padding_left, uint8_t padding_right);
SD_EXPORT_API void sdcmd_move_focus(uint8_t dir);


/* Level image operations */
SD_EXPORT_API void sdcmd_levelimg_set_level(uint32_t obj_id, uint16_t level);
SD_EXPORT_API void sdcmd_levelimg_set_recolor(uint32_t obj_id, uint16_t color);
SD_EXPORT_API void sdcmd_levelimg_start_anim(uint32_t obj_id);
SD_EXPORT_API void sdcmd_levelimg_stop_anim(uint32_t obj_id);


/* Label operations */
SD_EXPORT_API void sdcmd_label_set_text_color(uint32_t obj_id, uint16_t color);
SD_EXPORT_API void sdcmd_label_set_bg_color(uint32_t obj_id, uint16_t color);
SD_EXPORT_API void sdcmd_label_set_bg_end_color(uint32_t obj_id, uint16_t color);
SD_EXPORT_API void sdcmd_label_set_bg_grad_dir(uint32_t obj_id, uint8_t dir);
SD_EXPORT_API void sdcmd_label_set_bg_opa(uint32_t obj_id, uint8_t opa);
SD_EXPORT_API void sdcmd_label_set_text(uint32_t obj_id, const char *str);
SD_EXPORT_API void sdcmd_label_set_text_align(uint32_t obj_id, uint8_t align);
SD_EXPORT_API void sdcmd_label_set_long_mode(uint32_t obj_id, uint8_t longmode);


/* Imgbtn operations */
SD_EXPORT_API void sdcmd_imgbtn_set_text(uint32_t obj_id, const char* str);
SD_EXPORT_API void sdcmd_imgbtn_set_toggled(uint32_t obj_id, uint8_t toggled);


/* Button operations */
SD_EXPORT_API void sdcmd_btn_set_text(uint32_t obj_id, const char* str);
SD_EXPORT_API void sdcmd_btn_set_toggled(uint32_t obj_id, uint8_t toggled);


/* Image operations */
SD_EXPORT_API void sdcmd_img_set_recolor(uint32_t obj_id, uint16_t color);


/* Bar operations */
SD_EXPORT_API void sdcmd_bar_set_value(uint32_t obj_id, int16_t value);
SD_EXPORT_API void sdcmd_bar_set_value_max(uint32_t obj_id, int16_t value);
SD_EXPORT_API void sdcmd_bar_set_value_min(uint32_t obj_id, int16_t value);

/* Bar operations */
SD_EXPORT_API void sdcmd_arc_set_value(uint32_t obj_id, int16_t value);
SD_EXPORT_API void sdcmd_arc_set_value_max(uint32_t obj_id, int16_t value);
SD_EXPORT_API void sdcmd_arc_set_value_min(uint32_t obj_id, int16_t value);


#ifdef __cplusplus
} /*extern "C"*/
#endif


#endif /* SYNWIT_UGUI_SERIAL_DISPLAY_CLIENT_DECLARE_H */