# 第 25 课：USART 数据包协议

## 1. 本课到底在学什么

本课表面现象是：PC 通过串口发送 4 个字节（`AA CMD DATA 55`），STM32 解析出命令后控制 PC13 LED。

真正要学的是：USART 接收到的是连续字节流，不是天然分好包的"消息"。程序必须自己定义协议，再用状态机把一个个字节拼成一包，最后再执行动作。

本课的完整链路是：

```text
PC 串口工具发送字节
  -> USB 转串口模块输出 TX/RX 电平
  -> PA10 收到 USART1_RX 信号
  -> USART1 硬件把串行位流还原成字节
  -> RXNE 置位
  -> C 代码读取 USART1->DR（或 HAL_UART_Receive）
  -> 状态机识别 AA/CMD/DATA/55
  -> handle_packet() 根据 CMD/DATA 控制 PC13
```

这节课不能只记住 `while` 和 `switch`。你要把"线上的字节、电平变化、USART 寄存器、C 状态变量、LED 输出"连成一条因果链。

在体系中的位置：21~24 课讲了 UART 怎么收发字节，本课讲收到字节之后怎么组织成有意义的数据包。这是从"通信"到"协议"的跨越。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么串口接收不能默认"一次就是一包"？
2. `PA9` 和 `PA10` 在 USART1 里分别承担什么角色？
3. `USART1->BRR = 0x0271` 为什么对应 72MHz 下的 115200 波特率？
4. `USART_SR_RXNE` 为什么能告诉软件"有新字节到了"？
5. 读 `USART1->DR` 在硬件上会带来什么后果？
6. `WAIT_HEAD`、`WAIT_CMD`、`WAIT_DATA`、`WAIT_TAIL` 四个状态分别在等什么？
7. 为什么收到错误帧尾后要回到 `WAIT_HEAD`？
8. HAL 版的 `HAL_UART_Receive()` 对应寄存器版哪一个等待和读数据动作？
9. 为什么 PC13 LED 在很多 BluePill 上是低电平亮？
10. 如果上位机发送 `AA 01 01 55`，状态机会依次经过哪些状态？

## 3. 本课目录结构

```text
25_uart_packet_protocol/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 使用 `USART1->SR`、`USART1->DR`、`USART1->BRR` 等寄存器直接收字节。  
`hal/` 使用 `UART_HandleTypeDef` 和 `HAL_UART_Receive()` 做同一件事。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- 串口连接：USB 转串口模块
- 串口参数：115200，8 数据位，无校验，1 停止位
- LED：PC13 板载 LED，多数 BluePill 为低电平点亮

接线：

```text
USB-TTL TX  -> PA10 / USART1_RX
USB-TTL RX  -> PA9  / USART1_TX
USB-TTL GND -> BluePill GND
```

本课代码没有主动发送调试字符串，但仍配置了 `PA9` 作为 USART1_TX，方便你后续扩展回传 ACK 或调试信息。

## 5. 脑图

```text
system_clock_72mhz_init()       [已学]
  -> 让 SYSCLK=72MHz，PCLK2=72MHz

pc13_led_init()                 [已学]
  -> 打开 GPIOC
  -> 把 PC13 配成推挽输出
  -> 默认输出高电平，LED 熄灭

usart1_init()                   [已学]
  -> 打开 GPIOA 和 USART1 时钟
  -> PA9 配成复用推挽输出
  -> PA10 配成输入
  -> BRR 配成 115200
  -> CR1 使能发送、接收和 USART

main() while(1)                 [本课新增]
  -> 等 RXNE / HAL_UART_Receive                     [已学]
  -> 读 DR 得到一个字节                              [已学]
  -> 状态机判断它属于帧头、命令、数据还是帧尾          [★新增]
  -> 帧完整且 CMD=0x01 时控制 LED                    [★新增]
```

你要特别注意：协议解析发生在 C 代码层，但它依赖 USART 硬件已经把电平波形恢复成字节。如果 GPIO 复用、波特率或接线错了，状态机再正确也收不到正确字节。

## 6. 核心名词解释

### 6.1 已学名词速查

以下名词在 21~24 课已完整讲解，本课只用不重复展开：

| 名词 | 一句话提醒 |
|------|-----------|
| `USART1` | 挂在 APB2 的串口外设，本课用它从 PA10 接收字节 |
| `PA9 / PA10` | USART1 的默认复用引脚，PA9=TX（复用推挽输出），PA10=RX（输入） |
| `GPIOA->CRH` | GPIOA 高位配置寄存器，PA8~PA15 的模式配在这里 |
| `USART1->BRR` | 波特率寄存器，`0x0271` = 72MHz/115200，决定每位采样节奏 |
| `USART1->CR1` | 控制寄存器 1，`TE`=发、`RE`=收、`UE`=总开关 |
| `USART_SR_RXNE` | 接收数据寄存器非空标志，硬件收到字节后置位，读 DR 后自动清除 |
| `USART1->DR` | 数据寄存器，读它得到收到的字节，写它发送字节 |
| `system_clock_72mhz_init()` | 配置 HSE+PLL 产生 72MHz 系统时钟，PCLK2=72MHz 给 USART1 提供时钟基准 |
| `pc13_led_init()` | 打开 GPIOC 时钟，PC13 配推挽输出，默认高电平灭 LED |
| `usart1_init()` | 打开 GPIOA+USART1 时钟，配 PA9/PA10 复用，设 BRR 和 CR1 |
| `HAL_UART_Receive()` | HAL 阻塞接收 API，封装了"等待 RXNE 再读 DR" |
| `UART_HandleTypeDef` | HAL 的 UART 外设句柄结构体，`Instance` 绑定外设，`Init` 字段描述串口参数 |
| `HAL_GPIO_WritePin()` | HAL 写引脚 API，对应寄存器版的 `BSRR`/`BRR` 操作 |

### 6.2 帧头 0xAA — 本课新名词

帧头是协议层概念，不是 STM32 硬件寄存器。

`0xAA` 的二进制是 `10101010`，常被用作容易观察的同步字节。本课把它定义为一包数据的开始标志。

它控制的是状态机从"乱流中等待开始"进入"开始收命令"的时刻。只有收到 `0xAA` 才认为一包开始，收到其他字节就继续等。

本课为什么需要它：没有帧头，接收端无法知道一包数据从哪里开始。上电时如果 PC 已经发了一半数据，STM32 收到的第一个字节可能是包中间的某个数据，没有帧头标记就无法重新同步。

如果帧头值选得不好（比如用了数据中常见的 `0x00`），状态机可能把数据字节误判为帧头，导致帧错位。`0xAA` 和 `0x55` 互为 bit 反转，在二进制层面容易区分，不易和数据混淆。

### 6.3 帧尾 0x55 — 本课新名词

帧尾也是协议层概念，`0x55` 的二进制是 `01010101`，和 `0xAA` 形成明显区分。

它控制的是命令是否执行：只有帧尾正确时才调用 `handle_packet()`。帧尾错误时直接丢弃当前包并回到等待帧头。

本课为什么需要它：帧尾充当"校验+确认"双重角色。如果没有帧尾，状态机在收到 DATA 后就执行命令——但此时可能后面还有字节，或者 DATA 本身就是错误数据。帧尾让接收端确认"这一包确实完整结束了"。

配错会怎样：如果帧尾判断逻辑写成 `if (b == 0x55) handle_packet(); state = WAIT_HEAD;`，帧尾错误时虽然不执行，但也不回 WAIT_HEAD——下一包第一个字节会被当成帧尾判断，永久错位。

### 6.4 状态机 — 本课新名词

状态机是 C 代码层的协议解析方法。它用一个变量（`state`）记录"当前正在等什么"，再根据新收到的字节决定下一步（状态转移）。

本课四个状态：

- `WAIT_HEAD`：等待 `0xAA`，收到就进入 `WAIT_CMD`，否则原地等待
- `WAIT_CMD`：保存命令字节，无条件进入 `WAIT_DATA`
- `WAIT_DATA`：保存参数字节，无条件进入 `WAIT_TAIL`
- `WAIT_TAIL`：等待 `0x55`，正确则执行命令，无论正确与否都回到 `WAIT_HEAD`

本课为什么需要它：串口硬件只负责给你字节，不负责告诉你哪个字节是命令、哪个字节是参数。状态机是软件层面的"字节→包"的转换器。

配错会怎样：如果收到帧尾后不回 `WAIT_HEAD`，下一包第一个字节被当成帧尾判断，状态机永久错位。如果 `WAIT_CMD` 不保存 `cmd` 变量，后面 `handle_packet` 拿到的 `cmd` 是上次的值或未初始化的值。

### 6.5 handle_packet() — 本课新名词

`handle_packet()` 是协议层到动作层的分界函数。它接收已经解析出的 `cmd` 和 `data`，执行对应的业务动作。

本课代码：

```c
if (cmd == 0x01U) {
    if (data != 0U) GPIOC->BRR = GPIO_BRR_BR13;  // LED 亮
    else GPIOC->BSRR = GPIO_BSRR_BS13;             // LED 灭
}
```

`cmd=0x01` 表示控制 LED。`data!=0` 时把 PC13 拉低，LED 亮；`data=0` 时把 PC13 拉高，LED 灭。

本课为什么需要它：把协议解析和业务动作分开，状态机只负责"收到什么"，本函数只负责"做什么"。后续增加 `cmd=0x02`（蜂鸣器）、`cmd=0x03`（PWM）时，只扩展这个函数，不碰状态机。

配错会怎样：如果把 GPIO 操作直接写在状态机里，每增加一个命令就要改状态机，协议解析和业务逻辑纠缠在一起，代码越来越难维护。

## 7. 寄存器版代码讲解

### 7.1 已学步骤（快速过）

以下步骤在 21~24 课已逐条拆解，本课用列表方式快速过：

- `system_clock_72mhz_init()`：FLASH 等 2 周期 → 开 HSE → 等 HSE 稳定 → 配 PLL 9 倍频 → 等 PLL 稳定 → 切 SYSCLK 到 PLL。结果为 72MHz，PCLK2=72MHz。
- `pc13_led_init()`：开 GPIOC 时钟 → 清 CRH 的 MODE13/CNF13 → 设 MODE13=10（2MHz 推挽输出）→ BSRR 写 BS13 拉高，LED 熄灭。
- `usart1_init()` 中已学部分：开 GPIOA+USART1 时钟 → PA9 配复用推挽输出 → PA10 配浮空输入 → BRR=0x0271 → CR1 设 TE/RE/UE。

### 7.2 usart1_read_byte()：轮询等待一个字节 [已学，本课回顾]

```c
while ((USART1->SR & USART_SR_RXNE) == 0U) {
}
return (uint8_t)USART1->DR;
```

当没有新字节时，CPU 停在 `while` 里。收到完整字节后，硬件置位 `RXNE`，循环退出，代码读取 `DR`。

这个写法简单直观，但缺点也明显：等待期间 CPU 不能做别的事。后续中断、DMA 或 RTOS 课程会解决这个工程问题；本课先把字节流和协议解析讲清楚。

### 7.3 main() 中的状态变量 [本课新增]

```c
enum { WAIT_HEAD, WAIT_CMD, WAIT_DATA, WAIT_TAIL } state = WAIT_HEAD;
uint8_t cmd = 0, data = 0;
```

`state` 记录解析进度，`cmd/data` 保存当前包内容。

这三个变量属于 C 软件层，但它们处理的数据来自 `USART1->DR`。你可以在调试器 Watch 里观察它们，发送 `AA 01 01 55` 时会看到状态依次变化：`WAIT_HEAD → WAIT_CMD → WAIT_DATA → WAIT_TAIL → WAIT_HEAD`。

### 7.4 WAIT_HEAD：寻找帧头 [本课新增]

```c
case WAIT_HEAD:
    state = (b == 0xAAU) ? WAIT_CMD : WAIT_HEAD;
    break;
```

只有收到 `0xAA` 才认为一包开始。收到其他字节就继续等。

硬件后果：这个状态让协议具备重新同步能力。即使上电时 PC 已经发了一半数据，STM32 也会丢弃无关字节，直到下一个 `0xAA`。如果没有这个机制，从半包中间开始接收会导致之后所有帧都错位。

### 7.5 WAIT_CMD 和 WAIT_DATA：保存包内容 [本课新增]

```c
case WAIT_CMD:
    cmd = b;
    state = WAIT_DATA;
    break;

case WAIT_DATA:
    data = b;
    state = WAIT_TAIL;
    break;
```

这里没有立即执行动作，因为一包还没确认结束。先保存命令和参数，等帧尾正确后再处理。

如果你以后增加长度字段、校验字段，也是在这个阶段扩展状态机。

### 7.6 WAIT_TAIL：确认帧尾并执行 [本课新增]

```c
case WAIT_TAIL:
    if (b == 0x55U) handle_packet(cmd, data);
    state = WAIT_HEAD;
    break;
```

只有帧尾正确才执行命令。无论正确与否，最后都回到 `WAIT_HEAD`，准备下一包。

硬件后果：如果这里不回到 `WAIT_HEAD`，状态机会卡在 `WAIT_TAIL`，下一包的第一个字节会被当成帧尾判断，整个协议永久错位。这是状态机最常见的 bug——忘了在尾部回到初始状态。

### 7.7 handle_packet()：从协议到 GPIO [本课新增]

```c
if (cmd == 0x01U) {
    if (data != 0U) GPIOC->BRR = GPIO_BRR_BR13;
    else GPIOC->BSRR = GPIO_BSRR_BS13;
}
```

`CMD=0x01` 被定义为 LED 控制命令。

- `DATA!=0`：写 `BRR`，PC13 低电平，LED 亮
- `DATA=0`：写 `BSRR`，PC13 高电平，LED 灭

这一步把协议层命令落到寄存器层动作。若 PC13 硬件接法不同（有的板子高电平亮），亮灭逻辑可能相反。

## 8. HAL 版代码讲解

### 8.1 已学步骤（快速过）

- `HAL_Init()`：初始化 HAL 基础环境，包括 HAL Tick。
- `RCC_OscInitTypeDef` + `RCC_ClkInitTypeDef`：HAL 版时钟配置，对应寄存器版的 `RCC->CR` 和 `RCC->CFGR` 操作。
- `GPIO_InitTypeDef` 配置 PC13：`GPIO_MODE_OUTPUT_PP`、`GPIO_SPEED_FREQ_LOW`，对应寄存器版 `GPIOC->CRH` 的 MODE13/CNF13。
- `usart1_init()` 中的 PA9/PA10 配置：`GPIO_MODE_AF_PP`（复用推挽）对应寄存器版 PA9 的 CNF9=10、MODE9=10；`GPIO_MODE_INPUT` 对应寄存器版 PA10 的 MODE10=00、CNF10=01。
- `UART_HandleTypeDef` 字段填充：`BaudRate=115200`、`WordLength=8B`、`StopBits=1`、`Parity=NONE`、`Mode=TX_RX`，这些字段共同决定 HAL 写 `USART1->BRR` 和 `USART1->CR1` 的值。

### 8.2 HAL_UART_Receive() 与寄存器版接收 [本课新增：HAL↔寄存器映射]

```c
HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY)
```

参数含义：

- `&huart1`：使用 USART1（通过 `huart1.Instance = USART1` 绑定）
- `&b`：接收结果放到变量 `b`
- `1`：只收 1 个字节
- `HAL_MAX_DELAY`：一直阻塞等待

它对应寄存器版的操作链：

```text
HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY)
  ├── 内部调用 huart1.Instance->SR 检查 RXNE（对应 while((USART1->SR & USART_SR_RXNE)==0)）
  ├── 内部调用 huart1.Instance->DR 读取字节（对应 b = (uint8_t)USART1->DR）
  └── 返回 HAL_OK 表示成功收到 1 字节
```

HAL 不替你解析协议，也不会自动识别帧头帧尾。状态机 `switch (state)` 仍然由我们自己维护——HAL 只负责把"收字节"这一步从寄存器操作封装成函数调用。

### 8.3 主循环中的协议解析 [本课新增]

```c
if (HAL_UART_Receive(&huart1, &byte, 1U, HAL_MAX_DELAY) == HAL_OK) {
    packet_fsm_input(&state, byte, &cmd, &data);
}
```

每次收到 1 字节就喂给状态机。状态机代码和寄存器版完全一样——这验证了"协议逻辑不依赖底层 API"的设计原则。你把 `HAL_UART_Receive` 换成 `usart1_read_byte`，协议部分一行都不用改。

## 9. 两个版本怎么学

先寄存器版，后 HAL 版。原因：

1. **寄存器版让你看清底层因果**：`while (USART1->SR & USART_SR_RXNE)` 让你知道 CPU 在等什么、等不到会怎样。HAL 版把这个循环封在 `HAL_UART_Receive()` 内部，你看不到等待过程。
2. **HAL 版让你理解抽象的价值**：当你理解了寄存器版后，再看 HAL 版，你会发现 `HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY)` 一行代码等价于寄存器版的好几行——但前提是你知道它内部在干什么。
3. **协议逻辑在两个版本中完全一样**：`packet_fsm_input()` 和 `handle_packet()` 在 reg/ 和 hal/ 中代码相同，因为协议解析是纯软件逻辑，不依赖底层 API。这验证了"把协议和硬件解耦"的设计原则。

HAL↔寄存器快速映射表：

| HAL API / 字段 | 对应寄存器操作 |
|---------------|--------------|
| `HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY)` | `while(!(USART1->SR & USART_SR_RXNE)); b = USART1->DR;` |
| `huart1.Init.BaudRate = 115200` | `USART1->BRR = 0x0271` |
| `huart1.Init.Mode = UART_MODE_TX_RX` | `USART1->CR1 \|= USART_CR1_TE \| USART_CR1_RE` |
| `HAL_UART_Init(&huart1)` | 写 `BRR`、`CR1` 等寄存器，使能 USART |
| `HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET)` | `GPIOC->BRR = GPIO_BRR_BR13` |
| `HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET)` | `GPIOC->BSRR = GPIO_BSRR_BS13` |

## 10. 检验问题

1. **为什么串口接收不能默认"一次就是一包"？**  
   答：UART 硬件只负责把串行波形还原成字节，不负责判断字节之间的逻辑关系。上位机连续发送 `AA 01 01 55` 时，STM32 的 USART 收到的就是 4 个独立字节，硬件不知道哪几个字节属于同一包。协议帧的帧头帧尾是软件层面给字节流加的"边界"。

2. **忘了在 WAIT_TAIL 后回到 WAIT_HEAD 会怎样？**  
   答：状态机永远停在 `WAIT_TAIL`，下一包第一个字节被当成帧尾判断。如果这个字节恰好是 `0x55`，会错误执行上一包的 `cmd/data`；如果不是，整帧丢弃。无论哪种情况，后续所有帧都无法正确解析，LED 永久不受控。

3. **忘了在 WAIT_HEAD 中判断非 0xAA 时留在 WAIT_HEAD 会怎样？**  
   答：如果收到非 0xAA 的字节后进入了 `WAIT_CMD`，状态机会把一个无关字节当成命令码，后面的字节也依次错位。只有恰好下一个字节是 `0xAA` 且后面的字节也恰好符合协议，才可能偶然恢复——但概率极低，实际上 LED 会随机亮灭，无法稳定控制。

4. **HAL_UART_Receive(&huart1, &b, 1, HAL_MAX_DELAY) 对应寄存器版哪几步操作？**  
   答：对应寄存器版的两步：① 等待 `USART1->SR & USART_SR_RXNE`（轮询 RXNE 标志）；② 读取 `USART1->DR` 获得字节。HAL 把这两步封装在函数内部，返回值 `HAL_OK` 表示成功收到 1 字节。

5. **如果 USB-TTL 的 TX 接 PA9 而不是 PA10 会怎样？**  
   答：PA9 是 USART1_TX，输出脚。USB-TTL 的 TX 也是输出，两个输出接在一起，电平冲突。USART1 收不到任何数据，`RXNE` 永远不置位，程序卡在 `while` 等待中，LED 不响应。

6. **如果 PC 串口工具设 9600 波特率，STM32 设 115200，会怎样？**  
   答：USART1 按 115200 的节奏采样 PA10 上的电平，但 PC 按 9600 的节奏发送。采样时刻和数据位不匹配，收到的字节全是乱码。状态机永远等不到 `0xAA`，LED 不响应。

7. **为什么 handle_packet() 要独立成函数，而不是把 GPIO 操作直接写在状态机里？**  
   答：分离协议解析和业务动作。后续增加新命令（如蜂鸣器、PWM）时，只需扩展 `handle_packet()`，不用改状态机。如果 GPIO 操作散落在状态机中，每增加一个命令就要改状态机代码，协议逻辑和业务逻辑纠缠在一起，难以维护。

8. **发送 `AA 01 01 55` 时，Debug 模式下 state 会依次经过哪些值？**  
   答：初始 `WAIT_HEAD(0)` → 收到 `AA` 后 `WAIT_CMD(1)` → 收到 `01` 后 `WAIT_DATA(2)` → 收到 `01` 后 `WAIT_TAIL(3)` → 收到 `55` 后执行 `handle_packet`，然后回到 `WAIT_HEAD(0)`。

9. **如果发送 `AA 01 01 AA`（帧尾错误），LED 会亮吗？**  
   答：不会。状态机在 `WAIT_TAIL` 收到 `AA`（不是 `0x55`），不调用 `handle_packet()`，然后回到 `WAIT_HEAD`。整帧被丢弃，LED 保持不变。

10. **为什么用 `0xAA` 和 `0x55` 而不是 `0x00` 和 `0xFF`？**  
    答：`0xAA`（10101010）和 `0x55`（01010101）互为 bit 反转，在串口波形上特征明显，容易用逻辑分析仪观察。`0x00` 在波形上是连续低电平，`0xFF` 是连续高电平，和空闲状态不易区分，且容易误判。

## 11. 工程实现步骤

### 11.1 需求分析

- 功能需求：PC 通过串口发送命令控制 STM32 LED
- 协议需求：定义固定 4 字节帧格式 `AA CMD DATA 55`
- 命令需求：`CMD=0x01` 控制 LED，`DATA!=0` 亮，`DATA=0` 灭
- 性能需求：115200 波特率，轮询接收（本课先聚焦协议逻辑，性能不是重点）

### 11.2 硬件核查

- 开发板：确认 PC13 板载 LED 是低电平亮还是高电平亮（BluePill 多数低电平亮）
- 串口接线：确认 USB-TTL TX 接 PA10，RX 接 PA9，GND 共地
- 电源：BluePill 通过 ST-Link 或 USB 供电

### 11.3 寄存器路线

1. 初始化系统时钟 72MHz（PCLK2=72MHz 给 USART1 提供时钟基准）
2. 初始化 PC13 LED（推挽输出，默认灭）
3. 初始化 USART1（开时钟、配 PA9 PA10、设 BRR、使能 TE/RE/UE）
4. 主循环：轮询 RXNE → 读 DR → 喂状态机 → 完整帧触发 handle_packet

### 11.4 HAL 路线

1. HAL_Init() 初始化 HAL 库
2. 配置系统时钟（RCC_OscInitTypeDef + RCC_ClkInitTypeDef）
3. 初始化 PC13 LED（GPIO_InitTypeDef，MODE_OUTPUT_PP）
4. 初始化 USART1（配 PA9 AF_PP、PA10 INPUT，填充 UART_HandleTypeDef，HAL_UART_Init）
5. 主循环：HAL_UART_Receive 收 1 字节 → 喂状态机 → 完整帧触发 handle_packet

### 11.5 工程思维

- **协议与硬件解耦**：`packet_fsm_input()` 不依赖任何 USART API，只接收 `uint8_t`。这意味着你可以把串口换成 SPI、I2C 甚至文件读取，协议逻辑一行不改。
- **命令与动作分离**：`handle_packet()` 是唯一的"命令→动作"转换点。增加新外设命令时，只改这个函数。
- **状态机是自愈的**：任何错误（帧尾错误、半包开始、噪声字节）都会让状态机回到 `WAIT_HEAD`，不会永久错位。

### 11.6 常见陷阱

- **陷阱 1**：在 `WAIT_TAIL` 后忘了回 `WAIT_HEAD`。现象：第一包正常，第二包开始 LED 不响应。
- **陷阱 2**：`cmd` 和 `data` 变量没有初始化。现象：上电后第一次收到帧头前，如果意外进入 `handle_packet`，可能执行随机命令。
- **陷阱 3**：在 `WAIT_HEAD` 中收到非 `0xAA` 后进入了其他状态。现象：LED 随机亮灭，完全不受控。
- **陷阱 4**：PC 串口工具发送的是 ASCII 字符而非十六进制。现象：发送"AA"（ASCII 字符 A 和 A，即 0x41 0x41），状态机永远等不到 0xAA。

## 12. 运行现象

### 正常现象

1. 上电后 PC13 LED 熄灭（高电平）。
2. 打开串口工具（115200、8N1），发送十六进制 `AA 01 01 55`：PC13 LED 点亮。
3. 发送十六进制 `AA 01 00 55`：PC13 LED 熄灭。
4. 发送十六进制 `AA 01 01 55` 和 `AA 01 00 55` 交替：LED 以发送节奏闪烁，每次发送一帧立即响应。
5. 发送十六进制 `AA 02 01 55`（CMD=0x02，未定义）：LED 保持不变，因为 `handle_packet()` 只处理 `CMD=0x01`。

### 异常现象

- 发送 ASCII 字符 "AA 01 01 55"（而非十六进制）：LED 不响应，因为 `0x41`（ASCII 'A'）≠ `0xAA`。
- 发送 `AA 01 01 AA`（帧尾错误）：LED 不动作，帧被丢弃。
- 不接 USB-TTL 或只接 TX 不接 RX：程序卡在 `while(RXNE)` 等待，LED 永远不亮。
- 波特率设错（PC 9600，STM32 115200）：LED 不响应，上位机可能收到乱码或无数据。

## 13. 常见问题排查

### 13.1 LED 完全不亮，发送任何数据都没反应

排查顺序：
1. 检查 USB-TTL 的 TX 是否接 PA10（不是 PA9）
2. 检查 GND 是否共地
3. 检查串口工具波特率是否 115200
4. 检查串口工具是否以十六进制发送（不是 ASCII）
5. 用万用表测 PA10 电压，发送数据时应有电平变化

### 13.2 LED 乱闪，不按发送命令变化

排查顺序：
1. 检查是否发送了非协议帧的随机字节（如按键、重连时产生的噪声）
2. 检查波特率是否匹配（PC 和 STM32 必须一致）
3. 检查串口线是否接触不良（松动会产生随机字节）
4. 在 Debug 模式下观察 `state` 变量，看状态机是否卡在某个状态

### 13.3 第一包正常，后面全部不响应

排查顺序：
1. 检查 `WAIT_TAIL` 分支最后是否执行了 `*state = WAIT_HEAD`
2. 在 Debug 模式下观察 `state`，发送第一包后看 `state` 是否回到了 `WAIT_HEAD`
3. 如果 `state` 卡在 `WAIT_TAIL`，说明帧尾处理逻辑有问题

### 13.4 发送数据后 LED 偶尔亮偶尔不亮

排查顺序：
1. 检查串口工具是否以十六进制发送（不是 ASCII）
2. 检查是否有其他程序同时占用串口
3. 检查 USB-TTL 模块供电是否稳定（有些廉价模块供电不足会导致数据错误）
4. 在 Debug 模式下打断点在 `handle_packet()`，观察 `cmd` 和 `data` 的值是否和发送的一致

## 14. 本课结论

1. **UART 只负责字节，不负责协议**。把字节组织成有意义的帧是软件层的责任。
2. **帧头帧尾是给字节流加的"边界"**。没有边界，接收端无法知道一包从哪里开始、到哪里结束。
3. **状态机是把字节流变成结构化数据的标准方法**。它用一个变量记录"当前在等什么"，根据新字节决定下一步。
4. **状态机必须是自愈的**。任何错误（帧尾错、噪声）都应该让状态机回到初始状态，不能永久错位。
5. **协议解析和业务动作应该分离**。`packet_fsm_input()` 负责"收到什么"，`handle_packet()` 负责"做什么"。分离后扩展新命令只需改后者。
6. **HAL 封装了寄存器操作，但不封装协议逻辑**。`HAL_UART_Receive()` 替你做了"等 RXNE 读 DR"，但帧头帧尾的判断仍然由你的代码完成。
7. **协议逻辑不依赖底层 API**。`packet_fsm_input()` 的参数是 `uint8_t`，不管这个字节来自串口、SPI 还是 I2C，协议解析逻辑完全一样。

## 15. 阅读建议

1. 先看第 1 节"本课到底在学什么"建立全局认知
2. 看第 6 节已学名词速查表，快速回忆 21~24 课内容
3. 看第 6 节新名词（6.2~6.5），理解帧头、帧尾、状态机、handle_packet 的概念
4. 看第 7 节寄存器版代码，重点看 7.3~7.7 的新增步骤
5. 看第 8 节 HAL 版代码，重点看 8.2 的 HAL↔寄存器映射
6. 看第 9 节"两个版本怎么学"，建立映射关系
7. 用第 10 节检验问题自测
8. 用第 12 节运行现象验证代码是否正常工作
9. 遇到问题用第 13 节排查

## 16. 扩展练习

1. **增加 CMD=0x02 命令**：控制第二个 LED（如果有），或让 PC13 LED 闪烁指定次数。`DATA` 表示闪烁次数。只改 `handle_packet()`，不改状态机。
2. **增加帧长度字段**：把协议改为 `AA LEN CMD DATA 55`，`LEN` 表示数据长度。状态机新增 `WAIT_LEN` 状态。这是工业协议（如 Modbus）的常见做法。
3. **增加校验和**：把协议改为 `AA CMD DATA CHECKSUM 55`，`CHECKSUM = CMD ^ DATA`（异或）。状态机在 `WAIT_TAIL` 之前先校验，校验失败则丢弃。这能抵抗单 bit 错误。
4. **实现回传 ACK**：收到正确帧后，通过 USART1_TX 回传 `AA 01 01 55` 表示确认。上位机发送命令后等待 ACK，超时则重发。这能实现可靠的命令-响应交互。
5. **用逻辑分析仪抓取波形**：发送 `AA 01 01 55`，在逻辑分析仪上观察 TX 线上的波形，验证 `0xAA`（10101010）和 `0x55`（01010101）的二进制模式。

## 17. 下一课预告

下一课 `26_i2c_basic` 将从串口通信转向 I2C 总线通信。I2C 是板上芯片间通信的主流协议，用两根线（SCL + SDA）连接多个设备。你将学到 I2C 的起始/停止条件、地址帧、数据帧，以及如何使用 STM32 的 I2C 外设读写 EEPROM 和传感器。