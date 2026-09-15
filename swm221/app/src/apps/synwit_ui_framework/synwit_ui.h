#ifndef SYNWIT_UGUI_UI_H
#define SYNWIT_UGUI_UI_H

#include "ugui/ugui.h"
#include "synwit_serial_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*on_screen_msg_t)(UG_MESSAGE* msg, UG_WINDOW* wnd);

typedef struct {
    void (*on_screen_start)(UG_ID screen_id, UG_WINDOW *wnd);
    void (*on_screen_stop)(UG_ID screen_id, UG_WINDOW* wnd);
    UG_ON_TIMER on_screen_timer;
    on_screen_msg_t on_screen_message;
}synwit_ug_screen_callback_t;


/**@brief 启动UI框架(阻塞)
* @param[in]  sys_ops    回调函数集
* @param[in]  conf       配置屏幕宽、高、堆大小等信息
*/
EXPORT_API void synwit_ug_start(const SYS_OPS* sys_ops, const SYS_CONFIG *conf);



/**@brief 跳转到指定界面(传0表示跳转到入口界面)
* @param[in]  scr_id    上位机构建界面的ID，定义于文件screen_id.h中
* @return 跳转成功返回0，失败返回<0
*/
EXPORT_API int synwit_ug_load_screen(UG_ID scr_id);



/**@brief 从flash加载外部图片
* @param[in]   addr     图片在flash中的存放地址
* @param[out]  bmp      用于保存加载图片的结构体
* @return  加载成功返回0，否则返回<0
*/
EXPORT_API int synwit_ug_load_bmp(uint32_t addr, UG_BMP* bmp);



/**@brief 返回当前界面的id
* @param[in]  void
* @return  当前界面的id，出错则返回0
*/
EXPORT_API UG_ID synwit_ug_get_cur_screen_id();



/**@brief 返回当前界面的WINDOW指针
* @param[in]  void
* @return  当前界面的WINDOW指针
*/
EXPORT_API UG_WINDOW *synwit_ug_get_cur_window();



/**@brief 为当前界面启动定时器
* @param[in]  period        timer回调的触发间隔(ms)
* @param[in]  user_data     timer回调时传入的用户自定义数据
* @return 成功返回0，失败返回<0
*/
EXPORT_API int synwit_ug_start_scr_timer(uint32_t period, void* user_data);



/**@brief 停止当前界面的定时器
* @param[in]  void
* @return 成功返回0，失败返回<0
*/
EXPORT_API int synwit_ug_stop_scr_timer();



/**@brief 获取平台版本名
* @param[in]  ver_name      保存版本名的缓存
* @param[in]  size          缓存大小
*/
EXPORT_API void synwit_ug_get_platform_version_name(char* ver_name, uint32_t size);



/**@brief 过期接口，已无实际用途，仅保留函数声明做兼容旧版本代码之用
*/
EXPORT_API void synwit_ug_reset_glyph_cache(uint16_t max_num);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* SYNWIT_UGUI_UI_H */