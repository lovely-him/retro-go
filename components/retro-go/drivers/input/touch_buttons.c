#include "rg_system.h"

// 整个文件仅在 target 定义 RG_GAMEPAD_TOUCH_MAP 时参与编译, 与 drivers/input 下其它
// 驱动的门控方式一致。没定义该宏的 target 得到的是一个空翻译单元。
#ifdef RG_GAMEPAD_TOUCH_MAP

// 触摸后端目前只有 GT9xx 一种。分开这两个宏是为了让芯片驱动和按键层各自可替换,
// 但少了后端就没有触点来源, 链接期才会暴露, 不如在这里直接说清楚。
#ifndef RG_TOUCH_DRIVER_GT911
#error "RG_GAMEPAD_TOUCH_MAP 需要同时定义 RG_TOUCH_DRIVER_GT911(触摸后端)"
#endif

// 面板 -> 逻辑坐标的换算需要 target 提供偏移与倍数。它们由 config.h 给出, 并被
// mipi_dsi.h 用 #if 交叉校验过与显示驱动实际的定位规则一致, 所以两处不会漂移。
#if !defined(RG_DSI_OFFSET_X) || !defined(RG_DSI_OFFSET_Y) || !defined(RG_DSI_SCALE_X) || !defined(RG_DSI_SCALE_Y)
#error "RG_GAMEPAD_TOUCH_MAP 需要 target 在 config.h 里提供 RG_DSI_OFFSET_X/Y 与 RG_DSI_SCALE_X/Y"
#endif

#include "touch_buttons.h"
#include "gt911.h"

// 描边粗细(逻辑像素)。2 逻辑像素 = 4 面板像素, 在 10.1 寸屏上看得见又不突兀。
#define BTN_BORDER          2

// 颜色直接传 rg_gui.h 的 C_* 常量。rg_display_clear_rect 与 dsi_blit_span 各做一次
// 字节交换, 两次相消, 常量会原样落进帧缓冲, 与 rg_gui 画出来的颜色一致。
// 两个状态都只是【边框】颜色(理由见 overlay_compose): 空闲用暗灰, 叠在画面上不抢戏;
// 按下用亮白, 在任何背景上都足够明显。
#define BTN_COLOR_IDLE      C_DIM_GRAY
#define BTN_COLOR_DOWN      C_WHITE

// 布局坐标的单位是 viewport 的千分比, 理由见 touch_buttons.h 顶部。
#define BTN_PERMILLE        1000

// 绘制状态的去抖次数。命中判定用的是每次轮询的原始结果, 而手指按住不动时坐标会有
// 1~2 面板像素的抖动(3a 实测: (597,740) 与 (597,739)), 按钮又只有 32x33 逻辑像素,
// 所以手指停在边框附近时原始状态会逐次翻转。模拟器那边有 rg_input 的 DEBOUNCE 保护,
// 绘制这边没有 —— 表现就是按住时白框/灰框来回跳。100Hz 轮询下 3 次 = 30ms, 看不出来。
#define BTN_DEBOUNCE        3

// 自动隐藏超时。隐藏就是停止合成, 见 config.h 里 RG_TOUCH_HIDE_TIMEOUT_S 的说明。
#define BTN_HIDE_TIMEOUT_US ((int64_t)RG_TOUCH_HIDE_TIMEOUT_S * 1000000)

typedef struct
{
    rg_key_t key;
    int left, top, width, height; // 逻辑像素, viewport 变化时重算
} button_t;

static const rg_keymap_touch_t *keymap;
static size_t button_count;
static button_t buttons[RG_TOUCH_MAX_BUTTONS];

// rg_input 任务写(约 100Hz), rg_display 任务读(每次显示更新)。单个 uint32_t 的读写
// 在 RISC-V 上是一条指令, 不会撕裂; 最坏情况是某一帧的高亮慢一拍, 属于观感问题。
static uint32_t pressed_state;

// 自动隐藏状态。visible 初值为 true: 开机时先让按钮露个面, 5 秒没人碰再隐去。
// last_touch_time 初值 0, 而 rg_system_timer() 是"自启动起的微秒数", 所以开机后
// 第一次轮询算出的差值就是已开机时长, 不会误判成刚刚触摸过。
static bool overlay_visible = true;
static bool wake_pending = false;
static int64_t last_touch_time = 0;

// 绘制状态的去抖。返回值保持原始(不去抖): rg_input 的 input_task 已经对
// gamepad_state 做了 DEBOUNCE_PRESS/RELEASE, 这里再叠一层会把输入延迟从 20ms 抬到
// 50ms, 而绘制慢 30ms 是完全无感的。
static uint32_t draw_state;
static int draw_stable;

// 缓存的 viewport, 用来判断布局要不要重算。初值取 -1 保证第一次一定重算。
static int cached_left = -1, cached_top = -1, cached_width = -1, cached_height = -1;

// 面板坐标 -> 逻辑坐标。换算关系就是显示驱动的定位规则: 内容在面板内偏移
// RG_DSI_OFFSET_X/Y, 再按 RG_DSI_SCALE_X/Y 整数倍放大。
// 返回 false 表示触点落在内容区之外 —— 竖屏下是左右各 80 面板像素的黑边, 以及内容
// 下方那一大片。那里没有按键, 直接丢掉。
static bool panel_to_logical(int px, int py, int *lx, int *ly)
{
    // 先判下界再除: C 的整数除法向零取整, 负的 (px - OFFSET_X) 会算出错误的正坐标
    if (px < RG_DSI_OFFSET_X || py < RG_DSI_OFFSET_Y)
        return false;

    const int x = (px - RG_DSI_OFFSET_X) / RG_DSI_SCALE_X;
    const int y = (py - RG_DSI_OFFSET_Y) / RG_DSI_SCALE_Y;
    if (x >= RG_SCREEN_WIDTH || y >= RG_SCREEN_HEIGHT)
        return false;

    *lx = x;
    *ly = y;
    return true;
}

// viewport 变了才重算布局。读取的是 rg_display 任务拥有的结构, 理论上可能读到改到
// 一半的值; 但那样缓存就和真值不一致, 下一轮(10ms 后)必然再算一次, 自愈。
static void update_layout(void)
{
    const rg_display_t *disp = rg_display_get_info();
    const int left = disp->viewport.left, top = disp->viewport.top;
    const int width = disp->viewport.width, height = disp->viewport.height;

    if (left == cached_left && top == cached_top && width == cached_width && height == cached_height)
        return;

    cached_left = left, cached_top = top, cached_width = width, cached_height = height;

    for (size_t i = 0; i < button_count; ++i)
    {
        const rg_keymap_touch_t *m = &keymap[i];
        button_t *b = &buttons[i];
        b->key = m->key;
        b->left = left + (width * m->x) / BTN_PERMILLE;
        b->top = top + (height * m->y) / BTN_PERMILLE;
        b->width = (width * m->w) / BTN_PERMILLE;
        b->height = (height * m->h) / BTN_PERMILLE;
        // 千分比取整后可能是 0, 那样既画不出来也点不中
        if (b->width < 1)
            b->width = 1;
        if (b->height < 1)
            b->height = 1;
    }
}

static void draw_outline(const button_t *b, uint16_t color)
{
    // 太小的按钮画不出空心(边宽会把内部吃光), 退化成实心
    if (b->width <= BTN_BORDER * 2 || b->height <= BTN_BORDER * 2)
    {
        rg_display_clear_rect(b->left, b->top, b->width, b->height, color);
        return;
    }

    const int inner = b->height - BTN_BORDER * 2;
    rg_display_clear_rect(b->left, b->top, b->width, BTN_BORDER, color);                                // 上
    rg_display_clear_rect(b->left, b->top + b->height - BTN_BORDER, b->width, BTN_BORDER, color);       // 下
    rg_display_clear_rect(b->left, b->top + BTN_BORDER, BTN_BORDER, inner, color);                      // 左
    rg_display_clear_rect(b->left + b->width - BTN_BORDER, b->top + BTN_BORDER, BTN_BORDER, inner, color); // 右
}

// rg_display 任务在每次显示更新写入之后调用。见 touch_buttons.h 顶部的说明: 覆盖层
// 必须每帧整体重画, 否则会被游戏画面的增量推送冲成碎片。
static void overlay_compose(void)
{
    // 隐藏时什么都不画。已经画上去的那些边框会在应用下一次重画该区域时被盖掉 ——
    // 游戏里是下一帧, launcher 里是下一次交互。这里不需要也没有能力主动擦除。
    if (!overlay_visible)
        return;

    const uint32_t state = pressed_state;

    for (size_t i = 0; i < button_count; ++i)
    {
        const button_t *b = &buttons[i];
        // 显示初始化之前 viewport 还是 0, 布局算出来是空的, 此时什么都不画
        if (b->width < 1 || b->height < 1)
            continue;

        // 按下与松开必须画【完全相同的像素集合】, 只改颜色。这不是审美选择而是硬约束:
        // 覆盖层只能往上加像素, 没有能力擦除任何东西。一旦按下时填充了内部, 松开时
        // 只画边框就没人去重画内部, 那块实心会一直留着, 直到应用自己刷新该区域 ——
        // 游戏里是下一帧(16ms), 而 launcher 空闲时约 9 秒才 submit 一次, 肉眼可见。
        draw_outline(b, (state & b->key) ? BTN_COLOR_DOWN : BTN_COLOR_IDLE);
    }
}

// rg_display 用它判断一次【同步】写入有没有真的盖住按钮。同步路径指 rg_gui 在没有
// 绑定 surface 时走的 rg_display_write_rect —— 菜单和对话框都是这么画的, 而画一个
// 对话框要几十次 write_rect(背景按 16 行分块 + 4 条边框 + 每行文字一次), 每次都全量
// 合成的话开销完全不成比例。逐个按钮做矩形相交, 9 个按钮 36 次比较, 比合成便宜几个
// 数量级, 于是只有真正压到按钮的那几次写入才会触发合成。
//
// 注意不能用"所有按钮的总包围盒"来做这个判断: 十字键在左下、MENU 在右上, 包围盒会
// 张到几乎整屏, 任何写入都算相交, 优化就失效了。
static bool overlay_hits(int left, int top, int width, int height)
{
    for (size_t i = 0; i < button_count; ++i)
    {
        const button_t *b = &buttons[i];
        if (left < b->left + b->width && left + width > b->left && top < b->top + b->height &&
            top + height > b->top)
            return true;
    }
    return false;
}

void rg_touch_buttons_init(const rg_keymap_touch_t *map, size_t count)
{
    keymap = map;
    button_count = count;

    if (count > RG_TOUCH_MAX_BUTTONS)
    {
        RG_LOGW("touch-buttons: map has %u buttons but only %d fit, the rest are ignored", (unsigned)count,
                RG_TOUCH_MAX_BUTTONS);
        button_count = RG_TOUCH_MAX_BUTTONS;
    }

    // 只注册回调, 不画: 此刻显示驱动还没就绪(rg_input_init 在 rg_display_init 之前),
    // 帧缓冲都还不存在。第一次合成由 rg_display 任务在首帧更新后触发。
    rg_display_set_overlay(overlay_compose, overlay_hits);

    RG_LOGI("touch-buttons: %u button(s), overlay armed", (unsigned)button_count);
}

uint32_t rg_touch_buttons_read(void)
{
    update_layout();

    rg_touch_point_t points[RG_TOUCH_MAX_POINTS];
    const int count = rg_gt911_read(points, RG_COUNT(points));
    const int64_t now = rg_system_timer();

    if (count > 0)
        last_touch_time = now;

    if (!overlay_visible)
    {
        // 隐藏状态下的第一次触摸只负责把按钮唤回来, 不顺带触发按键 —— 否则伸手去点
        // 屏幕的那一下会误触。开始命中判定要等手指全部抬起之后。
        if (count > 0)
        {
            overlay_visible = true;
            wake_pending = true;
            // launcher 空闲时约 9 秒才 submit 一次, 不主动请求的话按钮要等很久才出现
            rg_display_invalidate_overlay();
        }
        return 0;
    }

    if (wake_pending)
    {
        if (count > 0)
            return 0; // 还按着唤醒那一下, 继续吞掉
        wake_pending = false;
    }

    // 超时隐藏。这里【不需要】任何擦除动作: 停止合成之后, 应用下一次重画该区域时会
    // 自然把按钮盖掉(游戏里约 33ms, launcher 里是下一次交互)。画面完全静止又没有
    // 输入时按钮会留着, 那是已知的观感取舍, 见 config.h 的 RG_TOUCH_HIDE_TIMEOUT_S。
    if (now - last_touch_time > BTN_HIDE_TIMEOUT_US)
    {
        overlay_visible = false;
        pressed_state = 0;
        draw_state = 0;
        draw_stable = 0;
        return 0;
    }

    uint32_t state = 0;
    for (int i = 0; i < count; ++i)
    {
        int lx, ly;
        if (!panel_to_logical(points[i].x, points[i].y, &lx, &ly))
            continue;

        // 无状态命中: 每个活动触点都对所有按钮试一遍。绝不能按数组下标记录"哪根手指
        // 按着哪个键" —— 下标不是 track id: 3a 的实测日志里 id0 抬起后, 剩下的手指
        // 落到了 points[0] 但 id 仍然是 1。
        for (size_t b = 0; b < button_count; ++b)
        {
            const button_t *btn = &buttons[b];
            if (lx >= btn->left && lx < btn->left + btn->width && ly >= btn->top && ly < btn->top + btn->height)
                state |= btn->key;
        }
    }

    // 只对【绘制】状态去抖, 返回值保持原始。原始信号在手指贴着边框抖动时会逐次翻转,
    // 直接拿它驱动高亮就会看到白框/灰框来回跳; 而输入那边 rg_input 的 input_task 已经
    // 有 DEBOUNCE_PRESS/RELEASE, 不需要也不应该在这里再叠一层。
    if (state == draw_state)
    {
        if (draw_stable < BTN_DEBOUNCE)
            ++draw_stable;
    }
    else
    {
        draw_state = state;
        draw_stable = 1;
    }

    if (draw_stable >= BTN_DEBOUNCE && draw_state != pressed_state)
    {
        pressed_state = draw_state;
        // 主动请求一次重合成: 游戏里下一帧自然会重画覆盖层, 但 launcher 只在需要时才
        // submit, 不请求的话高亮变化要等到下一次交互才看得见。
        rg_display_invalidate_overlay();
    }

    return state;
}

#endif // RG_GAMEPAD_TOUCH_MAP
