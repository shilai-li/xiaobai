#ifndef UGUI_IMAGE_H
#define UGUI_IMAGE_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"
#include "ugui_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Image types */
#define IMG_TYPE_BMP                                  (1<<0)

/* Image functions */
EXPORT_API UG_OBJECT* UG_ImageCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置图片源 */
EXPORT_API UG_RESULT UG_ImageSetBMP(UG_OBJECT* obj, const UG_BMP* bmp);

/* 设置重着色(仅对灰度图片有效) */
EXPORT_API UG_RESULT UG_ImageSetRecolor(UG_OBJECT* obj, UG_COLOR recolor);

/* 设置图片在控件区域内的对齐方式，有效值参考ugui.h内的Alignments部分 */
EXPORT_API UG_RESULT UG_ImageSetAlign(UG_OBJECT* obj, UG_U8 align);

/* 获取图片源 */
EXPORT_API UG_RESULT UG_ImageGetBMP(UG_OBJECT* obj, UG_BMP* bmp);

#ifdef __cplusplus
}
#endif
#endif