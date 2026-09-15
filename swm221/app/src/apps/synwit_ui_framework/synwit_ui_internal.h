#ifndef SYNWIT_UGUI_UI_INTERNAL_H
#define SYNWIT_UGUI_UI_INTERNAL_H

#include "synwit_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App object */
typedef struct {
    UG_ID scr_id;
    const synwit_ug_screen_callback_t* app_obj;
}ug_scr_reg_t;

EXPORT_API void _synwit_ug_set_screen_map(const ug_scr_reg_t* map, int num);

#define APP_OBJ_DECLARE(obj)  extern const synwit_ug_screen_callback_t obj;
#define SCREEN_REG_BEGIN   const ug_scr_reg_t g_screen_map[] = {
#define SCREEN_REG_END   }; void _ug_app_screen_register() { _synwit_ug_set_screen_map(g_screen_map, sizeof(g_screen_map) / sizeof(g_screen_map[0])); }
#define SCREEN_BIND(id, obj) \
    { .scr_id = id, .app_obj = &obj },


#define USING_WIDGET(name) \
	extern void using_widget_##name(); \
	using_widget_##name();


#ifdef WIN32_UGUI_SIMULATION
	void _ug_app_screen_register();
	#define SIMULATION_EXPORT __declspec(dllexport)
#else
	#ifndef WIN32
		void _ug_app_screen_register();
	#endif
	#define SIMULATION_EXPORT 
#endif

/* SWM166/SWM211只有64KB片内flash，已无法存放所有控件，
 * 所以即使是默认工程，也仅能支持部分控件
 */
#define IMPL_DEFAULT_WIDGET_REGISTER_FOR_64KB_FLASH \
        void _ug_widget_register()  \
        {   \
            USING_WIDGET(arc); \
            USING_WIDGET(bar); \
            USING_WIDGET(button); \
            USING_WIDGET(image); \
            USING_WIDGET(imgbtn); \
            USING_WIDGET(label); \
            USING_WIDGET(levelimg); \
            USING_WIDGET(screen); \
        }

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* SYNWIT_UGUI_UI_INTERNAL_H */