#ifndef SYNWIT_UGUI_SERIAL_DISPLAY_OPCODE_DECLARE_H
#define SYNWIT_UGUI_SERIAL_DISPLAY_OPCODE_DECLARE_H

/* Common operations */
#define sdcode_obj_show             0x01
#define sdcode_obj_hide             0x02
#define sdcode_load_screen          0x03
#define sdcode_set_pos              0x04
#define sdcode_set_focus            0x05
#define sdcode_set_focus_style      0x06
#define sdcode_move_focus           0x07


/* Level image operations */
#define sdcode_levelimg_set_level          0x10
#define sdcode_levelimg_set_recolor        0x11
#define sdcode_levelimg_start_anim         0x12
#define sdcode_levelimg_stop_anim          0x13


/* Label operations */
#define sdcode_label_set_text_color        0x20
#define sdcode_label_set_bg_color          0x21
#define sdcode_label_set_bg_end_color      0x22
#define sdcode_label_set_bg_grad_dir       0x23
#define sdcode_label_set_bg_opa            0x24
#define sdcode_label_set_text              0x25
#define sdcode_label_set_text_align        0x26
#define sdcode_label_set_long_mode         0x27


/* Imgbtn operations */
#define sdcode_imgbtn_set_text             0x30
#define sdcode_imgbtn_set_toggled          0x31


/* Button operations */
#define sdcode_btn_set_text                0x40
#define sdcode_btn_set_toggled             0x41


/* Image operations */
#define sdcode_img_set_recolor             0x50


/* Bar operations */
#define sdcode_bar_set_value               0x60
#define sdcode_bar_set_value_max           0x61
#define sdcode_bar_set_value_min           0x62


/* Arc operations */
#define sdcode_arc_set_value               0x70
#define sdcode_arc_set_value_max           0x71
#define sdcode_arc_set_value_min           0x72

#endif /* SYNWIT_UGUI_SERIAL_DISPLAY_OPCODE_DECLARE_H */