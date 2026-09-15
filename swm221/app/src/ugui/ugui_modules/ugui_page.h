#ifndef UGUI_PAGE_H
#define UGUI_PAGE_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"

/* Page的滚动模式 */
#define PAGE_SCROLL_MODE_NONE              0   // 禁止滚动
#define PAGE_SCROLL_MODE_HOR_CONTINOUS     1   // 水平连续滚动模式
#define PAGE_SCROLL_MODE_VER_CONTINOUS     2   // 垂直连续滚动模式
#define PAGE_SCROLL_MODE_ANY_CONTINOUS     3   // 任意方向连续滚动模式
#define PAGE_SCROLL_MODE_HOR_PAGING        4   // 水平分页模式
#define PAGE_SCROLL_MODE_VER_PAGING        5   // 垂直分页模式


#ifdef __cplusplus
extern "C" {
#endif

/* Page functions */
EXPORT_API UG_OBJECT* UG_PageCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置Page的背景色 */
EXPORT_API UG_RESULT UG_PageSetBackColor(UG_OBJECT* page, UG_COLOR bc);

/* 设置Page的背景渐变终止色 */
EXPORT_API UG_RESULT UG_PageSetBackColorGrad(UG_OBJECT* page, UG_COLOR grad);

/* 设置Page的背景色渐变方向：
    GRAD_DIR_NONE   无
    GRAD_DIR_VER    垂直渐变
    GRAD_DIR_HOR    水平渐变
 */
EXPORT_API UG_RESULT UG_PageSetGradDir(UG_OBJECT* page, UG_U8 dir);

/* 设置Page的背景透明度，0~255，0表示全透明，255表示完全不透明*/
EXPORT_API UG_RESULT UG_PageSetBackOpa(UG_OBJECT* page, UG_U8 opa);

/* 设置Page的滚动模式:
        PAGE_SCROLL_MODE_NONE              禁止滚动
        PAGE_SCROLL_MODE_HOR_CONTINOUS     水平连续滚动模式(默认)
        PAGE_SCROLL_MODE_VER_CONTINOUS     垂直连续滚动模式
        PAGE_SCROLL_MODE_ANY_CONTINOUS     任意方向连续滚动模式
        PAGE_SCROLL_MODE_HOR_PAGING        水平分页模式
        PAGE_SCROLL_MODE_VER_PAGING        垂直分页模式
 */
EXPORT_API UG_RESULT UG_PageSetScrollMode(UG_OBJECT* page, UG_U8 mode);

/* 设置Page的虚拟宽度，用于限制水平连续滚动的边界 */
EXPORT_API UG_RESULT UG_PageSetVirtualWidth(UG_OBJECT* page, UG_S16 v_width);

/* 设置Page的虚拟宽度，用于限制垂直连续滚动的边界 */
EXPORT_API UG_RESULT UG_PageSetVirtualHeight(UG_OBJECT* page, UG_S16 v_height);

/* 设置Page在分页模式下，过渡动画的速度, 0~255，数值越大速度越快，0表示不使用过渡效果，默认值为252 */
EXPORT_API UG_RESULT UG_PageSetAnimSpeed(UG_OBJECT* page, UG_U8 speed);

/* 将Page视口的左上坐标设置到(x,y)位置，anim表示移到该位置时是否需要过渡效果 */
EXPORT_API UG_RESULT UG_PageSetViewportPos(UG_OBJECT* page, UG_S16 x, UG_S16 y, UG_BOOL anim);

/* 在分页模式下，将Page切换到指定页，anim表示切换到该页时是否需要过渡效果 */
EXPORT_API UG_RESULT UG_PageSetPage(UG_OBJECT* page, UG_S16 n_page, UG_BOOL anim);

/* 将obj放置入page内第n_page页，(x, y)位置处 */
EXPORT_API UG_RESULT UG_PagePutObject(UG_OBJECT* page, UG_OBJECT *obj, UG_S16 n_page, UG_S16 x, UG_S16 y);

/* 设置在边界位置继续拖拽时是否有回弹效果，en为UG_TRUE表示开启回弹效果 */
EXPORT_API UG_RESULT UG_PageSetEdgeBounce(UG_OBJECT* page, UG_BOOL en);

/* 获取Page的虚拟宽度和高度 */
EXPORT_API UG_RESULT UG_PageGetVirtualSize(UG_OBJECT* page, UG_S16 *v_width, UG_S16 *v_height);

/* 在分页模式下，获取当前所在的页 */
EXPORT_API UG_S16 UG_PageGetCurPage(UG_OBJECT* page);

/* 获取当前视口的左上角坐标 */
EXPORT_API void UG_PageGetViewportPos(UG_OBJECT* page, UG_S16 *x, UG_S16 *y);

#ifdef __cplusplus
}
#endif
#endif