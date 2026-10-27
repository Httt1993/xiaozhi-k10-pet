#ifndef _PET_FX_H_
#define _PET_FX_H_

#include "lvgl.h"

/*
 * FX 粒子层：在猫脸周围飘的「情绪符号」。
 *
 *   ♥ 爱心  —— 开心 / 被摸头时从眼睛上方飘起
 *   ✦ 星星  —— 晕眩时绕头顶打转
 *   Z z     —— 困的时候从头顶右上角飘出
 *   💢 怒符 —— 生气时贴在额角（两笔交叉）
 *   ？      —— 好奇时在头顶冒一个
 *   泪滴    —— 难过时从眼角慢慢滑落
 *
 * 实现：对象池（5 个槽位复用，绝不运行时 new 出 LVGL 对象之外的东西）。
 * 每个槽位是一个小 lv_obj，内部按 kind 画不同形状；动画用 lv_anim 平移+淡出。
 * 刻意不用 emoji 字体 —— 系统字体没有这些码位，用几何图形拼：
 *   心 = 两个圆 + 一个旋转方块；星 = 四角星（两条交叉圆角条）；
 *   怒 = 两条交叉短粗线；问号 = 文本标签（问号在 ASCII 里，一定有）。
 */

enum PetFxKind {
    PET_FX_HEART = 0,
    PET_FX_STAR,
    PET_FX_ZZZ,
    PET_FX_ANGER,
    PET_FX_QUESTION,
    PET_FX_TEAR,
};

class PetFxLayer {
public:
    explicit PetFxLayer(lv_obj_t* parent);
    ~PetFxLayer();

    /* 在 (x, y) 处生成一个 FX，kind 决定形状。超出池容量会丢弃最旧的。 */
    void Spawn(PetFxKind kind, int x, int y);

    /* 困了：周期性在右上角冒 Zzz（由 PetFaceView::Tick 调用） */
    void SpawnSleepZ();

    void Clear();

private:
    struct Slot {
        lv_obj_t* root = nullptr;
        lv_obj_t* parts[4] = {nullptr, nullptr, nullptr, nullptr};
        lv_anim_t anim{};
        bool busy = false;
    };

    static void AnimReady(lv_anim_t* a);
    /* LVGL 9.x 的 exec_cb 签名是 void(void* var, int32_t value)，
     * 不是旧版的 void(lv_anim_t*) —— 照旧写会编译失败。 */
    static void AnimX(void* var, int32_t v);
    static void AnimY(void* var, int32_t v);

    void BuildShape(Slot& s, PetFxKind kind, int x, int y);
    void Release(Slot& s);

    lv_obj_t* layer_ = nullptr;
    Slot slots_[5];
    int zzz_flip_ = 0;
};

#endif  /* _PET_FX_H_ */
