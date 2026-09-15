#include "synwit_ui_framework/synwit_ui.h"
#include "synwit_ui_framework/synwit_ui_internal.h"
#include "screen_id.h"
#include "app.h"

/*
 * Register screens
 */
APP_OBJ_DECLARE(screen100_cb_obj)
APP_OBJ_DECLARE(screen301_cb_obj)
APP_OBJ_DECLARE(registration_success_cb_obj)
APP_OBJ_DECLARE(activation_success_cb_obj)
APP_OBJ_DECLARE(activation_failed_cb_obj)
APP_OBJ_DECLARE(connection_failed_cb_obj)
APP_OBJ_DECLARE(registration_failed_cb_obj)
APP_OBJ_DECLARE(connection_success_cb_obj)
APP_OBJ_DECLARE(activation_title_cb_obj)
APP_OBJ_DECLARE(registering_cb_obj)
APP_OBJ_DECLARE(connecting_cb_obj)
APP_OBJ_DECLARE(not_activated_cb_obj)

SCREEN_REG_BEGIN
    SCREEN_BIND(SCREEN100, screen100_cb_obj)
    SCREEN_BIND(SCREEN301, screen301_cb_obj)
    SCREEN_BIND(SCREEN_REGISTRATION_SUCCESS, registration_success_cb_obj)
    SCREEN_BIND(SCREEN_ACTIVATION_SUCCESS, activation_success_cb_obj)
    SCREEN_BIND(SCREEN_ACTIVATION_FAILED, activation_failed_cb_obj)
    SCREEN_BIND(SCREEN_CONNECTION_FAILED, connection_failed_cb_obj)
    SCREEN_BIND(SCREEN_REGISTRATION_FAILED, registration_failed_cb_obj)
    SCREEN_BIND(SCREEN_CONNECTION_SUCCESS, connection_success_cb_obj)
    SCREEN_BIND(SCREEN_ACTIVATION_TITLE, activation_title_cb_obj)
    SCREEN_BIND(SCREEN_REGISTERING, registering_cb_obj)
    SCREEN_BIND(SCREEN_CONNECTING, connecting_cb_obj)
    SCREEN_BIND(SCREEN_NOT_ACTIVATED, not_activated_cb_obj)
SCREEN_REG_END

#ifndef WIN32
/*
 * Register widgets
 */
void _ug_widget_register()
{
    USING_WIDGET(levelimg);
    USING_WIDGET(screen);
}
#endif /* WIN32 */

