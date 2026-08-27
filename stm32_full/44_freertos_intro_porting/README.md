# 第 44 课：FreeRTOS 入门移植与基础调度

## 1. 本课到底在学什么

本课表面现象是：两个 LED 节奏同时存在。PC13 由 `led_task` 每 500ms 翻转一次，PA1 由 `heartbeat_task` 每 1000ms 翻转一次。

真正要学的是 FreeRTOS 最小运行链路：

```text
配置 72MHz 系统时钟
  -> 初始化 GPIO 输出
  -> FreeRTOSConfig.h 告诉内核 CPU 频率、tick、heap、优先级规则
  -> xTaskCreate() 创建任务控制块和任务栈
  -> vTaskStartScheduler() 启动调度器
  -> SVC 启动第一个任务
  -> SysTick 产生 1ms 节拍
  -> vTaskDelay() 让任务进入阻塞态
  -> PendSV 在合适时机切换任务上下文
  -> 两个 LED 按不同节奏翻转
```

前面裸机课程里，`while(1)` 只有一个主循环。到了 FreeRTOS，多个任务函数看起来都写了 `while(1)`，但它们不是同时占用 CPU，而是由调度器按优先级和阻塞状态轮流运行。

如果你是从 00–43 课一路学过来的，可能会问：前面裸机也能用中断 + 状态机把多件事跑起来，为什么还需要 RTOS？答案是：裸机 super-loop 适合事件少、时序简单的场景；当系统要同时处理多个独立且可能阻塞的链路（例如一边等串口命令、一边等按键、一边周期采样 ADC），靠一个主循环加全局状态机很容易把代码写成“一大团 if-else”，响应时间也越来越难保证。RTOS 把每条独立链路拆成一个任务，任务自己管理等待和节奏，调度器保证高优先级任务及时得到 CPU。本课先不处理复杂同步，只建立最小运行链路，让你确认 FreeRTOS 能在板子上跑起来。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么 FreeRTOS 任务函数可以写成各自的 `while(1)`？
2. `xTaskCreate()` 的任务函数、名字、栈深度、参数、优先级、句柄分别是什么意思？
3. 为什么必须检查 `xTaskCreate()` 返回值？
4. `vTaskStartScheduler()` 启动后，为什么 `main()` 后面的 `while(1)` 正常情况下不会再执行？
5. `vTaskDelay(pdMS_TO_TICKS(500))` 为什么不是普通忙等待？
6. `FreeRTOSConfig.h` 里的 `configCPU_CLOCK_HZ` 和真实 72MHz 时钟为什么必须一致？
7. `vApplicationMallocFailedHook()` 和 `vApplicationStackOverflowHook()` 分别代表什么错误？
8. reg/hal 两个版本中，FreeRTOS 部分为什么几乎一样？

## 3. 本课目录结构

```text
44_freertos_intro_porting/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 用寄存器配置时钟和 GPIO，然后创建 FreeRTOS 任务。
`hal/` 用 HAL 配置时钟和 GPIO，然后创建同样的 FreeRTOS 任务。

两个版本的 FreeRTOS API 一样，差别主要在 STM32 外设初始化层。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- LED1：PC13，板载 LED，常见为低电平点亮
- LED2：PA1，可外接 LED 或用调试器观察 ODR
- PA2 在寄存器版初始化为输出，但本课任务没有实际翻转它
- PA0 在两个版本里配置为输入上拉，但本课没有按键任务

本课的主要观察点是 PC13 和 PA1 两个不同节奏。若没有外接 PA1 LED，也可以在调试器里观察 GPIOA ODR1。

## 5. 先建立一个最基本的脑图

```text
硬件初始化
  -> 72MHz 时钟
  -> PC13 输出
  -> PA1 输出

FreeRTOS 配置
  -> configCPU_CLOCK_HZ = 72000000
  -> configTICK_RATE_HZ = 1000
  -> configTOTAL_HEAP_SIZE = 12KB
  -> SVC/PendSV/SysTick 映射到 FreeRTOS port

任务创建
  -> led_task，栈 128 word，优先级 2，翻转 PC13，延时 500ms
  -> heartbeat_task，栈 128 word，优先级 1，翻转 PA1，延时 1000ms
  -> 任一创建失败则关中断停住

调度运行
  -> vTaskStartScheduler()
  -> SysTick 每 1ms 更新 tick
  -> vTaskDelay() 把任务放入阻塞态
  -> 到期后任务回到就绪态
  -> 调度器选择最高优先级就绪任务运行
```

这条链路里，GPIO 负责可见输出，FreeRTOS 负责“哪个任务什么时候运行”，Cortex-M 异常负责底层上下文切换。

## 6. 先认识本课里出现的核心名词

### 6.1 `FreeRTOS` 是什么

FreeRTOS 是一个实时操作系统内核。

它不替你配置 GPIO、RCC、USART 这类 STM32 外设；它负责创建任务、管理任务状态、提供延时、调度和内核对象。本课用它让两个 LED 任务按不同周期运行。

### 6.2 `FreeRTOSConfig.h` 是什么

`FreeRTOSConfig.h` 是 FreeRTOS 的编译期配置文件。

本课配置了 `configCPU_CLOCK_HZ=72000000`、`configTICK_RATE_HZ=1000`、`configTOTAL_HEAP_SIZE=12*1024`、`configMAX_PRIORITIES=5`，还打开了 malloc failed hook 和 stack overflow hook。配置错会直接影响 tick 时间、任务创建和错误处理。

### 6.3 `任务` 是什么

任务是 FreeRTOS 调度的基本执行单元。

本课有两个任务：`led_task` 控制 PC13，`heartbeat_task` 控制 PA1。每个任务都有自己的函数、栈、优先级和运行状态。

### 6.4 `TCB` 是什么

TCB 是 Task Control Block，任务控制块。

它保存任务状态、栈指针、优先级等调度需要的信息。`xTaskCreate()` 成功时，FreeRTOS 会从 heap 里分配 TCB 和任务栈。

### 6.5 `任务栈` 是什么

任务栈是每个任务独立使用的栈空间。

本课给两个任务都传入 `128`，单位是 FreeRTOS 的 stack depth，通常按 `StackType_t` 个数理解，不是简单 128 字节。栈太小可能进入 `vApplicationStackOverflowHook()`。

### 6.6 `xTaskCreate()` 是什么

`xTaskCreate()` 是动态创建任务的 API。

本课调用：

```c
xTaskCreate(led_task, "led", 128, NULL, 2, NULL);
xTaskCreate(heartbeat_task, "beat", 128, NULL, 1, NULL);
```

参数依次表示任务函数、任务名、栈深度、传给任务的参数、优先级、任务句柄输出位置。返回不是 `pdPASS` 时，任务没有创建成功。

### 6.7 `vTaskStartScheduler()` 是什么

`vTaskStartScheduler()` 启动 FreeRTOS 调度器。

它会创建空闲任务，配置 tick，并触发第一次任务上下文切换。正常启动后，CPU 进入任务世界，`main()` 后面的死循环只是兜底，不应该正常运行到。

### 6.8 `vTaskDelay()` 是什么

`vTaskDelay()` 让当前任务阻塞指定 tick 数。

它不是忙等待。调用后当前任务让出 CPU，调度器可以运行其他就绪任务。本课两个 LED 能互不耽误，就是因为任务延时期间不占 CPU。

### 6.9 `pdMS_TO_TICKS()` 是什么

`pdMS_TO_TICKS(ms)` 把毫秒转换成 FreeRTOS tick 数。

本课 `configTICK_RATE_HZ=1000`，所以 1 tick 约等于 1ms，`pdMS_TO_TICKS(500)` 约为 500 tick。若 tick 频率改了，这个宏能保持代码按毫秒表达。

### 6.10 `优先级` 是什么

FreeRTOS 任务优先级数字越大，任务优先级越高。

本课 `led_task` 优先级 2，高于 `heartbeat_task` 的 1。但两个任务大部分时间都在 `vTaskDelay()` 阻塞，所以你能看到两个节奏都存在。

### 6.11 `SysTick` 是什么

SysTick 是 Cortex-M 内核定时器。

本课通过 `FreeRTOSConfig.h` 把 `xPortSysTickHandler` 映射为 `SysTick_Handler`。调度器启动后，SysTick 负责产生 RTOS tick，让延时任务到期并触发调度判断。

### 6.12 `SVC / PendSV` 是什么

SVC 和 PendSV 是 Cortex-M 异常。

FreeRTOS 用 SVC 启动第一个任务，用 PendSV 做上下文切换。配置文件里：

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
```

这让启动文件里的异常入口能进入 FreeRTOS port。

### 6.13 `heap_4` 是什么

`heap_4.c` 是 FreeRTOS 的一种动态内存管理实现。

本工程使用 `lib/FreeRTOS-Kernel/portable/MemMang/heap_4.c`。动态创建任务时，TCB 和任务栈从 FreeRTOS heap 分配。heap 不足时会进入 malloc failed hook。

### 6.14 `Hook` 是什么

Hook 是 FreeRTOS 在特定错误或时机调用的用户函数。

本课实现了 `vApplicationMallocFailedHook()` 和 `vApplicationStackOverflowHook()`。它们会关中断并停住，让错误停在明确位置。

### 6.15 `阻塞态` 是什么

阻塞态是任务暂时不能运行的状态。

本课两个任务调用 `vTaskDelay()` 后进入阻塞态，直到 tick 到期才回到就绪态。阻塞态不是死循环等待，它会把 CPU 让给其他任务或空闲任务。

### 6.16 `就绪态` 是什么

就绪态表示任务已经具备运行条件，只是在等待调度器分配 CPU。

当 `led_task` 的 500ms 延时到期，它回到就绪态；如果此时没有更高优先级任务占用 CPU，它就会运行并翻转 PC13。

### 6.17 `空闲任务` 是什么

空闲任务是调度器自动创建的最低优先级任务。

当用户任务都在 `vTaskDelay()` 阻塞时，空闲任务运行。`vTaskStartScheduler()` 若无法创建空闲任务，通常会返回，说明 heap 可能不足。

### 6.18 `taskDISABLE_INTERRUPTS()` 是什么

这是 FreeRTOS 提供的关中断宏。

本课在任务创建失败或 Hook 中调用它，然后进入死循环。这样做是为了让系统停在明确错误现场，避免继续运行造成更混乱的现象。

## 7. 寄存器版代码逐步讲解

### 7.1 系统时钟

寄存器版先配置 Flash 等待周期、HSE、PLL x9、APB1 二分频，最终系统时钟为 72MHz。

这必须和 `FreeRTOSConfig.h` 里的 `configCPU_CLOCK_HZ` 一致，否则 tick 和延时都会失真。

### 7.2 GPIO 时钟

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
```

GPIOC 用于 PC13，GPIOA 用于 PA0/PA1/PA2。AFIO 在本课没有进一步用于 EXTI 映射，但被一并打开，不影响任务调度主线。

### 7.3 PC13 输出

清 `GPIOC->CRH` 的 PC13 配置，再设置 `MODE13_1`，把 PC13 配成 2MHz 推挽输出。

`GPIOC->BSRR = GPIO_BSRR_BS13` 先输出高电平，让常见 BluePill 板载 LED 熄灭。

### 7.4 PA0/PA1/PA2 配置

代码清 PA0、PA1、PA2 的配置位。PA0 设置输入上拉，PA1/PA2 设置输出。

本课任务实际翻转 PA1。PA2 在寄存器版只配置成输出，没有对应的翻转函数，也没有任务驱动它，所以 README 不能把 PA2 写成当前运行现象。

### 7.5 LED 翻转函数

`led_toggle_pc13()` 和 `led_toggle_pa1()` 都读取 ODR，再根据当前状态写 BRR 或 BSRR。

这和前面裸机 GPIO 课程一致，只是现在这些函数被任务调用，而不是主循环直接调用。

### 7.6 错误 Hook

`vApplicationMallocFailedHook()` 表示动态内存分配失败。`vApplicationStackOverflowHook()` 表示检测到任务栈溢出。

两个 Hook 都关中断并停在死循环，便于调试器定位错误。

### 7.7 `led_task`

```c
while(1) {
    led_toggle_pc13();
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

任务先翻转 PC13，再阻塞约 500ms。阻塞期间 CPU 可以运行其他就绪任务或空闲任务。

### 7.8 `heartbeat_task`

该任务翻转 PA1，然后阻塞约 1000ms。

它优先级低于 `led_task`，但因为 `led_task` 大部分时间在阻塞，所以 PA1 仍能按 1 秒节奏运行。

### 7.9 创建任务并检查返回值

代码用 `BaseType_t ok` 保存创建结果：

```c
ok=xTaskCreate(...);
ok &= xTaskCreate(...);
if(ok!=pdPASS){ taskDISABLE_INTERRUPTS(); while(1){} }
```

只要有任务创建失败，就不启动调度器。常见原因是 heap 不足或栈配置不合理。

### 7.10 启动调度器

`vTaskStartScheduler()` 启动调度器。正常情况下它不会返回。

如果它返回，通常表示空闲任务创建失败，多半还是 heap 不足。

### 7.11 `configCPU_CLOCK_HZ` 与时钟函数的对应

寄存器版把系统时钟配置到 72MHz，`FreeRTOSConfig.h` 也写 `configCPU_CLOCK_HZ (72000000UL)`。

这两个值必须一致。FreeRTOS port 用 CPU 频率配置 SysTick 重装值，进而产生 1ms tick。若真实时钟不是 72MHz，`pdMS_TO_TICKS(500)` 对应的实际时间就会偏差。

### 7.12 `pdMS_TO_TICKS()` 在任务里的位置

两个任务都在翻转 LED 后调用 `vTaskDelay(pdMS_TO_TICKS(...))`。

这意味着 LED 翻转动作很短，任务随后阻塞。若把耗时业务放在 `vTaskDelay()` 前面，任务实际周期会变成“业务耗时 + delay 时间”。

### 7.13 为什么本课没有任务句柄

`xTaskCreate()` 最后一个参数传 `NULL`，表示不保存任务句柄。

本课只需要创建后让任务自动运行，不需要后续删除、挂起或改优先级。下一课开始若要管理任务生命周期，就需要保存句柄。

### 7.14 PA2 为什么没有现象

寄存器版把 PA2 配置成输出，但没有提供翻转函数，也没有任何任务驱动它。

所以 README 不能写 PA2 会闪。配置了一个引脚，不等于它参与运行链路；判断运行现象必须看任务调用关系。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()`

HAL 版先初始化 HAL 基础环境。注意本课启动 FreeRTOS 后，系统 tick 会由 FreeRTOS port 接管。

不要把裸机里 `HAL_Delay()` 的思维直接套到任务调度里。本课任务延时使用 `vTaskDelay()`。

### 8.2 HAL 时钟配置

`RCC_OscInitTypeDef` 配 HSE 和 PLL x9，`RCC_ClkInitTypeDef` 配 SYSCLK、AHB、APB1、APB2。

它对应寄存器版的 RCC 配置，也必须和 `configCPU_CLOCK_HZ` 保持一致。

### 8.3 HAL GPIO 配置

HAL 版打开 GPIOC/GPIOA 时钟。PC13 配成输出，PA1/PA2 配成输出，PA0 配成输入上拉。

本课任务实际使用 PC13 和 PA1；PA0/PA2 是预留硬件资源，不是当前调度现象的核心。

### 8.4 HAL 版任务函数

HAL 版 `led_task` 调用 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`，`heartbeat_task` 调用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)`。

任务延时仍然是 `vTaskDelay()`，说明 RTOS 层逻辑和 HAL 无关。

### 8.5 `xTaskCreate()` 在 HAL 版里不变

HAL 只封装 STM32 外设初始化，不封装 FreeRTOS 任务创建。

因此 HAL 版同样要检查 `xTaskCreate()` 返回值，同样可能因为 heap 或栈不足失败。

### 8.6 `vTaskStartScheduler()` 在 HAL 版里不变

启动调度器的动作和寄存器版一致。

调度器启动后，两个任务按阻塞时间交替变成就绪态，LED 节奏由 FreeRTOS tick 决定。

### 8.7 Hook 在 HAL 版里不变

HAL 版也实现了 malloc failed 和 stack overflow hook。

这说明内核错误处理不属于 HAL 外设层。任务栈和 heap 是 RTOS 层资源。

### 8.8 HAL 与 FreeRTOS 的边界

HAL 负责把 GPIO 配好，FreeRTOS 负责安排任务何时调用 `HAL_GPIO_TogglePin()`。

若 LED 不闪，要分层排查：先看任务是否创建和调度，再看 GPIO 是否配置正确。

### 8.9 HAL 版为什么没有 `SysTick_Handler()`

HAL 版源码没有自己写 `SysTick_Handler()`，因为 FreeRTOS 配置里把 `xPortSysTickHandler` 映射为 `SysTick_Handler`。

调度器启动后，SysTick 由 FreeRTOS port 处理。这个点和前面的裸机 HAL 课程不同，不能再简单认为 `SysTick_Handler()` 一定只调用 `HAL_IncTick()`。

### 8.10 HAL 版为什么不用 `HAL_Delay()`

任务里用的是 `vTaskDelay()`，不是 `HAL_Delay()`。

在 RTOS 任务中，`vTaskDelay()` 能让当前任务阻塞并让出 CPU。若在任务里大量使用裸机式阻塞延时，会削弱调度器的意义。

### 8.11 HAL GPIO 字段和寄存器版对应关系

`GPIO_MODE_OUTPUT_PP` 对应推挽输出，`GPIO_PULLUP` 对应输入上拉，`GPIO_SPEED_FREQ_LOW` 对应低速输出配置。

HAL 版虽然没有直接写 `CRH/CRL/ODR`，但底层仍然表达同样的 GPIO 模式。PA0 上拉在当前课没有任务使用，但源码确实配置了它。

### 8.12 `HAL_GPIO_TogglePin()` 在任务中的代价

`HAL_GPIO_TogglePin()` 是普通函数调用，适合本课 LED 演示。

在高频实时任务里，HAL 函数开销可能比直接寄存器操作大。当前任务周期是 500ms/1000ms，这个开销完全可以忽略。

## 9. 两个版本真正应该怎么学

寄存器版让你看到 STM32 外设初始化的底层动作；HAL 版让你看到这些动作被结构体和 API 封装。FreeRTOS 部分在两个版本中几乎一样。

所以本课不要纠结“RTOS 有没有寄存器版”。RTOS 是软件内核，它运行在 Cortex-M 异常和栈切换机制之上；reg/hal 区别主要是外设初始化写法。

## 10. 检验问题清单

### 10.1 两个任务为什么不会互相卡死？

**答**：任务调用 `vTaskDelay()` 后进入阻塞态，让出 CPU。另一个任务到期就绪时可以运行。

### 10.2 `led_task` 优先级更高，PA1 为什么还能闪？

**答**：`led_task` 大部分时间阻塞 500ms，不是一直就绪。它阻塞时，低优先级的 `heartbeat_task` 可以运行。

### 10.3 `xTaskCreate()` 失败会怎样？

**答**：代码检测 `ok != pdPASS` 后关中断并停住，不会启动调度器。常见原因是 heap 不足。

### 10.4 `vTaskStartScheduler()` 正常会返回吗？

**答**：正常不会。若返回，通常是调度器启动所需内存分配失败，例如空闲任务创建失败。

### 10.5 `vTaskDelay()` 和裸机 delay 有什么区别？

**答**：`vTaskDelay()` 会阻塞当前任务并让出 CPU；裸机 delay 通常一直占着 CPU 忙等。

### 10.6 `configCPU_CLOCK_HZ` 配错会怎样？

**答**：FreeRTOS tick 时间基准会错，任务延时节奏也会不准。

### 10.7 本课有没有 FromISR API？

**答**：没有。本课没有用户中断唤醒任务，所以不要把 FromISR、队列或信号量写进当前源码流程。

### 10.8 栈溢出怎么发现？

**答**：本课打开 `configCHECK_FOR_STACK_OVERFLOW=2` 并实现了 `vApplicationStackOverflowHook()`。检测到栈问题会停在该 Hook。

## 11. 工程实现步骤

### 11.1 需求分析

用两个不同周期的 LED 任务证明 FreeRTOS 调度器已经运行，并理解任务阻塞让出 CPU。

### 11.2 硬件核查

确认 PC13 LED 正常；PA1 若要肉眼观察，需要外接 LED 和限流电阻，或用调试器看 GPIOA ODR。

### 11.3 寄存器路线

手动配置 RCC/GPIO，创建两个任务，检查返回值，启动调度器。

### 11.4 HAL 路线

用 HAL 配置 RCC/GPIO，创建相同任务，检查返回值，启动调度器。

### 11.5 工程思维

RTOS 程序排错要先问任务是否存在、是否就绪、是否阻塞、是否进入 Hook，再看外设输出。

### 11.6 常见工程陷阱

heap 太小、任务栈太小、忘记检查 `xTaskCreate()` 返回值、`configCPU_CLOCK_HZ` 和真实时钟不一致、把 `vTaskDelay()` 当忙等待理解，都会导致现象判断错误。

## 12. 运行现象

正常运行时，PC13 约每 500ms 翻转一次，PA1 约每 1000ms 翻转一次。两个节奏同时存在，说明调度器、SysTick 和任务阻塞/唤醒链路在工作。

如果没有外接 PA1 LED，可以在调试器里观察 GPIOA ODR1 或在 PA1 上接示波器/逻辑分析仪。

## 13. 常见问题排查

### 13.1 PC13 和 PA1 都不闪

先检查是否进入 `vApplicationMallocFailedHook()`，再检查 `vTaskStartScheduler()` 是否返回，最后检查时钟和 GPIO 初始化。

### 13.2 只有 PC13 闪，PA1 不闪

检查 PA1 是否接了 LED，GPIOA 时钟和 PA1 输出配置是否正确。调试器里看 `heartbeat_task` 是否运行。

### 13.3 程序停在 malloc failed hook

检查 `configTOTAL_HEAP_SIZE`、任务数量和栈深度。当前配置是 12KB，两个 128 深度任务正常应够用。

### 13.4 程序停在 stack overflow hook

说明某个任务栈不够或栈被破坏。查看传入的 `task_name`，定位哪个任务出问题。

### 13.5 LED 节奏不准

检查系统时钟是否真为 72MHz，`configCPU_CLOCK_HZ` 是否匹配，`configTICK_RATE_HZ` 是否为 1000。

### 13.6 `vTaskStartScheduler()` 返回

正常情况下不应返回。若返回，优先怀疑空闲任务创建失败，继续查 heap 大小、已创建任务数量和栈深度。

### 13.7 改了优先级后现象不明显

本课两个任务都会主动 `vTaskDelay()`，所以即使优先级不同，低优先级任务也有机会运行。要观察优先级压制，需要构造一个高优先级任务长期不阻塞的实验，但那不是本课默认代码。

### 13.8 PA1 外接 LED 不亮

确认 LED 极性、限流电阻和接地方式。也可以先用调试器看 GPIOA ODR1 是否翻转，区分软件任务没跑和外部接线问题。

## 14. 本课最核心的结论

1. FreeRTOS 让多个任务共享 CPU，不是让多个 while 真正同时占用 CPU。
2. `xTaskCreate()` 创建任务控制块和任务栈，必须检查返回值。
3. `vTaskDelay()` 让任务阻塞并让出 CPU，是两个 LED 节奏共存的关键。
4. `SysTick` 提供 RTOS tick，SVC/PendSV 支撑启动和上下文切换。
5. heap 和 stack 是 FreeRTOS 入门最常见的两类资源问题。
6. reg/hal 的差异在外设初始化，RTOS 调度逻辑相同。

## 15. 建议你现在怎么读这节课

先看两个任务函数，确认它们都“翻转 LED -> vTaskDelay”。再看 `xTaskCreate()` 参数，最后看 `FreeRTOSConfig.h` 里 CPU 时钟、tick、heap、hook 和异常映射。

## 16. 扩展练习

1. 把 `heartbeat_task` 的延时改成 250ms，观察 PA1 节奏。
2. 把两个任务优先级改成相同，观察当前 Demo 现象是否明显变化。
3. 故意把任务栈改小，观察是否进入 stack overflow hook。
4. 用调试器观察 `uxTaskGetStackHighWaterMark()` 的结果。

## 17. 下一课预告

- 上一课：[43_tft_lcd_fsmc](../43_tft_lcd_fsmc/README.md)
- 下一课：[45_freertos_task_create_delete](../45_freertos_task_create_delete/README.md)
