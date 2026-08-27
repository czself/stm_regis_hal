# 46_freertos_task_suspend_resume - FreeRTOS 任务挂起与恢复

## 1. 本课到底在学什么

本课表面现象是：PC13 先按 200ms 节奏闪烁，随后暂停约 1 秒，再恢复闪烁约 1 秒，如此反复。这个暂停不是 LED 坏了，也不是延时变长了，而是另一个任务把 blink 任务从调度器里挂起了。

真正要学的是 FreeRTOS 里“挂起态”和“阻塞态”的区别。`vTaskDelay()` 让任务因为等待时间进入阻塞态，时间到了会自动回到就绪态；`vTaskSuspend()` 让任务进入挂起态，它不会因为时间到期自己回来，必须由 `vTaskResume()` 或对应恢复 API 显式恢复。

这节课接在任务创建与删除之后。上一课关注任务从无到有、从有到删除；本课关注任务已经存在时，另一个任务如何临时让它不参与调度。后面的时间片、任务通知、事件组和低功耗，都需要你先分清“任务为什么不运行”。

## 2. 本课学习目标

学完本课你应该能做到：

- 能解释 PC13 为什么会“闪一会儿、停一会儿”，而不是 worker 删除后的间歇。
- 能画出 blink 任务在运行态、阻塞态、挂起态、就绪态之间的变化。
- 能说明 `vTaskSuspend(g_blink)` 和 `vTaskDelay()` 的根本区别。
- 能说明 `vTaskResume(g_blink)` 恢复的是哪个任务，为什么必须保存 `g_blink` 句柄。
- 能拆开 main 中两个 `xTaskCreate()` 的任务名、栈深度、优先级和句柄参数。
- 能解释 control 任务为什么优先级 2、blink 任务为什么优先级 1。
- 能知道 `INCLUDE_vTaskSuspend` 在 `FreeRTOSConfig.h` 中必须为 1。
- 能区分“blink 任务被挂起”和“blink 任务正在 `vTaskDelay(200ms)` 阻塞”。
- 能把寄存器版 PC13 翻转和 HAL 版 `HAL_GPIO_TogglePin()` 对应起来。
- 能根据 LED 永久停住、完全不闪、恢复节奏不对等现象定位问题。

## 3. 本课目录结构

```text
46_freertos_task_suspend_resume/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`platformio.ini` 为两个版本都加入了 `../../freertos` 和 `../../lib/FreeRTOS-Kernel/portable/GCC/ARM_CM3` 头文件路径，并通过 `lib_extra_dirs = ../../lib` 使用本仓库内核。reg/hal 的 RTOS 行为一致，差别在 RCC/GPIO 初始化和 GPIO 翻转 API。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill。
- 下载器：ST-Link。
- 时钟：HSE 8MHz，PLL x9，系统时钟 72MHz。
- 观察点：PC13 板载 LED，通常低电平点亮。
- 辅助输出：PA1、PA2 被初始化为输出，但本课没有翻转。
- 输入：PA0 被配置成上拉输入，当前任务逻辑没有读取。
- FreeRTOS tick：`configTICK_RATE_HZ = 1000`，1 tick 约为 1ms。
- 关键配置：`INCLUDE_vTaskSuspend = 1`，否则挂起/恢复 API 不可用。

本课没有外部中断和队列。不要把 PA0 当作触发源，暂停与恢复完全由 control 任务周期性调用 API 产生。

## 5. 先建立一个最基本的脑图

本课完整链路是：

```text
复位启动
  -> 配 72MHz 时钟
  -> 配 PC13 输出、PA1/PA2 输出、PA0 上拉输入
  -> 创建 blink_task，优先级 1，句柄保存到 g_blink
  -> 创建 control_task，优先级 2
  -> 启动调度器
  -> blink 每 200ms 翻转 PC13，然后阻塞等待下一次 tick 到期
  -> control 调用 vTaskSuspend(g_blink)
  -> blink 进入挂起态，不再因 200ms 到期恢复运行
  -> control 自己 vTaskDelay(1000ms)
  -> control 醒来后调用 vTaskResume(g_blink)
  -> blink 回到就绪态，继续闪烁
  -> control 再延时 1000ms 后重复这个过程
```

这条链路里有两个“等待”：blink 的 200ms 是阻塞等待，control 的 1000ms 也是阻塞等待；但 blink 被 control 挂起后，不再由时间自动唤醒。这个差别是本课的核心。

PC13 只是 blink 是否有机会运行的现象层证据。真正控制 PC13 是否继续翻转的是 RTOS 调度状态：blink 在阻塞态时会自动回来，在挂起态时必须等 control 恢复。

## 6. 先认识本课里出现的核心名词

### 6.1 `挂起态` 是什么

挂起态是 FreeRTOS 任务状态之一，英文通常是 Suspended state，属于 RTOS 调度状态层。

任务进入挂起态后，不在就绪列表和延时到期唤醒路径里竞争 CPU。它不等待 tick、不等待队列、不等待信号量，只等待其他代码显式恢复。本课 `vTaskSuspend(g_blink)` 就把 blink 放进挂起态。

如果误把挂起当成延时，常见现象是任务永久不运行，因为时间不会自动把它恢复。

### 6.2 `阻塞态` 是什么

阻塞态是任务等待时间或事件时的状态，英文是 Blocked state，属于 RTOS 调度状态层。

`vTaskDelay(pdMS_TO_TICKS(200))` 让 blink 阻塞 200ms；`vTaskDelay(pdMS_TO_TICKS(1000))` 让 control 阻塞 1000ms。tick 到期后，任务自动回到就绪态。

阻塞态和挂起态都表现为“任务此刻不运行”，但恢复机制完全不同。排错时必须问：它在等时间/事件，还是被显式挂起？

### 6.3 `vTaskSuspend()` 是什么

`vTaskSuspend()` 是 FreeRTOS 挂起任务 API，属于任务控制层。

源码里 `control_task()` 调用 `vTaskSuspend(g_blink)`。参数是要挂起的任务句柄，不是任务函数名。调用后，blink 任务即使原本延时到期，也不会继续运行，直到被恢复。

如果句柄错了，可能挂起错误任务；如果传入未初始化句柄，系统行为不可信。本课 main 先创建 blink 并保存句柄，再创建 control，就是为了保证 control 有目标。

### 6.4 `vTaskResume()` 是什么

`vTaskResume()` 是 FreeRTOS 恢复挂起任务 API，属于任务控制层。

control 延时 1000ms 后调用 `vTaskResume(g_blink)`，把 blink 从挂起态移回可调度状态。如果 blink 优先级和就绪条件满足，它会继续翻转 PC13。

如果忘记恢复，LED 会在第一次挂起后永久停住。这个现象和栈溢出、GPIO 错误都可能表现为灯停，所以要用调试器看 control 是否还在运行。

### 6.5 `TaskHandle_t g_blink` 是什么

`g_blink` 是保存 blink 任务句柄的静态全局变量，属于 FreeRTOS 任务引用层。

main 调用 `xTaskCreate(blink_task, "blink", 128, NULL, 1, &g_blink)` 时，内核把 blink 的句柄写入它。control 后续用这个句柄指定挂起和恢复目标。

如果 main 创建 blink 时最后一个参数写 NULL，control 就没有句柄可用。本课和上一课都在训练“需要控制一个具体任务时必须保存句柄”。

### 6.6 `control_task` 是什么

`control_task` 是本课的控制任务，属于应用任务层。

它不直接翻转 PC13，而是周期性改变 blink 的调度状态：挂起 blink、等待 1000ms、恢复 blink、等待 1000ms。它的优先级是 2，高于 blink 的 1，因此它醒来后能及时执行控制动作。

如果 control 没创建成功，blink 会一直按 200ms 闪烁，不会出现暂停段。

### 6.7 `blink_task` 是什么

`blink_task` 是本课的被控制任务，属于应用任务层。

它只做两件事：翻转 PC13，然后 `vTaskDelay(200ms)`。它自己不知道何时会被挂起，也不主动恢复自己。这能清楚体现“任务可以被另一个任务控制调度状态”。

如果 blink 没创建成功，PC13 完全不会按本课节奏闪烁。

### 6.8 `INCLUDE_vTaskSuspend` 是什么

`INCLUDE_vTaskSuspend` 是 `FreeRTOSConfig.h` 中控制 API 是否编译的宏，属于 FreeRTOS 配置层。

本仓库配置为 1，所以 `vTaskSuspend()` 和 `vTaskResume()` 可用。如果改成 0，代码可能编译或链接失败，表现为找不到相关函数。

它说明 FreeRTOS 不是所有 API 永远都启用，课程文档必须把用到的配置和源码对应起来。

### 6.9 `pdMS_TO_TICKS(1000)` 是什么

`pdMS_TO_TICKS(1000)` 把 1000ms 换算成 tick 数，属于 FreeRTOS 时间宏层。

control 用它控制“挂起 1 秒、恢复 1 秒”的节奏。由于 `configTICK_RATE_HZ = 1000`，1000ms 大约就是 1000 tick。

如果 tick 频率配置和实际 SysTick 不一致，暂停/恢复周期会偏离预期。

### 6.10 `BaseType_t ok` 是什么

`BaseType_t` 是 FreeRTOS 常用基础整数类型，属于 RTOS C 类型层。

main 中用 `blink_ok` 和 `ctrl_ok` 两个 `BaseType_t` 分别保存两次创建结果，再用 `(blink_ok != pdPASS) || (ctrl_ok != pdPASS)` 判断。只要任一不是 `pdPASS`，就关中断停住。

这段写法能跑，但要知道它依赖 `pdPASS` 的值参与按位与。阅读时重点不是模仿写法，而是理解两个任务创建都必须成功。

### 6.11 `优先级 1 和 2` 控制什么

FreeRTOS 中数字越大，任务优先级越高。本课 blink 优先级 1，control 优先级 2。

control 优先级更高，所以它从 `vTaskDelay(1000ms)` 醒来后，会优先执行挂起或恢复动作。blink 只有在 control 阻塞后才继续运行。

如果把 blink 优先级调高，现象可能仍能运行，但控制动作的及时性和调度顺序会变，需要重新分析。

### 6.12 `空闲任务` 在本课有什么作用

空闲任务是 FreeRTOS 自动创建的最低优先级任务，属于内核基础任务层。

当 blink 被挂起、control 又在 1000ms 延时里阻塞时，系统没有其他就绪任务，空闲任务就会运行。这说明“LED 停住”不等于 CPU 停住，而可能只是业务任务都不可运行。

调试时如果只看灯，容易误判系统死机；看任务状态会更准确。

### 6.13 `PC13` 在本课做什么

PC13 是 STM32 GPIO 硬件层的输出引脚，是 blink 任务是否运行的可见证据。

寄存器版通过 `led_toggle_pc13()` 读 ODR、写 BRR/BSRR；HAL 版通过 `HAL_GPIO_TogglePin()`。PC13 不参与挂起/恢复决策，只显示 blink 得到 CPU 后执行了翻转语句。

### 6.14 `SVC / PendSV / SysTick` 和本课有什么关系

这三个 Cortex-M 异常由 FreeRTOS 端口使用，属于内核调度支撑层。

SysTick 提供 `vTaskDelay()` 到期依据，PendSV 负责上下文切换，SVC 用于启动第一个任务。若这些映射错误，blink 和 control 的延时、切换、启动都会异常。

## 7. 寄存器版代码逐步讲解

### 7.1 头文件分工

寄存器版包含 `stm32f1xx.h`、`FreeRTOS.h`、`task.h`。

前者提供寄存器定义，后两者提供任务类型和 API。`vTaskSuspend()`、`vTaskResume()`、`xTaskCreate()` 都来自 FreeRTOS 任务模块。

### 7.2 时钟初始化和 tick 准确性

`system_clock_72mhz_init()` 配置 Flash 等待周期、HSE、PLL x9、APB1 二分频，并切换 SYSCLK 到 PLL。

FreeRTOS 配置里 `configCPU_CLOCK_HZ` 是 72MHz。若实际时钟不是 72MHz，SysTick 周期就不准，200ms 闪烁和 1000ms 暂停都会偏。

### 7.3 GPIO 时钟和模式配置

`gpio_init()` 打开 GPIOC、GPIOA、AFIO 时钟，配置 PC13 输出、PA1/PA2 输出、PA0 上拉输入。

PC13 属于 CRH，PA0/PA1/PA2 属于 CRL。先清 MODE/CNF 再设置新值，避免旧配置残留。当前课程主要观察 PC13。

### 7.4 `led_toggle_pc13()` 的硬件动作

函数读取 `GPIOC->ODR` 判断 PC13 当前输出，再写 `BRR` 或 `BSRR` 改变输出锁存。

BluePill 常见 PC13 LED 低电平亮，所以输出翻转会表现为亮灭变化。若硬件板载 LED 接法不同，现象可能反相，但节奏不变。

### 7.5 hook 函数的错误定位意义

malloc failed hook 和 stack overflow hook 都关中断后死循环。

本课创建两个任务，如果 heap 不足会停在 malloc hook；如果任务栈不够会停在 stack hook。LED 停住时，先确认是不是进入 hook，而不要只怀疑挂起逻辑。

### 7.6 main 创建 blink 任务

`xTaskCreate(blink_task,"blink",128,NULL,1,&g_blink)` 创建被控制任务。

栈深度 128，优先级 1，句柄写入 `g_blink`。这个句柄是后续挂起/恢复的唯一目标标识。创建失败时，`ok` 不会保持 `pdPASS`。

### 7.7 main 创建 control 任务

`xTaskCreate(control_task,"ctrl",128,NULL,2,NULL)` 创建控制任务。

它不需要保存句柄，因为本课没有别的任务控制它。优先级 2 高于 blink，可以让它在 1000ms 到期后及时执行挂起和恢复。

### 7.8 两个创建结果怎么检查

源码用 `blink_ok` 和 `ctrl_ok` 分别保存两次创建结果。

如果任一创建失败，`(blink_ok != pdPASS) || (ctrl_ok != pdPASS)` 成立，main 就关中断死循环。阅读时要知道这不是“两个任务都一定创建成功”，而是显式把失败挡在调度器启动前。

### 7.9 `blink_task()` 的阻塞节奏

blink 无限循环：翻转 PC13，然后 `vTaskDelay(200ms)`。

这意味着它大部分时间在阻塞态。只有 200ms 到期并且没有被挂起时，它才会回到就绪态并获得运行机会。

### 7.10 `control_task()` 的挂起半周期

control 先调用 `vTaskSuspend(g_blink)`，让 blink 进入挂起态。

随后 control 自己 `vTaskDelay(1000ms)`。这 1 秒内，blink 不会因为自己的 200ms 延时到期而运行，PC13 停在某个状态。

### 7.11 `control_task()` 的恢复半周期

control 1 秒后醒来，调用 `vTaskResume(g_blink)`。

blink 被恢复后可以继续参与调度。control 随后再延时 1000ms，这一秒里 blink 会按 200ms 节奏翻转 PC13。

### 7.12 为什么本课没有删除任务

blink 和 control 都是长期任务，没有调用 `vTaskDelete()`。

这和上一课不同。上一课强调任务生命周期结束；本课强调任务存在期间的调度状态控制。把两课混在一起，会误以为挂起就是删除，这是错误的。

### 7.13 为什么本课没有 FromISR API

本课没有外部中断参与恢复，也没有 `xTaskResumeFromISR()`。

所有挂起和恢复都发生在任务上下文。后面课程讲中断临界区时，才会讨论中断中能调用哪些 API、优先级如何限制。

### 7.14 用调试器怎么看 blink 的状态

如果调试器支持 FreeRTOS 任务列表，可以观察 blink 在 Running、Blocked、Suspended、Ready 之间变化。

没有任务列表时，也可以用断点间接判断：停在 `vTaskSuspend(g_blink)` 后，PC13 应停止变化；停在 `vTaskResume(g_blink)` 后，PC13 应恢复 200ms 翻转。这个观察比只看肉眼 LED 更可靠，因为 LED 停住可能是正确挂起，也可能是错误死机。

### 7.15 重复挂起同一个任务会怎样理解

本课 control 每轮只挂起一次、恢复一次，没有嵌套计数。

FreeRTOS 的任务挂起不是信号量计数模型。你不应该把它理解成“挂起两次就要恢复两次”。工程里如果多个控制源都可能暂停同一任务，应额外设计状态机或引用计数，而不是随意交叉调用挂起和恢复。

## 8. HAL 版代码逐步讲解

### 8.1 `HAL_Init()` 和 FreeRTOS 任务的边界

HAL 版 main 先调用 `HAL_Init()`，再配置时钟和 GPIO，最后创建 FreeRTOS 任务。

`HAL_Init()` 不创建任务，也不管理挂起恢复。它只是 HAL 外设库的基础初始化。

### 8.2 HAL RCC 字段对应寄存器版配置

`RCC_OscInitTypeDef` 设置 HSE、PLL 源、PLL x9；`RCC_ClkInitTypeDef` 设置 SYSCLK、AHB、APB1、APB2。

`HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2)` 对应寄存器版 Flash 等待周期和时钟切换。时间类 RTOS API 依赖这个时钟基础准确。

### 8.3 HAL GPIO 字段对应 PC13

`GPIO_InitTypeDef` 中 `Pin=GPIO_PIN_13`、`Mode=GPIO_MODE_OUTPUT_PP`、`Pull=GPIO_NOPULL`、`Speed=GPIO_SPEED_FREQ_LOW` 表示 PC13 推挽输出。

底层对应 CRH MODE13/CNF13 设置。`HAL_GPIO_WritePin(..., GPIO_PIN_SET)` 初始让 LED 熄灭。

### 8.4 HAL 版 PA0/PA1/PA2

PA1/PA2 配置为输出，PA0 配置为上拉输入。

这些字段和寄存器版意图一致。当前课程不读取 PA0，也不翻转 PA1/PA2，所以排错主要看 PC13 和任务状态。

### 8.5 HAL 版 blink 任务

HAL 版 blink 任务用 `HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13)` 翻转 LED，再 `vTaskDelay(200ms)`。

RTOS 状态变化和寄存器版完全一致：运行、阻塞、等待 tick、被挂起时停止参与调度。

### 8.6 HAL 版 control 任务

HAL 版 control 任务同样调用 `vTaskSuspend(g_blink)` 和 `vTaskResume(g_blink)`。

这两个 API 不属于 HAL。HAL 只影响 GPIO 翻转和初始化，不影响 FreeRTOS 挂起恢复语义。

### 8.7 HAL 版不使用 `HAL_Delay()`

任务里使用 `vTaskDelay()`，没有使用 `HAL_Delay()`。

这是正确的 RTOS 写法：任务等待应该交给调度器，让其他任务或空闲任务运行。把 RTOS 任务写成 HAL 裸机延时，会破坏多任务协作。

### 8.8 HAL 版创建失败处理

两个任务创建结果分别存入 `blink_ok` 和 `ctrl_ok`，任一失败后关中断死循环。

如果 HAL 版 PC13 完全不闪，先查创建结果和 hook，再查 GPIO。不能因为 HAL 初始化写法简洁，就忽略 RTOS 对象创建可能失败。

### 8.9 HAL 与 FreeRTOS SysTick 的关系

HAL 初始化阶段可能依赖 SysTick，FreeRTOS 调度阶段也使用 SysTick。

本课任务时间全部通过 `vTaskDelay()` 表达，因此调度器启动后以 FreeRTOS tick 为准。若 SysTick 映射不正确，HAL 版和寄存器版都会出现任务延时异常。

### 8.10 HAL 版也依赖 `INCLUDE_vTaskSuspend`

HAL 版能否调用 `vTaskSuspend()` 仍由 `FreeRTOSConfig.h` 控制。

这再次说明 HAL 不改变 FreeRTOS 编译配置。API 是否可用，取决于内核配置，不取决于你用寄存器还是 HAL 初始化 GPIO。

### 8.11 HAL 版 Watch 应该看哪些量

HAL 版排查时，除了看 PC13，还应该看 `g_blink` 是否非 NULL、control 是否周期性执行到 suspend/resume 两行、是否进入两个 hook。

如果只看 HAL GPIO 返回值，很容易漏掉 RTOS 层问题。HAL GPIO 配好了，只能证明引脚能被驱动；blink 被挂起、control 没创建、调度器没启动，都会让 LED 现象不符合预期。

### 8.12 HAL 回调和本课无关

本课 HAL 版没有 EXTI 回调、没有中断恢复任务。

这点要和下一批中断课程分开。当前暂停和恢复完全由 control 任务主动执行，不来自按键中断。若你看到 PA0 被配置成输入，也不要推断它会触发恢复。

## 9. 两个版本真正应该怎么学

寄存器版适合看硬件细节：GPIOC 时钟、PC13 CRH、ODR、BSRR、BRR。HAL 版适合看工程抽象：RCC/GPIO 结构体字段如何表达同样配置。

本课的主线是任务状态。两个版本都创建 blink 和 control，control 周期性挂起/恢复 blink，blink 只在可调度时翻转 PC13。只要你能解释 LED 停住时 blink 是挂起态还是阻塞态，这课就抓住了。

读代码时不要把“LED 不闪”都当成故障。挂起半周期里 LED 停住是正确现象；永久停住才需要排查句柄、恢复 API、任务创建和 hook。

## 10. 检验问题清单

### 10.1 挂起态和阻塞态最根本的区别是什么？

**答**：阻塞态等待时间或事件满足后会自动回到就绪态；挂起态不会自动恢复，必须显式调用恢复 API。

### 10.2 为什么必须保存 `g_blink`？

**答**：control 需要指定挂起和恢复哪个任务。没有 blink 的句柄，就无法调用 `vTaskSuspend(g_blink)` 和 `vTaskResume(g_blink)`。

### 10.3 control 为什么优先级比 blink 高？

**答**：control 醒来后要及时改变 blink 状态。优先级 2 高于 blink 的 1，可以让控制动作更确定。

### 10.4 LED 停 1 秒是不是系统死机？

**答**：不是。挂起半周期里 blink 被挂起，control 正在延时，空闲任务可能在运行。系统仍然活着，只是业务 LED 不翻转。

### 10.5 如果忘记 `vTaskResume()` 会怎样？

**答**：blink 第一次被挂起后不会再自动回来，PC13 会永久停住，但 control 任务本身可能仍在运行。

### 10.6 `INCLUDE_vTaskSuspend` 为 0 会怎样？

**答**：挂起/恢复 API 可能不可用，编译或链接会失败。这个宏必须和源码使用的 API 对齐。

### 10.7 本课有没有从中断恢复任务？

**答**：没有。挂起和恢复都发生在任务上下文，没有使用 `xTaskResumeFromISR()`。

### 10.8 为什么 blink 里还要 `vTaskDelay(200ms)`？

**答**：它让 LED 闪烁节奏可见，也让 blink 主动阻塞，把 CPU 交给 control 或空闲任务。

### 10.9 PA0 在本课触发恢复吗？

**答**：不触发。PA0 被配置成上拉输入，但源码没有读取它，也没有配置 EXTI。

### 10.10 reg/hal 版本的挂起恢复逻辑是否不同？

**答**：不同点不在 RTOS。两个版本都用同样的 `vTaskSuspend()` 和 `vTaskResume()`，差别只在 GPIO 初始化和翻转方式。

## 11. 工程实现步骤

### 11.1 需求分析

目标是演示一个长期存在的任务被另一个任务周期性暂停和恢复，突出挂起态与阻塞态的区别。

### 11.2 硬件核查

确认 PC13 LED 正常，HSE 时钟可靠，ST-Link 可下载。不要期待 PA0 按键改变现象，因为当前代码没有使用按键。

### 11.3 寄存器路线

手动配置 72MHz 时钟和 GPIO，创建 blink 与 control。blink 用寄存器翻转 PC13，control 用 FreeRTOS API 控制 blink 状态。

### 11.4 HAL 路线

用 HAL 初始化时钟和 GPIO，任务控制仍用 FreeRTOS API。HAL 版重点观察 `HAL_GPIO_TogglePin()` 只是替代寄存器翻转。

### 11.5 工程思维

挂起是强控制手段，适合明确暂停某个任务，但不适合替代普通延时或事件等待。长期工程里，队列、信号量、通知往往比随意挂起恢复更清晰。

### 11.6 常见工程陷阱

句柄未初始化就挂起、忘记恢复、把挂起当延时、在不确定任务状态时反复挂起恢复、把 LED 停住误判成死机、忽略 `INCLUDE_vTaskSuspend` 配置，都是本课重点避免的问题。

## 12. 运行现象

正常情况下，PC13 会闪烁约 1 秒，然后停住约 1 秒，再恢复闪烁。闪烁期间 blink 每 200ms 翻转一次；停住期间 blink 被挂起，无法因为 200ms 到期自动运行。

调试器里可以观察 `g_blink` 是否非 NULL，也可以在 `control_task()` 的 `vTaskSuspend()` 和 `vTaskResume()` 处打断点。若断点周期性命中，说明 control 仍在运行，LED 停住很可能是挂起现象而非死机。

HAL 版现象应与寄存器版一致，只是 GPIO 翻转由 HAL API 完成。

## 13. 常见问题排查

### 13.1 PC13 完全不闪

先查两个 `xTaskCreate()` 是否都返回 `pdPASS`，再查是否进入 malloc failed hook 或 stack overflow hook。若任务正常，再查 GPIOC 时钟和 PC13 配置。

### 13.2 PC13 第一次停住后永远不恢复

检查 control 是否执行到 `vTaskResume(g_blink)`，以及 `g_blink` 是否是有效句柄。也要确认 control 自己没有栈溢出或创建失败。

### 13.3 PC13 一直闪，没有暂停

检查 control 是否创建成功，优先级是否仍高于 blink，`vTaskSuspend(g_blink)` 是否被执行。若 control 没运行，blink 会一直按 200ms 闪。

### 13.4 暂停/恢复周期不对

检查 `configTICK_RATE_HZ`、系统时钟 72MHz 是否正确，以及 `pdMS_TO_TICKS(1000)` 是否被修改。

### 13.5 编译提示 `vTaskSuspend` 找不到

检查 `FreeRTOSConfig.h` 中 `INCLUDE_vTaskSuspend` 是否为 1，并确认工程包含的是本仓库 `freertos` 配置路径。

### 13.6 HAL 版停在延时相关位置

本课任务里不应使用 `HAL_Delay()`。若你改动后用了 HAL 延时，要重新确认 SysTick 和 FreeRTOS tick 关系。

### 13.7 LED 停住但 CPU 仍在运行

这是挂起半周期的正常可能。用调试器看 control 任务断点或空闲任务运行情况，不要只凭 LED 判断系统死机。

### 13.8 进入 stack overflow hook

说明 blink 或 control 栈不够，或者栈被破坏。增大对应任务栈深度，减少局部变量，并观察栈水位。

### 13.9 `g_blink` 是 NULL 或异常值

如果 `g_blink` 一直是 NULL，说明 blink 创建失败或创建语句没有执行到。先查 `ok` 和 main 的失败分支。如果 `g_blink` 看起来像异常地址，可能是内存被破坏，要查栈溢出、越界写和错误使用句柄。

### 13.10 control 正常运行但 blink 不恢复

在 `vTaskResume(g_blink)` 后打断点，确认语句确实执行。若执行后仍不恢复，检查 blink 是否已经被删除、句柄是否仍有效、是否误改了 blink 任务函数导致它卡在别处。本课源码没有删除 blink，所以正常情况下恢复后应继续闪烁。

## 14. 本课最核心的结论

1. 挂起态和阻塞态都能让任务暂时不运行，但恢复机制完全不同。
2. `vTaskSuspend()` 需要有效任务句柄，本课依赖 `g_blink` 指向 blink 任务。
3. `vTaskResume()` 是 blink 从挂起态回到可调度状态的唯一当前路径。
4. `vTaskDelay()` 是阻塞等待，会因 tick 到期自动恢复。
5. control 优先级高于 blink，是为了及时执行挂起和恢复动作。
6. PC13 停住不一定是死机，也可能是 blink 正确处于挂起态。
7. reg/hal 差异在 GPIO 表达方式，不在 FreeRTOS 挂起恢复语义。
8. `INCLUDE_vTaskSuspend` 必须和源码使用的 API 保持一致。

## 15. 建议你现在怎么读这节课

第一遍只看第 5 章，把 LED 的“闪”和“停”对应到 blink 的可运行和挂起状态。第二遍看 `control_task()`，用断点确认它先 suspend、延时、resume、再延时。第三遍看 `blink_task()`，理解它自己的 200ms 阻塞和外部挂起是两回事。

这课最值得做的调试动作，是在 `vTaskSuspend()` 和 `vTaskResume()` 两行分别打断点。你会看到 LED 停住不是神秘现象，而是任务状态被明确改变。

## 16. 扩展练习

1. 把 control 的两个 1000ms 改成 2000ms，观察暂停和恢复段变长。
2. 把 blink 的 200ms 改成 100ms，观察恢复段内闪烁更快。
3. 故意注释掉 `vTaskResume(g_blink)`，验证 blink 永久停住。
4. 把 control 优先级改成和 blink 相同，观察现象是否仍稳定。
5. 在调试器里查看 `g_blink`，确认它不是 NULL。

## 17. 下一课预告

- 上一课：[45_freertos_task_create_delete](../45_freertos_task_create_delete/README.md)
- 下一课：[47_freertos_scheduler_time_slice](../47_freertos_scheduler_time_slice/README.md)
