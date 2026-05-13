#ifndef LCD_H
#define LCD_H

#include <stdbool.h>
#include <stdint.h>

#define LCD_HOST        SPI2_HOST
#define LCD_PIN_MOSI    23
#define LCD_PIN_MISO    -1
#define LCD_PIN_CLK     18
#define LCD_PIN_CS      27
#define LCD_PIN_DC      2
#define LCD_PIN_RST     4
#define LCD_PIN_BLK     15

#define LCD_WIDTH       320
#define LCD_HEIGHT      240

/* 常用 RGB565 颜色 */
#define WHITE           0xFFFF
#define BLACK           0x0000
#define BLUE            0x001F
#define BRED            0xF81F
#define GRED            0xFFE0
#define GBLUE           0x07FF
#define RED             0xF800
#define MAGENTA         0xF81F
#define GREEN           0x07E0
#define CYAN            0x7FFF
#define YELLOW          0xFFE0

/**
 * @brief 初始化 LCD SPI 总线、控制引脚和屏幕寄存器。
 *
 * @note 必须在其它 LCD 绘制接口前调用一次。
 */
void LCD_Init(void);

/**
 * @brief 使用指定 RGB565 颜色清空整屏。
 *
 * @param Color RGB565 颜色值。
 */
void LCD_Clear(uint16_t Color);

/**
 * @brief 设置 LCD 后续写入的矩形窗口。
 *
 * @param xStar 起始 X 坐标。
 * @param yStar 起始 Y 坐标。
 * @param xEnd 结束 X 坐标。
 * @param yEnd 结束 Y 坐标。
 */
void LCD_SetWindows(uint16_t xStar,
                    uint16_t yStar,
                    uint16_t xEnd,
                    uint16_t yEnd);

/**
 * @brief 绘制单个像素点。
 *
 * @param x X 坐标。
 * @param y Y 坐标。
 * @param color RGB565 颜色值。
 */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief 使用 DMA 缓冲区填充矩形区域。
 *
 * @param sx 起始 X 坐标。
 * @param sy 起始 Y 坐标。
 * @param ex 结束 X 坐标。
 * @param ey 结束 Y 坐标。
 * @param color RGB565 颜色值。
 */
void LCD_Fill_DMA(uint16_t sx,
                  uint16_t sy,
                  uint16_t ex,
                  uint16_t ey,
                  uint16_t color);

/**
 * @brief 将 RGB565 位图数据绘制到指定区域。
 *
 * @param x 起始 X 坐标。
 * @param y 起始 Y 坐标。
 * @param width 位图宽度。
 * @param height 位图高度。
 * @param bitmap RGB565 像素数据指针，长度至少为 width * height。
 * @note bitmap 指针不能为 NULL，调用者负责保证缓冲区在传输期间有效。
 */
void LCD_DrawBitmap_DMA(uint16_t x,
                        uint16_t y,
                        uint16_t width,
                        uint16_t height,
                        const uint16_t *bitmap);

#endif /* LCD_H */
