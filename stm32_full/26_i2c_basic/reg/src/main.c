#include "stm32f1xx.h"

/*
 * 本文件是"寄存器版 I2C1 + AT24C02 基础实验"。
 *
 * 本课目标：
 * 1. 使用 I2C1 以主机模式访问 AT24C02 EEPROM
 * 2. 每次向 EEPROM 地址 0x00 写 1 个字节，再读回校验
 * 3. 通过板载 LED 观察读写结果是否一致
 * 4. 建立对 START / ADDR / ACK / STOP / 重复起始 的直观认识
 *
 * I2C vs SPI vs UART 核心区别：
 *   UART：异步，2 线（TX/RX），点对点
 *   SPI：同步，4 线（SCK/MOSI/MISO/NSS），一主多从
 *   I2C：同步，2 线（SCL/SDA），多主多从（通过地址区分设备）
 *
 * I2C 总线特点：
 *   - 开漏输出 + 上拉电阻：任何设备都可以拉低总线，不会冲突
 *   - 地址机制：每个从设备有唯一 7 位地址，主机通过地址选择通信对象
 *   - 起始/停止条件：由主机产生，标志一次传输开始/结束
 *   - 应答（ACK/NACK）：每字节后接收方回复是否成功接收
 *
 * 为什么 I2C 必须用开漏而不是推挽：
 *   推挽输出的两个设备如果同时输出相反电平，会直接短路烧 IO。
 *   开漏输出只能拉低不能推高，高电平由上拉电阻提供，多个设备安全共享总线。
 *   如果错配成推挽输出（GPIO_CRL_CNF6=10 而非 11），轻则通信异常，重则烧 IO。
 *
 * AT24C02 EEPROM：
 *   容量 256 字节（2Kbit），I2C 接口，7 位地址 0x50（A0/A1/A2 接地时）。
 *   写入后需要内部编程时间（典型 5ms，最大 10ms），期间不响应 I2C 总线。
 *
 * 本课地址说明：
 *   7 位器件地址 = 0x50（1010000）
 *   写地址字节 = (0x50 << 1) | 0 = 0xA0
 *   读地址字节 = (0x50 << 1) | 1 = 0xA1
 *
 * 错误后果：
 * - I2C 引脚错配成推挽：总线电平冲突，可能损坏硬件。
 * - 忘了外接上拉电阻：SDA/SCL 高电平无法形成，START 条件无法产生，SB 永不置位。
 * - FREQ 填错（如填 72 而非 36）：CCR 计算基于错误时钟，SCL 实际频率偏移，通信失败。
 * - 清 ADDR 顺序错（只读 SR1 不读 SR2）：ADDR 不清除，状态机卡在地址阶段，后续数据无法收发。
 * - 写完 EEPROM 不等 10ms 立刻读：EEPROM 处于写周期中不响应，读操作收到 NACK，AF 置位。
 * - 单字节读时忘了关 ACK：主机收到字节后发 ACK，从机继续发送下一个字节，RXNE 再次置位后数据被覆盖。
 */

#define I2C_TIMEOUT_COUNT 100000U
#define AT24C02_ADDR_7BIT 0x50U                  /* 7 位器件地址 */
#define AT24C02_ADDR_WRITE ((AT24C02_ADDR_7BIT << 1) | 0U)  /* 0xA0 */
#define AT24C02_ADDR_READ  ((AT24C02_ADDR_7BIT << 1) | 1U)  /* 0xA1 */
#define EEPROM_MEM_ADDR    0x00U

static volatile uint32_t g_ms_ticks = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_on(void);
static void led_off(void);
static void led_toggle(void);
static void i2c1_gpio_init(void);
static void i2c1_init(void);
static uint8_t i2c1_wait_sr1_set(uint32_t flag_mask);
static uint8_t i2c1_wait_bus_free(void);
static void i2c1_clear_addr_flag(void);
static uint8_t i2c1_send_start(void);
static void i2c1_send_stop(void);
static uint8_t i2c1_send_address(uint8_t address_byte);
static uint8_t at24c02_write_byte(uint8_t mem_addr, uint8_t data_byte);
static uint8_t at24c02_read_byte(uint8_t mem_addr, uint8_t *data_byte);

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

static void system_clock_72mhz_init(void)
{
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0U) {
    }

    RCC->CFGR &= ~(RCC_CFGR_HPRE |
                   RCC_CFGR_PPRE1 |
                   RCC_CFGR_PPRE2 |
                   RCC_CFGR_PLLSRC |
                   RCC_CFGR_PLLXTPRE |
                   RCC_CFGR_PLLMULL |
                   RCC_CFGR_SW);

    RCC->CFGR |= RCC_CFGR_HPRE_DIV1;
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;
    RCC->CFGR |= RCC_CFGR_PPRE2_DIV1;
    RCC->CFGR |= RCC_CFGR_PLLSRC;
    RCC->CFGR |= RCC_CFGR_PLLMULL9;

    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {
    }

    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void systick_init(void)
{
    SysTick->LOAD = 72000U - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms_ticks;
    while ((g_ms_ticks - start) < ms) {
    }
}

static void led_pc13_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_on(void)
{
    GPIOC->BRR = GPIO_BRR_BR13;
}

static void led_off(void)
{
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        led_on();
    } else {
        led_off();
    }
}

/*
 * i2c1_gpio_init —— 配置 I2C1 引脚
 *
 * I2C1 的默认引脚映射：
 *   PB6 → I2C1_SCL（串行时钟）
 *   PB7 → I2C1_SDA（串行数据）
 *
 * 为什么用"复用开漏输出"（不是复用推挽）？
 *   这是 I2C 总线电气特性的核心要求。
 *   I2C 是多主总线，多个设备可以共享 SCL 和 SDA。
 *   如果使用推挽输出，两个设备同时输出相反电平会短路。
 *
 *   开漏输出意味着：
 *   - 设备只能将引脚拉低（输出 0）
 *   - 拉高（输出 1）依靠外部上拉电阻
 *   - 这样多个设备可以安全地"线与"——任何设备拉低，总线就是低
 *
 *   外部必须接上拉电阻（典型值 4.7kΩ）到 3.3V。
 *   很多现成的 EEPROM 模块已自带上拉电阻。
 */
static void i2c1_gpio_init(void)
{
    /*
     * 第 1 步：开启 GPIOB、AFIO、I2C1 时钟
     *
     * 注意：I2C1 挂在 APB1 总线上（不是 APB2！）。
     * USART1、SPI1、ADC1 在 APB2（PCLK2=72MHz）
     * I2C1、I2C2、USART2/3、TIM 在 APB1（PCLK1=36MHz）
     * 这个区别对后续配置 CCR 和 TRISE 的公式计算至关重要。
     */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    /*
     * 第 2 步：配置 PB6(SCL) 和 PB7(SDA) 为复用开漏输出
     *
     * PB6/PB7 是低 8 位引脚（0~7），在 CRL 寄存器中配置。
     *
     * CNF = 11 → 复用开漏输出
     * MODE = 11 → 输出模式 50MHz
     *
     * 注意：不要和推挽输出（CNF=10）搞混。
     */
    GPIOB->CRL &= ~(GPIO_CRL_MODE6 | GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 | GPIO_CRL_CNF7);

    GPIOB->CRL |= GPIO_CRL_MODE6;      /* MODE6 = 11 */
    GPIOB->CRL |= GPIO_CRL_CNF6;       /* CNF6 = 11：复用开漏 */
    GPIOB->CRL |= GPIO_CRL_MODE7;      /* MODE7 = 11 */
    GPIOB->CRL |= GPIO_CRL_CNF7;       /* CNF7 = 11：复用开漏 */
}

/*
 * i2c1_init —— 初始化 I2C1 为 100kHz 标准模式主机
 *
 * 关键计算：
 *
 * CR2.FREQ：APB1 时钟频率（单位 MHz）
 *   PCLK1 = 36MHz，所以 FREQ = 36。
 *   这个值用于 I2C 内部时序生成，必须正确设置。
 *
 * CCR（Clock Control Register）：
 *   标准模式（Fscl = 100kHz）下：
 *   CCR = PCLK1 / (2 × Fscl) = 36MHz / (2 × 100kHz) = 180
 *   这里 PCLK1 单位 Hz，Fscl 单位 Hz。
 *   如果使用快速模式（400kHz），公式不同。
 *
 * TRISE（Maximum Rise Time Register）：
 *   标准模式下：TRISE = (PCLK1 的 MHz 值) + 1 = 36 + 1 = 37
 *   这个值用于 SCL 上升沿的最大时间，单位是 PCLK1 周期。
 *   100kHz 标准模式要求最大上升时间 1000ns，
 *   每个 PCLK1 周期 = 1/36MHz ≈ 27.8ns，
 *   1000ns / 27.8ns ≈ 36，再加 1 确保安全。
 */
static void i2c1_init(void)
{
    /*
     * 第 1 步：配置前先关闭 I2C
     *
     * 在修改 I2C 的配置寄存器之前，建议先关闭 PE。
     * 部分寄存器（如 CCR、TRISE）在 PE=1 时不能修改。
     * 配置完成后重新打开 PE。
     */
    I2C1->CR1 &= ~I2C_CR1_PE;

    /*
     * 第 2 步：配置 CR2.FREQ = 36
     *
     * FREQ[5:0]（bit 0-5）：APB1 时钟频率（MHz）。
     * 有效范围 2~50。PCLK1=36MHz → FREQ=36。
     *
     * 如果 FREQ 配置错误，I2C 的时序会全部不对。
     */
    I2C1->CR2 &= ~I2C_CR2_FREQ;
    I2C1->CR2 |= 36U;

    /*
     * 第 3 步：配置自身地址 OAR1
     *
     * 本课只做主机（不响应其他主机的寻址），
     * 但 STM32F103 I2C 硬件要求 OAR1 的 bit 14（ADDMODE）必须置 1。
     * 否则 I2C 初始化可能不正常。
     */
    I2C1->OAR1 = I2C_OAR1_ADDMODE;

    /*
     * 第 4 步：配置 CCR = 180
     *
     * CCR[11:0] 在标准模式下：
     *   SCL 频率 = PCLK1 / (2 × CCR)
     *   所以 CCR = PCLK1 / (2 × Fscl)
     *           = 36,000,000 / (2 × 100,000)
     *           = 180
     *
     * 验证：Fscl = 36MHz / (2 × 180) = 36MHz / 360 = 100kHz ✓
     */
    I2C1->CCR = 180U;

    /*
     * 第 5 步：配置 TRISE = 37
     *
     * TRISE[5:0]：
     *   标准模式：PCLK1(MHz) + 1 = 36 + 1 = 37
     *
     * TRISE 的单位是 PCLK1 周期数。
     * 它告诉 I2C 硬件：SCL 上升沿最长允许这么多周期。
     * 如果实际上升沿超过这个值，I2C 会等待。
     */
    I2C1->TRISE = 37U;

    /*
     * 第 6 步：使能 ACK
     *
     * CR1.ACK（bit 10）：
     *   0 = 收到字节后不应答（NACK）
     *   1 = 收到字节后应答（ACK） ← 本课
     *
     * 作为主机，接收数据时通常需要发送 ACK，
     * 告诉从机"我收到了，请继续"。
     * （在最后一个字节时，需要先关 ACK 再接收）
     */
    I2C1->CR1 |= I2C_CR1_ACK;

    /*
     * 第 7 步：打开 I2C 总使能
     */
    I2C1->CR1 |= I2C_CR1_PE;
}

/*
 * i2c1_wait_sr1_set —— 等待 SR1 的某个标志置位（带超时）
 *
 * 参数 flag_mask：要等待的标志位（如 I2C_SR1_TXE、I2C_SR1_RXNE 等）
 * 返回：1 = 成功（标志置位），0 = 超时
 */
static uint8_t i2c1_wait_sr1_set(uint32_t flag_mask)
{
    uint32_t timeout = I2C_TIMEOUT_COUNT;

    while (((I2C1->SR1 & flag_mask) == 0U) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 1U : 0U;
}

/*
 * i2c1_wait_bus_free —— 等待 I2C 总线空闲
 *
 * SR2.BUSY（bit 1）：
 *   0 = 总线空闲
 *   1 = 总线忙（正在通信）
 *
 * 在发起新的通信前，确保总线空闲是一个好习惯。
 * 如果总线一直忙（例如上次通信的 STOP 没被正确处理），应返回超时。
 */
static uint8_t i2c1_wait_bus_free(void)
{
    uint32_t timeout = I2C_TIMEOUT_COUNT;

    while (((I2C1->SR2 & I2C_SR2_BUSY) != 0U) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 1U : 0U;
}

/*
 * i2c1_clear_addr_flag —— 清除 ADDR 标志
 *
 * ADDR（Address Sent）标志在 SR1 中。
 * 当主机发送的地址字节被从机确认（收到 ACK）后，ADDR 置位。
 *
 * 清除 ADDR 的方法（STM32F103 I2C 硬件要求）：
 *   先读 SR1，再读 SR2。
 *   读取 SR2 后硬件自动清除 ADDR 标志。
 *   这两步**缺一不可**，且顺序不能反。
 *
 * 注意：这里用 volatile 临时变量 temp 接收 SR2 的值，
 * 确保编译器不会优化掉第二次读取。
 */
static void i2c1_clear_addr_flag(void)
{
    volatile uint32_t temp;

    temp = I2C1->SR1;
    temp = I2C1->SR2;
    (void)temp;
}

/*
 * i2c1_send_start —— 产生 START 条件
 *
 * START 条件：
 *   当 SCL = 1 时，SDA 从高电平切换到低电平。
 *
 * 软件操作：
 *   设置 CR1.START（bit 8）= 1。
 *   I2C 硬件会自动在总线上产生 START 条件。
 *   完成后 SR1.SB（Start Bit）= 1 表示 START 已发送。
 *
 * 返回：1 = START 发送成功，0 = 超时
 */
static uint8_t i2c1_send_start(void)
{
    I2C1->CR1 |= I2C_CR1_START;
    return i2c1_wait_sr1_set(I2C_SR1_SB);
}

/*
 * i2c1_send_stop —— 产生 STOP 条件
 *
 * STOP 条件：
 *   当 SCL = 1 时，SDA 从低电平切换到高电平。
 *
 * 软件操作：
 *   设置 CR1.STOP（bit 9）= 1。
 *   I2C 硬件自动产生 STOP 条件。
 *
 * 注意：STOP 不需要等待标志。
 *   产生 STOP 后，I2C 硬件自动处理后续时序。
 *   之后 SR2.BUSY 会变为 0。
 */
static void i2c1_send_stop(void)
{
    I2C1->CR1 |= I2C_CR1_STOP;
}

/*
 * i2c1_send_address —— 发送器件地址字节并等待应答
 *
 * 发送的地址字节 = 7 位地址左移 1 位 + R/W 位。
 *   写操作传入 AT24C02_ADDR_WRITE（0xA0）
 *   读操作传入 AT24C02_ADDR_READ（0xA1）
 *
 * 可能的结果：
 *   1. ADDR 置位 → 收到 ACK，地址被确认 → 返回 1
 *   2. AF 置位 → 收到 NACK，无设备响应 → 返回 0
 *   3. 超时 → 返回 0
 *
 * AF（Acknowledge Failure，SR1 bit 10）：
 *   地址或数据发送后未收到 ACK 时置位。
 *   需要软件手动清除。
 */
static uint8_t i2c1_send_address(uint8_t address_byte)
{
    uint32_t timeout = I2C_TIMEOUT_COUNT;

    I2C1->DR = address_byte;

    while (timeout > 0U) {
        uint32_t sr1 = I2C1->SR1;

        if ((sr1 & I2C_SR1_ADDR) != 0U) {
            return 1U;    /* 收到 ACK */
        }

        if ((sr1 & I2C_SR1_AF) != 0U) {
            I2C1->SR1 &= ~I2C_SR1_AF;   /* 清除 AF 标志 */
            return 0U;    /* 收到 NACK */
        }

        timeout--;
    }

    return 0U;    /* 超时 */
}

/*
 * at24c02_write_byte —— 向 AT24C02 写 1 个字节
 *
 * I2C 写操作完整序列：
 *   START → 地址+写位（0xA0）→ 清除 ADDR → 等待 TXE
 *   → 发内存地址（0x00）→ 等待 BTF
 *   → 发数据字节 → 等待 BTF
 *   → STOP
 *
 * BTF（Byte Transfer Finished，SR1 bit 2）：
 *   表示一个字节传输完成（DR 空 + 正在移位发送的也完成了）。
 *   对于连续发送场景，BTF 比 TXE 更可靠。
 *
 * 参数：mem_addr = EEPROM 内部地址，data_byte = 要写入的数据
 * 返回：1 = 写入成功，0 = 写入失败
 */
static uint8_t at24c02_write_byte(uint8_t mem_addr, uint8_t data_byte)
{
    /* 等待总线空闲 */
    if (i2c1_wait_bus_free() == 0U) {
        return 0U;
    }

    /* 发送 START */
    if (i2c1_send_start() == 0U) {
        return 0U;
    }

    /* 发送器件地址 + 写位（0xA0） */
    if (i2c1_send_address(AT24C02_ADDR_WRITE) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /* 清除 ADDR 标志 */
    i2c1_clear_addr_flag();

    /* 等待 TXE */
    if (i2c1_wait_sr1_set(I2C_SR1_TXE) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /* 发送 EEPROM 内部地址 */
    I2C1->DR = mem_addr;

    /* 等待 BTF（字节传输完成） */
    if (i2c1_wait_sr1_set(I2C_SR1_BTF) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /* 发送真正要写入 EEPROM 的数据字节 */
    I2C1->DR = data_byte;

    /* 等待 BTF */
    if (i2c1_wait_sr1_set(I2C_SR1_BTF) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /* 发送 STOP */
    i2c1_send_stop();
    return 1U;
}

/*
 * at24c02_read_byte —— 从 AT24C02 随机读 1 个字节
 *
 * "随机读"（Random Read）与"当前地址读"（Current Address Read）不同。
 * 随机读需要先写入目标内存地址，再用"重复起始"切换到读方向。
 *
 * 读操作完整序列：
 *   [第 1 段：写内存地址]
 *   START → 地址+写位（0xA0）→ 清除 ADDR → 发内存地址 → 等 BTF
 *
 *   [第 2 段：重复起始收数据]
 *   RESTART → 地址+读位（0xA1）→ 清除 ADDR（同时关 ACK + STOP）
 *   → 等 RXNE → 读 DR → 恢复 ACK
 *
 * 单字节接收的经典流程（"关 ACK + STOP + 等 RXNE"）：
 *   1. 收到地址的 ADDR 后，在清除前先关 ACK
 *      （因为只读 1 个字节，不应答告诉从机停止发送）
 *   2. 清除 ADDR
 *   3. 立即产生 STOP 条件
 *   4. 等待 RXNE（最后一个字节到达）
 *   5. 读取 DR
 *
 * 参数：mem_addr = EEPROM 内部地址，data_byte = 输出参数，存放读到的数据
 * 返回：1 = 读取成功，0 = 读取失败
 */
static uint8_t at24c02_read_byte(uint8_t mem_addr, uint8_t *data_byte)
{
    if (data_byte == 0) {
        return 0U;
    }

    /* ====== 第 1 段：写内存地址 ====== */
    if (i2c1_wait_bus_free() == 0U) {
        return 0U;
    }

    I2C1->CR1 |= I2C_CR1_ACK;

    if (i2c1_send_start() == 0U) {
        return 0U;
    }

    if (i2c1_send_address(AT24C02_ADDR_WRITE) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    i2c1_clear_addr_flag();

    if (i2c1_wait_sr1_set(I2C_SR1_TXE) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    I2C1->DR = mem_addr;

    if (i2c1_wait_sr1_set(I2C_SR1_BTF) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /* ====== 第 2 段：重复起始 + 读数据 ====== */

    /*
     * 重复起始（Repeated Start / RESTART）：
     *   在不产生 STOP 的情况下再次产生 START。
     *   这允许主机在同一个 I2C 总线上切换通信方向（写→读）。
     *   软件上：再次设置 CR1.START = 1（I2C 硬件知道当前在通信中，
     *   会自动产生 RESTART 而不是普通的 START）。
     */
    if (i2c1_send_start() == 0U) {
        return 0U;
    }

    if (i2c1_send_address(AT24C02_ADDR_READ) == 0U) {
        i2c1_send_stop();
        return 0U;
    }

    /*
     * 单字节接收的关键步骤：
     *
     * 为什么要在清除 ADDR 前关 ACK？
     *   如果 ACK=1，收到数据字节后主机会发 ACK，
     *   告诉从机"我收到了，请继续发下一个"。
     *   但本课只读 1 个字节，应该告诉从机"不要再发了"。
     *   所以先关 ACK，这样收到数据后主机发 NACK，从机停止发送。
     *
     * 为什么关 ACK 后立即产生 STOP？
     *   在清除 ADDR 后、从机发数据之前就产生 STOP，
     *   这样最后一个字节到达后主机不再应答，总线释放。
     *   这是 F103 I2C 单字节接收的标准时序。
     */
    I2C1->CR1 &= ~I2C_CR1_ACK;       /* 关 ACK */
    i2c1_clear_addr_flag();           /* 清 ADDR */
    i2c1_send_stop();                 /* 提前产生 STOP */

    /* 等待 RXNE（接收数据寄存器非空） */
    if (i2c1_wait_sr1_set(I2C_SR1_RXNE) == 0U) {
        I2C1->CR1 |= I2C_CR1_ACK;
        return 0U;
    }

    *data_byte = (uint8_t)I2C1->DR;

    /* 恢复 ACK，为下一次操作做准备 */
    I2C1->CR1 |= I2C_CR1_ACK;

    return 1U;
}

/*
 * main —— 主函数
 *
 * 流程：
 *   1. 系统初始化（时钟、SysTick、LED）
 *   2. I2C1 初始化（引脚 + 100kHz 标准模式）
 *   3. 主循环：
 *      a. 写 1 字节到 EEPROM
 *      b. 等待 EEPROM 内部写周期（10ms）
 *      c. 从 EEPROM 读回
 *      d. 比较读写结果 → 控制 LED
 *
 * 为什么写完后要等 10ms？
 *   AT24C02 写入内部存储单元需要时间（典型值 5ms，最大 10ms）。
 *   在这段时间内访问 EEPROM，可能读不到正确数据或写操作不完整。
 *   本课简单粗暴用 10ms 延时。
 *   更高效的做法是"轮询 ACK"——EEPROM 写完后才会响应总线。
 */
int main(void)
{
    uint8_t tx_byte = 0xA5U;
    uint8_t rx_byte = 0U;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    i2c1_gpio_init();
    i2c1_init();

    while (1) {
        /* 写 1 个字节到 EEPROM 地址 0x00 */
        if (at24c02_write_byte(EEPROM_MEM_ADDR, tx_byte) == 0U) {
            led_on();
            delay_ms(1000U);
            continue;
        }

        /* 等待 EEPROM 内部写周期结束 */
        delay_ms(10U);

        /* 从 EEPROM 地址 0x00 读回 */
        if (at24c02_read_byte(EEPROM_MEM_ADDR, &rx_byte) == 0U) {
            led_on();
            delay_ms(1000U);
            continue;
        }

        /* 比较读写结果 */
        if (rx_byte == tx_byte) {
            led_toggle();
        } else {
            led_on();
        }

        /* 交替测试字节 */
        if (tx_byte == 0xA5U) {
            tx_byte = 0x3CU;
        } else {
            tx_byte = 0xA5U;
        }

        delay_ms(1000U);
    }
}