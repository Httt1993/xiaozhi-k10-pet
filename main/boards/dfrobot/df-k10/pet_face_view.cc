#include "pet_face_view.h"

#include <esp_log.h>
#include <esp_random.h>
#include <strings.h>

#include <cstring>

#include "pet_face.h"

#define TAG "PetFaceView"

namespace {

/* ---- 版式基准：全部按 240x320 原图坐标，和 _hw/pet_face/analyze.py 的实测值一致 ---- */
constexpr int kScreenW = 240;
constexpr int kScreenH = 320;

constexpr int kEyeCxL = 78;   /* 左眼中心 x（实测 77.5） */
constexpr int kEyeCxR = 162;  /* 右眼中心 x（实测 161.5） */

/* 高光相对眼中心的偏移（实测：大高光偏左上，小高光偏右下） */
constexpr int kHl1Dx = -5;
constexpr int kHl1Dy = -6;
constexpr int kHl1D = 21;
constexpr int kHl2Dx = 10;
constexpr int kHl2Dy = 14;
constexpr int kHl2D = 9;

constexpr int kMouthCenterX = 120;

/* 注意：底图上的鼻子下沿在 y≈199，任何嘴都不能高于 200，否则会盖住鼻子。
 * 下面每个表情的 mouth_bottom 都是按这条红线推出来的。 */
constexpr int kNoseBottom = 200;

/* lv_color_hex() 不是 constexpr，只能用 static const */
static const lv_color_t kInk = lv_color_hex(0x1A1210);      /* 与原图眼/嘴的黑一致 */
static const lv_color_t kWhite = lv_color_hex(0xFFFFFF);
static const lv_color_t kTongue = lv_color_hex(0xE8748C);

/* 每个表情一套参数。eye_h=0 表示不用实心椭圆眼（改用弧线眼） */
struct ExprStyle {
    int eye_w, eye_h, eye_y;   /* 实心椭圆眼尺寸与左上角 y（x 由中心推） */
    int arc_w;                 /* 弧线眼宽度，0 = 不用 */
    int arc_thick;
    int arc_top;               /* 弧线眼对象的左上角 y */
    int arc_down;              /* 0 = 只留 top 边（∩ 笑眼）；1 = 只留 bottom（∪ 闭眼） */
    int hl_pct;                /* 高光缩放百分比，0 = 不显示 */
    int mouth_mode;            /* 0=ω 1=单弧 2=小圆 3=张大嘴 */
    int mouth_w;
    int mouth_thick;
    int mouth_bottom;          /* 嘴的基准 y：∪ 底 / ∩ 底两端 / 圆底 / 张嘴底 */
};

/*
 * neutral: 和原图一致 —— 大圆眼 + ω 嘴
 * happy  : ∩ 笑眼 + 宽 ∪ 笑嘴
 * sad    : 眼略小且下移 + ∩ 哭嘴（弧顶压在鼻尖下方）
 * sick   : ∪ 闭眼弧（虚弱）+ 小圆嘴
 * hungry : 眼睛更大 + 张大嘴（带小舌头），嘴整体下沉让开鼻子
 */
const ExprStyle kStyles[PetFaceView::kExprCount] = {
    /*      eye_w eye_h eye_y | arc_w thick top down | hl | mode m_w thick m_bottom */
    /* neutral */ {50, 58, 117, 0, 0, 0, 0, 100, 0, 0, 0, 214},
    /* happy   */ {0, 0, 117, 40, 9, 132, 0, 0, 1, 40, 6, 214},
    /* sad     */ {46, 50, 122, 0, 0, 0, 0, 70, 1, 40, 5, 222},   /* 弧顶 = 222-20 = 202 */
    /* sick    */ {0, 0, 117, 44, 8, 118, 1, 0, 2, 22, 3, 224},   /* 圆顶 = 224-22 = 202 */
    /* hungry  */ {56, 64, 112, 0, 0, 0, 0, 110, 3, 36, 0, 227},  /* 张嘴顶 = 227-27 = 200 */
};

lv_obj_t* MakeObj(lv_obj_t* parent, bool filled) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
                                                    LV_OBJ_FLAG_CLICKABLE |
                                                    LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, kInk, 0);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    }
    return o;
}

/* 配成「只有一条单边边框的圆弧」：单边边框 + 全圆角 = 一段弧 */
void MakeArc(lv_obj_t* o, int w, int h, int thick, lv_border_side_t side,
             lv_color_t color) {
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, thick, 0);
    lv_obj_set_style_border_color(o, color, 0);
    lv_obj_set_style_border_side(o, side, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
}

float EaseInOut(float t) {
    /* 平滑起停，眨眼不至于像硬切换 */
    if (t < 0.5f) {
        return 2.0f * t * t;
    }
    float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * 0.5f;
}

}  // namespace

PetFaceView::PetFaceView(lv_obj_t* parent) { Build(parent); }

void PetFaceView::Build(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, kScreenW, kScreenH);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_clear_flag(root_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
                                                        LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);

    /* 底图：渐变 + 腮红 + 鼻子 */
    lv_obj_t* bg = lv_image_create(root_);
    lv_image_set_src(bg, &pet_face_base);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE |
                                                     LV_OBJ_FLAG_CLICKABLE));

    for (int i = 0; i < 2; i++) {
        eye_[i] = MakeObj(root_, true);
        eye_arc_[i] = MakeObj(root_, false);
        hl1_[i] = MakeObj(root_, true);
        lv_obj_set_style_bg_color(hl1_[i], kWhite, 0);
        hl2_[i] = MakeObj(root_, true);
        lv_obj_set_style_bg_color(hl2_[i], kWhite, 0);
    }

    mouth_omega_[0] = MakeObj(root_, false);
    mouth_omega_[1] = MakeObj(root_, false);
    mouth_arc_ = MakeObj(root_, false);
    mouth_small_ = MakeObj(root_, false);
    mouth_open_ = MakeObj(root_, true);
    tongue_ = MakeObj(root_, true);
    lv_obj_set_style_bg_color(tongue_, kTongue, 0);

    Apply();
    ESP_LOGI(TAG, "PET_PATCH: face view built (vector, blinking enabled)");
}

const char* PetFaceView::ExprName(Expr e) {
    switch (e) {
        case kHappy:
            return "happy";
        case kSad:
            return "sad";
        case kSick:
            return "sick";
        case kHungry:
            return "hungry";
        default:
            return "neutral";
    }
}

const char* PetFaceView::NameList() {
    return "neutral(普通) / happy(开心) / sad(难过) / sick(生病) / hungry(饿了)";
}

PetFaceView::Expr PetFaceView::Parse(const char* name) {
    if (name == nullptr) {
        return kNeutral;
    }
    if (strcasecmp(name, "happy") == 0 || strcasecmp(name, "laughing") == 0) {
        return kHappy;
    }
    if (strcasecmp(name, "sad") == 0 || strcasecmp(name, "crying") == 0) {
        return kSad;
    }
    if (strcasecmp(name, "sick") == 0 || strcasecmp(name, "ill") == 0 ||
        strcasecmp(name, "tired") == 0 || strcasecmp(name, "sleepy") == 0) {
        return kSick;
    }
    if (strcasecmp(name, "hungry") == 0 || strcasecmp(name, "delicious") == 0) {
        return kHungry;
    }
    if (strcasecmp(name, "neutral") == 0 || strcasecmp(name, "relaxed") == 0) {
        return kNeutral;
    }
    /* 中文别名（孩子更可能这么说） */
    if (strstr(name, "开心") != nullptr || strstr(name, "高兴") != nullptr ||
        strstr(name, "笑") != nullptr) {
        return kHappy;
    }
    if (strstr(name, "难过") != nullptr || strstr(name, "伤心") != nullptr ||
        strstr(name, "委屈") != nullptr) {
        return kSad;
    }
    if (strstr(name, "病") != nullptr || strstr(name, "不舒服") != nullptr ||
        strstr(name, "困") != nullptr) {
        return kSick;
    }
    if (strstr(name, "饿") != nullptr) {
        return kHungry;
    }
    return kNeutral;
}

bool PetFaceView::Known(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    static const char* const kKeys[] = {"neutral", "happy",  "sad",  "sick",
                                        "hungry",  "laughing", "crying", "ill",
                                        "tired",   "sleepy", "relaxed", "delicious",
                                        "开心",    "高兴",   "笑",    "难过",
                                        "伤心",    "委屈",   "病",    "不舒服",
                                        "困",      "饿"};
    for (const char* k : kKeys) {
        if (strcasecmp(name, k) == 0) {
            return true;
        }
    }
    for (const char* k : kKeys) {
        if (strstr(name, k) != nullptr) {
            return true;
        }
    }
    return false;
}

void PetFaceView::Apply() {
    if (root_ == nullptr) {
        return;
    }
    const ExprStyle& s = kStyles[expr_];

    eye_w_ = s.eye_w > 0 ? s.eye_w : 50;
    eye_h_ = s.eye_h > 0 ? s.eye_h : 58;
    eye_y_ = s.eye_y;

    /* ---- 眼睛 ---- */
    for (int i = 0; i < 2; i++) {
        int cx = (i == 0) ? kEyeCxL : kEyeCxR;
        if (s.arc_w > 0) {
            /* 弧线眼：只留一条边 → ∩（笑眼）或 ∪（闭眼） */
            Show(eye_[i], false);
            MakeArc(eye_arc_[i], s.arc_w, s.arc_w, s.arc_thick,
                    s.arc_down ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_TOP, kInk);
            lv_obj_set_pos(eye_arc_[i], cx - s.arc_w / 2, s.arc_top);
            Show(eye_arc_[i], true);
        } else {
            Show(eye_arc_[i], false);
            lv_obj_set_style_bg_opa(eye_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(eye_[i], kInk, 0);
            lv_obj_set_pos(eye_[i], cx - eye_w_ / 2, eye_y_);
            lv_obj_set_size(eye_[i], eye_w_, eye_h_);
            Show(eye_[i], true);
        }
    }
    SetEyeOpenRatio(1.0f);

    /* ---- 高光 ---- */
    if (s.hl_pct <= 0) {
        for (int i = 0; i < 2; i++) {
            Show(hl1_[i], false);
            Show(hl2_[i], false);
        }
    } else {
        int d1 = kHl1D * s.hl_pct / 100;
        int d2 = kHl2D * s.hl_pct / 100;
        for (int i = 0; i < 2; i++) {
            int cx = (i == 0) ? kEyeCxL : kEyeCxR;
            int cy = eye_y_ + eye_h_ / 2;
            lv_obj_set_size(hl1_[i], d1, d1);
            lv_obj_set_pos(hl1_[i], cx + kHl1Dx - d1 / 2, cy + kHl1Dy - d1 / 2);
            lv_obj_set_size(hl2_[i], d2, d2);
            lv_obj_set_pos(hl2_[i], cx + kHl2Dx - d2 / 2, cy + kHl2Dy - d2 / 2);
            Show(hl1_[i], true);
            Show(hl2_[i], true);
        }
    }

    /* ---- 嘴 ---- */
    Show(mouth_omega_[0], false);
    Show(mouth_omega_[1], false);
    Show(mouth_arc_, false);
    Show(mouth_small_, false);
    Show(mouth_open_, false);
    Show(tongue_, false);

    switch (s.mouth_mode) {
        case 0: {  /* ω：两个 ∪ 环并排，重叠 2px 让接缝融掉 */
            constexpr int ring_w = 24;
            constexpr int ring_h = 26;
            for (int i = 0; i < 2; i++) {
                MakeArc(mouth_omega_[i], ring_w, ring_h, 5, LV_BORDER_SIDE_BOTTOM, kInk);
                lv_obj_set_pos(mouth_omega_[i], kMouthCenterX - ring_w + i * (ring_w - 2),
                               s.mouth_bottom - ring_h);
                Show(mouth_omega_[i], true);
            }
            break;
        }
        case 1: {  /* 单弧嘴：笑 = ∪，哭 = ∩ */
            const int w = s.mouth_w;
            const bool cry = (expr_ == kSad);
            MakeArc(mouth_arc_, w, w, s.mouth_thick,
                    cry ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_BOTTOM, kInk);
            /* ∪ 的可见弧占对象下半（高 w/2），底边对齐 mouth_bottom；
             * ∩ 的可见弧占对象上半（高 w/2），底端对齐 mouth_bottom。 */
            const int top = cry ? (s.mouth_bottom - w / 2) : (s.mouth_bottom - w);
            lv_obj_set_pos(mouth_arc_, kMouthCenterX - w / 2, top);
            Show(mouth_arc_, true);
            break;
        }
        case 2: {  /* 小圆嘴（生病） */
            const int w = s.mouth_w;
            MakeArc(mouth_small_, w, w, s.mouth_thick, LV_BORDER_SIDE_FULL, kInk);
            lv_obj_set_pos(mouth_small_, kMouthCenterX - w / 2, s.mouth_bottom - w);
            Show(mouth_small_, true);
            break;
        }
        default: {  /* 张大嘴 + 舌头 */
            const int w = s.mouth_w;
            const int h = w * 3 / 4;
            lv_obj_set_size(mouth_open_, w, h);
            lv_obj_set_pos(mouth_open_, kMouthCenterX - w / 2, s.mouth_bottom - h);
            Show(mouth_open_, true);
            const int tw = w * 35 / 100;
            const int th = tw / 2;
            lv_obj_set_size(tongue_, tw, th);
            lv_obj_set_pos(tongue_, kMouthCenterX - tw / 2, s.mouth_bottom - th - 3);
            Show(tongue_, true);
            break;
        }
    }

    ESP_LOGI(TAG, "PET_PATCH: expression -> %s (nose bottom %d)", ExprName(expr_),
             kNoseBottom);
}

void PetFaceView::SetExpression(Expr e) {
    if (e < 0 || e >= kExprCount) {
        e = kNeutral;
    }
    if (e == expr_) {
        return;
    }
    expr_ = e;
    blinking_ = false;
    blink_wait_ms_ = 2000 + static_cast<int32_t>(esp_random() % 3000);
    Apply();
}

bool PetFaceView::SetExpression(const char* name) {
    if (!Known(name)) {
        return false;
    }
    SetExpression(Parse(name));
    return true;
}

/*
 * ratio 1 = 完全睁开，0 = 闭合。
 * 只压高度会像「往下掉」，所以按中心对齐：y 跟着补偿 (base_h - h) / 2。
 * 弧线眼（happy）不参与眨眼 —— 弯着的眼睛本来就是一直在笑。
 */
void PetFaceView::SetEyeOpenRatio(float ratio) {
    const ExprStyle& s = kStyles[expr_];
    if (s.arc_w > 0) {
        return;
    }
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    int h = static_cast<int>(eye_h_ * ratio);
    if (h < 6) {
        h = 6;
    }
    const int dy = (eye_h_ - h) / 2;
    for (int i = 0; i < 2; i++) {
        const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
        lv_obj_set_pos(eye_[i], cx - eye_w_ / 2, eye_y_ + dy);
        lv_obj_set_size(eye_[i], eye_w_, h);
    }
    /* 快闭上时高光先消失，否则会露在眼白/背景上 */
    const bool vis = (ratio > 0.35f) && (s.hl_pct > 0);
    for (int i = 0; i < 2; i++) {
        Show(hl1_[i], vis);
        Show(hl2_[i], vis);
    }
}

void PetFaceView::Start() {
    if (timer_ != nullptr) {
        return;
    }
    timer_ = lv_timer_create(&PetFaceView::TickCb, 30, this);
    blink_wait_ms_ = 2500 + static_cast<int32_t>(esp_random() % 4000);
    gaze_wait_ms_ = 1800 + static_cast<int32_t>(esp_random() % 2500);
    ESP_LOGI(TAG, "PET_PATCH: blink timer started");
}

void PetFaceView::Stop() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void PetFaceView::TickCb(lv_timer_t* t) {
    auto* self = static_cast<PetFaceView*>(lv_timer_get_user_data(t));
    if (self != nullptr) {
        self->Tick();
    }
}

void PetFaceView::Tick() {
    if (root_ == nullptr) {
        return;
    }
    const ExprStyle& s = kStyles[expr_];
    constexpr int kTickMs = 30;

    /* ---------- 眨眼 ---------- */
    if (s.arc_w == 0) {
        if (!blinking_) {
            blink_wait_ms_ -= kTickMs;
            if (blink_wait_ms_ <= 0) {
                blinking_ = true;
                blink_ms_ = 0;
            }
        } else {
            blink_ms_ += kTickMs;
            float ratio = 1.0f;
            if (blink_ms_ < 90) {
                ratio = 1.0f - EaseInOut(blink_ms_ / 90.0f);
            } else if (blink_ms_ < 150) {
                ratio = 0.0f;
            } else if (blink_ms_ < 290) {
                ratio = EaseInOut((blink_ms_ - 150) / 140.0f);
            } else {
                blinking_ = false;
                ratio = 1.0f;
                blink_wait_ms_ = 2600 + static_cast<int32_t>(esp_random() % 5000);
            }
            SetEyeOpenRatio(ratio);
        }
    }

    /* ---------- 眼珠游走 ---------- */
    if (s.hl_pct > 0) {
        gaze_wait_ms_ -= kTickMs;
        if (gaze_wait_ms_ <= 0) {
            gaze_tx_ = static_cast<int>(esp_random() % 13) - 6; /* -6..+6 */
            gaze_ty_ = static_cast<int>(esp_random() % 13) - 6;
            gaze_wait_ms_ = 1600 + static_cast<int32_t>(esp_random() % 3200);
        }
        bool moved = false;
        if (gaze_dx_ != gaze_tx_) {
            gaze_dx_ += (gaze_tx_ > gaze_dx_) ? 1 : -1;
            moved = true;
        }
        if (gaze_dy_ != gaze_ty_) {
            gaze_dy_ += (gaze_ty_ > gaze_dy_) ? 1 : -1;
            moved = true;
        }
        if (moved) {
            const int d1 = kHl1D * s.hl_pct / 100;
            const int d2 = kHl2D * s.hl_pct / 100;
            for (int i = 0; i < 2; i++) {
                const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
                const int cy = eye_y_ + eye_h_ / 2;
                lv_obj_set_pos(hl1_[i], cx + gaze_dx_ + kHl1Dx - d1 / 2,
                               cy + gaze_dy_ + kHl1Dy - d1 / 2);
                lv_obj_set_pos(hl2_[i], cx + gaze_dx_ / 2 + kHl2Dx - d2 / 2,
                               cy + gaze_dy_ / 2 + kHl2Dy - d2 / 2);
            }
        }
    }
}

void PetFaceView::Show(lv_obj_t* obj, bool visible) {
    if (obj == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}
