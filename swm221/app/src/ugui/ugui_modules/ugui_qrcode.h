#ifndef UGUI_QRCODE_H
#define UGUI_QRCODE_H
#include "../ugui_config.h"
#include "ugui_types.h"
#include "ugui_object.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QRCode functions */
EXPORT_API UG_OBJECT* UG_QRCodeCreate(UG_OBJECT* parent, UG_ID id, UG_S16 xs, UG_S16 ys, UG_S16 xe, UG_S16 ye);

/* 设置QRCode的前景色 */
EXPORT_API UG_RESULT UG_QRCodeSetForeColor(UG_OBJECT* obj, UG_COLOR fg);

/* 设置QRCode的背景色 */
EXPORT_API UG_RESULT UG_QRCodeSetBackColor(UG_OBJECT* obj, UG_COLOR bc);

/* 设置QRCode的背景透明度，0~255，0表示全透明，255表示完全不透明*/
EXPORT_API UG_RESULT UG_QRCodeSetBackOpa(UG_OBJECT* obj, UG_U8 opa);

/* 设置最大version，有效值1~40，默认为version 10(57x57大小，最大395字母和数字) */
EXPORT_API UG_RESULT UG_QRCodeSetMaxVersion(UG_OBJECT* obj, UG_U8 max_ver);

/* 对字符串数据str进行编码 */
EXPORT_API UG_RESULT UG_QRCodeEncodeText(UG_OBJECT* obj, char* str);

#ifdef __cplusplus
}
#endif
#endif