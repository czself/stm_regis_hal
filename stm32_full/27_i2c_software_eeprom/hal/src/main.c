#include "stm32f1xx_hal.h"

/*
 * HAL 版：软件 I2C 写 EEPROM。
 *
 * HAL_GPIO_WritePin() 只是替代寄存器版的 BSRR/BRR 写法。
 * 本课重点仍然是软件按 I2C 规则控制 SCL/SDA 的时序。
 */

#define SCL_PIN GPIO_PIN_6
#define SDA_PIN GPIO_PIN_7

static void system_clock_72mhz_init(void);
static void pc13_led_init(void);
static void soft_i2c_gpio_init(void);
static void error_handler(void);

/*
 * 软件 I2C 速度由此延时决定。120 次空循环在 72MHz 下约 1.6µs。
 * 系统时钟变化时实际速率随之变化，没有硬件 I2C 的 CCR 精确分频。
 */
static void i2c_delay(void)
{
    for (volatile uint32_t i = 0U; i < 120U; i++) {
        __NOP();
    }
}

static void scl_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(GPIOB, SCL_PIN, state);
}

static void sda_write(GPIO_PinState state)
{
    HAL_GPIO_WritePin(GPIOB, SDA_PIN, state);
}

/*
 * START：SCL 高时 SDA 从高→低。
 * 如果 SDA 下降沿不在 SCL 高电平期间，AT24C02 不识别起始条件。
 */
static void i2c_start(void)
{
    sda_write(GPIO_PIN_SET);
    scl_write(GPIO_PIN_SET);
    i2c_delay();

    sda_write(GPIO_PIN_RESET);
    i2c_delay();

    scl_write(GPIO_PIN_RESET);
}

/*
 * STOP：SCL 高时 SDA 从低→高。
 * 缺少 STOP 时 EEPROM 不启动内部写周期，且总线持续被占用。
 */
static void i2c_stop(void)
{
    sda_write(GPIO_PIN_RESET);
    scl_write(GPIO_PIN_SET);
    i2c_delay();

    sda_write(GPIO_PIN_SET);
    i2c_delay();
}

/*
 * 写 1 bit。必须先放 SDA 再拉高 SCL，
 * 否则从机在 SCL 高期间采到的是上一位的电平。
 */
static void i2c_write_bit(uint8_t bit_is_one)
{
    sda_write(bit_is_one ? GPIO_PIN_SET : GPIO_PIN_RESET);
    i2c_delay();

    scl_write(GPIO_PIN_SET);
    i2c_delay();

    scl_write(GPIO_PIN_RESET);
}

static void i2c_write_byte(uint8_t byte)
{
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        i2c_write_bit((byte & 0x80U) != 0U);
        byte <<= 1;
    }

    /*
     * 第 9 个时钟：I2C 协议要求每字节后接收方应答。
     * 本代码只释放 SDA + 产生 SCL 脉冲，没有读 SDA 电平，
     * 所以不知道 AT24C02 是否真的 ACK 了。
     * LED 翻转只证明主机发出了波形，不证明 EEPROM 写成功。
     */
    sda_write(GPIO_PIN_SET);
    i2c_delay();
    scl_write(GPIO_PIN_SET);
    i2c_delay();
    scl_write(GPIO_PIN_RESET);
}

static void soft_i2c_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /*
     * GPIO_MODE_OUTPUT_OD = 开漏输出，对应寄存器版 CNF=11。
     *
     * 开漏模式下写 GPIO_PIN_SET = 释放（靠上拉拉高），
     * 写 GPIO_PIN_RESET = 拉低。
     * 如果误写成 GPIO_MODE_OUTPUT_PP（推挽），
     * 从机在第 9 个时钟无法拉低 SDA 做 ACK，电气上会冲突。
     */
    gpio.Pin = SCL_PIN | SDA_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    sda_write(GPIO_PIN_SET);
    scl_write(GPIO_PIN_SET);
}

/*
 * 主循环：每秒发送一次 I2C 写序列（START → 0xA0 → 0x00 → 0x5A → STOP）。
 * 边界：没有 ACK 读取和读回校验，LED 翻转不证明 EEPROM 写入成功。
 */
int main(void)
{
    HAL_Init();
    system_clock_72mhz_init();
    pc13_led_init();
    soft_i2c_gpio_init();

    while (1) {
        i2c_start();
        i2c_write_byte(0xA0U);
        i2c_write_byte(0x00U);
        i2c_write_byte(0x5AU);
        i2c_stop();

        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        HAL_Delay(1000);
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

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        error_handler();
    }
}

static void pc13_led_init(void)
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

void SysTick_Handler(void)
{
    HAL_IncTick();
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}
