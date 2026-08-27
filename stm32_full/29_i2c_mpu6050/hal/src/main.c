#include "stm32f1xx_hal.h"

/*
 * HAL 版：I2C 读写 MPU6050 寄存器。
 *
 * 和寄存器版的区别：寄存器版手动管理 START/STOP/ADDR/TXE/BTF/RXNE 等标志，
 * HAL 版用 HAL_I2C_Mem_Write/Read 把"器件地址 + 内部寄存器地址 + 数据"封装起来。
 *
 * 本课的关键概念：MPU6050 的 0x6B、0x75 都是器件内部寄存器地址，
 * 不是 I2C 器件地址（0x68/0xD0）。HAL 的 Mem_Write/Mem_Read 同时处理
 * 器件地址（选中芯片）和寄存器地址（选中芯片内部功能）两层寻址。
 *
 * HAL_I2C_Mem_Write 底层做的事（对应寄存器版 mpu_write_register）：
 *   1. START → 发器件地址 0xD0 → 等 ADDR
 *   2. 发寄存器地址 0x6B → 等 TXE
 *   3. 发数据 0x00 → 等 BTF
 *   4. STOP
 *
 * HAL_I2C_Mem_Read 底层做的事（对应寄存器版 mpu_read_register）：
 *   1. START → 发器件地址 0xD0 → 等 ADDR
 *   2. 发寄存器地址 0x75 → 等 BTF
 *   3. 重复起始 → 发器件地址 0xD1 → 等 ADDR
 *   4. 关 ACK、设 STOP → 等 RXNE → 读 DR
 */

#define MPU_ADDR 0xD0U

static I2C_HandleTypeDef hi2c1;

static void system_clock_72mhz_init(void);
static void pc13_led_init(void);
static void i2c1_init(void);
static void error_handler(void);

int main(void)
{
    uint8_t id = 0U;
    uint8_t wake_value = 0U;

    HAL_Init();
    system_clock_72mhz_init();
    pc13_led_init();
    i2c1_init();
    HAL_Delay(50);

    /*
     * PWR_MGMT_1 = 0x6B。
     *
     * 写 0x00 唤醒 MPU6050。上电后 MPU6050 默认 SLEEP=1（睡眠模式），
     * 加速度计和陀螺仪不工作以省电。WHO_AM_I 仍可读（I2C 接口独立供电），
     * 但测量数据寄存器不会更新。不写这一步，后续读到的加速度/陀螺仪数据
     * 永远是上电默认值，看起来像"通信成功了但数据不变"。
     */
    if (HAL_I2C_Mem_Write(&hi2c1,
                          MPU_ADDR,
                          0x6BU,
                          I2C_MEMADD_SIZE_8BIT,
                          &wake_value,
                          1U,
                          100U) != HAL_OK) {
        error_handler();
    }

    while (1) {
        /*
         * WHO_AM_I = 0x75。
         * 正常返回 0x68。WHO_AM_I 是芯片硬编码身份号，只读，永远不变。
         * 它不需要任何配置，是最适合做通信自检的寄存器。
         * 读到 0x68 → 供电/接线/地址/I2C读流程四层都通。
         * 读不到 → 问题一定在这四层之一。
         */
        if (HAL_I2C_Mem_Read(&hi2c1,
                             MPU_ADDR,
                             0x75U,
                             I2C_MEMADD_SIZE_8BIT,
                             &id,
                             1U,
                             100U) != HAL_OK) {
            error_handler();
        }

        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        HAL_Delay((id == 0x68U) ? 100U : 500U);
    }
}

static void i2c1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0U;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0U;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        error_handler();
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