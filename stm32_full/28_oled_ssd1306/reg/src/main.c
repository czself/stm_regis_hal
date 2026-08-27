#include "stm32f1xx.h"

/*
 * 寄存器版：SSD1306 OLED I2C 写命令和显存数据。
 *
 * 26 已经讲过 I2C1 的 START/ADDR/TXE/BTF。
 * 本课的新重点是 SSD1306 的 control byte：
 * - 0x00 表示后面的字节是命令
 * - 0x40 表示后面的字节是显示数据
 *
 * I2C 只负责把字节送到 OLED；这些字节到底是命令还是像素数据，
 * 由 SSD1306 根据 control byte 决定。
 */

#define OLED_ADDR 0x78U

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

static void i2c1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    /*
     * PB6/PB7 是 I2C1 的 SCL/SDA，必须配成复用开漏（CNF=11）。
     * 开漏模式下写 1 = 释放（上拉拉高），写 0 = 拉低。
     * 如果错配成推挽，多个设备同时输出相反电平会短路。
     * 先清零再设置：防止 CRL 中其他引脚（PB0~PB5）的配置被误改。
     */
    GPIOB->CRL &= ~(GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 |
                    GPIO_CRL_CNF7);

    GPIOB->CRL |= GPIO_CRL_MODE6 | GPIO_CRL_CNF6;
    GPIOB->CRL |= GPIO_CRL_MODE7 | GPIO_CRL_CNF7;

    /*
     * I2C1 时序配置：
     *   SWRST -> 软件复位，清除之前可能残留的状态
     *   CR2=36  -> PCLK1=36MHz，告诉 I2C1 内部时钟频率
     *   CCR=180 -> SCL=36MHz/(2*180)=100kHz（标准模式）
     *   TRISE=37 -> 最大上升时间 = (36MHz)+1 个 PCLK1 周期
     *   PE=1     -> 打开 I2C1 外设
     */
    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0U;

    I2C1->CR2 = 36U;
    I2C1->CCR = 180U;
    I2C1->TRISE = 37U;
    I2C1->CR1 = I2C_CR1_PE;
}

static void i2c1_start_addr(uint8_t addr)
{
    I2C1->CR1 |= I2C_CR1_START;
    while ((I2C1->SR1 & I2C_SR1_SB) == 0U) {
    }

    I2C1->DR = addr;
    while ((I2C1->SR1 & I2C_SR1_ADDR) == 0U) {
    }

    (void)I2C1->SR1;
    (void)I2C1->SR2;
}

static void i2c1_write(uint8_t byte)
{
    while ((I2C1->SR1 & I2C_SR1_TXE) == 0U) {
    }

    I2C1->DR = byte;
}

static void oled_write(uint8_t control, uint8_t value)
{
    /*
     * SSD1306 每次写入都先发 control byte。
     * control=0x00：value 是命令；control=0x40：value 是显存数据。
     */
    i2c1_start_addr(OLED_ADDR);
    i2c1_write(control);
    i2c1_write(value);

    while ((I2C1->SR1 & I2C_SR1_BTF) == 0U) {
    }

    I2C1->CR1 |= I2C_CR1_STOP;
}

static void oled_cmd(uint8_t command)
{
    oled_write(0x00U, command);
}

static void oled_data(uint8_t data)
{
    oled_write(0x40U, data);
}

static void oled_init(void)
{
    static const uint8_t init_commands[] = {
        0xAE, 0x20, 0x02, 0xB0, 0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6, 0xA8, 0x3F, 0xD3, 0x00,
        0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40,
        0x8D, 0x14, 0xAF
    };

    for (uint32_t i = 0U; i < sizeof(init_commands); i++) {
        oled_cmd(init_commands[i]);
    }
}

static void oled_write_test_pattern(void)
{
    /*
     * 选择 page 0、column 0，然后写 128 字节测试图案。
     * 0x55/0xAA 的 bit 交替，会形成便于观察的条纹。
     */
    oled_cmd(0xB0U);
    oled_cmd(0x00U);
    oled_cmd(0x10U);

    for (uint8_t i = 0U; i < 128U; i++) {
        uint8_t pattern = (i & 1U) ? 0xAAU : 0x55U;
        oled_data(pattern);
    }
}

int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    i2c1_init();
    delay_cycles(720000U);

    oled_init();
    oled_write_test_pattern();

    while (1) {
        pc13_toggle();
        delay_cycles(3600000U);
    }
}
