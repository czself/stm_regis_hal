# 58_freertos_low_power_tickless - FreeRTOS Tickless Idle

## 1. 本课到底在学什么

本课表面现象是：PC13 大约每 2000ms 翻转一次。任务翻转 LED 后调用 `vTaskDelay(pdMS_TO_TICKS(2000))`，系统在长时间没有就绪任务时会进入 tickless idle，并在空闲睡眠入口执行 `__WFI()`。

真正要学的是 FreeRTOS tickless idle 的最小链路。它不是 Stop 模式，也不是 Standby 模式；源码没有重配 PLL、没有关外设电源、没有设置 PWR/EXTI 唤醒。它只是在内核判断可睡眠时调用 `vApplicationSleep(expected_idle_time)`，本课在里面执行 ARM 指令 `__WFI()` 等待中断唤醒。

本课继续按六层来拆：现象层看板子或调试变量，硬件层看 STM32 引脚和外设，芯片模块层看 GPIO/USART/内核节拍，寄存器层看具体状态位和控制位，C/CMSIS 层看源码语句，HAL/工程层看封装 API、返回值和排错路径；FreeRTOS 章节还必须额外解释任务、对象、阻塞、调度和 hook。

## 2. 本课学习目标

- 能解释 tickless idle 为什么是 RTOS 空闲期优化。
- 能说明 `configUSE_TICKLESS_IDLE = 1` 的作用。
- 能解释 `vApplicationSleep()` 何时被调用。
- 能说明 `expected_idle_time` 表示内核预计可空闲的 tick 数。
- 能解释 `__WFI()` 是 Cortex-M 指令，不是 STM32 Stop/Standby。
- 能根据 PC13 的 2 秒节奏判断 slow_task 是否正常。
- 能说明本课为什么需要本地 FreeRTOSConfig.h。
- 能区分普通 sleep、tickless、Stop、Standby 的边界。

## 3. 本课目录结构

```text
58_freertos_low_power_tickless/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 和 `hal/` 都要读。寄存器版负责把外设控制摊开，HAL 版负责说明结构体字段和 API 怎样映射到底层动作。FreeRTOS 机制本身不因为 HAL 而改变。

## 4. 实验硬件与工程前提

- STM32F103C8T6 BluePill。
- PC13：slow_task 每 2000ms 翻转。
- 系统时钟 72MHz。
- 本课 reg/hal 都有本地 `freertos/FreeRTOSConfig.h`。
- 本地配置中 `configUSE_TICKLESS_IDLE = 1`。
- `vApplicationSleep()` 中只执行 `__WFI()`。
- 没有使用 PWR 深睡眠、RTC 唤醒、Stop 或 Standby 配置。

这类课不能只看 main 函数，还要看 FreeRTOSConfig、platformio.ini 和 hook 函数。因为很多异常不是业务代码写错，而是 heap、stack、tick、NVIC 优先级或配置宏没有满足 API 前提。

## 5. 先建立一个最基本的脑图

```text
复位启动
  -> 配 72MHz 时钟和 GPIO
  -> 创建 slow_task，优先级 1
  -> 启动调度器
  -> slow_task 翻转 PC13
  -> slow_task vTaskDelay(2000ms) 进入阻塞
  -> 内核发现较长时间没有普通任务就绪
  -> tickless idle 调用 vApplicationSleep(expected_idle_time)
  -> 执行 __WFI() 等待中断
  -> SysTick 或其他中断唤醒 CPU
  -> 时间到后 slow_task 再次运行
```

读这张图时，把“谁触发现象”和“现象最终改了哪个硬件状态”分开。RTOS 负责安排时机，STM32 外设负责产生可见结果，二者缺一层都解释不完整。

## 6. 先认识本课里出现的核心名词

### 6.1 `tickless idle` 是什么

tickless idle 是 FreeRTOS 在空闲时间减少周期 tick 中断的机制。
普通 tick 每 1ms 打断一次 CPU；tickless 在预计空闲较长时尝试睡眠，减少无意义唤醒。

### 6.2 `configUSE_TICKLESS_IDLE` 是什么

它是 FreeRTOSConfig.h 中启用 tickless 的宏。
本课在本地配置文件里设为 1，所以这节课不会被全局配置误伤。

### 6.3 `vApplicationSleep` 是什么

它是应用层提供给 FreeRTOS 的睡眠入口函数。
内核决定可以睡时调用它，并传入预计空闲 tick 数。

### 6.4 `expected_idle_time` 是什么

它表示内核预计还能空闲多久，单位是 tick。
本课没有用它做复杂功耗策略，只 `(void)expected_idle_time` 防止未使用警告。

### 6.5 `__WFI` 是什么

`__WFI()` 是 Wait For Interrupt 指令，属于 Cortex-M 内核指令层。
执行后 CPU 等待中断唤醒；它不是 STM32 的 Stop/Standby 模式配置。

### 6.6 `slow_task` 是什么

slow_task 是本课唯一普通任务。
它每次翻转 PC13 后阻塞 2000ms，制造足够长的空闲窗口。

### 6.7 `vTaskDelay(2000ms)` 是什么

它让 slow_task 进入阻塞态。
正因为任务阻塞，idle 才有机会运行，tickless 才有机会触发。

### 6.8 `idle task` 是什么

idle task 是 FreeRTOS 内核自动创建的空闲任务。
当没有其他就绪任务时它运行；tickless 逻辑发生在空闲路径上。

### 6.9 `SysTick` 是什么

SysTick 通常提供 FreeRTOS tick 节拍。
tickless 会围绕 tick 节拍做抑制和补偿，但本课重点不展开移植层细节。

### 6.10 `Sleep` 和 `Stop` 的区别

Sleep/WFI 主要让内核等待中断，外设和主时钟配置通常保持。
Stop 会进入更深低功耗，需要 PWR、时钟恢复等配置；本课没有这些代码。

### 6.11 `Standby` 是什么

Standby 是更深的低功耗模式，唤醒后接近复位流程。
本课没有进入 Standby，也不涉及备份域唤醒。

### 6.12 `PC13` 是什么

PC13 是 slow_task 周期运行的证据。
LED 2 秒节奏正常，说明任务延时和唤醒基本正常。

### 6.13 `本地 FreeRTOSConfig.h` 是什么

本课目录下有局部配置，保证 `configUSE_TICKLESS_IDLE` 对本课生效。
如果 include 路径没有选到本地配置，tickless 可能没有打开。

### 6.14 `中断唤醒` 是什么

WFI 后 CPU 需要中断才能继续。
FreeRTOS tick、调试中断或外设中断都可能唤醒 CPU。

### 6.15 `低功耗边界` 是什么

本课只演示 RTOS 空闲睡眠入口。
不要把它写成完整低功耗产品方案；真正产品还要处理时钟、外设、唤醒源和功耗测量。


## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

只需要 `FreeRTOS.h` 和 `task.h`。
tickless 由配置宏和 port 层参与，不需要额外对象头文件。

### 7.2 时钟初始化

系统仍配置为 72MHz。
本课不是为了改变主频，而是在任务空闲时让 CPU 等中断。

### 7.3 GPIO 初始化

PC13 输出用于观察 slow_task 周期。
PA0/PA1/PA2 是公共初始化，不参与 tickless 判断。

### 7.4 vApplicationSleep 定义

源码实现 `void vApplicationSleep(TickType_t expected_idle_time)`。
这个函数名由 FreeRTOS tickless 机制调用，签名不能随便改。

### 7.5 忽略 expected_idle_time

`(void)expected_idle_time;` 表示本课不按预计时间做额外策略。
复杂工程可以用它决定是否真的进入更深低功耗。

### 7.6 __WFI 指令

`__WFI()` 让 Cortex-M 内核等待中断。
这是 CMSIS 提供的内联函数，最终对应内核指令。

### 7.7 slow_task 翻转 PC13

任务每轮先翻转 LED。
这样你能看到任务从睡眠/阻塞后被唤醒继续运行。

### 7.8 slow_task 延时 2000ms

2 秒阻塞制造长空闲窗口。
如果延时很短，tickless 的效果不明显。

### 7.9 xTaskCreate 检查

创建 slow_task 失败会关中断停住。
没有任务就没有本课现象，也没有正常的空闲节奏。

### 7.10 调度器启动后才有 tickless

tickless 是调度器运行后的空闲路径行为。
main 里的 while(1) 正常不应执行。


## 8. HAL 版代码逐步讲解

### 8.1 HAL_Init

HAL 版先做 HAL 基础初始化。
这不改变 tickless 的 FreeRTOS 机制。

### 8.2 HAL 时钟配置

HAL 结构体同样配置 72MHz。
睡眠入口不是通过 HAL RCC API 实现。

### 8.3 HAL GPIO 配置

PC13 配成输出并初始置高。
LED 翻转仍是现象层证据。

### 8.4 HAL_GPIO_TogglePin

slow_task 用 HAL 翻转 PC13。
它替代寄存器版 ODR/BSRR/BRR 操作。

### 8.5 vApplicationSleep 不属于 HAL

这个 hook 是 FreeRTOS 应用钩子。
HAL 版也必须按 FreeRTOS 要求提供同名函数。

### 8.6 __WFI 在 HAL 版一样使用

`__WFI()` 来自 CMSIS 内核接口。
HAL 工程也可以直接调用 CMSIS 内核指令。

### 8.7 本地配置仍然重要

HAL 版也要使用本课自己的 FreeRTOSConfig.h。
否则 `configUSE_TICKLESS_IDLE` 可能不生效。

### 8.8 不调用 HAL_PWR 进入 Stop

源码没有 `HAL_PWR_EnterSTOPMode()`。
所以不要把本课现象解释成 Stop 模式唤醒。


## 9. 两个版本真正应该怎么学

寄存器版和 HAL 版不是两门课。它们做同一件事：先把芯片时钟和引脚准备好，再让 FreeRTOS 调度任务或处理同步对象，最后通过 LED、串口或调试变量验证机制。

寄存器版要重点看 `RCC`、`GPIOx->CRL/CRH`、`BSRR/BRR/ODR`、`USART1->SR/DR/BRR/CR1`、NVIC 等直接控制点。HAL 版要重点看 `GPIO_InitTypeDef`、`UART_HandleTypeDef`、`HAL_GPIO_Init()`、`HAL_UART_Init()`、`HAL_UART_Receive_IT()` 这些封装把哪些底层动作合并了。

FreeRTOS API 要单独成层理解。`xTaskCreate()`、`vTaskDelay()`、`xQueueCreate()`、`xQueueReceive()`、hook 函数、tickless hook 都不是 HAL 的一部分。它们控制任务状态和调度时机，硬件 API 只负责最终的 IO 行为。

## 10. 检验问题清单

### 10.1 tickless idle 是硬件低功耗模式吗？

**答**：它是 FreeRTOS 空闲期策略，本课只在睡眠入口执行 WFI，不等于 Stop 或 Standby。

### 10.2 `configUSE_TICKLESS_IDLE` 为什么要为 1？

**答**：它启用 FreeRTOS tickless 路径，否则不会按本课方式调用睡眠入口。

### 10.3 `vApplicationSleep()` 是谁调用的？

**答**：FreeRTOS 内核在预计空闲时调用，不是 slow_task 直接调用。

### 10.4 `expected_idle_time` 单位是什么？

**答**：单位是 tick，表示内核预计可以空闲的节拍数。

### 10.5 为什么本课忽略 expected_idle_time？

**答**：这是最小演示，只验证 WFI 入口，不做复杂功耗决策。

### 10.6 `__WFI()` 后靠什么醒来？

**答**：靠中断唤醒，例如 tick、调试或外设中断。

### 10.7 PC13 为什么是 2 秒节奏？

**答**：slow_task 每次翻转后 `vTaskDelay(2000ms)`，时间到再运行。

### 10.8 本课有没有配置 PWR Stop？

**答**：没有，源码没有 Stop/Standby 所需 PWR、唤醒源和时钟恢复代码。

### 10.9 HAL 版 tickless API 会变吗？

**答**：不会，`vApplicationSleep()` 和 FreeRTOS 配置与 HAL 无关。

### 10.10 tickless 不生效先查什么？

**答**：先查本地 FreeRTOSConfig.h 是否被 include，`configUSE_TICKLESS_IDLE` 是否为 1。


## 11. 工程实现步骤

### 11.1 需求分析

先明确这节课要验证的 RTOS 机制，再决定用哪个硬件现象承载它。LED、串口回显或 Watch 变量都只是观察窗口，真正要学的是机制背后的任务状态和资源消耗。

### 11.2 硬件核查

确认 PC13 是否可见，PA1/PA2 是否会影响外部电路，ST-Link 下载和复位是否正常。

### 11.3 寄存器路线

寄存器路线先配 72MHz，再开 GPIO/USART 时钟，再配置引脚模式和外设寄存器，最后启动 FreeRTOS。每个宏都要能回答“它改的是哪个寄存器哪一类 bit”。

### 11.4 HAL 路线

HAL 路线用结构体表达同样的硬件配置。读 HAL 版时不要只背 API 名字，要把字段翻译成寄存器动作：模式、速度、上下拉、波特率、字长、校验、中断使能。

### 11.5 工程思维

工程上要把失败路径留出来：创建失败要停住，栈溢出要停住，malloc 失败要停住，中断里要使用 FromISR API，任务里要用阻塞等待释放 CPU。

### 11.6 常见工程陷阱

常见陷阱是：用错上下文 API、任务栈估计过小、忘记检查对象句柄、串口中断优先级不符合 FreeRTOS 规则、在回调里做长阻塞、把低功耗 WFI 当成 Stop/Standby。

## 12. 运行现象

PC13 大约每 2 秒翻转一次。CPU 在 slow_task 阻塞期间可能进入 WFI 等待中断，调试时你看到的主要现象仍然是 LED 周期稳定，不会出现 Stop/Standby 那种唤醒后重新配时钟的流程。

正常现象要能和源码里的周期、队列长度、变量赋值或中断条件对应。异常时先查是否进入 hook，再查对象句柄和任务状态，最后才查 GPIO 或串口接线。

### 12.1 六层对应关系再核对

现象层：PC13 每 2 秒翻转，说明 slow_task 能按延时周期恢复运行。

硬件层：PC13 是 GPIO 输出；CPU 执行 WFI 后等待中断，外设和时钟没有按 Stop/Standby 方式重配。

芯片模块层：Cortex-M 内核提供 WFI 指令，FreeRTOS idle 路径决定什么时候调用睡眠入口。

寄存器层：本课没有配置 PWR_CR、EXTI、RTC 唤醒，也没有 Stop/Standby 的时钟恢复寄存器操作。

C/CMSIS 层：核心语句是 `vApplicationSleep(TickType_t expected_idle_time)` 里的 `__WFI()`。

RTOS 层：slow_task 阻塞 2000ms 后，内核看到较长空闲时间，tickless idle 才有机会进入 sleep hook。

HAL/工程层：HAL 版只替换 GPIO 初始化和翻转，tickless 入口仍是 FreeRTOS hook 加 CMSIS WFI。

### 12.2 本课推荐断点

第一个断点放在 slow_task 的 LED 翻转处，确认任务周期运行。

第二个断点放在 `vApplicationSleep()`，确认 tickless 路径是否进入。

第三个断点放在 `__WFI()` 后一行，用来观察 CPU 被中断唤醒后能否继续执行。

断点会干扰真实时间，所以它只用于验证路径；最终节奏要看连续运行时的 PC13。

### 12.3 和 Stop/Standby 课程的边界

Stop 模式需要进入深睡眠、配置 PWR，并在唤醒后恢复系统时钟。

Standby 模式唤醒后接近复位流程，通常还涉及备份域或唤醒标志。

本课都没有这些代码，因此只能说演示 tickless idle 的 WFI 睡眠入口。

把本课写成完整低功耗方案会误导排错方向：你会去查 PWR、RTC、EXTI，但源码真正需要核对的是 FreeRTOSConfig 和 idle 睡眠 hook。

### 12.4 源码逐项核对

`vApplicationSleep(TickType_t expected_idle_time)` 的函数名和参数类型要和 FreeRTOS port 期望一致。名字写错时，内核不会调用你以为的睡眠入口。

`(void)expected_idle_time;` 不是无意义代码，它说明本课明确选择不使用预计空闲时间做复杂策略，只保留最小 WFI 演示。

`__WFI()` 来自 CMSIS 内核接口，不需要 HAL PWR 模块。它的层级比 STM32 外设库更靠近 Cortex-M 内核。

`slow_task` 的栈深度 128 足够当前简单 LED 翻转；如果你加入串口打印低功耗日志，栈和功耗现象都会改变。

### 12.5 tickless 生效的前提

必须至少有一段时间没有普通任务处于就绪态。slow_task 延时 2000ms 正是为了制造这个窗口。

如果你新增一个高优先级任务一直 while 循环不阻塞，idle task 没机会运行，tickless 也就没有入口。

如果某个外设中断频繁触发，WFI 会频繁醒来。tickless 仍可能被调用，但实际睡眠片段会很短。

如果本地 FreeRTOSConfig.h 没被 include 到，`configUSE_TICKLESS_IDLE = 1` 就可能没有真正参与编译。

### 12.6 为什么本课不讲功耗数字

BluePill 板上有电源 LED、稳压器、下载器连接和外部模块，这些都会影响电流。

本课源码也没有关闭 GPIO、USART、ADC 等外设时钟，更没有进入 Stop/Standby。

因此用这节课直接测到的电流，不能代表 STM32F103 的极限低功耗能力。

本课真正要验证的是 FreeRTOS 空闲路径如何进入应用睡眠 hook，以及 WFI 在这条路径里的位置。

### 12.7 为什么只保留一个 slow_task

本课只创建一个 slow_task，是为了让系统空闲窗口非常清楚。任务翻转 PC13 后阻塞 2000ms，接下来没有其他业务任务争抢 CPU。

如果同时放入多个周期任务，tickless 是否进入就取决于最近一个任务的唤醒时间，`expected_idle_time` 也会更难解释。

所以这个 Demo 不是低功耗系统的最终形态，而是把 tickless 触发条件压到最小，先让你看懂 FreeRTOS 为什么会调用 `vApplicationSleep()`。

### 12.8 和真实项目的关系

真实低功耗项目还要处理外设关闭、唤醒源选择、时钟恢复和功耗测量夹具。

本课先把 FreeRTOS 进入空闲睡眠的入口讲清楚。如果你要做一个真正低功耗的产品，需要在 `vApplicationSleep()` 里根据 `expected_idle_time` 决定是否进入 Stop/Standby、关闭哪些外设时钟、选择 RTC 还是 EXTI 做唤醒源，并在唤醒后恢复系统时钟。这已经超出本课范围，属于低功耗工程专题，但本课的 tickless 入口是那条路的起点。

调试时如果想确认这条路径，优先观察 `vApplicationSleep()` 是否进断点，而不是先测板级电流。

## 13. 常见问题排查

### 13.1 PC13 不闪

检查 slow_task 是否创建成功，是否进入 hook。
再查 PC13 GPIO 配置。

### 13.2 PC13 节奏不是 2 秒

检查系统时钟、tick 频率和 `pdMS_TO_TICKS(2000)`。
调试器暂停也可能影响观察。

### 13.3 tickless 好像没生效

确认使用的是本课本地 FreeRTOSConfig.h。
确认 `configUSE_TICKLESS_IDLE` 为 1，并且任务确实长时间阻塞。

### 13.4 误以为进入 Stop

查看源码是否有 PWR/STOP/时钟恢复。
本课没有这些代码，只是 WFI。

### 13.5 WFI 后不醒

理论上需要中断唤醒。
检查 SysTick/FreeRTOS tick 是否正常，调试环境是否屏蔽中断。

### 13.6 功耗下降不明显

WFI 只是浅睡眠，GPIO、时钟和外设大多仍保持。
要明显降功耗需要更完整的低功耗设计。

### 13.7 HAL 版编译但不睡

HAL 初始化不决定 tickless。
重点查 FreeRTOS 配置和 vApplicationSleep 符号是否正确链接。

### 13.8 进入 malloc failed hook

slow_task 和 idle 任务都要 heap。
heap 不足会让调度器或任务创建失败。


### 13.9 vApplicationSleep 没有被调用

先确认本课使用的是局部 `freertos/FreeRTOSConfig.h`，并且 `configUSE_TICKLESS_IDLE` 为 1。

再确认系统确实存在足够长的空闲窗口。如果你又添加了一个高频常运行任务，idle 任务可能很少获得睡眠机会。

### 13.10 expected_idle_time 很小

这说明内核预计空闲时间不长，通常不值得进入更复杂的睡眠策略。

本课没有使用这个参数，但工程中可以根据它决定是否只 WFI，还是准备更深低功耗。

### 13.11 WFI 被频繁打断

任何已使能中断都可能唤醒 CPU，包括 SysTick、调试相关中断或外设中断。

如果你后来打开串口、定时器或外部中断，睡眠时间会被这些中断切碎。

### 13.12 唤醒后时钟没有恢复代码

这是正常的，因为本课没有进入 Stop/Standby，HSE/PLL 没有按深睡眠流程关闭。

如果你把本课扩展到 Stop 模式，就必须补充唤醒后的系统时钟恢复，这已经是另一节低功耗工程课。

## 14. 本课最核心的结论

1. tickless idle 是 FreeRTOS 空闲期优化，不是完整的低功耗产品方案。
2. 本课睡眠入口只执行 WFI，没有进入 Stop/Standby，也没有关闭 PLL 或外设时钟。
3. WFI 是 Cortex-M 等待中断指令，唤醒后 CPU 继续执行下一条指令。
4. 本课不是 Stop 或 Standby。
5. slow_task 的 2 秒阻塞制造空闲窗口。
6. 本地 FreeRTOSConfig.h 决定 tickless 是否打开。
7. PC13 只显示任务周期，不直接显示功耗数值。
8. 真正低功耗产品还要根据 `expected_idle_time` 决策睡眠深度、处理时钟恢复和唤醒源。

## 15. 建议你现在怎么读这节课

先把第 5 章画到纸上，再逐个追第 6 章的名词。读第 7、8 章时，一边看源码一边找对应硬件动作，不要让 API 名字悬空。

最后用第 13 章反向练习：给自己一个故障现象，要求能说出先查任务还是先查外设，先看句柄还是先看寄存器。能排查，才说明不是只会照抄。

## 16. 扩展练习

1. 修改本课周期或栈大小，观察现象是否按预期变化。
2. 故意让对象创建失败，观察是否进入对应停机分支。
3. 在关键 API 前后打断点，看任务状态如何变化。
4. 把寄存器版和 HAL 版的同一动作写成对应表。
5. 增加 Watch 变量，记录每次循环或回调是否真的执行。

## 17. 下一课预告

- 上一课：[57_freertos_memory_management](../57_freertos_memory_management/README.md)
- 下一课：[59_freertos_uart](../59_freertos_uart/README.md)
