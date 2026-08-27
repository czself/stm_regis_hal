#include "stm32f1xx_hal.h"

/*
 * HAL 版：USART1 数据包协议解析。
 *
 * 21~24 已经学过 UART 收发、printf 重定向。本课的新重点不是"收到一个字节"，
 * 而是把连续字节流切分成有边界、有含义的一帧数据。
 *
 * 本课约定 4 字节协议帧：
 *   0xAA  CMD  DATA  0x55
 *
 * 为什么要自己定义协议：
 * - UART 硬件只负责把串行波形还原成字节，不负责告诉你"这几个字节是一包"。
 * - 协议帧用帧头帧尾给字节流加上"边界"，让接收端知道一包从哪里开始、到哪里结束。
 *
 * 为什么用状态机而不是固定位置读取：
 * - 串口可能在任何时刻接入，第一个收到的字节可能是一包中间的数据。
 * - 状态机从 WAIT_HEAD 开始丢弃非帧头字节，直到下一个 0xAA 出现才重新同步。
 *
 * 错误后果：
 * - 帧头不是 0xAA：状态机卡在 WAIT_HEAD，LED 永远不会响应。
 * - 帧尾不是 0x55：整帧丢弃，LED 不动作，但状态机回到 WAIT_HEAD 等下一包。
 * - 波特率不匹配：收到的字节全是乱码，状态机永远等不到 0xAA。
 *
 * HAL_UART_Receive() 负责阻塞接收 1 个字节，封装了寄存器版的"等待 RXNE 再读 DR"。
 * 协议状态机仍然由我们自己维护——HAL 不替你解析协议，只替你收字节。
 */

typedef enum {
    WAIT_HEAD = 0,
    WAIT_CMD,
    WAIT_DATA,
    WAIT_TAIL
} PacketState;

static UART_HandleTypeDef huart1;

static void system_clock_72mhz_init(void);
static void pc13_led_init(void);
static void usart1_init(void);
static void error_handler(void);

static void handle_packet(uint8_t cmd, uint8_t data)
{
    /*
     * 协议层 → 动作层的分界函数。
     *
     * 前置条件：状态机已确认收到完整一帧（AA/CMD/DATA/55 都正确）。
     *
     * 目的：把协议解析和业务动作分开。状态机只负责"收到什么"，本函数只负责"做什么"。
     *       后续增加命令时只扩展这个函数，不碰状态机。
     *
     * CMD=0x01：控制 PC13 LED。BluePill 板载 LED 低电平点亮，
     *           data!=0 时写 RESET（亮），data=0 时写 SET（灭）。
     *
     * 错误后果：cmd 不是 0x01 时函数什么都不做，LED 保持不变。
     *           未定义命令静默忽略，不产生副作用。
     */
    if (cmd == 0x01U) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, data ? GPIO_PIN_RESET : GPIO_PIN_SET);
    }
}

static void packet_fsm_input(PacketState *state, uint8_t byte, uint8_t *cmd, uint8_t *data)
{
    /*
     * 协议状态机：每次处理 1 个字节，根据当前状态决定这个字节的含义。
     *
     * 前置条件：HAL_UART_Receive() 已返回 HAL_OK，byte 是从 USART1 DR 读到的有效字节。
     *
     * 目的：把连续字节流变成结构化数据包。HAL 只负责收字节，本函数负责判断
     *       每个字节在当前帧中的角色（帧头/命令/数据/帧尾）。
     *
     * 状态切换：
     *   WAIT_HEAD → 收到 0xAA 时进入 WAIT_CMD，否则丢弃（抵抗噪声和半包）
     *   WAIT_CMD  → 保存 cmd，进入 WAIT_DATA
     *   WAIT_DATA → 保存 data，进入 WAIT_TAIL
     *   WAIT_TAIL → 收到 0x55 时执行 handle_packet()，无论正确与否都回到 WAIT_HEAD
     *
     * 错误后果：
     * - 帧尾错误：整帧丢弃，但状态机回到 WAIT_HEAD，下一包不受影响。
     * - 如果在 WAIT_TAIL 后不回 WAIT_HEAD：状态机永久错位，之后所有帧无法正确解析。
     */
    switch (*state) {
    case WAIT_HEAD:
        /*
         * 不在帧内时只认包头 0xAA。
         * 其他字节全部丢弃，用来抵抗串口刚接入时的半包和噪声。
         */
        if (byte == 0xAAU) {
            *state = WAIT_CMD;
        }
        break;

    case WAIT_CMD:
        *cmd = byte;
        *state = WAIT_DATA;
        break;

    case WAIT_DATA:
        *data = byte;
        *state = WAIT_TAIL;
        break;

    case WAIT_TAIL:
        /*
         * 包尾正确才执行；包尾错误则整帧丢弃。
         * 执行后也回到 WAIT_HEAD，准备解析下一帧。
         */
        if (byte == 0x55U) {
            handle_packet(*cmd, *data);
        }
        *state = WAIT_HEAD;
        break;

    default:
        *state = WAIT_HEAD;
        break;
    }
}

int main(void)
{
    PacketState state = WAIT_HEAD;
    uint8_t byte = 0U;
    uint8_t cmd = 0U;
    uint8_t data = 0U;

    HAL_Init();
    system_clock_72mhz_init();
    pc13_led_init();
    usart1_init();

    while (1) {
        /*
         * HAL_UART_Receive() 封装了寄存器版的"等待 RXNE 再读 DR"。
         * 每次只接收 1 字节，把"串口收字节"和"协议解析"解耦。
         *
         * HAL_MAX_DELAY 表示一直阻塞等到收到数据，适合最小演示。
         * 工程中不建议用 HAL_MAX_DELAY 阻塞主循环，但本课先聚焦协议逻辑。
         *
         * 前置条件：usart1_init() 已成功，USB-TTL 已接 PA10 且共地。
         * 错误后果：如果 HAL_UART_Receive() 返回非 HAL_OK（如超时、错误），
         *           本字节被丢弃，状态机不更新，LED 不响应。
         */
        if (HAL_UART_Receive(&huart1, &byte, 1U, HAL_MAX_DELAY) == HAL_OK) {
            packet_fsm_input(&state, byte, &cmd, &data);
        }
    }
}

static void usart1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /*
     * PA9 = USART1_TX，复用推挽输出。
     */
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /*
     * PA10 = USART1_RX，输入。
     */
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