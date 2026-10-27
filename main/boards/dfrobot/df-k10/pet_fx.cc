#include "pet_fx.h"

#include <esp_random.h>

#include <cstring>

namespace {

const lv_color_t kPink = lv_color_hex(0xFF8FAB);
const lv_color_t kPinkDeep = lv_color_hex(0xFF5C7A);
const lv_color_t kButter = lv_color_hex(0xFFD97D);
const lv_color_t kLilac = lv_color_hex(0xB8A9E8);
const lv_color_t kSky = lv_color_hex(0x6EC1E4);
const lv_color_t kInk = lv_color_hex(0x1A1210);
const lv_color_t kTearBlue = lv_color_hex(0x7FB8E8);

lv_obj_t* MakeCircle(lv_obj_t* parent, int r, lv_color_t c) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(
                             LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t* MakeBar(lv_obj_t* parent, int w, int h, int angle_tenth,
                  lv_color_t c) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_transform_rotation(o, angle_tenth, 0);
    lv_obj_set_style_transform_pivot_x(o, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(o, h / 2, 0);
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(
                             LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t* MakeText(lv_obj_t* parent, const char* txt, lv_color_t c, int size) {
    /* 刻意用 lv_font_default() 而不是 montserrat_xx：
     * sdkconfig 没有显式打开 montserrat 系列，引了会链接失败。
     * 默认字体一定存在，大小差异用 transform_scale 补。 */
    lv_obj_t* o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_text_color(o, c, 0);
    lv_obj_set_style_text_font(o, lv_font_default(), 0);
    lv_label_set_text(o, txt);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    if (size >= 22) {
        lv_obj_set_style_transform_scale_x(o, 384, 0); /* 1.5x */
        lv_obj_set_style_transform_scale_y(o, 384, 0);
        lv_obj_set_style_transform_pivot_x(o, 8, 0);
        lv_obj_set_style_transform_pivot_y(o, 8, 0);
    }
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(
                             LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

}  // namespace

PetFxLayer::PetFxLayer(lv_obj_t* parent) {
    layer_ = lv_obj_create(parent);
    lv_obj_remove_style_all(layer_);
    lv_obj_set_size(layer_, 240, 320);
    lv_obj_set_pos(layer_, 0, 0);
    lv_obj_set_style_bg_opa(layer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer_, 0, 0);
    lv_obj_set_style_pad_all(layer_, 0, 0);
    lv_obj_clear_flag(layer_, static_cast<lv_obj_flag_t>(
                                  LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_CLICKABLE |
                                  LV_OBJ_FLAG_EVENT_BUBBLE));
    lv_obj_move_foreground(layer_);
}

PetFxLayer::~PetFxLayer() {
    Clear();
    if (layer_ != nullptr) {
        lv_obj_delete(layer_);
        layer_ = nullptr;
    }
}

void PetFxLayer::Release(Slot& s) {
    if (s.root != nullptr) {
        lv_anim_delete(s.root, AnimY);
        lv_anim_delete(s.root, AnimX);
        lv_obj_delete(s.root);
        s.root = nullptr;
    }
    for (auto& p : s.parts) {
        p = nullptr;
    }
    s.busy = false;
}

void PetFxLayer::Clear() {
    for (auto& s : slots_) {
        Release(s);
    }
}

void PetFxLayer::AnimX(lv_anim_t* a) {
    auto* o = static_cast<lv_obj_t*>(a->var);
    lv_obj_set_x(o, a->current_value);
}

void PetFxLayer::AnimY(lv_anim_t* a) {
    auto* o = static_cast<lv_obj_t*>(a->var);
    lv_obj_set_y(o, a->current_value);
}

void PetFxLayer::AnimReady(lv_anim_t* a) {
    auto* o = static_cast<lv_obj_t*>(a->var);
    /* 动画结束 → 找到所属 layer 的槽位释放。用 user_data 指回槽位。 */
    auto* slot = static_cast<Slot*>(a->user_data);
    if (slot != nullptr && slot->root == o) {
        lv_obj_delete(slot->root);
        slot->root = nullptr;
        slot->busy = false;
    }
}

void PetFxLayer::BuildShape(Slot& s, PetFxKind kind, int x, int y) {
    s.root = lv_obj_create(layer_);
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, 48, 48);
    lv_obj_set_pos(s.root, x - 24, y - 24);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.root, 0, 0);
    lv_obj_set_style_pad_all(s.root, 0, 0);
    lv_obj_clear_flag(s.root, static_cast<lv_obj_flag_t>(
                                  LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_CLICKABLE));

    switch (kind) {
        case PET_FX_HEART: {
            /* 心 = 两个圆 + 一个旋转方块（菱形当心尖） */
            s.parts[0] = MakeCircle(s.root, 7, kPinkDeep);
            lv_obj_set_pos(s.parts[0], 10, 12);
            s.parts[1] = MakeCircle(s.root, 7, kPinkDeep);
            lv_obj_set_pos(s.parts[1], 22, 12);
            s.parts[2] = MakeBar(s.root, 20, 20, 450, kPinkDeep);
            lv_obj_set_pos(s.parts[2], 14, 18);
            break;
        }
        case PET_FX_STAR: {
            /* 星 = 两条交叉圆角条（四角星） */
            s.parts[0] = MakeBar(s.root, 22, 6, 0, kButter);
            lv_obj_set_pos(s.parts[0], 13, 21);
            s.parts[1] = MakeBar(s.root, 22, 6, 900, kButter);
            lv_obj_set_pos(s.parts[1], 13, 21);
            s.parts[2] = MakeCircle(s.root, 3, kButter);
            lv_obj_set_pos(s.parts[2], 21, 21);
            break;
        }
        case PET_FX_ZZZ: {
            s.parts[0] = MakeText(s.root, "z", kLilac, 16);
            lv_obj_set_pos(s.parts[0], 6, 22);
            s.parts[1] = MakeText(s.root, "z", kLilac, 16);
            lv_obj_set_pos(s.parts[1], 18, 10);
            s.parts[2] = MakeText(s.root, "Z", kLilac, 22);
            lv_obj_set_pos(s.parts[2], 28, -2);
            break;
        }
        case PET_FX_ANGER: {
            /* 💢 = 两条交叉短粗线 */
            s.parts[0] = MakeBar(s.root, 18, 5, 300, kPinkDeep);
            lv_obj_set_pos(s.parts[0], 15, 21);
            s.parts[1] = MakeBar(s.root, 18, 5, -300, kPinkDeep);
            lv_obj_set_pos(s.parts[1], 15, 21);
            break;
        }
        case PET_FX_QUESTION: {
            s.parts[0] = MakeText(s.root, "?", kSky, 22);
            lv_obj_set_pos(s.parts[0], 16, 12);
            break;
        }
        case PET_FX_TEAR: {
            /* 泪滴 = 小圆 + 下方收窄的短条 */
            s.parts[0] = MakeCircle(s.root, 5, kTearBlue);
            lv_obj_set_pos(s.parts[0], 19, 16);
            s.parts[1] = MakeBar(s.root, 6, 8, 0, kTearBlue);
            lv_obj_set_pos(s.parts[1], 21, 24);
            break;
        }
    }
}

void PetFxLayer::Spawn(PetFxKind kind, int x, int y) {
    /* 找空槽；满了就回收最旧的（简单起见直接用第一个 busy 的） */
    Slot* slot = nullptr;
    for (auto& s : slots_) {
        if (!s.busy) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &slots_[0];
        Release(*slot);
    }
    slot->busy = true;
    BuildShape(*slot, kind, x, y);

    /* 动画：平移 + 淡出。泪滴向下、其它向上飘；星星带轻微横摆。 */
    const int dur = (kind == PET_FX_TEAR) ? 1500 : 1200;
    const int dy = (kind == PET_FX_TEAR) ? 60 : -50;
    const int dx =
        (kind == PET_FX_HEART) ? static_cast<int>(esp_random() % 21) - 10 : 0;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slot->root);
    lv_anim_set_exec_cb(&a, AnimY);
    lv_anim_set_values(&a, lv_obj_get_y(slot->root),
                       lv_obj_get_y(slot->root) + dy);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, slot);
    lv_anim_set_ready_cb(&a, AnimReady);
    lv_anim_start(&a);

    if (dx != 0) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, slot->root);
        lv_anim_set_exec_cb(&b, AnimX);
        lv_anim_set_values(&b, lv_obj_get_x(slot->root),
                           lv_obj_get_x(slot->root) + dx);
        lv_anim_set_time(&b, dur);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_start(&b);
    }

    /* 整体透明度随动画淡出 */
    lv_obj_set_style_opa(slot->root, LV_OPA_COVER, 0);
    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, slot->root);
    lv_anim_set_exec_cb(&o, [](lv_anim_t* a) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(a->var),
                             static_cast<lv_opa_t>(a->current_value), 0);
    });
    lv_anim_set_values(&o, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&o, dur);
    lv_anim_start(&o);
}

void PetFxLayer::SpawnSleepZ() {
    /* 右上角轮流冒 Zzz */
    static const int kPos[3][2] = {{186, 96}, {200, 78}, {172, 112}};
    const int* p = kPos[zzz_flip_ % 3];
    zzz_flip_++;
    Spawn(PET_FX_ZZZ, p[0], p[1]);
}
