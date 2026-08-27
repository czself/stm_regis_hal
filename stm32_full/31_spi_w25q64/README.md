# 第 31 课：SPI 读 W25Q64

## 1. 本课到底在学什么

本课表面现象是：STM32 通过 SPI1 向 W25Q64 Flash 发送 `0x9F` 命令读取 JEDEC ID。如果读到的 Manufacturer ID 是 `0xEF`，PC13 LED 快速闪烁；如果不是，LED 慢速闪烁。

真正要学的是：从 SPI 回环进入真实 SPI 从机访问后，多了两个必须单独理解的动作：

```text
片选 CS 控制一笔事务的开始和结束
命令字节决定从机接下来输出什么数据
```

本课完整链路是：

```text
PA4 拉低，选中 W25Q64
  -> 发送 0x9F JEDEC ID 命令
  -> 继续发送 0xFF dummy byte 提供 SCK
  -> W25Q64 在 MISO 上依次输出 Manufacturer ID、Memory Type、Capacity
  -> PA4 拉高，结束本次 SPI 事务
  -> 判断 Manufacturer ID 是否为 0xEF
```

这里一定要记住：SPI 读数据也需要主机继续“发送”字节，因为从机输出数据需要 SCK，而 SCK 只能由主机产生。

## 2. 本课学习目标

学完本课，你应该能回答：

1. W25Q64 是什么器件？
2. 为什么访问真实 SPI 从机必须控制 CS？
3. `0x9F` 命令为什么能读 JEDEC ID？
4. 为什么发送 `0xFF` dummy byte 才能读回 ID？
5. `0xEF` 表示什么？
6. PA4、PA5、PA6、PA7 分别承担什么信号？
7. 为什么拉高 CS 前要等 `BSY=0`？
8. HAL 版 `HAL_SPI_TransmitReceive()` 在读 ID 时对应哪一步？

## 3. 本课目录结构

```text
31_spi_w25q64/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 直接控制 GPIOA 和 SPI1 寄存器。  
`hal/` 使用 HAL GPIO 控制 CS，用 `HAL_SPI_TransmitReceive()` 交换字节。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- 外部模块：W25Q64 SPI Flash
- SPI1 引脚：
  - PA4：CS，软件片选
  - PA5：SCK
  - PA6：MISO
  - PA7：MOSI
- LED：PC13

接线：

```text
W25Q64 VCC  -> 3.3V
W25Q64 GND  -> GND
W25Q64 CS   -> PA4
W25Q64 CLK  -> PA5
W25Q64 DO   -> PA6 / MISO
W25Q64 DI   -> PA7 / MOSI
```

W25Q64 是 3.3V 器件，不要直接接 5V 逻辑。

## 5. 先建立一个最基本的脑图

```text
SPI1 初始化
  -> PA5/PA7 复用推挽
  -> PA6 输入
  -> PA4 普通推挽输出，默认高电平
  -> SPI1 主模式、Mode 0、/8 分频

读 JEDEC ID
  -> CS 拉低
  -> 发送 0x9F
  -> 发送 0xFF，读回 Manufacturer ID
  -> 发送 0xFF，读回 Memory Type
  -> 发送 0xFF，读回 Capacity
  -> 等 BSY=0
  -> CS 拉高

判断结果
  -> Manufacturer ID == 0xEF：快闪
  -> 否则：慢闪
```

上一课回环只验证 SPI 主机自己能收发；本课开始要理解外部从机协议：CS 包围一次事务，命令告诉从机要返回哪类数据。

## 6. 先认识本课里出现的核心名词

### 6.1 `W25Q64` 是什么

W25Q64 是 Winbond 的 SPI NOR Flash，容量 64Mbit，也就是 8MByte。

它属于外部存储器件层。STM32 通过 SPI 命令读写它的状态寄存器、JEDEC ID、数据区、擦除扇区等。

本课只读 ID，不做写入和擦除，目的是先确认 SPI 物理连接和命令事务正确。

### 6.2 `CS` 是什么

`CS` 是 Chip Select，片选信号。本课使用 PA4 手动控制。

SPI 总线上可以挂多个从机，每个从机通常有独立 CS。CS 拉低表示选中该器件，CS 拉高表示结束本次事务。

如果 CS 一直高，W25Q64 不会响应 `0x9F`。如果过早拉高，读 ID 事务会被中断。

### 6.3 `PA4` 是什么

PA4 在本课被当作普通 GPIO 输出，用来控制 W25Q64 的 CS。

虽然 SPI1 有 NSS 功能，但代码选择软件片选。这样读写外部 Flash 时更直观，也更适合很多实际驱动。

### 6.4 `0x9F` 是什么

`0x9F` 是 W25Q64 的 JEDEC ID 读取命令。

它属于 W25Q64 器件协议层，不是 STM32 SPI 寄存器。STM32 只是把这个字节从 MOSI 发出去；W25Q64 收到后才决定后续在 MISO 输出 ID。

命令错时，即使 SCK/MOSI/MISO 都正常，也读不到预期 ID。

### 6.5 `JEDEC ID` 是什么

JEDEC ID 是 Flash 器件身份信息，通常包括：

- Manufacturer ID：厂商 ID
- Memory Type：存储类型
- Capacity：容量编码

W25Q 系列 Winbond 厂商 ID 常见为 `0xEF`。本课只取第一个字节判断是否为 `0xEF`。

### 6.6 `dummy byte` 是什么

dummy byte 是主机为了产生 SCK 而发送的占位字节。

SPI 从机不会自己输出数据，必须由主机继续提供时钟。本课发送 `0xFF`，真正关心的是同时从 MISO 收到的 ID 字节。

发送 `0x00` 通常也能产生时钟，但 `0xFF` 在 SPI Flash 读操作中更常见，因为总线空闲和未驱动状态常表现为高电平。

dummy byte 属于 SPI 协议使用方式，不是 W25Q64 内部存储数据。主机发送 `0xFF` 的目的不是让 Flash 接收这个值，而是让 SCK 继续跳变。Flash 在这些时钟期间把 JEDEC ID 从 DO/MISO 推出来。

这正好回扣上一课全双工：读也是交换。每发一个 dummy，就同时读回一个字节。没有 dummy，就没有时钟；没有时钟，从机就没有机会把数据移出来。

### 6.7 `Manufacturer ID 0xEF` 是什么

`0xEF` 是 Winbond 常见 JEDEC 厂商 ID。

本课用它作为成功条件：读到 `0xEF` 说明 CS、SPI 命令、MISO 返回路径基本正确。

若读到 `0xFF`，可能 MISO 悬空或从机未被选中；读到 `0x00`，可能线路被拉低或模式错误。

### 6.8 `SPI1->DR` 是什么

`DR` 是 SPI 数据寄存器。

写 `DR` 会启动一字节移位；读 `DR` 得到同时收到的字节。W25Q64 读 ID 时，命令字节的读回值通常不重要，后续 dummy byte 的读回值才是 ID。

### 6.9 `TXE / RXNE / BSY` 是什么

这三个是 SPI 状态标志：

- `TXE`：可以写下一个发送字节
- `RXNE`：收到一个字节，可以读 `DR`
- `BSY`：SPI 总线仍忙

本课拉高 CS 前等待 `BSY=0`，确保最后一个 ID 字节已经传输完成。

### 6.10 `HAL_GPIO_WritePin()` 是什么

HAL 版用它控制 PA4 CS：

- `GPIO_PIN_RESET`：CS 拉低，选中 W25Q64
- `GPIO_PIN_SET`：CS 拉高，结束事务

它对应寄存器版写 `GPIOA->BRR/BSRR`。

### 6.11 `HAL_SPI_TransmitReceive()` 是什么

HAL 版用它实现 `spi_transfer()`，每调用一次交换 1 个字节。

读 JEDEC ID 时，发送 `0x9F` 是命令阶段；发送 `0xFF` 是提供时钟并读取返回字节。

### 6.12 `Mode 0` 是什么

W25Q64 支持常见 SPI Mode 0。本课配置 `CPOL=0`、`CPHA=0`，SCK 空闲低，第一个边沿采样。

若模式不匹配，可能读到稳定但错误的 ID。

## 7. 寄存器版代码逐步讲解

### 7.1 时钟和 LED

系统时钟为 72MHz，SPI1 在 APB2 上，分频后 SCK 为 9MHz。PC13 根据 ID 判断结果快慢闪。

### 7.2 `spi1_init()` 打开 GPIOA 和 SPI1

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_SPI1EN;
```

GPIOA 用于 SPI 引脚和 CS，SPI1 是通信外设本体。

### 7.3 配置 PA4 为 CS

代码把 PA4 配成普通推挽输出，并默认写高：

```c
GPIOA->BSRR = GPIO_BSRR_BS4;
```

高电平表示未选中 W25Q64。每次读 ID 前再拉低。

### 7.4 配置 PA5/PA6/PA7

PA5/SCK 和 PA7/MOSI 配成复用推挽，PA6/MISO 配成输入。这与上一课 SPI 回环一致，只是 MISO 现在来自外部 W25Q64。

### 7.5 配置 SPI1 主模式

```c
SPI1->CR1 = SPI_CR1_MSTR |
            SPI_CR1_SSM |
            SPI_CR1_SSI |
            SPI_CR1_BR_1 |
            SPI_CR1_SPE;
```

含义是主模式、软件 NSS、内部 NSS 有效、/8 分频、使能 SPI。CPOL/CPHA 默认 0，所以 Mode 0。

### 7.6 `spi1_transfer()` 等待 TXE

```c
while(!(SPI1->SR&SPI_SR_TXE)){}
```

等发送缓冲区空，确保可以写入下一个字节。

### 7.7 `spi1_transfer()` 写 8 位 DR

```c
*(__IO uint8_t *)&SPI1->DR=b;
```

写入一个字节后，SPI1 输出 8 个 SCK，MOSI 发出这个字节，同时 MISO 收入一个字节。

### 7.8 `spi1_transfer()` 等待 RXNE 并返回

```c
while(!(SPI1->SR&SPI_SR_RXNE)){}
return (uint8_t)SPI1->DR;
```

`RXNE` 置位说明一个字节交换完成。返回值就是这次交换从 MISO 收到的字节。

### 7.9 `w25q64_read_manufacturer_id()` 拉低 CS

```c
GPIOA->BRR=GPIO_BRR_BR4;
```

CS 低电平开始一笔 SPI Flash 命令事务。W25Q64 会从此刻开始解释后续 MOSI 字节。

### 7.10 发送 `0x9F`

```c
spi1_transfer(0x9F);
```

这一步把 JEDEC ID 命令发给 W25Q64。此时收到的返回值通常不使用，因为从机还在接收命令。

### 7.11 发送 dummy 读回 ID

```c
id=spi1_transfer(0xFF);
spi1_transfer(0xFF);
spi1_transfer(0xFF);
```

第一次 dummy 的返回值是 Manufacturer ID，后两次通常是 Memory Type 和 Capacity。当前代码只保存第一个字节。

### 7.12 等待 `BSY=0` 后拉高 CS

```c
while(SPI1->SR&SPI_SR_BSY){}
GPIOA->BSRR=GPIO_BSRR_BS4;
```

确认 SPI 完全空闲后结束事务。若过早拉高 CS，最后一字节可能还没完整传输。

CS 是 W25Q64 判断一笔命令事务开始和结束的边界。`0x9F`、三个 dummy byte 和返回的三个 ID 字节必须处在同一次 CS 低电平窗口里。如果中途拉高 CS，Flash 会认为命令结束，后续字节不再属于同一条读 ID 指令。

等待 `BSY=0` 后再拉高 CS，是为了确保最后一个 dummy 对应的返回位已经全部移完。这个习惯后续写使能、页编程、读状态寄存器时同样重要。

### 7.13 主循环判断 ID

读到 `0xEF` 时短延时快闪；否则长延时慢闪。这个现象让你不用串口也能判断 Flash 是否基本响应。

## 8. HAL 版代码逐步讲解

### 8.1 HAL GPIO 配置 CS

PA4 配成 `GPIO_MODE_OUTPUT_PP`，默认写 `GPIO_PIN_SET`。这对应寄存器版普通推挽输出和 `BSRR_BS4`。

### 8.2 HAL SPI 引脚配置

PA5/PA7 使用 `GPIO_MODE_AF_PP`，PA6 使用 `GPIO_MODE_INPUT`。这与寄存器版的复用推挽和输入配置对应。

### 8.3 `SPI_HandleTypeDef hspi1`

字段设置为主模式、双线全双工、8 位、Mode 0、软件 NSS、/8 分频、高位先发。

### 8.4 `HAL_SPI_Init()`

该函数根据句柄字段写 SPI1 的 `CR1/CR2`，并使能 SPI。它对应寄存器版 `SPI1->CR1 = ...`。

### 8.5 `spi_transfer()`

```c
HAL_SPI_TransmitReceive(&hspi1,&b,&r,1,100);
```

发送 1 字节，同时接收 1 字节。返回值 `r` 就是 MISO 读回的字节。

当前源码已经检查 `HAL_SPI_TransmitReceive()` 的返回值：不为 `HAL_OK` 时进入 `error_handler()`。否则 SPI 超时或状态错误时，`r` 可能只是初始值，主循环会把它误当作真实 ID。

另外，HAL 只负责交换字节，不负责自动控制 CS。CS 拉低、保持、拉高仍然由 `w25q64_read_manufacturer_id()` 自己控制。把 CS 控制放在 `spi_transfer()` 内部会破坏多字节事务，因为每交换一个字节就结束片选，Flash 就无法把 `0x9F + dummy` 当成同一条命令。

### 8.6 HAL 版 `w25q64_read_manufacturer_id()`

函数先 `HAL_GPIO_WritePin(...RESET)` 拉低 CS，再发送 `0x9F` 和三个 `0xFF`，最后 `GPIO_PIN_SET` 拉高 CS。

它和寄存器版事务顺序完全一致。

### 8.7 HAL 版 ID 判断

`mid==0xEF` 时 `HAL_Delay(100)`，否则 `HAL_Delay(500)`。快慢闪代表 W25Q64 是否返回预期厂商 ID。

### 8.8 HAL 返回值的边界

本课 `spi_transfer()` 已检查 `HAL_SPI_TransmitReceive()` 返回值，失败时进入 `error_handler()`。这正是实际工程推荐的做法，避免 SPI 超时后继续用旧的 `r` 判断。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
CS 低 -> 命令 -> dummy 读 -> BSY 空闲 -> CS 高
```

HAL 版重点看：

```text
HAL_GPIO_WritePin 控制 CS
HAL_SPI_TransmitReceive 交换每个字节
```

W25Q64 的关键不只是 SPI 基础配置，还包括器件命令协议。SPI 外设只负责搬运字节，Flash 决定 `0x9F` 后输出什么。

## 10. 检验问题清单

### 10.1 为什么本课需要 PA4？

**答**：PA4 用作 W25Q64 的 CS。CS 拉低时从机才响应 SPI 字节，拉高时结束当前命令事务。

### 10.2 `0x9F` 是 STM32 的寄存器值吗？

**答**：不是。`0x9F` 是 W25Q64 的 JEDEC ID 读取命令，属于外部 Flash 协议。

### 10.3 为什么读 ID 还要发送 `0xFF`？

**答**：SPI 从机输出数据需要 SCK，而 SCK 由主机产生。发送 dummy byte 是为了提供时钟，同时接收 MISO 数据。

### 10.4 `0xEF` 表示什么？

**答**：它是 Winbond 常见 JEDEC Manufacturer ID。读到它说明 W25Q64 基本在线且命令通信正确。

### 10.5 为什么拉高 CS 前要等 `BSY=0`？

**答**：确保最后一个字节已经完全移位结束。过早结束事务可能导致从机没有完整输出数据。

### 10.6 如果读到 `0xFF` 常见原因是什么？

**答**：可能 MISO 悬空、CS 没拉低、Flash 未供电，或者从机没有真正响应。

### 10.7 HAL 版的 `spi_transfer()` 和寄存器版哪段对应？

**答**：对应等待 TXE、写 DR、等待 RXNE、读 DR 的一字节交换过程。

### 10.8 W25Q64 后续写数据前还需要学什么？

**答**：需要学习写使能、页编程、扇区擦除、状态寄存器 BUSY 位和地址字节，这些不在本课最小读 ID 范围内。

## 11. 工程实现步骤

### 11.1 需求分析

本课目标是确认 SPI1 能和真实 Flash 从机通信。读 JEDEC ID 是最安全的只读验证，不会修改 Flash 内容。

### 11.2 硬件核查

确认 VCC 为 3.3V，GND 共地，CS/SCK/MISO/MOSI 不接反。特别注意 W25Q64 模块的 DO 接 STM32 MISO，DI 接 STM32 MOSI。

### 11.3 寄存器路线

配置 PA4 普通输出、PA5/PA7 复用推挽、PA6 输入，配置 SPI1 主模式 Mode 0，然后按 CS 包围 `0x9F + dummy` 事务。

### 11.4 HAL 路线

用 HAL GPIO 控制 CS，用 `SPI_HandleTypeDef` 配 SPI1，用 `HAL_SPI_TransmitReceive()` 交换命令和 dummy byte。

### 11.5 工程思维

读 ID 是 SPI Flash 驱动的第一步。只有 ID 稳定正确后，才值得继续写擦除和数据读写，否则先不要碰会修改 Flash 的命令。

### 11.6 常见工程陷阱

CS 忘记拉低、MISO/MOSI 接反、读数据时没发 dummy、片选过早拉高、SPI 模式不匹配、电源电平不对，是这课最常见的问题。

还有一个陷阱是只读第一个 ID 字节就过早下结论。本课为了最小验证只判断 `0xEF`，但完整 JEDEC ID 还有 Memory Type 和 Capacity。若厂商 ID 正确但容量字节不对，要继续检查后两个 dummy 对应的返回值。

另一个工程点是写操作前必须读状态。W25Q64 写入和擦除会进入 BUSY 状态，后续课程如果扩展页编程，不能只靠固定延时，应读状态寄存器 `0x05` 的 BUSY 位判断内部操作是否完成。

## 12. 运行现象

W25Q64 连接正确时，PC13 快速闪烁。连接错误、地址命令失败或读不到 `0xEF` 时，PC13 慢速闪烁。

逻辑分析仪上应看到：CS 拉低，MOSI 发送 `0x9F FF FF FF`，MISO 返回类似 `EF xx xx`。

## 13. 常见问题排查

### 13.1 LED 慢闪

说明 Manufacturer ID 不是 `0xEF`。先查供电、GND、CS、SCK、MISO、MOSI 接线。

### 13.2 读到 `0xFF`

常见是 MISO 悬空、CS 没选中、Flash 没供电或 DO 没接到 PA6。

### 13.3 读到 `0x00`

可能 MISO 被拉低、接线短路、SPI 模式或时钟异常。检查 PA6 是否被配置为输入。

### 13.4 SCK 有波形但 MISO 没数据

检查 CS 是否在整个 `0x9F + 3 dummy` 期间保持低电平，W25Q64 是否支持当前供电和引脚连接。

### 13.5 HAL 版现象不稳定

给 `HAL_SPI_TransmitReceive()` 加返回值检查，并降低 SPI 分频试试，例如改为 /16 或 /32，排除线长和模块速度问题。

## 14. 本课最核心的结论

1. 真实 SPI 从机访问必须用 CS 定义事务边界。
2. `0x9F` 是 W25Q64 的 JEDEC ID 命令，不是 STM32 的配置值。
3. SPI 读数据必须由主机继续发送 dummy byte 来提供时钟。
4. `0xEF` 是 Winbond 常见厂商 ID，可用于最小通信验证。
5. 拉高 CS 前等待 `BSY=0`，避免提前结束事务。
6. HAL 简化一字节交换，但 CS 时序和命令协议仍要自己管理。

## 15. 建议你现在怎么读这节课

先复习第 30 课的 `spi1_transfer_byte()`，再看本课如何用 PA4 把多个字节交换包成一笔 Flash 命令。最后用逻辑分析仪确认 CS、MOSI、MISO 的时序。

## 16. 扩展练习

1. 保存并显示完整三个字节 JEDEC ID。
2. 降低 SPI 分频，比较读 ID 是否仍稳定。
3. 实现读取状态寄存器 `0x05`。
4. 在 HAL 版 `spi_transfer()` 中检查返回值，失败时点亮 LED。

## 17. 下一课预告

- 上一课：[30_spi_basic](../30_spi_basic/README.md)
- 下一课：[32_can_basic](../32_can_basic/README.md)
