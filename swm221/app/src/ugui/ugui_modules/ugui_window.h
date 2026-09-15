#ifndef UGUI_WINDOW_H
#define UGUI_WINDOW_H
#include "../ugui_config.h"
#include "ugui_object.h"
#include "ugui_colors.h"
#include "ugui_geometry.h"
#include "ugui_message.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct S_WINDOW
{
    UG_OBJECT obj;

    UG_OBJECT* focused_obj;
    UG_OBJECT* act_obj;

    UG_AREA inv[24];
    UG_U8 invcnt;
    UG_U8 invmerged;
    UG_COLOR bc;
    UG_U8 style;
    void (*cb)(UG_MESSAGE*);
}UG_WINDOW;

/* Window styles */
#define WND_STYLE_2D                                  (0<<0)
#define WND_STYLE_3D                                  (1<<0)

/* Window functions */
EXPORT_API UG_WINDOW* UG_WindowCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye,
                                        void (*cb)(UG_MESSAGE*));
EXPORT_API UG_RESULT UG_WindowShow(UG_WINDOW* wnd);
UG_RESULT UG_WindowSetBackColor(UG_WINDOW* wnd, UG_COLOR bc);

EXPORT_API UG_RESULT UG_WindowSetStyle(UG_WINDOW* wnd, UG_U8 style);
EXPORT_API UG_COLOR UG_WindowGetBackColor(UG_WINDOW* wnd);
EXPORT_API UG_U8 UG_WindowGetStyle(UG_WINDOW* wnd);

#ifdef __cplusplus
}
#endif
#endif