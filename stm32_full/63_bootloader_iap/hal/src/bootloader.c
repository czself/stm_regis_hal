#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * HAL 版：最小 Bootloader。
 *
 * 与寄存器版做同一件事，但把时钟、GPIO、串口的底层操作换成 HAL：
 *   RCC 配置        -> HAL_RCC_OscConfig / HAL_RCC_ClockConfig
 *   GPIO 输出       -> HAL_GPIO_Init / HAL_GPIO_TogglePin
 *   USART 收发      -> HAL_UART_Init / HAL_UART_Transmit / HAL_UART_Receive
 *
 * 跳转 app 的部分 HAL 没有封装，仍然要直接操作 VTOR、MSP 和函数指针，
 * 这是本课最重要的“HAL 边界”：厂商库不替你完成固件交接。
 */

#define APP_START_ADDR   0x08008000UL
#define APP_END_ADDR     0x08010000UL
#define SRAM_START_ADDR  0x20000000UL
#define SRAM_END_ADDR    0x20005000UL
#define BOOT_WAIT_MS     3000U

static UART_HandleTypeDef huart1;

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void uart1_init(void);
static void boot_print(const char *text);
static uint8_t app_is_valid(void);
static void jump_to_app(void);
static void error_handler(void);

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

static void uart1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200U;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        error_handler();
    }
}

static void boot_print(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)strlen(text), 100U);
}

/*
 * 与寄存器版相同的有效性判断，只是写法换成 HAL/标准库风格。
 * 仍然是检查向量表前两个字：栈顶和复位入口。
 */
static uint8_t app_is_valid(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);

    if ((app_sp < SRAM_START_ADDR) || (app_sp > SRAM_END_ADDR)) {
        return 0U;
    }
    if ((app_reset < APP_START_ADDR) || (app_reset >= APP_END_ADDR)) {
        return 0U;
    }
    if ((app_reset & 1U) == 0U) {
        return 0U;
    }
    return 1U;
}

/*
 * 跳转前先反初始化串口，避免 app 重新初始化 USART1 时
 * 还残留 bootloader 的收发状态和 NVIC 使能位。
 */
static void jump_to_app(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);
    uint32_t i;

    __disable_irq();

    HAL_UART_DeInit(&huart1);

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    SCB->VTOR = APP_START_ADDR;
    __set_MSP(app_sp);
    __enable_irq();

    ((void (*)(void))app_reset)();
}

int main(void)
{
    uint32_t start_tick;

    HAL_Init();
    system_clock_72mhz_init();
    led_pc13_init();
    uart1_init();

    boot_print("\r\n[BOOT-HAL] STM32F103 bootloader v1.0");
    boot_print("\r\n[BOOT-HAL] app region: 0x08008000 - 0x0800FFFF");

    if (app_is_valid() == 0U) {
        boot_print("\r\n[BOOT-HAL] app invalid, stay here");
        while (1) {
            HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
            HAL_Delay(500U);
        }
    }

    boot_print("\r\n[BOOT-HAL] send 'u' within 3s to stay\r\n");

    start_tick = HAL_GetTick();
    while ((HAL_GetTick() - start_tick) < BOOT_WAIT_MS) {
        uint8_t ch = 0U;

        if (HAL_UART_Receive(&huart1, &ch, 1U, 10U) == HAL_OK) {
            if ((ch == 'u') || (ch == 'U')) {
                boot_print("\r\n[BOOT-HAL] stay in bootloader\r\n");
                while (1) {
                    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
                    HAL_Delay(500U);
                }
            }
        }
    }

    boot_print("\r\n[BOOT-HAL] jump to app\r\n");
    HAL_Delay(20U);

    jump_to_app();

    while (1) {
    }
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
