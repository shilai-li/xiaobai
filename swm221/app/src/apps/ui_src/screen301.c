#include "synwit_ui_framework/synwit_ui.h"
#include "appkit/screen_id.h"
#include "appkit/screens/screen301.h"

/*
 * 界面回调函数
 */
extern volatile int finish_play_flag;
extern volatile int sleep_interupt_play_flag;

void screen301_finish_callback(UG_OBJECT* obj, void* data)
{
    finish_play_flag = 0;
    sleep_interupt_play_flag = 1;
}

static void screen301_on_timer(UG_TIMER * timer, void* data)
{
    /* 界面定时器回调函数 */
}

static void screen301_on_msg(UG_MESSAGE * msg, UG_WINDOW * wnd)
{
    /* 界面消息回调函数 */
}

static void screen301_start(UG_ID screen_id, UG_WINDOW *wnd)
{
    UG_OBJECT* levelimg;

    levelimg = UG_FindObject(wnd, WIDGET_LEVELIMG_2);
    UG_LevelImgSetAnimReadyCb(levelimg, screen301_finish_callback, NULL);
    /* 在界面被显示给用户前，这个接口会被调用。 */

    // 打开下面的注释可以为本界面开启一个每100ms触发一次的定时器
    //synwit_ug_start_scr_timer(100, NULL);
}

static void screen301_stop(UG_ID screen_id, UG_WINDOW * wnd)
{
    /* 准备切换到其它界面前，这个接口会被调用。 */
}


/*
 * 界面注册对象
 */
const synwit_ug_screen_callback_t screen301_cb_obj = {
    .on_screen_message = screen301_on_msg,
    .on_screen_start = screen301_start,
    .on_screen_stop = screen301_stop,
    .on_screen_timer = screen301_on_timer,
};

