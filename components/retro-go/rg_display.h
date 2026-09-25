#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    RG_DISPLAY_SCALING_OFF = 0, // No scaling, center image on screen
    RG_DISPLAY_SCALING_FIT,     // Scale and preserve aspect ratio
    RG_DISPLAY_SCALING_FULL,    // Scale and stretch to fill screen
    RG_DISPLAY_SCALING_ZOOM,  // Custom zoom and preserve aspect ratio
    RG_DISPLAY_SCALING_COUNT
} display_scaling_t;

typedef enum
{
    RG_DISPLAY_FILTER_OFF = 0,
    RG_DISPLAY_FILTER_HORIZ,
    RG_DISPLAY_FILTER_VERT,
    RG_DISPLAY_FILTER_BOTH,
    RG_DISPLAY_FILTER_COUNT,
} display_filter_t;

typedef enum
{
    RG_DISPLAY_ROTATION_OFF = 0,
    RG_DISPLAY_ROTATION_AUTO,
    RG_DISPLAY_ROTATION_LEFT,
    RG_DISPLAY_ROTATION_RIGHT,
    RG_DISPLAY_ROTATION_COUNT,
} display_rotation_t;

typedef enum
{
    RG_DISPLAY_BACKLIGHT_MIN = 1,
    RG_DISPLAY_BACKLIGHT_MAX = 100,
} display_backlight_t;

enum
{
    RG_DISPLAY_WRITE_NOSYNC = (1 << 0),
    RG_DISPLAY_WRITE_NOSWAP = (1 << 1),
};

typedef struct
{
    display_rotation_t rotation;
    display_scaling_t scaling;
    display_filter_t filter;
    display_backlight_t backlight;
    char *border_file;
    double custom_zoom;
} rg_display_config_t;

typedef struct
{
    int32_t totalFrames;
    int32_t fullFrames;
    int32_t partFrames;
    int64_t blockTime;
    int64_t busyTime;
} rg_display_counters_t;

typedef struct
{
    const char *name;                                               // Driver name
    bool (*init)(void);                                             // Init the display
    bool (*deinit)(void);                                           // Deinit the display
    bool (*sync)(void);                                             // Pause until all data has been flushed
    bool (*set_backlight)(float percent);                           // Set backlight 0.0 - 1.0
    bool (*set_window)(int left, int top, int width, int height);   // Set draw window
    uint16_t *(*get_buffer)(size_t length);                         // Get a DMA-capable buffer to write pixels to and send via send_buffer
    bool (*send_buffer)(uint16_t *buffer, size_t length);           // Send data to display (buffer MUST be acquired via get_buffer)
    // bool (*write)(int left, int top, int width, int height, int pitch, const uint16_t data);
} rg_display_driver_t;

typedef struct
{
    struct
    {
        int real_width, real_height; // Real physical resolution
        int width, height; // Visible resolution (minus margins)
        struct {int left, top, right, bottom;} margins;
        int format;
    } screen;
    struct
    {
        int top, left;
        int width, height;
        float step_x, step_y;
        bool filter_x, filter_y;
    } viewport;
    struct
    {
        int width, height;
    } source;
    bool changed;
} rg_display_t;

#include "rg_surface.h"

void rg_display_init(void);
void rg_display_deinit(void);
void rg_display_write_rect(int left, int top, int width, int height, int stride, const uint16_t *buffer, uint32_t flags);
void rg_display_clear_rect(int left, int top, int width, int height, uint16_t color_le);
void rg_display_clear_except(int left, int top, int width, int height, uint16_t color_le);
void rg_display_clear(uint16_t color_le);
bool rg_display_sync(bool block);
void rg_display_force_redraw(void);
void rg_display_submit(const rg_surface_t *update, uint32_t flags);

// 覆盖层合成回调。在每次显示更新写入之后被调用一次, 用于在正式画面之上叠加需要常驻
// 的内容(例如触摸虚拟按键)。传 NULL 取消注册。
//
// 为什么必须有这个钩子: DPI 面板没有 GRAM, 帧缓冲由显示驱动持久持有, 而 rg_display
// 只推送校验和发生变化的行 —— 也就是说画面是"留在内存里"的, 不存在每帧全量重画。
// 叠加内容若只画一次, 会被后续任何覆盖到该区域的推送冲掉, 而且冲掉多少取决于游戏
// 画面变了哪几行, 无法预测。反过来, 每帧把整个覆盖层重画一遍就自愈了。
//
// 【重要】覆盖层只能往上加像素, 它没有能力擦除自己画过的东西。"停止合成"并不等于
// "擦干净": 那些像素会一直留在帧缓冲里, 直到应用自己重画该区域。游戏里是下一帧
// (16ms), 但 launcher 空闲时约 9 秒才 submit 一次, 画面静止的暂停界面则永远不会重推
// (行校验和没变)。所以覆盖层的两个状态必须画【完全相同的像素集合】, 只改颜色, 这样
// 重画就是全覆盖、不留残余。任何"缩小覆盖范围"的功能(例如超时自动隐藏)都需要一个
// 真正的擦除原语, 不能靠停止合成。
//
// 回调在 rg_display 任务上下文里执行, 不要在里头阻塞。
typedef void (*rg_display_overlay_fn_t)(void);

// 判断一次【同步】写入有没有盖住覆盖层。rg_gui 在没有绑定 surface 时(菜单、对话框)
// 走 rg_display_write_rect 同步路径, 那条路不经过显示任务、不会自动触发合成, 于是会
// 把覆盖层盖掉。但画一个对话框要几十次 write_rect, 每次都全量合成开销不成比例, 所以
// 只在真的相交时才请求一次。传 NULL 表示"任何同步写入都请求"。
// 在调用方任务里执行, 必须非常便宜。
typedef bool (*rg_display_overlay_hit_fn_t)(int left, int top, int width, int height);

void rg_display_set_overlay(rg_display_overlay_fn_t draw, rg_display_overlay_hit_fn_t hits);

// 请求立即重合成一次覆盖层(不重画画面)。用于覆盖层的内容变了、但画面本身没有更新的
// 场合 —— 例如触摸按键抬起: 游戏里下一帧自然会重画, 而 launcher 只在需要时才 submit
// (空闲时约 9 秒一次), 不主动请求的话高亮会一直留在屏幕上。
// 可以在任何任务里调用; 没有注册覆盖层时是空操作。
void rg_display_invalidate_overlay(void);

rg_display_counters_t rg_display_get_counters(void);
const rg_display_t *rg_display_get_info(void);
int rg_display_get_width(void);
int rg_display_get_height(void);

void rg_display_set_scaling(display_scaling_t scaling);
display_scaling_t rg_display_get_scaling(void);
void rg_display_set_filter(display_filter_t filter);
display_filter_t rg_display_get_filter(void);
void rg_display_set_rotation(display_rotation_t rotation);
display_rotation_t rg_display_get_rotation(void);
void rg_display_set_backlight(display_backlight_t percent);
display_backlight_t rg_display_get_backlight(void);
void rg_display_set_border(const char *filename);
char *rg_display_get_border(void);
void rg_display_set_custom_zoom(double factor);
double rg_display_get_custom_zoom(void);
