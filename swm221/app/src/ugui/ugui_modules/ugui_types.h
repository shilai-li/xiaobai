#ifndef UGUI_TYPES_H
#define UGUI_TYPES_H
#include "../ugui_config.h"
#include "ugui_colors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef UG_S8   UG_RESULT;
typedef void    UG_HANDLE;

typedef struct {
    UG_U8 ratio;
    UG_COLOR color;
    UG_COLOR grad_color;
}UG_COLOR_SEGMENT;

#ifdef __cplusplus
}
#endif
#endif