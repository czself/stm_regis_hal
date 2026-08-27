# 59_freertos_uart - FreeRTOS UART 中断队列

## 1. 本课到底在学什么

本课表面现象是：电脑串口助手发给 USART1 的字节会被板子回显回来；如果发送 `t` 或 `T`，PC13 会翻转一次。这个现象说明 UART 接收中断、FreeRTOS 队列、串口任务和 GPIO 输出串成了一条完整链路。

真正要学的是“中断只收字节，任务处理业务”的工程模式。USART1 ISR 读取接收字节后，用 `xQueueSendFromISR()` 投递到 `g_uart_queue`；`uart_task` 阻塞等待队列，收到字节后回显并判断是否翻转 LED。HAL 版把底层 RXNE 处理交给 `HAL_UART_IRQHandler()` 和 `HAL_UART_RxCpltCallback()`，但队列和任务分工不变。

本课继续按六层来拆：现象层看回显和 PC13，硬件层看 PA9/PA10/PC13，芯片模块层看 USART1、GPIO、NVIC，寄存器层看 BRR、CR1、SR、DR，C/CMSIS 层看 ISR 和队列 API，HAL/工程层看 UART handle、接收回调和重新启动接收。

## 2. 本课学习目标

- 能解释 USART1 的 PA9/PA10 如何连接到电脑串口。
- 能说明 RXNE、TXE、DR、BRR、CR1 各自负责什么。
- 能解释为什么 ISR 里只能做短动作。
- 能正确说明 `xQueueSendFromISR()` 和 `portYIELD_FROM_ISR()`。
- 能解释队列长度 32、元素大小 1 字节的意义。
- 能说明 `uart_task` 为什么用 `portMAX_DELAY` 阻塞等待。
- 能解释 HAL 版为什么必须重新 `HAL_UART_Receive_IT()`。
- 能根据无回显、乱码、只收一次、LED 不翻等现象排查。

## 3. 本课目录结构

```text
59_freertos_uart/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个工程的串口参数都应和 `platformio.ini` 的 `monitor_speed = 115200` 对齐。`reg/` 直接写 USART1 寄存器，`hal/` 通过 HAL UART handle 管理同一个外设。

## 4. 实验硬件与工程前提

- STM32F103C8T6 BluePill。
- USART1_TX：PA9，连接 USB-TTL RX。
- USART1_RX：PA10，连接 USB-TTL TX。
- GND 必须共地。
- PC13：收到 t/T 后翻转。
- 串口参数：115200、8 数据位、无校验、1 停止位。
- USART1 IRQ 优先级：6。
- 队列：32 个 `uint8_t` 元素。

串口课同时跨硬件接线、外设寄存器、中断优先级和 RTOS 队列。任何一层错了，现象都可能只是“没回显”，所以文档必须把每一层拆开。

## 5. 先建立一个最基本的脑图

```text
电脑串口助手发送字节
  -> USB-TTL 把字节变成 PA10 上的串口波形
  -> USART1 接收完成，SR.RXNE 置位
  -> NVIC 进入 USART1_IRQHandler
  -> ISR 读取 DR 得到 byte
  -> xQueueSendFromISR(g_uart_queue, &byte, &woken)
  -> portYIELD_FROM_ISR(woken)
  -> uart_task 从队列取出 byte
  -> 通过 PA9 回显 byte
  -> 如果 byte 是 t/T，翻转 PC13
```

这条链路里，ISR 不解析命令、不阻塞发送、不做长循环。它只把硬件事件变成队列消息，任务再做业务。

## 6. 先认识本课里出现的核心名词

### 6.1 `USART1` 是什么

USART1 是 STM32F103 的串口外设之一，挂在 APB2 总线上。

本课用 PA9 做 TX、PA10 做 RX，让电脑串口助手和板子交换字节。

### 6.2 `PA9 TX` 是什么

PA9 被配置成复用推挽输出，用来输出 USART1 发送波形。

寄存器版设置 MODE9 和 CNF9，HAL 版设置 `GPIO_MODE_AF_PP`。

### 6.3 `PA10 RX` 是什么

PA10 被配置成输入，用来接收 USB-TTL 发来的串口波形。

寄存器版设置 CNF10 浮空输入，HAL 版设置输入无上下拉。

### 6.4 `115200 8N1` 是什么

115200 是波特率，8N1 表示 8 数据位、无校验、1 停止位。

电脑串口助手参数必须一致，否则会乱码或无回显。

### 6.5 `BRR` 是什么

BRR 是 USART 波特率寄存器。

本课寄存器版写 `USART1->BRR = 0x0271U`，这是 72MHz APB2 下常用的 115200 配置值。F1 的 BRR 编码包含整数和小数分频，不要把它当成普通十进制除数直接套错公式。

### 6.6 `CR1` 是什么

CR1 是 USART 控制寄存器 1。

本课设置 TE、RE、RXNEIE、UE，分别打开发送、接收、接收非空中断和 USART 总使能。

### 6.7 `SR.RXNE` 是什么

RXNE 是接收数据寄存器非空标志。

收到一个字节后置位，ISR 读取 DR 后该接收事件被消费。

### 6.8 `SR.TXE` 是什么

TXE 是发送数据寄存器空标志。

寄存器版发送前等待 TXE，确认可以写下一个字节到 DR。

### 6.9 `DR` 是什么

DR 是 USART 数据寄存器。

读 DR 取得收到的字节，写 DR 开始发送一个字节。

### 6.10 `USART1_IRQHandler` 是什么

它是 USART1 的中断服务函数。

寄存器版在里面读 RXNE/DR 并投递队列；HAL 版在里面调用 `HAL_UART_IRQHandler()`。

### 6.11 `xQueueSendFromISR` 是什么

这是中断上下文专用的队列发送 API。

ISR 不能阻塞等待普通队列 API，所以必须使用 FromISR 版本。

### 6.12 `portYIELD_FROM_ISR` 是什么

它根据 `woken` 决定中断退出时是否立刻切换到更高优先级任务。

本课接收字节后可能唤醒 uart_task，因此 ISR 末尾调用它。

### 6.13 `g_uart_queue` 是什么

这是 UART 接收字节队列句柄。

队列长度 32，每个元素 1 字节，用来把 ISR 收到的数据交给任务处理。

### 6.14 `uart_task` 是什么

uart_task 是串口业务任务。

它阻塞等待队列字节，收到后回显，并在字节为 t/T 时翻转 PC13。

### 6.15 `NVIC 优先级 6` 是什么

USART1 中断优先级设为 6。

在常见 FreeRTOS 配置下，调用 FromISR API 的中断优先级数字必须不高于禁止线，本课 6 符合要求。

### 6.16 `HAL_UART_Receive_IT` 是什么

它启动一次 HAL UART 中断接收。

HAL 版必须先调用一次接收，回调里还要重新调用，否则只能收到一个字节。

### 6.17 `HAL_UART_RxCpltCallback` 是什么

它是 HAL 接收完成回调。

本课在回调里把字节投递到 FreeRTOS 队列，并重新启动下一次 1 字节接收。

### 6.18 `PC13` 是什么

PC13 是串口命令反馈 LED。

收到 t 或 T 时翻转，帮助确认队列和任务处理都走通了。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

`queue.h` 提供队列句柄、创建、接收和 FromISR 发送 API。

`task.h` 提供任务创建、调度、hook 支持。

### 7.2 72MHz 时钟

USART1 挂 APB2，本课 APB2 为 72MHz。

波特率配置必须和这个时钟一致，否则串口会乱码。

### 7.3 LED 初始化

PC13 配成输出并置高。

它不是串口必须项，而是命令反馈现象。

### 7.4 USART GPIO 时钟

`RCC->APB2ENR` 打开 GPIOA 和 USART1 时钟。

没开时钟时，写 GPIOA 或 USART1 寄存器不会得到预期外设行为。

### 7.5 PA9 配复用推挽

PA9 是 USART1_TX，需要复用输出而不是普通 GPIO 输出。

寄存器版通过 CRH 的 MODE9/CNF9 组合表达。

### 7.6 PA10 配输入

PA10 是 USART1_RX，需要输入模式接收外部电平。

寄存器版设置 CNF10 浮空输入。

### 7.7 BRR 配波特率

`0x0271` 对应 72MHz 下常见 115200 USARTDIV 编码。

不要把 BRR 当成一个普通十进制除数直接套错公式。

### 7.8 CR1 打开 USART

TE 打开发送，RE 打开接收，RXNEIE 打开接收非空中断，UE 打开 USART。

少任何一个都可能导致只能收、只能发或完全无中断。

### 7.9 NVIC 设置

`NVIC_SetPriority(USART1_IRQn, 6U)` 后再 enable。

优先级 6 允许 ISR 调用 FreeRTOS FromISR API。

### 7.10 创建队列

`xQueueCreate(32, sizeof(uint8_t))` 创建 32 字节接收缓冲。

ISR 收到字节后入队，任务慢一点也能短时间缓冲。

### 7.11 uart_task 接收队列

任务用 `xQueueReceive(..., portMAX_DELAY)` 阻塞等待字节。

没有数据时任务不占 CPU，有数据时被唤醒处理。

### 7.12 uart1_write_byte

发送前等待 TXE，再写 DR。

这是同步发送一个字节，放在任务里可以接受，不应放在 ISR 里做长等待。

### 7.13 USART1_IRQHandler

ISR 判断 RXNE，读取 DR 得到 byte，然后 `xQueueSendFromISR()`。

ISR 只做短路径，解析命令和回显放到任务。

### 7.14 portYIELD_FROM_ISR

如果入队唤醒了更高优先级任务，退出中断时可以立刻调度。

这样串口响应更及时。

### 7.15 创建失败检查

源码用 `if ((g_uart_queue == NULL) || (uart_ok != pdPASS)) stop_for_debug();` 检查队列和任务创建结果。

这正是工程上应有的做法，否则 queue 为 NULL 时 ISR 投递会出问题。

## 8. HAL 版代码逐步讲解

### 8.1 HAL_Init 和时钟

HAL 版先 `HAL_Init()`，再配置 72MHz。

USART 波特率同样依赖 APB2 时钟。

### 8.2 UART_HandleTypeDef

`huart1.Instance = USART1` 绑定具体外设。

后续 HAL_UART_Init、Transmit、Receive_IT 都靠这个 handle 找到硬件。

### 8.3 GPIO 初始化

PC13 输出、PA9 复用推挽、PA10 输入无上下拉。

这些字段对应寄存器版 CRH 的模式组合。

### 8.4 HAL_NVIC_SetPriority

HAL 版同样把 USART1 IRQ 设为 6 并使能。

这一步仍要满足 FreeRTOS FromISR 规则。

### 8.5 HAL_UART_Init

配置 115200、8 数据位、1 停止位、无校验、收发模式、无硬件流控、16 倍过采样。

它会写 BRR、CR1 等 USART 寄存器。

### 8.6 第一次 HAL_UART_Receive_IT

uart_task 进入循环前先启动一次 1 字节中断接收。

没有这一步，HAL 不会开始接收中断流程。

### 8.7 HAL_UART_IRQHandler

USART1_IRQHandler 只调用 HAL 统一处理函数。

HAL 内部识别 RXNE、读取数据、维护状态并触发回调。

### 8.8 HAL_UART_RxCpltCallback

回调确认 `huart->Instance == USART1` 后，把 `g_rx_byte` 送入队列。

这是 HAL 中断层和 FreeRTOS 任务层的交界。

### 8.9 回调里重新 Receive_IT

HAL 一次 Receive_IT 完成后需要重新启动下一次接收。

如果忘记重装，通常只能收到第一个字节。

### 8.10 HAL_UART_Transmit

任务收到队列字节后同步发送回显，超时 20ms。

它在任务上下文中阻塞等待，不能直接搬到 ISR 里。

### 8.11 HAL_GPIO_TogglePin

收到 t/T 后翻转 PC13。

这证明回调入队、任务出队、命令判断三步都完成。

### 8.12 HAL 版栈更大

HAL 版 uart_task 栈为 192，比寄存器版 160 大。

HAL 调用层级更深，工程上通常要给更多栈余量。

## 9. 两个版本真正应该怎么学

寄存器版的主线是 USART1 硬件事件：PA10 收到波形，RXNE 置位，ISR 读 DR，字节进入队列；任务出队后等待 TXE，再写 DR 从 PA9 发回去。你要能把每个宏定位到这个链条上的位置。

HAL 版的主线是 HAL 状态机：`HAL_UART_Receive_IT()` 启动一次接收，USART1 中断进 `HAL_UART_IRQHandler()`，HAL 读出字节后调用 `HAL_UART_RxCpltCallback()`，回调里投递队列并重新启动接收。HAL 不是省略中断，而是把中断细节藏进 handle 和回调里。

两个版本的 RTOS 设计完全一致：中断投递队列，任务阻塞等待，业务在任务里做。这个分工比“能回显”更重要，因为后续 DMA、协议解析、命令控制都会沿用它。

## 10. 检验问题清单

### 10.1 为什么 UART 接收要用中断加队列？

**答**：中断能及时拿走 DR 中的字节，队列把字节交给任务慢慢处理，避免 ISR 里做耗时业务。

### 10.2 ISR 里为什么用 `xQueueSendFromISR`？

**答**：ISR 不能调用可能阻塞的普通队列发送 API，必须用 FromISR 版本。

### 10.3 USART1 优先级 6 有什么意义？

**答**：它满足 FreeRTOS 对可调用 FromISR API 的中断优先级限制。

### 10.4 队列长度 32 表示什么？

**答**：最多临时缓存 32 个 1 字节元素，任务处理慢时可吸收短突发输入。

### 10.5 收到 t/T 为什么翻转 PC13？

**答**：这是命令处理成功的可见反馈，说明字节从中断走到了任务。

### 10.6 寄存器版发送为什么等 TXE？

**答**：TXE 为 1 说明发送数据寄存器空，可以写入下一个字节。

### 10.7 HAL 版为什么要反复 `HAL_UART_Receive_IT`？

**答**：HAL 的一次中断接收完成后不会自动无限接收，回调里必须重新启动下一字节。

### 10.8 串口乱码先查什么？

**答**：先查电脑端 115200 8N1、共地、PA9/PA10 接线，再查 BRR 和系统时钟。

### 10.9 PC13 不翻但串口有回显说明什么？

**答**：接收、队列和发送基本正常，重点查命令字节是否为 t/T 或 PC13 GPIO。

### 10.10 源码缺少创建返回值检查有什么风险？

**答**：队列或任务创建失败后仍启动调度，ISR 或任务可能使用无效句柄。

## 11. 工程实现步骤

### 11.1 需求分析

目标是做一个最小 UART 接收中断加 FreeRTOS 队列 Demo：收到任何字节就回显，收到 t/T 就翻转 LED。回显验证收发，LED 验证命令分支。

### 11.2 硬件核查

确认 USB-TTL 的 TX 接 PA10，RX 接 PA9，GND 共地。串口助手设 115200 8N1，下载器和串口工具不要抢占同一接口。

### 11.3 寄存器路线

开 GPIOA 和 USART1 时钟，PA9 配复用推挽，PA10 配输入，写 BRR，打开 TE/RE/RXNEIE/UE，设置 NVIC 优先级，创建队列和任务，最后启动调度器。

### 11.4 HAL 路线

配置 GPIO 和 NVIC，填 `UART_HandleTypeDef`，调用 `HAL_UART_Init()`，在任务开始处启动第一次 `HAL_UART_Receive_IT()`，在接收完成回调里入队并重新启动下一次接收。

### 11.5 工程思维

ISR 负责短路径，任务负责慢路径。队列是中间缓冲，但不是无限缓冲。工程版还应检查 `xQueueCreate()`、`xTaskCreate()`、`xQueueSendFromISR()` 的返回值，并统计丢字节次数。

### 11.6 常见工程陷阱

常见陷阱包括 TX/RX 没交叉、忘记共地、波特率不匹配、HAL 版忘记重新 Receive_IT、ISR 里调用普通队列 API、IRQ 优先级不合规、任务栈不够、队列满时没有处理失败。

## 12. 运行现象

串口助手发送任意字符，板子应回显同一个字符。发送 `t` 或 `T` 时，PC13 翻转一次。连续发送字符时，如果任务处理跟得上，回显顺序应和发送顺序一致。

如果无回显，不要只怀疑代码；先查接线和串口参数，再查 USART1 初始化、中断是否进入、队列是否创建、任务是否运行。

## 13. 常见问题排查

### 13.1 串口完全无回显

检查 USB-TTL 是否共地，TX/RX 是否交叉连接。

再查 USART1 时钟、PA9/PA10 模式和 NVIC 是否使能。

### 13.2 串口乱码

确认电脑端 115200、8N1。

检查系统时钟是否真为 72MHz，BRR 是否对应该时钟。

### 13.3 只能收到第一个字节

HAL 版重点查回调里是否重新 `HAL_UART_Receive_IT()`。

寄存器版则查 RXNEIE 是否持续打开。

### 13.4 PC13 不随 t/T 翻转

确认发送的是 ASCII 字符 t 或 T。

若回显正常，重点查 PC13 GPIO；若无回显，先查接收链路。

### 13.5 高频输入后丢字节

队列长度只有 32，任务回显是阻塞发送。

输入过快时 ISR 入队可能失败，工程上应检查返回值或加大缓冲。

### 13.6 进入 FreeRTOS 断言或异常

检查 USART1 IRQ 优先级是否符合 FromISR API 规则。

优先级数字过小可能越过 FreeRTOS 管理边界。

### 13.7 任务不运行

检查 `xTaskCreate()` 返回值和 heap。

当前源码没有严格检查，建议调试时在创建后看句柄和返回值。

### 13.8 HAL 发送阻塞太久

`HAL_UART_Transmit()` 是同步发送。

大量数据应改成中断发送、DMA 或发送队列。

### 13.9 寄存器版 RXNE 一直异常

读 DR 是消费接收事件的重要步骤。

若不读 DR，RXNE 不会按预期清除。

### 13.10 调试时偶发卡住

中断、队列、任务三层都有状态。

分别在 ISR 入队、任务出队、发送函数处打断点定位。

## 14. 本课最核心的结论

1. UART 接收中断负责及时拿走硬件字节。
2. 队列负责把 ISR 字节交给任务。
3. 任务负责回显、命令判断和 LED 控制。
4. ISR 中必须使用 FromISR API。
5. USART1 IRQ 优先级 6 是为了符合 FreeRTOS 调用规则。
6. HAL 版必须在回调里重新启动下一次接收。
7. 串口乱码大多先查波特率、时钟和接线。
8. 源码当前缺少创建返回值检查，工程化时应补上。

## 15. 建议你现在怎么读这节课

先用第 5 章脑图记住“中断入队、任务出队”。再看第 6 章，把 USART1 的硬件名词和 FreeRTOS 的队列名词分开，不要混成一团。

读寄存器版时盯住 SR/DR/BRR/CR1；读 HAL 版时盯住 handle、Receive_IT、IRQHandler、RxCpltCallback。最后用串口助手实际发 t/T，看 PC13 是否跟着任务处理结果变化。

## 16. 扩展练习

1. 把队列长度从 32 改小，快速发送一串字符，观察是否丢字节。
2. 增加一个丢字节计数器，记录 `xQueueSendFromISR()` 失败次数。
3. 把回显改为只回显大写字母，练习任务内解析。
4. HAL 版故意去掉回调里的重新 Receive_IT，观察只能收一次的现象。
5. 把同步发送改成发送队列，为后续 UART TX 中断或 DMA 做准备。

## 17. 下一课预告

- 上一课：[58_freertos_low_power_tickless](../58_freertos_low_power_tickless/README.md)
- 下一课：[60_freertos_adc_dma](../60_freertos_adc_dma/README.md)
