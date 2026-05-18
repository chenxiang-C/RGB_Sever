# ESP32 ESP-NOW 调色从机

本工程是一个基于 ESP-IDF 的 ESP32 从机节点程序。设备通过 ESP-NOW 与主机通信，接收主机分配的 `part` 编号，在本地 LCD 触摸屏上调节 RGB 颜色，并把当前颜色按固定格式回传给主机。

工程同时包含 320x240 SPI LCD、FT6336 触摸、LVGL UI、ESP-NOW 通信和 WS2812B 灯带驱动代码。其中当前 `app_main()` 已启动 UI 和 ESP-NOW 相关任务；WS2812B 驱动已编译进工程，但默认没有创建 `led_task()`。

## 主要功能

- ESP-NOW 从机节点，固定节点 ID 为 `SLAVE_MAGIC_NUMBER = 0x2`。
- Wi-Fi 使用 STA 模式，ESP-NOW 通道固定为 1。
- 上电后等待主机广播心跳，收到合法广播后自动绑定主机 MAC。
- 主机下发 `part:<编号>` 后，从机进入颜色上报模式。
- UI 提供 RGB 三通道滑块、颜色预览、当前绘制部位和主机在线状态显示。
- UI 提供系统监控页，显示 CPU、堆内存、LVGL 内存和运行时间等信息。
- 主机超时 16 秒未通信时，从机清空绑定状态并回到离线/待机状态。

## 工程结构

```text
.
├── CMakeLists.txt
├── main
│   ├── main.c              # 应用入口，初始化 NVS、Wi-Fi、ESP-NOW 并创建任务
│   ├── APP
│   │   ├── my_lvgl.c/.h    # LVGL UI、颜色缓存、状态显示
│   │   ├── ws2812b.c/.h    # WS2812B 灯带驱动
│   │   └── ui_font_cn_16.c # 中文字体
│   └── iot
│       ├── now.c/.h        # ESP-NOW 从机协议和任务
├── Hardware
│   ├── LCD                 # SPI LCD 驱动
│   └── TOUCH               # FT6336 触摸驱动
├── components/lvgl         # LVGL 组件
└── managed_components      # ESP-IDF 组件管理器下载的组件
```

## 硬件连接

当前代码按 ESP32 + SPI LCD + FT6336 触摸屏配置，主要引脚如下。

### LCD

| 功能 | GPIO |
| --- | --- |
| MOSI | GPIO23 |
| MISO | 未使用 |
| SCLK | GPIO18 |
| CS | GPIO27 |
| DC | GPIO2 |
| RST | GPIO4 |
| BLK | GPIO15 |

LCD 分辨率为 `320x240`，SPI 主机为 `SPI2_HOST`。

### FT6336 触摸

| 功能 | GPIO |
| --- | --- |
| SDA | GPIO22 |
| SCL | GPIO21 |
| RST | GPIO33 |
| INT | GPIO32 |

I2C 端口为 `I2C_NUM_0`，速率为 400 kHz。

### WS2812B

| 功能 | GPIO |
| --- | --- |
| 数据线 | GPIO25 |

默认灯珠数量为 6。当前主程序没有启动灯带同步任务，如需启用，可在 `app_create_tasks()` 中增加 `led_task`。

## ESP-NOW 通信协议

通信帧使用统一头部：

```c
typedef struct {
    uint8_t type;
    uint8_t state;
    uint16_t seq_num;
    uint16_t crc;
    uint32_t magic;
    uint8_t payload[0];
} example_espnow_data_t;
```

从机使用 CRC16 校验整帧。未绑定主机时只接收广播帧；绑定后只接收已绑定主机发送的帧。

### 主机到从机

payload 使用字符串格式：

```text
part:<number>
```

- `part:0`：主机心跳，仅刷新在线状态。
- `part:<大于0>`：分配当前绘制部位，从机进入颜色上报模式。
- 格式错误、负数或超过 `uint16_t` 范围时，从机会回传错误信息。

### 从机到主机

从机周期性回传：

```text
part: <part>,R:<r>,G:<g>,B:<b>
```

说明：

- 未进入颜色上报模式时，每 6 秒发送一次 `part: 0,R:0,G:0,B:0` 作为待机心跳。
- 进入颜色上报模式后，每 3 秒检查一次 UI 颜色。
- 颜色或 `part` 变化时上报真实颜色；未变化时发送 `part: 0,R:0,G:0,B:0` 作为在线心跳。
- 解析异常时回传 `<SLAVE_MAGIC_NUMBER>:error`。

## 任务划分

`main/main.c` 当前创建 4 个长期运行任务：

| 任务 | 核心 | 作用 |
| --- | --- | --- |
| `lvgl_task` | Core 1 | 初始化 LCD、触摸、LVGL，并运行 UI 主循环 |
| `espnow_slave_event` | Core 0 | 消费 ESP-NOW 回调事件，解析主机数据 |
| `espnow_slave_send` | Core 0 | 周期发送心跳或颜色数据 |
| `espnow_slave_heartbeat` | Core 0 | 检测主机超时并复位连接状态 |

## 依赖与环境

- 芯片目标：`esp32`
- ESP-IDF：`>= 4.1.0`
- Flash：当前 `sdkconfig` 配置为 16 MB、DIO 模式
- 组件依赖：
  - `lvgl/lvgl`
  - `espressif/led_strip`

## 编译与烧录

进入工程目录后执行：

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

Windows 下请把 `COMx` 替换为实际串口号，例如：

```bash
idf.py -p COM5 flash monitor
```

退出串口监视器使用 `Ctrl+]`。

## 配置说明

工程仍保留了 `main/Kconfig.projbuild` 中的 ESP-NOW 示例配置项，但当前核心通信参数在源码中固定：

- `ESPNOW_CHANNEL = 1`
- `ESPNOW_PMK = "ESP32_HOST_SLAVE_PMK"`
- `SLAVE_MAGIC_NUMBER = 0x2`
- `SLAVE_SEND_INTERVAL_MS = 3000`

如果需要让多个从机使用不同编号，请修改 `main/iot/now.h` 中的 `SLAVE_MAGIC_NUMBER`，并保证主机端协议与之匹配。

## 运行流程

1. 设备启动后初始化 NVS、Wi-Fi STA 和 ESP-NOW。
2. LCD 显示 UI，默认状态为离线，当前节点显示为 `节点 2`。
3. 从机周期性发送待机心跳，并等待主机广播。
4. 收到主机合法广播后绑定主机 MAC，UI 状态切换为在线。
5. 主机下发有效 `part` 后，UI 显示当前绘制部位。
6. 用户通过触摸屏调节 RGB，设备按周期把颜色数据回传主机。
7. 如果主机超过 16 秒没有发送有效数据，从机解绑并回到离线待机。

## 常见问题

### 收不到 ESP-NOW 数据

请检查主机和从机是否使用相同通道。本工程从机固定为通道 1，主机端也必须工作在同一通道。

### 从机一直离线

确认主机端发送的是广播心跳，且 payload 格式为 `part:0` 或有效的 `part:<编号>`。从机在未绑定前只接收广播帧。

### 收到数据但没有进入颜色上报

确认主机下发的 `part` 大于 0。`part:0` 只作为心跳，不会开启颜色上报模式。

### 触摸方向不正确

触摸坐标映射在 `main/APP/my_lvgl.c` 的 `touchpad_read()` 中处理。如果屏幕安装方向或 LCD 扫描方向变化，需要同步调整坐标转换逻辑。

### 中文显示异常

工程使用 `main/APP/ui_font_cn_16.c` 中的中文字体。若新增中文文案，需要确认字体文件包含对应字形。
