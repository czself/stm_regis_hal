#include "stm32f1xx_hal.h"

/*
 * HAL 版：TIM1 高级定时器 PWM。
 *
 * 本课看 HAL 如何表达高级定时器的额外输出门。
 * 普通 PWM 字段仍然对应 PSC/ARR/CCR/OCM，新的重点是
 * TIM_BreakDeadTimeConfigTypeDef 对应底层 BDTR 寄存器。
 */

static TIM_HandleTypeDef htim1;

static void system_clock_72mhz_init(void);
static void pc13_led_init(void);
static void tim1_pwm_init(void);
static void error_handler(void);

int main(void)
{
    HAL_Init();
    system_clock_72mhz_init();
    pc13_led_init();
    tim1_pwm_init();

    while (1) {
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        HAL_Delay(500);
    }
}

static void tim1_pwm_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_OC_InitTypeDef oc = {0};
    TIM_BreakDeadTimeConfigTypeDef bd = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();

    /*
     * PA8 是 TIM1_CH1 复用输出脚。
     */
    gpio.Pin = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    htim1.Instance = TIM1;
    htim1.Init.Prescaler = 72U - 1U;
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = 1000U - 1U;
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;

    /*
     * RepetitionCounter 是高级定时器才有的重复计数器。
     * 本课不使用重复更新，所以设为 0，表示每个周期都正常更新。
     */
    htim1.Init.RepetitionCounter = 0U;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK) {
        error_handler();
    }

    /*
     * 这部分和普通 PWM 课程相同：
     * PWM1 + Pulse=300 -> 约 30% 占空比。
     */
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 300U;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;

    if (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) {
        error_handler();
    }

    /*
     * Break/DeadTime 配置对应 TIM1->BDTR。
     *
     * 本课不启用刹车输入，也不设置死区。
     * AutomaticOutput 对应 BDTR.AOE，表示刹车释放后的自动输出策略；
     * 真正打开主输出门 MOE 的动作由后面的 HAL_TIM_PWM_Start() 完成。
     *
     * 结构体零初始化后，所有字段已经是 DISABLE/0，
     * 只需显式设 BreakPolarity（HAL 要求即使 BreakState=DISABLE 也必须赋值）。
     */
    bd.BreakState = TIM_BREAK_DISABLE;
    bd.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
    bd.DeadTime = 0U;
    bd.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;

    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bd) != HAL_OK) {
        error_handler();
    }

    /*
     * 对 TIM1 来说，HAL_TIM_PWM_Start() 不只是启动计数和通道。
     * CubeF1 HAL 会在高级定时器启动 PWM 时设置 MOE，让主输出门打开。
     */
    if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1) != HAL_OK) {
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