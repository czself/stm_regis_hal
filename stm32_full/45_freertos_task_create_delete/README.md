# 45_freertos_task_create_delete - FreeRTOS 任务创建与删除

## 1. 本课到底在学什么

本课表面现象很简单：程序启动后，PC13 会快速闪 6 次左右，然后停一段时间，再重新出现一段快速闪烁。这个现象不是裸机 `while(1)` 里写延时闪灯，而是一个长期存在的 creator 任务周期性创建 worker 任务，worker 任务运行一小段时间后删除自己。

真正要学的是 FreeRTOS 任务生命周期。`xTaskCreate()` 不是“调用函数让代码马上执行”，而是向内核申请一个任务控制块和任务栈，把任务放进就绪列表；`vTaskStartScheduler()` 启动调度器后，内核才根据优先级和阻塞状态选择哪个任务运行；`vTaskDelete(NULL)` 让当前任务退出调度，后续资源回收还依赖空闲任务。

这节课接在 FreeRTOS 移植入门之后。上一课证明内核能跑起来，本课开始讲“任务作为内核管理对象”到底怎么出生、怎么运行、怎么退出。后面的挂起恢复、时间片、队列、信号量、事件组，都会建立在这节课的任务状态理解上。

## 2. 本课学习目标

学完本课你应该能做到：

- 能说出为什么 PC13 是“一阵闪烁、一阵停顿”，而不是固定周期一直闪。
- 能解释 `creator_task` 为什么只创建 worker，不自己闪灯。
- 能解释 `worker_task` 为什么最后调用 `vTaskDelete(NULL)`，不能直接从任务函数返回。
- 能说明 `TaskHandle_t g_worker` 保存了什么，为什么 creator 任务要靠它判断 worker 是否存在。
- 能拆开 `xTaskCreate(worker_task, "work", 128, NULL, 2, &g_worker)` 每个参数的含义。
- 能说明任务优先级 2 的 worker 为什么比优先级 1 的 creator 更容易先运行。
- 能解释 `vTaskDelay(pdMS_TO_TICKS(200))` 和 `vTaskDelay(pdMS_TO_TICKS(2000))` 分别让哪个任务进入阻塞态。
- 能知道 `configTOTAL_HEAP_SIZE`、任务栈大小、malloc failed hook、stack overflow hook 和创建失败有什么关系。
- 能把寄存器版 GPIO 翻转和 HAL 版 `HAL_GPIO_TogglePin()` 对应起来。
- 能根据“完全不闪、只闪一次、闪几轮后死掉、进 hook”这些现象定位到时钟、GPIO、heap、栈或删除流程。

## 3. 本课目录结构

```text
45_freertos_task_create_delete/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 和 `hal/` 都使用 `lib/FreeRTOS-Kernel`，并通过 `platformio.ini` 增加 `../../freertos` 和 FreeRTOS ARM_CM3 portable 头文件路径。两份代码的 RTOS API 基本相同，差别主要在 STM32 时钟和 GPIO 初始化：寄存器版直接写 RCC/GPIO 寄存器，HAL 版用 HAL 结构体和 API 表达同样意图。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill。
- 调试/下载：ST-Link。
- 系统时钟：外部 HSE 8MHz，经 PLL x9 得到 72MHz。
- 主要观察点：PC13 板载 LED，通常低电平点亮。
- 辅助引脚：PA1、PA2 在本课初始化为输出，但当前任务没有使用它们翻转。
- 输入引脚：PA0 被配置为上拉输入，当前课程没有读取它。
- FreeRTOS 配置：`freertos/FreeRTOSConfig.h`。
- 内核堆：`configTOTAL_HEAP_SIZE = 12 * 1024`。
- tick 频率：`configTICK_RATE_HZ = 1000`，所以 1 tick 约等于 1ms。

本课没有串口输出，也没有按键触发。最可靠的观察方式是 PC13 现象加调试器 Watch：看 `g_worker`、任务创建返回值、hook 是否被进入。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
复位启动
  -> 配 72MHz 系统时钟
  -> 配 PC13/PA1/PA2/PA0 GPIO
  -> 创建 creator_task，优先级 1
  -> 启动 FreeRTOS 调度器
  -> creator 发现 g_worker == NULL
  -> xTaskCreate 创建 worker_task，优先级 2，并把句柄写入 g_worker
  -> worker 抢占或随后运行
  -> worker 翻转 PC13，延时 200ms，重复 6 次
  -> worker 把 g_worker 置 NULL
  -> worker 调用 vTaskDelete(NULL) 删除自己
  -> creator 每 2000ms 再次检查并创建下一轮 worker
```

从现象层看，学生看到的是 PC13 一阵一阵闪。从 RTOS 层看，这是一段任务生命周期反复发生：创建、就绪、运行、阻塞、再次就绪、删除。从硬件层看，PC13 只是任务运行的输出证据，不参与调度决策。

要特别注意 `g_worker = NULL` 和 `vTaskDelete(NULL)` 的顺序。源码里 worker 先把全局句柄清空，再删除自己。这样 creator 下一轮醒来时能知道 worker 已不存在。如果忘记清空句柄，creator 会以为 worker 还存在，后续就不再创建新 worker。

## 6. 先认识本课里出现的核心名词

### 6.1 `Task` 是什么

`Task` 是 FreeRTOS 的任务，中文通常叫任务或线程。它属于 RTOS 内核对象层，不是 STM32 外设，也不是普通 C 函数调用。

任务控制的是一段函数在调度器管理下何时运行、何时让出 CPU、何时等待。源码里的 `creator_task()` 和 `worker_task()` 都是任务函数，它们只有被 `xTaskCreate()` 注册并在调度器启动后，才会按任务规则执行。

本课需要任务，是因为我们要让“创建者”和“工作者”分开：creator 长期存在，worker 短期存在。写错任务函数，例如让它 return，可能导致系统进入不可预期状态；写成裸机函数调用，则无法观察任务生命周期。

### 6.2 `TCB` 是什么

`TCB` 是 Task Control Block，任务控制块。中文可以叫任务控制块，属于 FreeRTOS 内核数据结构层。

TCB 保存任务名、优先级、状态、栈顶位置、链表节点等调度信息。`xTaskCreate()` 成功时，内核会为新任务分配 TCB 和任务栈；创建失败时，多半是 heap 不够或参数不合理。

本课没有直接访问 TCB 字段，但 `TaskHandle_t` 本质上就是用来引用任务控制块的句柄。TCB 出问题，现象可能是 `xTaskCreate()` 返回不是 `pdPASS`，或者进入 malloc failed hook。

### 6.3 `任务栈` 是什么

任务栈是每个任务独立拥有的栈空间，属于 FreeRTOS 内存管理和 Cortex-M 上下文保存层。

任务函数调用、局部变量、寄存器上下文保存都会用到任务栈。本课 `worker_task` 栈深度是 `128`，`creator_task` 栈深度是 `160`，单位在 FreeRTOS ARM 端口里通常是 `StackType_t` 个数，不是简单字节数。

栈太小会进入 `vApplicationStackOverflowHook()`，也可能先表现为奇怪跑飞。本课任务函数很短，所以 128/160 足够；如果你在任务里放大数组或复杂 printf，就必须重新评估栈。

### 6.4 `TaskHandle_t` 是什么

`TaskHandle_t` 是任务句柄类型，属于 FreeRTOS 软件抽象层。它让应用代码能保存并引用一个任务。

源码里 `static TaskHandle_t g_worker;` 保存 worker 任务句柄。creator 用 `g_worker == NULL` 判断是否需要创建新 worker；worker 结束前把它清成 NULL，告诉 creator 自己即将不存在。

如果句柄没有保存，后续就无法指定删除、挂起、恢复或通知某个任务。如果句柄过期却继续使用，可能控制到已经删除的任务对象，造成难查的问题。

### 6.5 `xTaskCreate()` 是什么

`xTaskCreate()` 是 FreeRTOS 创建任务 API，属于 RTOS 内核对象创建层。

本课两处使用：main 创建 `creator_task`，creator 创建 `worker_task`。它的参数依次是任务函数、任务名、栈深度、传参指针、优先级、句柄输出地址。成功返回 `pdPASS`，失败返回错误值。

它控制的不是 GPIO，而是内核是否分配 TCB/栈并把任务放进就绪列表。它出错时，任务根本不会运行，PC13 可能完全没有 worker 那段闪烁。

### 6.6 `pdPASS` 是什么

`pdPASS` 是 FreeRTOS 成功返回宏，属于 FreeRTOS C API 层。

main 里检查 `xTaskCreate(creator_task, ...) != pdPASS`，创建失败就关中断并停住。creator 里创建 worker 时也把返回值保存到 `create_ok` 并检查，失败时关中断停住；这和生产代码应检查创建结果的要求一致。

如果不检查返回值，heap 不足时程序可能表面还在跑，但关键任务没有创建，LED 现象就会缺失。

### 6.7 `vTaskStartScheduler()` 是什么

`vTaskStartScheduler()` 是启动 FreeRTOS 调度器的 API，属于 RTOS 调度核心层。

调用前，main 只是创建了内核对象；调用后，FreeRTOS 配置 SysTick、PendSV、SVC 等机制并开始任务调度。正常情况下它不会返回到后面的 `while(1)`。

如果调度器启动失败，常见原因是 heap 不足导致空闲任务或定时器任务创建失败。现象是程序落到后面的死循环，任务函数不运行。

### 6.8 `vTaskDelay()` 是什么

`vTaskDelay()` 是任务延时 API，属于 FreeRTOS 时间管理层。

worker 每次翻转后 `vTaskDelay(pdMS_TO_TICKS(200))`，让自己阻塞约 200ms；creator 每轮 `vTaskDelay(pdMS_TO_TICKS(2000))`，让自己约 2 秒后再检查。阻塞期间 CPU 可以运行其他就绪任务或空闲任务。

如果把延时删掉，高优先级 worker 可能连续快速执行完，肉眼看不到闪烁；creator 也可能频繁创建任务，增加 heap 压力。

### 6.9 `pdMS_TO_TICKS()` 是什么

`pdMS_TO_TICKS()` 是 FreeRTOS 时间换算宏，属于 RTOS 配置和 C 宏层。

本课 `configTICK_RATE_HZ` 是 1000，因此 `pdMS_TO_TICKS(200)` 大约是 200 tick，`pdMS_TO_TICKS(2000)` 大约是 2000 tick。它让代码用毫秒表达时间，避免手写和 tick 频率绑定的数字。

如果 tick 频率改了而代码不用这个宏，延时会变得不符合预期。现象就是闪烁节奏突然变快或变慢。

### 6.10 `vTaskDelete(NULL)` 是什么

`vTaskDelete()` 是删除任务 API，属于 FreeRTOS 生命周期管理层。参数为 `NULL` 时，表示删除当前正在运行的任务。

本课 worker 闪烁 6 次后调用 `vTaskDelete(NULL)`。这一步让 worker 从调度中退出，后续不再占用 CPU；动态分配的资源需要空闲任务清理。

如果 worker 不删除自己，而是无限循环，那么 creator 后续看到 `g_worker` 不为 NULL，就不会再创建新 worker；如果 worker 直接 return，则不是 FreeRTOS 推荐写法。

### 6.11 `空闲任务` 是什么

空闲任务是调度器启动时自动创建的最低优先级任务，属于 FreeRTOS 内核基础任务。

它在没有其他就绪任务时运行。动态删除任务后，某些资源回收依赖空闲任务执行，所以工程里不能让高优先级任务永远不阻塞，导致空闲任务没有机会运行。

本课 creator 和 worker 都会 `vTaskDelay()`，因此空闲任务有机会运行。若你把所有延时删掉，系统可能长期忙在高优先级任务里，删除回收就会变得不健康。

### 6.12 `heap_4` 和 `configTOTAL_HEAP_SIZE` 是什么

本工程使用 FreeRTOS 内核的堆管理，`configTOTAL_HEAP_SIZE` 在配置里是 `12 * 1024`。它属于 RTOS 内存管理层。

任务 TCB、任务栈、队列、信号量等动态对象都可能从这个 heap 分配。本课反复创建 worker，如果删除和回收正常，heap 不应持续下降；如果任务不删除或空闲任务被饿死，就可能逐步耗尽。

heap 不足会触发 `vApplicationMallocFailedHook()`，现象是程序关中断后停在 hook 的死循环里。

### 6.13 `vApplicationMallocFailedHook()` 是什么

这是 FreeRTOS malloc 失败钩子函数，属于 RTOS 错误处理层。

源码里实现为 `taskDISABLE_INTERRUPTS(); while(1){}`。一旦动态分配失败，系统停在明确位置，方便调试器定位，而不是继续运行到更混乱的错误。

如果你创建太多任务、栈给太大或 heap 配太小，就可能进入这里。排查时看调用栈和最近一次对象创建。

### 6.14 `vApplicationStackOverflowHook()` 是什么

这是 FreeRTOS 栈溢出钩子，属于 RTOS 运行时保护层。

本课配置 `configCHECK_FOR_STACK_OVERFLOW = 2`，源码实现 hook 后同样关中断死循环。参数 `task` 和 `task_name` 能帮助定位哪个任务栈出问题，但当前代码把它们 `(void)` 掉，没有进一步输出。

如果你在 worker 或 creator 里增加大局部变量、深层函数调用，可能进入这个 hook。现象是 LED 停住，调试器停在栈溢出处理。

### 6.15 `PC13` 在本课做什么

PC13 属于 STM32 GPIO 硬件层，是本课唯一主要肉眼反馈。

寄存器版通过 `GPIOC->ODR` 判断当前输出，再写 `BRR` 或 `BSRR` 翻转；HAL 版通过 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)` 翻转。PC13 不决定任务是否创建，它只是 worker 任务运行的可见证据。

如果 PC13 不闪，要先分层判断：任务没创建、任务创建了但没运行、任务运行了但 GPIO 没配对、LED 硬件接法和预期不同。

### 6.16 `SVC / PendSV / SysTick` 是什么

它们属于 Cortex-M 内核异常层，是 FreeRTOS 在 ARM Cortex-M3 上完成调度的关键异常。

`FreeRTOSConfig.h` 把 `vPortSVCHandler` 映射到 `SVC_Handler`，把 `xPortPendSVHandler` 映射到 `PendSV_Handler`，把 `xPortSysTickHandler` 映射到 `SysTick_Handler`。SVC 常用于启动第一个任务，SysTick 提供节拍，PendSV 负责上下文切换。

如果这些映射错了，调度器可能启动不了，或者 tick 不走，`vTaskDelay()` 不会按预期唤醒任务。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件为什么同时包含 CMSIS 和 FreeRTOS

寄存器版包含 `stm32f1xx.h`、`FreeRTOS.h`、`task.h`。

`stm32f1xx.h` 提供 RCC、GPIO、FLASH 等寄存器结构体和位宏；`FreeRTOS.h` 提供内核基础定义；`task.h` 提供 `xTaskCreate()`、`vTaskDelay()`、`vTaskDelete()` 等任务 API。少了 CMSIS 头文件，寄存器名不能用；少了 FreeRTOS 头文件，任务 API 和类型不能用。

### 7.2 `system_clock_72mhz_init()` 的硬件后果

函数先写 `FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2`，让 Flash 在 72MHz 下有合适等待周期和预取。

然后打开 HSE，等待 `RCC_CR_HSERDY`；配置 PLL 源为 HSE、倍频为 x9，APB1 二分频，APB2 不分频；等待 `RCC_CR_PLLRDY` 后切换 SYSCLK 到 PLL，并等待 `RCC_CFGR_SWS` 确认切换完成。

这一步对 FreeRTOS 很重要，因为 `configCPU_CLOCK_HZ` 写的是 72MHz。实际系统时钟和配置不一致时，SysTick 节拍会不准，`vTaskDelay(200ms)` 的真实时间就会偏。

### 7.3 `gpio_init()` 开 GPIO 时钟

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_AFIOEN;
```

GPIOC 和 GPIOA 都挂在 APB2 总线上，PC13、PA1、PA2、PA0 都需要对应 GPIO 时钟。AFIO 在本课当前代码中没有实际配置 EXTI，但被打开了；这保持了后续 RTOS 中断课程的初始化风格。

不开 GPIO 时钟，后面对 CRH/CRL 的配置不能可靠作用到硬件，PC13 也不会按任务运行翻转。

### 7.4 PC13 为什么配置 CRH

PC13 是 GPIOC 的 13 号引脚，F1 每个 GPIO 端口低 8 个引脚在 CRL，高 8 个引脚在 CRH，所以 PC13 要清 `GPIO_CRH_MODE13 | GPIO_CRH_CNF13`。

随后设置 `GPIO_CRH_MODE13_1`，表示输出速度 2MHz；CNF 保持 00，表示通用推挽输出。最后 `GPIOC->BSRR = GPIO_BSRR_BS13` 输出高电平，让 BluePill 上常见的低电平点亮 LED 初始熄灭。

### 7.5 PA0/PA1/PA2 的配置边界

源码清除 PA0、PA1、PA2 在 `GPIOA->CRL` 里的 MODE/CNF 字段，再设置 `GPIO_CRL_CNF0_1 | GPIO_CRL_MODE1_1 | GPIO_CRL_MODE2_1`。

PA0 被配置成输入上拉/下拉模式，并通过 `GPIOA->BSRR = GPIO_BSRR_BS0` 让 ODR0 为 1，形成上拉输入。PA1、PA2 被配置成 2MHz 推挽输出。本课 worker 没有翻转 PA1/PA2，别把它们当作运行现象主证据。

### 7.6 `led_toggle_pc13()` 的寄存器动作

`led_toggle_pc13()` 先读 `GPIOC->ODR & GPIO_ODR_ODR13` 判断当前输出锁存状态。

如果当前为 1，就写 `GPIOC->BRR = GPIO_BRR_BR13` 拉低；如果当前为 0，就写 `GPIOC->BSRR = GPIO_BSRR_BS13` 拉高。BSRR/BRR 是原子置位/复位寄存器，比读改写 ODR 更适合在可能有并发访问时使用。

### 7.7 hook 函数为什么关中断后死循环

`vApplicationMallocFailedHook()` 和 `vApplicationStackOverflowHook()` 都调用 `taskDISABLE_INTERRUPTS()`，再进入 `while(1)`。

这是教学工程里很好的失败策略：一旦 heap 分配失败或栈溢出，不继续执行不可信状态，而是停住让你用调试器看位置。如果 LED 突然停住，调试器第一步就应该看是否落在这两个 hook。

### 7.8 `g_worker` 的初始状态

`static TaskHandle_t g_worker;` 是静态全局变量，C 运行时会把它初始化为 0，也就是 NULL。

creator 第一次运行时看到 `g_worker == NULL`，于是创建 worker。worker 结束前把它再次置 NULL，给下一轮创建打开门。如果它是局部变量，creator 和 worker 就无法共享这个状态。

### 7.9 `worker_task()` 的运行过程

worker 任务函数先忽略参数，然后执行 6 次循环。

每次循环翻转 PC13，再调用 `vTaskDelay(pdMS_TO_TICKS(200))`。延时调用后，worker 从运行态进入阻塞态，等待 tick 计数到期。6 次之后，worker 把 `g_worker` 清空，再调用 `vTaskDelete(NULL)` 删除当前任务。

### 7.10 `creator_task()` 为什么长期存在

creator 任务是一个无限循环。

它每轮先判断 `g_worker`，如果为 NULL，就调用 `xTaskCreate()` 创建 worker，并要求内核把句柄写回 `&g_worker`。随后 creator 延时 2000ms。这个延时避免 creator 高频检查，也给 worker 和空闲任务运行机会。

### 7.11 worker 优先级为什么是 2

creator 创建 worker 时给优先级 2，main 创建 creator 时给优先级 1。

FreeRTOS 数字越大优先级越高，所以 worker 就绪时会优先于 creator 运行。worker 只有在 `vTaskDelay()` 阻塞时，creator 或空闲任务才有机会运行。这个安排让 LED 闪烁动作更及时，也让学生能看到高优先级短任务的生命周期。

### 7.12 main 创建 creator 的返回值检查

main 中：

```c
if(xTaskCreate(creator_task,"make",160,NULL,1,NULL)!=pdPASS){
    taskDISABLE_INTERRUPTS();
    while(1){}
}
```

这是创建系统第一个业务任务。如果它失败，后面启动调度器也没有意义，所以直接停住。失败原因通常是 heap 不足，或者 FreeRTOS 配置/链接有问题。

### 7.13 `vTaskStartScheduler()` 之后为什么还有 `while(1)`

正常情况下，调度器启动后不会返回，CPU 会在任务、空闲任务和异常处理之间切换。

后面的 `while(1){}` 是防御性代码：如果调度器因为资源不足等原因启动失败，程序至少不会跑出 main。若你调试发现执行到了这里，优先查 heap、FreeRTOSConfig 映射和启动调度器前的任务创建。

### 7.14 当前课程没有 FromISR API

本课没有中断向任务投递事件，也没有调用 `xQueueSendFromISR()` 这类 API。

所以第 7 章只讨论任务上下文 API。FromISR API 会在后面的中断与临界区课程出现。这里提前分清边界很重要：普通任务里用 `xTaskCreate()`、`vTaskDelay()`；中断里不能随便调用会阻塞的任务 API。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()` 在本课负责什么

HAL 版 main 先调用 `HAL_Init()`。

它初始化 HAL 基础状态，并设置 HAL tick 相关机制。虽然调度器启动后 SysTick 会由 FreeRTOS 端口接管，但在启动前使用 HAL RCC/GPIO API 时，HAL 基础初始化仍是常规入口。

### 8.2 HAL 时钟结构体对应寄存器版哪几步

`RCC_OscInitTypeDef osc` 描述 HSE、HSE 预分频、PLL 开关、PLL 源和 PLL 倍频；`RCC_ClkInitTypeDef clk` 描述 SYSCLK、HCLK、PCLK1、PCLK2。

这些字段对应寄存器版对 `RCC->CR`、`RCC->CFGR` 和 `FLASH->ACR` 的配置。`HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2)` 里的 `FLASH_LATENCY_2` 对应 72MHz 下 Flash 等待周期。

### 8.3 HAL GPIO 初始化对应哪些硬件字段

HAL 版先 `__HAL_RCC_GPIOC_CLK_ENABLE()` 和 `__HAL_RCC_GPIOA_CLK_ENABLE()`，对应寄存器版 APB2ENR 的 IOPCEN/IOPAEN。

`gpio.Pin = GPIO_PIN_13`、`gpio.Mode = GPIO_MODE_OUTPUT_PP`、`gpio.Pull = GPIO_NOPULL`、`gpio.Speed = GPIO_SPEED_FREQ_LOW`，表达 PC13 推挽输出。底层会配置 CRH 的 MODE13/CNF13。`HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET)` 对应初始输出高电平。

### 8.4 PA1/PA2/PA0 的 HAL 字段

`gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2` 配 PA1、PA2 输出；`gpio.Pin = GPIO_PIN_0`、`gpio.Mode = GPIO_MODE_INPUT`、`gpio.Pull = GPIO_PULLUP` 配 PA0 上拉输入。

HAL 里 `GPIO_PULLUP` 对 F1 来说不只是一个抽象词，底层会让输入上拉/下拉模式配合 ODR 位形成上拉。当前课程不使用 PA0，但文档必须说明它在源码中确实被配置了。

### 8.5 HAL 版 worker 与寄存器版 worker 的差别

HAL 版 worker 的 RTOS 流程完全一样：循环 6 次、每次翻转 PC13、延时 200ms、清空 `g_worker`、删除自己。

差别只有翻转函数从 `led_toggle_pc13()` 变成 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`。这说明 HAL 没有改变任务生命周期，只封装了 GPIO 输出动作。

### 8.6 HAL 版 creator 与寄存器版 creator 的差别

HAL 版 creator 同样检查 `g_worker == NULL`，同样调用 `xTaskCreate(worker_task,"work",128,NULL,2,&g_worker)`，同样延时 2000ms。

这部分没有 HAL API，因为它属于 FreeRTOS 内核，不属于 STM32 外设库。读 HAL 版时不能把所有函数都理解成 HAL；`xTaskCreate()`、`vTaskDelay()`、`vTaskDelete()` 仍然是 FreeRTOS。

### 8.7 HAL tick 和 FreeRTOS tick 的关系

本课 HAL 任务里没有使用 `HAL_Delay()`，而是使用 `vTaskDelay()`。

这很重要。调度器启动后，任务里应优先使用 FreeRTOS 延时，让任务进入阻塞态，把 CPU 交还调度器。若在任务里大量使用裸机忙等或不合适的 HAL 延时，会影响其他任务运行。

### 8.8 HAL 版也必须实现 hook

HAL 版同样实现 malloc failed hook 和 stack overflow hook。

因为任务创建、栈分配、删除回收都属于 FreeRTOS，与寄存器/HAL 初始化方式无关。HAL 不能替你解决 heap 不足，也不能替你自动扩大任务栈。

### 8.9 HAL 版创建失败时停在哪里

main 创建 creator 失败时，HAL 版也关中断并进入死循环。

如果你在 HAL 版看到 LED 完全不闪，不要只查 `HAL_GPIO_Init()`。应先确认 `xTaskCreate()` 是否返回 `pdPASS`，再确认 `vTaskStartScheduler()` 是否启动成功。

### 8.10 HAL 版没有 `HAL_FreeRTOS_CreateTask`

源码里没有所谓 HAL 的任务创建 API。

这是一个重要边界：HAL 管 STM32 外设初始化和访问，FreeRTOS 管任务和调度。工程里常常同时使用两者，但它们不是上下级替代关系。任务创建仍然直接调用 FreeRTOS 原生 API。

## 9. 两个版本真正应该怎么学

寄存器版让你看清硬件反馈是怎么来的：RCC 开 GPIO 时钟，CRH/CRL 配引脚模式，BSRR/BRR 改输出电平。HAL 版让你看清工程表达方式：用结构体字段描述同样的引脚意图。

但本课的主线不是 GPIO，而是 FreeRTOS 任务生命周期。两个版本都创建 creator，creator 再创建 worker，worker 闪 6 次后删除自己。只要 RTOS 流程理解了，reg/hal 的差异只是“如何让 PC13 翻转”。

学习时建议把代码分成三层看：硬件初始化层、RTOS 对象创建层、任务函数行为层。排错也按这个顺序分层：先看时钟/GPIO，再看任务是否创建成功，再看任务是否按预期阻塞和删除。

## 10. 检验问题清单

### 10.1 为什么 PC13 不是一直匀速闪？

**答**：因为 PC13 只在 worker 任务存在时快速闪烁。worker 闪 6 次后删除自己，creator 约 2 秒后才创建下一轮 worker，所以现象是一段闪烁加一段停顿。

### 10.2 `g_worker` 为什么要设为 NULL？

**答**：creator 用 `g_worker == NULL` 判断 worker 是否不存在。worker 删除自己前把它清空，下一轮 creator 才会重新创建 worker。

### 10.3 `vTaskDelete(NULL)` 里的 NULL 表示什么？

**答**：表示删除当前正在运行的任务，也就是 worker 自己。它不是删除空指针，而是 FreeRTOS API 对“当前任务”的约定写法。

### 10.4 worker 的栈深度 128 太小会怎样？

**答**：可能进入 `vApplicationStackOverflowHook()`，也可能出现跑飞、LED 停住等现象。本课 worker 很简单，128 通常够；复杂任务要重新测栈水位。

### 10.5 `xTaskCreate()` 失败最先查什么？

**答**：先查返回值和 malloc failed hook，再查 `configTOTAL_HEAP_SIZE`、任务栈深度、是否创建了过多动态对象。

### 10.6 为什么任务函数不能直接 return？

**答**：FreeRTOS 任务应长期循环或显式调用 `vTaskDelete()` 结束。直接 return 会离开内核期望的任务执行模型，容易造成不可预期问题。

### 10.7 `vTaskDelay()` 和裸机延时有什么本质区别？

**答**：`vTaskDelay()` 让当前任务进入阻塞态，CPU 可调度其他任务；裸机忙等通常一直占 CPU，不利于多任务系统。

### 10.8 为什么空闲任务和删除有关？

**答**：动态删除任务后的资源清理依赖空闲任务运行。如果高优先级任务一直不阻塞，空闲任务可能没机会回收资源。

### 10.9 reg/hal 版的 FreeRTOS 逻辑是否不同？

**答**：不同很少。任务创建、延时、删除逻辑一样，主要差异在 PC13 和时钟 GPIO 初始化方式。

### 10.10 本课有没有使用中断唤醒任务？

**答**：没有。本课没有 FromISR API，也没有队列/信号量唤醒。它只演示任务创建、延时阻塞和任务删除。

## 11. 工程实现步骤

### 11.1 需求分析

目标是在 FreeRTOS 中演示一个短生命周期任务：被创建、运行几次、自己删除，再由另一个任务周期性重建。

### 11.2 硬件核查

确认 PC13 LED 可用，HSE 8MHz 晶振正常，ST-Link 能下载。PA1、PA2、PA0 虽被初始化，但不是本课主要现象。

### 11.3 寄存器路线

先配置 Flash/HSE/PLL 到 72MHz，再开 GPIOA/GPIOC 时钟，配置 PC13 输出。然后创建 creator 任务，启动调度器。worker 中用 BSRR/BRR 翻转 PC13。

### 11.4 HAL 路线

先 `HAL_Init()`，再用 RCC 结构体配置 72MHz，用 `GPIO_InitTypeDef` 配 PC13/PA1/PA2/PA0。RTOS 部分仍直接调用 FreeRTOS API，不经过 HAL 包装。

### 11.5 工程思维

动态创建任务要有退出策略、错误检查和资源回收意识。教学里可以反复创建删除，工程里要谨慎评估 heap 碎片、空闲任务运行机会和任务栈余量。

### 11.6 常见工程陷阱

忘记检查 `xTaskCreate()` 返回值、任务函数直接 return、删除前不更新句柄、任务栈给太小、把 `vTaskDelay()` 换成忙等、让高优先级任务不阻塞导致空闲任务没机会运行，都是本课要避免的坑。

## 12. 运行现象

正常运行时，PC13 会出现一段快速闪烁。每次 worker 创建后，它每 200ms 翻转一次，循环 6 次，因此大约持续一小段时间；worker 删除后，creator 延时 2000ms，再创建下一轮 worker。

调试器里可以观察 `g_worker`。worker 不存在时它应为 NULL；创建成功后变成非 NULL；worker 结束前又被清回 NULL。这个变量比肉眼看 LED 更能证明生命周期是否按预期发生。

如果你只看到 PC13 初始状态不变，先确认是否进入 hook 或 main 的创建失败死循环。如果你看到只闪第一轮，重点检查 `g_worker` 是否被清空、worker 是否真的执行到 `vTaskDelete(NULL)`。

## 13. 常见问题排查

### 13.1 PC13 完全不闪

先看程序是否停在 `vApplicationMallocFailedHook()`、`vApplicationStackOverflowHook()` 或 main 的创建失败分支。若没有，再查 GPIOC 时钟、PC13 CRH 配置或 HAL GPIO 初始化。

### 13.2 只闪一轮后不再闪

重点看 `g_worker` 是否被清成 NULL。如果 worker 删除前没有清空句柄，creator 会认为 worker 仍存在，从而不再创建下一轮。

### 13.3 闪烁太快看不清

检查 `configTICK_RATE_HZ` 是否为 1000，系统时钟是否真是 72MHz，`pdMS_TO_TICKS(200)` 是否被改动。也可以在 worker 循环里打断点确认次数。

### 13.4 进入 malloc failed hook

说明 FreeRTOS heap 分配失败。检查 `configTOTAL_HEAP_SIZE`、任务栈大小、是否反复创建任务但没有删除，或者空闲任务是否被饿死导致资源回收不及时。

### 13.5 进入 stack overflow hook

说明某个任务栈不够或栈被破坏。先看 `task_name` 参数定位任务，再减少局部变量、增大栈深度，或使用栈水位 API 评估余量。

### 13.6 调度器启动后像卡死

确认 SVC/PendSV/SysTick 映射正确，`configCPU_CLOCK_HZ` 和实际时钟一致，且启动前至少有一个任务创建成功。调度器启动失败通常会落回 main 后面的死循环。

### 13.7 HAL 版 GPIO 正常但任务不运行

把 HAL 层和 RTOS 层分开查。GPIO 初始化成功只说明引脚可输出，不说明任务创建成功。继续查 `xTaskCreate()` 返回值、heap、hook 和调度器。

### 13.8 删除后资源没有及时回收

检查是否有高优先级任务长期不阻塞，让空闲任务没有运行机会。本课 creator 和 worker 都会延时，正常情况下空闲任务能运行。

## 14. 本课最核心的结论

1. `xTaskCreate()` 创建的是 FreeRTOS 任务对象，不是普通函数立即调用。
2. 任务创建成功需要 heap 同时容纳 TCB 和任务栈。
3. `TaskHandle_t` 是后续识别和控制任务的重要句柄。
4. `vTaskDelay()` 会让任务阻塞，把 CPU 交还调度器。
5. 短生命周期任务结束时应显式调用 `vTaskDelete(NULL)`。
6. 动态删除任务后的资源回收依赖空闲任务有机会运行。
7. reg/hal 的差异主要在 GPIO 和时钟初始化，RTOS 生命周期逻辑相同。
8. PC13 是任务运行证据，不是调度器本身；排错要同时看 hook、返回值和句柄。

## 15. 建议你现在怎么读这节课

第一遍只看第 5 章链路，把“一阵闪、一阵停”对应到 creator 和 worker 的状态变化。第二遍看 `worker_task()`，确认 6 次翻转、6 次延时、清句柄、删除自己这四步。第三遍看 `creator_task()`，确认它为什么每 2000ms 只在 `g_worker == NULL` 时创建。最后对比 reg/hal 的 PC13 翻转方式。

不要急着改优先级。先在调试器里看 `g_worker` 从 NULL 到非 NULL 再回 NULL，这个证据比肉眼看灯更稳定。

## 16. 扩展练习

1. 把 worker 循环次数从 6 改成 3，观察闪烁段变短。
2. 把 creator 延时从 2000ms 改成 1000ms，观察两段闪烁间隔变短。
3. 故意注释掉 `g_worker = NULL`，验证是否只闪第一轮。
4. 故意增大 worker 栈或创建更多 worker，观察 heap 失败风险。
5. 在 worker 删除前后打断点，观察 `g_worker` 和 PC13 状态。

## 17. 下一课预告

- 上一课：[44_freertos_intro_porting](../44_freertos_intro_porting/README.md)
- 下一课：[46_freertos_task_suspend_resume](../46_freertos_task_suspend_resume/README.md)
