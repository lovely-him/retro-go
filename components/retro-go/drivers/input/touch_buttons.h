// 触摸虚拟按键层
//
// 把 RG_GAMEPAD_TOUCH_MAP 描述的矩形画在正式画面之上, 并把落在矩形里的触点翻译成
// RG_KEY_* 位图。芯片层(GT9xx)在 gt911.c, 本文件只管布局、绘制与命中判定。
//
// ── 为什么是"每次显示更新后重画", 而不是画一次 ────────────────────────────────
// DPI 面板没有 GRAM, 帧缓冲由显示驱动持久持有, 而 rg_display 只推送校验和变化的行。
// 所以覆盖在上面的东西会被后续任何盖到该区域的推送冲掉, 冲掉多少取决于游戏画面变了
// 哪几行, 无法预测。
//
// 也正因为如此, "记住按钮下面的像素、隐藏时放回去"这条路走不通: 游戏一重绘, 存下来
// 的那份就成了上一帧的残影。可行的模型只有一个 —— 每帧把整个覆盖层重新合成一遍,
// 局部被冲掉会自愈, 而停止合成即等于擦除。合成时机由 rg_display_set_overlay() 提供。
//
// ── 为什么布局用 viewport 的千分比 ────────────────────────────────────────────
// rg_display_get_info()->viewport 就是 retro-go 算好的"真实画面"矩形, 它随缩放模式
// 和各核心的源宽高比自动变化(GB 的 160x144 会得到一个更窄的 viewport)。按它的千分比
// 描述按键, 同一套布局就能在竖屏、将来的横屏、以及不同宽高比的核心上自适应排布,
// 不必为每种情况各写一份坐标。

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rg_input.h"

// 静态按键表的上限。实际数量由 RG_GAMEPAD_TOUCH_MAP 决定, 超出会被截断并打一条警告。
#define RG_TOUCH_MAX_BUTTONS      16

// 记住按键表并向 rg_display 注册合成回调。
// 刻意不在这里画任何东西: rg_input_init() 跑在 rg_display_init() 之前, 此刻显示驱动
// 还没就绪, 帧缓冲也还不存在。第一次绘制由 rg_display 任务触发。
void rg_touch_buttons_init(const rg_keymap_touch_t *keymap, size_t count);

// 取当前被按下的按键位图(RG_KEY_* 的或)。由 rg_input 在每次轮询时调用, 约 100Hz。
// 返回的是【未去抖】的原始命中结果 —— rg_input 的 input_task 自己会对它做
// DEBOUNCE_PRESS/RELEASE。本模块内部另有一份去抖后的状态, 只用于决定边框颜色。
//
// 自动隐藏: 超过 RG_TOUCH_HIDE_TIMEOUT_S 没有触摸就不再合成, 按钮由应用的下一次重画
// 自然盖掉。隐藏期间的第一次触摸只把按钮唤回来、不触发按键(避免伸手去点屏幕那一下
// 误触), 要等手指全部抬起后才开始命中判定。
uint32_t rg_touch_buttons_read(void);
