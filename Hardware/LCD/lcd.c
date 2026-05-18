#include "lcd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

typedef struct {
    int dc_level;
    lcd_trans_done_cb_t done_cb;
    void *user_ctx;
} lcd_spi_trans_ctx_t;

// 声明日志输出标签
static const char *TAG = "LCD_DMA";
// 声明串行外设接口控制句柄
static spi_device_handle_t spi;

// 定义直接内存访问单次传输的最大像素点数量
#define DMA_BUFFER_PIXELS (LCD_WIDTH * 40U)
// 声明直接内存访问数据缓冲区指针
static uint16_t *dma_buffer = NULL;
static spi_transaction_t s_async_trans;
static lcd_spi_trans_ctx_t s_async_ctx;
static volatile bool s_async_trans_in_progress;

// 传输前回调函数用于硬件底层自动切换数据命令引脚电平
/**
 * @brief SPI 传输前回调，用于切换 LCD D/C 引脚。
 *
 * @param t SPI 传输描述符，user 字段保存 D/C 电平。
 * @note user 为 0 表示命令，user 为 1 表示数据。
 */
static void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    const lcd_spi_trans_ctx_t *ctx = (const lcd_spi_trans_ctx_t *)t->user;

    if (ctx != NULL) {
        gpio_set_level(LCD_PIN_DC, ctx->dc_level);
    }
}

/**
 * @brief SPI 传输完成回调，用于通知异步调用方。
 *
 * @param t SPI 传输描述符，user 字段保存传输上下文。
 * @note 该回调运行在 SPI 驱动回调上下文，只做完成通知。
 */
static void lcd_spi_post_transfer_callback(spi_transaction_t *t)
{
    const lcd_spi_trans_ctx_t *ctx = (const lcd_spi_trans_ctx_t *)t->user;

    if (ctx == &s_async_ctx) {
        s_async_trans_in_progress = false;
    }

    if ((ctx != NULL) && (ctx->done_cb != NULL)) {
        ctx->done_cb(ctx->user_ctx);
    }
}

// 向屏幕发送八位控制命令
/**
 * @brief 向 LCD 写入 8 位命令。
 *
 * @param cmd LCD 控制命令。
 */
static void LCD_WR_REG(uint8_t cmd)
{
    spi_transaction_t t;
    lcd_spi_trans_ctx_t ctx = {
        .dc_level = 0,
    };

    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    t.user = &ctx;
    spi_device_polling_transmit(spi, &t);
}

// 向屏幕发送八位配置数据
/**
 * @brief 向 LCD 写入 8 位数据。
 *
 * @param data 待写入的数据字节。
 */
static void LCD_WR_DATA(uint8_t data)
{
    spi_transaction_t t;
    lcd_spi_trans_ctx_t ctx = {
        .dc_level = 1,
    };

    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &data;
    t.user = &ctx;
    spi_device_polling_transmit(spi, &t);
}

// 向屏幕发送十六位色彩数据并自动交换高低字节
/**
 * @brief 向 LCD 写入 16 位 RGB565 数据。
 *
 * @param Data RGB565 数据。
 * @note 当前代码路径未直接调用，保留用于单像素/小块写入扩展。
 */
static void Lcd_WriteData_16Bit(uint16_t Data)
{
    uint16_t swapped = (Data >> 8) | (Data << 8);
    spi_transaction_t t;
    lcd_spi_trans_ctx_t ctx = {
        .dc_level = 1,
    };

    memset(&t, 0, sizeof(t));
    t.length = 16;
    t.tx_buffer = &swapped;
    t.user = &ctx;
    spi_device_polling_transmit(spi, &t);
}

// 设置屏幕即将进行数据写入的矩形窗口坐标
/**
 * @brief 设置 LCD 写入窗口。
 *
 * @param xStar 起始 X 坐标。
 * @param yStar 起始 Y 坐标。
 * @param xEnd 结束 X 坐标。
 * @param yEnd 结束 Y 坐标。
 */
void LCD_SetWindows(uint16_t xStar, uint16_t yStar, uint16_t xEnd, uint16_t yEnd)
{   
    LCD_WR_REG(0x2A);   
    LCD_WR_DATA(xStar >> 8);
    LCD_WR_DATA(0x00FF & xStar);      
    LCD_WR_DATA(xEnd >> 8);
    LCD_WR_DATA(0x00FF & xEnd);

    LCD_WR_REG(0x2B);   
    LCD_WR_DATA(yStar >> 8);
    LCD_WR_DATA(0x00FF & yStar);      
    LCD_WR_DATA(yEnd >> 8);
    LCD_WR_DATA(0x00FF & yEnd);

    LCD_WR_REG(0x2C);
}

// 采用直接内存访问机制对指定矩形区域进行纯色填充
/**
 * @brief 使用内部 DMA 缓冲区填充矩形区域。
 *
 * @param sx 起始 X 坐标。
 * @param sy 起始 Y 坐标。
 * @param ex 结束 X 坐标。
 * @param ey 结束 Y 坐标。
 * @param color RGB565 颜色值。
 * @note 依赖 LCD_Init() 分配的 dma_buffer。
 */
void LCD_Fill_DMA(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    if (dma_buffer == NULL) {
        ESP_LOGE(TAG, "DMA buffer is not initialized");
        return;
    }

    uint16_t width = ex - sx + 1;
    uint16_t height = ey - sy + 1;
    uint32_t total_pixels = width * height;
    lcd_spi_trans_ctx_t ctx = {
        .dc_level = 1,
    };
    
    LCD_SetWindows(sx, sy, ex, ey);
    
    // 提前计算好字节反转后的颜色并填充满缓冲区块
    uint16_t swapped_color = (color >> 8) | (color << 8);
    for (int i = 0; i < DMA_BUFFER_PIXELS; i++) {
        dma_buffer[i] = swapped_color;
    }

    // 分块将数据送入底层进行连续传输
    uint32_t pixels_sent = 0;
    while (pixels_sent < total_pixels) {
        uint32_t send_now = total_pixels - pixels_sent;
        if (send_now > DMA_BUFFER_PIXELS) send_now = DMA_BUFFER_PIXELS;

        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = send_now * 16;
        t.tx_buffer = dma_buffer;
        t.user = &ctx;
        spi_device_polling_transmit(spi, &t);
        
        pixels_sent += send_now;
    }
}

// 将外部传入的图像数组直接推送到屏幕的指定区域
/**
 * @brief 将位图数据通过 SPI DMA 推送到 LCD。
 *
 * @param x 起始 X 坐标。
 * @param y 起始 Y 坐标。
 * @param width 位图宽度。
 * @param height 位图高度。
 * @param bitmap RGB565 位图缓冲区。
 */
void LCD_DrawBitmap_DMA(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint16_t *bitmap)
{
    lcd_spi_trans_ctx_t ctx = {
        .dc_level = 1,
    };

    LCD_SetWindows(x, y, x + width - 1, y + height - 1);
    
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = width * height * 16;
    t.tx_buffer = bitmap;
    t.user = &ctx;
    spi_device_polling_transmit(spi, &t);
}

esp_err_t LCD_DrawBitmap_DMA_Async(uint16_t x,
                                   uint16_t y,
                                   uint16_t width,
                                   uint16_t height,
                                   const uint16_t *bitmap,
                                   lcd_trans_done_cb_t done_cb,
                                   void *user_ctx)
{
    if ((spi == NULL) || (bitmap == NULL) ||
        (width == 0U) || (height == 0U) ||
        ((uint32_t)x + width > LCD_WIDTH) ||
        ((uint32_t)y + height > LCD_HEIGHT)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_async_trans_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    LCD_SetWindows(x, y, x + width - 1, y + height - 1);

    memset(&s_async_trans, 0, sizeof(s_async_trans));
    s_async_ctx.dc_level = 1;
    s_async_ctx.done_cb = done_cb;
    s_async_ctx.user_ctx = user_ctx;

    s_async_trans.length = (uint32_t)width * (uint32_t)height * 16U;
    s_async_trans.tx_buffer = bitmap;
    s_async_trans.user = &s_async_ctx;

    s_async_trans_in_progress = true;
    esp_err_t ret = spi_device_queue_trans(spi,
                                           &s_async_trans,
                                           pdMS_TO_TICKS(10));
    if (ret != ESP_OK) {
        s_async_trans_in_progress = false;
    }

    return ret;
}

// 使用指定颜色覆盖整个屏幕画布
/**
 * @brief 清空整屏为指定颜色。
 *
 * @param Color RGB565 颜色值。
 */
void LCD_Clear(uint16_t Color)
{
    LCD_Fill_DMA(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, Color);
}

// 屏幕硬件及其总线底层的全局初始化流程
/**
 * @brief 初始化 LCD 控制引脚、SPI 总线和屏幕控制器。
 *
 * @note 初始化完成后会清屏为白色。
 */
void LCD_Init(void)
{
    // 配置控制和复位相关的引脚为推挽输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<LCD_PIN_DC) | (1ULL<<LCD_PIN_RST) | (1ULL<<LCD_PIN_BLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // 强制拉高背光控制引脚点亮屏幕底层光源
    gpio_set_level(LCD_PIN_BLK, 1);

    // 向系统申请一块专门用于直接内存访问传输的内部连续内存
    dma_buffer = heap_caps_malloc(DMA_BUFFER_PIXELS * 2, MALLOC_CAP_DMA);
    if (dma_buffer == NULL) {
        ESP_LOGE(TAG, "DMA allocation failed");
    }

    // 绑定串行外设接口的输入输出和时钟引脚
    spi_bus_config_t buscfg = {
        .miso_io_num = LCD_PIN_MISO,
        .mosi_io_num = LCD_PIN_MOSI,
        .sclk_io_num = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DMA_BUFFER_PIXELS * 2 + 8
    };
    
    // 初始化总线并启用自带的错误检查拦截机制
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret); 

    // 配置通信频率传输模式片选引脚以及取消假读限制
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 50 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 7,
        .pre_cb = lcd_spi_pre_transfer_callback,
        .post_cb = lcd_spi_post_transfer_callback,
        .flags = SPI_DEVICE_NO_DUMMY | SPI_DEVICE_HALFDUPLEX,
    };

    // 将屏幕设备挂载到总线上并获取有效的通信句柄
    ret = spi_bus_add_device(LCD_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    // 按照指定时序拉高拉低复位引脚触发屏幕芯片硬件复位
    gpio_set_level(LCD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(LCD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));

    // 顺序发送由原厂提供的底层驱动寄存器初始化配置指令
    LCD_WR_REG(0xCF);
    LCD_WR_DATA(0x00); LCD_WR_DATA(0xC1); LCD_WR_DATA(0x30);
    LCD_WR_REG(0xED);
    LCD_WR_DATA(0x64); LCD_WR_DATA(0x03); LCD_WR_DATA(0X12); LCD_WR_DATA(0X81);
    LCD_WR_REG(0xE8);
    LCD_WR_DATA(0x85); LCD_WR_DATA(0x00); LCD_WR_DATA(0x78);
    LCD_WR_REG(0xCB);
    LCD_WR_DATA(0x39); LCD_WR_DATA(0x2C); LCD_WR_DATA(0x00); LCD_WR_DATA(0x34); LCD_WR_DATA(0x02);
    LCD_WR_REG(0xF7); LCD_WR_DATA(0x20);
    LCD_WR_REG(0xEA); LCD_WR_DATA(0x00); LCD_WR_DATA(0x00);
    LCD_WR_REG(0xC0); LCD_WR_DATA(0x13);
    LCD_WR_REG(0xC1); LCD_WR_DATA(0x13);
    LCD_WR_REG(0xC5); LCD_WR_DATA(0x22); LCD_WR_DATA(0x35);
    LCD_WR_REG(0xC7); LCD_WR_DATA(0xBD);
    LCD_WR_REG(0x21);
    LCD_WR_REG(0x36); LCD_WR_DATA(0x08);
    LCD_WR_REG(0xB6); LCD_WR_DATA(0x0A); LCD_WR_DATA(0xA2);
    LCD_WR_REG(0x3A); LCD_WR_DATA(0x55);
    LCD_WR_REG(0xF6); LCD_WR_DATA(0x01); LCD_WR_DATA(0x30);
    LCD_WR_REG(0xB1); LCD_WR_DATA(0x00); LCD_WR_DATA(0x1B);
    LCD_WR_REG(0xF2); LCD_WR_DATA(0x00);
    LCD_WR_REG(0x26); LCD_WR_DATA(0x01);
    LCD_WR_REG(0xE0);
    LCD_WR_DATA(0x0F);LCD_WR_DATA(0x35);LCD_WR_DATA(0x31);LCD_WR_DATA(0x0B);LCD_WR_DATA(0x0E);
    LCD_WR_DATA(0x06);LCD_WR_DATA(0x49);LCD_WR_DATA(0xA7);LCD_WR_DATA(0x33);LCD_WR_DATA(0x07);
    LCD_WR_DATA(0x0F);LCD_WR_DATA(0x03);LCD_WR_DATA(0x0C);LCD_WR_DATA(0x0A);LCD_WR_DATA(0x00);
    LCD_WR_REG(0XE1);
    LCD_WR_DATA(0x00);LCD_WR_DATA(0x0A);LCD_WR_DATA(0x0F);LCD_WR_DATA(0x04);LCD_WR_DATA(0x11);
    LCD_WR_DATA(0x08);LCD_WR_DATA(0x36);LCD_WR_DATA(0x58);LCD_WR_DATA(0x4D);LCD_WR_DATA(0x07);
    LCD_WR_DATA(0x10);LCD_WR_DATA(0x0C);LCD_WR_DATA(0x32);LCD_WR_DATA(0x34);LCD_WR_DATA(0x0F);

    // 发送退出睡眠模式指令并阻塞延时等待内部硬件电路稳定
    LCD_WR_REG(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    
    // 发送开启显示指令让屏幕正式工作
    LCD_WR_REG(0x29);

    // 配置屏幕的数据扫描方向为默认的横屏模式
    LCD_WR_REG(0x36);
    LCD_WR_DATA((1<<3) | (1<<5) | (1<<6));

    // 调用清屏函数并填充白色背景以覆盖开机时的杂乱花屏像素
    LCD_Clear(WHITE);
}
