#include "rg_system.h"

// 整个文件仅在 target 定义了 RG_GAMEPAD_USB_MAP 时才有内容。这样 drivers/input 可以
// 无条件加入 COMPONENT_SRCDIRS: 其它 target 得到的只是一个空翻译单元, 既不会引用
// components/usb, 也不会因为该组件在 IDF 4.x 上行为不同而构建失败。
#ifdef RG_GAMEPAD_USB_MAP

#ifndef RG_USB_HOST_ENABLE
#error "RG_GAMEPAD_USB_MAP 需要同时定义 RG_USB_HOST_ENABLE(见 drivers/usb/usb_host_svc.c)"
#endif

#include <string.h>

#include <usb/usb_host.h>

#include "usb_host_svc.h"
#include "usb_hid.h"

#define USB_BOOT_REPORT_LEN     8
#define USB_MAX_MPS             64

// 认领设备之后的推进阶段。所有阶段都在服务的事件泵任务里推进, 绝不阻塞等待:
// USB 传输的完成回调是从 usb_host_client_handle_events() 里派发的, 也就是从那个
// 任务里。在自己的回调里等自己的下一个回调必然死锁, 所以控制传输做成链式异步,
// 由服务每轮调用一次 poll() 推进一步。
typedef enum
{
    KB_IDLE = 0,        // 没有认领任何设备
    KB_SET_PROTOCOL,    // SET_PROTOCOL(boot) 在途
    KB_SET_IDLE,        // SET_IDLE(0,0) 在途
    KB_START,           // 待提交中断 IN
    KB_READY,           // 正常收报告
    KB_FAILED,          // 出错; poll() 返回 false, 服务随后转入 detach
} kb_state_t;

static kb_state_t kb_state = KB_IDLE;
static usb_device_handle_t kb_dev = NULL;
static usb_transfer_t *kb_ctrl_xfer = NULL;
static usb_transfer_t *kb_data_xfer = NULL;
static uint8_t kb_intf_num = 0;
static uint8_t kb_ep_addr = 0;
static uint16_t kb_ep_mps = USB_BOOT_REPORT_LEN;
static bool kb_ctrl_inflight = false;
static bool kb_data_inflight = false;
static bool kb_was_ready = false;
static bool ctrl_done = false;
static usb_transfer_status_t ctrl_status = USB_TRANSFER_STATUS_COMPLETED;
static bool kb_registered = false;

// kb_report 由服务的事件泵任务写、rg_input 任务读; kb_state 同样是跨任务读。
// 8 字节的拷贝不是原子的, 撕裂最多造成一次 10ms 轮询的抖动; 而 rg_input 的去抖要求
// 连续 RG_GAMEPAD_DEBOUNCE_PRESS(默认 2)次采样一致, 所以既不会误触发也不会卡键。
static rg_usb_report_t kb_report;

// ---------------------------------------------------------------------------
// 描述符解析
// ---------------------------------------------------------------------------

// 在配置描述符里找一个 HID 键盘接口及其 interrupt IN 端点。
static bool find_boot_keyboard(const usb_config_desc_t *config_desc, uint8_t *intf_num, bool *boot_capable,
                               uint8_t *ep_addr, uint16_t *mps)
{
    for (int i = 0; i < config_desc->bNumInterfaces; ++i)
    {
        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, i, 0, &offset);
        if (!intf || intf->bInterfaceClass != HID_CLASS || intf->bInterfaceProtocol != HID_PROTOCOL_KEYBOARD)
            continue;

        for (int e = 0; e < intf->bNumEndpoints; ++e)
        {
            int ep_offset = offset;
            const usb_ep_desc_t *ep =
                usb_parse_endpoint_descriptor_by_index(intf, e, config_desc->wTotalLength, &ep_offset);
            if (!ep)
                continue;
            if (!(ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK))
                continue; // 只要 IN
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_INT)
                continue; // 只要 interrupt

            *intf_num = intf->bInterfaceNumber;
            *boot_capable = (intf->bInterfaceSubClass == HID_SUBCLASS_BOOT);
            *ep_addr = ep->bEndpointAddress;
            *mps = ep->wMaxPacketSize & USB_W_MAX_PACKET_SIZE_MPS_MASK;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 传输回调(都在服务的事件泵任务上下文里执行)
// ---------------------------------------------------------------------------

static void ctrl_transfer_cb(usb_transfer_t *xfer)
{
    kb_ctrl_inflight = false;
    ctrl_status = xfer->status;
    ctrl_done = true;
}

static void data_transfer_cb(usb_transfer_t *xfer)
{
    kb_data_inflight = false;

    if (kb_state != KB_READY) // 正在拆除, 不要再提交
        return;

    // 注意 status 必须先判: 设备拔出后 IDF 会以 NO_DEVICE 完成在途 URB
    // (hcd_dwc.c:1820), 那条路径绝不能重新提交, 否则就是往一个已消失的设备上发。
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes >= USB_BOOT_REPORT_LEN)
    {
        // 报告布局 [0]=修饰键 [1]=保留 [2..7]=最多 6 个 usage ID。
        // 逐字段拷贝而非整体 memcpy: rg_usb_report_t 不含保留字节, 只有 7 字节。
        kb_report.modifier = xfer->data_buffer[0];
        memcpy(kb_report.key, &xfer->data_buffer[2], sizeof(kb_report.key));
        xfer->num_bytes = kb_ep_mps;
        if (usb_host_transfer_submit(xfer) == ESP_OK)
            kb_data_inflight = true;
        else
            kb_state = KB_FAILED;
        return;
    }

    // NO_DEVICE / ERROR / STALL / CANCELED
    kb_state = KB_FAILED;
}

// ---------------------------------------------------------------------------
// 阶段推进
// ---------------------------------------------------------------------------

static bool kb_submit_ctrl(uint8_t bRequest, uint16_t wValue)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)kb_ctrl_xfer->data_buffer;
    setup->bmRequestType = HID_BMREQ_OUT_CLASS_INTF;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = kb_intf_num;
    setup->wLength = 0;

    kb_ctrl_xfer->device_handle = kb_dev;
    kb_ctrl_xfer->bEndpointAddress = 0;
    kb_ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t);
    kb_ctrl_xfer->callback = ctrl_transfer_cb;
    kb_ctrl_xfer->context = NULL;

    ctrl_done = false;
    if (usb_host_transfer_submit_control(rg_usb_host_client(), kb_ctrl_xfer) != ESP_OK)
        return false;
    kb_ctrl_inflight = true;
    return true;
}

static void kb_ctrl_step(void)
{
    if (!ctrl_done)
        return; // 请求还在途
    ctrl_done = false;

    if (ctrl_status != USB_TRANSFER_STATUS_COMPLETED)
    {
        RG_LOGW("usb-hid: HID class request failed (status %d), releasing device", (int)ctrl_status);
        kb_state = KB_FAILED;
        return;
    }

    if (kb_state == KB_SET_PROTOCOL)
    {
        kb_state = KB_SET_IDLE;
        // duration=0 表示"仅在状态变化时上报", report_id=0 表示对所有报告生效
        if (!kb_submit_ctrl(HID_REQ_SET_IDLE, 0))
            kb_state = KB_FAILED;
        return;
    }

    kb_state = KB_START; // SET_IDLE 完成
}

static void kb_start(void)
{
    kb_data_xfer->device_handle = kb_dev;
    kb_data_xfer->bEndpointAddress = kb_ep_addr;
    kb_data_xfer->num_bytes = kb_ep_mps;
    kb_data_xfer->callback = data_transfer_cb;
    kb_data_xfer->context = NULL;
    kb_data_xfer->timeout_ms = 0;

    if (usb_host_transfer_submit(kb_data_xfer) != ESP_OK)
    {
        RG_LOGE("usb-hid: interrupt IN submit failed");
        kb_state = KB_FAILED;
        return;
    }
    kb_data_inflight = true;
    kb_state = KB_READY;
    kb_was_ready = true;
    RG_LOGI("usb-hid: keyboard ready");
}

// ---------------------------------------------------------------------------
// rg_usb_class_t 实现
// ---------------------------------------------------------------------------

static bool kb_probe(uint8_t address, usb_device_handle_t dev, void *arg)
{
    const usb_config_desc_t *config_desc = NULL;
    if (usb_host_get_active_config_descriptor(dev, &config_desc) != ESP_OK)
        return false;

    uint8_t intf_num = 0, ep_addr = 0;
    uint16_t mps = 0;
    bool boot_capable = false;
    if (!find_boot_keyboard(config_desc, &intf_num, &boot_capable, &ep_addr, &mps))
    {
        // 首次上电时这条会命中板载 CH334 hub; 它的存在恰好证明 hub 枚举链路是通的。
        RG_LOGI("usb-hid: addr %d has no boot keyboard interface, skipping", address);
        return false;
    }

    if (kb_dev)
    {
        RG_LOGW("usb-hid: addr %d ignored, a keyboard is already active", address);
        return false;
    }

    if (!boot_capable)
    {
        // 报告布局由厂商的报告描述符决定, 本驱动不解析描述符
        RG_LOGW("usb-hid: addr %d is a keyboard without boot subclass, not supported", address);
        return false;
    }

    if (mps < USB_BOOT_REPORT_LEN || mps > USB_MAX_MPS)
    {
        RG_LOGW("usb-hid: addr %d interrupt IN mps %u out of range [%d, %d]", address, (unsigned)mps,
                USB_BOOT_REPORT_LEN, USB_MAX_MPS);
        return false;
    }

    usb_host_client_handle_t client = rg_usb_host_client();

    // 接口一旦被 claim, 该接口的所有端点已由 usb_host_interface_claim() 自动分配
    // (IDF 5.5 起没有 usb_host_endpoint_open(), 提交时按 bEndpointAddress 查找)。
    if (usb_host_interface_claim(client, dev, intf_num, 0) != ESP_OK)
    {
        RG_LOGE("usb-hid: failed to claim interface %d", intf_num);
        return false;
    }

    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &kb_ctrl_xfer) != ESP_OK ||
        usb_host_transfer_alloc(mps, 0, &kb_data_xfer) != ESP_OK)
    {
        RG_LOGE("usb-hid: transfer allocation failed");
        if (kb_ctrl_xfer)
        {
            usb_host_transfer_free(kb_ctrl_xfer);
            kb_ctrl_xfer = NULL;
        }
        if (kb_data_xfer)
        {
            usb_host_transfer_free(kb_data_xfer);
            kb_data_xfer = NULL;
        }
        // 必须在返回 false 之前把接口释放掉: 服务收到 false 会直接 close 设备, 而
        // usb_host_device_close() 要求本客户端持有的接口已全部释放(usb_host.c:1039)。
        // 这里不认领也就不会自动重试, 但两个几十字节的分配失败基本等于系统已经没救,
        // 重新插拔键盘即可再试, 不值得为此加一套重试机制。
        usb_host_interface_release(client, dev, intf_num);
        return false;
    }

    const usb_device_desc_t *dev_desc = NULL;
    usb_host_get_device_descriptor(dev, &dev_desc);

    kb_dev = dev;
    kb_intf_num = intf_num;
    kb_ep_addr = ep_addr;
    kb_ep_mps = mps;

    RG_LOGI("usb-hid: boot keyboard at addr %d (vid:pid %04x:%04x, intf %d, ep 0x%02x, mps %u)", address,
            dev_desc ? dev_desc->idVendor : 0, dev_desc ? dev_desc->idProduct : 0, intf_num, ep_addr, (unsigned)mps);

    // 提交失败也不在这里回滚: 接口已经 claim 了, 交给 poll -> detach 的统一路径清理。
    if (!kb_submit_ctrl(HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT))
    {
        RG_LOGE("usb-hid: SET_PROTOCOL submit failed");
        kb_state = KB_FAILED;
        return true;
    }
    kb_state = KB_SET_PROTOCOL;
    return true;
}

static bool kb_poll(void *arg)
{
    switch (kb_state)
    {
    case KB_SET_PROTOCOL:
    case KB_SET_IDLE: kb_ctrl_step(); break;
    case KB_START: kb_start(); break;
    default: break;
    }
    return kb_state != KB_FAILED;
}

static bool kb_detach(void *arg)
{
    memset(&kb_report, 0, sizeof(kb_report)); // 立刻松开所有按键; 重复调用无害

    // URB 在途时不能释放 transfer 对象。设备拔出后 IDF 会以 NO_DEVICE 完成所有在途
    // URB, 回调从服务的事件泵任务派发, 所以下一轮 detach 必然能等到, 不需要
    // endpoint_halt/flush(那两个的阻塞语义在这个任务里反而是死锁源)。
    if (kb_ctrl_inflight || kb_data_inflight)
        return false;

    if (kb_dev)
    {
        usb_host_interface_release(rg_usb_host_client(), kb_dev, kb_intf_num);
        kb_dev = NULL;
    }
    if (kb_ctrl_xfer)
    {
        usb_host_transfer_free(kb_ctrl_xfer);
        kb_ctrl_xfer = NULL;
    }
    if (kb_data_xfer)
    {
        usb_host_transfer_free(kb_data_xfer);
        kb_data_xfer = NULL;
    }

    ctrl_done = false;
    kb_state = KB_IDLE;

    if (kb_was_ready)
        RG_LOGI("usb-hid: keyboard released");
    kb_was_ready = false;
    return true;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

bool rg_usb_hid_init(void)
{
    static const rg_usb_class_t usb_hid_class = {
        .probe = kb_probe,
        .poll = kb_poll,
        .detach = kb_detach,
    };

    if (kb_registered)
        return true;

    if (!rg_usb_host_init())
        return false;

    if (!rg_usb_host_add_class(&usb_hid_class, NULL))
        return false;

    kb_registered = true;
    RG_LOGI("usb-hid: registered, waiting for a boot-protocol keyboard");
    return true;
}

bool rg_usb_hid_read(rg_usb_report_t *out)
{
    if (kb_state != KB_READY)
        return false;
    memcpy(out, &kb_report, sizeof(*out));
    return true;
}

#endif // RG_GAMEPAD_USB_MAP
