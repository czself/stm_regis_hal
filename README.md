# STM32 全栈修炼手册

> 寄存器与 HAL 双轨对照，编号 00–62，从点灯到 FreeRTOS，把 STM32F103 每个外设都掰开揉碎讲清楚。

## 项目简介

本项目是一套面向 STM32F103C8T6（BluePill）的系统性嵌入式学习资源，采用 **寄存器版 + HAL 版双轨对照** 的教学方式，让每一课都能同时看到底层硬件操作和工程标准写法。每课配有详尽的 README 讲解，从"为什么这样做"到"每一步对应什么寄存器"，力求消除黑盒。

## 快速开始

安装 PlatformIO Core 或 VS Code 的 PlatformIO 扩展，在仓库根目录执行：

```bash
# 编译第一课的寄存器版与 HAL 版
pio run -d stm32_full/01_led/reg
pio run -d stm32_full/01_led/hal

# 接好 ST-Link 后烧录寄存器版
pio run -d stm32_full/01_led/reg -t upload
```

其他课程替换 `01_led` 即可。每个 `reg/`、`hal/` 都有独立的 `platformio.ini`；接线与实验要求见对应课程 README。环境搭建从 [第 00 课](stm32_full/00_environment_intro/README.md) 开始。

仓库根目录的 `stm32_full.zip` 是已有打包文件；日常查阅和修改请使用 `stm32_full/` 下的源码与文档，压缩包不会随目录修改自动更新。

## 课程体系

### 基础入门

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 00 | [环境搭建与第一个工程](stm32_full/00_environment_intro/README.md) | PlatformIO、ST-Link、编译下载链路 |
| 01 | [LED 点灯](stm32_full/01_led/README.md) | GPIO 输出、RCC 时钟使能、BSRR/BRR |
| 02 | [GPIO 按键](stm32_full/02_gpio_key/README.md) | GPIO 输入、上拉下拉、软件消抖 |
| 03 | [时钟树](stm32_full/03_clock_tree/README.md) | HSE/HSI/PLL、SYSCLK/AHB/APB 分频 |
| 04 | [内核架构与存储器映射](stm32_full/04_core_architecture_memory_map/README.md) | Cortex-M3、Flash/SRAM 布局 |
| 05 | [SysTick](stm32_full/05_systick/README.md) | 系统滴答定时器、HAL_Delay 原理 |

### 定时器系列

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 06 | [定时器基础](stm32_full/06_timer_base/README.md) | TIM 时基、预分频、计数模式 |
| 07 | [输出比较](stm32_full/07_timer_output_compare/README.md) | OC 模式、翻转输出 |
| 08 | [PWM 基础](stm32_full/08_pwm_basic/README.md) | PWM 模式、占空比调节 |
| 09 | [PWM 进阶](stm32_full/09_pwm_advanced/README.md) | 中心对齐、互补输出、死区 |
| 10 | [外部中断](stm32_full/10_exti/README.md) | EXTI、中断服务函数 |
| 11 | [输入捕获](stm32_full/11_input_capture/README.md) | IC 模式、脉宽/频率测量 |
| 12 | [PWM 输入](stm32_full/12_timer_pwm_input/README.md) | 双 IC 通道测量 PWM |
| 13 | [编码器模式](stm32_full/13_timer_encoder/README.md) | 正交编码器接口 |
| 14 | [高级定时器 TIM1](stm32_full/14_timer_advanced_tim1/README.md) | 重复计数器、刹车输入 |

### ADC 与 DMA

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 15 | [ADC 轮询](stm32_full/15_adc_polling/README.md) | 单通道采样、转换值读取 |
| 16 | [ADC 中断](stm32_full/16_adc_interrupt/README.md) | 转换完成中断 |
| 17 | [ADC 多通道扫描](stm32_full/17_adc_multichannel_scan/README.md) | 扫描模式、DMA 配合 |
| 18 | [DMA 基础](stm32_full/18_dma_basic/README.md) | 通道、传输方向、循环模式 |
| 19 | [DMA 内存与 UART 案例](stm32_full/19_dma_memory_uart_cases/README.md) | 内存到内存、外设到内存 |
| 20 | [ADC + DMA](stm32_full/20_adc_dma/README.md) | 连续采样 + DMA 自动搬运 |

### 串口通信

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 21 | [UART 轮询](stm32_full/21_uart_polling/README.md) | 发送/接收、阻塞等待 |
| 22 | [UART 中断](stm32_full/22_uart_interrupt/README.md) | 接收中断、空闲中断 |
| 23 | [UART + DMA](stm32_full/23_uart_dma/README.md) | 收发 DMA、空闲中断配合 |
| 24 | [printf 重定向](stm32_full/24_uart_printf_redirect/README.md) | fputc 重定向、微库 |
| 25 | [串口数据包协议](stm32_full/25_uart_packet_protocol/README.md) | 帧头帧尾、校验、解析 |

### I2C 总线

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 26 | [I2C 基础](stm32_full/26_i2c_basic/README.md) | 起始/停止/应答、主机发送接收 |
| 27 | [软件 I2C 读写 EEPROM](stm32_full/27_i2c_software_eeprom/README.md) | GPIO 模拟 I2C、AT24C02 |
| 28 | [OLED SSD1306 显示](stm32_full/28_oled_ssd1306/README.md) | I2C 驱动 OLED、帧缓冲 |
| 29 | [MPU6050 六轴传感器](stm32_full/29_i2c_mpu6050/README.md) | I2C 读取加速度/陀螺仪 |

### SPI 总线

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 30 | [SPI 基础](stm32_full/30_spi_basic/README.md) | 全双工、CPOL/CPHA、NSS |
| 31 | [SPI 读写 W25Q64](stm32_full/31_spi_w25q64/README.md) | Flash 擦写、页编程 |

### CAN 总线

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 32 | [CAN 基础](stm32_full/32_can_basic/README.md) | 波特率、标识符、收发过滤 |

### 中断与系统

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 33 | [NVIC 优先级](stm32_full/33_nvic_priority/README.md) | 抢占优先级、子优先级、优先级分组 |
| 34 | [独立看门狗 IWDG](stm32_full/34_watchdog_iwdg/README.md) | 超时复位、喂狗 |
| 35 | [窗口看门狗 WWDG](stm32_full/35_watchdog_wwdg/README.md) | 窗口约束、提前唤醒 |
| 36 | [备份寄存器 BKP](stm32_full/36_bkp_backup_register/README.md) | 掉电保持、侵入检测 |
| 37 | [RTC 实时时钟](stm32_full/37_rtc_basic/README.md) | 秒中断、日期时间 |
| 38 | [内部 Flash 读写](stm32_full/38_flash_internal/README.md) | 解锁、擦除、编程 |

### 低功耗

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 39 | [睡眠模式](stm32_full/39_low_power_sleep/README.md) | WFI/WFE、中断唤醒 |
| 40 | [停机与待机](stm32_full/40_low_power_stop_standby/README.md) | 深度低功耗、WKUP 唤醒 |

### 调试与 FSMC

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 41 | [调试工具链](stm32_full/41_debug_toolchain/README.md) | SWD、GDB、OpenOCD |
| 42 | [FSMC SRAM](stm32_full/42_fsmc_sram/README.md) | 外扩 SRAM 读写 |
| 43 | [FSMC TFT LCD](stm32_full/43_tft_lcd_fsmc/README.md) | 并口 LCD 驱动、刷屏 |

### FreeRTOS 实时系统

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 44 | [FreeRTOS 入门与移植](stm32_full/44_freertos_intro_porting/README.md) | 源码结构、port 移植 |
| 45 | [任务创建与删除](stm32_full/45_freertos_task_create_delete/README.md) | xTaskCreate、vTaskDelete |
| 46 | [任务挂起与恢复](stm32_full/46_freertos_task_suspend_resume/README.md) | vTaskSuspend、vTaskResume |
| 47 | [调度器与时间片](stm32_full/47_freertos_scheduler_time_slice/README.md) | 同等优先级轮转 |
| 48 | [时间管理](stm32_full/48_freertos_time_management/README.md) | vTaskDelay、Tick 频率 |
| 49 | [中断与临界区](stm32_full/49_freertos_interrupt_critical/README.md) | taskENTER_CRITICAL、fromISR |
| 50 | [队列](stm32_full/50_freertos_queue/README.md) | xQueueSend/Receive |
| 51 | [信号量（二值/计数）](stm32_full/51_freertos_semaphore_binary_counting/README.md) | xSemaphoreCreate/Take/Give |
| 52 | [互斥量与优先级反转](stm32_full/52_freertos_mutex_priority_inversion/README.md) | 优先级继承 |
| 53 | [队列集](stm32_full/53_freertos_queue_set/README.md) | 多队列统一等待 |
| 54 | [事件组](stm32_full/54_freertos_event_group/README.md) | xEventGroupSet/Wait |
| 55 | [任务通知](stm32_full/55_freertos_task_notification/README.md) | 轻量级信号/邮箱 |
| 56 | [软件定时器](stm32_full/56_freertos_software_timer/README.md) | xTimerStart/Pend |
| 57 | [内存管理](stm32_full/57_freertos_memory_management/README.md) | heap_1~heap_5 |
| 58 | [低功耗 Tickless](stm32_full/58_freertos_low_power_tickless/README.md) | 空闲钩子、停止 SysTick |
| 59 | [FreeRTOS + UART](stm32_full/59_freertos_uart/README.md) | 串口收发任务化 |
| 60 | [FreeRTOS + ADC + DMA](stm32_full/60_freertos_adc_dma/README.md) | 采样任务 + DMA 中断 |

### 综合项目

| 编号 | 课程 | 关键词 |
|------|------|--------|
| 61 | [小型系统项目](stm32_full/61_small_system_project/README.md) | 多外设协同、状态机 |
| 62 | [LCD 12864 显示](stm32_full/62_lcd_12864/README.md) | 并口/串口 LCD 驱动 |

## 目录结构

```
stm32/
├── stm32_full/                  # 主教程：63 个课程目录（编号 00–62）
│   ├── 00_environment_intro/    # 每课目录结构：
│   │   ├── README.md            #   详尽讲解文档
│   │   ├── reg/                 #   寄存器版工程
│   │   │   ├── platformio.ini
│   │   │   └── src/main.c
│   │   └── hal/                 #   HAL 版工程
│   │       ├── platformio.ini
│   │       └── src/main.c
│   ├── 01_led/
│   ├── ...
│   ├── 62_lcd_12864/
│   ├── common/                  # 公共工具（VOFA+ 等）
│   ├── freertos/                # FreeRTOS 配置文件
│   └── lib/                     # FreeRTOS 内核源码
├── stm_register/                # 裸机编程指南（参考项目）
├── re/                          # 寄存器笔记
│   └── register/
│       ├── BSRR寄存器.md
│       ├── GPIO常见寄存器操作.md
│       └── 引脚配置寄存器.md
└── .gitignore
```

## 开发环境

- **目标芯片**：STM32F103C8T6（BluePill）
- **构建工具**：PlatformIO
- **开发框架**：STM32Cube（CMSIS + HAL）
- **下载器**：ST-Link（SWD 接口）
- **外部晶振**：8MHz HSE，系统时钟配置到 72MHz

## 学习建议

1. **先寄存器，后 HAL** — 每课先读 `reg/` 理解底层在改什么，再看 `hal/` 学习工程标准写法
2. **按编号顺序推进** — 课程之间有依赖关系，跳课容易断层
3. **动手跑每一课** — 编译、下载、观察现象，再对照 README 逐行理解
4. **回答每课的检验问题** — README 末尾都有自测题，能讲清楚才算吃透

## 子项目说明

- **stm32_full**：主教程，63 个课程目录（编号 00–62），每课含寄存器版 + HAL 版双轨代码与详尽中文讲解
- **stm_register**：裸机编程参考指南，展示不依赖任何框架、仅用 GCC + 数据手册开发 MCU 的方法
- **re/register**：寄存器速查笔记，聚焦 BSRR、GPIO 等高频操作寄存器的实战用法
