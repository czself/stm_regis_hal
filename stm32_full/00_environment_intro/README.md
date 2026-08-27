# 第 0 课：环境搭建与第一个工程

## 1. 本课到底在学什么

这节课表面上是在做三件事：

- 用 PlatformIO 编译一个 STM32F103 工程
- 用 ST-Link 把固件下载到 BluePill 开发板
- 观察板载 `PC13` LED 周期闪烁

真正学习的是 STM32 入门前必须跑通的完整工程链路：

```text
platformio.ini 描述工程
  -> PlatformIO 根据 board/framework/build_flags 准备编译环境
  -> C 源文件和启动文件被编译、链接成 .elf/.bin
  -> ST-Link 通过 SWD 把固件写入 Flash
  -> STM32 复位后从 Flash 取指执行
  -> 程序配置系统时钟和 GPIOC
  -> PC13 输出电平变化
  -> 板载 LED 闪烁
```

这一课不是正式讲 GPIO，也不是正式讲时钟树。它的作用是先确认电脑、工具链、工程配置、下载器、芯片、启动文件、CMSIS/HAL 头文件和板载 LED 能连成闭环。后面每一课都默认这个闭环已经可靠。

如果本课没跑通，后面看到任何外设“不工作”，你都很难判断是代码问题、工程配置问题、下载问题，还是硬件接线问题。

## 2. 本课学习目标

学完本课，你应该能回答：

1. `platformio.ini` 为什么是工程链路的第一步？
2. `platform = ststm32`、`board = genericSTM32F103C8`、`framework = stm32cube` 分别影响什么？
3. `upload_protocol = stlink` 为什么必须和你的下载器一致？
4. `build_flags = -D HSE_VALUE=8000000U` 为什么会影响时钟配置理解？
5. `reg/` 版本为什么只包含 `stm32f1xx.h` 也能访问 `RCC`、`FLASH`、`GPIOC`？
6. `hal/` 版本为什么必须先调用 `HAL_Init()`？
7. 固件从电脑到 LED 闪烁，中间经过哪些硬件和软件层？
8. 编译失败、下载失败、下载成功但 LED 不闪，排查顺序有什么不同？

## 3. 本课目录结构

```text
00_environment_intro/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 是寄存器版。它包含 `stm32f1xx.h`，通过 CMSIS 提供的寄存器结构体名字直接访问 `FLASH`、`RCC`、`GPIOC`。

`hal/` 是 HAL 版。它包含 `stm32f1xx_hal.h`，通过 `HAL_Init()`、`HAL_RCC_OscConfig()`、`HAL_GPIO_Init()` 等 API 表达同样的硬件配置意图。

两个版本都使用 STM32Cube 框架，是为了让 PlatformIO 提供启动文件、链接脚本、CMSIS 设备头和 HAL 库。区别不在工程框架，而在 `main.c` 里你是直接写寄存器，还是通过 HAL API 间接写寄存器。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill 或等价最小系统板
- 下载器：ST-Link
- 下载接口：SWDIO、SWCLK、GND，必要时接 3.3V 供电或目标电压参考
- 板载 LED：常见 BluePill 接在 `PC13`
- 时钟假设：板上有 8MHz HSE，代码把系统时钟配置到 72MHz

BluePill 常见板载 LED 是低电平点亮：

```text
PC13 = 0 -> LED 亮
PC13 = 1 -> LED 灭
```

所以后面你看到代码初始化时把 `PC13` 输出高电平，不是为了点亮，而是为了先让 LED 熄灭。

## 5. 先建立一个最基本的脑图

本课完整链路如下：

```text
platformio.ini
  -> platform = ststm32，选择 STM32 平台包
  -> board = genericSTM32F103C8，选择 F103C8 的板级/链接配置
  -> framework = stm32cube，提供启动文件、CMSIS、HAL
  -> build_flags 指定 HSE_VALUE = 8MHz
  -> pio run 编译、汇编、链接，生成固件
  -> pio run -t upload 调用 ST-Link 下载
  -> ST-Link 通过 SWD 写入 STM32 Flash
  -> 复位后 CPU 从 Flash 地址取指
  -> system_clock_72mhz_init() 配置 HSE/PLL/SYSCLK
  -> pc13_led_init() 打开 GPIOC 时钟并配置 PC13 输出
  -> 主循环翻转 PC13
  -> 板载 LED 闪烁
```

这条链路最关键的两个判断是：

1. 程序有没有成功到达芯片：看编译和下载是否成功。
2. 程序有没有在芯片上正常运行：看 PC13 是否按预期闪烁。

如果第一步没过，不要查 GPIO；如果下载都成功但 LED 不闪，再查时钟、GPIO、LED 极性和主循环。

## 6. 先认识本课里出现的核心名词

### 6.1 `platformio.ini` 是什么

`platformio.ini` 全称可以理解为：

- PlatformIO project configuration file

中文通常叫：

- PlatformIO 工程配置文件

它属于工程配置层，不属于 STM32 硬件外设。它的作用是告诉 PlatformIO：这个目录里的代码要按什么平台、什么芯片、什么框架、什么下载方式来处理。

在本课里，它处于整条链路的第一步。`platformio.ini` 配错时，错误可能发生在很早的位置：头文件找不到、链接脚本不对、Flash/RAM 大小不匹配，或者下载工具调用错。

你可以先把它理解成：

- 电脑端编译和下载流程的说明书。

### 6.2 `platform = ststm32` 是什么

`platform` 在 PlatformIO 里表示目标平台包。

`ststm32` 的意思是：

- 使用 PlatformIO 针对 ST STM32 系列准备的平台支持包

它属于电脑工具链层，控制的是编译器、调试/下载工具、板级配置和框架包的来源。

在本课里，如果没有 `platform = ststm32`，PlatformIO 就不知道这是 STM32 工程，也不会按 STM32 的方式准备启动文件、链接脚本和下载工具。

写错时，常见现象是 PlatformIO 找不到板子、找不到框架，或者按完全错误的 MCU 平台去编译。

### 6.3 `board = genericSTM32F103C8` 是什么

`board` 全称是：

- PlatformIO board identifier

中文通常叫：

- 开发板/芯片型号标识

它属于工程配置层。它告诉 PlatformIO 当前目标接近 STM32F103C8 这种资源配置：Flash、RAM、芯片系列、默认链接脚本等都要按这个目标准备。

本课实际 `platformio.ini` 使用的是：

```ini
board = genericSTM32F103C8
```

这和口语里的 BluePill 不矛盾。BluePill 是常见开发板名字，`genericSTM32F103C8` 是 PlatformIO 里更通用的 F103C8 目标标识。

如果 `board` 选错，可能出现：

- 编译能过，但链接地址或 Flash 大小不匹配
- 下载成功，但程序运行异常
- 使用了不适合当前芯片的启动文件或宏定义

### 6.4 `framework = stm32cube` 是什么

`framework` 全称是：

- software framework

中文通常叫：

- 软件开发框架

它属于软件支撑层。`stm32cube` 会让 PlatformIO 引入 ST 的 STM32Cube 生态，包括启动文件、CMSIS 设备头文件和 HAL 库。

本课寄存器版虽然直接写寄存器，但仍然使用 `framework = stm32cube`，因为它也需要：

- 启动文件把 CPU 带到 `main()`
- 链接脚本安排 Flash/SRAM
- `stm32f1xx.h` 提供寄存器名字

HAL 版还额外需要 `stm32f1xx_hal.h` 和 HAL 函数实现。

如果 framework 配错，常见现象是头文件找不到、`main()` 无法链接、HAL API 未定义，或者启动流程不完整。

### 6.5 `upload_protocol = stlink` 是什么

`upload_protocol` 全称是：

- upload protocol

中文通常叫：

- 下载协议或烧录方式

它属于电脑到开发板的下载链路层，控制 PlatformIO 用哪种工具把固件写进 STM32。

本课写：

```ini
upload_protocol = stlink
```

意思是使用 ST-Link 下载器，通过 SWD 接口访问芯片 Flash。

如果你实际接的是 ST-Link，但这里写成别的协议，PlatformIO 会调用错误工具。反过来，如果这里写 `stlink`，但硬件没接 ST-Link 或驱动不可用，下载阶段会失败。

### 6.6 `build_flags = -D HSE_VALUE=8000000U` 是什么

`build_flags` 是传给编译器的额外编译选项。

本课使用：

```ini
build_flags =
  -D HSE_VALUE=8000000U
```

`-D` 表示在编译时定义一个宏。`HSE_VALUE=8000000U` 告诉库代码和你的工程：外部高速晶振 HSE 是 8MHz。

它属于编译配置层，但会影响你对硬件时钟的理解。HAL 或系统时钟相关代码可能会用 `HSE_VALUE` 计算频率。如果板子实际不是 8MHz，却仍写 8MHz，后面定时器、串口、延时计算就可能整体偏差。

### 6.7 `ST-Link` 是什么

`ST-Link` 全称是：

- STMicroelectronics debug and programming probe

中文通常叫：

- ST-Link 调试下载器

它属于电脑和芯片之间的硬件桥梁。它通过 SWD 协议连接 STM32，可以下载程序、复位芯片、单步调试、读取内存和外设寄存器。

在本课链路中，ST-Link 位于“固件已经生成”和“固件写入 Flash”之间。

如果 ST-Link 出问题，现象通常不是 LED 不闪，而是 upload 失败，例如找不到 target、连接超时、无法 halt 芯片。优先查 USB、驱动、SWDIO、SWCLK、GND 和目标板供电。

### 6.8 `Flash` 是什么

`Flash` 中文通常叫：

- 片内 Flash 程序存储器

它属于 STM32 芯片内部存储器层。它的作用是保存掉电不丢失的程序代码和常量数据。

本课下载时，ST-Link 把编译得到的固件写入 Flash。复位后，CPU 从 Flash 中的启动地址取指，经过启动文件初始化运行环境，最后进入 `main()`。

如果 Flash 没写成功，程序不会按你刚编译的代码运行；如果写入了错误固件，你可能看到旧现象或完全没有现象。

### 6.9 `CMSIS` 是什么

`CMSIS` 全称是：

- Cortex Microcontroller Software Interface Standard

中文通常叫：

- Cortex 微控制器软件接口标准

它属于 C 代码到硬件寄存器之间的抽象层。CMSIS 把芯片手册里的固定地址和寄存器布局定义成 C 语言结构体、宏和指针。

本课寄存器版包含：

```c
#include "stm32f1xx.h"
```

之后就能写：

```c
RCC->APB2ENR
GPIOC->CRH
FLASH->ACR
```

这些不是普通变量，而是 CMSIS 根据外设基地址定义出来的寄存器访问入口。

### 6.10 `HAL` 是什么

`HAL` 全称是：

- Hardware Abstraction Layer

中文通常叫：

- 硬件抽象层

它属于软件封装层。HAL 用结构体和 API 表达硬件配置意图，然后在内部写底层寄存器。

本课 HAL 版包含：

```c
#include "stm32f1xx_hal.h"
```

然后使用 `HAL_Init()`、`HAL_RCC_OscConfig()`、`HAL_GPIO_Init()` 等函数。

HAL 不是脱离硬件的另一套规则。比如 `HAL_GPIO_Init()` 最终仍然要配置 GPIO 的模式寄存器；`HAL_GPIO_TogglePin()` 最终仍然要改变 PC13 的输出状态。

### 6.11 `PC13` 是什么

`PC13` 可以拆成：

- `P`：Port，引脚端口
- `C`：GPIOC 端口
- `13`：第 13 号引脚

中文通常叫：

- GPIOC 第 13 号引脚

它属于 GPIO 引脚层。本课用它作为程序运行现象的输出点，因为 BluePill 常见板载 LED 接在 PC13。

在代码里，寄存器版通过 `GPIOC->CRH` 配置 PC13，通过 `BSRR/BRR` 改变 PC13 电平；HAL 版通过 `GPIO_PIN_13` 和 `GPIOC` 表达同一个引脚。

如果板子 LED 不在 PC13，或者 LED 极性和常见 BluePill 不同，你会看到下载成功但 LED 不按文档现象闪烁。

### 6.12 `HAL_Init()` 是什么

`HAL_Init()` 是 HAL 工程的基础初始化入口。

它属于 HAL 软件层，不是 STM32 某个外设寄存器。它会初始化 HAL 内部状态，并配置 HAL 默认时间基准。后面的 `HAL_Delay()` 依赖这个时间基准继续工作。

在本课 HAL 版里，它必须在其他 HAL API 前调用：

```c
HAL_Init();
system_clock_72mhz_init();
pc13_led_init();
```

如果省略它，部分 HAL 函数可能仍然看似能跑，但 `HAL_Delay()`、超时等待和 HAL 内部状态管理都可能不可靠。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 先看完整逻辑

寄存器版主流程是：

```c
int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    while (1) {
        pc13_toggle();
        delay_cycles(3600000U);
    }
}
```

顺序不能随便换。先配时钟，是为了让 CPU 和后续延时有明确频率基础；再配 PC13，是为了让 GPIOC 具备输出能力；最后主循环翻转 LED，才产生可见现象。

### 7.2 `#include "stm32f1xx.h"` 为什么足够

寄存器版只包含：

```c
#include "stm32f1xx.h"
```

这个头文件来自 CMSIS/STM32Cube 设备支持包。它提供了：

- `RCC`、`FLASH`、`GPIOC` 这些外设结构体指针
- `RCC_CR_HSEON`、`GPIO_CRH_MODE13` 这些位掩码宏
- `uint32_t` 等常用类型依赖
- `__NOP()` 这类内核辅助函数

所以寄存器版不需要 HAL API，也能访问硬件。它不是绕过工程框架，而是使用 CMSIS 这层更贴近寄存器的定义。

### 7.3 `system_clock_72mhz_init()` 为什么先写 `FLASH->ACR`

函数第一句是：

```c
FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
```

`FLASH->ACR` 属于 Flash 接口寄存器。它控制 CPU 从 Flash 取指时的预取和等待周期。

本课后面要把 CPU 跑到 72MHz。频率提高后，Flash 访问速度跟不上 CPU 时钟，所以必须先设置 `LATENCY_2`，让 Flash 读取多等待几个周期。

如果先切 72MHz 再配 Flash 等待周期，程序可能在高速取指时不稳定，严重时直接跑飞。

### 7.4 为什么要打开并等待 HSE

代码执行：

```c
RCC->CR |= RCC_CR_HSEON;
while ((RCC->CR & RCC_CR_HSERDY) == 0U) {
}
```

`RCC->CR` 是 RCC 时钟控制寄存器。`HSEON` 是外部高速时钟开关，`HSERDY` 是外部高速时钟稳定标志。

本课使用板载 8MHz HSE 作为 PLL 输入。如果只打开 HSE 但不等待 `HSERDY`，后面 PLL 可能拿到还没稳定的输入时钟。

如果板上没有 8MHz 晶振、晶振损坏或焊接问题，程序会卡在这个 `while`，LED 不会开始闪烁。

### 7.5 为什么配置 `RCC->CFGR` 前先清相关位

代码先清掉一组时钟配置位：

```c
RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 |
               RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL |
               RCC_CFGR_SW);
```

`RCC->CFGR` 是时钟配置寄存器，里面同时包含 AHB 分频、APB 分频、PLL 来源、PLL 倍频、系统时钟选择等字段。

先清位的原因是：这些字段不是单 bit 开关，而是多 bit 选择项。旧值如果残留，后面 `|=` 新值可能变成错误组合。

例如 PLL 倍频字段如果不先清，`PLLMULL9` 可能和旧倍频位叠在一起，导致倍频不是 x9。

### 7.6 `RCC->CFGR` 里每个分频和 PLL 选择是什么意思

代码设置：

```c
RCC->CFGR |= RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV2 |
             RCC_CFGR_PPRE2_DIV1 | RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9;
```

这一步把时钟树路线定下来：

- `HPRE_DIV1`：AHB 不分频，HCLK = SYSCLK
- `PPRE1_DIV2`：APB1 二分频，72MHz 下得到 36MHz
- `PPRE2_DIV1`：APB2 不分频，仍为 72MHz
- `PLLSRC`：PLL 输入选择 HSE
- `PLLMULL9`：8MHz 乘 9 得到 72MHz

APB1 要分频，是因为 STM32F103 的 APB1 最高 36MHz。APB2 可以保持 72MHz，所以 GPIOA/GPIOC 等 APB2 外设仍能正常工作。

### 7.7 为什么要打开 PLL 并等待 `PLLRDY`

代码执行：

```c
RCC->CR |= RCC_CR_PLLON;
while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {
}
```

`PLLON` 是 PLL 开关，`PLLRDY` 是 PLL 输出稳定标志。

PLL 从 HSE 得到输入后，需要一点时间锁定到目标频率。软件必须等 `PLLRDY` 置位，才能把系统时钟切到 PLL。

如果不等，CPU 可能切到未稳定时钟，后面所有外设频率和程序执行都不可靠。

### 7.8 为什么最后切换 SYSCLK 并等待 `SWS`

代码执行：

```c
RCC->CFGR |= RCC_CFGR_SW_PLL;
while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
}
```

`SW` 是系统时钟源选择字段，`SWS` 是系统时钟源状态字段。

设置 `SW_PLL` 只是提出“我要用 PLL 当 SYSCLK”。硬件完成切换后，`SWS` 才会显示当前系统时钟确实来自 PLL。

如果不等 `SWS`，后面的代码可能以为已经是 72MHz，但 CPU 实际仍在旧时钟下运行，延时和外设计算都会混乱。

### 7.9 `pc13_led_init()` 为什么先开 GPIOC 时钟

代码执行：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
```

GPIOC 挂在 APB2 总线上。`IOPCEN` 是 GPIOC 外设时钟使能位。

STM32 的外设默认不会都开着，这是为了省电。不开 GPIOC 时钟，就算你写 `GPIOC->CRH`，GPIOC 硬件也没有正常工作条件。

这一步对应第 5 章链路中的“配置 GPIOC/PC13”前置条件。

### 7.10 为什么 PC13 配在 `CRH`，并且要先清再设

代码执行：

```c
GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
GPIOC->CRH |= GPIO_CRH_MODE13_1;
```

F103 的 GPIO 每个引脚占 4 bit：

- `MODE[1:0]` 控制输入/输出和输出速度
- `CNF[1:0]` 控制推挽、开漏、复用、输入类型

`CRL` 管 0 到 7 号引脚，`CRH` 管 8 到 15 号引脚。PC13 是 13 号，所以必须配置 `GPIOC->CRH`。

清位后再设置，可以避免旧模式残留。最终配置是：

- `MODE13 = 10`：输出模式，2MHz
- `CNF13 = 00`：通用推挽输出

如果配成输入，写 `BSRR/BRR` 不会按预期驱动 LED；如果改错到 `CRL`，PC13 根本没被配置。

### 7.11 为什么初始化后写 `BSRR`

代码执行：

```c
GPIOC->BSRR = GPIO_BSRR_BS13;
```

`BSRR` 低 16 位写 1 表示把对应引脚置高。这里把 PC13 置高。

因为 BluePill 板载 LED 常见是低电平点亮，所以 PC13 置高表示先让 LED 熄灭。这样程序启动后的初始状态更清楚。

### 7.12 `pc13_toggle()` 为什么读 `ODR` 再写 `BRR/BSRR`

`pc13_toggle()` 先读：

```c
GPIOC->ODR & GPIO_ODR_ODR13
```

`ODR` 是输出数据寄存器，可以反映当前软件输出状态。

如果当前 PC13 为高，代码写：

```c
GPIOC->BRR = GPIO_BRR_BR13;
```

`BRR` 写 1 会把 PC13 拉低，LED 点亮。

如果当前 PC13 为低，代码写：

```c
GPIOC->BSRR = GPIO_BSRR_BS13;
```

`BSRR` 写 1 会把 PC13 拉高，LED 熄灭。

这种写法只影响 PC13，不需要读-改-写整个端口，比较安全。

### 7.13 `delay_cycles()` 为什么只是临时延时

代码是：

```c
static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- != 0U) {
        __NOP();
    }
}
```

`volatile` 防止编译器把这个空循环优化掉。`__NOP()` 是一条空操作指令，让循环确实消耗 CPU 时间。

但这个延时不精确。它受 CPU 频率、编译优化、循环指令开销影响。本课只用它证明环境跑通；后面 SysTick 和定时器课程会用更正规的时间基准。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 HAL 版和寄存器版的本质差异

HAL 版主流程是：

```c
HAL_Init();
system_clock_72mhz_init();
pc13_led_init();
while (1) {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    HAL_Delay(500);
}
```

它和寄存器版做的是同一件事：准备时钟、配置 PC13、周期翻转 LED。区别是 HAL 版用结构体字段表达配置意图，再由 HAL 函数写底层寄存器。

### 8.2 `HAL_Init()` 为什么必须在最前面

`HAL_Init()` 是 HAL 库基础初始化入口。它会初始化 HAL 内部状态，并配置 HAL 默认 Tick。

本课后面用到：

```c
HAL_Delay(500);
```

`HAL_Delay()` 依赖 HAL Tick 递增。如果没有先 `HAL_Init()`，HAL 的时间基准和内部状态就没有被正确准备。

### 8.3 `RCC_OscInitTypeDef osc = {0};` 为什么先清零

HAL 版定义：

```c
RCC_OscInitTypeDef osc = {0};
```

`RCC_OscInitTypeDef` 是 HAL 的振荡器配置结构体，用来描述 HSE、HSI、PLL 等时钟源怎么配置。

`= {0}` 的作用是把所有字段先清零。这样后面只填写本课需要的字段，避免未初始化字段带着随机值影响 HAL 判断。

### 8.4 `osc` 结构体每个字段是什么意思

本课填写：

```c
osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
osc.HSEState = RCC_HSE_ON;
osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
osc.PLL.PLLState = RCC_PLL_ON;
osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
osc.PLL.PLLMUL = RCC_PLL_MUL9;
```

它们分别对应寄存器版的时钟源和 PLL 配置：

- `OscillatorType`：声明要配置 HSE。
- `HSEState`：打开 HSE，对应 `RCC_CR_HSEON`。
- `HSEPredivValue`：HSE 不预分频。
- `PLLState`：打开 PLL，对应 `RCC_CR_PLLON`。
- `PLLSource`：PLL 输入选择 HSE，对应 `RCC_CFGR_PLLSRC`。
- `PLLMUL`：PLL 倍频 x9，对应 `RCC_CFGR_PLLMULL9`。

字段写错时，系统时钟可能不是 72MHz，甚至 HAL 等待时钟稳定失败后卡在错误处理循环。

### 8.5 `HAL_RCC_OscConfig()` 底层做了什么

调用：

```c
HAL_RCC_OscConfig(&osc)
```

它会根据 `osc` 结构体配置 RCC 振荡器相关寄存器，大致对应寄存器版这些动作：

- 打开 HSE
- 等待 HSE ready
- 配置 PLL 输入和倍频
- 打开 PLL
- 等待 PLL ready

它只负责“时钟源和 PLL”，还没有把 SYSCLK 切到 PLL。切换系统时钟由下一步 `HAL_RCC_ClockConfig()` 完成。

### 8.6 `RCC_ClkInitTypeDef clk = {0};` 是什么

`RCC_ClkInitTypeDef` 是 HAL 的总线时钟配置结构体，用来描述 SYSCLK、HCLK、PCLK1、PCLK2 的来源和分频。

它对应寄存器版 `RCC->CFGR` 中的系统时钟选择和 AHB/APB 分频字段。

同样先 `{0}`，是为了避免未初始化字段影响 HAL 配置。

### 8.7 `clk` 结构体每个字段是什么意思

本课填写：

```c
clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
clk.APB1CLKDivider = RCC_HCLK_DIV2;
clk.APB2CLKDivider = RCC_HCLK_DIV1;
```

含义是：

- `ClockType`：声明这次要配置 SYSCLK、HCLK、PCLK1、PCLK2。
- `SYSCLKSource`：系统时钟选择 PLL。
- `AHBCLKDivider`：AHB 不分频。
- `APB1CLKDivider`：APB1 二分频，得到 36MHz。
- `APB2CLKDivider`：APB2 不分频，保持 72MHz。

这和寄存器版 `HPRE_DIV1`、`PPRE1_DIV2`、`PPRE2_DIV1`、`SW_PLL` 对应。

### 8.8 `HAL_RCC_ClockConfig()` 为什么还传 `FLASH_LATENCY_2`

调用：

```c
HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2)
```

第一个参数是总线和系统时钟配置，第二个参数是 Flash 等待周期。

这对应寄存器版的两类动作：

- 配置 `RCC->CFGR` 的 SYSCLK/AHB/APB 字段
- 配置 `FLASH->ACR` 的等待周期

72MHz 下必须传 `FLASH_LATENCY_2`。如果等待周期太低，切换高速后可能不稳定。

### 8.9 `GPIO_InitTypeDef gpio = {0};` 是什么

`GPIO_InitTypeDef` 是 HAL 的 GPIO 初始化结构体。

本课用它描述：

- 配哪个引脚
- 配成什么模式
- 是否上下拉
- 输出速度是多少

`= {0}` 仍然是为了避免未初始化字段残留。

### 8.10 `__HAL_RCC_GPIOC_CLK_ENABLE()` 对应哪一步

HAL 版执行：

```c
__HAL_RCC_GPIOC_CLK_ENABLE();
```

这是一个宏，本质对应寄存器版：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
```

它属于开 GPIOC 外设时钟这一步。没有它，后面的 `HAL_GPIO_Init(GPIOC, &gpio)` 无法可靠配置 GPIOC 硬件。

### 8.11 `gpio` 每个字段是什么意思

本课填写：

```c
gpio.Pin = GPIO_PIN_13;
gpio.Mode = GPIO_MODE_OUTPUT_PP;
gpio.Pull = GPIO_NOPULL;
gpio.Speed = GPIO_SPEED_FREQ_LOW;
```

含义是：

- `Pin = GPIO_PIN_13`：选择 PC13。
- `Mode = GPIO_MODE_OUTPUT_PP`：通用推挽输出，对应寄存器版 `MODE13 = 输出`、`CNF13 = 00`。
- `Pull = GPIO_NOPULL`：输出模式下不需要内部上下拉。
- `Speed = GPIO_SPEED_FREQ_LOW`：LED 闪烁不需要高速输出。

字段写错时，可能出现 LED 不亮、引脚没有输出、或者输出模式不符合预期。

### 8.12 `HAL_GPIO_Init()` 底层做什么

调用：

```c
HAL_GPIO_Init(GPIOC, &gpio);
```

它会根据 `gpio.Pin` 判断要配置 13 号引脚，根据端口 `GPIOC` 判断目标是 GPIOC，然后把 `Mode/Pull/Speed` 转换成 F103 GPIO 配置寄存器的 bit。

对于 PC13，它底层最终会落到 `GPIOC->CRH` 对应的 `MODE13/CNF13` 字段。

### 8.13 `HAL_GPIO_WritePin()` 为什么先写 SET

初始化末尾：

```c
HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
```

它把 PC13 输出高电平。由于 BluePill LED 常见低电平点亮，所以这一步让 LED 初始熄灭。

底层相当于写 GPIO 输出相关寄存器，把 PC13 置高。

### 8.14 `HAL_GPIO_TogglePin()` 和 `HAL_Delay()` 如何形成闪烁

主循环里：

```c
HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
HAL_Delay(500);
```

`HAL_GPIO_TogglePin()` 改变 PC13 当前输出状态。`HAL_Delay(500)` 阻塞等待约 500ms。

如果 `SysTick_Handler()` 没有正确调用 `HAL_IncTick()`，HAL Tick 不增长，`HAL_Delay()` 就可能一直卡住。本课的工程环境需要保证 HAL Tick 正常工作。

## 9. 两个版本真正应该怎么学

### 9.1 为什么先学寄存器版

寄存器版把底层条件暴露出来：Flash 等待周期、HSE/PLL、GPIOC 时钟、PC13 模式、BSRR/BRR。它能训练你遇到问题时按硬件链路排查。

### 9.2 为什么再看 HAL 版

HAL 版更接近实际工程写法。它用结构体和 API 减少重复位操作，但每个字段最终仍然对应寄存器配置。

### 9.3 正确心智模型

以后看到 HAL API，要能自动翻译：

- `HAL_RCC_OscConfig()` -> 配 HSE 和 PLL
- `HAL_RCC_ClockConfig()` -> 切 SYSCLK 和配置总线分频
- `__HAL_RCC_GPIOC_CLK_ENABLE()` -> 开 GPIOC 时钟
- `HAL_GPIO_Init()` -> 配 GPIO 模式寄存器
- `HAL_GPIO_TogglePin()` -> 改变 GPIO 输出状态

## 10. 检验问题清单

1. **`platformio.ini` 为什么会影响工程能不能跑起来？**

   - **答**：它决定平台包、目标板、框架、下载协议和编译宏。写错后，可能在编译、链接、下载或运行阶段出问题。
2. **`board = genericSTM32F103C8` 和 BluePill 是什么关系？**

   - **答**：BluePill 是常见开发板名字，`genericSTM32F103C8` 是 PlatformIO 中针对 STM32F103C8 的通用目标配置。本课用它描述芯片资源和链接配置。
3. **为什么寄存器版只包含 `stm32f1xx.h` 就能写 `RCC->CR`？**

   - **答**：CMSIS 设备头文件已经把 RCC 的基地址、寄存器结构体和位掩码宏定义好了。`RCC->CR` 本质是访问固定地址上的 RCC 控制寄存器。
4. **为什么 72MHz 前要先配置 `FLASH->ACR`？**

   - **答**：CPU 从 Flash 取指，72MHz 下 Flash 需要等待周期。先配置 `LATENCY_2` 可以避免切高速后取指不稳定。
5. **为什么要等待 `HSERDY`、`PLLRDY`、`SWS`？**

   - **答**：`HSERDY` 表示 HSE 稳定，`PLLRDY` 表示 PLL 锁定，`SWS` 表示系统时钟源已经切换完成。不等待就可能基于未稳定或未切换的时钟继续运行。
6. **如果下载成功但 LED 不闪，第一步该查工程配置还是 GPIO？**

   - **答**：先确认程序是否卡在时钟初始化，例如 HSE 是否可用；再查 PC13 是否真是板载 LED、GPIOC 时钟是否打开、PC13 是否配置成输出、LED 极性是否低电平点亮。
7. **HAL 版的 `RCC_OscInitTypeDef` 和 `RCC_ClkInitTypeDef` 分别对应寄存器版哪部分？**

   - **答**：`RCC_OscInitTypeDef` 对应 HSE/PLL 这类振荡器配置，主要落到 `RCC->CR` 和 PLL 相关配置；`RCC_ClkInitTypeDef` 对应 SYSCLK/AHB/APB 分频和系统时钟切换，主要落到 `RCC->CFGR`。
8. **`HAL_Delay()` 卡住最可能和什么有关？**

   - **答**：它依赖 HAL Tick。若 `HAL_Init()` 没调用、SysTick 没正常中断、`SysTick_Handler()` 没有让 HAL Tick 递增，就可能一直等不到延时结束。

## 11. 工程实现步骤

### 11.1 需求分析

本课需求是跑通最小 STM32 工程闭环：

- 电脑能编译工程
- ST-Link 能下载固件
- STM32 能从 Flash 启动并进入 `main()`
- PC13 LED 能闪烁证明程序在运行

所需能力包括：PlatformIO 配置、STM32Cube/CMSIS 支撑、ST-Link 下载、系统时钟初始化、GPIO 输出。

### 11.2 硬件核查

先核查硬件，不要一开始就怀疑代码：

- 开发板是否确实是 STM32F103C8T6 或兼容目标
- ST-Link 是否接了 SWDIO、SWCLK、GND
- 目标板是否供电稳定
- 板上是否有 8MHz HSE
- 板载 LED 是否接在 PC13，且是否低电平点亮

其中最容易忽略的是 GND。ST-Link 和目标板没有共地，下载和调试都会不稳定。

### 11.3 寄存器实现路线

按这个顺序：

1. 配置 Flash 等待周期。因为后面要把 CPU 提到 72MHz。
2. 打开 HSE 并等待稳定。因为 PLL 需要稳定输入。
3. 配置 PLL 和总线分频。因为 SYSCLK、AHB、APB 都依赖这一步。
4. 打开 PLL 并等待锁定。因为不能切到未稳定 PLL。
5. 切换 SYSCLK 到 PLL 并等待确认。因为后续延时假设已经是目标频率。
6. 打开 GPIOC 时钟。因为 PC13 属于 GPIOC。
7. 配置 PC13 为推挽输出。因为 LED 需要由引脚输出驱动。
8. 主循环翻转 PC13。因为这是最终可观察现象。

任何一步出错，现象都不同：时钟步骤错，程序可能卡在初始化；GPIO 步骤错，程序运行但 LED 不受控；主循环错，LED 可能只保持一个状态。

### 11.4 HAL 实现路线

按这个顺序：

1. `HAL_Init()`：准备 HAL 基础状态和 Tick。
2. 填 `RCC_OscInitTypeDef` 并调用 `HAL_RCC_OscConfig()`：对应寄存器版 HSE/PLL 配置。
3. 填 `RCC_ClkInitTypeDef` 并调用 `HAL_RCC_ClockConfig()`：对应 SYSCLK 和总线分频配置。
4. `__HAL_RCC_GPIOC_CLK_ENABLE()`：对应打开 GPIOC 时钟。
5. 填 `GPIO_InitTypeDef` 并调用 `HAL_GPIO_Init()`：对应配置 PC13 输出模式。
6. `HAL_GPIO_TogglePin()` 加 `HAL_Delay()`：对应翻转引脚并等待。

HAL 版每个 API 都有顺序依赖。比如先 `HAL_GPIO_Init()` 再开 GPIOC 时钟就不合理；先用 `HAL_Delay()` 但没有可靠 HAL Tick，也会出问题。

### 11.5 工程思维

学习阶段先看寄存器版，是为了建立硬件链路直觉：时钟、总线、寄存器、引脚、电平。

工程阶段常用 HAL，是为了减少重复代码，让团队更容易维护。但使用 HAL 不代表不用懂寄存器。真正排错时，仍然要知道 HAL 字段最终控制的是哪类硬件行为。

长期维护时，最要关注的是工程配置和代码假设是否一致：芯片型号、HSE 频率、下载器、LED 引脚、外设时钟都不能只凭感觉写。

### 11.6 常见工程陷阱

- `board` 和实际芯片不匹配：可能编译通过但链接地址、Flash/RAM 大小或启动配置不对。
- `HSE_VALUE` 和板上晶振不一致：时钟计算、延时、串口波特率和定时器频率都会偏。
- ST-Link 只接 SWDIO/SWCLK 不接 GND：下载容易失败或连接不稳定。
- 程序卡在等待 HSE：说明外部晶振不可用或代码假设和板子不一致。
- LED 极性判断错：BluePill 常见低电平亮，高电平灭。

## 12. 运行现象

寄存器版和 HAL 版都应该看到 PC13 周期闪烁。

寄存器版使用空循环延时，闪烁周期只是大约值。HAL 版使用 `HAL_Delay(500)`，理论上每 500ms 翻转一次，完整亮灭周期约 1 秒。

如果 LED 一直亮或一直灭，要先区分：程序是否已经下载成功、是否卡在时钟初始化、PC13 是否真连 LED、LED 是否低电平点亮。

## 13. 常见问题排查

### 13.1 编译失败

现象是 `pio run` 没有生成固件，终端出现编译或链接错误。

优先按这个顺序查：

1. 当前目录是否是 `00_environment_intro/reg` 或 `00_environment_intro/hal`。
2. `platformio.ini` 是否存在。
3. `platform = ststm32`、`board = genericSTM32F103C8`、`framework = stm32cube` 是否写对。
4. 错误信息中是否提示头文件找不到，例如 `stm32f1xx.h` 或 `stm32f1xx_hal.h`。

最容易忽略的是在错误目录运行命令。

### 13.2 下载失败

现象是编译成功，但 `pio run -t upload` 连接不上目标或写入失败。

优先按这个顺序查：

1. ST-Link USB 是否被系统识别。
2. `upload_protocol` 是否是 `stlink`。
3. SWDIO、SWCLK、GND 是否接对。
4. 目标板是否供电。
5. 芯片是否被读保护或处于异常低功耗/复位状态。

最容易忽略的是 GND 没接，或者 ST-Link 只给了信号线没有共同参考电平。

### 13.3 下载成功但 LED 完全不闪

这种情况说明电脑到芯片的链路大概率已经过了，重点转向运行链路。

排查顺序：

1. 看程序是否卡在 `HSERDY` 或 `PLLRDY` 等待循环。
2. 确认板上是否有可用 8MHz HSE。
3. 确认板载 LED 是否接在 PC13。
4. 确认 `RCC_APB2ENR_IOPCEN` 是否打开 GPIOC 时钟。
5. 确认 PC13 是否配置在 `GPIOC->CRH`，不是 `CRL`。

### 13.4 LED 闪烁逻辑和预期相反

现象是你以为“写 1 应该亮”，但实际写 0 才亮。

这通常不是软件错误，而是 BluePill 的 LED 硬件接法导致低电平点亮。排查时先看原理图或直接用代码分别输出高低电平测试。

### 13.5 HAL 版卡在 `HAL_Delay()`

现象是 HAL 版可能只翻转一次，或者进入 `HAL_Delay()` 后不再继续。

优先检查：

1. `HAL_Init()` 是否在最前面调用。
2. SysTick 中断是否正常。
3. `SysTick_Handler()` 是否让 HAL Tick 递增。
4. 系统时钟配置后 HAL Tick 是否仍然匹配。

这类问题属于 HAL 时间基准问题，不是 PC13 GPIO 本身的问题。

## 14. 本课最核心的结论

1. `platformio.ini` 是电脑端工程链路的入口，决定平台、芯片、框架、下载方式和编译宏。
2. ST-Link 负责把固件通过 SWD 写入 STM32 Flash，下载失败时先查工具和接线。
3. STM32 复位后从 Flash 取指执行，启动文件完成基础准备后才进入 `main()`。
4. CMSIS 让 C 代码能用 `RCC->CR`、`GPIOC->CRH` 这种形式访问固定地址上的硬件寄存器。
5. GPIO 仍然遵守“先开时钟、再配模式、最后输出电平”的规则。
6. HAL 只是把寄存器操作包装成结构体和 API，排错时仍要能回到底层链路。

## 15. 建议你现在怎么读这节课

建议顺序：

1. 先看第 5 章，把电脑到 LED 闪烁的完整链路读顺。
2. 再打开两个 `platformio.ini`，确认配置项和第 6 章解释能对上。
3. 然后读寄存器版 `main.c`，重点看时钟配置和 PC13 初始化。
4. 再读 HAL 版 `main.c`，把每个 HAL 字段翻译成寄存器版动作。
5. 最后故意制造一个小错误，例如改错 `upload_protocol` 或注释 GPIOC 时钟，观察错误发生在哪一层。

能做到“看到现象就知道应该查电脑端、下载端、时钟端还是 GPIO 端”，这节课才算真的学透。

## 16. 扩展练习

1. 把寄存器版延时参数改大或改小，观察闪烁节奏变化。
2. 注释掉 `RCC_APB2ENR_IOPCEN`，确认下载成功但 LED 不受控。不亮了
3. 把 HAL 版 `HAL_GPIO_TogglePin()` 改成两次 `HAL_GPIO_WritePin()` 手动亮灭。
4. 在调试器里观察 `GPIOC->ODR`，确认它随 PC13 状态变化。
5. 临时把 `HSE_VALUE` 改错，记录哪些现象可能受影响。

## 17. 下一课预告

下一课进入 [01_led](../01_led/README.md)。

本课确认了环境链路能跑通。下一课会把“PC13 LED 为什么能亮灭”拆得更细：`RCC` 为什么要先开 GPIOC 时钟，`GPIOC->CRH` 为什么配置 PC13，`MODE/CNF` 怎么决定输出模式，`BSRR/BRR` 为什么能改变引脚电平。
