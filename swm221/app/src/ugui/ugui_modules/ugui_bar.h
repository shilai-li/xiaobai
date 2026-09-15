#ifndef UGUI_BAR_H
#define UGUI_BAR_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"
#include "ugui_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bar functions */
EXPORT_API UG_OBJECT* UG_BarCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置Bar的进度值 */
EXPORT_API UG_RESULT UG_BarSetValue(UG_OBJECT* obj, UG_S16 value);

/* 设置Bar的进度最小值 */
EXPORT_API UG_RESULT UG_BarSetValMin(UG_OBJECT* obj, UG_S16 min);

/* 设置Bar的进度最大值 */
EXPORT_API UG_RESULT UG_BarSetValMax(UG_OBJECT* obj, UG_S16 max);

/* 获取Bar当前的进度值 */
EXPORT_API UG_S16 UG_BarGetValue(UG_OBJECT* obj);

/* 获取Bar当前的进度最小值 */
EXPORT_API UG_S16 UG_BarGetValMin(UG_OBJECT* obj);

/* 获取Bar当前的进度最大值 */
EXPORT_API UG_S16 UG_BarGetValMax(UG_OBJECT* obj);

/* 设置进度条背景色 */
EXPORT_API void UG_BarSetBackColor(UG_OBJECT* obj, UG_COLOR color);

/* 设置进度条颜色 */
EXPORT_API void UG_BarSetIndicColor(UG_OBJECT* obj, UG_COLOR color);

/* 设置进度条渐变终色 */
EXPORT_API void UG_BarSetIndicGradColor(UG_OBJECT* obj, UG_COLOR color);

/* 设置进度条渐变方向
 *    dir参数可设置为GRAD_DIR_NONE、GRAD_DIR_VER、GRAD_DIR_HOR之一
 */
EXPORT_API void UG_BarSetIndicGradDir(UG_OBJECT* obj, UG_U8 dir);

/* 设置进度条边框颜色 */
EXPORT_API void UG_BarSetBorderColor(UG_OBJECT* obj, UG_COLOR color);

/* 设置进度条边框粗细 */
EXPORT_API void UG_BarSetBorderWidth(UG_OBJECT* obj, UG_U8 width);



/* 设置圆弧的颜色分段， segs指向分段表，num_of_segs为分段总数。
 * 【注意】分段结构的v字段为百分比，分段表必须按百分比由小到大按序排列。
 *
 * 示例：
 *    UG_COLOR_SEGMENT segs[] = {
 *       {25, C_GREEN,  C_GREEN},       // (0%, 25%]颜色为绿色(不带渐变)
 *       {50, C_CYAN,   C_LIGHT_CYAN},  // (25%, 50%]颜色为青色(渐变效果)
 *       {75, C_ORANGE, C_GOLD},        // (50%, 75%]颜色为橙色(渐变效果)
 *       {100, C_RED,   C_RED},         // (75%, 100%]颜色为红色(不带渐变)
 *    };
 *    UG_BarSetColorSegments(arc, segs, 4);
 */
EXPORT_API UG_RESULT UG_BarSetColorSegments(UG_OBJECT* obj, const UG_COLOR_SEGMENT* segs, UG_U8 num_of_segs);


/* 设置进度条是否允许触控调节, enabled传UG_TRUE表示允许，UG_FALSE表示禁止触控调节 */
EXPORT_API void UG_BarSetAdjustable(UG_OBJECT* obj, UG_BOOL enabled);

#ifdef __cplusplus
}
#endif

#endif