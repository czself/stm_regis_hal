# 第 2 课：GPIO 按键输入

## 1. 本课到底在学什么

这节课表面上是在做：

- 按下 `PA0` 按键，点亮 `PC13` 板载 LED
- 松开 `PA0` 按键，熄灭 `PC13` 板载 LED

真正学习的是 STM32 GPIO 输入链路：

```text
按键接到 GND
  -> PA0 使用内部上拉
  -> 松开时 PA0 = 1，按下时 PA0 = 0
  -> CPU 读取 GPIOA->IDR bit0
  -> 根据读到的电平控制 PC13 输出
  -> LED 亮灭
```

上一课你已经知道“输出一个电平”要先开 GPIO 时钟、配置输出模式、再写输出寄存器。本课补上另一半：**输入引脚怎么配置，CPU 怎么读取外部电平**。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么 `PA0` 接按键到 GND 时，要启用内部上拉？
2. `GPIOA->CRL` 为什么负责配置 `PA0`？
3. `MODE0 = 00` 和 `CNF0 = 10` 合起来表示什么？
4. 为什么输入上拉/下拉模式下，还要设置 `ODR0 = 1`？
5. `GPIOA->IDR` 读取到的 bit0 为什么能表示按键状态？
6. 为什么本课里按下按键读到的是低电平？
7. HAL 版的 `GPIO_PULLUP` 对应寄存器版哪一步？
8. 如果按键不稳定或乱跳，优先查什么？

## 3. 本课目录结构

```text
02_gpio_key/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

`reg/` 用寄存器方式配置 `PA0` 输入和 `PC13` 输出。
`hal/` 用 HAL API 完成同一件事。

## 4. 实验硬件

- 开发板：STM32F103C8T6 BluePill
- 下载器：ST-Link
- LED：板载 `PC13`，常见为低电平点亮
- 按键：一端接 `PA0`，另一端接 `GND`
- 电平：使用 `PA0` 内部上拉，不需要外接上拉电阻

本课按键逻辑是：

```text
松开：PA0 被内部上拉到 1
按下：PA0 被按键短接到 GND，读到 0
```

## 5. 先建立一个最基本的脑图

```text
打开 GPIOA 时钟
  -> 配置 PA0 为输入上拉/下拉模式
  -> 设置 ODR0 = 1，选择内部上拉
  -> 打开 GPIOC 时钟
  -> 配置 PC13 为推挽输出
  -> 主循环读取 GPIOA->IDR bit0
  -> bit0 = 0 表示按下，拉低 PC13 点亮 LED
  -> bit0 = 1 表示松开，拉高 PC13 熄灭 LED
```

这条链路里最关键的是两点：

1. `PA0` 不能悬空，必须有明确默认电平。
2. F103 的“输入上拉/下拉”由 `CRL` 选择模式，再由 `ODR` 决定上拉还是下拉。

## 6. 先认识本课里出现的核心名词

### 6.1 `GPIOA` 是什么

`GPIOA` 全称是：

- General Purpose Input/Output Port A

中文通常叫：

- GPIO A 端口

它的作用是：

- 管理 `PA0` 到 `PA15` 这一组引脚。
- 通过配置寄存器决定引脚是输入、输出还是复用功能。

你可以先把它理解成：

- STM32 里负责 A 组引脚的硬件控制器。

在本课里，按键接在 `PA0`，所以必须先打开 `GPIOA` 时钟，再配置 `GPIOA->CRL`，最后读取 `GPIOA->IDR`。如果 GPIOA 时钟没开，PA0 配置不会可靠生效。

### 6.2 `GPIOA->CRL` 是什么

`CRL` 全称是：

- Configuration Register Low

中文通常叫：

- GPIO 低 8 位配置寄存器

它的作用是：

- 配置 `Px0` 到 `Px7` 的模式。
- 每个引脚占 4 bit：`MODE[1:0]` 和 `CNF[1:0]`。

你可以先把它理解成：

- 0 到 7 号引脚的模式设置表。

在本课里，`PA0` 是 0 号引脚，所以它在 `GPIOA->CRL`，不是 `CRH`。如果改错寄存器，比如去改 `CRH`，PA0 模式不会改变。

### 6.3 `MODE0` 是什么

`MODE0` 全称可以理解为：

- Pin 0 mode bits

中文通常叫：

- 0 号引脚模式位

它的作用是：

- 在 F103 GPIO 中决定该引脚是输入还是输出，以及输出速度。
- `MODE0 = 00` 表示输入模式。

你可以先把它理解成：

- 告诉 PA0 “你现在不是输出脚，而是输入脚”。

在本课里，PA0 用来读取按键，所以 `MODE0` 必须保持 `00`。如果误设成输出，按键按下时可能和 MCU 输出电平冲突。

### 6.4 `CNF0` 是什么

`CNF0` 全称可以理解为：

- Pin 0 configuration bits

中文通常叫：

- 0 号引脚配置位

它的作用是：

- 在输入模式下选择浮空输入、上拉/下拉输入、模拟输入等类型。
- 本课设置 `CNF0 = 10`，表示输入上拉/下拉模式。

你可以先把它理解成：

- 告诉 PA0 “输入时用哪种电气方式”。

在本课里，按键另一端接 GND，所以 PA0 松开时需要默认高电平，必须使用上拉输入。若配成浮空输入，松开时电平不确定，LED 可能乱闪。

### 6.5 `ODR0` 是什么

`ODR0` 全称是：

- Output Data Register bit 0

中文通常叫：

- 输出数据寄存器第 0 位

它的作用是：

- 在输出模式下决定 PA0 输出 0 还是 1。
- 在输入上拉/下拉模式下，决定使用上拉还是下拉。

你可以先把它理解成：

- F103 用来选择“上拉还是下拉”的开关。

在本课里，配置 `CNF0 = 10` 后，还要设置 `ODR0 = 1`，这样 PA0 才是内部上拉。如果 `ODR0 = 0`，PA0 会变成内部下拉，按键松开也读到 0，LED 逻辑就错了。

### 6.6 `GPIOA->IDR` 是什么

`IDR` 全称是：

- Input Data Register

中文通常叫：

- 输入数据寄存器

它的作用是：

- 反映 GPIO 引脚当前输入电平。
- `IDR0` 对应 `PA0`。

你可以先把它理解成：

- CPU 查看外部引脚电平的窗口。

在本课里，`GPIOA->IDR & GPIO_IDR_IDR0` 用来判断按键是否按下。读到 0 表示 PA0 被按键拉到 GND，读到 1 表示 PA0 被内部上拉保持高电平。

### 6.7 `HAL_GPIO_ReadPin` 是什么

`HAL_GPIO_ReadPin` 全称是：

- HAL GPIO Read Pin

中文通常叫：

- HAL GPIO 读取引脚函数

它的作用是：

- 读取指定 GPIO 引脚当前电平。
- 返回 `GPIO_PIN_SET` 或 `GPIO_PIN_RESET`。

你可以先把它理解成：

- HAL 版读取 `IDR` 的封装。

在本课 HAL 版里，`HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0)` 对应寄存器版读取 `GPIOA->IDR` bit0。参数端口或引脚写错，会读到错误引脚。

### 6.8 `HAL_GPIO_WritePin` 是什么

`HAL_GPIO_WritePin` 全称是：

- HAL GPIO Write Pin

中文通常叫：

- HAL GPIO 写引脚函数

它的作用是：

- 把指定输出引脚置高或置低。
- 底层会操作 GPIO 输出相关寄存器。

你可以先把它理解成：

- HAL 版控制 PC13 电平的函数。

在本课里，按键按下时 HAL 版写 `GPIO_PIN_RESET` 让 PC13 输出低电平，点亮 LED；松开时写 `GPIO_PIN_SET` 熄灭 LED。

### 6.9 `GPIO_PULLUP` 是什么

`GPIO_PULLUP` 全称是：

- GPIO pull-up configuration

中文通常叫：

- GPIO 内部上拉配置

它的作用是：

- 在 HAL 结构体中表示启用内部上拉。
- 底层对应 F103 的输入上拉/下拉模式和 ODR 选择。

你可以先把它理解成：

- HAL 版里一句话表达“松开时默认高电平”。

在本课 HAL 版里，`gpio.Pull = GPIO_PULLUP` 对应寄存器版 `CNF0 = 10` 加 `ODR0 = 1`。漏掉它，PA0 松开时可能悬空。

## 7. 寄存器版代码逐步讲解

寄存器版在 [reg/src/main.c](reg/src/main.c)。

### 7.1 先看完整逻辑

```c
led_pc13_init();
key_pa0_init();

while (1) {
    if (key_is_pressed()) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}
```

代码分三段：先初始化 LED 输出，再初始化按键输入，最后循环读取按键并控制 LED。

### 7.2 `led_pc13_init()` 做了什么

这一段和 LED 课一致：

1. `RCC->APB2ENR |= RCC_APB2ENR_IOPCEN` 打开 GPIOC 时钟。
2. 清除 `MODE13/CNF13`，避免旧模式残留。
3. 设置 `MODE13 = 10`，`CNF13 = 00`，把 PC13 配成 2MHz 推挽输出。
4. 写 `GPIOC->BSRR = GPIO_BSRR_BS13`，让 LED 初始熄灭。

如果 PC13 没配置成输出，后面写 `BRR/BSRR` 不会得到稳定 LED 现象。

### 7.3 `key_pa0_init()` 为什么分两步决定上拉

第一步打开 GPIOA 时钟：

```c
RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
```

第二步配置 PA0 模式：

```c
GPIOA->CRL &= ~(GPIO_CRL_MODE0 | GPIO_CRL_CNF0);
GPIOA->CRL |= GPIO_CRL_CNF0_1;
```

清零后，`MODE0 = 00`。再设置 `CNF0_1` 后，`CNF0 = 10`。这表示“输入上拉/下拉模式”。

第三步选择上拉：

```c
GPIOA->BSRR = GPIO_BSRR_BS0;
```

这等价于把 `ODR0` 置 1。F103 的规则是：输入上拉/下拉模式下，`ODR0 = 1` 选择上拉，`ODR0 = 0` 选择下拉。

### 7.4 `key_is_pressed()` 怎么读按键

```c
if ((GPIOA->IDR & GPIO_IDR_IDR0) == 0U) {
    return 1U;
}
```

`GPIO_IDR_IDR0` 是 `IDR` bit0 的掩码。按键按下时 PA0 被接到 GND，所以 bit0 为 0。

本课故意让函数返回“是否按下”，把低电平有效这个硬件细节藏在函数内部。主循环读起来就更接近业务逻辑。

### 7.5 主循环为什么按下时写 `BRR`

BluePill 常见 PC13 LED 是低电平点亮：

- 按键按下：`GPIOC->BRR = GPIO_BRR_BR13`，PC13 变低，LED 亮。
- 按键松开：`GPIOC->BSRR = GPIO_BSRR_BS13`，PC13 变高，LED 灭。

如果你的板子 LED 极性不同，现象会相反。

## 8. HAL 版代码逐步讲解

HAL 版在 [hal/src/main.c](hal/src/main.c)。

### 8.1 HAL 版和寄存器版的本质差异

HAL 版仍然做同样的事：配置 PC13 输出、配置 PA0 输入上拉、循环读取按键。区别是寄存器字段被 `GPIO_InitTypeDef` 和 HAL API 封装了。

### 8.2 `key_pa0_init()` 的 HAL 参数

```c
gpio.Pin = GPIO_PIN_0;
gpio.Mode = GPIO_MODE_INPUT;
gpio.Pull = GPIO_PULLUP;
gpio.Speed = GPIO_SPEED_FREQ_LOW;
HAL_GPIO_Init(GPIOA, &gpio);
```

这些字段含义是：

- `Pin`：选择 PA0。
- `Mode`：普通输入，对应寄存器版 `MODE0 = 00`。
- `Pull`：内部上拉，对应寄存器版输入上拉/下拉模式并选择上拉。
- `Speed`：输入模式下影响不大，但结构体字段仍要给出合理值。

调用前必须先 `__HAL_RCC_GPIOA_CLK_ENABLE()`，对应寄存器版打开 GPIOA 时钟。

### 8.3 `HAL_GPIO_ReadPin()` 对应哪一步

```c
HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0)
```

它对应寄存器版读取 `GPIOA->IDR & GPIO_IDR_IDR0`。返回 `GPIO_PIN_RESET` 时表示 PA0 为低电平，也就是按键按下。

### 8.4 `HAL_GPIO_WritePin()` 对应哪一步

```c
HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
```

它对应寄存器版写 `GPIOC->BRR` 或 `GPIOC->BSRR`。第三个参数决定输出低电平还是高电平。

## 9. 两个版本真正应该怎么学

### 9.1 先学寄存器版

寄存器版能看清 F103 的关键细节：输入上拉不是只配 `CNF`，还要通过 `ODR` 选择上拉或下拉。

### 9.2 再看 HAL 版

HAL 版把这些细节合并成 `GPIO_PULLUP`，工程上更方便。

### 9.3 正确心智模型

`GPIO_PULLUP` 不是新硬件能力，它只是 HAL 帮你表达了寄存器版的 `CNF0 = 10` 和 `ODR0 = 1`。

## 10. 检验问题清单

1. **为什么 PA0 松开时不会自动有确定电平？**

   - **答**：输入脚如果没有上拉或下拉就可能悬空，电平受噪声影响，所以要启用内部上拉。
2. **为什么本课按键按下读到 0？**

   - **答**：按键另一端接 GND，按下时 PA0 被直接拉低。
3. **`PA0` 为什么配置在 `GPIOA->CRL`？**

   - **答**：`CRL` 配置 0~7 号引脚，PA0 是 GPIOA 的 0 号引脚。
4. **`CNF0 = 10` 后为什么还要设置 `ODR0 = 1`？**

   - **答**：`CNF0 = 10` 只是选择上拉/下拉输入模式，`ODR0` 决定具体是上拉还是下拉。
5. **如果忘记内部上拉，会出现什么现象？**

   - **答**：按键松开时 PA0 悬空，LED 可能乱闪或状态不稳定。
6. **HAL 版的 `GPIO_PULLUP` 对应寄存器版哪几步？**

   - **答**：对应输入上拉/下拉模式，并把 ODR 对应位设置为上拉。
7. **如果 LED 现象反了，先查什么？**

   - **答**：先查 PC13 LED 是否低电平点亮，再查按键接法是否是按下接 GND。

## 11. 工程实现步骤

### 11.1 需求分析

本课要读取一个机械按键，并用它控制 LED。需要 GPIO 输入、内部上拉、GPIO 输出三部分。

### 11.2 硬件核查

确认 PA0 按键另一端接 GND，确认 PC13 是板载 LED，确认板子 GND 正常。

### 11.3 寄存器实现路线

1. 打开 GPIOC 时钟并配置 PC13 输出，用于显示结果。
2. 打开 GPIOA 时钟，否则 PA0 配置无效。
3. 清除 PA0 的 `MODE/CNF` 配置位，避免旧模式残留。
4. 设置 `CNF0 = 10`，进入输入上拉/下拉模式。
5. 设置 `ODR0 = 1`，选择内部上拉。
6. 循环读取 `IDR0`，根据按键状态写 PC13。

### 11.4 HAL 实现路线

1. 调用 `HAL_Init()`。
2. `__HAL_RCC_GPIOC_CLK_ENABLE()` 后配置 PC13 输出。
3. `__HAL_RCC_GPIOA_CLK_ENABLE()` 后配置 PA0 输入上拉。
4. 用 `HAL_GPIO_ReadPin()` 读取按键。
5. 用 `HAL_GPIO_WritePin()` 控制 LED。

### 11.5 工程思维

输入引脚一定要先解决“默认电平”问题。工程中常见做法是内部上拉配合按键接地，或者外部上拉配合更强抗干扰能力。

### 11.6 常见工程陷阱

- 按键没上拉：松开时电平乱跳。
- 按键接到 3.3V 但代码按接 GND 理解：逻辑完全相反。
- 忘记 GPIOA 时钟：PA0 配置不生效。
- 机械按键抖动：按一次可能产生多次变化，后续会学习消抖。

## 12. 运行现象

正常现象：

- 松开 PA0 按键：PC13 输出高电平，板载 LED 熄灭。
- 按下 PA0 按键：PC13 输出低电平，板载 LED 点亮。

如果 LED 相反，优先确认板载 LED 极性和按键接线方式。

## 13. 常见问题排查

### 13.1 松开按键时 LED 乱闪

优先检查 PA0 是否启用了上拉。寄存器版查 `CNF0` 和 `ODR0`，HAL 版查 `GPIO_PULLUP`。

### 13.2 按下没有反应

按顺序查：PA0 是否真的接到按键、按键另一端是否接 GND、GPIOA 时钟是否打开、代码是否读取 `GPIO_PIN_0`。

### 13.3 LED 亮灭反了

先确认 PC13 是否低电平点亮，再确认主循环里按下时写的是低电平还是高电平。

### 13.4 按一次像按了多次

这是机械按键抖动。当前课程先学习输入链路，后续可以用延时、定时器或状态机消抖。

## 14. 本课最核心的结论

1. 输入引脚必须有确定默认电平，否则会悬空。
2. F103 的输入上拉/下拉模式由 `CNF` 和 `ODR` 共同决定。
3. `IDR` 反映的是引脚当前输入电平。
4. 按键接 GND 时，按下通常是低电平有效。
5. HAL 的 `GPIO_PULLUP` 封装了底层输入上拉配置。

## 15. 建议你现在怎么读这节课

1. 先画出 PA0、按键、GND、内部上拉之间的电路关系。
2. 再看寄存器版 `key_pa0_init()`，重点看 `CNF0` 和 `ODR0`。
3. 然后看 HAL 版，把 `GPIO_PULLUP` 映射回寄存器版。
4. 最后自己解释为什么按下读 0、松开读 1。

## 16. 扩展练习

1. 把 PA0 改成内部下拉，并把按键另一端接 3.3V，观察逻辑变化。
2. 在主循环里加一个简单延时，观察按键抖动是否减轻。
3. 用调试器观察 `GPIOA->IDR` 的 bit0。
4. 把 LED 控制改成按一次翻转一次，思考为什么需要边沿检测。

## 17. 下一课预告

下一课进入 [03_clock_tree](../03_clock_tree/README.md)。

你已经学会了 GPIO 输出和输入。下一步要理解系统时钟，因为后面的延时、串口波特率、定时器频率都依赖时钟树。
