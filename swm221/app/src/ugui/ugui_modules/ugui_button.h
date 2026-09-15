#ifndef UGUI_BUTTON_H
#define UGUI_BUTTON_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"
#include "ugui_window.h"
#include "ugui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Button states */
#define BTN_STATE_TOGGLED                             (1<<0)

/* Button style */
#define BTN_STYLE_NO_BORDERS                          (1<<0)
#define BTN_STYLE_NO_FILL                             (1<<1)
#define BTN_STYLE_3D                                  (1<<2)
#define BTN_STYLE_TOGGLEABLE                          (1<<3)

#define BTN_STYLE_DEFAULT       (0)

/* Button functions */

/* 创建按钮控件 */
EXPORT_API UG_OBJECT* UG_ButtonCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置边框宽度*/
EXPORT_API UG_RESULT UG_ButtonSetBorderWidth(UG_OBJECT* obj, UG_U8 width);

/* 设置按钮正常状态下的边框颜色 */
EXPORT_API UG_RESULT UG_ButtonSetBorderColor(UG_OBJECT* obj, UG_COLOR bc);

/* 设置按钮按压状态下的边框颜色 */
EXPORT_API UG_RESULT UG_ButtonSetAlternateBorderColor(UG_OBJECT* obj, UG_COLOR bc);

/* 设置按钮正常状态下的背景色 */
EXPORT_API UG_RESULT UG_ButtonSetBackColor(UG_OBJECT* obj, UG_COLOR bc);

/* 设置按钮按压状态下的背景色 */
EXPORT_API UG_RESULT UG_ButtonSetAlternateBackColor(UG_OBJECT* obj, UG_COLOR abc);

/* 设置按钮正常状态下的背景渐变终止色 */
EXPORT_API UG_RESULT UG_ButtonSetBackColorGrad(UG_OBJECT* obj, UG_COLOR grad);

/* 设置按钮按压状态下的背景渐变终止色 */
EXPORT_API UG_RESULT UG_ButtonSetAlternateBackColorGrad(UG_OBJECT* obj, UG_COLOR grad);

/* 设置按钮边框透明度，0~255，0表示全透明，255表示完全不透明*/
EXPORT_API UG_RESULT UG_ButtonSetBorderOpa(UG_OBJECT* obj, UG_U8 opa);

/* 设置按钮背景透明度，0~255，0表示全透明，255表示完全不透明*/
EXPORT_API UG_RESULT UG_ButtonSetBackOpa(UG_OBJECT* obj, UG_U8 opa);

/* 设置按钮按压状态下的背景透明度，0~255，0表示全透明，255表示完全不透明*/
EXPORT_API UG_RESULT UG_ButtonSetAlternateBackOpa(UG_OBJECT* obj, UG_U8 opa);

/* 设置按钮的背景色渐变方向：
    GRAD_DIR_NONE   无
    GRAD_DIR_VER    垂直渐变
    GRAD_DIR_HOR    水平渐变
 */
EXPORT_API UG_RESULT UG_ButtonSetGradDir(UG_OBJECT* obj, UG_U8 dir);

/* 设置按钮风格，参考本文件内的BTN_STYLE_XXX定义 */
EXPORT_API UG_RESULT UG_ButtonSetStyle(UG_OBJECT* obj, UG_U8 style);

/* 设置开关按钮的开关状态(仅对设置了BTN_STYLE_TOGGLEABLE风格的按钮有效) */
EXPORT_API UG_RESULT UG_ButtonToggle(UG_OBJECT* obj, UG_BOOL toggle);

/* 获取按钮风格 */
EXPORT_API UG_U8 UG_ButtonGetStyle(UG_OBJECT* obj);

/* 获取按钮的开关状态，1表示按钮处于按下状态，0表示按钮处于普通状态 */
EXPORT_API UG_BOOL UG_ButtonIsToggled(UG_OBJECT* obj);

/* 获取按钮内的文本控件(Label) */
EXPORT_API UG_OBJECT* UG_ButtonGetLabel(UG_OBJECT* obj);

#ifdef __cplusplus
}
#endif
#endif