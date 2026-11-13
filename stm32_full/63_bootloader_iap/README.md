# 第 63 课：Bootloader 基础

## 1. 本课到底在学什么

本课表面现象是：上电后串口先打印 `[BOOT]` 信息，3 秒内发送字符 `u` 会停在 Bootloader；不发就自动跳到 app，app 以 2Hz 翻转 PC13，并每秒打印 `[APP] alive`。

真正要学的是：**一个芯片里如何同时放两段程序，并让第一段把控制权交给第二段**。这条链路是：

```text
上电复位
  -> CPU 从 0x08000000 取向量表前两个字
  -> MSP = app 之外的另一套栈顶，PC = Bootloader 的 Reset_Handler
  -> Bootloader 初始化串口/LED，留出 3 秒窗口
  -> 检查 0x08008000 处的向量表是否合法
  -> 关中断、停 SysTick、清 NVIC、设 VTOR、设 MSP
  -> 跳到 0x08008000 的 Reset_Handler
  -> app 重新初始化时钟/串口，把自己的向量表挂到 VTOR
```

本课不涉及“从串口接收固件并写 Flash”，那是第 64 课的内容。本课只把“分区 + 跳转”讲透，因为跳转错误是 Bootloader 最常见的死机原因。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么 Bootloader 必须烧在 `0x08000000`，app 必须烧在 `0x08008000`？
2. app 的链接脚本改了什么？不改会怎样？
3. 向量表的第 0 个字和第 1 个字分别是什么？
4. 跳转前为什么要设置 `SCB->VTOR`？不设会怎样？
5. 跳转前为什么要设置 MSP？不设会怎样？
6. 为什么跳转前要关中断、停 SysTick、清 NVIC？
7. `app_is_valid()` 检查了哪几个条件？空片 Flash 为什么必须被拦住？
8. Reset_Handler 地址最低位为什么必须是 1？
9. 用 ST-Link 烧 app 时，为什么不需要手动填 `0x08008000`？
10. HAL 版里哪些步骤 HAL 帮你做了，哪些必须自己写？

## 3. 本课目录结构

```text
63_bootloader_iap/
├── README.md
├── reg/
│   ├── platformio.ini      # env:bootloader / env:app
│   ├── ld/bootloader.ld    # Bootloader 链接脚本（32KB 上限）
│   ├── ld/app.ld           # app 专用链接脚本（FLASH 起始 0x08008000）
│   └── src/
│       ├── bootloader.c
│       └── app.c
└── hal/
    ├── platformio.ini
    ├── ld/bootloader.ld
    ├── ld/app.ld
    └── src/
        ├── bootloader.c
        └── app.c
```

一个 `reg/`（或 `hal/`）目录里有两个 PlatformIO 环境：

```bash
# 编译 Bootloader（链接到 0x08000000）
pio run -d stm32_full/63_bootloader_iap/reg -e bootloader

# 烧录 Bootloader
pio run -d stm32_full/63_bootloader_iap/reg -e bootloader -t upload

# 编译并烧录 app（链接到 0x08008000）
pio run -d stm32_full/63_bootloader_iap/reg -e app -t upload
```

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill（64KB Flash / 20KB SRAM）
- 下载器：ST-Link（SWD）
- 串口：USB-TTL，PA9 接模块 RX，PA10 接模块 TX，共地
- LED：板载 PC13
- 串口参数：115200 8N1

注意：BluePill 市面上有 128KB Flash 的兼容芯片，本课按数据手册的 64KB 规划。若你的芯片确实是 128KB，也不要随意扩大 app 区，先把 64KB 方案跑通。

## 5. 先建立一个最基本的脑图

```text
Flash 地址空间
  0x08000000 ~ 0x08007FFF   32KB  Bootloader
  0x08008000 ~ 0x0800FFFF   32KB  app

app 向量表（位于 0x08008000）
  +0x00 : 初始 MSP（栈顶）
  +0x04 : Reset_Handler 地址
  +0x08 : NMI_Handler
  ...

Bootloader 启动
  -> 初始化 72MHz / SysTick / LED / USART1
  -> 打印版本
  -> 3 秒内等 'u'
       收到 -> 停在 bootloader，LED 1Hz
       没收到 -> app_is_valid() ?
                  合法 -> jump_to_app()
                  非法 -> 停在 bootloader，LED 1Hz

jump_to_app()
  -> 关中断
  -> 停 SysTick
  -> 清 NVIC 使能/挂起
  -> SCB->VTOR = 0x08008000
  -> MSP = app 栈顶
  -> 开中断，跳 Reset_Handler
```

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词 | 一句话提醒 |
|------|-----------|
| `0x08000000` | STM32F1 用户 Flash 起始地址，上电默认从这里取指（04 课） |
| 向量表 | 中断入口地址表，前两个字是 MSP 和 Reset_Handler（04 课） |
| `FLASH->ACR/KEYR/CR` | Flash 等待周期、解锁、擦写控制（03/38 课） |
| `USART1->BRR` | 波特率分频，72MHz/115200 时写 `0x0271`（21 课） |
| `SysTick` | 24 位递减系统定时器，本课用 1ms 中断计时（05 课） |

### 6.2 `Bootloader` 是什么

Bootloader 是“上电后第一段运行的程序”，它的职责不是业务功能，而是决定“接下来运行谁”。在 STM32 上，它就是一个普通固件，只是被烧在 Flash 最前面。

本课需要它，是因为 MCU 没有操作系统来加载程序。复位后 CPU 只会去 `0x08000000` 取向量表，谁占住这个地址，谁就获得第一棒。

配错的表现：如果 app 也链接到 `0x08000000`，烧录 app 会直接覆盖 Bootloader，板子仍然能跑，但“升级入口”永远消失。

### 6.3 `IAP` 是什么

IAP（In-Application Programming）指“程序运行期间通过通信接口改写自己的 Flash”。它的前提就是本课的分区：Bootloader 区不能被自己擦掉，app 区可以被重写。

IAP 与 ICP（用 ST-Link 下载）的区别是：IAP 不需要下载器，现场通过串口/CAN/LoRa 就能升级。

### 6.4 `地址分区` 是什么

分区是把同一块 Flash 按用途切成互不重叠的区间。本课的分区是：

```text
Bootloader : 0x08000000 ~ 0x08007FFF  (32KB)
app        : 0x08008000 ~ 0x0800FFFF  (32KB)
```

为什么是 32KB：F103C8 只有 64KB，对半开最容易讲清楚。真实项目按 Bootloader 实际大小缩减，例如 12KB Bootloader + 52KB app，只要链接脚本和擦除范围一致即可。

风险：分区边界必须同时写在三个地方——`app.ld` 的 `MEMORY`、Bootloader 的 `APP_START_ADDR`、烧录命令。任何一处不一致，都会出现“app 能编译但跳过去跑飞”。

### 6.5 `向量表重定位` 和 `VTOR` 是什么

Cortex-M3 的向量表默认在 `0x00000000`（STM32F1 映射到 Flash 起始）。`SCB->VTOR`（Vector Table Offset Register）告诉内核“我的向量表实际在哪个地址”。

app 的向量表在 `0x08008000`，所以 app 必须在打开任何中断之前写：

```c
SCB->VTOR = 0x08008000UL;
```

不写的后果：app 里 SysTick、USART 中断仍然跳到 Bootloader 的向量表，执行 Bootloader 的处理函数。典型现象是 `HAL_Delay()` 永远不返回、串口接收中断进不去，甚至 HardFault。

### 6.6 `MSP` 是什么

MSP（Main Stack Pointer）是主栈指针。复位时硬件自动从向量表第 0 个字加载 MSP。

Bootloader 和 app 有各自的栈顶（都在 SRAM 顶端，但必须是各自链接脚本算出来的值）。跳转前执行：

```c
__set_MSP(app_stack);
```

不设的后果：app 继续使用 Bootloader 的栈。两者栈空间重叠，函数调用一深就会踩坏对方数据，表现为“跑一会儿就随机死机”。

### 6.7 `Thumb 位` 是什么

Cortex-M 只支持 Thumb 指令集，函数地址最低位用来标记“这是 Thumb 代码”。所以 Reset_Handler 地址一定是奇数，例如 `0x08008105`。

`app_is_valid()` 检查 `(app_reset & 1U) == 0U` 就是拦住偶数地址。跳到偶数地址会触发 UsageFault/HardFault。

### 6.8 `函数指针跳转` 是什么

C 语言里函数名就是地址。把整数地址强制转换成函数指针再调用：

```c
((void (*)(void))app_reset)();
```

这一步等价于 `BX app_reset`：CPU 把 PC 设成新地址，从那里继续取指。它不会保存返回地址，因为 app 永远不会“返回”Bootloader。

### 6.9 `链接脚本` 是什么

链接脚本（`.ld`）告诉链接器：代码放哪个地址、RAM 从哪里开始、栈顶是多少。本课 `ld/app.ld` 的关键行：

```ld
MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 32K
}
```

只改这两行，其余段布局沿用 ST 标准脚本即可。`_estack` 仍然指向 RAM 顶端，所以 app 的 MSP 自动变成 `0x20005000`。

### 6.10 `SRAM 合法性检查` 是什么

本课检查 `0x20000000 <= app_sp <= 0x20005000`。空片 Flash 读出来全是 `0xFF`，即 `app_sp = 0xFFFFFFFF`。如果不检查，跳过去会把 MSP 设成非法地址，第一次压栈就 HardFault。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

- `system_clock_72mhz_init()`：HSE 8MHz → PLL ×9 → 72MHz（03 课）
- `systick_init()`：1ms 中断，`g_ms_ticks++`（05 课）
- `led_pc13_init()` / `led_toggle()`：PC13 推挽输出（01 课）
- `uart1_init()`：PA9 复用推挽、PA10 浮空输入、BRR=`0x0271`（21 课）
- `uart1_putc()` / `uart1_puts()`：轮询 TXE 发送（21 课）

### 7.2 新增步骤：3 秒窗口

```c
start_ms = g_ms_ticks;
while ((g_ms_ticks - start_ms) < BOOT_WAIT_MS) {
    int ch = uart1_try_getc();
    if ((ch == 'u') || (ch == 'U')) {
        /* 停在 bootloader */
    }
}
```

为什么用非阻塞读：如果直接 `while (RXNE==0)`，没有上位机时 Bootloader 会永远卡死，app 永远起不来。用 `uart1_try_getc()` 每轮立即返回，才能同时做“等字符”和“计时超时”两件事。

错误后果：把窗口设成无限等待，设备离线时永远不进 app；设成 0，则 PC 来不及发 `u`。

### 7.3 新增步骤：`app_is_valid()`

```c
uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);
```

这两次读取就是读 app 向量表的头两个字。随后检查：

1. 栈顶落在 SRAM 范围
2. 复位入口落在 app 区
3. 复位入口最低位为 1（Thumb）

三个条件缺一不可。尤其是第 3 条，空片读出的 `0xFFFFFFFF` 恰好是奇数，会被误判为合法，所以第 1、2 条必须同时存在。

### 7.4 新增步骤：`jump_to_app()` 的顺序

```c
__disable_irq();
SysTick->CTRL = 0;
SysTick->LOAD = 0;
SysTick->VAL  = 0;
for (i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFF;
    NVIC->ICPR[i] = 0xFFFFFFFF;
}
SCB->VTOR = APP_START_ADDR;
__set_MSP(app_sp);
__enable_irq();
((void (*)(void))app_reset)();
```

每一步的硬件后果：

- `__disable_irq()`：防止跳转过程中 SysTick 或串口中断插入，打断 MSP/VTOR 的切换
- 停 SysTick：它的向量还在 Bootloader 表里，必须停掉；app 会重新配置
- 清 NVIC：把 Bootloader 打开过的中断使能位和挂起位清零，避免 app 一开中断就进旧 ISR
- 设 VTOR：让 app 的中断入口生效
- 设 MSP：切换到 app 的栈
- 开中断：app 运行后可以正常响应中断

### 7.5 新增步骤：app 侧搬向量表

`app.c` 的 `main()` 第一行：

```c
SCB->VTOR = APP_START_ADDR;
```

必须在 `systick_init()` 之前。顺序反了，SysTick 中断会按旧 VTOR 进入 Bootloader 的 `SysTick_Handler`，app 的 `g_ms_ticks` 永远不增加，`delay_ms()` 卡死。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

- `HAL_Init()`、`HAL_RCC_OscConfig()`、`HAL_RCC_ClockConfig()`（00/03 课）
- `HAL_GPIO_Init()`、`HAL_GPIO_TogglePin()`（01 课）
- `HAL_UART_Init()`、`HAL_UART_Transmit()`、`HAL_UART_Receive()`（21/22 课）

### 8.2 新增步骤：HAL 版等待窗口

```c
if (HAL_UART_Receive(&huart1, &ch, 1U, 10U) == HAL_OK) { ... }
```

超时参数 10ms 让函数“等不到就返回”，效果等同于寄存器版的 `uart1_try_getc()`。HAL 内部仍然是在轮询 RXNE，只是把超时逻辑封装了。

### 8.3 新增步骤：`HAL_UART_DeInit()`

跳转前调用它，把 USART1 的寄存器恢复默认、关闭中断使能。对应寄存器版“清 NVIC + 不再使用 USART”。

如果不 DeInit，app 重新 `HAL_UART_Init()` 时通常也能工作，但 Bootloader 留下的 RXNE/TC 标志可能让 app 第一次接收读到脏数据。

### 8.4 HAL 没封装的部分

以下操作 HAL 没有 API，必须直接写寄存器/CMSIS：

| 操作 | 寄存器版 | HAL 版 |
|------|---------|--------|
| 关全局中断 | `__disable_irq()` | `__disable_irq()` |
| 停 SysTick | `SysTick->CTRL=0` | `SysTick->CTRL=0` |
| 改向量表 | `SCB->VTOR` | `SCB->VTOR` |
| 改栈顶 | `__set_MSP()` | `__set_MSP()` |
| 跳转 | 函数指针 | 函数指针 |

这就是本课的“HAL 边界”：外设可以交给 HAL，**固件交接必须自己写**。

### 8.5 验证 VTOR 是否生效

app 里用 `HAL_Delay()` 闪灯。如果 LED 常亮不闪、串口只打印一次，首先怀疑 VTOR 没设或设晚。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
读向量表 -> 校验 -> 关中断 -> 停 SysTick -> 清 NVIC -> VTOR -> MSP -> 跳转
```

HAL 版重点看：

```text
同一套跳转流程 + HAL 负责外设初始化/反初始化
```

先读寄存器版的 `jump_to_app()`，理解每个动作对应的硬件状态；再看 HAL 版，确认哪些被库封装、哪些必须手写。两版的 `app_is_valid()` 逻辑完全一致，可以逐行对照。

## 10. 检验问题清单

### 10.1 为什么 Bootloader 必须在 `0x08000000`？

**答**：复位后 CPU 固定从该地址取向量表，只有占据这个地址的固件能第一个运行。

### 10.2 app 链接脚本不改会怎样？

**答**：app 仍然链接到 `0x08000000`，烧录会覆盖 Bootloader；即使不覆盖，函数地址也和实际存放位置不符，跳转后取指错误。

### 10.3 忘记设置 VTOR 会怎样？

**答**：app 的中断仍进入 Bootloader 向量表。SysTick/HAL_Delay 失效，串口中断异常，严重时 HardFault。

### 10.4 忘记设置 MSP 会怎样？

**答**：app 复用 Bootloader 的栈，栈空间重叠，运行一段时间后随机死机。

### 10.5 为什么跳转前要清 NVIC？

**答**：Bootloader 可能打开过 SysTick/串口中断，挂起位和使能位会带到 app，导致 app 还没初始化就进旧 ISR。

### 10.6 空片 Flash 为什么必须拦住？

**答**：读出 `0xFFFFFFFF`，栈顶非法、复位地址非法；直接跳会立刻 HardFault。

### 10.7 Reset_Handler 地址为什么是奇数？

**答**：Cortex-M 只用 Thumb 指令，地址最低位为 1 表示 Thumb 状态；偶数地址会触发异常。

### 10.8 HAL 版跳转和寄存器版有什么对应关系？

**答**：HAL 只替换了时钟/GPIO/UART 初始化，VTOR/MSP/清 NVIC/函数指针跳转完全一致，对应同一组 CMSIS 操作。

### 10.9 用 ST-Link 烧 app 需要手动指定地址吗？

**答**：不需要。app 的 ELF 已经按 `0x08008000` 链接，OpenOCD 按 ELF 段地址写入。只有烧 `.bin` 时才需要手动指定偏移。

### 10.10 如果 app 超过 32KB 会怎样？

**答**：链接阶段 `region FLASH overflowed` 报错；必须调整分区，且同步修改 Bootloader 的 `APP_END_ADDR`。

## 11. 工程实现步骤

### 11.1 需求分析

做一个最小可验证的 Bootloader：上电先运行 Bootloader，3 秒窗口后可跳转到 app，app 能正常使用中断和串口。

### 11.2 硬件核查

确认芯片容量、SWD 接线、串口交叉连接和共地。先分别烧 Bootloader、app，确认单独运行正常，再做跳转。

### 11.3 寄存器路线

先写 `jump_to_app()` 的 8 个步骤，再补 `app_is_valid()`，最后加串口窗口。顺序不要反，否则调试时分不清是跳转问题还是串口问题。

### 11.4 HAL 路线

用 HAL 初始化外设，跳转部分照抄寄存器版。HAL 版的价值是验证“外设初始化方式不影响跳转本质”。

### 11.5 工程思维

Bootloader 是产品的一次性代码，必须尽量小、尽量稳。分区一旦发布就不能随意改，否则现场设备无法升级。app 必须自带版本号，方便确认升级是否成功。

### 11.6 常见工程陷阱

- app 忘改链接脚本，覆盖 Bootloader
- 忘设 VTOR，中断跑回 Bootloader
- 忘设 MSP，随机死机
- 跳转前没关中断，切换过程被中断打断
- app 体积超过分区，链接报错或越界
- 用 `.bin` 烧录时没加偏移

## 12. 运行现象

烧录顺序：先烧 `bootloader`，再烧 `app`，然后按复位。

串口 115200 应看到：

```text
[BOOT] STM32F103 bootloader v1.0
[BOOT] app region: 0x08008000 - 0x0800FFFF
[BOOT] send 'u' within 3s to stay, others jump to app
[BOOT] jump to app
[APP] bootloader jump ok, running at 0x08008000
[APP] PC13 blinks at 2Hz
[APP] alive
[APP] alive
```

- PC13：app 运行时 2Hz 翻转（250ms 亮、250ms 灭）
- 3 秒内发送 `u`：打印 `[BOOT] stay in bootloader mode`，LED 变 1Hz，不再进入 app
- 未烧 app 或 app 非法：打印 `[BOOT] app invalid, stay in bootloader`，LED 1Hz

异常表现：

- 只看到 `[BOOT]` 后无输出：跳转后 app 没跑起来，查链接脚本和 VTOR
- 跳转后串口乱码：app 时钟没初始化或波特率不对
- 跳转后立即死机：MSP/栈顶检查失效或 app 向量表不在 `0x08008000`

## 13. 常见问题排查

### 13.1 一直停留在 Bootloader

先确认 app 是否真的烧到 `0x08008000`。用 ST-Link 读 Flash，或看 app 编译输出的 `.map` 中 `.isr_vector` 地址。

### 13.2 跳转后 LED 不闪、串口无输出

检查 app `main()` 是否第一行设置 `SCB->VTOR`，以及 `ld/app.ld` 的 FLASH ORIGIN 是否为 `0x08008000`。

### 13.3 烧完 app 后 Bootloader 消失

app 仍在用默认链接脚本。检查 `platformio.ini` 是否设置了 `board_build.ldscript = ld/app.ld`。

### 13.4 串口没有任何输出

确认 USB-TTL 的 RX 接 PA9、TX 接 PA10、共地；确认 Bootloader 和 app 都用 115200。

### 13.5 跳转后运行几秒死机

怀疑栈冲突。确认 `__set_MSP(app_sp)` 在跳转前执行，且 `app_sp` 来自 app 向量表而不是硬编码。

## 14. 本课最核心的结论

1. Bootloader 占据 `0x08000000`，app 必须链接到分区起点 `0x08008000`。
2. app 链接脚本的 `MEMORY` 决定代码实际地址，不是靠烧录命令“搬”过去。
3. 跳转前必须完成：关中断、停 SysTick、清 NVIC、设 VTOR、设 MSP。
4. VTOR 决定中断入口，MSP 决定栈空间，两者缺一不可。
5. 空片/损坏的 app 必须用栈顶和复位地址双重校验拦住。
6. HAL 只封装外设，固件交接必须自己写。
7. 先保证两段程序单独可运行，再验证跳转，排错才有参照。

## 15. 建议你现在怎么读这节课

先看 `ld/app.ld` 的 `MEMORY`，再看 `bootloader.c` 的 `app_is_valid()` 和 `jump_to_app()`，最后对照 `app.c` 第一行的 `SCB->VTOR`。读完后用调试器在 `jump_to_app()` 下断点，单步看 VTOR/MSP 的变化。

## 16. 扩展练习

1. 把分区改成 Bootloader 16KB + app 48KB，同步修改三个位置并验证。
2. 在 app 里打印自己的版本号和编译时间，升级后确认版本变化。
3. 增加一个按键：上电按住 PA0 强制停留在 Bootloader。
4. 在跳转前对 app 区做 CRC32 校验，校验失败禁止跳转。
5. 用调试器观察 `SCB->VTOR` 和 MSP 在跳转前后的值。

## 17. 下一课预告

- 上一课：[62_lcd_12864](../62_lcd_12864/README.md)
- 下一课：[64_uart_ota](../64_uart_ota/README.md)：在 Bootloader 里接入串口升级协议，把新固件真正写进 app 区。
