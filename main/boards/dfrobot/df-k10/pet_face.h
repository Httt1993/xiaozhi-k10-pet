#pragma once

/* 宠物猫脸底图（LVGL 图像描述符）。
 *
 * 只有「渐变背景 + 腮红 + 鼻子」——眼睛和嘴已经挖掉，运行时由
 * pet_face_view.cc 用 LVGL 图形对象绘制，这样才能眨眼、换表情。
 * 数据为 RGB565，与 K10 的 ILI9341（240x320）原生格式一致，无需运行时转换。
 * 由 _hw/pet_face/gen_base.py 生成。 */

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t pet_face_base;

#ifdef __cplusplus
}
#endif
