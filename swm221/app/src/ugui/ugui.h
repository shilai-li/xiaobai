/* -------------------------------------------------------------------------------- */
/* -- µGUI - Generic GUI module (C)Achim Döbler, 2015                            -- */
/* -------------------------------------------------------------------------------- */
// µGUI is a generic GUI module for embedded systems.
// This is a free software that is open for education, research and commercial
// developments under license policy of following terms.
//
//  Copyright (C) 2015, Achim Döbler, all rights reserved.
//  URL: http://www.embeddedlightning.com/
//
// * The µGUI module is a free software and there is NO WARRANTY.
// * No restriction on use. You can use, modify and redistribute it for
//   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
// * Redistributions of source code must retain the above copyright notice.
//
/* -------------------------------------------------------------------------------- */
#ifndef __UGUI_H
#define __UGUI_H

#include "ugui_config.h"
#include "ug_mem.h"

#include "ugui_modules/ugui_types.h"
#include "ugui_modules/ugui_geometry.h"
#include "ugui_modules/ugui_colors.h"
#include "ugui_modules/ugui_font.h"
#include "ugui_modules/ugui_bmp.h"
#include "ugui_modules/ugui_message.h"
#include "ugui_modules/ugui_draw.h"

#include "ugui_modules/ugui_object.h"
#include "ugui_modules/ugui_window.h"
#include "ugui_modules/ugui_button.h"
#include "ugui_modules/ugui_timer.h"
#include "ugui_modules/ugui_image.h"
#include "ugui_modules/ugui_levelimg.h"
#include "ugui_modules/ugui_bar.h"
#include "ugui_modules/ugui_arc.h"
#include "ugui_modules/ugui_label.h"
#include "ugui_modules/ugui_imgbtn.h"
#include "ugui_modules/ugui_page.h"
#include "ugui_modules/ugui_qrcode.h"

#ifdef __cplusplus
extern "C" {
#endif
/* -------------------------------------------------------------------------------- */
/* -- µGUI FONTS                                                                 -- */
/* -------------------------------------------------------------------------------- */
EXPORT_VAR extern __UG_FONT_DATA UG_FONT UG_FONT_MONTSERRAT_10;


/* -------------------------------------------------------------------------------- */
/* -- TYPEDEFS                                                                   -- */
/* -------------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------------- */
/* -- DEFINES                                                                    -- */
/* -------------------------------------------------------------------------------- */
#ifndef NULL
   #define NULL ((void*) 0)
#endif

#define UG_TRUE     1
#define UG_FALSE    0

#define UG_MIN(a, b)    ((a) < (b) ? (a) : (b))
#define UG_MAX(a, b)    ((a) < (b) ? (b) : (a))
#define UG_DIM(x)   (sizeof(x) / sizeof((x)[0]))

/* Alignments */
#define ALIGN_H_LEFT                                  (1<<0)
#define ALIGN_H_CENTER                                (1<<1)
#define ALIGN_H_RIGHT                                 (1<<2)
#define ALIGN_V_TOP                                   (1<<3)
#define ALIGN_V_CENTER                                (1<<4)
#define ALIGN_V_BOTTOM                                (1<<5)
#define ALIGN_BOTTOM_RIGHT                            (ALIGN_V_BOTTOM|ALIGN_H_RIGHT)
#define ALIGN_BOTTOM_CENTER                           (ALIGN_V_BOTTOM|ALIGN_H_CENTER)
#define ALIGN_BOTTOM_LEFT                             (ALIGN_V_BOTTOM|ALIGN_H_LEFT)
#define ALIGN_CENTER_RIGHT                            (ALIGN_V_CENTER|ALIGN_H_RIGHT)
#define ALIGN_CENTER                                  (ALIGN_V_CENTER|ALIGN_H_CENTER)
#define ALIGN_CENTER_LEFT                             (ALIGN_V_CENTER|ALIGN_H_LEFT)
#define ALIGN_TOP_RIGHT                               (ALIGN_V_TOP|ALIGN_H_RIGHT)
#define ALIGN_TOP_CENTER                              (ALIGN_V_TOP|ALIGN_H_CENTER)
#define ALIGN_TOP_LEFT                                (ALIGN_V_TOP|ALIGN_H_LEFT)

/* Text long modes */
#define TEXT_LONG_MODE_NONE                                (0)
#define TEXT_LONG_MODE_SCROLL_CIRCLE                       (1) /* Roll the text circularly */
#define TEXT_LONG_MODE_SCROLL                              (2) /* Roll the text back and forth */

/* Gradient directions */
#define GRAD_DIR_NONE   0
#define GRAD_DIR_VER    1
#define GRAD_DIR_HOR    2

/* -------------------------------------------------------------------------------- */
/* -- FUNCTION RESULTS                                                           -- */
/* -------------------------------------------------------------------------------- */
#define UG_RESULT_FAIL                               -1
#define UG_RESULT_OK                                  0

/* -------------------------------------------------------------------------------- */
/* -- TOUCH                                                                      -- */
/* -------------------------------------------------------------------------------- */
/* Touch structure */
typedef struct
{
   UG_U32 timestamp;
   UG_S16 xp;
   UG_S16 yp;
   UG_U8 state;
} UG_TOUCH;

/* Gesture structure */
typedef struct {
    UG_U32 timestamp;

    UG_S16 delta_x;
    UG_S16 delta_y;
    UG_S16 amount_x;
    UG_S16 amount_y;

    UG_BOOL fling;
} UG_GESTURE;


#define TOUCH_STATE_PRESSED                           1
#define TOUCH_STATE_RELEASED                          0

/* Focus pattern */
enum {
    UG_FOCUS_EFFECT_FRAME = 0,
    UG_FOCUS_EFFECT_SOLID,
};
typedef struct {
    // 焦点控件的标记样式:
    //      UG_FOCUS_EFFECT_FRAME   标记样式为边框
    //      UG_FOCUS_EFFECT_SOLID   标记样式为填充色
    UG_U8 effect;

    /* 颜色 */
    UG_COLOR color;

    /* 透明度， 0~255*/
    UG_U8 opa;

    /* 采用边框样式时的线条宽度 */
    UG_S16 line_width;

    /* 标记效果的上/下/左/右缩进像素数 */
    UG_U8 padding_top;
    UG_U8 padding_bottom;
    UG_U8 padding_left;
    UG_U8 padding_right;
}UG_FOCUS_EFFECT;

/* -------------------------------------------------------------------------------- */
/* -- OBJECTS                                                                    -- */
/* -------------------------------------------------------------------------------- */

/* Currently supported objects */
enum {
    OBJ_TYPE_OBJECT = 0,
    OBJ_TYPE_WINDOW,
    OBJ_TYPE_BUTTON,
    OBJ_TYPE_LABEL,
    OBJ_TYPE_IMAGE,
    OBJ_TYPE_BAR,
    OBJ_TYPE_LEVELIMG,
    OBJ_TYPE_ARC,
    OBJ_TYPE_IMGBTN,
    OBJ_TYPE_PAGE,
    OBJ_TYPE_QRCODE,

    // The last one
    OBJ_TYPE_NUM
};

/* Standard object events */
#define UG_EVENT_NONE                                0
#ifdef USE_PRERENDER_EVENT
#define UG_EVENT_PRERENDER                           1
#endif
#ifdef USE_POSTRENDER_EVENT
#define UG_EVENT_POSTRENDER                          2
#endif

#define UG_EVENT_PRESSED                             3
#define UG_EVENT_RELEASED                            4
#define UG_EVENT_CLICKED                             5
#define UG_EVENT_LONG_PRESSING                       6
#define UG_EVENT_TOUCH_LEAVE                         7
#define UG_EVENT_VALUE_CHANGED                       8
#define UG_EVENT_GESTURE_BEGIN                       9
#define UG_EVENT_GESTURE_END                         10
#define UG_EVENT_GESTURE                             11

/* -------------------------------------------------------------------------------- */
/* -- µGUI DRIVER                                                                -- */
/* -------------------------------------------------------------------------------- */
typedef struct
{
  void* driver;
  UG_U8 state;
} UG_DRIVER;

#define DRIVER_REGISTERED                             (1<<0)
#define DRIVER_ENABLED                                (1<<1)

/* Supported drivers */
#define NUMBER_OF_DRIVERS                             3
#define DRIVER_DRAW_LINE                              0
#define DRIVER_FILL_FRAME                             1
#define DRIVER_FILL_AREA                              2

/* -------------------------------------------------------------------------------- */
/* -- µGUI CORE STRUCTURE                                                        -- */
/* -------------------------------------------------------------------------------- */

typedef struct {
    /* 页面某个区域开始刷新前会触发这个回调，防撕裂(TE)信号的同步处理可以加在这个回调内 */
    void (*flush_start)(UG_S16 x, UG_S16 y, UG_S16 w, UG_S16 h);
    /* 显示屏更新区域设置，根据x,y,w,h参数设置显示屏即将要接收数据的对应区域 */
    void (*area_set)(UG_S16 x, UG_S16 y, UG_S16 w, UG_S16 h);
    /* 开始传输显示数据，将colors的num个数据写入显示屏 */
    void (*flush_pixels)(UG_COLOR* colors, UG_U16 num, UG_U8 asyncable);
    /* 页面某个区域刷新结束会触发这个回调。仅作保留用途，一般用不到 */
    void (*flush_ready)(UG_S16 x, UG_S16 y, UG_S16 w, UG_S16 h);
}DISPLAY_OPS;

typedef struct {
    int (*ui_data_read)(UG_U32 offset, UG_U32 size, void* buf);
}DATA_OPS;

typedef struct {
    void (*framework_ready)(void);
    void (*app_ready)(void);

    void (*main_tick)(void);
    /* 指定main_tick回调函数的间隔，毫秒为单位，默认为50ms */
    UG_U32 main_tick_period;

    /* 钩子函数，进入蓝屏界面前会被调用 */
    void (*pre_blue)(const char* msg);

    /* 全局钩子函数，进入新界面后会被调用 */
    void (*on_screen_start)(UG_ID screen_id, UG_WINDOW* wnd);
    /* 全局钩子函数，离开当前界面前会被调用 */
    void (*on_screen_stop)(UG_ID screen_id, UG_WINDOW* wnd);
}APP_CALLBACK;

typedef struct {
    void (*rx_handler)();
    void (*notify)(const uint8_t* data, uint32_t len);
}SERIAL_DISP_OPS;

typedef struct {
    DISPLAY_OPS disp;
    DATA_OPS data;
    APP_CALLBACK app;

    SERIAL_DISP_OPS sdisp;
}SYS_OPS;

typedef struct {
    UG_S16 display_width;
    UG_S16 display_height;

    MEM_UNIT* heap;
    UG_U32 heap_len;

    UG_U16 pixels_per_pfb;
    UG_U8 num_of_pfb;

    UG_U8 flush_area_alignment;
}SYS_CONFIG;
/* Flush area align */
#define FAA_X2              0x01
#define FAA_Y2              0x02
#define FAA_W2              0x04
#define FAA_H2              0x08
#define FAA_X4              0x20
#define FAA_W4              0x40

#define FAA_EVEN_X          FAA_X2
#define FAA_EVEN_Y          FAA_Y2
#define FAA_EVEN_W          FAA_W2
#define FAA_EVEN_H          FAA_H2
#define FAA_EVEN_PIXELS     FAA_W2

typedef struct
{
   const SYS_OPS *sys_ops;
   void* render;
   UG_U8 asyncable_pfb;
   UG_S16 x_dim;
   UG_S16 y_dim;
   UG_TOUCH touch;
   UG_TOUCH last_touch;
   UG_GESTURE gesture;
   UG_WINDOW* next_window;
   UG_WINDOW* active_window;
   UG_WINDOW* last_window;

   UG_FONT font;
   UG_S8 char_h_space;
   UG_S8 char_v_space;
   UG_COLOR desktop_color;
   UG_U8 state;
   UG_DRIVER driver[NUMBER_OF_DRIVERS];

   UG_FOCUS_EFFECT focus_effect;
} UG_GUI;

#define UG_SATUS_ON_GESTURE                      (1<<0)

#define UG_MALLOC   ug_mem_alloc
#define UG_FREE     ug_mem_free
/* -------------------------------------------------------------------------------- */
/* -- PROTOTYPES                                                                 -- */
/* -------------------------------------------------------------------------------- */
EXPORT_API UG_OBJECT* UG_FindObject(UG_WINDOW* wnd, UG_ID id);

/* Classic functions */
EXPORT_API UG_S16 UG_Init( UG_GUI* g, const SYS_OPS *ops, const SYS_CONFIG* conf);
EXPORT_API void UG_Deinit(UG_GUI* g);
EXPORT_API UG_S16 UG_SelectGUI( UG_GUI* g );
EXPORT_API UG_GUI *UG_GetGUI();

EXPORT_API UG_S16 UG_GetXDim( void );
EXPORT_API UG_S16 UG_GetYDim( void );

/* Font functions */
EXPORT_API void UG_FontSelect(const UG_FONT* font);
EXPORT_API void UG_FontSetHSpace( UG_U16 s );
EXPORT_API void UG_FontSetVSpace( UG_U16 s );
EXPORT_API UG_S8 UG_FontGetHSpace();
EXPORT_API UG_S8 UG_FontGetVSpace();

/* Miscellaneous functions */
EXPORT_API void UG_Update( void );
EXPORT_API void UG_TickInc(UG_U32 tick_period);
EXPORT_API UG_U32 UG_TickGet(void);
EXPORT_API void UG_TouchUpdate( UG_S16 xp, UG_S16 yp, UG_U8 state );

/* Driver functions */
EXPORT_API void UG_DriverRegister( UG_U8 type, void* driver );
EXPORT_API void UG_DriverEnable( UG_U8 type );
EXPORT_API void UG_DriverDisable( UG_U8 type );

/*----------------------焦点类APIs----------------------*/
/* 设置/取消焦点对象 */
EXPORT_API UG_RESULT UG_SetFocus(UG_OBJECT* obj, UG_BOOL focus);
/* 获取当前窗体焦点对象 */
EXPORT_API UG_OBJECT* UG_GetFocusedObject();
/* 判断某个对象是否焦点对象 */
EXPORT_API UG_BOOL UG_IsFocusedObject(UG_OBJECT* obj);
/* 设定聚焦时的标记样式 */
EXPORT_API void UG_SetFocusEffect(const UG_FOCUS_EFFECT *effect);
/* 移动焦点到下一个/上一个对象 */
EXPORT_API UG_OBJECT* UG_FocusFoward();
EXPORT_API UG_OBJECT* UG_FocusBackward();
/* 移动焦点到上方/下方/左侧/右侧对象 */
EXPORT_API UG_OBJECT* UG_FocusMoveToUp();
EXPORT_API UG_OBJECT* UG_FocusMoveToDown();
EXPORT_API UG_OBJECT* UG_FocusMoveToLeft();
EXPORT_API UG_OBJECT* UG_FocusMoveToRight();
#ifdef __cplusplus
}
#endif
#endif
