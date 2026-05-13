#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#define CTP_I2C_PORT        I2C_NUM_0
#define CTP_PIN_SDA         22
#define CTP_PIN_SCL         21
#define CTP_PIN_RST         33
#define CTP_PIN_INT         32

#define FT_CMD_WR           0x70
#define FT_CMD_RD           0x71
#define FT_REG_NUM_FINGER   0x02
#define FT_TP1_REG          0x03

#define CTP_MAX_TOUCH       2

#define TP_PRES_DOWN        0x80
#define TP_CATH_PRES        0x40

/**
 * @brief FT6336 触摸状态缓存。
 *
 * @note FT6336_Scan() 更新该结构，LVGL 触摸读取回调读取该结构。
 */
typedef struct {
    uint16_t x[CTP_MAX_TOUCH];
    uint16_t y[CTP_MAX_TOUCH];
    uint8_t sta;
} m_tp_dev;

extern m_tp_dev tp_dev;

/**
 * @brief 初始化 FT6336 触摸芯片和 I2C 总线。
 *
 * @return 初始化成功返回 true；芯片 ID 校验失败返回 false。
 * @note 必须在 FT6336_Scan() 前调用一次。
 */
bool TP_Init(void);

/**
 * @brief 扫描当前触摸状态。
 *
 * @return 检测到有效触摸返回 1，否则返回 0。
 * @note 结果写入全局 tp_dev，供 LVGL 触摸读取回调使用。
 */
uint8_t FT6336_Scan(void);

#endif /* TOUCH_H */
