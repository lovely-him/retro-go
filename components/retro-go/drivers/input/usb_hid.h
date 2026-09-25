// USB HID boot-protocol 键盘驱动
//
// 为什么不用托管组件 espressif/usb_host_hid: 它是 Apache-2.0, 而 retro-go 是
// GPL-2.0-only(见仓库根目录 COPYING)。Apache-2.0 带有 GPL-2.0 第 7 条不允许的
// 附加限制(专利终止条款等), 二者不兼容, 因此其源码既不能抄入本仓库, 也不宜作为
// 依赖引入。本驱动只依赖 IDF 内置的 components/usb。
//
// 为什么只需要 boot protocol: HID 1.11 规定 boot interface subclass 的键盘在收到
// SET_PROTOCOL(0) 之后, 报告格式被规范**固定**为 8 字节
//     [0] 修饰键位图  [1] 保留(恒 0)  [2..7] 最多 6 个同时按下的 usage ID
// 也就是说完全不需要解析报告描述符。IDF 内置 components/usb 也确实不提供 hid.h /
// hid_usage_keyboard.h(那两个头文件属于托管组件), 所以下面的常量在此自定义。
//
// 代价是: 不支持没有 boot subclass 的键盘, 也不支持手柄(手柄的报告布局由厂商的
// 报告描述符决定, 必须解析)。手柄留待后续, 可参考 IDF 内置 esp_hid 组件的
// esp_hid_common.c 做描述符解析。

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- USB HID 1.11 类/子类/协议 ----
#define HID_CLASS               0x03
#define HID_SUBCLASS_BOOT       0x01
#define HID_PROTOCOL_KEYBOARD   0x01

// ---- HID 类请求(bmRequestType = Host-to-device | Class | Interface) ----
#define HID_BMREQ_OUT_CLASS_INTF  0x21
#define HID_REQ_SET_IDLE          0x0A
#define HID_REQ_SET_PROTOCOL      0x0B
#define HID_PROTOCOL_BOOT         0x00

// ---- boot 键盘报告字节 0: 修饰键位图 ----
#define HID_MOD_LCTRL           0x01
#define HID_MOD_LSHIFT          0x02
#define HID_MOD_LALT            0x04
#define HID_MOD_LGUI            0x08
#define HID_MOD_RCTRL           0x10
#define HID_MOD_RSHIFT          0x20
#define HID_MOD_RALT            0x40
#define HID_MOD_RGUI            0x80

// ---- HID Usage Tables, Keyboard/Keypad 页(0x07) ----
// 只列出本 target 用到的键; 需要更多键时按 HID Usage Tables 12.1 节补即可
// (字母 a..z 连续排布于 0x04..0x1D, 数字 1..9,0 于 0x1E..0x27)。按 usage 值排序。
#define HID_KEY_A               0x04
#define HID_KEY_Q               0x14
#define HID_KEY_S               0x16
#define HID_KEY_W               0x1A
#define HID_KEY_X               0x1B
#define HID_KEY_Z               0x1D
#define HID_KEY_ENTER           0x28
#define HID_KEY_ESC             0x29
#define HID_KEY_TAB             0x2B
#define HID_KEY_ARROW_RIGHT     0x4F
#define HID_KEY_ARROW_LEFT      0x50
#define HID_KEY_ARROW_DOWN      0x51
#define HID_KEY_ARROW_UP        0x52

// boot 键盘报告里普通键的槽位数(报告字节 2..7)
#define RG_USB_KEY_SLOTS        6

typedef struct
{
    uint8_t modifier;
    uint8_t key[RG_USB_KEY_SLOTS];
} rg_usb_report_t;

// 安装 USB Host Library、注册客户端、启动事件泵任务。
// 非阻塞: 返回时键盘很可能还没插入或还没枚举完, 枚举在后台异步进行。
// 失败时返回 false, 调用方可以照常继续(退化为无输入)。
bool rg_usb_hid_init(void);

// 取最近一次收到的 boot 报告。没有键盘在位时返回 false。
bool rg_usb_hid_read(rg_usb_report_t *out);

// 判断一条 rg_keymap_usb_t 映射是否被 out 命中。
//   modifier 为 0 表示不关心修饰键; usage 为 0 表示"纯修饰键绑定"(如右 Shift)。
//   modifier 非 0 时要求全部指定位都被按下(子集匹配)。
static inline bool rg_usb_hid_key_hit(const rg_usb_report_t *out, uint8_t modifier, uint8_t usage)
{
    if (modifier && (out->modifier & modifier) != modifier)
        return false;
    if (usage == 0)
        return modifier != 0;
    for (int i = 0; i < RG_USB_KEY_SLOTS; ++i)
    {
        if (out->key[i] == usage)
            return true;
    }
    return false;
}
