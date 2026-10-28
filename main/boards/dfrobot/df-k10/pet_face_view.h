#pragma once

#include "lvgl.h"
#include "pet_fx.h"

/*
 * 猫脸「可动」渲染层（活物感版）。
 *
 * 图层关系（自下而上）：
 *   root_ (240x320 透明容器，挂 screen index=1)
 *   ├─ bg            底图：渐变 + 腮红 + 鼻子（pet_face_base）
 *   ├─ face_group_   脸部组：受 Q 弹缩放 / 侧躺倾斜
 *   │  ├─ eyes_group_   眼睛组：呼吸 ±1px 浮动
 *   │  │   (实心椭圆眼 / 弧线眼 / 螺旋晕眩眼 + 高光 + 眉毛)
 *   │  ├─ mouth_group_  嘴组：呼吸 ±1px 浮动
 *   │  │   (ω / 单弧 / 小圆 / 张大嘴 / 平直嘴 / 波浪嘴)
 *   │  └─ blush_[2]     腮红叠层：呼吸改变透明度、情绪加深
 *   └─ fx_           FX 粒子层（爱心/星星/Zzz/怒符/问号/泪滴）
 *
 * 为什么不用位图：一张 240x320 RGB565 = 150KB，5 种表情就是 750KB，
 * 而且位图眼睛动不了。这里只保留一张底图，眼睛和嘴全部用 LVGL 图形对象画 ——
 * 眨眼、换表情、Q 弹都变成改样式属性，零加载、零闪屏。
 *
 * 为什么只用 lv_obj + 圆角 + 单边边框：给一个全圆角的对象只留 bottom 边，
 * 得到的就是一段 ∪ 形圆弧；只留 top 边就是 ∩ 形。最基础的样式能力，
 * 不依赖任何可选 widget，构建风险最低。
 */

class PetFaceView {
public:
    enum Expr {
        kNeutral = 0,
        kHappy,
        kSad,
        kSick,
        kHungry,
        /* 活物感新增 5 态 */
        kAngry,
        kScared,
        kDizzy,
        kCurious,
        kSleepy,
        kExprCount,
    };

    explicit PetFaceView(lv_obj_t* parent);

    /* 表情名解析：英文名 + 中文别名都认，认不出返回 kNeutral */
    static Expr Parse(const char* name);
    static bool Known(const char* name);
    static const char* ExprName(Expr e);
    /* 全部表情名 + 中文说明，给模型看 */
    static const char* NameList();

    /* AI / 工具入口：会向情绪状态机登记「对话态」(P2, 8s 衰减) */
    void SetExpression(Expr e);
    bool SetExpression(const char* name);
    Expr expression() const { return expr_; }

    /* ---- 活物感控制接口（外部事件驱动） ---- */
    /* Q 弹：sx<1 压扁，sy>1 拉长，弹簧回弹（LVGL overshoot 路径） */
    void PlaySquash(float sx, float sy, uint32_t ms);
    /* 侧躺整脸倾斜（度，±15） */
    void SetTilt(float deg);
    /* 强制闭眼一段时间（被摸头时） */
    void PetHead(uint32_t ms);
    /* 咀嚼动画（被喂食时） */
    void PlayChew();
    /* 手动投放 FX */
    void SpawnFx(PetFxKind kind, int x, int y);

    /* 更新属性能量条（上 2 下 2：饱食/心情/精力/金币），值 0~100（coins 不限） */
    void SetStats(int hunger, int mood, int energy, int coins);

    void Start();  /* 启动眨眼 / 呼吸 / 体感定时器 */
    void Stop();

    /* 外层容器，供 LcdDisplay 调整图层顺序 */
    lv_obj_t* root() const { return root_; }

private:
    void Build(lv_obj_t* parent);
    void Apply();
    /* 只换脸、不碰情绪状态机（状态机联动用；AI 入口才登记对话态） */
    void ApplyExpr(Expr e);
    void Tick();
    static void TickCb(lv_timer_t* t);

    /* ratio: 1 = 完全睁开，0 = 闭合（会乘以表情自身的基础开度） */
    void SetEyeOpenRatio(float ratio);
    /* 按「当前开度 + 视线偏移」摆放眼睛与高光（眨眼和视线共用，避免打架） */
    void ApplyEyeLayout();
    void PlaceEyes();
    void PlaceMouth();
    void Show(lv_obj_t* obj, bool visible);
    void OnMoodChanged();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* face_group_ = nullptr;
    lv_obj_t* eyes_group_ = nullptr;
    lv_obj_t* mouth_group_ = nullptr;
    PetFxLayer* fx_ = nullptr;

    /* 实心椭圆眼（neutral / sad / angry / scared / curious / hungry） */
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    /* 弧线眼（happy 的 ∩ 笑眼 / sick·sleepy 的 ∪ 闭眼） */
    lv_obj_t* eye_arc_[2] = {nullptr, nullptr};
    /* 螺旋晕眩眼：每只眼两道同心圆环 */
    lv_obj_t* spiral_outer_[2] = {nullptr, nullptr};
    lv_obj_t* spiral_inner_[2] = {nullptr, nullptr};
    /* 高光：hl1 大、hl2 小 */
    lv_obj_t* hl1_[2] = {nullptr, nullptr};
    lv_obj_t* hl2_[2] = {nullptr, nullptr};
    /* 眉毛（怒眉 / 垂眉 / 困眉） */
    lv_obj_t* brow_[2] = {nullptr, nullptr};
    /* 嘴：omega 的两条 ∪ 弧 */
    lv_obj_t* mouth_omega_[2] = {nullptr, nullptr};
    /* 单弧嘴（笑 ∪ / 哭 ∩） */
    lv_obj_t* mouth_arc_ = nullptr;
    /* 小圆嘴（生病 / 惊吓） */
    lv_obj_t* mouth_small_ = nullptr;
    /* 平直嘴（生气） */
    lv_obj_t* mouth_flat_ = nullptr;
    /* 波浪嘴（晕眩）：一 ∩ 一 ∪ 拼成 ~ */
    lv_obj_t* mouth_wave_[2] = {nullptr, nullptr};
    /* 张大嘴 + 舌头（饿 / 咀嚼） */
    lv_obj_t* mouth_open_ = nullptr;
    lv_obj_t* tongue_ = nullptr;
    /* 腮红叠层（呼吸透明度 + 情绪加深） */
    lv_obj_t* blush_[2] = {nullptr, nullptr};

    /* ── 图标状态机：小鱼干三态（饱食）+ 爱心三态（心情） ─────────────
     * satiety(0-100) → FishIconState；mood(0-100) → HeartIconState。
     * 每态一组 LVGL 图元容器，切换 = 显隐，无需重绘。
     * 饱食 <30（kSkeleton 档）时小鱼干做轻微水平抖动，提示“饿了”。 */
   public:
    enum class FishIconState { kWhole = 0, kBitten = 1, kSkeleton = 2 };
    enum class HeartIconState { kSolid = 0, kHollow = 1, kCracked = 2 };
    static FishIconState FishStateFromSatiety(int satiety) {
        if (satiety >= 60) return FishIconState::kWhole;
        if (satiety >= 30) return FishIconState::kBitten;
        return FishIconState::kSkeleton;
    }
    static HeartIconState HeartStateFromMood(int mood) {
        if (mood >= 60) return HeartIconState::kSolid;
        if (mood >= 30) return HeartIconState::kHollow;
        return HeartIconState::kCracked;
    }
    /** 未完成任务角标：0=隐藏，>0 右上角剪贴板+数字 */
    void SetTaskBadge(int count);

   private:
    void SetFishState(FishIconState st);
    void SetHeartState(HeartIconState st);
    void UpdateHungerShake(int satiety);

    /* 底部一排：小鱼干(饱食) / 爱心(心情) / 电池(精力) / 金币数字 */
    lv_obj_t* stat_fish_grp_ = nullptr;   /* 小鱼干宿主（抖动动画作用对象） */
    lv_obj_t* fish_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* stat_heart_grp_ = nullptr;  /* 爱心宿主 */
    lv_obj_t* heart_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* stat_batt_ = nullptr;       /* 精力：电池填充 */
    lv_obj_t* stat_coins_lbl_ = nullptr;  /* 金币：只显数字 */
    lv_obj_t* stat_tear_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* stat_star_[4] = {nullptr, nullptr, nullptr, nullptr};
    FishIconState fish_state_ = FishIconState::kWhole;
    HeartIconState heart_state_ = HeartIconState::kSolid;
    int fish_base_x_ = 10;                /* 小鱼干锚点 x（抖动回中用） */
    bool fish_shaking_ = false;

    /* 未完成任务角标（屏幕顶部） */
    lv_obj_t* task_badge_ = nullptr;
    lv_obj_t* task_badge_lbl_ = nullptr;

    lv_timer_t* timer_ = nullptr;

    Expr expr_ = kNeutral;

    /* 当前眼睛几何，眨眼动画按它算高度 */
    int eye_y_ = 117;
    int eye_w_ = 50;
    int eye_h_ = 58;
    float base_open_ = 1.0f;   /* 表情基础开度（困 = 0.35） */
    float open_ratio_ = 1.0f;  /* 当前眨眼开度（0~1），视线移动时要按它重摆 */

    /* 眨眼状态机 */
    bool blinking_ = false;
    uint32_t blink_ms_ = 0;
    int32_t blink_wait_ms_ = 3000;
    int blink_count_ = 0;
    bool blink_double_pending_ = false;
    int32_t pet_closed_ms_ = 0;   /* 被摸头强制闭眼剩余时间 */

    /* 视线 = 重力追踪 + 漫游，叠加后作用于眼与高光（不同倍率产生视差） */
    float gaze_dx_ = 0;
    float gaze_dy_ = 0;
    int gaze_tx_ = 0;
    int gaze_ty_ = 0;
    int32_t gaze_wait_ms_ = 2000;

    /* 呼吸 */
    float breath_phase_ = 0;

    /* Q 弹（transform_scale，256 = 1.0） */
    lv_obj_t* squash_obj_ = nullptr;

    /* 侧躺倾斜目标角（度） */
    float tilt_target_ = 0;
    float tilt_now_ = 0;

    /* 咀嚼 */
    int chew_left_ = 0;
    int32_t chew_ms_ = 0;

    /* 困觉 Zzz 节流 */
    int32_t sleep_zz_ms_ = 0;

    /* 上一个情绪（用于切换时触发 FX） */
    int last_mood_ = -1;
};
