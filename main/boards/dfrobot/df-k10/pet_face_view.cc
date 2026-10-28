#include "pet_face_view.h"

#include <esp_log.h>
#include <esp_random.h>
#include <strings.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "pet_config.h"
#include "pet_emotion.h"
#include "pet_face.h"
#if PET_MOTION_ENABLE
#include "pet_motion.h"
#endif

#define TAG "PetFaceView"

namespace {

/* ---- 版式基准：全部按 240x320 原图坐标，与 _hw/pet_face/analyze.py 实测一致 ---- */
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

/* 底图鼻子下沿 y≈199，任何嘴都不能高于 200，否则盖住鼻子 */
constexpr int kNoseBottom = 200;

/* 腮红叠层（底图自带腮红中心 53.5/185.5, y 191.5，实测 30x16） */
constexpr int kBlushCxL = 53;
constexpr int kBlushCxR = 186;
constexpr int kBlushCy = 192;

static const lv_color_t kInk = lv_color_hex(0x1A1210);
static const lv_color_t kWhite = lv_color_hex(0xFFFFFF);
static const lv_color_t kTongue = lv_color_hex(0xE8748C);
static const lv_color_t kBlushPink = lv_color_hex(0xFF8FAB);

/* 每个表情一套参数。
 * eye_h=0 表示不用实心椭圆眼；arc_w>0 用弧线眼；spiral>0 用螺旋眼。 */
struct ExprStyle {
    int eye_w, eye_h, eye_y;
    int arc_w;
    int arc_thick;
    int arc_top;
    int arc_down;      /* 0 = ∩ 笑眼；1 = ∪ 闭眼 */
    int hl_pct;
    int mouth_mode;    /* 0=ω 1=单弧 2=小圆 3=张大嘴 4=平直 5=波浪 */
    int mouth_w;
    int mouth_thick;
    int mouth_bottom;
    int brow_mode;     /* 0=无 1=怒眉(下压斜线) 2=垂眉 3=困眉(平) */
    int spiral;        /* 0=无 1=螺旋晕眩眼 */
    float base_open;   /* 基础开度：困=0.35，惊吓=1.15 */
    int blush_pct;     /* 腮红加深：100=常态，125=开心/生气 */
    float breath_s;    /* 呼吸周期秒：4 常态，6 困倦 */
};

/*
 * neutral 大圆眼 + ω 嘴
 * happy   ∩ 笑眼 + 宽 ∪ 笑嘴 + 腮红加深
 * sad     眼略小下移 + ∩ 哭嘴 + 垂眉
 * sick    ∪ 闭眼弧 + 小圆嘴
 * hungry  眼睛更大 + 张大嘴（带舌头）
 * angry   怒眉 + 眼变窄 0.75× + 平直嘴 + 腮红加深
 * scared  瞪圆眼 1.15× + 小圆嘴（张成 o）
 * dizzy   螺旋眼 + 波浪嘴
 * curious 眼略大 + 小 ω
 * sleepy  眼半闭（base_open 0.35）+ 小 ω + 困眉 + 呼吸变慢
 *
 * 注意：eye_w/eye_h 就是最终尺寸，base_open 只表达「半闭眼」这类连续开度，
 * 不要两边都改 —— 否则会被缩放两次。
 */
const ExprStyle kStyles[PetFaceView::kExprCount] = {
    /*        eye_w eye_h eye_y | arc_w thk top down | hl | mode m_w m_thk m_bot | brow spiral open blush breath */
    /* neutral */ {50, 58, 117, 0, 0, 0, 0, 100, 0, 0, 0, 214, 0, 0, 1.00f, 100, 4.0f},
    /* happy   */ {0, 0, 117, 40, 9, 132, 0, 0, 1, 40, 6, 214, 0, 0, 1.00f, 125, 3.0f},
    /* sad     */ {46, 50, 122, 0, 0, 0, 0, 70, 1, 40, 5, 222, 2, 0, 1.00f, 100, 5.0f},
    /* sick    */ {0, 0, 117, 44, 8, 118, 1, 0, 2, 22, 3, 224, 3, 0, 1.00f, 90, 6.0f},
    /* hungry  */ {56, 64, 112, 0, 0, 0, 0, 110, 3, 36, 0, 227, 0, 0, 1.00f, 110, 3.5f},
    /* angry   */ {50, 44, 122, 0, 0, 0, 0, 85, 4, 34, 5, 214, 1, 0, 1.00f, 130, 2.5f},
    /* scared  */ {58, 67, 110, 0, 0, 0, 0, 120, 2, 18, 3, 220, 0, 0, 1.00f, 115, 3.0f},
    /* dizzy   */ {0, 0, 117, 0, 0, 0, 0, 0, 5, 36, 4, 214, 0, 1, 1.00f, 105, 3.5f},
    /* curious */ {54, 62, 114, 0, 0, 0, 0, 115, 0, 22, 0, 210, 0, 0, 1.00f, 110, 3.5f},
    /* sleepy  */ {50, 58, 117, 0, 0, 0, 0, 0, 0, 16, 0, 212, 3, 0, 0.35f, 100, 6.0f},
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

    /* 脸部组：Q 弹缩放 / 侧躺倾斜都作用在它身上 */
    face_group_ = lv_obj_create(root_);
    lv_obj_remove_style_all(face_group_);
    lv_obj_set_size(face_group_, kScreenW, kScreenH);
    lv_obj_set_pos(face_group_, 0, 0);
    lv_obj_set_style_bg_opa(face_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_group_, 0, 0);
    lv_obj_set_style_pad_all(face_group_, 0, 0);
    lv_obj_clear_flag(face_group_, static_cast<lv_obj_flag_t>(
                                      LV_OBJ_FLAG_SCROLLABLE |
                                      LV_OBJ_FLAG_CLICKABLE));
    /* Q 弹支点在下巴（屏幕底中点） */
    lv_obj_set_style_transform_pivot_x(face_group_, kMouthCenterX, 0);
    lv_obj_set_style_transform_pivot_y(face_group_, kScreenH, 0);

    eyes_group_ = lv_obj_create(face_group_);
    lv_obj_remove_style_all(eyes_group_);
    lv_obj_set_size(eyes_group_, kScreenW, kScreenH);
    lv_obj_set_pos(eyes_group_, 0, 0);
    lv_obj_set_style_bg_opa(eyes_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eyes_group_, 0, 0);
    lv_obj_clear_flag(eyes_group_, static_cast<lv_obj_flag_t>(
                                      LV_OBJ_FLAG_SCROLLABLE |
                                      LV_OBJ_FLAG_CLICKABLE));

    mouth_group_ = lv_obj_create(face_group_);
    lv_obj_remove_style_all(mouth_group_);
    lv_obj_set_size(mouth_group_, kScreenW, kScreenH);
    lv_obj_set_pos(mouth_group_, 0, 0);
    lv_obj_set_style_bg_opa(mouth_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouth_group_, 0, 0);
    lv_obj_clear_flag(mouth_group_, static_cast<lv_obj_flag_t>(
                                       LV_OBJ_FLAG_SCROLLABLE |
                                       LV_OBJ_FLAG_CLICKABLE));

    for (int i = 0; i < 2; i++) {
        eye_[i] = MakeObj(eyes_group_, true);
        eye_arc_[i] = MakeObj(eyes_group_, false);
        spiral_outer_[i] = MakeObj(eyes_group_, false);
        spiral_inner_[i] = MakeObj(eyes_group_, false);
        hl1_[i] = MakeObj(eyes_group_, true);
        lv_obj_set_style_bg_color(hl1_[i], kWhite, 0);
        hl2_[i] = MakeObj(eyes_group_, true);
        lv_obj_set_style_bg_color(hl2_[i], kWhite, 0);
        brow_[i] = MakeObj(eyes_group_, true);
    }

    mouth_omega_[0] = MakeObj(mouth_group_, false);
    mouth_omega_[1] = MakeObj(mouth_group_, false);
    mouth_arc_ = MakeObj(mouth_group_, false);
    mouth_small_ = MakeObj(mouth_group_, false);
    mouth_flat_ = MakeObj(mouth_group_, true);
    mouth_wave_[0] = MakeObj(mouth_group_, false);
    mouth_wave_[1] = MakeObj(mouth_group_, false);
    mouth_open_ = MakeObj(mouth_group_, true);
    tongue_ = MakeObj(mouth_group_, true);
    lv_obj_set_style_bg_color(tongue_, kTongue, 0);

    /* 腮红叠层：半透明粉圆盖在底图腮红上，呼吸改透明度 */
    for (int i = 0; i < 2; i++) {
        blush_[i] = MakeObj(face_group_, true);
        lv_obj_set_style_bg_color(blush_[i], kBlushPink, 0);
        lv_obj_set_style_bg_opa(blush_[i], LV_OPA_30, 0);
        lv_obj_set_size(blush_[i], 34, 20);
        lv_obj_set_pos(blush_[i], (i == 0 ? kBlushCxL : kBlushCxR) - 17,
                       kBlushCy - 10);
    }

    fx_ = new PetFxLayer(root_);

    /* ---- 属性可视化：底部一排四件宝（无文字，童趣图标） ----
     * 脸占主屏全部，参数缩到底部一排 4 格：
     *   饭碗(米饭高度=饱食) · 爱心(亮度=心情) · 电池(电量=精力) · 金色数字(金币)
     * 不用进度条不用百分比，看「装得多满」就够。<30 挂小泪滴，>=90 冒小星星。 */
    {
        lv_obj_t* bar = lv_obj_create(root_);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 232, 40);
        lv_obj_set_pos(bar, 4, 276);
        lv_obj_set_style_bg_opa(bar, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A1E18), 0);
        lv_obj_set_style_radius(bar, 12, 0);
        lv_obj_clear_flag(bar, static_cast<lv_obj_flag_t>(
                                   LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        auto mk = [&](lv_obj_t* parent, int x, int y, int w, int h,
                      lv_color_t c, int r) {
            lv_obj_t* o = lv_obj_create(parent);
            lv_obj_remove_style_all(o);
            lv_obj_set_size(o, w, h);
            lv_obj_set_pos(o, x, y);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(o, c, 0);
            lv_obj_set_style_radius(o, r, 0);
            lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(
                                     LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            return o;
        };
        const lv_color_t kCream = lv_color_hex(0xFFF6E8);
        const lv_color_t kOrange = lv_color_hex(0xE89B4B);
        const lv_color_t kOrangeL = lv_color_hex(0xFFB74D);
        const lv_color_t kPink = lv_color_hex(0xFF8A9E);
        const lv_color_t kGreen = lv_color_hex(0x81C784);
        const lv_color_t kGold = lv_color_hex(0xFFD54F);
        const lv_color_t kTear = lv_color_hex(0x378ADD);

        /* 格 1 饱食饭碗：碗沿 + 碗身 + 米饭（高度随饱食） */
        mk(bar, 11, 23, 36, 5, kOrangeL, 3);
        mk(bar, 14, 27, 30, 9, kOrange, 5);
        stat_rice_ = mk(bar, 18, 14, 22, 12, kCream, 4);

        /* 格 2 心情爱心：双圆叶 + 圆角底，整组透明度随心情 */
        stat_heart_grp_ = lv_obj_create(bar);
        lv_obj_remove_style_all(stat_heart_grp_);
        lv_obj_set_size(stat_heart_grp_, 30, 24);
        lv_obj_set_pos(stat_heart_grp_, 72, 8);
        lv_obj_set_style_bg_opa(stat_heart_grp_, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(stat_heart_grp_, static_cast<lv_obj_flag_t>(
                                               LV_OBJ_FLAG_SCROLLABLE |
                                               LV_OBJ_FLAG_CLICKABLE));
        mk(stat_heart_grp_, 0, 2, 12, 12, kPink, 6);
        mk(stat_heart_grp_, 11, 2, 12, 12, kPink, 6);
        mk(stat_heart_grp_, 0, 8, 23, 11, kPink, 5);

        /* 格 3 精力电池：壳 + 小帽 + 电量填充 */
        lv_obj_t* shell = mk(bar, 122, 13, 30, 15, kGreen, 4);
        lv_obj_set_style_bg_opa(shell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(shell, 2, 0);
        lv_obj_set_style_border_color(shell, kGreen, 0);
        mk(bar, 152, 17, 4, 7, kGreen, 1);
        stat_batt_ = mk(bar, 125, 16, 24, 9, kGreen, 2);

        /* 格 4 金币：只显数字 */
        stat_coins_lbl_ = lv_label_create(bar);
        lv_label_set_text(stat_coins_lbl_, "0");
        lv_obj_set_style_text_color(stat_coins_lbl_, kGold, 0);
        lv_obj_set_style_text_font(stat_coins_lbl_, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(stat_coins_lbl_, 188, 12);

        /* 小泪滴 / 小星星挂角 */
        for (int i = 0; i < 4; i++) {
            const int cx = 29 + 58 * i;
            stat_tear_[i] = mk(bar, cx + 14, 2, 6, 6, kTear, 3);
            lv_obj_add_flag(stat_tear_[i], LV_OBJ_FLAG_HIDDEN);
            stat_star_[i] = mk(bar, cx + 14, 2, 5, 5, kGold, 2);
            lv_obj_add_flag(stat_star_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    Apply();
    ESP_LOGI(TAG, "PET_PATCH: face view built (living-ui, %d expressions)",
             kExprCount);
}

const char* PetFaceView::ExprName(Expr e) {
    switch (e) {
        case kHappy:    return "happy";
        case kSad:      return "sad";
        case kSick:     return "sick";
        case kHungry:   return "hungry";
        case kAngry:    return "angry";
        case kScared:   return "scared";
        case kDizzy:    return "dizzy";
        case kCurious:  return "curious";
        case kSleepy:   return "sleepy";
        default:        return "neutral";
    }
}

const char* PetFaceView::NameList() {
    return "neutral(普通) / happy(开心) / sad(难过) / sick(生病) / hungry(饿了) / "
           "angry(生气) / scared(惊吓) / dizzy(晕) / curious(好奇) / sleepy(困)";
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
    if (strcasecmp(name, "sick") == 0 || strcasecmp(name, "ill") == 0) {
        return kSick;
    }
    if (strcasecmp(name, "hungry") == 0 || strcasecmp(name, "delicious") == 0) {
        return kHungry;
    }
    if (strcasecmp(name, "angry") == 0 || strcasecmp(name, "mad") == 0) {
        return kAngry;
    }
    if (strcasecmp(name, "scared") == 0 || strcasecmp(name, "afraid") == 0 ||
        strcasecmp(name, "surprised") == 0) {
        return kScared;
    }
    if (strcasecmp(name, "dizzy") == 0) {
        return kDizzy;
    }
    if (strcasecmp(name, "curious") == 0 || strcasecmp(name, "interested") == 0) {
        return kCurious;
    }
    if (strcasecmp(name, "sleepy") == 0 || strcasecmp(name, "tired") == 0 ||
        strcasecmp(name, "sleeping") == 0) {
        return kSleepy;
    }
    if (strcasecmp(name, "neutral") == 0 || strcasecmp(name, "relaxed") == 0) {
        return kNeutral;
    }
    /* 中文别名 */
    if (strstr(name, "开心") || strstr(name, "高兴") || strstr(name, "笑")) {
        return kHappy;
    }
    if (strstr(name, "难过") || strstr(name, "伤心") || strstr(name, "委屈") ||
        strstr(name, "哭")) {
        return kSad;
    }
    if (strstr(name, "病") || strstr(name, "不舒服")) {
        return kSick;
    }
    if (strstr(name, "饿")) {
        return kHungry;
    }
    if (strstr(name, "生气") || strstr(name, "愤怒") || strstr(name, "发火")) {
        return kAngry;
    }
    if (strstr(name, "吓") || strstr(name, "惊") || strstr(name, "害怕")) {
        return kScared;
    }
    if (strstr(name, "晕")) {
        return kDizzy;
    }
    if (strstr(name, "好奇") || strstr(name, "疑问")) {
        return kCurious;
    }
    if (strstr(name, "困") || strstr(name, "睡")) {
        return kSleepy;
    }
    return kNeutral;
}

bool PetFaceView::Known(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    static const char* const kKeys[] = {
        "neutral", "happy", "sad", "sick", "hungry", "angry", "scared",
        "dizzy", "curious", "sleepy", "laughing", "crying", "ill",
        "mad", "afraid", "surprised", "interested", "tired", "sleeping",
        "relaxed", "delicious", "开心", "高兴", "笑", "难过", "伤心",
        "委屈", "哭", "病", "不舒服", "饿", "生气", "愤怒", "发火",
        "吓", "惊", "害怕", "晕", "好奇", "疑问", "困", "睡"};
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

void PetFaceView::PlaceEyes() {
    const ExprStyle& s = kStyles[expr_];
    for (int i = 0; i < 2; i++) {
        const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
        if (s.spiral) {
            /* 螺旋晕眩眼：两道同心圆环 */
            Show(eye_[i], false);
            Show(eye_arc_[i], false);
            MakeArc(spiral_outer_[i], 30, 30, 3, LV_BORDER_SIDE_FULL, kInk);
            lv_obj_set_pos(spiral_outer_[i], cx - 15, eye_y_ + 29 - 15);
            MakeArc(spiral_inner_[i], 12, 12, 2, LV_BORDER_SIDE_FULL, kInk);
            lv_obj_set_pos(spiral_inner_[i], cx - 6, eye_y_ + 29 - 6);
            Show(spiral_outer_[i], true);
            Show(spiral_inner_[i], true);
        } else {
            Show(spiral_outer_[i], false);
            Show(spiral_inner_[i], false);
            if (s.arc_w > 0) {
                Show(eye_[i], false);
                MakeArc(eye_arc_[i], s.arc_w, s.arc_w, s.arc_thick,
                        s.arc_down ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_TOP,
                        kInk);
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
    }
}

void PetFaceView::PlaceMouth() {
    const ExprStyle& s = kStyles[expr_];
    Show(mouth_omega_[0], false);
    Show(mouth_omega_[1], false);
    Show(mouth_arc_, false);
    Show(mouth_small_, false);
    Show(mouth_flat_, false);
    Show(mouth_wave_[0], false);
    Show(mouth_wave_[1], false);
    Show(mouth_open_, false);
    Show(tongue_, false);

    switch (s.mouth_mode) {
        case 0: {  /* ω：两个 ∪ 环并排，重叠 2px 让接缝融掉 */
            const int ring_w = s.mouth_w > 0 ? s.mouth_w : 24;
            const int ring_h = 26;
            for (int i = 0; i < 2; i++) {
                MakeArc(mouth_omega_[i], ring_w, ring_h, 5,
                        LV_BORDER_SIDE_BOTTOM, kInk);
                lv_obj_set_pos(mouth_omega_[i],
                               kMouthCenterX - ring_w + i * (ring_w - 2),
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
            const int top = cry ? (s.mouth_bottom - w / 2) : (s.mouth_bottom - w);
            lv_obj_set_pos(mouth_arc_, kMouthCenterX - w / 2, top);
            Show(mouth_arc_, true);
            break;
        }
        case 2: {  /* 小圆嘴（生病 o / 惊吓 o） */
            const int w = s.mouth_w;
            MakeArc(mouth_small_, w, w, s.mouth_thick, LV_BORDER_SIDE_FULL, kInk);
            lv_obj_set_pos(mouth_small_, kMouthCenterX - w / 2, s.mouth_bottom - w);
            Show(mouth_small_, true);
            break;
        }
        case 3: {  /* 张大嘴 + 舌头 */
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
        case 4: {  /* 平直嘴（生气）：一条粗横线，两端微微下压 */
            const int w = s.mouth_w;
            lv_obj_set_size(mouth_flat_, w, s.mouth_thick);
            lv_obj_set_style_radius(mouth_flat_, s.mouth_thick / 2, 0);
            lv_obj_set_pos(mouth_flat_, kMouthCenterX - w / 2,
                           s.mouth_bottom - s.mouth_thick);
            Show(mouth_flat_, true);
            break;
        }
        default: { /* 波浪嘴（晕眩）：一 ∩ 一 ∪ 拼成 ~ */
            const int half = s.mouth_w / 2;
            MakeArc(mouth_wave_[0], half, half, s.mouth_thick,
                    LV_BORDER_SIDE_TOP, kInk);
            lv_obj_set_pos(mouth_wave_[0], kMouthCenterX - half,
                           s.mouth_bottom - half / 2);
            MakeArc(mouth_wave_[1], half, half, s.mouth_thick,
                    LV_BORDER_SIDE_BOTTOM, kInk);
            lv_obj_set_pos(mouth_wave_[1], kMouthCenterX, s.mouth_bottom - half);
            Show(mouth_wave_[0], true);
            Show(mouth_wave_[1], true);
            break;
        }
    }
}

void PetFaceView::Apply() {
    if (root_ == nullptr) {
        return;
    }
    const ExprStyle& s = kStyles[expr_];

    eye_w_ = s.eye_w > 0 ? s.eye_w : 50;
    eye_h_ = s.eye_h > 0 ? s.eye_h : 58;
    eye_y_ = s.eye_y;
    base_open_ = s.base_open;

    PlaceEyes();
    SetEyeOpenRatio(1.0f);

    /* ---- 高光 ---- */
    if (s.hl_pct <= 0 || s.spiral) {
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

    /* ---- 眉毛 ---- */
    if (s.brow_mode == 0) {
        Show(brow_[0], false);
        Show(brow_[1], false);
    } else {
        for (int i = 0; i < 2; i++) {
            const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
            lv_obj_set_size(brow_[i], 34, 5);
            lv_obj_set_style_radius(brow_[i], 3, 0);
            /* 怒眉内端下压（向中心倾斜），垂眉内端上抬，困眉放平 */
            int angle = 0;
            if (s.brow_mode == 1) {
                angle = (i == 0) ? 120 : -120;   /* ±12° */
            } else if (s.brow_mode == 2) {
                angle = (i == 0) ? -100 : 100;
            }
            lv_obj_set_style_transform_rotation(brow_[i], angle, 0);
            lv_obj_set_style_transform_pivot_x(brow_[i], 17, 0);
            lv_obj_set_style_transform_pivot_y(brow_[i], 2, 0);
            const int brow_y = (s.brow_mode == 1) ? (eye_y_ - 14) : (eye_y_ - 12);
            lv_obj_set_pos(brow_[i], cx - 17, brow_y);
            Show(brow_[i], true);
        }
    }

    PlaceMouth();

    /* ---- 腮红 ---- */
    const lv_opa_t blush_opa =
        static_cast<lv_opa_t>(LV_OPA_20 + (LV_OPA_40 - LV_OPA_20) * s.blush_pct / 130);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_opa(blush_[i], blush_opa, 0);
    }

    breath_phase_ = 0;
    ESP_LOGI(TAG, "PET_PATCH: expression -> %s", ExprName(expr_));
}

void PetFaceView::SetExpression(Expr e) {
    if (e < 0 || e >= kExprCount) {
        e = kNeutral;
    }
    /* AI / 工具入口：登记「对话态」(P2, 8s 衰减)，本地体感仍可按优先级打断 */
    PetEmotion().Fire(PET_MOOD_TALKING,
                      PetEmotionFsm::DefaultPriority(PET_MOOD_TALKING),
                      8000);
    ApplyExpr(e);
}

void PetFaceView::ApplyExpr(Expr e) {
    if (e < 0 || e >= kExprCount) {
        e = kNeutral;
    }
    if (e == expr_) {
        return;
    }
    expr_ = e;
    blinking_ = false;
    open_ratio_ = 1.0f;
    blink_wait_ms_ = 2000 + static_cast<int32_t>(esp_random() % 3000);
    Apply();

    /* 换表情时顺手放个小 FX，像真的「反应过来」 */
    switch (e) {
        case kHappy:
            SpawnFx(PET_FX_HEART, kEyeCxL, 110);
            PlaySquash(0.92f, 1.06f, 200);
            break;
        case kAngry:
            SpawnFx(PET_FX_ANGER, 120, 100);
            PlaySquash(1.04f, 0.94f, 180);
            break;
        case kScared:
            PlaySquash(0.86f, 1.12f, 160);
            break;
        case kDizzy:
            SpawnFx(PET_FX_STAR, 60, 105);
            SpawnFx(PET_FX_STAR, 180, 105);
            SpawnFx(PET_FX_STAR, 120, 88);
            break;
        case kCurious:
            SpawnFx(PET_FX_QUESTION, 186, 96);
            break;
        case kSad:
            SpawnFx(PET_FX_TEAR, kEyeCxL + 24, 170);
            break;
        default:
            break;
    }
}

bool PetFaceView::SetExpression(const char* name) {
    if (!Known(name)) {
        return false;
    }
    SetExpression(Parse(name));
    return true;
}

void PetFaceView::PlaySquash(float sx, float sy, uint32_t ms) {
    if (face_group_ == nullptr) {
        return;
    }
    /* 两轴各跑一条「压扁 → overshoot 弹回原状」的弹簧。
     * scale 256 = 1.0；early_apply 让第一帧立刻进入压扁态。 */
    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, face_group_);
    lv_anim_set_exec_cb(&ay, [](void* var, int32_t v) {
        lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(var), v, 0);
    });
    lv_anim_set_values(&ay, static_cast<int32_t>(sy * 256.0f), 256);
    lv_anim_set_time(&ay, ms);
    lv_anim_set_early_apply(&ay, true);
    lv_anim_set_path_cb(&ay, lv_anim_path_overshoot);
    lv_anim_start(&ay);

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, face_group_);
    lv_anim_set_exec_cb(&ax, [](void* var, int32_t v) {
        lv_obj_set_style_transform_scale_x(static_cast<lv_obj_t*>(var), v, 0);
    });
    lv_anim_set_values(&ax, static_cast<int32_t>(sx * 256.0f), 256);
    lv_anim_set_time(&ax, ms);
    lv_anim_set_early_apply(&ax, true);
    lv_anim_set_path_cb(&ax, lv_anim_path_overshoot);
    lv_anim_start(&ax);
}

void PetFaceView::SetTilt(float deg) { tilt_target_ = deg; }

void PetFaceView::PetHead(uint32_t ms) {
    pet_closed_ms_ = static_cast<int32_t>(ms);
    SpawnFx(PET_FX_HEART, kEyeCxR, 110);
    PlaySquash(0.95f, 1.04f, 220);
}

void PetFaceView::PlayChew() {
    chew_left_ = 6;
    chew_ms_ = 0;
}

void PetFaceView::SpawnFx(PetFxKind kind, int x, int y) {
    if (fx_ != nullptr) {
        fx_->Spawn(kind, x, y);
    }
}

void PetFaceView::SetStats(int hunger, int mood, int energy, int coins) {
    if (stat_rice_ == nullptr) {
        return;
    }
    if (hunger < 0) hunger = 0;
    if (mood < 0) mood = 0;
    if (energy < 0) energy = 0;
    if (coins < 0) coins = 0;
    if (hunger > 100) hunger = 100;
    if (mood > 100) mood = 100;
    if (energy > 100) energy = 100;

    /* 饱食：米饭堆高 3~15px（越高越饱） */
    const int rh = 3 + (12 * hunger) / 100;
    lv_obj_set_size(stat_rice_, 22, rh);
    lv_obj_set_pos(stat_rice_, 18, 27 - rh);

    /* 心情：爱心亮度（满=亮 / 半=半亮 / 低=暗） */
    const lv_opa_t hopa = (mood >= 70) ? LV_OPA_COVER
                                       : (mood >= 30 ? LV_OPA_75 : LV_OPA_50);
    lv_obj_set_style_opa(stat_heart_grp_, hopa, 0);

    /* 精力：电池电量 2~24px */
    lv_obj_set_width(stat_batt_, 2 + (22 * energy) / 100);

    /* 金币：只显数字 */
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", coins);
    lv_label_set_text(stat_coins_lbl_, buf);

    /* 小泪滴（<30 挂上）/ 小星星（>=90 冒出；金币 >=50） */
    const int vals[4] = {hunger, mood, energy, coins};
    for (int i = 0; i < 4; i++) {
        const bool low = (i < 3) && vals[i] < 30;
        const bool high = (i < 3) ? (vals[i] >= 90) : (vals[i] >= 50);
        if (stat_tear_[i] != nullptr) {
            if (low) {
                lv_obj_remove_flag(stat_tear_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(stat_tear_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (stat_star_[i] != nullptr) {
            if (!low && high) {
                lv_obj_remove_flag(stat_star_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(stat_star_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void PetFaceView::SetEyeOpenRatio(float ratio) {
    const ExprStyle& s = kStyles[expr_];
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.2f) ratio = 1.2f;
    open_ratio_ = ratio;
    if (s.arc_w > 0 || s.spiral) {
        return;
    }
    ApplyEyeLayout();
}

/*
 * 眨眼与视线共用的唯一摆放入口。
 * 眼睛：基座 y 随开度补偿 + 视线 1.0× 平移；
 * 高光：视差反向（大 0.4× / 小 0.6×），快闭眼时先消失。
 */
void PetFaceView::ApplyEyeLayout() {
    const ExprStyle& s = kStyles[expr_];
    if (s.arc_w > 0 || s.spiral) {
        return;
    }
    const float eff = open_ratio_ * base_open_;
    int h = static_cast<int>(eye_h_ * eff);
    if (h < 6) h = 6;
    const int dy = (eye_h_ - h) / 2;
    const int ox = static_cast<int>(gaze_dx_);
    const int oy = static_cast<int>(gaze_dy_);
    for (int i = 0; i < 2; i++) {
        const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
        lv_obj_set_pos(eye_[i], cx - eye_w_ / 2 + ox, eye_y_ + dy + oy);
        lv_obj_set_size(eye_[i], eye_w_, h);
    }
    const bool vis = (open_ratio_ > 0.35f) && (s.hl_pct > 0);
    const int d1 = kHl1D * s.hl_pct / 100;
    const int d2 = kHl2D * s.hl_pct / 100;
    for (int i = 0; i < 2; i++) {
        const int cx = (i == 0) ? kEyeCxL : kEyeCxR;
        const int cy = eye_y_ + eye_h_ / 2;
        /* 高光相对眼中心的基准位置，再叠加视线视差（反向） */
        lv_obj_set_pos(hl1_[i],
                       cx + kHl1Dx - d1 / 2 - static_cast<int>(ox * 0.4f),
                       cy + kHl1Dy - d1 / 2 - static_cast<int>(oy * 0.4f));
        lv_obj_set_pos(hl2_[i],
                       cx + kHl2Dx - d2 / 2 - static_cast<int>(ox * 0.6f),
                       cy + kHl2Dy - d2 / 2 - static_cast<int>(oy * 0.6f));
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
    ESP_LOGI(TAG, "PET_PATCH: living-ui timer started");
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

void PetFaceView::OnMoodChanged() {
    /* 情绪状态机主情绪 → 表情映射（P0~P5 全表）。
     * 必须走 ApplyExpr 而不是 SetExpression —— 后者会反过来向状态机登记
     * 对话态，把状态机刚定下的情绪顶掉，形成回环。 */
    const PetMood m = PetEmotion().current();
    if (static_cast<int>(m) == last_mood_) {
        return;
    }
    last_mood_ = static_cast<int>(m);
    switch (m) {
        case PET_MOOD_HAPPY:    ApplyExpr(kHappy); break;
        case PET_MOOD_ANGRY:    ApplyExpr(kAngry); break;
        case PET_MOOD_SAD:      ApplyExpr(kSad); break;
        case PET_MOOD_SCARED:   ApplyExpr(kScared); break;
        case PET_MOOD_DIZZY:    ApplyExpr(kDizzy); break;
        case PET_MOOD_CURIOUS:  ApplyExpr(kCurious); break;
        case PET_MOOD_SLEEPY:   ApplyExpr(kSleepy); break;
        case PET_MOOD_PLAYING:  ApplyExpr(kHappy); break;
        case PET_MOOD_IDLE:     ApplyExpr(kNeutral); break;
        case PET_MOOD_TALKING:  break;  /* 对话态保持 AI 设的表情 */
        default: break;
    }
}

void PetFaceView::Tick() {
    if (root_ == nullptr) {
        return;
    }
    const ExprStyle& s = kStyles[expr_];
    constexpr int kTickMs = 30;

    /* ---------- 体感事件 → 情绪状态机 ---------- */
#if PET_MOTION_ENABLE
    {
        PetMotionEvent evs[8];
        const int n = PetMotionDrain(evs, 8);
        for (int i = 0; i < n; i++) {
            switch (evs[i]) {
                case PET_MOTION_FALL:
                    PetEmotion().Fire(PET_MOOD_SCARED,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_SCARED),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_SCARED));
                    break;
                case PET_MOTION_SHAKE:
                    PetEmotion().Fire(PET_MOOD_DIZZY,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_DIZZY),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_DIZZY));
                    break;
                case PET_MOTION_FLIP:
                    PetEmotion().Fire(PET_MOOD_ANGRY,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_ANGRY),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_ANGRY));
                    break;
                case PET_MOTION_TAP:
                case PET_MOTION_FEED:
                case PET_MOTION_PET:
                    PetEmotion().Fire(PET_MOOD_HAPPY,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_HAPPY),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_HAPPY));
                    if (evs[i] == PET_MOTION_FEED) PlayChew();
                    if (evs[i] == PET_MOTION_PET) PetHead(1500);
                    if (evs[i] == PET_MOTION_TAP)
                        PlaySquash(0.90f, 1.08f, 180);
                    break;
                case PET_MOTION_GENTLE:
                    /* 轻摇安抚：低落就转开心，否则加深呼吸 */
                    if (PetEmotion().current() == PET_MOOD_SAD) {
                        PetEmotion().Fire(PET_MOOD_HAPPY,
                            PetEmotionFsm::DefaultPriority(PET_MOOD_HAPPY),
                            3000);
                    }
                    break;
                case PET_MOTION_PICKUP:
                    PetEmotion().Fire(PET_MOOD_CURIOUS,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_CURIOUS),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_CURIOUS));
                    break;
                case PET_MOTION_SIDE_LIE:
                case PET_MOTION_STILL:
                    PetEmotion().Fire(PET_MOOD_SLEEPY,
                        PetEmotionFsm::DefaultPriority(PET_MOOD_SLEEPY),
                        PetEmotionFsm::DefaultHoldMs(PET_MOOD_SLEEPY));
                    break;
                case PET_MOTION_SIDE_END:
                case PET_MOTION_UNFLIP:
                case PET_MOTION_WAKE:
                    /* 回落到 idle（用 idle 的优先级 0，但高于 sleepy 的 2？不 ——
                     * 这里用「至少 P3」强制拉回，避免困 12s 压住唤醒） */
                    if (PetEmotion().current() == PET_MOOD_SLEEPY) {
                        PetEmotion().Fire(PET_MOOD_IDLE, 3, 0);
                        PetEmotion().Fire(PET_MOOD_HAPPY, 3, 1200);
                    }
                    break;
                default:
                    break;
            }
        }
    }
#endif

    PetEmotion().Tick(kTickMs);
    OnMoodChanged();

    /* ---------- 呼吸：眼嘴组 ±1px 浮动 + 腮红透明度正弦 ---------- */
    breath_phase_ += (2.0f * 3.1415926f * kTickMs) / (s.breath_s * 1000.0f);
    if (breath_phase_ > 2.0f * 3.1415926f) {
        breath_phase_ -= 2.0f * 3.1415926f;
    }
    const float bob = sinf(breath_phase_);  /* -1..1 */
    const int dy = (bob > 0.33f) ? 1 : (bob < -0.33f ? -1 : 0);
    lv_obj_set_y(eyes_group_, dy);
    lv_obj_set_y(mouth_group_, dy);
    const lv_opa_t b0 = LV_OPA_20 + (LV_OPA_40 - LV_OPA_20) * s.blush_pct / 130;
    const lv_opa_t blush_opa = static_cast<lv_opa_t>(
        b0 * (0.82f + 0.18f * (0.5f + 0.5f * bob)));
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_opa(blush_[i], blush_opa, 0);
    }

    /* ---------- 侧躺倾斜：目标角低通逼近 ---------- */
    tilt_now_ += (tilt_target_ - tilt_now_) * 0.08f;
    if (fabsf(tilt_now_) > 0.3f) {
        lv_obj_set_style_transform_rotation(
            face_group_,
            static_cast<int32_t>(tilt_now_ * 10.0f), 0);
    } else {
        lv_obj_set_style_transform_rotation(face_group_, 0, 0);
    }

    /* ---------- 被摸头强制闭眼 ---------- */
    if (pet_closed_ms_ > 0) {
        pet_closed_ms_ -= kTickMs;
        SetEyeOpenRatio(0.0f);
    } else {
        /* ---------- 眨眼（每 5 次来一次双眨） ---------- */
        if (s.arc_w == 0 && !s.spiral) {
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
                    blink_count_++;
                    if (blink_count_ % 5 == 0 && !blink_double_pending_) {
                        /* 双眨：紧接着再来一次 */
                        blink_double_pending_ = true;
                        blink_wait_ms_ = 120;
                    } else {
                        blink_double_pending_ = false;
                        /* 困的时候眨眼更慢 */
                        const int base = (expr_ == kSleepy) ? 5200 : 2600;
                        const int span = (expr_ == kSleepy) ? 6000 : 5000;
                        blink_wait_ms_ =
                            base + static_cast<int32_t>(esp_random() % span);
                    }
                }
                SetEyeOpenRatio(ratio);
            }
        }
    }

    /* ---------- 视线 = 重力追踪 + 随机漫游（视差在 ApplyEyeLayout 里） ---------- */
#if PET_MOTION_ENABLE
    float gdx = 0, gdy = 0;
    PetMotionGetGaze(&gdx, &gdy);
    /* 好奇时视线增益 2.5×，困时减弱 */
    float gain = 1.0f;
    if (expr_ == kCurious) gain = 2.5f;
    if (expr_ == kSleepy) gain = 0.3f;
    gaze_dx_ += (gdx * gain - gaze_dx_) * 0.12f;
    gaze_dy_ += (gdy * gain - gaze_dy_) * 0.12f;
#endif
    if (s.hl_pct > 0 && !s.spiral) {
        gaze_wait_ms_ -= kTickMs;
        if (gaze_wait_ms_ <= 0) {
            /* 漫游目标偏向屏幕下方偏左（设计稿：更有「在看」的感觉） */
            gaze_tx_ = static_cast<int>(esp_random() % 11) - 7;
            gaze_ty_ = static_cast<int>(esp_random() % 11) - 3;
            gaze_wait_ms_ = 2000 + static_cast<int32_t>(esp_random() % 2000);
        }
        /* 漫游目标缓慢并入视线（treated as extra offset） */
        const float target_x = gaze_dx_ + static_cast<float>(gaze_tx_) * 0.3f;
        const float target_y = gaze_dy_ + static_cast<float>(gaze_ty_) * 0.3f;
        gaze_dx_ += (target_x - gaze_dx_) * 0.05f;
        gaze_dy_ += (target_y - gaze_dy_) * 0.05f;
        ApplyEyeLayout();
    }

    /* ---------- 咀嚼：嘴型在 ω 与张开间切换 ---------- */
    if (chew_left_ > 0) {
        chew_ms_ += kTickMs;
        if (chew_ms_ > 160) {
            chew_ms_ = 0;
            chew_left_--;
            const bool open = (chew_left_ % 2 == 0);
            if (open && mouth_open_ != nullptr) {
                PlaceMouth();
                Show(mouth_omega_[0], false);
                Show(mouth_omega_[1], false);
                lv_obj_set_size(mouth_open_, 28, 18);
                lv_obj_set_pos(mouth_open_, kMouthCenterX - 14, 206);
                Show(mouth_open_, true);
            } else {
                PlaceMouth();
            }
        }
    }

    /* ---------- 困觉 Zzz ---------- */
    if (expr_ == kSleepy) {
        sleep_zz_ms_ -= kTickMs;
        if (sleep_zz_ms_ <= 0) {
            sleep_zz_ms_ = 2500 + static_cast<int32_t>(esp_random() % 1500);
            if (fx_ != nullptr) {
                fx_->SpawnSleepZ();
            }
        }
    }
}
