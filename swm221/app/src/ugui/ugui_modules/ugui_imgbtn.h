#ifndef UGUI_IMGBTN_H
#define UGUI_IMGBTN_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"
#include "ugui_window.h"
#include "ugui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Imgbtn functions */

/* 创建按钮控件 */
EXPORT_API UG_OBJECT* UG_ImgbtnCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置松开状态下使用的图片源 */
EXPORT_API UG_RESULT UG_ImgbtnSetBMP(UG_OBJECT* obj, const UG_BMP* bmp);

/* 设置按压状态下使用的图片源 */
EXPORT_API UG_RESULT UG_ImgbtnSetAltBMP(UG_OBJECT* obj, const UG_BMP* bmp);

/* 设置松开状态下的图片重着色 */
EXPORT_API UG_RESULT UG_ImgbtnSetRecolor(UG_OBJECT* obj, UG_COLOR c);

/* 设置按压状态下的图片重着色 */
EXPORT_API UG_RESULT UG_ImgbtnSetAltRecolor(UG_OBJECT* obj, UG_COLOR c);

/* 设置按钮风格，参考ugui_button.h文件内的BTN_STYLE_XXX定义 */
EXPORT_API UG_RESULT UG_ImgbtnSetStyle(UG_OBJECT* obj, UG_U8 style);

/* 设置开关按钮的开关状态(仅对设置了BTN_STYLE_TOGGLEABLE风格的按钮有效) */
EXPORT_API UG_RESULT UG_ImgbtnToggle(UG_OBJECT* obj, UG_BOOL toggle);

/* 获取按钮的风格，参考ugui_button.h文件内的BTN_STYLE_XXX定义 */
EXPORT_API UG_U8 UG_ImgbtnGetStyle(UG_OBJECT* obj);

/* 获取按钮的开关状态，1表示按钮处于按下状态，0表示按钮处于普通状态 */
EXPORT_API UG_BOOL UG_ImgbtnIsToggled(UG_OBJECT* obj);

/* 获取按钮内的文本控件(Label) */
EXPORT_API UG_OBJECT* UG_ImgbtnGetLabel(UG_OBJECT* obj);

#ifdef __cplusplus
}
#endif
#endif