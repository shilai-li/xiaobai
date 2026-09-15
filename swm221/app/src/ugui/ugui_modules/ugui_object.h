#ifndef UGUI_OBJECT_H
#define UGUI_OBJECT_H
#include "../ugui_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct S_OBJECT
{
    struct S_OBJECT* parent;
    struct S_OBJECT* child;
    struct S_OBJECT* next_sibling;
    struct S_OBJECT* prev_sibling;

    UG_ID id;                                 /* object ID                                  */

    UG_AREA a_abs;                            /* absolute area of the object                */

    UG_U8 state;                              /* object state                               */
    UG_U8 cap;                                /* Capabilities */
    UG_U8 touch_state;                        /* object touch state                         */
    UG_U8 type;                               /* object type                                */
}UG_OBJECT;
#include "ugui_window.h"

#define OBJ_ID_AUTO          0
#define OBJ_ID_USER(id)      (0x10000000 + id)

    /* Default IDs */
#define OBJ_ID_0    OBJ_ID_USER(0)
#define OBJ_ID_1    OBJ_ID_USER(1)
#define OBJ_ID_2    OBJ_ID_USER(2)
#define OBJ_ID_3    OBJ_ID_USER(3)
#define OBJ_ID_4    OBJ_ID_USER(4)
#define OBJ_ID_5    OBJ_ID_USER(5)
#define OBJ_ID_6    OBJ_ID_USER(6)
#define OBJ_ID_7    OBJ_ID_USER(7)
#define OBJ_ID_8    OBJ_ID_USER(8)
#define OBJ_ID_9    OBJ_ID_USER(9)
#define OBJ_ID_10   OBJ_ID_USER(10)
#define OBJ_ID_11   OBJ_ID_USER(11)
#define OBJ_ID_12   OBJ_ID_USER(12)
#define OBJ_ID_13   OBJ_ID_USER(13)
#define OBJ_ID_14   OBJ_ID_USER(14)
#define OBJ_ID_15   OBJ_ID_USER(15)
#define OBJ_ID_16   OBJ_ID_USER(16)
#define OBJ_ID_17   OBJ_ID_USER(17)
#define OBJ_ID_18   OBJ_ID_USER(18)
#define OBJ_ID_19   OBJ_ID_USER(19)

/* Object states */
#define OBJ_STATE_INIT                                0
#define OBJ_STATE_FOCUSED                             (1<<2)
#define OBJ_STATE_VISIBLE                             (1<<3)
#define OBJ_STATE_ENABLE                              (1<<4)
#define OBJ_STATE_REDRAW                              (1<<5)
#define OBJ_STATE_TOUCH_ENABLE                        (1<<6)


/* Object capabilities */
#define OBJ_CAP_FOCUSABLE           (1<<0)
#define OBJ_CAP_TOUCHABLE           (1<<1)
#define OBJ_CAP_GESTUREABLE         (OBJ_CAP_TOUCHABLE | (1<<2))
#define OBJ_CAP_GESTURE_PARENT      (1<<3)

/* Ojbect touch states */
#define OBJ_TOUCH_STATE_INIT                 0
#define OBJ_TOUCH_STATE_CHANGED             (1<<0)
#define OBJ_TOUCH_STATE_PRESSED             (1<<1)
#define OBJ_TOUCH_STATE_LONG_PRESSING       (1<<2)

/* 将控件对象设置为不可见 */
EXPORT_API UG_RESULT UG_ObjectHide(UG_OBJECT* obj);

/* 将控件对象设置为可见 */
EXPORT_API UG_RESULT UG_ObjectShow(UG_OBJECT* obj);

/* 重绘对象 */
EXPORT_API void UG_ObjectInvalidate(UG_OBJECT* obj);

/* 设置对象是否可以聚焦 */
EXPORT_API void UG_ObjectSetFocusable(UG_OBJECT* obj, UG_BOOL focusable);

/* 获取控件的可见情况，返回0表示不可见，1表示可见 */
EXPORT_API UG_U8 UG_ObjectIsVisible(UG_OBJECT* obj);

/* 设置控件的位置 */
EXPORT_API UG_RESULT UG_ObjectSetPos(UG_OBJECT* obj, UG_S16 rel_x, UG_S16 rel_y);

/* 获取控件的x位置(相对坐标) */
EXPORT_API UG_S16 UG_ObjectGetX(UG_OBJECT* obj);

/* 获取控件的y位置(相对坐标) */
EXPORT_API UG_S16 UG_ObjectGetY(UG_OBJECT* obj);

/* 获取控件的宽度 */
EXPORT_API UG_S16 UG_ObjectGetWidth(UG_OBJECT* obj);

/* 获取控件的高度 */
EXPORT_API UG_S16 UG_ObjectGetHeight(UG_OBJECT* obj);

/* 删除obj控件极其子控件 */
EXPORT_API UG_RESULT UG_ObjectDelete(UG_OBJECT* obj);

/* 将控件从控件树中分离 */
EXPORT_API UG_RESULT UG_ObjectDetach(UG_OBJECT* obj);

/* 给obj指定新的父控件 */
EXPORT_API UG_RESULT UG_ObjectSetParent(UG_OBJECT* obj, UG_OBJECT *new_parent);

/* 将obj移动到控件层次的最顶层 */
EXPORT_API UG_RESULT UG_ObjectMoveForeground(UG_OBJECT* obj);

/* 将obj移动到控件层次的最底层 */
EXPORT_API UG_RESULT UG_ObjectMoveBackground(UG_OBJECT* obj);

/* 由顶至下遍历obj下的子控件, child第1次传入NULL，之后传入返回的UG_OBJECT指针，返回值为NULL则表示已遍历完毕 */
EXPORT_API UG_OBJECT* UG_ObjectGetChild(UG_OBJECT* obj, UG_OBJECT *child);

/* 由底至上遍历obj下的子控件, child第1次传入NULL，之后传入返回的UG_OBJECT指针，返回值为NULL则表示已遍历完毕 */
EXPORT_API UG_OBJECT* UG_ObjectGetChildBack(UG_OBJECT* obj, UG_OBJECT* child);

#ifdef __cplusplus
}
#endif
#endif