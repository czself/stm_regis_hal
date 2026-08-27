# 61_small_system_project - FreeRTOS 小系统综合项目

## 1. 本课到底在学什么

本课表面现象是：系统启动后串口输出 `system ready`，每 2 秒打印一次 `adc=数值`；按下 PB0 会翻转 PC13 并打印 `key`；串口发送 `t/T` 会触发 `toggle` 事件并翻转 LED，发送 `s/S` 会立即打印当前 ADC 值。

真正要学的是 FreeRTOS 小系统架构：ADC、按键、UART、LED 不再塞进一个 while，而是拆成 adc_task、key_task、uart_task、control_task、status_task，事件通过队列流动。

本课继续按六层拆解：现象层看板子和串口输出，硬件层看引脚和连接，芯片模块层看 ADC/DMA/UART/GPIO/NVIC，寄存器层看控制位和状态位，C/CMSIS 层看源码语句，HAL/工程层看结构体字段、回调和返回值；FreeRTOS 部分还要解释任务、队列、通知、阻塞和 ISR 边界。

## 2. 本课学习目标

- 能说出现象来自哪条执行链路。
- 能解释每个新对象属于硬件层、寄存器层还是 RTOS 层。
- 能把寄存器版语句和 HAL 字段对应起来。
- 能根据具体现象排查时钟、GPIO、外设、DMA、中断和任务。
- 能说明 ISR 里为什么只做短动作。
- 能说明任务为什么阻塞等待而不是轮询。
- 能看懂源码里的缓冲区、句柄和返回值风险。
- 能解释为什么多任务共享 UART 发送需要互斥锁。
- 能把本课和前后课程的边界分清楚。

## 3. 本课目录结构

```text
61_small_system_project/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个版本都要读。寄存器版让你看到 RCC、GPIO、ADC、DMA、USART 等寄存器怎样配合；HAL 版让你看到同一件事如何变成 handle、Init 结构体、回调和封装 API。

## 4. 实验硬件与工程前提

- PB0 上拉按键，按下为低。
- PA0 ADC 模拟输入。
- PA9/PA10 USART1 115200 8N1。
- PC13 LED。
- g_event_queue 长度 8，传事件号。
- g_uart_queue 长度 32，传串口字节。
- g_uart_mutex 保护串口发送，避免多任务并发时字节交错。

## 5. 先建立一个最基本的脑图

```text
ADC/按键/串口输入
  -> adc_task 更新 g_adc_value
  -> key_task 投递 EVENT_KEY
  -> USART1 ISR 投递字节到 g_uart_queue
  -> uart_task 解析 t/s 命令
  -> control_task 消费事件翻 LED 打印
  -> status_task 周期打印 ADC
  -> g_uart_mutex 保证多任务串口输出不交错
```

先用这张图记住数据从哪里来、经过哪个中断或任务、最后落到哪个输出。后面所有寄存器和 API 都应该能放回这张图里。

## 6. 先认识本课里出现的核心名词

### 6.1 `g_event_queue` 是什么

系统事件队列，元素是 uint8_t 事件号。
control_task 阻塞等待它。

### 6.2 `g_uart_queue` 是什么

串口字节队列，ISR 写，uart_task 读。
它把中断和解析解耦。

### 6.3 `EVENT_KEY` 是什么

按键按下事件。
key_task 检测上升边沿后投递。

### 6.4 `EVENT_UART_TOGGLE` 是什么

串口 t/T 命令转成的控制事件。
uart_task 不直接翻 LED，而是交给 control_task。

### 6.5 `adc_task` 是什么

每 100ms 采样 PA0。
写入共享变量 g_adc_value。

### 6.6 `key_task` 是什么

每 20ms 轮询 PB0。
用 last/now 做简单边沿检测。

### 6.7 `control_task` 是什么

系统控制中心。
收到事件后翻转 LED 并打印日志。

### 6.8 `uart_task` 是什么

解析串口命令。
t/T 投事件，s/S 打印 ADC。

### 6.9 `status_task` 是什么

低优先级周期状态输出。
每 2 秒打印 ADC。

### 6.10 `volatile g_adc_value` 是什么

ADC 当前值共享变量。
被 adc_task 写，被 uart/status 读。

### 6.11 `USART1_IRQHandler` 是什么

接收字节 ISR。
只入队，不解析。

### 6.12 `portMAX_DELAY` 是什么

control/uart 任务无事件时一直阻塞。
节省 CPU。

### 6.13 `队列长度 8/32` 是什么

事件队列缓存 8 个事件，UART 队列缓存 32 字节。
突发过大可能丢。

### 6.14 `任务优先级` 是什么

adc/key/control/uart 为 2，status 为 1。
状态输出让位给交互任务。

### 6.15 `共享变量原子性` 是什么

uint16_t 在 Cortex-M3 上读写通常原子。
复杂结构要 mutex。

### 6.16 `g_uart_mutex` 是什么

`g_uart_mutex` 是一把互斥锁（mutex），用来保护 USART1 发送。

`control_task`、`uart_task`、`status_task` 都会调用 `uart_write_str()` 或 `uart_write_u16()` 往串口输出。如果两个任务同时发送，后启动的任务可能在另一个任务发送中途抢占 CPU，导致两串字符字节交错，例如 `key` 和 `adc=1234\r\n` 混在一起变成 `kaeydc=1234\r\n`。互斥锁保证一次完整字符串或数字在发送期间不会被其他任务打断。

这把锁对应 52 课讲的互斥量：创建用 `xSemaphoreCreateMutex()`，拿锁用 `xSemaphoreTake(..., portMAX_DELAY)`，释放用 `xSemaphoreGive()`。它和二值信号量看起来相似，但语义是“资源所有权”——谁拿谁释放，并且 FreeRTOS 会启用优先级继承，防止高优先级任务被低优先级持有者长期阻塞。

如果忘了加锁，系统大部分时候可能正常输出，但在任务切换恰好落在发送中途时会出现随机乱码；如果创建失败（例如 heap 不足），`g_uart_mutex` 为 NULL，初始化检查会让程序停在 `stop_for_debug()`。

## 7. 寄存器版代码逐步讲解

### 7.1 时钟和 ADC 分频

RCC 同时配置 72MHz 和 ADCPRE_DIV6。
ADC 需要合规时钟。

### 7.2 GPIO 初始化

PC13 输出，PB0 上拉输入，PA0 模拟输入。
每个引脚对应一个系统角色。

### 7.3 USART1 初始化

PA9/PA10、BRR=0x0271、RXNEIE。
接收通过中断进入队列。

### 7.4 ADC 单次读取

adc1_read 启动转换，等 EOC，读 DR。
这是轮询式 ADC，不用 DMA。

### 7.5 按键边沿

last/now 防止一直按住重复投递。
20ms 周期兼顾消抖和响应。

### 7.6 事件队列

key 和 uart_task 都向 g_event_queue 发送。
control_task 统一处理输出。

### 7.7 UART 队列

ISR 读 DR 后 xQueueSendFromISR。
解析在 uart_task。

### 7.8 打印函数与 UART 互斥锁

`uart_write_str()` 和 `uart_write_u16()` 是同步发送。调用前必须先拿到 `g_uart_mutex`，发送完再释放。这样即使 `control_task`、`uart_task`、`status_task` 并发调用，串口输出的每一串字符也是完整的，不会出现字节交错。

这把锁不是保护“某个变量”，而是保护“USART1 这个硬件资源在同一时刻只被一个任务使用”。这是 52 课互斥量思想的实际应用。

### 7.9 任务创建与内核对象创建

五个任务代表五个职责。当前源码已检查两个队列、一把互斥锁和五个任务的创建返回值，任一失败会停在 `stop_for_debug()`。

### 7.10 共享 ADC 值

g_adc_value 被多个任务访问。
简单 uint16_t 可以接受。

## 8. HAL 版代码逐步讲解

### 8.1 HAL GPIO/ADC/UART

HAL 版用结构体配置相同硬件。
任务和队列 API 不变。

### 8.2 HAL_ADC_Start/Poll/Get/Stop

adc_task 每 100ms 启动、等待、读取、停止。
对应寄存器版 ADON/EOC/DR。

### 8.3 HAL_UART_Receive_IT

uart_task 启动第一次接收。
回调里重启下一字节。

### 8.4 HAL_UART_RxCpltCallback

回调入队 g_rx_byte。
再调用 Receive_IT。

### 8.5 HAL_UART_Transmit 与 UART 互斥锁

任务中同步打印字符串和数字。由于 `control_task`、`uart_task`、`status_task` 都可能并发调用 `uart_write_str()` 或 `uart_write_u16()`，它们内部先用 `xSemaphoreTake(g_uart_mutex, ...)` 拿锁，再调用 `HAL_UART_Transmit()`，发送完再 `xSemaphoreGive()`。这样保证一次完整输出不会被打断。

注意 `HAL_UART_Transmit()` 本身不是线程安全的；在 RTOS 环境里多个任务共享同一个 UART handle 时，必须外加同步机制。这里选择互斥锁而不是二值信号量，是因为互斥量有优先级继承，能缓解高优先级任务等低优先级持有者释放资源时的优先级反转问题。

### 8.6 HAL_GPIO_TogglePin

control_task 翻转 PC13。
对应寄存器版 led_toggle。

### 8.7 HAL 栈更大

uart_task 栈 224。
HAL 调用链更深。

### 8.8 SysTick 说明

FreeRTOS 接管 SysTick 提供系统节拍，HAL 延时不依赖它。
要保证工程 FreeRTOS 配置一致。

## 9. 两个版本真正应该怎么学

寄存器版和 HAL 版做的是同一个系统。寄存器版适合看清时钟、引脚模式、状态标志、DMA 通道映射和中断清标志；HAL 版适合看清工程结构、handle 关联、回调入口和错误处理。

不要只背 API 名称。每看到一个 API，都要问它最终配置了哪个外设、影响了哪个数据流、失败后现象是什么。这样读，后面遇到综合项目才不会散。

## 10. 检验问题清单

### 10.1 为什么要多个任务？

**答**：按职责拆分，降低耦合，让输入、处理、输出更清楚。

### 10.2 为什么有两个队列？

**答**：UART 字节和系统事件语义不同，分开更清楚。

### 10.3 为什么 ISR 不解析命令？

**答**：解析和打印耗时，应放任务。

### 10.4 status_task 为什么优先级低？

**答**：周期打印不应影响按键和串口响应。

### 10.5 按键为什么要 last？

**答**：检测按下边沿，避免按住时重复触发。

### 10.6 s 命令做什么？

**答**：立即打印当前 ADC 值。

### 10.7 t 命令为什么不直接翻 LED？

**答**：统一由 control_task 处理控制输出。

### 10.8 共享 ADC 值安全吗？

**答**：本例 uint16_t 简单读写可接受，复杂数据要保护。

### 10.9 无串口输出先查什么？

**答**：查接线、波特率、USART 初始化、任务是否运行。

### 10.10 为什么要给 UART 发送加互斥锁？

**答**：`control_task`、`uart_task`、`status_task` 都会调用打印函数。如果不加锁，一个任务发送中途被另一个任务抢占，两串字符可能字节交错。互斥锁保证一次完整字符串或数字发送不被打断，同时互斥量的优先级继承机制可以缓解优先级反转。

### 10.11 队列满会怎样？

**答**：xQueueSend 可能失败，事件或字节丢失。

## 11. 工程实现步骤

### 11.1 需求分析

先定义输入、处理和输出，不急着写代码。输入可能是 ADC、按键、串口或 DMA；处理可能在任务或回调；输出可能是 LED、串口日志或显示屏。

### 11.2 硬件核查

确认引脚、接线、电源、共地、模拟输入范围和串口参数。ADC 输入不能超过供电范围，串口 TX/RX 要交叉，LCD 还要确认模式脚和对比度。

### 11.3 寄存器路线

按 RCC 时钟、GPIO 模式、外设参数、DMA/NVIC、任务或主循环的顺序搭起来。每一步都能对应到源码里的具体寄存器或宏。

### 11.4 HAL 路线

用 HAL 结构体表达同样的硬件选择，再通过 Init、Start、Callback 把运行链路串起来。HAL 版也要检查返回值和句柄关联。

### 11.5 工程思维

中断和 DMA 只负责搬运或通知，复杂计算、打印、解析、显示刷新放在任务或主循环里。共享数据要明确谁写谁读。

### 11.6 常见工程陷阱

常见问题包括 include 路径错、对象创建失败不查、DMA 通道映射错、中断优先级不合规、串口波特率不匹配、HAL 回调没重启接收、缓冲区长度和协议长度不一致。

## 12. 运行现象

本课表面现象是：系统启动后串口输出 `system ready`，每 2 秒打印一次 `adc=数值`；按下 PB0 会翻转 PC13 并打印 `key`；串口发送 `t/T` 会触发 `toggle` 事件并翻转 LED，发送 `s/S` 会立即打印当前 ADC 值。

### 12.1 六层对应关系

现象不是凭空出现的：硬件引脚提供输入输出，芯片外设采样或传输，寄存器决定模式，C 代码组织流程，HAL 封装工程写法，RTOS 层决定任务何时运行。

### 12.2 推荐断点

在初始化结束处、ISR 或回调入口、任务解除阻塞处、最终输出函数处打断点。四个点能把问题分成初始化失败、数据没到、中断没触发、任务没醒、输出没执行。

### 12.3 配置核查

重点核查 `platformio.ini` 的 include 路径、时钟宏、monitor_speed、FreeRTOS 配置以及本课本地配置。配置错时，源码看起来对也会编译或运行失败。

## 13. 常见问题排查

### 13.1 没有 system ready

查 uart_task 是否创建运行，USART TX 接线和波特率。

### 13.2 按键无反应

查 PB0 上拉、按下电平、key_task 周期。

### 13.3 t 不翻 LED

查 UART 接收、g_uart_queue、uart_task 解析和事件队列。

### 13.4 s 不打印 ADC

查 uart_task 解析和 g_adc_value 是否更新。

### 13.5 ADC 一直 0

查 PA0 输入、ADC 时钟和采样函数。

### 13.6 打印卡顿

同步 UART 发送会占用任务时间，减少打印或改异步。

### 13.7 事件丢失

队列满或发送返回失败未处理。

### 13.8 进入 hook

检查五个任务栈和 heap。

## 14. 本课最核心的结论

1. 小系统要先设计数据流。
2. ISR 只搬运字节，任务解析命令。
3. 事件队列让控制输出集中。
4. UART 队列让串口接收和解析解耦。
5. UART 发送是多任务共享资源，要用互斥锁保护，避免字节交错。
6. 低优先级状态任务不能压过交互任务。
7. 共享变量简单场景可用 volatile，复杂场景要互斥。
8. 源码应进一步补返回值检查。
9. 综合项目排错要按数据流逐段断点。

## 15. 建议你现在怎么读这节课

先看第 5 章脑图，再读第 6 章名词。第 7、8 章要对着源码读，不要脱离代码背概念。最后用第 13 章做反向排查训练。

## 16. 扩展练习

1. 改变缓冲区长度或周期，观察现象变化。
2. 故意断开一个初始化步骤，记录失败现象。
3. 在 ISR/回调和任务中各加计数器，观察数据流。
4. 把寄存器版和 HAL 版同一动作写成对应表。
5. 增加返回值检查，让失败路径更清楚。

## 17. 下一课预告

- 上一课：[60_freertos_adc_dma](../60_freertos_adc_dma/README.md)
- 下一课：[62_lcd_12864](../62_lcd_12864/README.md)


### 12.4 源码复盘点

复盘时不要只看现象，要把本节源码中的全局变量、句柄、缓冲区长度、IRQ 名称和最终输出函数逐项对应。如果其中一个名字说不出作用，就回到第 6 章重新定位它属于哪一层。
