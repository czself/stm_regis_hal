# 56_freertos_software_timer - FreeRTOS 软件定时器

## 1. 本课到底在学什么

本课表面现象是：PC13 由软件定时器回调大约每 500ms 翻转一次，PA1 由普通 monitor 任务大约每 1000ms 翻转一次。两个节奏同时存在，用来区分“定时器回调执行”和“普通任务循环执行”。

真正要学的是 FreeRTOS software timer。软件定时器不是 STM32 的 TIM 外设，它由 FreeRTOS 内核维护到期时间，再由 timer service task 执行回调函数。本课用 `xTimerCreate()` 创建自动重装定时器，用 `xTimerStart()` 把启动命令送入定时器命令队列。

这节课仍然按六层来读：先看板子上能看到的现象，再追到 STM32 的引脚和外设，再追到寄存器和 bit，再看 C/CMSIS 怎么写，最后看 HAL 版把哪些底层动作封装掉；因为本课属于 FreeRTOS 阶段，还要额外看 RTOS 对象、任务状态和调度行为。

## 2. 本课学习目标

- 能解释软件定时器为什么不是硬件 TIM。
- 能说明 `xTimerCreate()` 五个参数分别控制什么。
- 能解释 `pdTRUE` 自动重装为什么让 PC13 周期翻转。
- 能说明 timer callback 为什么必须短。
- 能解释 timer service task 和 timer command queue 的作用。
- 能区分 PC13 的 500ms 节奏和 PA1 的 1000ms 节奏。
- 能说出 `configUSE_TIMERS`、`configTIMER_TASK_PRIORITY`、`configTIMER_QUEUE_LENGTH` 的意义。
- 能把寄存器版 GPIO 翻转和 HAL 版 `HAL_GPIO_TogglePin()` 对上。

## 3. 本课目录结构

```text
56_freertos_software_timer/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两个工程做同一个实验。`reg/` 让你看到寄存器和 FreeRTOS API 怎样直接配合；`hal/` 让你看到 HAL 只替换硬件初始化和 GPIO 操作，RTOS 思路没有变。

## 4. 实验硬件与工程前提

- STM32F103C8T6 BluePill。
- PC13：软件定时器回调翻转。
- PA1：monitor 任务翻转，用来证明普通任务仍在运行。
- PA0 上拉输入、PA2 输出在初始化中保留，但本课主现象不用它们。
- 系统时钟 72MHz，FreeRTOS tick 1000Hz。
- FreeRTOS 配置必须启用 `configUSE_TIMERS = 1`。
- 定时器任务栈深度和优先级由 `configTIMER_TASK_STACK_DEPTH`、`configTIMER_TASK_PRIORITY` 决定。

编译时要同时确认 `platformio.ini`、FreeRTOS 配置头文件、源码里的头文件包含三者一致。README 不能只讲 API 名字，必须能解释为什么这些配置会影响最终现象。

## 5. 先建立一个最基本的脑图

```text
复位启动
  -> 配 72MHz 时钟和 GPIO
  -> 创建软件定时器 g_timer，周期 500ms，自动重装
  -> 创建 monitor 任务，周期 1000ms 翻转 PA1
  -> xTimerStart(g_timer, 0) 发送启动命令
  -> 启动调度器
  -> timer service task 接收命令并维护到期时间
  -> 500ms 到期后执行 timer_cb()
  -> timer_cb() 翻转 PC13
  -> monitor 独立每 1000ms 翻转 PA1
```

这张图要从上往下读：上面是复位后的工程初始化，中间是 RTOS 对象或任务建立，下面才是板子上看到的 LED、串口或调试变量变化。若现象不对，排查也按这个顺序倒着查。

## 6. 先认识本课里出现的核心名词

### 6.1 `Software Timer` 是什么

软件定时器属于 FreeRTOS 内核层，不属于 STM32 TIM 外设层。它依赖 tick 计数判断时间是否到期。

本课用它让 PC13 周期翻转，目的是学习 RTOS 层定时回调，而不是学习硬件定时器寄存器。若把它误认为 TIM 外设，就会去找 TIMx->PSC/ARR，但源码里没有这些寄存器。

### 6.2 `TimerHandle_t g_timer` 是什么

`g_timer` 是软件定时器对象句柄，保存 `xTimerCreate()` 返回的对象地址。

后续 `xTimerStart(g_timer, 0)` 必须靠它指定启动哪个定时器。若它为 NULL，说明 heap 不足或定时器创建失败，PC13 不会翻转。

### 6.3 `xTimerCreate` 是什么

`xTimerCreate("blink", pdMS_TO_TICKS(500), pdTRUE, NULL, timer_cb)` 创建一个名为 blink 的软件定时器。

周期参数决定 500ms 到期；`pdTRUE` 决定到期后自动重新计时；ID 为 NULL 表示本课不需要用 timer ID 区分多个定时器；最后一个参数是回调函数。

### 6.4 `pdMS_TO_TICKS(500)` 是什么

这个宏把毫秒转换成 FreeRTOS tick 数。tick 频率为 1000Hz 时，500ms 约等于 500 tick。

若 `configTICK_RATE_HZ` 改变，同一个毫秒值会换算成不同 tick 数；若系统 tick 异常，PC13 节奏也会异常。

### 6.5 `pdTRUE 自动重装` 是什么

`pdTRUE` 表示 auto-reload timer。定时器每次到期执行回调后，会自动安排下一次到期。

如果改成 `pdFALSE`，它会变成 one-shot timer，PC13 通常只翻转一次。

### 6.6 `timer_cb` 是什么

`timer_cb(TimerHandle_t timer)` 是软件定时器到期后执行的回调。寄存器版在里面调用 `led_toggle_pc13()`，HAL 版调用 `HAL_GPIO_TogglePin()`。

回调运行在 timer service task 上，不是独立任务，也不是硬件中断。回调里不能长时间阻塞，否则会拖住其他软件定时器命令和回调。

### 6.7 `timer service task` 是什么

timer service task 是 FreeRTOS 为软件定时器创建的内部任务。它从定时器命令队列取启动/停止/重置命令，并在定时器到期时调用回调。

本课你没有显式创建它，但启用 `configUSE_TIMERS` 后，内核会在调度器启动时处理相关机制。

### 6.8 `timer command queue` 是什么

软件定时器 API 通常不是直接立刻修改所有内部状态，而是向定时器命令队列发送命令。

`xTimerStart(g_timer, 0)` 的第二个参数是发送命令时愿意等待队列空间的 tick 数。本课填 0，表示不等待，失败就进入停机分支。

### 6.9 `monitor 任务` 是什么

monitor 是普通 FreeRTOS 任务，每 1000ms 翻转 PA1。

它用来和 PC13 区分：PA1 证明普通任务调度仍在运行，PC13 证明软件定时器回调在运行。

### 6.10 `configUSE_TIMERS` 是什么

`configUSE_TIMERS` 必须为 1，软件定时器 API 才可用。

如果关闭它，`timers.h` 里的相关功能不可用或链接失败，本课无法构建。

### 6.11 `configTIMER_TASK_PRIORITY` 是什么

它控制 timer service task 的优先级。优先级太低时，如果高优先级任务长期占用 CPU，软件定时器回调会延迟。

本课 monitor 优先级为 1，定时器任务常见配置为 2，因此回调能比较及时执行。

### 6.12 `configTIMER_QUEUE_LENGTH` 是什么

它控制定时器命令队列能缓存多少启动、停止、重置命令。

本课只启动一个定时器，长度 5 足够；复杂工程里多个任务频繁操作定时器时，队列满会导致 API 返回失败。

### 6.13 `PC13 和 PA1` 是什么

PC13 是软件定时器回调的可见输出，PA1 是普通任务的可见输出。

若 PA1 正常而 PC13 不动，优先查软件定时器；若二者都不动，优先查调度器、时钟、GPIO 或 hook。

### 6.14 `malloc failed hook` 是什么

软件定时器对象、任务 TCB、任务栈都依赖 heap。

本课检查 `g_timer == NULL` 和任务创建返回值，失败后停住，避免调度器启动后才出现更难看的异常。

### 6.15 `stack overflow hook` 是什么

timer callback 虽短，但 monitor 任务仍有栈。

栈配置过小或回调中加入复杂调用，都可能触发栈问题。hook 停住能让调试器看到错误。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件为什么包含 timers.h

`timers.h` 提供 `TimerHandle_t`、`xTimerCreate()`、`xTimerStart()` 等声明。

如果只包含 `task.h`，任务 API 可用，但软件定时器 API 没有声明。

### 7.2 72MHz 时钟初始化

寄存器版仍从 HSE、PLL、FLASH latency 开始。

软件定时器依赖 tick，tick 又依赖系统时钟配置；时钟错误会让 500ms 和 1000ms 变得不准。

### 7.3 GPIO 初始化

PC13 配成输出并初始置高；PA1、PA2 配成输出；PA0 配成上拉输入。

当前主现象使用 PC13 和 PA1。PA2、PA0 是课程工程公共初始化遗留，不影响定时器机制。

### 7.4 led_toggle_pc13

函数读取 `GPIOC->ODR` 判断当前输出状态，再写 `BRR` 或 `BSRR` 翻转。

这一步是硬件层现象发生的位置；FreeRTOS 只决定什么时候执行它。

### 7.5 led_toggle_pa1

monitor 任务调用该函数翻转 PA1。

如果 PA1 每秒翻转，说明普通任务调度和 `vTaskDelay()` 是活的。

### 7.6 创建 g_timer

`xTimerCreate()` 返回值写入 `g_timer`。

这里没有直接启动定时器，创建只是分配对象并设置参数。

### 7.7 创建 monitor

`xTaskCreate(monitor, "mon", 128, NULL, 1, NULL)` 创建普通任务。

它不需要被其他任务引用，所以最后一个参数为 NULL。

### 7.8 启动定时器

`xTimerStart(g_timer, 0)` 向定时器命令队列发送启动命令。

第二个参数为 0，说明如果命令队列暂时没有空间，不阻塞等待。

### 7.9 失败分支

源码同时检查 `g_timer == NULL`、`ok != pdPASS`、`xTimerStart() != pdPASS`。

任一失败都关中断停住，说明工程没有满足运行前提。

### 7.10 timer_cb 执行位置

回调不是中断函数，不需要 FromISR API。

它运行在 FreeRTOS 定时器服务任务里，因此应短小，不能写死循环或长时间等待。

### 7.11 vTaskStartScheduler

调度器启动后，monitor 和 timer service task 才会按内核规则运行。

若调度器启动失败，通常是 heap 不够创建 idle/timer 任务。

## 8. HAL 版代码逐步讲解

### 8.1 HAL_Init 与时钟

HAL 版先调用 `HAL_Init()`，再用 `RCC_OscInitTypeDef` 和 `RCC_ClkInitTypeDef` 配 72MHz。

这对应寄存器版 FLASH、RCC->CR、RCC->CFGR 的一组操作。

### 8.2 HAL GPIO

`GPIO_InitTypeDef` 把 PC13、PA1、PA2 配成推挽输出，把 PA0 配成上拉输入。

`HAL_GPIO_Init()` 最终仍会写 GPIO CRL/CRH 等寄存器。

### 8.3 HAL 版 timer_cb

回调里调用 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`。

这和寄存器版 `led_toggle_pc13()` 目标相同，都是改变 PC13 输出电平。

### 8.4 HAL 版 monitor

monitor 调用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)` 后 `vTaskDelay(1000ms)`。

HAL 只负责翻转引脚，任务阻塞仍由 FreeRTOS 完成。

### 8.5 xTimerCreate 不属于 HAL

HAL 版仍直接调用 FreeRTOS 的 `xTimerCreate()`。

不要把 HAL 工程误解成所有功能都由 HAL 管；RTOS 对象仍由 FreeRTOS 管。

### 8.6 xTimerStart 返回值

HAL 版同样检查 `xTimerStart()` 是否为 `pdPASS`。

这一步能暴露定时器命令队列或对象创建问题。

### 8.7 timer 参数未使用

`timer_cb(TimerHandle_t timer)` 中 `(void)timer` 是为了消除未使用参数警告。

如果以后多个定时器共用一个回调，可以用这个参数区分来源。

### 8.8 HAL_Delay 不该出现在任务循环

monitor 使用 `vTaskDelay()`，不是 `HAL_Delay()`。

RTOS 工程中任务等待应交给调度器，这样其他任务和定时器服务任务才能运行。

## 9. 两个版本真正应该怎么学

不要把寄存器版和 HAL 版当成两套互不相关的知识。它们面对的是同一颗 STM32F103、同一组引脚、同一个 FreeRTOS 内核，只是硬件初始化的表达方式不同。

寄存器版的价值是把时钟、GPIO、USART、NVIC、状态标志这些动作摊开；HAL 版的价值是把同样的动作组织成结构体字段和 API 调用。RTOS 层的任务、阻塞、唤醒、对象句柄、hook 函数，在两个版本里都应该用同一套逻辑理解。

读代码时可以先问三个问题：第一，现象由哪个任务或回调触发；第二，这个任务为什么会在这个时刻运行；第三，最终落到哪个硬件寄存器或 HAL API 改变了引脚/外设状态。能回答这三个问题，说明这节课不是只会照抄。

## 10. 检验问题清单

### 10.1 软件定时器是不是 STM32 TIM？

**答**：不是。它是 FreeRTOS 内核对象，依赖 tick 和 timer service task，不配置 TIMx 寄存器。

### 10.2 为什么 PC13 是 500ms，PA1 是 1000ms？

**答**：PC13 由 500ms 软件定时器回调翻转，PA1 由 monitor 任务每 1000ms 翻转。

### 10.3 `pdTRUE` 改成 `pdFALSE` 会怎样？

**答**：定时器变成单次定时器，到期执行一次回调后停止，PC13 通常只翻转一次。

### 10.4 `xTimerStart(g_timer, 0)` 的 0 是什么？

**答**：它是向定时器命令队列发送启动命令时的等待时间，0 表示不等待。

### 10.5 timer callback 能不能做很耗时的事？

**答**：不应该。它运行在 timer service task，耗时会延迟其他定时器回调和命令处理。

### 10.6 为什么要检查 `g_timer == NULL`？

**答**：软件定时器对象需要 heap，创建失败会返回 NULL，不检查会导致后续启动无效对象。

### 10.7 HAL 版的软件定时器 API 变了吗？

**答**：没有。HAL 只替换硬件操作，软件定时器仍是 FreeRTOS API。

### 10.8 PC13 不动但 PA1 正常说明什么？

**答**：普通任务调度正常，问题更可能在软件定时器创建、启动、配置或 timer service task。

### 10.9 PA1 和 PC13 都不动先查什么？

**答**：先查任务创建、调度器启动、是否进入 malloc/stack hook，再查 GPIO 和时钟。

### 10.10 `configTIMER_QUEUE_LENGTH` 太小会怎样？

**答**：多个定时器命令可能发送失败，`xTimerStart`、reset、stop 等 API 可能返回非 `pdPASS`。

## 11. 工程实现步骤

### 11.1 需求分析

先用一句话定义需求：本课不是单纯让 LED 动起来，而是用一个具体现象验证某个 FreeRTOS 机制和 STM32 外设链路。需求越清楚，后面越不容易把所有 API 混在一起。

### 11.2 硬件核查

确认 PC13、PA1 等本课涉及的引脚是否真的接到可观察对象；确认 ST-Link 下载正常。

### 11.3 寄存器路线

先配置 72MHz 时钟，再开对应 GPIO/USART 时钟，再配置引脚模式和外设寄存器，最后进入 FreeRTOS 创建任务或对象。寄存器路线的重点是每一步都能在参考手册里找到对应位置。

### 11.4 HAL 路线

HAL 路线先 `HAL_Init()`，再用 RCC/GPIO/UART 结构体配置硬件。HAL 并没有替代 FreeRTOS，任务创建、阻塞等待、对象发送接收仍然直接调用 FreeRTOS API。

### 11.5 工程思维

把耗时动作放到任务里，把短动作放到中断或回调里；把共享状态变成明确的 RTOS 对象；把失败路径接到 hook 或停机点。这样调试时能看到问题停在哪一层。

### 11.6 常见工程陷阱

常见问题包括：对象创建失败不检查、任务栈给得太小、在 ISR 里调用非 FromISR API、NVIC 优先级不符合 FreeRTOS 规则、用 HAL_Delay 代替 RTOS 延时、只看现象不查句柄和返回值。

## 12. 运行现象

本课表面现象是：PC13 由软件定时器回调大约每 500ms 翻转一次，PA1 由普通 monitor 任务大约每 1000ms 翻转一次。两个节奏同时存在，用来区分“定时器回调执行”和“普通任务循环执行”。

正常时，现象应该稳定、可重复，并且和源码里的延时、周期、队列长度或中断触发条件对应。异常时不要先猜 API 名字，先确认任务有没有创建成功、调度器有没有启动、硬件初始化有没有真的执行到。

### 12.1 六层对应关系再核对

现象层：PC13 每 500ms 翻转，PA1 每 1000ms 翻转。

硬件层：PC13 和 PA1 都是 GPIO 输出，硬件本身不知道“软件定时器”这个概念。

芯片模块层：GPIO 模块负责输出，SysTick/内核 tick 提供 FreeRTOS 时间基准。

寄存器层：寄存器版最终通过 ODR 判断，再写 BSRR/BRR 翻转引脚。

C/CMSIS 层：`timer_cb()` 是 PC13 翻转入口，`monitor()` 是 PA1 翻转入口。

RTOS 层：timer service task 处理定时器命令队列，到期后调用回调；monitor 是普通任务，靠 `vTaskDelay()` 周期运行。

HAL/工程层：HAL 版只替换 GPIO 翻转和初始化，`xTimerCreate()`、`xTimerStart()`、回调运行规则不变。

### 12.2 本课推荐断点

第一个断点放在 `xTimerCreate()` 后，看 `g_timer` 是否非 NULL。

第二个断点放在 `xTimerStart()` 返回处，看启动命令是否成功送入定时器命令队列。

第三个断点放在 `timer_cb()`，验证 PC13 的 500ms 节奏确实来自软件定时器回调。

第四个断点放在 `monitor()` 的 PA1 翻转处，验证普通任务和软件定时器服务任务是两条不同执行路径。

### 12.3 必须核对的配置项

`configUSE_TIMERS` 必须为 1，否则软件定时器功能不可用。

`configTIMER_TASK_PRIORITY` 决定回调执行是否容易被其他任务延迟。

`configTIMER_QUEUE_LENGTH` 决定启动、停止、重置等命令能排队多少个。

`configTIMER_TASK_STACK_DEPTH` 决定 timer service task 的栈余量，回调越复杂越需要关注。

这些配置不是背景知识，而是直接决定本课 PC13 是否按 500ms 稳定翻转。

### 12.4 源码逐项核对

`g_timer` 是静态全局句柄，创建成功后一直指向同一个软件定时器对象。不要在局部变量里接收返回值后丢掉，否则后续无法启动、停止或重置定时器。

`xTimerCreate()` 的名字参数 `"blink"` 主要用于调试识别，不决定硬件行为。真正影响 PC13 节奏的是周期、自动重装参数和回调函数。

`xTimerStart(g_timer, 0)` 在调度器启动前调用也可以，因为它发送的是定时器命令；调度器启动后 timer service task 才真正处理这些命令。

`timer_cb()` 里 `(void)timer` 表示本课暂时不用定时器句柄。以后多个 timer 共用回调时，这个参数就不应该忽略。

## 13. 常见问题排查

### 13.1 PC13 完全不翻转

先看 `g_timer` 是否为 NULL，再看 `xTimerStart()` 是否返回 `pdPASS`。

如果 PA1 正常，GPIO 和调度器大概率没坏，重点查软件定时器配置。

### 13.2 PC13 只翻转一次

检查 `xTimerCreate()` 第三个参数是否为 `pdTRUE`。

若误改为 `pdFALSE`，定时器到期一次后不会自动重装。

### 13.3 PC13 节奏明显不准

检查 `configTICK_RATE_HZ`、系统时钟和 `pdMS_TO_TICKS(500)`。

tick 不准时，软件定时器所有周期都会偏。

### 13.4 PA1 不翻转

检查 monitor 任务创建返回值和是否进入 hook。

再查 PA1 GPIO 模式是否输出。

### 13.5 进入 malloc failed hook

软件定时器对象、timer service task、monitor 任务都要 heap。

减小对象数量或增大 `configTOTAL_HEAP_SIZE`。

### 13.6 进入 stack overflow hook

检查 monitor 栈和 timer task 栈。

如果在回调里加入复杂代码，更要增大 `configTIMER_TASK_STACK_DEPTH`。

### 13.7 编译找不到 xTimerCreate

确认包含 `timers.h`，并且 `configUSE_TIMERS` 为 1。

只包含 `task.h` 不够。

### 13.8 定时器启动失败

检查定时器命令队列长度和调度器启动前后的 API 调用位置。

本课启动命令等待时间为 0，队列满时不会等待。

### 13.9 timer service task 优先级过低

如果你把 `configTIMER_TASK_PRIORITY` 改得很低，而其他任务长期处于就绪态，定时器回调会延迟。

本课 monitor 任务优先级为 1，常见配置下 timer service task 优先级为 2，所以 PC13 的 500ms 节奏应比普通低优先级任务更稳定。

### 13.10 回调里误用阻塞 API

timer callback 运行在定时器服务任务里，不是给每个定时器单独创建一个任务。

若在回调里做长阻塞，等于把整个软件定时器服务任务卡住，后续 start、stop、reset 和其他回调都会受影响。

### 13.11 把软件定时器和 TIM 外设混淆

本课源码没有 `TIMx->PSC`、`TIMx->ARR`、`HAL_TIM_Base_Start_IT()`。

所以排查 500ms 不准时，不应该先找 TIM 寄存器，而应该查 FreeRTOS tick、系统时钟和软件定时器配置。

### 13.12 多个定时器共用回调时怎么区分

本课 timer ID 是 NULL，回调也忽略 `timer` 参数，因为只有一个软件定时器。

如果以后多个定时器共用同一个 callback，应通过 `timer` 句柄或 timer ID 判断来源，不能只靠全局变量猜。

## 14. 本课最核心的结论

1. 软件定时器是 FreeRTOS 机制，不是 STM32 硬件 TIM。
2. `xTimerCreate()` 只创建对象，`xTimerStart()` 才发送启动命令。
3. `pdTRUE` 自动重装让 PC13 每 500ms 重复翻转。
4. timer callback 运行在 timer service task，必须短小。
5. PA1 的 monitor 任务用于对照普通任务调度。
6. 定时器 API 失败常见原因是 heap、配置宏或命令队列。
7. 寄存器版和 HAL 版的 RTOS 层完全相同。
8. 排错时先分清是定时器问题、任务问题还是 GPIO 问题。

## 15. 建议你现在怎么读这节课

先读第 5 章脑图，把现象和任务/对象关系串起来；再读第 6 章名词，不要跳过句柄、返回值和阻塞参数；接着读第 7、8 章，把寄存器版和 HAL 版逐句对应。

最后用第 13 章做反向训练：假设 PC13 不动、串口没回显、变量不变或进入 hook，你应该能说出先查哪一层、再查哪一层。能这样排查，才算真正掌握。

## 16. 扩展练习

1. 把周期参数改大一倍，观察现象是否按预期变慢。
2. 故意减小任务栈，观察是否进入 stack overflow hook 或变量异常。
3. 把创建对象的返回值保存到 Watch，确认失败时停在哪。
4. 在关键 API 前后打断点，观察任务阻塞和唤醒顺序。
5. 把寄存器版和 HAL 版同一动作逐句对应写在笔记里。

## 17. 下一课预告

- 上一课：[55_freertos_task_notification](../55_freertos_task_notification/README.md)
- 下一课：[57_freertos_memory_management](../57_freertos_memory_management/README.md)
