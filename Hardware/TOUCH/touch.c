#include "touch.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "FT6336";
m_tp_dev tp_dev = {0};

// ======================= 硬件 I2C 底层 =======================

/**
 * @brief 向 FT6336 指定寄存器写入数据。
 *
 * @param reg 寄存器地址。
 * @param data 待写入数据缓冲区。
 * @param len 待写入字节数。
 * @return ESP_OK 表示写入成功，否则返回 I2C 错误码。
 * @note 当前工程暂未使用写寄存器接口，保留用于后续配置触摸芯片。
 */
static esp_err_t ft_write_reg(uint8_t reg, uint8_t *data, uint8_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, FT_CMD_WR, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(CTP_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/**
 * @brief 从 FT6336 指定寄存器读取数据。
 *
 * @param reg 起始寄存器地址。
 * @param data 输出数据缓冲区。
 * @param len 读取字节数。
 * @return ESP_OK 表示读取成功，否则返回 I2C 错误码。
 */
static esp_err_t ft_read_reg(uint8_t reg, uint8_t *data, uint8_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, FT_CMD_WR, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd); // Repeated start
    i2c_master_write_byte(cmd, FT_CMD_RD, true);
    i2c_master_read(cmd, data, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(CTP_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// ======================= 触摸核心逻辑 =======================

/**
 * @brief 初始化 FT6336 触摸控制器。
 *
 * @return 初始化和芯片 ID 校验成功返回 true，否则返回 false。
 * @note 会初始化 I2C 主机、复位脚和中断脚，并读取厂商 ID。
 */
bool TP_Init(void)
{
    // 1. 初始化 I2C 主机模式
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CTP_PIN_SDA,
        .scl_io_num = CTP_PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000, // 400KHz 高速 I2C
    };
    i2c_param_config(CTP_I2C_PORT, &conf);
    i2c_driver_install(CTP_I2C_PORT, conf.mode, 0, 0, 0);

    // 2. 初始化复位与中断引脚
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL<<CTP_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    
    io_conf.pin_bit_mask = (1ULL<<CTP_PIN_INT);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // 3. 硬件复位触摸屏
    gpio_set_level(CTP_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CTP_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. 验证芯片 ID
    uint8_t id_buf[2];
    ft_read_reg(0xA8, &id_buf[0], 1); // 厂商 ID
    if (id_buf[0] != 0x11) {
        ESP_LOGE(TAG, "FT6336 Not Found! ID: 0x%02X", id_buf[0]);
        return false;
    }
    ESP_LOGI(TAG, "FT6336 Touhc Panel initialized successfully.");
    return true;
}

/**
 * @brief 扫描一次 FT6336 触摸数据。
 *
 * @return 检测到触摸返回 1，否则返回 0。
 * @note 读取到的坐标写入全局 tp_dev；I2C 读取失败时按无触摸处理。
 */
uint8_t FT6336_Scan(void)
{
    uint8_t buf[4];
    uint8_t mode = 0;
    
    // 1. LVGL 已经有 30ms 轮询节拍，直接读最新数据，删掉所有的 t 降频变量
    esp_err_t ret = ft_read_reg(FT_REG_NUM_FINGER, &mode, 1); 
    
    // 如果 I2C 读取失败，直接当做没有触摸，防止读出乱码
    if (ret != ESP_OK) return 0;

    // 2. 判断是否有手指按下 (FT6336最多支持2点触摸)
    if (mode > 0 && mode < 3) {
        tp_dev.sta = TP_PRES_DOWN | TP_CATH_PRES; 
        
        ft_read_reg(FT_TP1_REG, buf, 4);
        
        // 提取原始坐标
        uint16_t raw_x = ((uint16_t)(buf[0] & 0x0F) << 8) + buf[1];
        uint16_t raw_y = ((uint16_t)(buf[2] & 0x0F) << 8) + buf[3];

        // ========================================================
        // ⚠️ 重点避坑：如果你之前把 LCD 屏幕设置成了横屏
        // 这里的触摸坐标必须跟着交换！否则你上下滑，屏幕左右动，也会觉得卡顿
        // 竖屏写法：tp_dev.x[0] = raw_x; tp_dev.y[0] = raw_y;
        // 横屏写法 (根据你实际屏幕旋转方向，可能还需要用 LCD_HEIGHT 减去坐标)：
        // ========================================================
        tp_dev.x[0] = raw_x; 
        tp_dev.y[0] = raw_y; 

        return 1; // 成功读取
    } else {
        // 3. 确实没有手指按下，干净利落地清空状态
        tp_dev.sta = 0; 
        tp_dev.x[0] = 0xFFFF;
        tp_dev.y[0] = 0xFFFF;
        return 0;
    }
}
