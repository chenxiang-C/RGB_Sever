#include "ws2812b.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "my_lvgl.h"

static const char *TAG = "WS2812B";

#define LED_RMT_RESOLUTION_HZ  (10000000U)
#define LED_RMT_MEM_SYMBOLS    (64U)
#define LED_TASK_DELAY_MS      (20U)
#define LED_DEFAULT_R          (100U)
#define LED_DEFAULT_G          (0U)
#define LED_DEFAULT_B          (0U)

static led_strip_handle_t s_led_strip;
static ui_rgb_color_t s_last_color = {
    .r = LED_DEFAULT_R,
    .g = LED_DEFAULT_G,
    .b = LED_DEFAULT_B,
};

/**
 * @brief 初始化 WS2812B RMT 驱动实例。
 *
 * @note 初始化成功后会清空灯带；失败时 s_led_strip 保持 NULL，
 *       后续 ws2812b_set_color() 会直接返回。
 */
void ws2812b_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812B_GPIO_PIN,
        .max_leds = WS2812B_LED_NUM,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = LED_RMT_MEM_SYMBOLS,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config,
                                             &rmt_config,
                                             &s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WS2812B init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = led_strip_clear(s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS2812B clear failed: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "WS2812B initialized");
}

/**
 * @brief 将所有 WS2812B 灯珠设置为同一个 RGB 颜色。
 *
 * @param r 红色通道值。
 * @param g 绿色通道值。
 * @param b 蓝色通道值。
 * @note 本函数依赖 led_strip 组件；每个底层调用都会检查返回值。
 */
void ws2812b_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_strip == NULL) {
        return;
    }

    for (uint32_t i = 0U; i < WS2812B_LED_NUM; i++) {
        esp_err_t ret = led_strip_set_pixel(s_led_strip, i, r, g, b);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "set pixel failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    esp_err_t ret = led_strip_refresh(s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief 灯带刷新任务入口。
 *
 * @param pvParameters FreeRTOS 任务参数，当前未使用。
 * @note 任务周期读取 UI 颜色，仅在颜色变化时刷新灯带，减少 RMT 传输。
 */
void led_task(void *pvParameters)
{
    (void)pvParameters;
    ui_rgb_color_t color = {0};

    ws2812b_init();
    if (ui_get_color(&color)) {
        ws2812b_set_color(color.r, color.g, color.b);
        s_last_color = color;
    }

    while (true) {
        if (ui_get_color(&color) &&
            ((s_last_color.r != color.r) ||
             (s_last_color.g != color.g) ||
             (s_last_color.b != color.b))) {
            ws2812b_set_color(color.r, color.g, color.b);
            s_last_color = color;
            ESP_LOGI(TAG, "RGB: %u, %u, %u", color.r, color.g, color.b);
        }
        vTaskDelay(pdMS_TO_TICKS(LED_TASK_DELAY_MS));
    }
}
