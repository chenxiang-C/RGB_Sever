#include "my_lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd.h"
#include "lvgl.h"
#include "now.h"
#include "touch.h"

static const char *TAG = "lvgl_base";

#define UI_DEFAULT_R             (100U)
#define UI_DEFAULT_G             (0U)
#define UI_DEFAULT_B             (0U)
#define UI_SLIDER_MAX            (255U)
#define UI_LVGL_BUF_LINES        (10U)
#define UI_LVGL_LOCK_TIMEOUT_MS  (50U)
#define UI_LVGL_TASK_DELAY_MS    (10U)
#define UI_TOUCH_PHYSICAL_WIDTH  (240)
#define UI_TICK_PERIOD_US        (1000U)

static ui_rgb_color_t s_ui_color = {
    .r = UI_DEFAULT_R,
    .g = UI_DEFAULT_G,
    .b = UI_DEFAULT_B,
};
static uint16_t s_current_part;
static SemaphoreHandle_t s_lvgl_mutex;
static portMUX_TYPE s_ui_color_lock = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *s_node_label;
static lv_obj_t *s_slave_status_label;
static lv_obj_t *s_color_preview;
static lv_obj_t *s_part_label;
static lv_obj_t *s_slider_r;
static lv_obj_t *s_slider_g;
static lv_obj_t *s_slider_b;
static lv_obj_t *s_label_r;
static lv_obj_t *s_label_g;
static lv_obj_t *s_label_b;

static void lvgl_disp_flush_cb(lv_display_t *disp,
                               const lv_area_t *area,
                               uint8_t *color_map);
static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data);
static void increase_lvgl_tick(void *arg);

/**
 * @brief 写入 UI 当前 RGB 颜色缓存。
 *
 * @param r 红色通道值。
 * @param g 绿色通道值。
 * @param b 蓝色通道值。
 * @note 使用 FreeRTOS 临界区保护，避免 UI 任务写入时通信任务读取到半更新值。
 */
static void ui_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    portENTER_CRITICAL(&s_ui_color_lock);
    s_ui_color.r = r;
    s_ui_color.g = g;
    s_ui_color.b = b;
    portEXIT_CRITICAL(&s_ui_color_lock);
}

/**
 * @brief 读取 UI 当前 RGB 颜色缓存。
 *
 * @param color 输出颜色结构体指针，不能为 NULL。
 * @return 读取成功返回 true；参数非法返回 false。
 */
bool ui_get_color(ui_rgb_color_t *color)
{
    if (color == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_ui_color_lock);
    *color = s_ui_color;
    portEXIT_CRITICAL(&s_ui_color_lock);

    return true;
}

/**
 * @brief 尝试获取 LVGL 互斥锁。
 *
 * @param timeout 最大等待 tick 数。
 * @return 获取成功返回 true，否则返回 false。
 * @note 所有非 LVGL 主循环中的 LVGL 对象访问都必须先获取该锁。
 */
static bool ui_take_lvgl(TickType_t timeout)
{
    return (s_lvgl_mutex != NULL) &&
           (xSemaphoreTake(s_lvgl_mutex, timeout) == pdTRUE);
}

/**
 * @brief 释放 LVGL 互斥锁。
 */
static void ui_give_lvgl(void)
{
    if (s_lvgl_mutex != NULL) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}

/**
 * @brief RGB 滑块事件回调。
 *
 * @param event LVGL 事件对象。
 * @note 在 LVGL 任务上下文执行；负责同步颜色缓存、文本和预览色块。
 */
static void slider_event_cb(lv_event_t *event)
{
    ui_rgb_color_t color = {
        .r = (uint8_t)lv_slider_get_value(s_slider_r),
        .g = (uint8_t)lv_slider_get_value(s_slider_g),
        .b = (uint8_t)lv_slider_get_value(s_slider_b),
    };
    static uint32_t s_last_update_time;
    uint32_t current_time = (uint32_t)xTaskGetTickCount() *
                            (uint32_t)portTICK_PERIOD_MS;
    lv_event_code_t code = lv_event_get_code(event);

    ui_set_color(color.r, color.g, color.b);

    lv_label_set_text_fmt(s_label_r, "R: %u", color.r);
    lv_label_set_text_fmt(s_label_g, "G: %u", color.g);
    lv_label_set_text_fmt(s_label_b, "B: %u", color.b);
    lv_obj_set_style_bg_color(s_color_preview,
                              lv_color_make(color.r, color.g, color.b),
                              0);

    if ((code == LV_EVENT_RELEASED) ||
        ((current_time - s_last_update_time) > 30U)) {
        s_last_update_time = current_time;
    }
}

/**
 * @brief 更新当前 part 标签显示。
 *
 * @param part_id 当前 part 编号；0 表示空闲。
 * @note 可由 ESP-NOW 任务调用，内部使用 LVGL 互斥锁保护。
 */
void ui_update_part_id(uint16_t part_id)
{
    if (!ui_take_lvgl(pdMS_TO_TICKS(UI_LVGL_LOCK_TIMEOUT_MS))) {
        return;
    }

    s_current_part = part_id;
    if (s_part_label != NULL) {
        if (part_id == 0U) {
            lv_label_set_text(s_part_label, "Part: 0 (Idle)");
            lv_obj_set_style_text_color(s_part_label,
                                        lv_color_hex(0xAAAAAA),
                                        0);
        } else {
            lv_label_set_text_fmt(s_part_label, "Part: %u", part_id);
            lv_obj_set_style_text_color(s_part_label,
                                        lv_color_hex(0xFFFFFF),
                                        0);
        }
    }

    ui_give_lvgl();
}

/**
 * @brief 更新主机连接状态标签显示。
 *
 * @param is_connected true 显示 Online，false 显示 Offline。
 * @note 可由 ESP-NOW 任务调用，内部使用 LVGL 互斥锁保护。
 */
void ui_update_slave_status(bool is_connected)
{
    if ((s_slave_status_label == NULL) ||
        !ui_take_lvgl(pdMS_TO_TICKS(UI_LVGL_LOCK_TIMEOUT_MS))) {
        return;
    }

    if (is_connected) {
        lv_label_set_text(s_slave_status_label, LV_SYMBOL_WIFI " Online");
        lv_obj_set_style_text_color(s_slave_status_label,
                                    lv_color_hex(0x00FF00),
                                    0);
    } else {
        lv_label_set_text(s_slave_status_label,
                          LV_SYMBOL_WARNING " Offline");
        lv_obj_set_style_text_color(s_slave_status_label,
                                    lv_color_hex(0xFF4444),
                                    0);
    }

    ui_give_lvgl();
}

/**
 * @brief 创建一组颜色滑块及其数值标签。
 *
 * @param parent 父容器对象。
 * @param name 左侧通道名称，例如 "R"。
 * @param color 滑块指示条和旋钮颜色。
 * @param out_slider 输出创建的滑块对象指针。
 * @param out_label 输出创建的标签对象指针。
 * @param init_val 初始通道值。
 */
static void create_color_slider(lv_obj_t *parent,
                                const char *name,
                                lv_color_t color,
                                lv_obj_t **out_slider,
                                lv_obj_t **out_label,
                                uint8_t init_val)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 35);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    *out_label = lv_label_create(row);
    lv_label_set_text_fmt(*out_label, "%s: %u", name, init_val);
    lv_obj_set_style_text_color(*out_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(*out_label, 60);

    *out_slider = lv_slider_create(row);
    lv_obj_set_flex_grow(*out_slider, 1);
    lv_obj_set_height(*out_slider, 15);
    lv_obj_set_ext_click_area(*out_slider, 15);
    lv_slider_set_range(*out_slider, 0, UI_SLIDER_MAX);
    lv_slider_set_value(*out_slider, init_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*out_slider, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*out_slider, color, LV_PART_KNOB);
    lv_obj_set_style_pad_all(*out_slider, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(*out_slider,
                        slider_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(*out_slider,
                        slider_event_cb,
                        LV_EVENT_RELEASED,
                        NULL);
}

/**
 * @brief 创建顶部标题栏。
 *
 * @param screen 当前活动屏幕对象。
 * @note 标题栏显示本机节点编号和主机在线状态。
 */
static void build_header(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);

    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_node_label = lv_label_create(header);
    lv_obj_align(s_node_label, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_font(s_node_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_node_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text_fmt(s_node_label, "Node: %lu",
                          (unsigned long)SLAVE_MAGIC_NUMBER);

    s_slave_status_label = lv_label_create(header);
    lv_obj_align(s_slave_status_label, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_font(s_slave_status_label,
                               &lv_font_montserrat_16,
                               0);
    lv_label_set_text(s_slave_status_label, LV_SYMBOL_WARNING " Offline");
    lv_obj_set_style_text_color(s_slave_status_label,
                                lv_color_hex(0xFF4444),
                                0);
}

/**
 * @brief 创建主内容容器。
 *
 * @param screen 当前活动屏幕对象。
 * @return 创建成功的容器对象。
 */
static lv_obj_t *build_main_container(lv_obj_t *screen)
{
    lv_obj_t *main_cont = lv_obj_create(screen);

    lv_obj_set_size(main_cont, lv_pct(94), lv_pct(82));
    lv_obj_align(main_cont, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_opa(main_cont, 0, 0);
    lv_obj_set_style_border_width(main_cont, 0, 0);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(main_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_cont,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(main_cont, 8, 0);

    return main_cont;
}

/**
 * @brief 创建颜色预览区域和 part 标签。
 *
 * @param parent 主内容容器。
 * @note part 标签放在颜色预览块中间，颜色由当前 UI 缓存初始化。
 */
static void build_color_preview(lv_obj_t *parent)
{
    ui_rgb_color_t color = {0};
    (void)ui_get_color(&color);

    s_color_preview = lv_obj_create(parent);
    lv_obj_set_size(s_color_preview, lv_pct(100), 55);
    lv_obj_set_style_radius(s_color_preview, 10, 0);
    lv_obj_set_style_border_width(s_color_preview, 2, 0);
    lv_obj_set_style_border_color(s_color_preview,
                                  lv_color_hex(0x555555),
                                  0);
    lv_obj_set_style_bg_color(s_color_preview,
                              lv_color_make(color.r, color.g, color.b),
                              0);

    s_part_label = lv_label_create(s_color_preview);
    lv_obj_center(s_part_label);
    lv_obj_set_style_text_font(s_part_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_opa(s_part_label, 100, 0);
    lv_obj_set_style_bg_color(s_part_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(s_part_label, 4, 0);
    lv_obj_set_style_radius(s_part_label, 5, 0);
    ui_update_part_id(s_current_part);
}

/**
 * @brief 构建完整调色 UI。
 *
 * @note 包含顶部状态栏、颜色预览块和 RGB 三个滑块。
 */
static void build_color_picker_ui(void)
{
    ui_rgb_color_t color = {0};
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *main_cont = NULL;

    (void)ui_get_color(&color);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1E1E1E), 0);
    build_header(screen);
    main_cont = build_main_container(screen);
    build_color_preview(main_cont);
    create_color_slider(main_cont,
                        "R",
                        lv_color_hex(0xFF4444),
                        &s_slider_r,
                        &s_label_r,
                        color.r);
    create_color_slider(main_cont,
                        "G",
                        lv_color_hex(0x44FF44),
                        &s_slider_g,
                        &s_label_g,
                        color.g);
    create_color_slider(main_cont,
                        "B",
                        lv_color_hex(0x4444FF),
                        &s_slider_b,
                        &s_label_b,
                        color.b);
}

/**
 * @brief 初始化 LVGL 显示端口。
 *
 * @return 成功返回 true，失败返回 false。
 * @note 分配两块 DMA 内部 RAM 作为 LVGL 局部刷新缓冲区。
 */
static bool lv_port_disp_init(void)
{
    LCD_Init();

    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_disp_flush_cb);

    size_t buf_size_bytes = LCD_WIDTH * UI_LVGL_BUF_LINES *
                            sizeof(uint16_t);
    void *buf_1 = heap_caps_malloc(buf_size_bytes,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf_2 = heap_caps_malloc(buf_size_bytes,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if ((buf_1 == NULL) || (buf_2 == NULL)) {
        ESP_LOGE(TAG, "LVGL DMA buffer allocation failed");
        heap_caps_free(buf_1);
        heap_caps_free(buf_2);
        return false;
    }

    lv_display_set_buffers(disp,
                           buf_1,
                           buf_2,
                           buf_size_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    return true;
}

/**
 * @brief 初始化 LVGL 触摸输入设备。
 *
 * @return 成功返回 true，失败返回 false。
 * @note 当前保留红色光标点用于触摸坐标调试。
 */
static bool lv_port_indev_init(void)
{
    TP_Init();

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        ESP_LOGE(TAG, "lv_indev_create failed");
        return false;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);
    lv_indev_set_display(indev, lv_display_get_default());

    lv_obj_t *cursor_obj = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(cursor_obj, 15, 15);
    lv_obj_set_style_bg_color(cursor_obj, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(cursor_obj, 200, 0);
    lv_obj_set_style_radius(cursor_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(cursor_obj, 0, 0);
    lv_obj_remove_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_indev_set_cursor(indev, cursor_obj);

    return true;
}

/**
 * @brief LVGL 显示刷新回调。
 *
 * @param disp LVGL 显示对象。
 * @param area 需要刷新的屏幕区域。
 * @param color_map RGB565 像素数据缓冲区。
 * @note LCD 驱动要求高低字节交换，因此发送前会原地调整 color_map。
 */
static void lvgl_disp_flush_cb(lv_display_t *disp,
                               const lv_area_t *area,
                               uint8_t *color_map)
{
    const uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
    const uint16_t height = (uint16_t)(area->y2 - area->y1 + 1);
    uint32_t pixel_count = (uint32_t)width * (uint32_t)height;
    uint16_t *color16 = (uint16_t *)color_map;

    (void)disp;
    for (uint32_t i = 0U; i < pixel_count; i++) {
        color16[i] = (uint16_t)((color16[i] >> 8U) |
                                (color16[i] << 8U));
    }

    LCD_DrawBitmap_DMA(area->x1, area->y1, width, height, color16);
    lv_display_flush_ready(disp);
}

/**
 * @brief LVGL 触摸读取回调。
 *
 * @param indev LVGL 输入设备对象，当前未使用。
 * @param data 输出触摸状态和坐标。
 * @note 将 FT6336 原始坐标映射到当前横屏 UI 坐标。
 */
static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static lv_point_t last_point = {0, 0};

    (void)indev;
    FT6336_Scan();

    if ((tp_dev.sta & TP_PRES_DOWN) != 0U) {
        last_point.x = tp_dev.y[0];
        last_point.y = UI_TOUCH_PHYSICAL_WIDTH - tp_dev.x[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    data->point = last_point;
    data->continue_reading = false;
}

/**
 * @brief LVGL tick 定时器回调。
 *
 * @param arg 定时器参数，当前未使用。
 * @note 每 1 ms 调用一次 lv_tick_inc(1)。
 */
static void increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/**
 * @brief 创建并启动 LVGL tick 定时器。
 *
 * @return 成功返回 true，失败返回 false。
 */
static bool lvgl_timer_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick = NULL;
    esp_err_t ret = esp_timer_create(&timer_args, &lvgl_tick);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_timer_start_periodic(lvgl_tick, UI_TICK_PERIOD_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s",
                 esp_err_to_name(ret));
        return false;
    }

    return true;
}

/**
 * @brief 初始化 LVGL 相关资源并进入 UI 主循环。
 *
 * @note 本函数在 lvgl_task() 内调用。若初始化失败则直接返回，
 *       任务随后删除自身。
 */
static void lvgl_base_init(void)
{
    lv_init();
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == NULL) {
        ESP_LOGE(TAG, "LVGL mutex allocation failed");
        return;
    }

    if (!lv_port_disp_init() || !lv_port_indev_init() ||
        !lvgl_timer_init()) {
        ESP_LOGE(TAG, "LVGL initialization failed");
        return;
    }

    build_color_picker_ui();

    while (true) {
        if (ui_take_lvgl(portMAX_DELAY)) {
            (void)lv_timer_handler();
            ui_give_lvgl();
        }
        vTaskDelay(pdMS_TO_TICKS(UI_LVGL_TASK_DELAY_MS));
    }
}

/**
 * @brief LVGL UI 任务入口。
 *
 * @param pvParameters FreeRTOS 任务参数，当前未使用。
 */
void lvgl_task(void *pvParameters)
{
    (void)pvParameters;
    lvgl_base_init();
    vTaskDelete(NULL);
}
