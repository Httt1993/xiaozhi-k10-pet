#pragma once

#include "lvgl.h"

/*
 * 猫脸「可动」渲染层。
 *
 * 为什么不用位图：
 *   一张 240x320 RGB565 = 150KB，5 种表情就是 750KB，而且位图眼睛动不了。
 *   这里只保留一张「渐变背景 + 腮红 + 鼻子」的底图（pet_face_base），
 *   眼睛和嘴全部用 LVGL 图形对象画 —— 于是眨眼、眼珠游走、换表情都变成
 *   改样式属性，零加载、零闪屏，flash 占用还比原来小。
 *
 * 为什么不用 lv_arc / lv_img / canvas：
 *   只用 lv_obj + 圆角 + 单边边框。给一个全圆角的对象只留 bottom 边，
 *   得到的就是一段 ∪ 形圆弧；只留 top 边就是 ∩ 形。这是最基础的样式能力，
 *   不依赖任何可选 widget，构建风险最低。
 *
 * 图层关系：
 *   root_ 挂在一个 240x320 的透明容器里，容器由 LcdDisplay 放在 screen 上
 *   的 index=1 层（在 UI container 之下、屏幕底图之上），对话文字照样压在脸上。
 */
class PetFaceView {
public:
    enum Expr {
        kNeutral = 0,
        kHappy,
        kSad,
        kSick,
        kHungry,
        kExprCount,
    };

    explicit PetFaceView(lv_obj_t* parent);

    /* 表情名解析：英文名 + 中文别名都认，认不出返回 kNeutral */
    static Expr Parse(const char* name);
    static bool Known(const char* name);
    static const char* ExprName(Expr e);
    /* "neutral / happy / sad / sick / hungry" + 中文说明，给模型看 */
    static const char* NameList();

    void SetExpression(Expr e);
    bool SetExpression(const char* name);
    Expr expression() const { return expr_; }

    void Start();  /* 启动眨眼 / 眼珠定时器 */
    void Stop();

    /* 外层容器，供 LcdDisplay 调整图层顺序 */
    lv_obj_t* root() const { return root_; }

private:
    void Build(lv_obj_t* parent);
    void Apply();
    void Tick();
    static void TickCb(lv_timer_t* t);

    /* ratio: 1 = 完全睁开，0 = 闭合 */
    void SetEyeOpenRatio(float ratio);
    void Show(lv_obj_t* obj, bool visible);

    lv_obj_t* root_ = nullptr;

    /* 实心椭圆眼（neutral / sad / sick / hungry） */
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    /* 弧线眼（happy 的 ∩ 笑眼） */
    lv_obj_t* eye_arc_[2] = {nullptr, nullptr};
    /* 高光：hl1 大、hl2 小 */
    lv_obj_t* hl1_[2] = {nullptr, nullptr};
    lv_obj_t* hl2_[2] = {nullptr, nullptr};
    /* 嘴：omega 的两条 ∪ 弧 */
    lv_obj_t* mouth_omega_[2] = {nullptr, nullptr};
    /* 单弧嘴（笑 / 哭） */
    lv_obj_t* mouth_arc_ = nullptr;
    /* 小圆嘴（生病） */
    lv_obj_t* mouth_small_ = nullptr;
    /* 张大嘴 + 舌头（饿） */
    lv_obj_t* mouth_open_ = nullptr;
    lv_obj_t* tongue_ = nullptr;

    lv_timer_t* timer_ = nullptr;

    Expr expr_ = kNeutral;

    /* 当前眼睛几何，眨眼动画按它算高度 */
    int eye_y_ = 117;
    int eye_w_ = 50;
    int eye_h_ = 58;

    /* 眨眼状态机 */
    bool blinking_ = false;
    uint32_t blink_ms_ = 0;
    int32_t blink_wait_ms_ = 3000;

    /* 眼珠游走 */
    int gaze_dx_ = 0;
    int gaze_dy_ = 0;
    int gaze_tx_ = 0;
    int gaze_ty_ = 0;
    int32_t gaze_wait_ms_ = 2000;
};
