
# 57_freertos_memory_management - FreeRTOS 内存与栈水位

## 1. 本课到底在学什么

本课表面现象是：PC13 大约每 500ms 翻转一次，同时调试器 Watch 里可以观察 `g_stack_watermark` 的数值。LED 证明任务还在运行，水位值证明当前任务栈曾经剩下多少空间。

真正要学的是 FreeRTOS 里的 heap、任务栈、TCB、栈高水位和失败 hook。源码没有创建队列、信号量或定时器，而是专门让一个 `memory_task` 周期性调用 `uxTaskGetStackHighWaterMark(NULL)`，把返回值保存到 `volatile` 全局变量里。

本课继续按六层来拆：现象层看板子或调试变量，硬件层看 STM32 引脚和外设，芯片模块层看 GPIO/USART/内核节拍，寄存器层看具体状态位和控制位，C/CMSIS 层看源码语句，HAL/工程层看封装 API、返回值和排错路径；FreeRTOS 章节还必须额外解释任务、对象、阻塞、调度和 hook。

## 2. 本课学习目标

- 能区分 FreeRTOS heap 和每个任务自己的 stack。
- 能解释 TCB 和任务栈为什么在 `xTaskCreate()` 时分配。
- 能说明 `uxTaskGetStackHighWaterMark(NULL)` 返回值表示什么。
- 能解释为什么 `g_stack_watermark` 要写成 `volatile`。
- 能知道 stack high-water mark 单位通常是 stack word，不是字节。
- 能根据 malloc failed hook 和 stack overflow hook 判断内存类错误。
- 能解释任务栈过小、heap 不足、递归或大数组会造成什么现象。
- 能把 PC13 翻转和 Watch 变量结合起来排查。

## 3. 本课目录结构

```text
57_freertos_memory_management/
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
- PC13：`memory_task` 每 500ms 翻转。
- Watch 变量：`g_stack_watermark`。
- 任务栈深度：源码创建 `memory_task` 时给 160。
- FreeRTOS heap：配置中 `configTOTAL_HEAP_SIZE = 12 * 1024`。
- 栈溢出检测：`configCHECK_FOR_STACK_OVERFLOW = 2`。
- 高水位 API：`INCLUDE_uxTaskGetStackHighWaterMark = 1`。

这类课不能只看 main 函数，还要看 FreeRTOSConfig、platformio.ini 和 hook 函数。因为很多异常不是业务代码写错，而是 heap、stack、tick、NVIC 优先级或配置宏没有满足 API 前提。

## 5. 先建立一个最基本的脑图

```text
复位启动
  -> 配 72MHz 时钟和 GPIO
  -> 创建 memory_task，栈深度 160，优先级 1
  -> FreeRTOS 从 heap 分配 TCB 和任务栈
  -> 启动调度器
  -> memory_task 运行
  -> uxTaskGetStackHighWaterMark(NULL) 读取当前任务历史剩余栈空间
  -> 写入 volatile 全局变量 g_stack_watermark
  -> 翻转 PC13
  -> vTaskDelay(500ms) 阻塞等待下一轮
```

读这张图时，把“谁触发现象”和“现象最终改了哪个硬件状态”分开。RTOS 负责安排时机，STM32 外设负责产生可见结果，二者缺一层都解释不完整。

## 6. 先认识本课里出现的核心名词

### 6.1 `heap` 是什么

heap 是 FreeRTOS 用来动态分配内核对象、TCB 和任务栈的一块内存区域。
本课 `xTaskCreate()` 需要从 heap 里拿内存；若 heap 不足，创建失败并进入停机分支或 malloc failed hook。

### 6.2 `stack` 是什么

stack 是每个任务私有的调用栈，用来保存局部变量、返回地址、寄存器现场等。
`memory_task` 的栈深度为 160，函数调用越深、局部数组越大，栈消耗越多。

### 6.3 `TCB` 是什么

TCB 是 Task Control Block，任务控制块。内核用它记录任务状态、优先级、栈指针、通知值等信息。
创建任务时，TCB 和栈都要分配成功，任务才可能进入就绪态。

### 6.4 `uxTaskGetStackHighWaterMark` 是什么

它是 FreeRTOS 提供的栈余量观测 API。
参数为 NULL 时表示查询当前任务，也就是正在运行的 `memory_task`。

### 6.5 `g_stack_watermark` 是什么

它是保存高水位返回值的全局变量。
写成 `volatile` 是为了让调试器和主循环观察时不被编译器优化成不可见的缓存行为。

### 6.6 `UBaseType_t` 是什么

它是 FreeRTOS 的无符号基础类型，宽度随平台移植层变化。
高水位返回值用它承载，说明这是 RTOS 移植相关的计数值。

### 6.7 `NULL` 参数 是什么

`uxTaskGetStackHighWaterMark(NULL)` 的 NULL 不是空任务，而是约定为当前任务。
如果要查别的任务，需要保存目标任务句柄再传入。

### 6.8 `configCHECK_FOR_STACK_OVERFLOW` 是什么

它控制 FreeRTOS 是否检查任务栈溢出。
本课配置为 2，检查更严格；栈越界时会调用 `vApplicationStackOverflowHook()`。

### 6.9 `vApplicationMallocFailedHook` 是什么

heap 分配失败时进入这个 hook。
若任务、队列、定时器等对象创建需要内存但 heap 不足，停在这里比继续运行更容易定位。

### 6.10 `vApplicationStackOverflowHook` 是什么

任务栈溢出时进入这个 hook。
源码里关中断后死循环，方便调试器停住查看是哪个任务出错。

### 6.11 `configTOTAL_HEAP_SIZE` 是什么

它定义 FreeRTOS heap 总量。
本课为 12KB，足够简单任务；复杂工程增加任务、队列、定时器后要重新估算。

### 6.12 `160` 栈深度 是什么

`xTaskCreate(memory_task, "mem", 160, ...)` 中 160 是任务栈深度。
在 Cortex-M FreeRTOS 里通常按 StackType_t 个数计，不应直接当成 160 字节。

### 6.13 `vTaskDelay(500ms)` 是什么

它让任务进入阻塞态 500ms。
阻塞期间 CPU 可以运行 idle 或其他任务，而不是空转。

### 6.14 `PC13` 是什么

PC13 是本课运行状态的可见证据。
LED 翻转说明任务循环在执行，但不能单独证明栈余量安全，必须结合水位变量看。

### 6.15 `volatile` 是什么

volatile 告诉编译器该变量可能被调试器或异步上下文观察，不要把读写优化掉。
本课没有中断修改它，但为了 Watch 稳定观察，使用 volatile 很合理。


## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

`FreeRTOS.h` 和 `task.h` 提供任务、延时、高水位 API。
本课没有 `queue.h`、`timers.h`，说明重点不是对象通信。

### 7.2 时钟初始化

系统仍配置到 72MHz。
虽然内存 API 不直接依赖 GPIO 频率，但 `vTaskDelay()` 的 tick 节奏依赖系统时钟。

### 7.3 GPIO 初始化

PC13 配输出并初始置高。
PA0/PA1/PA2 的公共初始化存在，但本课核心现象只用 PC13。

### 7.4 g_stack_watermark 定义

`static volatile UBaseType_t g_stack_watermark;` 让变量只在本文件可见，又能被调试器观察。
Watch 里看它能知道任务历史最低剩余栈空间。

### 7.5 memory_task 查询水位

循环里先调用 `uxTaskGetStackHighWaterMark(NULL)`。
返回值越小，说明历史上栈越接近用尽；为 0 或很小就危险。

### 7.6 memory_task 翻转 PC13

查询后翻转 PC13。
LED 变化说明任务没有卡死在 hook，也说明调度器在运行。

### 7.7 memory_task 延时

`vTaskDelay(pdMS_TO_TICKS(500))` 让任务周期运行。
这比 busy loop 更符合 RTOS 工程方式。

### 7.8 xTaskCreate 栈参数

`xTaskCreate(memory_task, "mem", 160, NULL, 1, NULL)` 中 160 是估算出的任务栈。
后续应根据高水位结果决定是否需要增减。

### 7.9 创建失败处理

若 `xTaskCreate()` 不返回 `pdPASS`，源码关中断死循环。
这通常意味着 heap 不足，任务没有被创建出来。

### 7.10 两个 hook 函数

malloc failed hook 和 stack overflow hook 都会停住。
这让内存错误能被明显暴露，而不是表现成随机跑飞。

### 7.11 调度器启动

`vTaskStartScheduler()` 会创建 idle 任务，启用调度。
如果启动失败，常见原因仍然是 heap 不足。


## 8. HAL 版代码逐步讲解

### 8.1 HAL_Init

HAL 版先初始化 HAL tick 和基础状态。
FreeRTOS 启动后任务延时仍以 FreeRTOS 为准。

### 8.2 HAL 时钟结构体

RCC 两个结构体把 HSE、PLL、AHB/APB 分频和 FLASH latency 封装起来。
对应寄存器版的 RCC/FLASH 配置。

### 8.3 HAL GPIO 初始化

`GPIO_InitTypeDef` 配 PC13 输出。
`HAL_GPIO_Init()` 最终写 GPIO 配置寄存器。

### 8.4 HAL_GPIO_TogglePin

HAL 版用它翻转 PC13。
这替代寄存器版读取 ODR 再写 BSRR/BRR 的手动逻辑。

### 8.5 高水位 API 不变

`uxTaskGetStackHighWaterMark(NULL)` 在 HAL 版里完全相同。
内存管理属于 FreeRTOS，不属于 HAL。

### 8.6 volatile 变量不变

HAL 版同样应保留 `volatile` 全局变量。
观察变量的教学目的不因硬件封装变化而变化。

### 8.7 hook 函数不变

HAL 工程同样实现 malloc failed 和 stack overflow hook。
RTOS 错误处理不应被 HAL 初始化掩盖。

### 8.8 不使用 HAL_Delay

任务周期使用 `vTaskDelay()`。
RTOS 工程里不要用 HAL_Delay 代替任务阻塞。


## 9. 两个版本真正应该怎么学

寄存器版和 HAL 版不是两门课。它们做同一件事：先把芯片时钟和引脚准备好，再让 FreeRTOS 调度任务或处理同步对象，最后通过 LED、串口或调试变量验证机制。

寄存器版要重点看 `RCC`、`GPIOx->CRL/CRH`、`BSRR/BRR/ODR`、`USART1->SR/DR/BRR/CR1`、NVIC 等直接控制点。HAL 版要重点看 `GPIO_InitTypeDef`、`UART_HandleTypeDef`、`HAL_GPIO_Init()`、`HAL_UART_Init()`、`HAL_UART_Receive_IT()` 这些封装把哪些底层动作合并了。

FreeRTOS API 要单独成层理解。`xTaskCreate()`、`vTaskDelay()`、`xQueueCreate()`、`xQueueReceive()`、hook 函数、tickless hook 都不是 HAL 的一部分。它们控制任务状态和调度时机，硬件 API 只负责最终的 IO 行为。

## 10. 检验问题清单

### 10.1 heap 和 stack 有什么区别？

**答**：heap 是 FreeRTOS 分配对象和任务栈的总内存池；stack 是每个任务自己的调用栈。

### 10.2 `uxTaskGetStackHighWaterMark(NULL)` 查的是谁？

**答**：查当前正在运行的任务，本课就是 `memory_task`。

### 10.3 返回值越大越好吗？

**答**：一般表示历史剩余栈空间更多，更安全；太小说明栈接近耗尽。

### 10.4 返回值单位一定是字节吗？

**答**：不一定，通常是 stack word 个数，要结合 `StackType_t` 理解。

### 10.5 为什么变量要 volatile？

**答**：为了让调试器观察到真实写入，避免编译器优化影响 Watch 结果。

### 10.6 PC13 翻转能证明栈安全吗？

**答**：不能，只能证明任务在跑；栈是否安全要看高水位值。

### 10.7 任务创建失败通常是什么原因？

**答**：常见是 FreeRTOS heap 不足，无法分配 TCB 或任务栈。

### 10.8 栈溢出会怎样？

**答**：配置开启后会进入 `vApplicationStackOverflowHook()`，源码中关中断停住。

### 10.9 HAL 版内存 API 会变吗？

**答**：不会，FreeRTOS 内存 API 与 HAL 无关。

### 10.10 如何根据水位调整栈？

**答**：若长期余量很大可适当减小；若余量很小或为 0，应增大栈并检查局部变量。


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

PC13 大约 500ms 翻转一次；Watch 中 `g_stack_watermark` 会被周期刷新。正常情况下它不应持续接近 0。若进入 hook，LED 通常停止，说明内存或栈前提已经失败。

正常现象要能和源码里的周期、队列长度、变量赋值或中断条件对应。异常时先查是否进入 hook，再查对象句柄和任务状态，最后才查 GPIO 或串口接线。

### 12.1 六层对应关系再核对

现象层：PC13 每 500ms 翻转，Watch 里 `g_stack_watermark` 周期更新。

硬件层：PC13 只是任务仍在运行的可见输出，SRAM 才是 heap 和 stack 实际占用的物理存储。

芯片模块层：Cortex-M 使用 MSP/PSP 等栈指针机制，FreeRTOS 为每个任务维护自己的栈区域。

寄存器层：本课没有新外设寄存器，重点在内核保存任务上下文时对栈内存的使用。GPIO 寄存器只负责 LED 现象。

C/CMSIS 层：`g_stack_watermark = uxTaskGetStackHighWaterMark(NULL);` 是核心观测语句。

RTOS 层：TCB 记录任务栈边界和当前栈指针，高水位 API 通过栈填充值估算历史最低余量。

HAL/工程层：HAL 版 GPIO 封装不改变内存模型；任务栈、heap、hook 都仍由 FreeRTOS 管理。

### 12.2 本课推荐断点和 Watch

Watch 中加入 `g_stack_watermark`，观察它是否从初始值变成稳定数值。

在 `uxTaskGetStackHighWaterMark(NULL)` 后打断点，可以确认当前显示值来自本轮任务执行。

在 `vApplicationMallocFailedHook()` 打断点，可以捕获 heap 分配失败。

在 `vApplicationStackOverflowHook()` 打断点，可以捕获任务栈越界，并查看 `task_name`。

### 12.3 栈估算不要只凭感觉

任务栈太大，会浪费 heap，甚至让后续任务创建失败。

任务栈太小，可能先表现为偶发异常，而不是立刻进入 hook。

高水位值给你一个实测依据：保留足够余量后再调整，而不是凭“这个任务看起来简单”随便写数字。

如果任务后来加入 `printf`、协议解析、数组缓存或 HAL 串口发送，必须重新观察水位，因为调用链已经变了。

### 12.4 源码逐项核对

`static volatile UBaseType_t g_stack_watermark;` 同时体现了三个工程选择：静态全局便于 Watch，`volatile` 防止观察被优化干扰，`UBaseType_t` 匹配 FreeRTOS API 返回类型。

`memory_task` 里没有大数组、没有递归、没有复杂库调用，所以它是观察高水位 API 的干净样例。你后来添加任何重调用链，都应该重新测水位。

`xTaskCreate(memory_task, "mem", 160, NULL, 1, NULL)` 不保存任务句柄，因为本课只查当前任务。如果要在别的任务里查 mem 任务水位，就必须保存句柄。

`configCHECK_FOR_STACK_OVERFLOW = 2` 不是万能保护。它能提高溢出被发现的概率，但不能替代栈水位观察，也不能发现所有越界写。

### 12.5 heap 和 stack 的典型误区

把任务栈调大，会消耗更多 heap，因为任务栈本身就是从 FreeRTOS heap 里分配出来的。

所以“栈溢出”不能简单地用无限加大栈解决。加得太大，可能让别的任务或队列创建失败。

高水位很大时，可以说明这个任务栈余量充足，但不能说明整个系统 heap 充足。

heap 充足时，也不能说明每个任务栈都安全；某个任务仍可能因为局部数组或调用链太深而溢出。

### 12.6 读数如何转成工程决策

如果水位长期很大，说明当前栈配置有富余，但是否下调要看后续是否还会加功能。教学 Demo 可以保守一点，产品工程才需要精细压缩。

如果水位偶尔变小，重点看变小之前任务执行了什么函数。栈消耗通常来自调用链最高峰，而不是循环平均情况。

如果水位接近 0，不要只看 LED 是否还闪。应立即增大栈，并检查是否有局部大数组、递归、格式化输出或 HAL 深层调用。

### 12.7 为什么本课没有新通信对象

本课刻意不创建队列、信号量、事件组或软件定时器，是为了让 heap/stack 观察更干净。

如果同时创建很多对象，malloc failed hook 可能来自队列缓冲区，也可能来自任务栈，学生会很难分辨问题来源。

当前只有一个 `memory_task`，所以 `xTaskCreate()` 的栈参数、`g_stack_watermark` 的读数、hook 的触发原因更容易一一对应。

### 12.8 和真实项目的关系

真实项目里，内存问题往往不是一开始就炸，而是某条很少执行的分支突然调用了更深的函数。

所以本课的高水位观察方法，要在功能跑过主要路径后再判断，不能只看刚启动时的读数。

## 13. 常见问题排查

### 13.1 PC13 不闪

先查是否进入 malloc failed 或 stack overflow hook。
再查 `xTaskCreate()` 是否返回 `pdPASS`。

### 13.2 g_stack_watermark 看不到

确认变量是全局 `volatile`，并且编译优化没有让调试信息缺失。
确认任务已经运行到赋值语句。

### 13.3 水位值很小

说明任务历史栈余量不足。
增大 `xTaskCreate()` 的栈深度，检查局部大数组和函数调用深度。

### 13.4 进入 malloc failed hook

说明 heap 分配失败。
增大 `configTOTAL_HEAP_SIZE` 或减少任务/对象栈配置。

### 13.5 进入 stack overflow hook

说明某个任务栈越界。
查看 hook 参数里的任务名，优先检查该任务栈深度。

### 13.6 改大栈后创建失败

单个任务栈也来自 heap。
栈加太大会导致 heap 不够，反而创建失败。

### 13.7 HAL 版 LED 不动但变量变

说明 RTOS 任务在运行，重点查 HAL GPIO 配置。
PC13 硬件接法也要确认。

### 13.8 变量正常但系统偶发跑飞

高水位只能反映栈余量，不能覆盖所有内存越界。
还要查数组越界、野指针和中断栈使用。


### 13.9 高水位一开始就很低

检查任务栈深度是否被改小，或者任务启动后是否立刻调用了较深的 HAL/库函数。

如果一开始就接近 0，不要等到系统跑飞再处理，应先增大栈并重新观察。

### 13.10 高水位很大但 heap 仍失败

这说明单个任务栈余量充足，但 FreeRTOS 总 heap 可能不够。

栈水位和 heap 剩余是两个维度：一个看任务私有栈，一个看内核分配池。

### 13.11 只看 LED 会误判

PC13 翻转只能说明任务循环还能执行。

很多栈风险在 LED 正常时已经存在，所以本课必须同时看 Watch 变量。

### 13.12 hook 里 task_name 怎么用

`vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)` 的第二个参数能帮助定位哪个任务栈溢出。

本课只有 `mem` 任务，复杂工程里这个名字非常重要，创建任务时不要随便都叫同一个名字。

## 14. 本课最核心的结论

1. FreeRTOS heap 用来分配任务和内核对象。
2. 每个任务都有自己的 stack。
3. 高水位值反映历史最低剩余栈空间。
4. `NULL` 参数表示查询当前任务。
5. `volatile` 让 Watch 观察更可靠。
6. PC13 只证明任务在跑，不证明栈一定安全。
7. malloc failed hook 和 stack overflow hook 是内存问题定位入口。
8. 栈大小要根据实测水位和工程余量调整。

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

- 上一课：[56_freertos_software_timer](../56_freertos_software_timer/README.md)
- 下一课：[58_freertos_low_power_tickless](../58_freertos_low_power_tickless/README.md)
