#include "stm32f1xx.h"

/*
 * 寄存器版：软件 I2C 写 EEPROM。
 *
 * 26 已经讲过硬件 I2C1 的 START/ADDR/TXE/BTF。
 * 本课的新重点是：不用 I2C 外设，只用 GPIO 手工制造 SCL/SDA 时序。
 *
 * 当前代码只演示写序列：
 * START -> 0xA0 -> 0x00 -> 0x5A -> STOP
 *
 * 注意边界：这里没有读取 ACK，也没有读回校验。
 * PC13 翻转只能说明程序持续输出波形，不能单独证明 EEPROM 一定写成功。
 */

#define SCL_PIN_MASK GPIO_BSRR_BS6
#define SDA_PIN_MASK GPIO_BSRR_BS7

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

    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void pc13_led_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;

    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;

    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void pc13_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- != 0U) {
        __NOP();
    }
}

/*
 * 软件 I2C 的速度由此延时决定。120 次循环在 72MHz 下约 1.6µs。
 * 系统时钟变化时，实际 I2C 速率也随之变化——不像硬件 I2C 有 CCR 精确分频。
 * 参数太小 → SCL 过快，从机采样不稳定；太大 → 通信变慢，但不会出错。
 */
static void soft_i2c_delay(void)
{
    delay_cycles(120U);
}

static void scl_release(void)
{
    GPIOB->BSRR = SCL_PIN_MASK;
}

static void scl_low(void)
{
    GPIOB->BRR = GPIO_BRR_BR6;
}

static void sda_release(void)
{
    GPIOB->BSRR = SDA_PIN_MASK;
}

static void sda_low(void)
{
    GPIOB->BRR = GPIO_BRR_BR7;
}

static void soft_i2c_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    /*
     * PB6/PB7 配成开漏输出（MODE=11, CNF=11）。
     *
     * 为什么开漏？因为 I2C 总线上可能有多个设备同时拉 SDA。
     * 开漏模式下写 1 = 释放（靠上拉电阻拉高），写 0 = 拉低。
     * 如果错配成推挽（CNF=10），主机强推高 SDA 时从机无法拉低做 ACK，
     * 更糟的是两个设备同时输出相反电平会短路。
     *
     * 先清零再设置：CRL 里还有其他引脚（如 PB0~PB5）的配置位，
     * 不能直接覆盖——先 &~ 清除 PB6/PB7 的 4 位，再 | 写目标值。
     */
    GPIOB->CRL &= ~(GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 |
                    GPIO_CRL_CNF7);

    GPIOB->CRL |= GPIO_CRL_MODE6;
    GPIOB->CRL |= GPIO_CRL_CNF6;
    GPIOB->CRL |= GPIO_CRL_MODE7;
    GPIOB->CRL |= GPIO_CRL_CNF7;

    sda_release();
    scl_release();
}

/*
 * START：SCL 高电平时 SDA 从高→低。
 * 如果 SDA 下降沿不是发生在 SCL 高电平期间，AT24C02 不会识别为起始条件，
 * 后续所有地址和数据都会被忽略。
 */
static void i2c_start(void)
{
    sda_release();
    scl_release();
    soft_i2c_delay();

    sda_low();
    soft_i2c_delay();

    scl_low();
}

/*
 * STOP：SCL 高电平时 SDA 从低→高。
 * 如果 STOP 条件未发出（如 SDA 拉高时 SCL 不是高电平），
 * EEPROM 可能认为传输未结束，不启动内部写周期，或总线持续被占用。
 */
static void i2c_stop(void)
{
    sda_low();
    scl_release();
    soft_i2c_delay();

    sda_release();
    soft_i2c_delay();
}

/*
 * 写 1 bit。顺序固定：先放 SDA 再拉高 SCL。
 * 如果 SDA 在 SCL 拉高之后才变化，从机采样到的可能是上一位的电平。
 */
static void i2c_write_bit(uint8_t bit_is_one)
{
    if (bit_is_one != 0U) {
        sda_release();
    } else {
        sda_low();
    }

    soft_i2c_delay();
    scl_release();
    soft_i2c_delay();
    scl_low();
}

static void i2c_write_byte(uint8_t byte)
{
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        i2c_write_bit((byte & 0x80U) != 0U);
        byte <<= 1;
    }

    /*
     * 第 9 个时钟：I2C 协议规定每发完 8 位数据后，接收方必须应答。
     * 应答方式是：接收方在 SCL 高电平期间把 SDA 拉低（ACK）或不拉低（NACK）。
     *
     * 本课代码只做了"主机释放 SDA + 给出第 9 个 SCL 脉冲"，
     * 但没有在 SCL 高时读 GPIOB->IDR 检查 SDA 是否被 AT24C02 拉低。
     * 所以这里只是"给了 ACK 的窗口"，没有"验证 ACK 的结果"。
     *
     * 这是本课最重要的边界：LED 翻转只说明主机发出了波形，
     * 不说明 EEPROM 真的接收了数据。
     */
    sda_release();
    soft_i2c_delay();
    scl_release();
    soft_i2c_delay();
    scl_low();
}

/*
 * 主循环：反复发送 START → 0xA0 → 0x00 → 0x5A → STOP。
 * 边界：没有 ACK 读取和读回校验，LED 翻转不证明 EEPROM 写入成功。
 */
int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    soft_i2c_gpio_init();

    while (1) {
        i2c_start();
        i2c_write_byte(0xA0U);
        i2c_write_byte(0x00U);
        i2c_write_byte(0x5AU);
        i2c_stop();

        pc13_toggle();
        delay_cycles(7200000U);
    }
}
