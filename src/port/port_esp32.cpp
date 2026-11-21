/**
 * port_esp32.cpp —— 微雪 ESP32-S3-Touch-AMOLED-2.16 硬件移植层（LVGL 9）
 *
 * 依赖微雪官方 demo 库（放在 vendor/waveshare-demo，见 platformio.ini）：
 *   - pin_config.h           引脚定义
 *   - SensorLib (TouchDrvCSTXXX.hpp)  触摸驱动
 *   - Arduino_GFX_Library    CO5300 QSPI 显示屏驱动
 *
 * 本文件是"硬件到了直接用"的关键：引脚全部取自 pin_config.h，无需改代码。
 * 若你的接线不同，只改 pin_config.h 即可，UI/业务代码零改动。
 */
#include "port.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
/* PMU：官方 demo 同款（XPowersLib 已在 vendor，platformio lib_extra_dirs 挂载）。
 * 对齐 waveshareteam/ESP32-S3-Touch-AMOLED-2.16 的 03_LVGL_AXP2101_ADC_Data：
 * 只做 begin + ADC 测量开启 + 按键中断，不动供电轨（官方也不动，轨默认上电即开） */
#include <XPowersLib.h>

#define PORT_W LCD_WIDTH
#define PORT_H LCD_HEIGHT
#define BUF_LINES 24    /* 24 行×2=46KB：给 TLS 留出 ≥40KB 连续块（32 行时最大块仅 31KB，HTTPS 全灭） */

static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;
static TouchDrvCST92xx  s_touch;
static XPowersPMU       s_pmu;
static int16_t s_tx[2], s_ty[2];

/* ---------- 真机截屏（调试）：flush 时镜像进 PSRAM 全帧 ---------- */
static uint16_t *s_snap = nullptr;       /* 480x480 RGB565 全帧（PSRAM） */
static volatile int s_snap_on = 0;

static uint32_t s_snap_deadline = 0;

extern "C" void port_snap_begin(void)
{
    if (!s_snap)
        s_snap = (uint16_t *)heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
    if (s_snap) {
        memset(s_snap, 0, 480 * 480 * 2);
        s_snap_on = 1;
        /* 强制整屏重绘（LVGL 是局部刷新，否则采不到未变化区域） */
        lv_obj_invalidate(lv_screen_active());
        s_snap_deadline = millis() + 900;
        Serial.println("[snap] 采集中（等 LVGL 重绘完整帧）…");
    }
}

/* 由主循环每轮调用：采集期到了返回 1（此时才 dump，期间 LVGL 照常跑） */
extern "C" int port_snap_tick(void)
{
    if (s_snap_on && s_snap_deadline && (int32_t)(millis() - s_snap_deadline) >= 0)
        return 1;
    return 0;
}

extern "C" void port_snap_dump(void)
{
    if (!s_snap) return;
    Serial.println("SNAPBEGIN 460800");
    {   /* 头 32 字节 hex 文本：分辨设备端数据 vs 传输污染 */
        char hx[96] = "";
        for (int i = 0; i < 32; i++)
            snprintf(hx + i * 2, sizeof(hx) - i * 2, "%02X", ((const uint8_t *)s_snap)[i]);
        Serial.printf("[snap] head32=%s\n", hx);
    }
    Serial.write((const uint8_t *)s_snap, 480 * 480 * 2);
    Serial.println();
    Serial.println("SNAPEND");
    s_snap_on = 0;
}

/* ---------- LVGL 9 显示 flush ---------- */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    if (s_snap_on && s_snap) {
        for (int y = area->y1; y <= area->y2; y++) {
            memcpy(s_snap + y * 480 + area->x1,
                   px_map + (y - area->y1) * w * 2, w * 2);
        }
    }
    if (s_snap_on && s_snap) {
        for (int y = area->y1; y <= area->y2; y++) {
            memcpy(s_snap + y * 480 + area->x1,
                   px_map + (y - area->y1) * w * 2, w * 2);
        }
    }
    s_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(disp);
}

/* ---------- LVGL 9 触摸读取 ---------- */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t last = 0;
    static bool down = false;
    static lv_coord_t lx = 0, ly = 0;
    if (millis() - last >= 15) {
        last = millis();
        if (s_touch.getPoint(s_tx, s_ty, s_touch.getSupportTouchPoint())) {
            down = true; lx = s_tx[0]; ly = s_ty[0];
        } else {
            down = false;
        }
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    (void)indev;
}

/* ---------- LVGL tick（毫秒） ---------- */
static uint32_t tick_cb(void) { return (uint32_t)millis(); }

void board_init(void)
{
    /* ===== 到货自检：每个硬件环节在串口报状态（115200） ===== */
    uint32_t psram = ESP.getPsramSize();
    Serial.printf("[port] PSRAM: %s (%u bytes)\n",
                  psram ? "OK" : "!! 未启用（查 qio_opi / BOARD_HAS_PSRAM）", psram);
    Serial.printf("[port] 内部剩余堆: %u bytes\n", ESP.getFreeHeap());

    /* PMU：AXP2101（供电/电池/电量）。初始化顺序照抄官方示例 03：
     * 不改任何供电轨开关，只开 ADC 测量与电源键中断——屏幕不亮时
     * 这里能立刻分辨"PMU 不在线"还是"供电轨被谁关了" */
    if (s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        s_pmu.clearIrqStatus();
        s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
        s_pmu.enableTemperatureMeasure();
        s_pmu.enableBattDetection();
        s_pmu.enableVbusVoltageMeasure();
        s_pmu.enableBattVoltageMeasure();
        s_pmu.enableSystemVoltageMeasure();
        Serial.printf("[port] AXP2101 OK（电池%s %.0fmV，充电:%s）\n",
                      s_pmu.isBatteryConnect() ? "在位" : "未接",
                      (float)s_pmu.getBattVoltage(),
                      s_pmu.isCharging() ? "是" : "否");
    } else {
        Serial.println("[port] !! PMU 不在线（查 I2C 15/14；屏/触摸供电轨由它管）");
    }

    /* 显示屏：QSPI + CO5300 */
    s_bus = new Arduino_ESP32QSPI(
        LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    s_gfx = new Arduino_CO5300(
        s_bus, LCD_RESET, 0, PORT_W, PORT_H, 0, 0, 0, 0);
    if (!s_gfx->begin()) {
        Serial.println("[port] !! gfx begin FAILED（查 QSPI 接线/屏排线/PMU 供电轨）");
    } else {
        Serial.println("[port] CO5300 OK（480x480 QSPI）");
    }
    s_bus->writeC8D8(0x36, 0xA0);            /* 屏幕方向：若到货方向反，见排障手册 */
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setBrightness(180);

    /* 触摸：CST92xx（I2C） */
    s_touch.setPins(TP_RST, TP_INT);
    if (!s_touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("[port] !! touch offline（查 I2C 15/14、TP_RST 40、PMU 供电轨）");
    } else {
        Serial.printf("[port] CST92xx OK（%d 点，addr=0x5A）\n",
                      s_touch.getSupportTouchPoint());
    }
    s_touch.setMaxCoordinates(PORT_W, PORT_H);
    s_touch.setSwapXY(true);
    s_touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    Serial.println("[port] 硬件自检完成");
}

void lvgl_port_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    /* 显示缓冲（DMA 内部 RAM，双缓冲局部刷新） */
    size_t buf_bytes = PORT_W * BUF_LINES * sizeof(lv_color_t);
    /* 单缓冲：双缓冲要多占 23KB 内部 RAM，网络 TLS 就不够用了（实测最大块仅 7.6KB，
     * HTTPS 请求体都拼不完整）。LVGL 单缓冲同样工作，只是渲染与刷屏不能重叠。 */
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);

    lv_display_t *disp = lv_display_create(PORT_W, PORT_H);
    lv_display_set_flush_cb(disp, disp_flush);
    if (!buf1) {
        Serial.println("[port] 显示缓冲分配失败");
        return;
    }
    lv_display_set_buffers(disp, buf1, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* 触摸 indev */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
}
