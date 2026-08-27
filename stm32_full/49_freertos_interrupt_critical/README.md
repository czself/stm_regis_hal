# 49_freertos_interrupt_critical - FreeRTOS 中断与临界区

## 1. 本课到底在学什么

本课表面现象是：按下 PA0 输入触发 EXTI0 中断后，任务收到一个事件，`g_shared` 计数增加，PC13 翻转一次。寄存器版在 `EXTI0_IRQHandler()` 里直接清 EXTI 标志并向队列发送事件；HAL 版先进入 `HAL_GPIO_EXTI_IRQHandler()`，再在 `HAL_GPIO_EXTI_Callback()` 里发送事件。

真正要学的是 FreeRTOS 中断和任务之间的安全交接。中断里不能直接做耗时业务，也不能随便调用普通阻塞 API；本课用 `xQueueSendFromISR()` 把一个 `uint8_t` 事件从 EXTI0 中断送进队列，任务用 `xQueueReceive(..., portMAX_DELAY)` 阻塞等待，收到事件后在临界区里修改共享变量。

这节课接在时间管理之后。前几课任务之间的调度都发生在任务上下文里，本课第一次把“外部硬件事件 -> Cortex-M 中断 -> FreeRTOS ISR API -> 队列 -> 任务唤醒 -> 临界区处理 -> GPIO 反馈”串成完整链路。后面的信号量、事件组、任务通知、UART/ADC DMA 都会反复用到这个模式。

## 2. 本课学习目标

学完本课你应该能做到：

- 能画出 PA0 下降沿如何进入 EXTI0，再如何唤醒 `event_task`。
- 能解释为什么中断里使用 `xQueueSendFromISR()`，而不是 `xQueueSend()`。
- 能解释 `BaseType_t w` 和 `portYIELD_FROM_ISR(w)` 的作用。
- 能说明 `xQueueReceive(g_queue, &e, portMAX_DELAY)` 为什么让任务长期阻塞等待事件。
- 能说明 `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` 保护了哪段共享数据访问。
- 能解释 `g_shared` 为什么用 `volatile uint32_t`。
- 能说清楚 NVIC 优先级 6 为什么符合 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` 的约束。
- 能区分寄存器版 EXTI 清标志和 HAL 版回调分发。
- 能根据“按键无反应、只进一次中断、队列满、HardFault、PC13 不翻转”等现象排查。
- 能理解中断短、任务长的工程分层原则。

## 3. 本课目录结构

```text
49_freertos_interrupt_critical/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个版本都使用 `FreeRTOS.h`、`task.h` 和 `queue.h`。`platformio.ini` 指向本仓库 `freertos/FreeRTOSConfig.h` 和 FreeRTOS ARM_CM3 portable 目录。寄存器版和 HAL 版的队列、任务、临界区逻辑相同，差别在 EXTI 配置入口和 GPIO/EXTI 封装方式。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill。
- 下载器：ST-Link。
- 系统时钟：HSE 8MHz，PLL x9，SYSCLK 72MHz。
- 输入引脚：PA0，上拉输入，下降沿触发 EXTI0。
- 触发方式：PA0 默认上拉为高，按键或跳线把 PA0 接到 GND 时产生下降沿。
- 输出引脚：PC13 板载 LED，收到事件后翻转。
- 辅助输出：PA1/PA2 被初始化为输出，但当前任务没有使用。
- FreeRTOS 队列：长度 4，每个元素 1 字节。
- 中断优先级：EXTI0 设置为 6。

如果实际板子没有外部按键，需用跳线让 PA0 从高电平变为低电平。必须共地，不能让 PA0 悬空。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
PA0 上拉保持高电平
  -> 按下或接地，PA0 出现下降沿
  -> EXTI0 线检测到下降沿
  -> NVIC 响应 EXTI0_IRQn
  -> 进入 EXTI0_IRQHandler
  -> ISR 清 EXTI pending 标志
  -> ISR 调用 xQueueSendFromISR(g_queue, &e, &w)
  -> 若发送使更高优先级任务就绪，w 变为 pdTRUE
  -> ISR 末尾 portYIELD_FROM_ISR(w)
  -> event_task 从 xQueueReceive(portMAX_DELAY) 返回
  -> event_task 进入临界区，g_shared++
  -> event_task 退出临界区，翻转 PC13
```

这里每一层都不能跳过。硬件层是 PA0 电平变化，芯片外设层是 AFIO/EXTI/NVIC，RTOS 层是 ISR 安全队列 API 和任务阻塞唤醒，C 代码层是 `uint8_t e` 和 `volatile uint32_t g_shared`，现象层是 PC13 翻转和 `g_shared` 增加。

本课的工程思想是：中断只投递事件，任务处理业务。ISR 越短，系统越稳定；任务可以阻塞等待、进入临界区、更新共享状态并做相对耗时的动作。

## 6. 先认识本课里出现的核心名词

### 6.1 `PA0` 是什么

PA0 是 STM32 GPIOA 的 0 号引脚，属于 GPIO 电气层和 EXTI 输入层。

本课把 PA0 配成上拉输入。默认读到高电平，按键接地或跳线接 GND 时变成低电平，形成下降沿。这个下降沿是整个链路的起点。

如果 PA0 悬空、没有共地或接线反了，EXTI0 可能不触发、乱触发或只触发一次。

### 6.2 `EXTI0` 是什么

EXTI0 是外部中断/事件控制器的第 0 条线，属于 STM32 片上外设层。

它可以把 PA0 的边沿变化转换成中断请求。寄存器版通过 `AFIO->EXTICR[0]` 选择 EXTI0 来自 PA0，通过 `EXTI->IMR` 放开中断屏蔽，通过 `EXTI->FTSR` 选择下降沿触发。

如果 EXTI line 没有打开或触发边沿选错，按键电平变化不会进入中断。

### 6.3 `AFIO->EXTICR[0]` 是什么

`EXTICR` 是 AFIO 的外部中断配置寄存器，属于 STM32 复用功能映射层。

F1 的 EXTI0 可以来自 PA0、PB0、PC0 等同编号引脚，`AFIO->EXTICR[0] &= ~AFIO_EXTICR1_EXTI0` 选择 PA0。这里清零不是随便写，而是因为 PA 对应编码 0000。

如果映射到错误端口，你按 PA0 不会触发预期 EXTI0。

### 6.4 `EXTI->IMR` 是什么

IMR 是 Interrupt Mask Register，中断屏蔽寄存器，属于 EXTI 控制层。

`EXTI->IMR |= EXTI_IMR_MR0` 表示允许 EXTI0 产生中断请求。边沿检测和中断放行是两步，FTSR 只负责检测下降沿，IMR 决定是否送到 NVIC。

IMR 没开时，PA0 边沿可能设置 pending，但不会进入 IRQHandler。

### 6.5 `EXTI->FTSR` 是什么

FTSR 是 Falling Trigger Selection Register，下降沿触发选择寄存器。

`EXTI->FTSR |= EXTI_FTSR_TR0` 让 EXTI0 对下降沿敏感。本课 PA0 上拉，按下接地，所以从 1 到 0 的下降沿正好表示按下。

如果改成上升沿，可能变成松手触发；如果上下沿都开，按下和松手都可能投递事件。

### 6.6 `EXTI->PR` 是什么

PR 是 Pending Register，挂起标志寄存器，属于 EXTI 标志层。

寄存器版 ISR 先判断 `EXTI->PR & EXTI_PR_PR0`，再写 `EXTI->PR = EXTI_PR_PR0` 清除标志。F1 的 EXTI pending 标志通常写 1 清除。

如果不清标志，中断可能反复进入；如果清错标志，可能影响其他 EXTI 线。

### 6.7 `NVIC_SetPriority(EXTI0_IRQn, 6)` 是什么

NVIC 是 Cortex-M 内核中断控制器，属于内核异常管理层。

本课把 EXTI0 优先级设为 6。FreeRTOS 配置 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`，在 Cortex-M 上，数值越小逻辑优先级越高。调用 FromISR API 的中断优先级数值必须不高于限制，也就是要使用 5 或更大的数值，6 是安全的。

如果把 EXTI0 设成 0 这类过高优先级，再调用 FreeRTOS ISR API，可能触发断言、HardFault 或随机异常。

### 6.8 `QueueHandle_t g_queue` 是什么

`QueueHandle_t` 是 FreeRTOS 队列句柄类型，属于 RTOS 内核对象层。

`g_queue = xQueueCreate(4, sizeof(uint8_t))` 创建长度 4、元素 1 字节的队列。ISR 发送 `uint8_t e=1`，任务接收这个事件。

如果队列创建失败，main 会停住；如果队列满，ISR 发送可能失败，本课源码没有检查返回值，这是可改进点。

### 6.9 `xQueueCreate()` 是什么

`xQueueCreate()` 是 FreeRTOS 创建队列 API，属于 RTOS 对象创建层。

它从 heap 分配队列控制结构和存储区。本课长度 4 表示最多缓存 4 个按键事件，每个事件大小是 `sizeof(uint8_t)`。

heap 不足时返回 NULL，源码会关中断停住。队列长度太短时，连续快速按键可能丢事件。

### 6.10 `xQueueSendFromISR()` 是什么

`xQueueSendFromISR()` 是中断上下文安全的队列发送 API，属于 FreeRTOS ISR API 层。

寄存器版在 `EXTI0_IRQHandler()` 里调用，HAL 版在 `HAL_GPIO_EXTI_Callback()` 里调用。它不会像普通阻塞 API 那样等待队列空位，而是立即尝试发送。

如果在 ISR 里调用普通 `xQueueSend()` 并带阻塞时间，会破坏中断上下文规则。

### 6.11 `BaseType_t w` 是什么

`w` 是 `xHigherPriorityTaskWoken` 的本地变量，属于 ISR 到调度器交接层。

ISR 发送队列后，如果唤醒了更高优先级任务，FreeRTOS 会把它置为 `pdTRUE`。随后 `portYIELD_FROM_ISR(w)` 根据这个值决定是否在中断退出时请求上下文切换。

如果不用它，任务可能要等到下一次 tick 才运行，实时性变差。

### 6.12 `portYIELD_FROM_ISR()` 是什么

`portYIELD_FROM_ISR()` 是 FreeRTOS 端口层宏，属于 Cortex-M 上下文切换触发层。

它通常会在需要时触发 PendSV，让中断退出后立即切换到被唤醒的高优先级任务。本课 `event_task` 优先级 2，如果它被队列唤醒，应该尽快处理事件。

如果漏掉它，功能可能仍能跑，但响应会滞后。

### 6.13 `xQueueReceive(..., portMAX_DELAY)` 是什么

`xQueueReceive()` 是任务上下文队列接收 API，`portMAX_DELAY` 表示一直等，属于 RTOS 阻塞等待层。

`event_task` 没有事件时阻塞，不占 CPU；ISR 发送事件后，任务被唤醒，接收 `uint8_t e` 并继续处理。

如果队列句柄错误或没有 ISR 发送，任务会一直阻塞，PC13 不会翻转。

### 6.14 `taskENTER_CRITICAL()` 是什么

`taskENTER_CRITICAL()` 进入临界区，属于 FreeRTOS 临界保护层。

本课用它保护 `g_shared++`。递增操作在 C 层不是单条抽象动作，通常包含读、加、写。临界区避免这个过程被可屏蔽中断或任务切换打断。

临界区要短。把耗时操作放进临界区，会增加中断延迟。

### 6.15 `g_shared` 是什么

`g_shared` 是 `static volatile uint32_t` 全局变量，属于任务共享状态层。

每收到一个事件，任务在临界区里让它加 1。`volatile` 让调试观察和每次访问更直观，避免编译器把访问优化得不符合教学观察。

如果多个任务或 ISR 都修改它，必须设计更严格的同步。本课只有 event_task 修改，但仍用临界区训练保护思路。

### 6.16 `HAL_GPIO_EXTI_Callback()` 是什么

这是 HAL 的 EXTI 回调函数，属于 HAL 中断分发层。

HAL 版 `EXTI0_IRQHandler()` 只调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`，HAL 清标志并分发后调用用户实现的 `HAL_GPIO_EXTI_Callback(uint16_t pin)`。用户在回调里判断 `pin == GPIO_PIN_0` 再发送队列。

回调不是普通任务函数，它仍在中断上下文里，所以仍必须使用 FromISR API。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件为什么包含 `queue.h`

寄存器版除了 `FreeRTOS.h` 和 `task.h`，还包含 `queue.h`。

队列类型 `QueueHandle_t`、`xQueueCreate()`、`xQueueSendFromISR()`、`xQueueReceive()` 都在队列模块声明。少了它，队列 API 无法正确编译。

### 7.2 时钟初始化

`system_clock_72mhz_init()` 配置 Flash、HSE、PLL x9 和总线分频。

FreeRTOS tick、NVIC 响应和 GPIO 速度都建立在系统时钟配置之上。`configCPU_CLOCK_HZ` 也写成 72MHz。

### 7.3 GPIO 初始化

`gpio_init()` 打开 GPIOC、GPIOA、AFIO 时钟。

PC13、PA1、PA2 配为输出；PA0 配为上拉输入。PA0 上拉需要输入上拉/下拉模式加 ODR0 置 1，这对应 `GPIO_CRL_CNF0_1` 和 `GPIOA->BSRR = GPIO_BSRR_BS0`。

### 7.4 EXTI0 映射 PA0

main 中 `AFIO->EXTICR[0] &= ~AFIO_EXTICR1_EXTI0` 选择 PA0 作为 EXTI0 来源。

这一步依赖 AFIO 时钟已打开。若 AFIO 没开或映射错误，PA0 边沿不会进入 EXTI0。

### 7.5 打开 EXTI0 中断和下降沿

`EXTI->IMR |= EXTI_IMR_MR0` 放开 EXTI0 中断屏蔽，`EXTI->FTSR |= EXTI_FTSR_TR0` 选择下降沿触发。

PA0 上拉后按下接地，所以下降沿代表按键按下。若你的硬件是按下接 VCC，就要改成上升沿或改变输入策略。

### 7.6 NVIC 优先级和使能

`NVIC_SetPriority(EXTI0_IRQn, 6)` 设置中断优先级，`NVIC_EnableIRQ(EXTI0_IRQn)` 使能中断线。

优先级 6 符合 FreeRTOS ISR API 限制。使能 NVIC 后，EXTI0 pending 才能进入 `EXTI0_IRQHandler()`。

### 7.7 创建队列

`g_queue = xQueueCreate(4, sizeof(uint8_t));`

这创建一个最多保存 4 个事件的队列。元素按值复制，ISR 里的局部变量 `e` 可以安全发送，因为队列会复制 1 字节内容。

### 7.8 创建 `event_task`

main 创建 `event_task`，栈 160，优先级 2。

创建失败或队列为 NULL 都会关中断停住。任务优先级较高，保证事件到来后能及时处理。

### 7.9 `event_task` 阻塞等待队列

任务循环里调用：

```c
xQueueReceive(g_queue, &e, portMAX_DELAY)
```

没有事件时任务阻塞，不占 CPU。ISR 发送事件后，它返回 `pdPASS`，说明 `e` 中收到一个事件值。

### 7.10 临界区保护 `g_shared++`

收到事件后，任务进入临界区，执行 `g_shared++`，再退出临界区。

虽然本课只有一个任务写它，但递增共享变量是最典型的临界区示例。临界区范围只包住一行递增，保持很短。

### 7.11 PC13 翻转作为事件处理完成证据

临界区后调用 `led_toggle_pc13()`。

这说明事件已经从 ISR 传到任务，并且任务完成了共享变量更新。若 `g_shared` 增加但 PC13 不翻，查 GPIO；若 PC13 不翻且 `g_shared` 不增，查中断和队列。

### 7.12 `EXTI0_IRQHandler()` 判断和清标志

ISR 先定义 `BaseType_t w = pdFALSE`，再判断 `EXTI->PR` 的 PR0。

确认是 EXTI0 后，写 1 清 pending 标志。这个顺序避免误处理其他中断，也避免标志不清导致反复进入。

### 7.13 ISR 发送队列事件

ISR 内定义 `uint8_t e = 1U`，调用 `xQueueSendFromISR(g_queue, &e, &w)`。

它把事件复制进队列。如果 event_task 因等待队列而被唤醒，`w` 可能变为 true。

### 7.14 ISR 末尾请求切换

`portYIELD_FROM_ISR(w)` 放在 ISR 末尾。

如果 `w` 表示有更高优先级任务被唤醒，FreeRTOS 会安排中断退出后尽快切到该任务。这样按键响应不必等下一个 tick。

## 8. HAL 版代码逐步讲解

### 8.1 HAL 初始化和时钟

HAL 版先 `HAL_Init()`，再用 RCC 结构体配置 72MHz。

这些步骤对应寄存器版时钟配置，确保 FreeRTOS tick 和外设时钟基础正确。

### 8.2 HAL GPIO 配置

PC13/PA1/PA2 配输出，PA0 配成 `GPIO_MODE_IT_FALLING` 加 `GPIO_PULLUP`。

`HAL_GPIO_Init()` 会据此在底层完成 AFIO EXTI0 映射、IMR 放行和 FTSR 下降沿选择。这一句结构体配置等价于寄存器版 `exti0_pa0_init()` 里手动写 AFIO/IMR/FTSR 的几步，再配合下面的 NVIC 使能，中断链路即完整。

### 8.3 HAL NVIC 配置

`HAL_NVIC_SetPriority(EXTI0_IRQn, 6, 0)` 设置优先级，`HAL_NVIC_EnableIRQ(EXTI0_IRQn)` 使能中断。

这对应寄存器版 NVIC 设置。优先级 6 同样满足 FreeRTOS FromISR API 约束。

### 8.4 HAL 版队列创建

HAL 版同样 `xQueueCreate(4, sizeof(uint8_t))`。

队列属于 FreeRTOS，不属于 HAL。HAL 不会替你创建或管理这个队列。

### 8.5 HAL 版 `event_task`

HAL 版任务同样 `xQueueReceive(..., portMAX_DELAY)`，收到事件后进入临界区增加 `g_shared`，再用 `HAL_GPIO_TogglePin()` 翻转 PC13。

RTOS 行为和寄存器版一致，差别只有 GPIO 翻转封装。

### 8.6 HAL 版 IRQHandler

`EXTI0_IRQHandler()` 里调用 `HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0)`。

HAL handler 会检查并清除对应 EXTI 标志，然后调用用户回调。它封装了寄存器版手动判断 PR 和清 PR 的步骤。

### 8.7 HAL 回调仍在中断上下文

`HAL_GPIO_EXTI_Callback(uint16_t pin)` 不是任务函数。

它由 IRQHandler 间接调用，仍然处在中断上下文。因此回调里使用 `xQueueSendFromISR()` 是正确的，不能改成带阻塞的普通队列发送。

### 8.8 HAL 回调里的 pin 判断

源码判断 `if(pin == GPIO_PIN_0)`，只处理 PA0 对应事件。

这能避免其他 EXTI 引脚共用回调时误发送队列事件。即使当前只有 EXTI0，也应保留这种判断习惯。

### 8.9 HAL 版 `portYIELD_FROM_ISR()`

回调最后调用 `portYIELD_FROM_ISR(w)`。

这和寄存器版一样，负责在 ISR 唤醒高优先级任务后尽快切换。HAL 不会自动替你做 FreeRTOS 的 yield 判断。

### 8.10 HAL 版 EXTI 配置由结构体一步完成

源码对 PA0 使用 `GPIO_MODE_IT_FALLING` 加 `GPIO_PULLUP`。

`HAL_GPIO_Init()` 会据此把 EXTI0 映射到 PA0、放行中断屏蔽并选择下降沿，相当于寄存器版手写 AFIO/IMR/FTSR。所以 HAL 版只需再用 `HAL_NVIC_SetPriority`/`HAL_NVIC_EnableIRQ` 打开 NVIC，中断链路即完整。

## 9. 两个版本真正应该怎么学

寄存器版把 EXTI 的每个底层动作摊开：AFIO 映射、IMR 放行、FTSR 选边沿、PR 清标志、NVIC 使能。HAL 版把 IRQHandler 到 callback 的分发封装起来，但 FreeRTOS 队列发送和 yield 仍然要你自己写。

本课最重要的不是“按键点灯”，而是中断到任务的责任分工：ISR 只记录事件并快速退出，任务阻塞等待并处理业务，临界区只保护短小共享数据访问。

## 10. 检验问题清单

### 10.1 为什么 ISR 里用 `xQueueSendFromISR()`？

**答**：因为 ISR 不能调用可能阻塞的普通任务 API。FromISR 版本专门用于中断上下文，立即尝试发送并通过参数报告是否唤醒任务。

### 10.2 `portYIELD_FROM_ISR(w)` 不写会怎样？

**答**：事件可能仍被发送，但被唤醒的高优先级任务可能要等下一个 tick 才运行，响应延迟变大。

### 10.3 `portMAX_DELAY` 在接收队列里表示什么？

**答**：表示任务一直阻塞等待队列事件，不消耗 CPU。只要队列有数据或被中断唤醒，才返回处理。

### 10.4 为什么 EXTI0 优先级设为 6？

**答**：FreeRTOS 配置允许调用系统 API 的最高逻辑优先级边界是 5。Cortex-M 数值越小优先级越高，所以设置为 6 符合约束。

### 10.5 `g_shared++` 为什么放进临界区？

**答**：递增不是抽象上的不可分割动作。临界区保护读、加、写过程，训练共享数据保护思路。

### 10.6 队列长度 4 有什么后果？

**答**：最多缓存 4 个未处理事件。按键太快或任务处理太慢时，队列可能满，后续 ISR 发送会失败。

### 10.7 HAL 回调里能不能调用普通 `xQueueSend()`？

**答**：不能按普通任务上下文理解。HAL 回调仍在 ISR 中，应使用 `xQueueSendFromISR()`。

### 10.8 PC13 翻转说明了什么？

**答**：说明事件至少已经从 ISR 进入队列，并被 event_task 接收处理。它不是 PA0 边沿的直接硬件输出。

### 10.9 HAL 版只开 NVIC 够不够？

**答**：单看 NVIC 不够，还必须把 PA0 配成 EXTI 下降沿并正确清标志。本课 HAL 版已用 `GPIO_MODE_IT_FALLING` 完成 EXTI 配置，`HAL_GPIO_EXTI_IRQHandler()` 负责清标志，所以 NVIC 只是最后一步。

### 10.10 为什么中断里不直接修改复杂业务？

**答**：ISR 应尽量短，避免长时间关住低优先级中断或影响调度。复杂业务放到任务里更安全。

## 11. 工程实现步骤

### 11.1 需求分析

目标是让 PA0 外部事件安全唤醒 FreeRTOS 任务，任务再更新共享变量并翻转 LED。

### 11.2 硬件核查

确认 PA0 默认高电平，按下或跳线接 GND 会产生下降沿。确认 PC13 可输出，ST-Link 可调试，按键有共地。

### 11.3 寄存器路线

配置 GPIO 和 AFIO，映射 PA0 到 EXTI0，打开 IMR 和下降沿，设置 NVIC 优先级 6。创建队列和任务。ISR 清标志并 FromISR 发送队列。

### 11.4 HAL 路线

用 HAL 配置时钟和 GPIO，启用 EXTI0 NVIC，在 IRQHandler 里调用 HAL EXTI handler，在 callback 里 FromISR 发送队列。注意核查 PA0 是否配置成中断模式。

### 11.5 工程思维

中断只做事件投递，任务做业务处理；共享变量用临界区或其他同步机制保护；ISR 调用 RTOS API 必须遵守中断优先级限制。

### 11.6 常见工程陷阱

ISR 调普通阻塞 API、NVIC 优先级过高、忘记清 EXTI pending、队列太短导致丢事件、临界区太长、HAL 回调误以为是任务上下文、PA0 没有真实下降沿，都是本课常见坑。

## 12. 运行现象

正常情况下，每次 PA0 出现下降沿，队列收到一个事件，`event_task` 被唤醒，`g_shared` 增加 1，PC13 翻转一次。

可以在调试器 Watch 里观察 `g_shared`。如果它增加而 PC13 不翻，说明 RTOS 事件链路基本通，问题偏向 GPIO 输出；如果它不增加，问题偏向 PA0/EXTI/NVIC/队列。

HAL 版的 PA0 已用 `GPIO_MODE_IT_FALLING` 配好 EXTI 触发，配合 NVIC 使能即可响应按键。若无反应，优先查接线、上拉和抖动，而不是怀疑 EXTI 模式没配。

## 13. 常见问题排查

### 13.1 按 PA0 没反应

先确认 PA0 电平是否真的从高到低变化，再查 GPIOA 时钟、PA0 上拉、AFIO EXTI0 映射、EXTI IMR/FTSR 和 NVIC 使能。

### 13.2 中断只进一次

检查 EXTI pending 标志是否正确清除。寄存器版要写 1 清 `EXTI_PR_PR0`；HAL 版要确认 HAL handler 被调用。

### 13.3 进入 HardFault 或异常

检查 EXTI0 NVIC 优先级是否满足 FreeRTOS API 约束。调用 FromISR API 的中断不能配置成过高逻辑优先级。

### 13.4 队列收不到事件

确认 `g_queue` 创建成功，ISR 里调用的是 `xQueueSendFromISR()`，队列没有满，`event_task` 正在 `xQueueReceive()` 等待。

### 13.5 PC13 不翻但 `g_shared` 增加

说明事件已经到任务，重点查 PC13 GPIO 初始化和翻转函数。寄存器版看 CRH/ODR/BSRR/BRR，HAL 版看 `HAL_GPIO_Init()`。

### 13.6 `g_shared` 数值异常

检查是否有其他代码也修改它，临界区是否被删，变量是否仍是 `volatile`。若事件抖动严重，也可能一次按键触发多次。

### 13.7 按一次触发多次

机械按键有抖动，下降沿可能出现多次。当前源码没有消抖，队列会收到多个事件。可以后续在任务里加时间过滤。

### 13.8 HAL 版按键无响应

本课 PA0 已用 `GPIO_MODE_IT_FALLING` 配好 EXTI。若无响应，重点查接线、`GPIO_PULLUP` 是否生效、按键是否真的拉低 PA0，以及 NVIC 优先级是否符合约束。

### 13.9 队列满导致丢事件

快速连续触发 PA0，而任务处理不及时，长度 4 的队列可能满。当前 ISR 没检查发送返回值，工程中应记录失败次数或做防抖限流。

## 14. 本课最核心的结论

1. PA0 下降沿经过 EXTI0/NVIC 后进入中断，不是任务自己轮询出来的。
2. ISR 中要使用 FromISR API，不能调用可能阻塞的普通队列发送。
3. `portYIELD_FROM_ISR()` 让被唤醒任务能更快运行。
4. `xQueueReceive(..., portMAX_DELAY)` 让任务无事件时阻塞，不浪费 CPU。
5. 临界区应短，只保护必须保护的共享访问。
6. NVIC 优先级必须符合 FreeRTOS ISR API 约束。
7. HAL 回调仍属于中断上下文，不能按普通任务函数处理。
8. HAL 版用 `GPIO_MODE_IT_FALLING` 一步配好 PA0 的 EXTI 触发，`HAL_GPIO_Init()` 在底层完成 AFIO/IMR/FTSR。

## 15. 建议你现在怎么读这节课

先按第 5 章把链路画出来，尤其标出哪些步骤属于硬件，哪些属于 RTOS。第二遍读寄存器版，从 AFIO、EXTI、NVIC、ISR、队列、任务一路跟到 PC13。第三遍读 HAL 版，重点看 IRQHandler 和 Callback 的关系，理解 `GPIO_MODE_IT_FALLING` 如何一步等价于寄存器版的 AFIO/IMR/FTSR。

调试时建议在 `EXTI0_IRQHandler()`、`xQueueSendFromISR()`、`xQueueReceive()` 返回后、`g_shared++` 四个位置打断点。这样能准确定位事件断在哪一层。

## 16. 扩展练习

1. 给按键事件增加简单软件消抖，例如任务收到事件后忽略 50ms 内的新事件。
2. 记录 `xQueueSendFromISR()` 的返回值，统计队列满导致的丢事件。
3. 把队列长度从 4 改成 1，观察快速按键时的差异。
4. 故意把 EXTI0 优先级改成过高，观察 FreeRTOS API 约束带来的问题。
5. 在 HAL 版把 PA0 改成 `GPIO_MODE_IT_RISING`，验证改成上升沿触发后按键时机的变化。

## 17. 下一课预告

- 上一课：[48_freertos_time_management](../48_freertos_time_management/README.md)
- 下一课：[50_freertos_queue](../50_freertos_queue/README.md)
