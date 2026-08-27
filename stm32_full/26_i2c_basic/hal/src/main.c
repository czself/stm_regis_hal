#include "stm32f1xx_hal.h"

/*
 * 本文件是"HAL版 I2C1 + AT24C02 基础实验"。
 *
 * 本课目标：
 * 1. 使用 HAL 初始化 I2C1 为 100kHz 标准模式主机
 * 2. 使用 HAL_I2C_Mem_Write / HAL_I2C_Mem_Read 访问 AT24C02
 * 3. 每次写 1 个字节并读回校验，再用 LED 给出结果反馈
 *
 * 与寄存器版的对应关系：
 *   HAL_I2C_Init         → I2C1->CR2.FREQ, CCR, TRISE, CR1.PE 等
 *   HAL_I2C_Mem_Write()  → at24c02_write_byte() 的完整状态机
 *   HAL_I2C_Mem_Read()   → at24c02_read_byte() 的完整状态机（含重复起始）
 *
 * 理解寄存器版有助于理解 HAL 函数内部在做什么。
 * 理解 HAL 版有助于在实际项目中快速开发。
 *
 * 为什么 I2C 必须用开漏而不是推挽：
 *   推挽输出的两个设备如果同时输出相反电平，会直接短路烧 IO。
 *   开漏输出只能拉低不能推高，高电平由上拉电阻提供，多个设备安全共享总线。
 *   HAL 中 GPIO_MODE_AF_OD 对应寄存器版 CNF=11 的复用开漏。
 *
 * 错误后果：
 * - I2C 引脚错配成 GPIO_MODE_AF_PP（推挽）：总线电平冲突，可能损坏硬件。
 * - 忘了外接上拉电阻：SDA/SCL 高电平无法形成，HAL_I2C_Mem_Write 返回 HAL_ERROR。
 * - ClockSpeed 填错（如填 400000 但 EEPROM 只支持 100kHz）：通信不稳定或失败。
 * - 写完 EEPROM 不等 10ms 立刻读：EEPROM 处于写周期中，HAL_I2C_Mem_Read 返回 HAL_ERROR。
 */

#define AT24C02_ADDR_7BIT 0x50U
/*
 * HAL 版地址说明：
 *   HAL 的 I2C 接口函数要求传入的地址是左移 1 位后的 8 位地址格式。
 *   即 7 位地址 0x50 左移 1 位 → 0xA0。
 *   HAL 内部在发送时会自动处理 R/W 位，所以传入的是纯地址左移值。
 *
 *   寄存器版中手动拼接 R/W 位：
 *     写：0xA0（0x50<<1 | 0）
 *     读：0xA1（0x50<<1 | 1）
 *   HAL 版中传入 0xA0，HAL 根据操作类型自动加 R/W 位。
 */
#define AT24C02_ADDR_HAL  (AT24C02_ADDR_7BIT << 1)  /* = 0xA0 */
#define EEPROM_MEM_ADDR   0x00U

static I2C_HandleTypeDef hi2c1;

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void led_on(void);
static void led_toggle(void);
static void i2c1_gpio_init(void);
static void i2c1_init(void);
static void error_handler(void);

/*
 * main —— 主函数
 *
 * 与寄存器版逻辑完全相同：
 *   写 EEPROM → 等 10ms → 读 EEPROM → 比较 → 控制 LED
 *
 * 但 HAL 将复杂的 I2C 状态机封装成了两个 API 调用：
 *   HAL_I2C_Mem_Write() 封装了写操作全部 6 步
 *   HAL_I2C_Mem_Read()  封装了读操作全部 11 步（含重复起始）
 */
int main(void)
{
    uint8_t tx_byte = 0xA5U;
    uint8_t rx_byte = 0U;

    HAL_Init();

    system_clock_72mhz_init();
    led_pc13_init();
    i2c1_gpio_init();
    i2c1_init();

    while (1) {
        /*
         * HAL_I2C_Mem_Write() —— 向带内存地址的 I2C 设备写数据
         *
         * 参数说明：
         *   1. I2C 句柄指针
         *   2. 设备地址（7 位地址左移 1 位，即 0xA0）
         *   3. 内存/寄存器地址（0x00）
         *   4. 内存地址长度（8 位）
         *   5. 数据缓冲区
         *   6. 数据长度（1 字节）
         *   7. 超时时间
         *
         * 内部实现对寄存器版 at24c02_write_byte() 的封装：
         *   START → 发送地址+写位 → 等待 ADDR → 清 ADDR
         *   → 发送内存地址 → 等待 TXE/BTF → 发送数据 → 等待 BTF → STOP
         */
        if (HAL_I2C_Mem_Write(&hi2c1,
                              AT24C02_ADDR_HAL,
                              EEPROM_MEM_ADDR,
                              I2C_MEMADD_SIZE_8BIT,
                              &tx_byte,
                              1U,
                              HAL_MAX_DELAY) != HAL_OK) {
            led_on();
            HAL_Delay(1000U);
            continue;
        }

        /* 等待 EEPROM 内部写周期结束 */
        HAL_Delay(10U);

        /*
         * HAL_I2C_Mem_Read() —— 从带内存地址的 I2C 设备读数据
         *
         * 内部实现对寄存器版 at24c02_read_byte() 的封装：
         *   [第 1 段] START → 地址+写位 → 清 ADDR → 发内存地址 → 等 BTF
         *   [第 2 段] RESTART → 地址+读位 → 清 ADDR → 收数据 → STOP
         *
         * 单字节接收时的 ACK/STOP 时序也由 HAL 自动处理。
         */
        if (HAL_I2C_Mem_Read(&hi2c1,
                             AT24C02_ADDR_HAL,
                             EEPROM_MEM_ADDR,
                             I2C_MEMADD_SIZE_8BIT,
                             &rx_byte,
                             1U,
                             HAL_MAX_DELAY) != HAL_OK) {
            led_on();
            HAL_Delay(1000U);
            continue;
        }

        if (rx_byte == tx_byte) {
            led_toggle();
        } else {
            led_on();
        }

        if (tx_byte == 0xA5U) {
            tx_byte = 0x3CU;
        } else {
            tx_byte = 0xA5U;
        }

        HAL_Delay(1000U);
    }
}

static void system_clock_72mhz_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        error_handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        error_handler();
    }
}

static void led_pc13_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_13;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}

static void led_on(void)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
}

static void led_toggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}

/*
 * i2c1_gpio_init —— HAL 版配置 I2C1 引脚
 *
 * PB6(SCL) + PB7(SDA) → GPIO_MODE_AF_OD（复用开漏输出）
 *
 * 注意：I2C 的复用模式不是 AF_PP（推挽），而是 AF_OD（开漏）。
 * 这是由 I2C 总线电气特性决定的。
 */
static void i2c1_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;         /* 复用开漏输出！不是 AF_PP */
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
}

/*
 * i2c1_init —— HAL 版 I2C1 初始化（100kHz 标准模式主机）
 *
 * 各配置项与寄存器版的对应关系：
 *
 * ClockSpeed = 100000          → CCR = 180（100kHz 标准模式）
 * DutyCycle = I2C_DUTYCYCLE_2  → 标准模式下固定为 2
 * OwnAddress1 = 0              → OAR1 本课不需要（不做从机）
 * AddressingMode = 7BIT        → 7 位地址模式
 * DualAddressMode = DISABLE    → 不使能双地址
 * GeneralCallMode = DISABLE    → 不响应广播呼叫
 * NoStretchMode = DISABLE      → 允许时钟拉伸
 */
static void i2c1_init(void)
{
    __HAL_RCC_I2C1_CLK_ENABLE();

    hi2c1.Instance = I2C1;

    /*
     * ClockSpeed：I2C 时钟频率（Hz）
     *   100000 = 100kHz 标准模式
     *   对应寄存器版：CCR = 180
     */
    hi2c1.Init.ClockSpeed = 100000U;

    /*
     * DutyCycle：时钟占空比
     *   标准模式下固定为 I2C_DUTYCYCLE_2
     *   快速模式（400kHz）下可以选择 16/9 以获得更高频率
     */
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;

    /*
     * OwnAddress1：本设备自己的 I2C 地址
     *   本课只做主机，不响应从机寻址，所以设 0。
     *   对应寄存器版：OAR1 中写入 ADDMODE
     */
    hi2c1.Init.OwnAddress1 = 0U;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0U;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    /*
     * HAL_I2C_Init() 内部做了什么？
     *   1. 如果定义了 MspInit 回调，调用它
     *   2. 写 CR2.FREQ = APB1 时钟频率（MHz）
     *   3. 写 CCR = 根据 ClockSpeed 和 DutyCycle 计算
     *   4. 写 TRISE
     *   5. 写 OAR1
     *   6. 设置 CR1.ACK = 1
     *   7. 设置 CR1.PE = 1（打开 I2C）
     *
     * 对应寄存器版 i2c1_init() 中的所有 7 步操作。
     */
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        error_handler();
    }
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}