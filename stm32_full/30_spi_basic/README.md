# 第 30 课：SPI 基础

## 1. 本课到底在学什么

本课表面现象是：用一根杜邦线把 `PA7(MOSI)` 接到 `PA6(MISO)`，STM32 通过 SPI1 发送 1 个字节，如果收到的字节和发出的字节一致，PC13 LED 每秒翻转一次；如果不一致，LED 点亮表示错误。

真正要学的是 SPI 的同步全双工本质：

```text
主机写 1 个字节到 SPI1->DR
  -> SPI1 在 PA5 输出 8 个 SCK 时钟
  -> 每个时钟从 PA7(MOSI) 移出 1 bit
  -> 同时从 PA6(MISO) 移入 1 bit
  -> 8 bit 后 RXNE 置位
  -> 软件读 SPI1->DR 得到收到的字节
```

本课使用回环，不接外部 SPI 设备，是为了先排除器件协议干扰，只验证 SPI1 主模式、引脚复用、时钟相位极性和收发标志是否真正理解。

## 2. 本课学习目标

学完本课，你应该能回答：

1. SPI 为什么说发送和接收同时发生？
2. `PA5`、`PA6`、`PA7` 分别对应 SPI1 的哪根线？
3. 为什么 SCK/MOSI 要配成复用推挽，而 MISO 是输入？
4. `CPOL=0`、`CPHA=0` 为什么叫 Mode 0？
5. `BR=010` 为什么得到 9MHz SCK？
6. `TXE`、`RXNE`、`BSY` 三个标志各表示什么？
7. 为什么 SPI 读数据前必须先发送一个字节？
8. HAL 版 `HAL_SPI_TransmitReceive()` 对应寄存器版哪几步？

## 3. 本课目录结构

```text
30_spi_basic/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接配置 SPI1 的 GPIO、`CR1`、`SR`、`DR`。
`hal/` 使用 `SPI_HandleTypeDef` 和 `HAL_SPI_TransmitReceive()` 完成同样的回环测试。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- 回环线：`PA7(MOSI)` 接 `PA6(MISO)`
- SPI 时钟脚：`PA5(SCK)` 可用逻辑分析仪观察
- LED：PC13，常见 BluePill 为低电平点亮

本课不需要外部 SPI 从机，也不使用 `PA4(NSS)` 物理片选，SPI1 使用软件 NSS。

## 5. 先建立一个最基本的脑图

```text
72MHz 系统时钟
  -> PCLK2 = 72MHz
  -> SPI1 时钟源来自 PCLK2

GPIOA 复用配置
  -> PA5 = SCK，复用推挽输出
  -> PA7 = MOSI，复用推挽输出
  -> PA6 = MISO，浮空输入

SPI1 配置
  -> 主模式 MSTR=1
  -> Mode 0：CPOL=0，CPHA=0
  -> BR=/8，SCK=9MHz
  -> 软件 NSS：SSM=1，SSI=1
  -> SPE=1 启动 SPI

回环测试
  -> 写 DR 发送 0xA5 或 0x3C
  -> PA7 输出位流
  -> 杜邦线送回 PA6
  -> RXNE 后读 DR
  -> 相等则 LED 翻转
```

SPI 的核心不是“写出去一个字节后再读”，而是“写出去的同时就读回来一个字节”。这一点后面读 Flash、OLED、无线模块时都要用。

## 6. 核心名词解释

### 6.1 已学名词速查

以下概念在前面课程中已详细讲过，本课不再重复展开。

| 名词         | 一句话提醒                                                                       |
| ------------ | -------------------------------------------------------------------------------- |
| 复用推挽输出 | GPIO 引脚由外设控制（不是软件控制），CNF=10，用于 SCK/MOSI 这类外设输出信号      |
| 浮空输入     | GPIO 引脚读取外部电平，不接上下拉电阻，CNF=01，用于 MISO 这类外设输入信号        |
| APB2 总线    | SPI1 挂在 APB2（PCLK2=72MHz），所以要开`RCC_APB2ENR_SPI1EN`，BR 分频基于 72MHz |
| SysTick 1ms  | 72MHz / 72000 = 1kHz，用于`delay_ms()` 控制测试周期                            |
| PC13 LED     | 推挽输出，低电平点亮，本课用翻转/常亮区分回环成功/失败                           |

---

### 6.2 SPI 是什么

SPI（Serial Peripheral Interface）是同步串行外设接口。它和 UART、I2C 一样是板级通信协议，但工作机制完全不同。

**SPI 和 UART 的核心区别**：

- UART 是异步的：双方各自用内部时钟生成波特率，不需要共享时钟线
- SPI 是同步的：主机额外输出一根 SCK 时钟线，从机跟着 SCK 节奏收发数据

**SPI 和 I2C 的核心区别**：

- I2C 是半双工：同一时刻只能收或发，靠地址区分不同从机
- SPI 是全双工：发送和接收同时发生，靠片选线（CS）区分不同从机
- I2C 只有两根线（SCL+SDA），SPI 至少四根（SCK+MOSI+MISO+CS）
- SPI 没有 ACK 机制，不确认从机是否收到数据

**SPI 的物理层工作方式**：主机和从机内部各有一个移位寄存器。主机每打一拍 SCK，把自己的移位寄存器最高位移出到 MOSI，同时从 MISO 采入一位放到自己移位寄存器的最低位。8 拍后，主机的移位寄存器变成了从机移位寄存器的值，从机移位寄存器变成了主机的值——两边交换了数据。

这就决定了 SPI 最核心的特性：**只要主机产生 SCK，发送和接收就同时发生**。即使你只关心发送，也会收到一个字节；即使你只想读取从机，也必须发送一个字节来提供时钟。

### 6.3 SPI1 是什么

`SPI1` 是 STM32F103 内部的 SPI 外设，挂在 APB2 总线（PCLK2=72MHz）。F103 有两个 SPI：SPI1 在 APB2，SPI2 在 APB1。SPI1 的默认引脚是 PA5(SCK)、PA6(MISO)、PA7(MOSI)、PA4(NSS)。

代码里的 `SPI1->CR1`、`SPI1->SR`、`SPI1->DR` 都是访问 SPI1 的寄存器。如果 `RCC_APB2ENR_SPI1EN` 没打开，即使配置了这些寄存器，SPI1 外设也不会工作。

### 6.4 SCK 是什么

`SCK`（Serial Clock）是 SPI 时钟线，使用 PA5。它完全由主机输出，决定每一 bit 什么时候被移出和什么时候被采样。

**为什么 SCK 必须配成复用推挽输出**：SCK 信号由 SPI1 外设内部产生，需要通过 PA5 引脚输出到外部。如果 PA5 配成通用推挽输出（GPIO 控制），SPI1 的信号无法到达引脚。复用推挽输出（CNF=10）让引脚的控制权交给 SPI1 外设，而不是 GPIO 寄存器。

**SCK 频率计算**：SCK = PCLK2 / BR 分频。本课 PCLK2=72MHz，BR=8（/8），SCK=9MHz。如果 BR 设错（比如 /256），SCK 只有 281kHz，通信会非常慢；如果设 /2 得到 36MHz，可能超出 F103 的 SPI 最高频率限制。

### 6.5 MOSI 和 MISO 是什么

`MOSI`（Master Out Slave In）是主机输出、从机输入，使用 PA7。配成复用推挽输出，由 SPI1 外设驱动。

`MISO`（Master In Slave Out）是主机输入、从机输出，使用 PA6。配成浮空输入，SPI1 接收器从 PA6 引脚电平采样。

**为什么 MOSI 和 MISO 的 GPIO 配置不同**：MOSI 是输出信号，电平由 SPI1 外设驱动，所以用复用推挽输出。MISO 是输入信号，电平由外部从机（或回环线）驱动，所以用浮空输入。如果 MISO 误配成推挽输出，STM32 和外部从机同时驱动 PA6，可能烧 IO。

### 6.6 回环（Loopback）是什么

回环就是把输出信号直接接回输入端。本课用一根杜邦线把 PA7(MOSI) 接到 PA6(MISO)，让 STM32 自己发给自己收。

**为什么本课用回环而不是接外部 SPI 设备**：回环排除了从机协议、片选时序、命令格式等所有外部因素，只测试 SPI 主模式、引脚复用、SCK 产生和收发寄存器四件事。回环通过了，说明 SPI1 基础配置正确；如果回环不通过，接外部设备也一定失败。

**回环的局限**：回环只能验证 0 和 1 的电平正确，不能验证时序容差、时钟抖动、从机响应速度等。回环通过后接真实从机，还可能出现 CPOL/CPHA 不匹配、SCK 太快、片选时序错等问题。

### 6.7 CPOL 和 CPHA 是什么（SPI Mode）

`CPOL`（Clock Polarity）是时钟极性，决定 SCK 空闲时的电平。`CPHA`（Clock Phase）是时钟相位，决定在第几个边沿采样。

**四种 Mode**：

| Mode   | CPOL | CPHA | 空闲电平 | 采样边沿             |
| ------ | ---- | ---- | -------- | -------------------- |
| Mode 0 | 0    | 0    | 低       | 上升沿（第一个边沿） |
| Mode 1 | 0    | 1    | 低       | 下降沿（第二个边沿） |
| Mode 2 | 1    | 0    | 高       | 下降沿（第一个边沿） |
| Mode 3 | 1    | 1    | 高       | 上升沿（第二个边沿） |

本课使用 Mode 0（CPOL=0, CPHA=0），这是最常见的模式，大多数 SPI Flash、传感器、显示屏都支持。

**CPOL/CPHA 配错的后果**：如果开发板配 Mode 0 但 FLASH 芯片要求 Mode 3，数据会在错误的边沿被采样，收到的值可能偏移一位或完全错误。回环实验里 CPOL/CPHA 配错不影响（因为发和收用同一个时钟），但接真实从机时就是最常见的通信失败原因。

### 6.8 BR 是什么（SCK 频率计算）

`BR[2:0]` 是 SPI1 CR1 里的波特率分频字段。SCK 频率 = PCLK2 / 分频系数。

**本课 BR=010（/8）为什么得到 9MHz**：PCLK2 = 72MHz，72MHz / 8 = 9MHz。BR=010 不是直接写 8，而是写编码值 010 对应分频系数 8。

**完整对照表**（PCLK2=72MHz）：

| BR 值 | 分频系数 | SCK 频率     |
| ----- | -------- | ------------ |
| 000   | /2       | 36MHz        |
| 001   | /4       | 18MHz        |
| 010   | /8       | 9MHz ← 本课 |
| 011   | /16      | 4.5MHz       |
| 100   | /32      | 2.25MHz      |
| 101   | /64      | 1.125MHz     |
| 110   | /128     | 562.5kHz     |
| 111   | /256     | 281.25kHz    |

**为什么本课选 9MHz 而不是最快**：回环线短，9MHz 完全够用。36MHz 可能超出 F103 SPI1 的 II2S 模式限制。接外部设备时，SCK 不能超过从机手册标称的最高频率。

### 6.9 SSM 和 SSI 是什么

`SSM`（Software Slave Management）是软件 NSS 管理。置 1 后，SPI 外设不从 PA4(NSS) 引脚读取片选信号，而是从 `SSI` 位读取。

`SSI`（Internal Slave Select）是内部 NSS 信号值。当 SSM=1 时，SSI=1 告诉 SPI 外设"片选有效，可以工作"。

**为什么本课需要 SSM=1、SSI=1**：本课不接外部从机，不配置 PA4 物理片选。如果不设 SSM=1，SPI 外设会检查 PA4 引脚电平——如果 PA4 浮空为低，SPI 认为片选无效，主模式不产生 SCK。设 SSM=1、SSI=1 等于告诉 SPI："不用看 PA4，片选一直有效"。

**不设 SSI=1 的后果**：主模式下 SPI 不会产生 SCK 时钟，数据发不出去，TXE 一直为 0，程序卡在等 TXE 的地方。

### 6.10 TXE、RXNE、BSY 三个标志

**TXE**（Transmit buffer Empty）：发送缓冲区空。TXE=1 表示可以向 DR 写入下一个字节。TXE=0 时强行写 DR 会覆盖正在发送的数据。

**RXNE**（Receive buffer Not Empty）：接收缓冲区非空。SPI 每发送 1 字节就同时收到 1 字节。RXNE=1 表示收到的字节已在 DR 中，可以读取。读 DR 后 RXNE 自动清除。

**BSY**（Busy）：总线忙。BSY=1 表示 SPI 移位器还在工作或数据还没完全移出。RXNE=1 但 BSY 可能还是 1，因为数据虽然在接收缓冲区了，但最后一个 bit 可能还在 SCLK 线上。

**为什么等 BSY 而不仅等 RXNE**：RXNE 只说明数据已到接收缓冲区，但移位器可能还在处理最后一个 bit。如果不等 BSY=0 就拉高 CS（片选），从机可能收到不完整的命令。本课回环没有片选，等 BSY 是在培养正确习惯。

### 6.11 HAL_SPI_TransmitReceive() 是什么

`HAL_SPI_TransmitReceive()` 是 HAL 的同步收发 API。它一次完成发送和接收等长数据，最符合 SPI 全双工本质。

**本课调用**：`HAL_SPI_TransmitReceive(&hspi1, &tx_byte, &rx_byte, 1U, HAL_MAX_DELAY)`

**底层做的事**（对应寄存器版 `spi1_transfer_byte`）：

1. 等 TXE=1
2. 写 DR 发送数据
3. 等 RXNE=1
4. 读 DR 取回数据
5. 等 BSY=0

**HAL SPI 的 Transmit 和 Receive 为什么不单独用**：

- `HAL_SPI_Transmit()` 只发送，不读接收缓冲区。SPI 发了就一定会收，不读会导致 RXNE 一直为 1，下次传输可能出错。
- `HAL_SPI_Receive()` 在主模式下也要发送数据来产生 SCK 时钟，HAL 内部会发 dummy byte。
- `HAL_SPI_TransmitReceive()` 同时收发，把"边发边收"表达得最清楚。

### 6.12 全双工是什么

全双工表示发送和接收同时发生。SPI 的移位寄存器在每个 SCK 边沿，一边把 MOSI 数据推出去，一边把 MISO 数据采进来。

这和 UART 不同：UART 的 TX 和 RX 可以独立工作，你发你的、我收我的。但 SPI 主机只要产生 SCK，MOSI 和 MISO 就同时移位。即使你只关心发送，也会收到一个字节；即使你只想读取从机，也必须发送一个字节来提供时钟。

**这个特性对后续课程的影响**：31 课读 W25Q64 Flash ID 时，你需要发命令字节 `0x90` 和三个 dummy byte，才能从 MISO 读到 ID 数据。发 dummy byte 不是为了"发送什么"，而是为了"产生 SCK 时钟让从机把数据移出来"。不理解这个点，就会疑惑"为什么要发 `0xFF`"。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

| 步骤           | 关键点                                                                            |
| -------------- | --------------------------------------------------------------------------------- |
| 系统时钟 72MHz | HSE 8MHz → PLL ×9 → 72MHz，PCLK2=72MHz（SPI1 时钟源），FLASH 2 等待周期        |
| SysTick 1ms    | LOAD=71999，1ms 中断，`g_ms_ticks++`，`delay_ms(1000)` 控制测试周期           |
| PC13 LED 推挽  | CRH 配 MODE13=10，推挽输出，初始输出高（LED 灭），`BSRR`/`BRR` 控制翻转和点亮 |

---

### 7.2 本课新增：SPI1 GPIO 初始化 —— 打开三层时钟

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;    /* GPIOA 时钟 */
RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;    /* 复用功能时钟 */
RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;    /* SPI1 外设时钟 */
```

**为什么需要三个时钟**：

- `IOPAEN`：让 PA5/PA6/PA7 引脚能工作，这是引脚层
- `AFIOEN`：让复用功能映射能生效，这是复用层
- `SPI1EN`：让 SPI1 外设寄存器可读写，这是外设层

**三者对应三层**：引脚层、复用层、外设层。少了任何一层，SPI 通信都不会正常工作。这和三极管需要基极电流才能导通是一个道理——每一层都是下一层的前提。

---

### 7.3 本课新增：PA5(SCK) 和 PA7(MOSI) —— 复用推挽输出

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE5 | GPIO_CRL_CNF5 |
                GPIO_CRL_MODE7 | GPIO_CRL_CNF7);
GPIOA->CRL |= GPIO_CRL_MODE5;      /* MODE5 = 11 → 50MHz 输出 */
GPIOA->CRL |= GPIO_CRL_CNF5_1;     /* CNF5 = 10 → 复用推挽 */ 
GPIOA->CRL |= GPIO_CRL_MODE7;      /* MODE7 = 11 → 50MHz 输出 */
GPIOA->CRL |= GPIO_CRL_CNF7_1;     /* CNF7 = 10 → 复用推挽 */
```

**为什么是复用推挽，不是通用推挽**：通用推挽输出（CNF=00）由 GPIO 的 ODR 寄存器控制引脚电平。复用推挽输出（CNF=10）由外设（SPI1）控制引脚电平。SCK 和 MOSI 的信号由 SPI1 内部的移位寄存器产生，不是 GPIO 软件控制，所以必须用复用推挽。

**为什么 MODE=11（50MHz）**：SPI 时钟频率较高，引脚需要足够的翻转速度。虽然本课 SCK=9MHz，但配 50MHz 是标准做法，留有裕量。

**如果配成通用推挽的后果**：SPI1 产生的 SCK 和 MOSI 信号无法到达 PA5 和 PA7 引脚，逻辑分析仪上看不到时钟，回环永远失败。

---

### 7.4 本课新增：PA6(MISO) —— 浮空输入

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE6 | GPIO_CRL_CNF6);
GPIOA->CRL |= GPIO_CRL_CNF6_0;     /* CNF6 = 01 → 浮空输入 */
```

**为什么是浮空输入，不是推挽输出**：MISO 是输入信号，电平由外部从机（或回环线）驱动。如果配成推挽输出，STM32 也会驱动 PA6，和外部驱动冲突，可能烧 IO。浮空输入让 PA6 只读取电平，不主动驱动。

**为什么浮空输入而不是上拉/下拉输入**：本课回环线直接把 PA7 输出接到 PA6，PA7 是推挽输出，驱动能力强，PA6 不需要上下拉。接真实从机时，MISO 通常由从机驱动，也不需要上下拉。

---

### 7.5 本课新增：SPI1 CR1 配置 —— 清旧位 + 设新位

**第一步：清空所有可能影响配置的位**

```c
SPI1->CR1 &= ~(SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_CRCEN |
               SPI_CR1_DFF | SPI_CR1_RXONLY | SPI_CR1_SSM |
               SPI_CR1_SSI | SPI_CR1_LSBFIRST | SPI_CR1_BR |
               SPI_CR1_MSTR | SPI_CR1_CPOL | SPI_CR1_CPHA);
```

**为什么先清位再设位**：上电或复位后 CR1 的值不一定是 0。如果不清除旧值，新设的位可能和旧位组合出意料之外的模式。比如旧代码设了 CPOL=1，本课想设 CPOL=0，如果不先清 CPOL，直接写 CR1 不会清除 CPOL=1，最终硬件看到的是 CPOL=1 而不是 0。

**第二步：设需要的位**

```c
SPI1->CR1 |= SPI_CR1_MSTR;     /* MSTR=1 → 主模式 */
SPI1->CR1 |= SPI_CR1_BR_1;     /* BR=010 → /8 → 9MHz */
SPI1->CR1 |= SPI_CR1_SSM;      /* SSM=1 → 软件 NSS */
SPI1->CR1 |= SPI_CR1_SSI;      /* SSI=1 → 内部 NSS 有效 */
```

**每位的含义和为什么是这个值**：

- `MSTR=1`：主模式。本课 STM32 控制 SCK，所以是主机。如果设 MSTR=0（从模式），SPI1 不会产生 SCK。
- `BR=010`：PCLK2/8=9MHz。9MHz 对回环线足够，对大多数外部设备也安全。
- `SSM=1`：不使用 PA4 物理片选。如果 SSM=0，SPI1 会检查 PA4 电平——PA4 浮空可能是低电平，SPI 认为片选无效，不产生 SCK。
- `SSI=1`：内部片选有效。和 SSM=1 配合，告诉 SPI1"片选一直有效"。
- `CPOL=0, CPHA=0`：默认值就是 0，所以不需要显式置位。Mode 0 是最通用的模式。

**第三步：打开 SPI**

```c
SPI1->CR1 |= SPI_CR1_SPE;
```

**为什么 SPE 最后打开**：如果先打开 SPE 再配置其他位，SPI 可能在配置过程中产生异常时钟脉冲，导致外部从机收到错误数据。所有配置完成后打开 SPE，SPI 从干净状态开始工作。

---

### 7.6 本课新增：`spi1_transfer_byte()` —— SPI 同步收发

这是 SPI 最核心的函数，对应 HAL 的 `HAL_SPI_TransmitReceive()`。

```c
static uint8_t spi1_transfer_byte(uint8_t tx_byte)
{
    /* ① 等发送缓冲区空 */
    while ((SPI1->SR & SPI_SR_TXE) == 0U) { }

    /* ② 写 DR 启动传输 */
    *(__IO uint8_t *)&SPI1->DR = tx_byte;

    /* ③ 等接收缓冲区非空 */
    while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }

    /* ④ 读 DR 取回数据 */
    uint8_t rx_byte = *(__IO uint8_t *)&SPI1->DR;

    /* ⑤ 等总线空闲 */
    while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

    return rx_byte;
}
```

**五步的物理含义**：

| 步骤       | 标志   | 硬件在做什么                                                                       |
| ---------- | ------ | ---------------------------------------------------------------------------------- |
| ① 等 TXE  | TXE=1  | 发送缓冲区空，移位寄存器也空，可以写新数据                                         |
| ② 写 DR   | -      | 数据写入发送缓冲区，SPI 自动在 SCK 上产生 8 个时钟，同时从 MOSI 移出、从 MISO 移入 |
| ③ 等 RXNE | RXNE=1 | 8 个时钟结束，收到的数据已到接收缓冲区                                             |
| ④ 读 DR   | -      | 取走收到的数据，RXNE 自动清除                                                      |
| ⑤ 等 BSY  | BSY=0  | 移位器完全空闲，最后一个 bit 已发送完毕                                            |

**为什么用 `*(__IO uint8_t *)&SPI1->DR` 而不是 `SPI1->DR`**：`SPI1->DR` 是 16 位寄存器。如果直接读写，编译器会生成 16 位访问指令。但本课的数据帧是 8 位（DFF=0），所以用 8 位指针强转，只操作低 8 位。写 16 位到 DR 会触发 16 位传输，多出 8 个时钟，数据错乱。

**为什么 BSY 在 RXNE 之后还要等**：RXNE=1 说明数据已到接收缓冲区，但移位器可能还在处理最后一个 bit 的收尾工作。如果不等 BSY=0 就拉高片选（后续课程），从机会收到不完整的命令。本课回环没有片选，但养成这个习惯很重要。

---

### 7.7 本课新增：`main()` 中的回环测试

```c
while (1) {
    uint8_t rx_byte = spi1_transfer_byte(tx_byte);

    if (rx_byte == tx_byte) {
        led_toggle();    /* 回环成功，LED 翻转 */
    } else {
        led_on();        /* 回环失败，LED 常亮 */
    }

    /* 交替发 0xA5 和 0x3C */
    tx_byte = (tx_byte == 0xA5U) ? 0x3CU : 0xA5U;
    delay_ms(1000);
}
```

**为什么交替发 0xA5 和 0x3C**：

- `0xA5` = `1010 0101`：0 和 1 交替，测试每个 bit 的电平变化
- `0x3C` = `0011 1100`：和 0xA5 完全不同的 bit 模式，避免"巧合相同"
- 两个值交替，能暴露更多问题：如果只发一个值，某个 bit 恰好因为电平不变而"看起来正确"，实际电路有问题也发现不了

**为什么判断标准是 `rx_byte == tx_byte`**：回环的物理含义是 PA7(MOSI) 的输出直接接到 PA6(MISO) 的输入。如果 GPIO 配置、SPI 模式、SCK 时序都正确，收到的必定等于发出的。不等于说明至少有一层出错。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

| 步骤           | HAL API                                             | 对应寄存器版                     |
| -------------- | --------------------------------------------------- | -------------------------------- |
| 系统时钟 72MHz | `HAL_RCC_OscConfig()` + `HAL_RCC_ClockConfig()` | 手动写 RCC→CR、CFGR、FLASH→ACR |
| SysTick        | `HAL_Init()` 内部调用 `HAL_InitTick()`          | 手动写 SysTick→LOAD、CTRL       |
| PC13 LED       | `HAL_GPIO_Init(GPIOC, PIN_13, PP)`                | 手动写 GPIOC→CRH                |
| GPIO 配置      | `HAL_GPIO_Init(GPIOA, PIN_5/6/7, ...)`            | 手动写 GPIOA→CRL                |

**HAL SPI1 配置字段 → 寄存器映射**：

| HAL 字段              | 本课取值                    | 对应寄存器位                 | 位含义                      |
| --------------------- | --------------------------- | ---------------------------- | --------------------------- |
| `Mode`              | `SPI_MODE_MASTER`         | `CR1.MSTR`                 | MSTR=1：主模式              |
| `Direction`         | `SPI_DIRECTION_2LINES`    | `CR1.BIDIMODE=0, RXONLY=0` | 全双工，同时用 MOSI 和 MISO |
| `DataSize`          | `SPI_DATASIZE_8BIT`       | `CR1.DFF`                  | DFF=0：8 位数据帧           |
| `CLKPolarity`       | `SPI_POLARITY_LOW`        | `CR1.CPOL`                 | CPOL=0：空闲低              |
| `CLKPhase`          | `SPI_PHASE_1EDGE`         | `CR1.CPHA`                 | CPHA=0：第一个边沿采样      |
| `NSS`               | `SPI_NSS_SOFT`            | `CR1.SSM=1, CR1.SSI=1`     | 软件 NSS，内部片选有效      |
| `BaudRatePrescaler` | `SPI_BAUDRATEPRESCALER_8` | `CR1.BR=010`               | PCLK2/8=9MHz                |
| `FirstBit`          | `SPI_FIRSTBIT_MSB`        | `CR1.LSBFIRST`             | LSBFIRST=0：高位先发        |

---

### 8.2 本课新增：`HAL_SPI_Init()` 底层做了什么

```c
hspi1.Instance = SPI1;
hspi1.Init.Mode = SPI_MODE_MASTER;
// ... 设置其他字段
HAL_SPI_Init(&hspi1);
```

**HAL_SPI_Init 的底层操作**（对应寄存器版 `spi1_init`）：

1. 根据 `hspi1.Init` 各字段，计算 CR1 的最终值
2. 写 CR1（MSTR、BR、CPOL、CPHA、SSM、SSI、LSBFIRST、DFF 等）
3. 写 CR2（配置中断使能等，本课不需要）
4. 写 `CR1.SPE = 1`（使能 SPI）

**HAL 和寄存器版的区别**：HAL 不需要你手动清位再设位——HAL 内部会先算出目标值，一次性写入 CR1。但 HAL 不会帮你检查配置是否合理（比如 BR=/2 得到 36MHz 是否超限），这个需要你自己查手册。

---

### 8.3 本课新增：`HAL_SPI_TransmitReceive()` —— 同步收发

```c
if (HAL_SPI_TransmitReceive(&hspi1, &tx_byte, &rx_byte, 1U, HAL_MAX_DELAY) != HAL_OK) {
    error_handler();
}
```

**参数含义**：

- `&hspi1`：SPI 句柄，包含 SPI1 的配置和状态
- `&tx_byte`：发送缓冲区指针，本课 1 字节
- `&rx_byte`：接收缓冲区指针，本课 1 字节
- `1U`：数据长度=1 字节
- `HAL_MAX_DELAY`：超时永不超时（阻塞等待）

**HAL 底层做的事**（对应寄存器版 `spi1_transfer_byte` 的全部 5 步）：

1. 等 TXE=1
2. 写 DR 发送 `tx_byte`
3. 等 RXNE=1
4. 读 DR 到 `rx_byte`
5. 等 BSY=0

**为什么检查返回值**：`HAL_SPI_TransmitReceive()` 返回非 `HAL_OK` 时，可能是 SPI 外设状态异常、超时或参数错误。此时 `rx_byte` 里的值没有意义，不能用来判断回环是否成功。检查返回值把"API 调用失败"和"通信成功但数据不对"区分开。

**HAL 的 TransmitReceive 和单独 Transmit/Receive 的区别**：

- `HAL_SPI_Transmit()` 只发不收，接收缓冲区被忽略，RXNE 不清
- `HAL_SPI_Receive()` 要发 dummy byte 产生时钟，HAL 内部自动发
- `HAL_SPI_TransmitReceive()` 同时收发，最符合 SPI 全双工本质

---

### 8.4 本课新增：`error_handler()` —— 通信失败处理

```c
static void error_handler(void)
{
    __disable_irq();
    while (1) { }
}
```

**为什么需要这个函数**：如果 `HAL_SPI_TransmitReceive()` 失败（比如 SPI 没初始化、时钟没开），继续运行会读到无效数据，LED 现象不可靠。`__disable_irq()` 关全局中断，死循环，LED 保持进入时的状态——如果成功闪烁过，LED 停在当前状态；如果初始化就失败，LED 灭。

**和寄存器版的区别**：寄存器版没有错误处理，如果 SPI 配置错误导致 TXE 一直为 0，程序会卡在 `while (!TXE)` 死循环。HAL 版有超时机制，超时后返回错误，不会永久卡死。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
GPIO 复用 -> CR1 模式 -> TXE -> DR -> RXNE -> DR -> BSY
```

HAL 版重点看：

```text
SPI_HandleTypeDef.Init 字段 -> SPI1->CR1
HAL_SPI_TransmitReceive -> 同步收发一个或多个字节
```

SPI 的“读”通常也需要“写”来提供时钟，这个模型后面访问 W25Q64 时会立刻用到。

## 10. 检验问题清单

### 10.1 为什么 SPI 发送和接收同时发生？

**答**：SPI 主机每产生一个 SCK 时钟，MOSI 移出 1 bit，同时 MISO 移入 1 bit。8 个时钟后，发送和接收各完成 1 字节。

### 10.2 PA7 为什么要接 PA6？

**答**：这是回环验证。PA7 输出的 MOSI 数据直接送到 PA6 的 MISO 输入，收到值应等于发送值。

### 10.3 `CPOL=0, CPHA=0` 表示什么？

**答**：SCK 空闲低电平，第一个边沿采样数据，也就是 SPI Mode 0。

### 10.4 `TXE=1` 表示可以直接读数据吗？

**答**：不是。`TXE=1` 只表示发送缓冲区空，可以写入待发送数据。接收完成要看 `RXNE`。

### 10.5 为什么还要等 `BSY=0`？

**答**：`RXNE` 表示接收缓冲区有数据，但总线移位或收尾可能尚未完全结束。等 `BSY=0` 可以确认 SPI 空闲。

### 10.6 `HAL_SPI_TransmitReceive()` 为什么比单独 Transmit 更适合本课？

**答**：因为 SPI 本质是同步收发。回环测试既要发送也要检查收到的数据，TransmitReceive 正好对应这个模型。

### 10.7 如果没接 PA7 到 PA6，会怎样？

**答**：MISO 悬空，读回值可能随机或固定错误，LED 会点亮或不按预期翻转。

### 10.8 SCK 频率由什么决定？

**答**：由 SPI1 时钟源 PCLK2 和 `BR` 分频决定。本课 PCLK2=72MHz，BR=/8，所以 SCK=9MHz。

### 10.9 忘了设 SSM=1 和 SSI=1 会怎样？

**答**：SPI 外设会检查 PA4(NSS) 引脚的电平。PA4 浮空时电平不确定，可能为低电平——SPI 认为片选无效，主模式不会产生 SCK 时钟。表现是 TXE 一直为 0，程序会卡在 `while (!TXE)` 死循环。即使 PA4 恰好浮空为高，也可能因为电磁干扰导致 SPI 在传输中途认为片选无效而停止。

### 10.10 忘了等 BSY=0 会怎样（本课回环场景）？

**答**：本课回环没有片选线，不等 BSY 暂时不会出问题。但 31 课接 W25Q64 时，如果在 BSY=1 时拉高 CS 片选，最后一个 bit 可能还在 SCLK 线上，W25Q64 收到不完整的命令（比如少了最后一个 bit 的命令字节），Flash 会忽略这个命令或进入错误状态，后续读取全部失败。

### 10.11 HAL 的 `SPI_BAUDRATEPRESCALER_8` 对应寄存器版的什么？

**答**：对应 `CR1.BR = 010`，即 PCLK2 / 8。HAL 的枚举值 `SPI_BAUDRATEPRESCALER_8` 在 HAL 内部会被翻译成 `CR1.BR = 010`。HAL 还提供 `SPI_BAUDRATEPRESCALER_2/4/16/32/64/128/256` 等选项，分别对应 BR 的不同编码值。注意枚举值名是"分频系数"而不是"BR 编码值"——`SPI_BAUDRATEPRESCALER_8` 表示分频 8，BR 编码是 010。

## 11. 工程实现步骤

### 11.1 需求分析

本课先验证 SPI1 最小收发链路，不接外部从机。回环能证明主机模式和引脚复用是否正确。

### 11.2 硬件核查

确认 PA7 和 PA6 已用杜邦线相连。若用逻辑分析仪，可同时观察 PA5 SCK、PA7 MOSI、PA6 MISO。

### 11.3 寄存器路线

打开 GPIOA、AFIO、SPI1 时钟，配置 PA5/PA7 复用推挽、PA6 输入，设置 SPI1 主模式、Mode 0、/8 分频、软件 NSS，最后轮询 TXE/RXNE/BSY。

### 11.4 HAL 路线

用 `GPIO_InitTypeDef` 配引脚，用 `SPI_HandleTypeDef` 配 SPI1，用 `HAL_SPI_TransmitReceive()` 完成同步收发。

### 11.5 工程思维

先做回环，再接外部器件。这样接 W25Q64 或 OLED 出问题时，可以区分是 SPI 基础配置错，还是器件协议/片选/命令错。

### 11.6 常见工程陷阱

MOSI/MISO 接反、PA5/PA7 没配复用推挽、NSS 管理没配好、CPOL/CPHA 与器件不匹配、忘记读 DR 清 RXNE，都会导致通信失败。

还有一个常见误区是只看 MOSI，不看 MISO。SPI 回环必须同时验证 PA7 输出和 PA6 输入；逻辑分析仪上 MOSI 有波形但 MISO 没回到 PA6，LED 仍会报错。

另一个陷阱是把 SPI 速度一开始就开太高。本课 9MHz 对短回环线通常没问题；接长线、面包板或外部模块时，可以先把分频降到 /16、/32，确认协议正确后再提速。

## 12. 运行现象

**通信正常时（回环线已接 PA7→PA6）**：

- PC13 LED：每 1 秒翻转一次（亮 1 秒、灭 1 秒），周期约 2 秒。因为 `delay_ms(1000)` 在每次循环后执行，`rx_byte == tx_byte` 时只翻转不点亮。
- 逻辑分析仪：PA5(SCK) 每 1 秒出现一组 8 个时钟脉冲，频率约 9MHz。PA7(MOSI) 输出 `0xA5`（`10100101`）和 `0x3C`（`00111100`）交替。PA6(MISO) 的波形和 PA7 完全相同（回环）。

**通信异常时**：

- 回环线没接：MISO 浮空，接收值随机，大概率不等于发送值。LED 常量（低电平点亮），不再翻转。
- PA5/PA7 没配复用推挽（配成通用推挽）：SCK 和 MOSI 无波形，TXE 一直为 0，程序卡在 `while (!TXE)` 死循环，LED 不闪不亮。
- SSM/SSI 没设：寄存器版卡在等 TXE，HAL 版 `HAL_SPI_TransmitReceive()` 超时返回错误，进入 `error_handler()`，LED 保持在进入时的状态（灭）。

**如何区分"卡死"和"常亮"**：

- LED 常亮 = 程序在运行，但回环数据不对
- LED 不闪不亮 = 程序卡在等 TXE 或 `error_handler()` 中
- 用逻辑分析仪看 PA5 是否有 8 脉冲组：有 = 程序在运行，无 = 卡死

## 13. 常见问题排查

### 13.1 LED 一直亮

先检查 PA7 是否真正接到 PA6，再查 GPIOA 和 SPI1 时钟是否打开、PA5/PA7 是否复用推挽。

### 13.2 逻辑分析仪看不到 SCK

检查 `SPE` 是否置位、`MSTR/SSM/SSI` 是否配置正确，以及代码是否真的写入 `DR`。

### 13.3 收到值总是固定错误

检查 MISO 是否悬空、PA6 是否配置输入、回环线是否接错到 PA5 或 PA4。

### 13.4 HAL 版卡在收发函数

检查 `HAL_SPI_Init()` 是否成功，`hspi1.Instance` 是否为 SPI1，GPIO 复用是否配置在 HAL 调用之前。

### 13.5 接外部器件后失败

回到本课先验证回环。回环正常后，再查外部器件的 CS、CPOL/CPHA、最高 SCK、命令格式和供电。

## 14. 本课最核心的结论

1. SPI 是同步全双工，发送和接收在同一组 SCK 时钟里同时完成。
2. SPI1 默认使用 PA5/SCK、PA6/MISO、PA7/MOSI。
3. SCK/MOSI 必须配成复用推挽，MISO 配成输入。
4. Mode 0 表示 `CPOL=0`、`CPHA=0`。
5. `TXE/RXNE/BSY` 分别对应可写、可读、总线忙三个阶段。
6. HAL 的 `TransmitReceive` 是对 SPI 同步收发模型的直接封装。

## 15. 建议你现在怎么读这节课

先用第 5 章脑图理解一次字节交换，再读第 6 章把 SCK/MOSI/MISO 和 `CR1/SR/DR` 对上。最后逐行读 `spi1_transfer_byte()`，确认每个等待标志对应哪个硬件阶段。

## 16. 扩展练习

1. 把分频改成 `/16`，用逻辑分析仪观察 SCK 变化。
2. 改成 Mode 3，观察空闲电平和采样边沿变化。
3. 连续发送 4 个字节，写一个 `spi1_transfer_buffer()`。
4. 在 HAL 版检查 `HAL_SPI_TransmitReceive()` 的返回值并加入错误闪烁。

## 17. 下一课预告

- 上一课：[29_i2c_mpu6050](../29_i2c_mpu6050/README.md)
- 下一课：[31_spi_w25q64](../31_spi_w25q64/README.md)
