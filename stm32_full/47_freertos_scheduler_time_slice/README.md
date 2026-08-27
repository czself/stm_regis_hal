# 47_freertos_scheduler_time_slice - FreeRTOS 调度器与时间片

## 1. 本课到底在学什么

本课表面现象是：PA1 和 PA2 会被两个同优先级任务轮流快速翻转，PC13 则由更高优先级任务每 500ms 翻转一次。肉眼可能更容易看到 PC13 的慢节奏，而 PA1/PA2 更适合接逻辑分析仪或在调试器里观察 ODR 变化。

真正要学的是 FreeRTOS 调度器如何在“高优先级任务”和“同优先级任务”之间选择运行者。`same_a` 和 `same_b` 都是优先级 1，并且每轮主动调用 `taskYIELD()`；`high_task` 是优先级 2，但它每次翻转 PC13 后会 `vTaskDelay(500ms)`，阻塞期间 CPU 才能给同优先级的 PA1/PA2 任务。

这节课接在任务挂起恢复之后。上一课讲一个任务可以被另一个任务显式移出调度；本课讲任务都处于可调度状态时，调度器如何按优先级、时间片和主动让出来分配 CPU。后面的队列、信号量、互斥量都离不开这个规则。

## 2. 本课学习目标

学完本课你应该能做到：

- 能解释为什么优先级 2 的 `high_task` 醒来后会优先翻转 PC13。
- 能解释为什么 `high_task` 阻塞后，优先级 1 的 `same_a` 和 `same_b` 才有机会运行。
- 能说明 `taskYIELD()` 的作用是主动让出当前 CPU 使用机会。
- 能说明 `configUSE_TIME_SLICING = 1` 对同优先级就绪任务的意义。
- 能区分抢占式调度、时间片轮转、主动让出和阻塞等待。
- 能把 PA1、PA2、PC13 三个现象分别对应到三个任务。
- 能解释为什么没有 `vTaskDelay()` 的同优先级任务会占用大量 CPU。
- 能知道 reg/hal 两版 RTOS 逻辑相同，差别只是 GPIO 翻转方式。
- 能根据 PA1/PA2 不动、PC13 不动、系统进 hook 等现象定位问题。
- 能读懂三个 `BaseType_t` 变量分别保存任务创建结果的作用。

## 3. 本课目录结构

```text
47_freertos_scheduler_time_slice/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

两套工程都使用同一个 FreeRTOS 内核和 `freertos/FreeRTOSConfig.h`。`platformio.ini` 指向 `../../freertos` 和 FreeRTOS ARM_CM3 portable 目录。课程重点不在外设驱动复杂度，而在同优先级任务如何被调度。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill。
- 下载器：ST-Link。
- 系统时钟：HSE 8MHz，PLL x9，SYSCLK 72MHz。
- PC13：板载 LED，由 `high_task` 翻转。
- PA1：输出引脚，由 `same_a` 翻转。
- PA2：输出引脚，由 `same_b` 翻转。
- PA0：配置为上拉输入，当前任务逻辑不读取。
- FreeRTOS 配置：`configUSE_PREEMPTION = 1`，`configUSE_TIME_SLICING = 1`，`configMAX_PRIORITIES = 5`。

PA1/PA2 翻转很快，肉眼看外接 LED 可能只是微亮或看不清。用示波器、逻辑分析仪或调试器看 `GPIOA->ODR` 更可靠。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
复位启动
  -> 配 72MHz 时钟
  -> 配 PC13、PA1、PA2 输出
  -> 创建 same_a，优先级 1
  -> 创建 same_b，优先级 1
  -> 创建 high_task，优先级 2
  -> 启动调度器
  -> high_task 优先运行，翻转 PC13
  -> high_task vTaskDelay(500ms)，进入阻塞态
  -> same_a 和 same_b 成为最高优先级就绪任务
  -> same_a 翻转 PA1 后 taskYIELD()
  -> same_b 翻转 PA2 后 taskYIELD()
  -> 两个同优先级任务轮流获得 CPU
  -> 500ms 到期后 high_task 回到就绪态并抢占
```

这条链路里有三类调度行为。第一，高优先级任务就绪时优先运行；第二，高优先级任务阻塞后，低优先级任务才运行；第三，同优先级任务之间通过时间片和 `taskYIELD()` 分享 CPU。

不要把时间片理解成硬件定时器直接翻转 PA1/PA2。PA1/PA2 的每次变化仍然是任务函数里的 C 语句执行结果；时间片只决定哪个任务此刻能执行这些语句。

## 6. 先认识本课里出现的核心名词

### 6.1 `调度器` 是什么

调度器是 FreeRTOS 内核中决定哪个任务运行的部分，属于 RTOS 核心层。

它根据任务优先级、任务状态、tick 中断和主动让出请求选择运行任务。本课 `vTaskStartScheduler()` 后，main 不再直接控制执行顺序，调度器开始在 `same_a`、`same_b`、`high_task` 和空闲任务之间切换。

调度器出问题或没有启动时，三个任务都不会按预期翻转 GPIO。

### 6.2 `抢占式调度` 是什么

抢占式调度由 `configUSE_PREEMPTION = 1` 启用，属于 FreeRTOS 调度策略层。

当更高优先级任务从阻塞态回到就绪态时，它可以抢占当前低优先级任务。本课 `high_task` 的 500ms 延时到期后，会优先于 PA1/PA2 两个任务运行。

如果没有抢占，高优先级任务可能要等当前任务主动让出才运行，实时性会变差。

### 6.3 `时间片` 是什么

时间片是同优先级就绪任务轮流运行的时间分配机制，属于 RTOS 同级调度层。

本配置 `configUSE_TIME_SLICING = 1`，意味着同优先级任务不会永远只跑一个。`same_a` 和 `same_b` 都是优先级 1，所以在 high_task 阻塞期间，它们可以轮流执行。

时间片只在同优先级任务之间有意义。优先级 2 的 `high_task` 就绪时，不会和优先级 1 的任务平分 CPU。

### 6.4 `taskYIELD()` 是什么

`taskYIELD()` 是 FreeRTOS 主动让出 CPU 的 API/宏，属于任务调度控制层。

`same_a` 每次翻转 PA1 后调用它，`same_b` 每次翻转 PA2 后也调用它。这样两个同优先级任务不会等到 tick 才切换，而是主动给同优先级的另一个任务运行机会。

如果删掉 `taskYIELD()`，在时间片仍开启时同优先级任务仍可能轮转，但切换节奏更依赖 tick；若时间片关闭，一个任务可能长期占据 CPU。

### 6.5 `同优先级任务` 是什么

同优先级任务是 FreeRTOS 优先级数值相同的任务，属于 RTOS 调度队列层。

本课 `same_a` 和 `same_b` 都用优先级 1 创建。它们没有延时阻塞，几乎一直处于就绪态，因此调度器必须在它们之间分配 CPU。

如果其中一个优先级改成 2，它就会和 `high_task` 同级，现象和教学目标都会改变。

### 6.6 `高优先级任务` 是什么

高优先级任务是优先级数字更大的任务。FreeRTOS 默认数字越大优先级越高。

本课 `high_task` 优先级 2，高于 `same_a`/`same_b` 的 1。它一旦就绪，调度器优先选择它；它调用 `vTaskDelay(500ms)` 阻塞后，低优先级任务才运行。

如果 `high_task` 不阻塞，PA1/PA2 任务可能几乎没有运行机会。

### 6.7 `vTaskDelay(500ms)` 在本课做什么

`vTaskDelay(pdMS_TO_TICKS(500))` 让 `high_task` 进入阻塞态约 500ms，属于 FreeRTOS 时间管理层。

这一步是 PA1/PA2 任务能运行的前提。高优先级任务如果一直就绪，低优先级任务不会被调度。

闪烁节奏不对时，要同时查 tick 配置、系统时钟和这个延时值。

### 6.8 `PA1` 在本课做什么

PA1 是 STM32 GPIOA 的 1 号输出引脚，属于硬件输出层。

寄存器版 `same_a` 调用 `led_toggle_pa1()`，HAL 版 `same_a` 调用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)`。PA1 的变化代表 `same_a` 得到 CPU 并执行了循环体。

PA1 不变可能是 GPIO 未配置，也可能是 `same_a` 没创建成功或被更高优先级任务饿死。

### 6.9 `PA2` 在本课做什么

PA2 是 GPIOA 的 2 号输出引脚，属于硬件输出层。

它由 `same_b` 翻转，用来和 PA1 配对观察同优先级任务轮转。PA1/PA2 都变化，说明两个同级任务都有运行机会。

如果 PA1 变化但 PA2 不变化，优先查 `same_b` 是否创建成功、PA2 CRL 字段或 HAL Pin 配置是否正确。

### 6.10 `PC13` 在本课做什么

PC13 由高优先级 `high_task` 翻转，是观察高优先级任务周期醒来的证据。

寄存器版读写 ODR/BRR/BSRR；HAL 版用 `HAL_GPIO_TogglePin()`。PC13 每 500ms 左右变化一次，说明 `high_task` 的延时和唤醒在工作。

### 6.11 `configUSE_TIME_SLICING` 是什么

`configUSE_TIME_SLICING` 是 FreeRTOS 配置宏，属于内核调度配置层。

本仓库配置为 1。它影响同优先级就绪任务是否按 tick 时间片轮换。本课即使有 `taskYIELD()`，仍要知道这个配置存在，因为它决定“同级任务默认是否公平轮转”的基础行为。

### 6.12 三个创建结果变量是什么

`a_ok`、`b_ok`、`high_ok` 分别保存三个 `xTaskCreate()` 返回值，属于 FreeRTOS API 返回值处理层。

三个任务都创建成功，系统才启动调度器；任一失败，就关中断停住。PA1/PA2/PC13 任一现象缺失，都要考虑对应任务是否创建失败。

### 6.13 `空闲任务` 和本课有什么关系

空闲任务是调度器自动创建的最低优先级任务。

本课 `same_a` 和 `same_b` 一直就绪，所以空闲任务几乎没有机会运行。这个现象用来提醒你：不阻塞的任务会吃掉 CPU，工程里要谨慎设计。

### 6.14 `SVC / PendSV / SysTick` 和时间片有什么关系

SysTick 提供周期节拍，PendSV 执行上下文切换，SVC 用于启动第一个任务。

时间片轮转、`vTaskDelay()` 到期、抢占切换都依赖这些 Cortex-M 异常映射正确。映射错时，任务可能启动不了、延时不恢复或同级轮转异常。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件和模块

寄存器版包含 CMSIS 和 FreeRTOS 任务头文件。

CMSIS 提供 RCC/GPIO/FLASH 寄存器，FreeRTOS 提供 `xTaskCreate()`、`vTaskDelay()`、`taskYIELD()`。本课没有队列、信号量和中断投递。

### 7.2 72MHz 时钟为什么仍要配置

系统时钟配置和前面课程一致：Flash 两等待周期，HSE ready，PLL x9，APB1 二分频，切换到 PLL。

FreeRTOS tick 需要准确的 CPU 时钟。`high_task` 的 500ms 延时和时间片节奏都建立在这个时钟基础上。

### 7.3 GPIO 初始化

GPIOC 和 GPIOA 时钟打开后，PC13 配为推挽输出，PA1/PA2 配为推挽输出，PA0 配为上拉输入。

PA1、PA2 在 CRL，PC13 在 CRH。先清字段再设字段，避免旧配置影响输出。

### 7.4 `led_toggle_pa1()` 和 `led_toggle_pa2()`

两个函数都读取 `GPIOA->ODR`，再通过 `BRR` 或 `BSRR` 翻转对应位。

它们是同优先级任务运行的硬件证据。PA1 对应 `same_a`，PA2 对应 `same_b`，不要把两个现象混在一起。

### 7.5 `same_a()` 的执行后果

`same_a` 无限循环：翻转 PA1，然后 `taskYIELD()`。

它没有 `vTaskDelay()`，所以不会主动进入阻塞态。`taskYIELD()` 只是让出当前运行机会，如果没有同优先级任务等待，它仍可能很快再次运行。

### 7.6 `same_b()` 的执行后果

`same_b` 与 `same_a` 对称：翻转 PA2，然后 `taskYIELD()`。

两个函数结构故意相同，便于把 PA1/PA2 的变化归因到调度器的同级轮转，而不是业务逻辑差异。

### 7.7 `high_task()` 的执行后果

`high_task` 翻转 PC13 后调用 `vTaskDelay(500ms)`。

它优先级 2，因此醒来时会抢占 PA1/PA2 任务；它阻塞时，PA1/PA2 才成为最高优先级就绪任务。

### 7.8 main 创建三个任务

main 依次创建 `same_a`、`same_b`、`high_task`。

前两个优先级 1，第三个优先级 2。所有栈深度都是 128。创建结果分别存入 `a_ok`、`b_ok`、`high_ok`，任一失败就停住。

### 7.9 为什么三个任务都不保存句柄

本课没有挂起、恢复、删除或通知指定任务，所以创建时句柄参数都是 NULL。

这和上一课不同：上一课 control 必须控制 blink，所以要保存 `g_blink`。本课只观察调度结果，不需要外部引用某个任务。

### 7.10 `taskYIELD()` 不是延时

`taskYIELD()` 不让任务等待固定时间，它只请求调度器切换到同优先级其他就绪任务。

如果没有同级任务，或者更高优先级任务就绪，结果都不同。不要把它当成 `vTaskDelay(1)`。

### 7.11 hook 的排错价值

malloc failed hook 和 stack overflow hook 与前两课一样存在。

三个任务都动态创建，任何一个创建失败都可能造成某个 GPIO 不动。进入 hook 时，LED 现象只是后果，真正原因在 heap 或 stack。

### 7.12 调度器启动后的执行顺序

`vTaskStartScheduler()` 启动后，最高优先级就绪任务先运行。

因此通常 `high_task` 会先翻转 PC13，然后阻塞 500ms。随后 `same_a` 和 `same_b` 在优先级 1 上轮流运行。

### 7.13 为什么 PA1/PA2 任务没有延时也能切换

`same_a` 和 `same_b` 没有 `vTaskDelay()`，所以它们不会因为等待时间进入阻塞态。

它们能切换，靠的是两个条件：一是两者同优先级，二是循环里主动调用 `taskYIELD()`。如果只保留一个同优先级任务，`taskYIELD()` 后还是可能回到自己；如果有两个同级任务，就能把运行机会交给另一个同级就绪任务。

### 7.14 用寄存器观察时间片现象

PA1 和 PA2 翻转太快时，可以在调试器里观察 `GPIOA->ODR` 的 bit1 和 bit2。

如果两个 bit 都快速变化，说明同级任务都在运行。如果只有 bit1 变化，说明 `same_b` 没有得到执行机会或 PA2 配置错误。观察 ODR 比看 LED 更适合本课，因为时间片切换可能快到肉眼无法分辨。

### 7.15 为什么本课故意不用阻塞等待事件

真实工程里，任务通常不应该像 `same_a` 和 `same_b` 这样一直翻转引脚并 yield，因为这会持续占用 CPU。

本课这样写是为了把同优先级调度现象放大：两个任务始终就绪，调度器必须在它们之间选择。后面学队列、信号量、事件组时，任务会更多地阻塞等待事件，那时 CPU 占用会更健康。

## 8. HAL 版代码逐步讲解

### 8.1 HAL 初始化边界

HAL 版先 `HAL_Init()`，再配置时钟和 GPIO，最后创建 FreeRTOS 任务。

HAL 不参与任务调度。调度规则完全由 FreeRTOS 决定。

### 8.2 HAL RCC 配置

`RCC_OscInitTypeDef` 和 `RCC_ClkInitTypeDef` 表达 HSE、PLL x9、SYSCLK、HCLK、PCLK1、PCLK2。

这对应寄存器版 RCC/FLASH 配置，保证 tick 基准和延时准确。

### 8.3 HAL GPIO 配置

PC13 配为输出，PA1/PA2 配为输出，PA0 配为上拉输入。

`GPIO_InitTypeDef` 字段映射到底层 CRH/CRL 和 ODR 上拉选择。当前主要观察 PC13、PA1、PA2。

### 8.4 HAL 版 `same_a`

`same_a` 调用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)`，然后 `taskYIELD()`。

HAL 封装 GPIO 翻转，FreeRTOS 仍控制任务何时运行。

### 8.5 HAL 版 `same_b`

`same_b` 调用 `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_2)`，然后 `taskYIELD()`。

如果 HAL 版 PA2 不动，既要查 HAL GPIO 初始化，也要查 `same_b` 创建是否成功。

### 8.6 HAL 版 `high_task`

`high_task` 调用 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)`，再 `vTaskDelay(500ms)`。

这和寄存器版 `led_toggle_pc13()` 对应，RTOS 状态变化一致。

### 8.7 HAL 版不使用 `HAL_Delay()`

任务延时使用 `vTaskDelay()`，不是 `HAL_Delay()`。

RTOS 任务里用 FreeRTOS 延时，才能进入阻塞态并让出 CPU。HAL 只负责外设访问。

### 8.8 HAL 版创建结果检查

三个任务创建结果同样汇总到 `ok`。

如果 `ok != pdPASS`，程序停住。HAL 不会替 FreeRTOS 增加 heap 或修复栈不足。

### 8.9 HAL 与时间片无直接关系

时间片由 FreeRTOS 配置和调度器实现，和 GPIO 是否用 HAL 翻转无关。

所以 reg/hal 两版 PA1/PA2 的轮转逻辑应一致。

### 8.10 HAL 版调试建议

如果肉眼看不出 PA1/PA2，建议在两个 `HAL_GPIO_TogglePin()` 行打断点，或观察 GPIOA ODR。

不要只用 PC13 判断同优先级任务是否运行，因为 PC13 属于高优先级任务。

### 8.11 HAL 版为什么也能看底层 ODR

HAL 封装了 GPIO 操作，但没有改变硬件事实。`HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_1)` 最终仍会改变 GPIOA 输出状态。

所以 HAL 版排查 PA1/PA2 时，照样可以看 GPIOA 的 ODR，也可以看 `HAL_GPIO_TogglePin()` 是否被两个任务轮流执行。HAL 代码不是不能看寄存器，而是把写寄存器动作藏在库函数里。

### 8.12 HAL 版不应该用 HAL 延时替代 yield

本课 `same_a` 和 `same_b` 用 `taskYIELD()` 是为了观察同级就绪任务之间的调度。

如果改成 `HAL_Delay()`，任务不会按 FreeRTOS 阻塞模型表达等待，教学目标会变掉。若想让任务真正等待一段时间，应使用 `vTaskDelay()`；若想主动让给同级任务，应使用 `taskYIELD()`。

### 8.13 HAL 版任务名仍然有调试价值

创建任务时使用了 `"a"`、`"b"`、`"high"` 这些名字。

如果调试器支持 FreeRTOS 任务视图，任务名能帮助你把 PA1、PA2、PC13 对应到具体任务。即使用 HAL 初始化，任务名和优先级仍来自 FreeRTOS，不来自 HAL。

## 9. 两个版本真正应该怎么学

寄存器版让你看到 GPIO 翻转的底层寄存器后果，HAL 版让你看到同样动作的工程写法。两者共同表达的 RTOS 重点是：高优先级任务就绪时先运行，同优先级任务通过时间片和 `taskYIELD()` 共享 CPU。

学习时把三个引脚当成三个任务的示波器通道：PC13 是高优先级周期任务，PA1/PA2 是同优先级忙循环任务。哪个引脚不动，就回到对应任务的创建、优先级和 GPIO 配置去查。

## 10. 检验问题清单

### 10.1 为什么 `high_task` 不会让 PA1/PA2 永远没机会？

**答**：因为它每次翻转 PC13 后调用 `vTaskDelay(500ms)` 进入阻塞态，阻塞期间 PA1/PA2 任务才运行。

### 10.2 `taskYIELD()` 会阻塞任务吗？

**答**：不会。它只是主动让出当前运行机会，不等待固定时间，也不进入延时阻塞。

### 10.3 时间片只影响哪些任务？

**答**：主要影响同优先级就绪任务。本课是 `same_a` 和 `same_b`，不影响优先级更高的 `high_task` 抢占规则。

### 10.4 为什么 PA1/PA2 可能肉眼看不清？

**答**：两个任务没有延时，翻转频率很高，外接 LED 可能看成常亮或微亮。用逻辑分析仪或调试器更可靠。

### 10.5 如果去掉 `taskYIELD()` 会怎样？

**答**：同级任务仍可能因时间片轮转而切换，但切换更依赖 tick；若关闭时间片，一个同级任务可能长期占用 CPU。

### 10.6 为什么本课没有保存任务句柄？

**答**：因为没有挂起、恢复、删除指定任务的需求，只观察调度行为，所以创建时句柄参数可以是 NULL。

### 10.7 `configUSE_TIME_SLICING` 为 0 会影响什么？

**答**：同优先级就绪任务不会按 tick 自动时间片轮转，更依赖主动让出或阻塞，PA1/PA2 的公平性可能改变。

### 10.8 PC13 不闪但 PA1/PA2 变化说明什么？

**答**：说明调度器和低优先级任务可能在跑，重点查 `high_task` 创建、PC13 GPIO 配置或 `vTaskDelay()` 相关逻辑。

### 10.9 PA1 变化但 PA2 不变化说明什么？

**答**：优先查 `same_b` 是否创建成功、PA2 GPIO 是否配置正确，以及 `taskYIELD()` 是否仍在两个同级任务中执行。

### 10.10 本课有没有中断唤醒任务？

**答**：没有。所有任务切换来自调度器、tick、阻塞和主动让出，不涉及 FromISR API。

## 11. 工程实现步骤

### 11.1 需求分析

目标是用三个 GPIO 输出观察 FreeRTOS 优先级、时间片和主动让出的关系。

### 11.2 硬件核查

确认 PC13、PA1、PA2 可观察。PA1/PA2 最好接逻辑分析仪；仅接 LED 可能看不出快速翻转。

### 11.3 寄存器路线

配置时钟和 GPIO，创建三个任务。PA1/PA2 任务同优先级并调用 `taskYIELD()`；PC13 任务更高优先级并周期阻塞。

### 11.4 HAL 路线

用 HAL 配置同样的 GPIO，任务调度 API 保持不变。用 HAL 翻转引脚，不改变调度规则。

### 11.5 工程思维

高优先级任务必须有合理阻塞点，否则低优先级任务可能被饿死。同优先级任务也不应长期忙循环，真实工程通常通过阻塞等待事件来降低 CPU 占用。

### 11.6 常见工程陷阱

把 `taskYIELD()` 当延时、让高优先级任务不阻塞、用肉眼判断高速 PA1/PA2、忘记检查任务创建返回值、误以为 HAL 影响时间片，都是本课常见误区。

## 12. 运行现象

PC13 大约每 500ms 翻转一次，表示 `high_task` 周期醒来。PA1 和 PA2 会快速变化，表示 `same_a` 和 `same_b` 在 high_task 阻塞期间轮流运行。

如果使用调试器，可以观察 `GPIOA->ODR` 的 ODR1/ODR2 位。HAL 版也可以观察同一个寄存器，因为 HAL 最终仍改变 GPIO 输出状态。

## 13. 常见问题排查

### 13.1 PC13 不闪

检查 `high_task` 是否创建成功、PC13 是否配置为输出、系统是否进入 hook，以及 `vTaskDelay(500ms)` 是否正常返回。

### 13.2 PA1 不变化

检查 `same_a` 创建结果、PA1 CRL 或 HAL Pin 配置、是否被错误改了优先级或函数体。

### 13.3 PA2 不变化

检查 `same_b` 创建结果、PA2 配置和 `taskYIELD()` 是否执行。PA1 正常不代表 PA2 一定正常。

### 13.4 PA1/PA2 肉眼看起来常亮

这可能是正常现象，因为翻转太快。用逻辑分析仪、示波器或调试器观察。

### 13.5 进入 malloc failed hook

三个任务创建都需要 heap。检查 `configTOTAL_HEAP_SIZE`、栈深度和是否新增了更多动态对象。

### 13.6 进入 stack overflow hook

检查三个任务的栈深度。当前任务简单，若你添加 printf 或大局部变量，栈需求会增加。

### 13.7 同级任务看起来不公平

确认 `configUSE_TIME_SLICING` 是否为 1，`taskYIELD()` 是否保留，是否有某个任务优先级被改高。

### 13.8 低优先级任务完全没机会

检查高优先级任务是否还有 `vTaskDelay()`。如果高优先级任务一直就绪，低优先级任务会被饿死。

### 13.9 PC13 正常但 PA1/PA2 都不明显

这通常不是调度器完全失败，因为 PC13 正常说明 `high_task` 和 tick 至少在运行。重点查 PA1/PA2 是否接了外部 LED、翻转频率是否太高、GPIOA 时钟和 CRL 配置是否正确。用调试器观察 ODR 可以把“看不见”和“没运行”分开。

### 13.10 改优先级后现象完全变了

如果把 `same_a` 或 `same_b` 改成优先级 2，它们会和 `high_task` 同级竞争；如果改成 3，甚至可能压住 `high_task`。FreeRTOS 调度首先看优先级，再看同级轮转，所以改优先级会直接改变本课现象。

## 14. 本课最核心的结论

1. FreeRTOS 优先选择最高优先级就绪任务运行。
2. 高优先级任务阻塞后，低优先级任务才有运行机会。
3. 同优先级任务可通过时间片和 `taskYIELD()` 轮流运行。
4. `taskYIELD()` 是主动让出，不是固定时间延时。
5. PA1、PA2、PC13 分别对应三个任务的运行证据。
6. `configUSE_TIME_SLICING` 影响同优先级任务轮转。
7. reg/hal 差异在 GPIO 操作，不在调度规则。
8. 真实工程中忙循环任务会消耗 CPU，应优先使用阻塞等待事件。

## 15. 建议你现在怎么读这节课

先盯住三个任务和三个引脚的对应关系：`same_a` -> PA1，`same_b` -> PA2，`high_task` -> PC13。再看优先级：1、1、2。最后看每个任务是否阻塞或让出：PA1/PA2 任务 yield，PC13 任务 delay。

这课最好配合逻辑分析仪学习。如果没有，就在 `taskYIELD()` 前后和 `high_task` 的 `vTaskDelay()` 前打断点，观察执行顺序。

## 16. 扩展练习

1. 注释掉 `taskYIELD()`，观察 PA1/PA2 行为变化。
2. 把 `same_b` 优先级改成 2，分析和 `high_task` 的竞争。
3. 把 `high_task` 的延时改成 100ms，观察 PC13 更快抢占。
4. 关闭 `configUSE_TIME_SLICING` 后重新分析同级任务行为。
5. 给 PA1/PA2 接逻辑分析仪，观察翻转频率。

## 17. 下一课预告

- 上一课：[46_freertos_task_suspend_resume](../46_freertos_task_suspend_resume/README.md)
- 下一课：[48_freertos_time_management](../48_freertos_time_management/README.md)
