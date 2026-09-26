// Target definition
#define RG_TARGET_NAME             "ESP32-P4-WIFI6-DEVKIT"

// Battery
#define RG_BATTERY_DRIVER           0   // 0 = Disable, 1 = ADC, 2 = I2C

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_0
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
#define RG_STORAGE_SDMMC_WIDTH      4
#define RG_STORAGE_SD_PWR_LDO_CHAN  4
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_43
#define RG_GPIO_SDSPI_CMD           GPIO_NUM_44
#define RG_GPIO_SDSPI_D0            GPIO_NUM_39
#define RG_GPIO_SDSPI_D1            GPIO_NUM_40
#define RG_GPIO_SDSPI_D2            GPIO_NUM_41
#define RG_GPIO_SDSPI_D3            GPIO_NUM_42

// Audio
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable, 1 = Enable

// Video
#define RG_SCREEN_DRIVER            3   // 0 = ILI9341/ST7789, 3 = my-driver, 99 = sdl2
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240

// Input
#define RG_GAMEPAD_USB_MAP                          \
    {                                               \
        {RG_KEY_UP, 0, HID_KEY_ARROW_UP},           \
        {RG_KEY_DOWN, 0, HID_KEY_ARROW_DOWN},       \
        {RG_KEY_LEFT, 0, HID_KEY_ARROW_LEFT},       \
        {RG_KEY_RIGHT, 0, HID_KEY_ARROW_RIGHT},     \
        {RG_KEY_A, 0, HID_KEY_Z},                   \
        {RG_KEY_B, 0, HID_KEY_X},                   \
        {RG_KEY_X, 0, HID_KEY_S},                   \
        {RG_KEY_Y, 0, HID_KEY_A},                   \
        {RG_KEY_L, 0, HID_KEY_Q},                   \
        {RG_KEY_R, 0, HID_KEY_W},                   \
        {RG_KEY_START, 0, HID_KEY_ENTER},           \
        {RG_KEY_SELECT, HID_MOD_RSHIFT, 0},         \
        {RG_KEY_MENU, 0, HID_KEY_ESC},              \
        {RG_KEY_OPTION, 0, HID_KEY_TAB},            \
    }
#define RG_GAMEPAD_TOUCH_MAP                                            \
    {                                                                   \
        /* 顶部肩键与菜单键, 扁条 */                                     \
        {RG_KEY_L,        8,   6, 110, 55},  /* (  2,  1) 35x13 */      \
        {RG_KEY_MENU,   330,   6, 140, 55},  /* (105,  1) 44x13 */      \
        {RG_KEY_OPTION, 530,   6, 140, 55},  /* (169,  1) 44x13 */      \
        {RG_KEY_R,      882,   6, 110, 55},  /* (282,  1) 35x13 */      \
        /* 十字键: 单元格 100x140 千分比 = 32x33 逻辑像素 */              \
        {RG_KEY_UP,     115, 545, 100, 140},  /* ( 36,130) 32x33 */     \
        {RG_KEY_LEFT,    15, 690, 100, 140},  /* (  4,165) 32x33 */     \
        {RG_KEY_RIGHT,  215, 690, 100, 140},  /* ( 68,165) 32x33 */     \
        {RG_KEY_DOWN,   115, 835, 100, 140},  /* ( 36,200) 32x33 */     \
        /* 面键菱形: 单元格 100x130 千分比 = 32x31 逻辑像素 */            \
        {RG_KEY_X,      785, 590, 100, 130},  /* (251,141) 32x31 */     \
        {RG_KEY_Y,      680, 725, 100, 130},  /* (217,174) 32x31 */     \
        {RG_KEY_A,      890, 725, 100, 130},  /* (284,174) 32x31 */     \
        {RG_KEY_B,      785, 860, 100, 130},  /* (251,206) 32x31 */     \
        /* 底部中央 */                                                   \
        {RG_KEY_SELECT, 370, 930, 110,  62},  /* (118,223) 35x14 */     \
        {RG_KEY_START,  520, 930, 110,  62},  /* (166,223) 35x14 */     \
    }
