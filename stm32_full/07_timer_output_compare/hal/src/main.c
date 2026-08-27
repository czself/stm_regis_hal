#include "stm32f1xx_hal.h"

/*
 * HAL 版：TIM 输出比较。
 *
 * 06 已经学过基础定时器和更新中断。本课要把注意力放到输出比较：
 * HAL 的 TIM_OC_InitTypeDef 字段最终会配置底层 CCMR/CCR/CCER，
 * 让 TIM2_CH1 在 CNT 匹配 CCR1 时自动翻转 PA0。
 */

static TIM_HandleTypeDef htim2;
static UART_HandleTypeDef huart1;

static void system_clock_72mhz_init(void);
static void pc13_led_init(void);
static void pa1_input_init(void);
static void uart1_init(void);
static void tim2_ch1_output_compare_init(void);
static void error_handler(void);

int main(void)
{
    HAL_Init();
    system_clock_72mhz_init();
    pc13_led_init();
    pa1_input_init();
    uart1_init();
    tim2_ch1_output_compare_init();

    while (1) {
        /*
         * 读 PA1 电平：PA0 通过杜邦线接到 PA1，
         * PA1 读到的就是 TIM2_CH1 输出比较产生的方波。
         * GPIO_PIN_SET = 1 表示高电平，GPIO_PIN_RESET = 0 表示低电平。
         */
        uint8_t level = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1) == GPIO_PIN_SET) ? 1U : 0U;

        /*
         * FireWater 协议：每行一个数值，以 \n 结尾。
         * VOFA+ 收到 "0\n" 或 "1\n" 后在波形窗口显示跳变。
         */
        uint8_t buf[2];
        buf[0] = '0' + level;
        buf[1] = '\n';
        HAL_UART_Transmit(&huart1, buf, 2U, 100U);

        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        HAL_Delay(50);
    }
}

static void pa1_input_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    /*
     * PA1 配成输入模式，用来读取 PA0 输出的方波电平。
     * 用杜邦线把 PA0 接到 PA1，PA1 读到的就是 TIM2_CH1 的输出状态。
     */
    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

static void uart1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /*
     * PA9 = USART1_TX，配成复用推挽输出。
     * PA10 = USART1_RX，配成输入（本课不用接收，但引脚模式要配对）。
     */
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /*
     * USART1 时钟 = 72MHz（APB2），波特率 115200。
     * HAL 内部根据 BRR = 72MHz / 115200 = 625 自动计算。
     */
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200U;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        error_handler();
    }
}

static void tim2_ch1_output_compare_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_OC_InitTypeDef oc = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM2_CLK_ENABLE();

    /*
     * PA0 必须配置成复用推挽输出。
     * GPIO_MODE_AF_PP 对应寄存器版的 CNF0=10、MODE0=10，
     * 表示引脚电平交给 TIM2_CH1 这类复用外设驱动。
     */
    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    htim2.Instance = TIM2;

    /*
     * Prescaler 和 Period 仍然对应底层 PSC / ARR。
     * 72MHz / 7200 = 10kHz，10kHz / 10000 = 1Hz。
     */
    htim2.Init.Prescaler = 7200U - 1U;
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 10000U - 1U;
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_OC_Init(&htim2) != HAL_OK) {
        error_handler();
    }

    /*
     * OCMode = TOGGLE 对应寄存器版的 OC1M=011：
     * 当 CNT == CCR1 时，通道输出翻转一次。
     */
    oc.OCMode = TIM_OCMODE_TOGGLE;

    /*
     * Pulse 对应 CCR1。
     * 这里写 5000，表示在 1 秒周期的中点发生比较匹配。
     */
    oc.Pulse = 5000U;

    /*
     * 初始有效电平为高。对 toggle 模式来说，更重要的是“匹配时翻转”，
     * 极性主要决定输出通道的有效电平定义。
     */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;

    if (HAL_TIM_OC_ConfigChannel(&htim2, &oc, TIM_CHANNEL_1) != HAL_OK) {
        error_handler();
    }

    /*
     * Start 会打开通道输出并启动计数器。
     * 漏掉它时，前面的配置只是写进了句柄和寄存器，PA0 不会真正开始输出。
     */
    if (HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1) != HAL_OK) {
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
    /* PC13 软件心跳使用 HAL_Delay()，因此需要维护 HAL tick。 */
    HAL_IncTick();
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}