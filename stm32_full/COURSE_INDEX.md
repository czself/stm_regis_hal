# STM32 课程总索引

## 说明

这份文档是 `stm32_full` 的课程顺序、依赖关系和 Demo 目标总表。课程已经按学习链路插入式重编号，编号从 `00_environment_intro` 到 `67_multichannel_ota` 连续排列。

- 学习问题记录：[LEARNING_ISSUES.md](LEARNING_ISSUES.md)
- 通信接口对比：[COMMUNICATION_INTERFACES_COMPARISON.md](COMMUNICATION_INTERFACES_COMPARISON.md)
- 教学写作规范：[TEACHING_PROMPT.md](TEACHING_PROMPT.md)

统一约束：每课包含 `README.md`、`reg/`、`hal/`；README 按 17 章教学闭环维护；代码落到可编译、可上板或明确标注的可编译模拟 Demo。

## 当前进度

`00-67` 课程目录已经纳入总索引。STM32 主线、FreeRTOS 主线、Bootloader/OTA 主线都按插入式学习顺序排列，后续维护以本索引为准。

## 全部课程

### 00_environment_intro

- 课题：环境搭建与第一个工程
- Demo：编译、下载并观察 PC13 LED 闪烁
- 入口：[00_environment_intro/README.md](00_environment_intro/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 01_led

- 课题：LED 点灯
- Demo：PC13 LED 周期闪烁
- 入口：[01_led/README.md](01_led/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 02_gpio_key

- 课题：GPIO 按键输入
- Demo：PA0 按键控制 PC13 LED
- 入口：[02_gpio_key/README.md](02_gpio_key/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 03_clock_tree

- 课题：时钟树与 72MHz 系统时钟
- Demo：切换 HSE+PLL 后用 LED 节奏验证系统时钟
- 入口：[03_clock_tree/README.md](03_clock_tree/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 04_core_architecture_memory_map

- 课题：内核架构与存储器映射
- Demo：调试变量记录 CPUID、Flash/SRAM/外设地址
- 入口：[04_core_architecture_memory_map/README.md](04_core_architecture_memory_map/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 05_systick

- 课题：SysTick 毫秒节拍
- Demo：SysTick 中断产生 1ms 计数并翻转 LED
- 入口：[05_systick/README.md](05_systick/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 06_timer_base

- 课题：定时器基础
- Demo：TIM2 周期更新中断翻转 LED
- 入口：[06_timer_base/README.md](06_timer_base/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 07_timer_output_compare

- 课题：TIM 输出比较
- Demo：TIM2_CH1 在 CCR1 匹配时自动翻转 PA0
- 入口：[07_timer_output_compare/README.md](07_timer_output_compare/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 08_pwm_basic

- 课题：PWM 基础
- Demo：TIM PWM 输出固定频率和占空比
- 入口：[08_pwm_basic/README.md](08_pwm_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 09_pwm_advanced

- 课题：PWM 进阶
- Demo：动态更新 CCR 形成呼吸灯
- 入口：[09_pwm_advanced/README.md](09_pwm_advanced/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 10_exti

- 课题：外部中断 EXTI
- Demo：PA0 按键中断切换 LED
- 入口：[10_exti/README.md](10_exti/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 11_input_capture

- 课题：输入捕获
- Demo：TIM 捕获输入边沿时间戳并计算周期
- 入口：[11_input_capture/README.md](11_input_capture/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 12_timer_pwm_input

- 课题：TIM PWM 输入模式
- Demo：TIM3 同时测周期和高电平时间
- 入口：[12_timer_pwm_input/README.md](12_timer_pwm_input/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 13_timer_encoder

- 课题：TIM 编码器接口
- Demo：旋转编码器让 TIM3 计数增减
- 入口：[13_timer_encoder/README.md](13_timer_encoder/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 14_timer_advanced_tim1

- 课题：TIM1 高级定时器
- Demo：TIM1_CH1 输出 PWM 并解释 MOE/BDTR
- 入口：[14_timer_advanced_tim1/README.md](14_timer_advanced_tim1/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 15_adc_polling

- 课题：ADC 轮询采样
- Demo：轮询读取 PA0 模拟电压并改变 LED 节奏
- 入口：[15_adc_polling/README.md](15_adc_polling/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 16_adc_interrupt

- 课题：ADC 中断采样
- Demo：EOC 中断读取 ADC 值
- 入口：[16_adc_interrupt/README.md](16_adc_interrupt/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 17_adc_multichannel_scan

- 课题：ADC 多通道扫描
- Demo：按规则序列采样 PA0/PA1
- 入口：[17_adc_multichannel_scan/README.md](17_adc_multichannel_scan/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 18_dma_basic

- 课题：DMA 基础
- Demo：DMA 自动搬运 ADC 单值到内存
- 入口：[18_dma_basic/README.md](18_dma_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 19_dma_memory_uart_cases

- 课题：DMA 内存搬运与 UART 发送
- Demo：内存复制后用 DMA 发送 USART1 字符串
- 入口：[19_dma_memory_uart_cases/README.md](19_dma_memory_uart_cases/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 20_adc_dma

- 课题：ADC + DMA 缓冲区
- Demo：ADC 连续采样进入循环缓冲并计算平均值
- 入口：[20_adc_dma/README.md](20_adc_dma/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 21_uart_polling

- 课题：UART 轮询收发
- Demo：USART1 周期打印或回显
- 入口：[21_uart_polling/README.md](21_uart_polling/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 22_uart_interrupt

- 课题：UART 中断接收
- Demo：RXNE 中断接收字符控制 LED
- 入口：[22_uart_interrupt/README.md](22_uart_interrupt/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 23_uart_dma

- 课题：UART + DMA
- Demo：DMA 发送整块串口数据
- 入口：[23_uart_dma/README.md](23_uart_dma/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 24_uart_printf_redirect

- 课题：USART printf 重定向
- Demo：printf 通过 USART1 PA9 输出
- 入口：[24_uart_printf_redirect/README.md](24_uart_printf_redirect/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 25_uart_packet_protocol

- 课题：USART 数据包协议
- Demo：解析帧头帧尾数据包控制 LED
- 入口：[25_uart_packet_protocol/README.md](25_uart_packet_protocol/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 26_i2c_basic

- 课题：I2C 基础
- Demo：I2C 读写 EEPROM 或地址探测
- 入口：[26_i2c_basic/README.md](26_i2c_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 27_i2c_software_eeprom

- 课题：软件 I2C 与 EEPROM
- Demo：PB6/PB7 模拟 I2C 写 AT24C02
- 入口：[27_i2c_software_eeprom/README.md](27_i2c_software_eeprom/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 28_oled_ssd1306

- 课题：OLED SSD1306 显示屏
- Demo：I2C OLED 显示测试图案
- 入口：[28_oled_ssd1306/README.md](28_oled_ssd1306/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 29_i2c_mpu6050

- 课题：I2C 读写 MPU6050
- Demo：读取 WHO_AM_I 并唤醒 MPU6050
- 入口：[29_i2c_mpu6050/README.md](29_i2c_mpu6050/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 30_spi_basic

- 课题：SPI 基础
- Demo：SPI1 主机回环收发
- 入口：[30_spi_basic/README.md](30_spi_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 31_spi_w25q64

- 课题：SPI 读写 W25Q64
- Demo：读取 JEDEC ID
- 入口：[31_spi_w25q64/README.md](31_spi_w25q64/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 32_can_basic

- 课题：CAN 基础
- Demo：CAN 回环发送接收标准帧
- 入口：[32_can_basic/README.md](32_can_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 33_nvic_priority

- 课题：NVIC 优先级
- Demo：两个中断源演示抢占关系
- 入口：[33_nvic_priority/README.md](33_nvic_priority/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 34_watchdog_iwdg

- 课题：独立看门狗 IWDG
- Demo：按键决定是否喂狗，观察复位
- 入口：[34_watchdog_iwdg/README.md](34_watchdog_iwdg/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 35_watchdog_wwdg

- 课题：窗口看门狗 WWDG
- Demo：在窗口期喂狗，否则复位
- 入口：[35_watchdog_wwdg/README.md](35_watchdog_wwdg/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 36_bkp_backup_register

- 课题：BKP 备份寄存器
- Demo：复位后保留计数或标志
- 入口：[36_bkp_backup_register/README.md](36_bkp_backup_register/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 37_rtc_basic

- 课题：RTC 基础
- Demo：RTC 秒计数驱动 LED 或串口输出
- 入口：[37_rtc_basic/README.md](37_rtc_basic/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 38_flash_internal

- 课题：内部 Flash 读写
- Demo：擦除页并写入配置值
- 入口：[38_flash_internal/README.md](38_flash_internal/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 39_low_power_sleep

- 课题：Sleep 低功耗
- Demo：WFI 进入 Sleep 并由中断唤醒
- 入口：[39_low_power_sleep/README.md](39_low_power_sleep/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 40_low_power_stop_standby

- 课题：Stop/Standby 低功耗
- Demo：PA0 唤醒停止/待机模式
- 入口：[40_low_power_stop_standby/README.md](40_low_power_stop_standby/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 41_debug_toolchain

- 课题：调试工具链与断点观察
- Demo：用断点/watch 观察变量和寄存器变化
- 入口：[41_debug_toolchain/README.md](41_debug_toolchain/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 42_fsmc_sram

- 课题：FSMC SRAM 教学模拟
- Demo：用可编译数组模拟外部 SRAM 读写校验
- 入口：[42_fsmc_sram/README.md](42_fsmc_sram/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 43_tft_lcd_fsmc

- 课题：FSMC TFT LCD 教学模拟
- Demo：用 framebuffer 模拟 TFT 写命令/数据和画矩形
- 入口：[43_tft_lcd_fsmc/README.md](43_tft_lcd_fsmc/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 44_freertos_intro_porting

- 课题：FreeRTOS 入门、移植与基础调度
- Demo：两个任务以不同周期翻转 LED
- 入口：[44_freertos_intro_porting/README.md](44_freertos_intro_porting/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 45_freertos_task_create_delete

- 课题：任务创建与删除
- Demo：周期创建 worker 任务，worker 闪烁后删除自己
- 入口：[45_freertos_task_create_delete/README.md](45_freertos_task_create_delete/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 46_freertos_task_suspend_resume

- 课题：任务挂起与恢复
- Demo：控制任务挂起/恢复 LED 任务
- 入口：[46_freertos_task_suspend_resume/README.md](46_freertos_task_suspend_resume/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 47_freertos_scheduler_time_slice

- 课题：调度器、抢占与时间片
- Demo：同优先级任务轮转，高优先级周期抢占
- 入口：[47_freertos_scheduler_time_slice/README.md](47_freertos_scheduler_time_slice/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 48_freertos_time_management

- 课题：FreeRTOS 时间管理
- Demo：对比 vTaskDelay 和 vTaskDelayUntil
- 入口：[48_freertos_time_management/README.md](48_freertos_time_management/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 49_freertos_interrupt_critical

- 课题：中断管理与临界区
- Demo：EXTI0 用 FromISR 投递队列
- 入口：[49_freertos_interrupt_critical/README.md](49_freertos_interrupt_critical/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 50_freertos_queue

- 课题：FreeRTOS 队列
- Demo：按键任务发送事件，LED 任务阻塞接收
- 入口：[50_freertos_queue/README.md](50_freertos_queue/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 51_freertos_semaphore_binary_counting

- 课题：二值信号量与计数信号量
- Demo：二值信号量触发 LED，计数信号量限制资源
- 入口：[51_freertos_semaphore_binary_counting/README.md](51_freertos_semaphore_binary_counting/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 52_freertos_mutex_priority_inversion

- 课题：互斥锁与优先级翻转
- Demo：mutex 保护共享资源并演示优先级继承
- 入口：[52_freertos_mutex_priority_inversion/README.md](52_freertos_mutex_priority_inversion/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 53_freertos_queue_set

- 课题：队列集 Queue Set
- Demo：一个任务同时等待两个队列
- 入口：[53_freertos_queue_set/README.md](53_freertos_queue_set/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 54_freertos_event_group

- 课题：事件组 Event Group
- Demo：等待两个任务都置位后翻转 LED
- 入口：[54_freertos_event_group/README.md](54_freertos_event_group/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 55_freertos_task_notification

- 课题：任务通知
- Demo：任务通知替代轻量信号量
- 入口：[55_freertos_task_notification/README.md](55_freertos_task_notification/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 56_freertos_software_timer

- 课题：软件定时器
- Demo：timer service task 周期回调翻转 LED
- 入口：[56_freertos_software_timer/README.md](56_freertos_software_timer/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 57_freertos_memory_management

- 课题：内存管理与栈水位
- Demo：读取栈剩余水位并检查任务创建返回值
- 入口：[57_freertos_memory_management/README.md](57_freertos_memory_management/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 58_freertos_low_power_tickless

- 课题：FreeRTOS Tickless 低功耗
- Demo：空闲期关闭周期 Tick 并执行 WFI
- 入口：[58_freertos_low_power_tickless/README.md](58_freertos_low_power_tickless/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 59_freertos_uart

- 课题：FreeRTOS + UART 中断
- Demo：串口中断收字节，通过队列交给任务处理
- 入口：[59_freertos_uart/README.md](59_freertos_uart/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 60_freertos_adc_dma

- 课题：FreeRTOS + ADC DMA
- Demo：ADC DMA 后台采样，任务处理缓冲区
- 入口：[60_freertos_adc_dma/README.md](60_freertos_adc_dma/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 61_small_system_project

- 课题：小型系统综合项目
- Demo：按键、串口、ADC、LED/显示组合成小系统
- 入口：[61_small_system_project/README.md](61_small_system_project/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 62_lcd_12864

- 课题：LCD12864 显示
- Demo：驱动 LCD12864 显示字符或图案
- 入口：[62_lcd_12864/README.md](62_lcd_12864/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 63_bootloader_iap

- 课题：Bootloader 基础
- Demo：Bootloader 3 秒窗口后跳转到 0x08008000 的 app
- 入口：[63_bootloader_iap/README.md](63_bootloader_iap/README.md)
- 工程：`reg/` + `hal/`，每个目录含 `bootloader` / `app` 两个 PlatformIO 环境
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 64_uart_ota

- 课题：串口 OTA 升级
- Demo：Python 上位机分包发送固件，Bootloader 写 Flash、CRC32 校验后跳转
- 入口：[64_uart_ota/README.md](64_uart_ota/README.md)
- 工程：`reg/` + `hal/`，每个目录含 `bootloader` / `app` 两个环境与 `tools/ota_upload.py`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 65_can_advanced

- 课题：CAN 进阶
- Demo：双过滤器 + FIFO0/FIFO1 中断 + 远程帧回应 + Bus-Off 监测
- 入口：[65_can_advanced/README.md](65_can_advanced/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 66_lora_sx1278

- 课题：LoRa SX1278 通信
- Demo：SPI 配置 SX1278，周期发送 PING，收到 PING 回 PONG 并打印 RSSI/SNR
- 入口：[66_lora_sx1278/README.md](66_lora_sx1278/README.md)
- 工程：`reg/` + `hal/`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

### 67_multichannel_ota

- 课题：三通道 OTA
- Demo：UART/CAN/LoRa 任一通道接收固件，统一协议写入 app 区并跳转
- 入口：[67_multichannel_ota/README.md](67_multichannel_ota/README.md)
- 工程：`reg/` + `hal/`，每个目录含 `bootloader` / `app` 两个环境与 `tools/ota_upload.py`
- 状态：已纳入 00-67 主线，按 `TEACHING_PROMPT.md` 维护。

## 阶段划分

- 00-04：环境、GPIO、时钟、内核地址空间。
- 05-14：SysTick、定时器、PWM、捕获、编码器、高级定时器。
- 15-20：ADC、DMA、ADC DMA。
- 21-32：UART、I2C、SPI、CAN 和外设模块。
- 33-43：NVIC、看门狗、RTC、Flash、低功耗、调试、FSMC/LCD。
- 44-60：FreeRTOS 从移植到同步通信、内存和 tickless。
- 61-62：综合项目与 LCD12864。
- 63-67：Bootloader 分区跳转、串口 OTA、CAN 进阶、LoRa SX1278、三通道 OTA。
