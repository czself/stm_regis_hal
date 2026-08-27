#include "stm32f1xx.h"

/*
 * 寄存器版：USART1 数据包协议解析。
 *
 * 21~24 已经学过 UART 收发、printf 重定向。本课的新重点不是"收到一个字节"，
 * 而是把连续字节流切分成有边界、有含义的一帧数据。
 *
 * 本课约定 4 字节协议帧：
 *   0xAA  CMD  DATA  0x55
 *
 * 为什么要自己定义协议：
 * - UART 硬件只负责把串行波形还原成字节，不负责告诉你"这几个字节是一包"。
 * - 如果不定义帧头和帧尾，上位机发送 AA 01 01 55 时，STM32 收到的就是四个
 *   无关联的字节，不知道哪个是命令、哪个是参数。
 * - 协议帧用帧头帧尾给字节流加上"边界"，让接收端知道一包从哪里开始、到哪里结束。
 *
 * 为什么用状态机而不是固定位置读取：
 * - 串口可能在任何时刻接入（上电、热插拔），第一个收到的字节可能是一包中间的数据。
 * - 固定位置读取会永远错位。状态机从 WAIT_HEAD 开始，丢弃所有非帧头字节，
 *   直到下一个 0xAA 出现才重新同步，具备自愈能力。
 *
 * 错误后果：
 * - 帧头不是 0xAA：状态机卡在 WAIT_HEAD，LED 永远不会响应。
 * - 帧尾不是 0x55：整帧丢弃，LED 不动作，但状态机回到 WAIT_HEAD 等下一包。
 * - 波特率不匹配：收到的字节全是乱码，状态机永远等不到 0xAA。
 */

typedef enum {
    WAIT_HEAD = 0,
    WAIT_CMD,
    WAIT_DATA,
    WAIT_TAIL
} PacketState;

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

static void led_set(uint8_t on)
{
    if (on != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static void usart1_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;

    /*
     * PA9 = USART1_TX，复用推挽输出。
     * PA10 = USART1_RX，浮空输入。本课靠 RX 接收上位机发来的协议帧。
     */
    GPIOA->CRH &= ~(GPIO_CRH_MODE9 |
                    GPIO_CRH_CNF9 |
                    GPIO_CRH_MODE10 |
                    GPIO_CRH_CNF10);

    GPIOA->CRH |= GPIO_CRH_MODE9_1;
    GPIOA->CRH |= GPIO_CRH_CNF9_1;
    GPIOA->CRH |= GPIO_CRH_CNF10_0;

    USART1->BRR = 0x0271; /* 72MHz / 115200 */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static uint8_t usart1_read_byte(void)
{
    /*
     * 阻塞等待 USART1 收到一个完整字节。
     *
     * 前置条件：usart1_init() 已打开 USART1 时钟和 CR1.RE，
     *           PA10 已配成输入，USB-TTL 的 TX 已接 PA10 且共地。
     *
     * 轮询 RXNE 直到硬件置位，表示 DR 中已有新字节。
     * 读 DR 会同时清除 RXNE，为下一次接收做准备。
     *
     * 错误后果：如果 PA10 悬空或波特率不匹配，RXNE 永远不置位，
     *           main() 会卡死在这里，LED 没有任何响应。
     */
    while ((USART1->SR & USART_SR_RXNE) == 0U) {
    }
    return (uint8_t)USART1->DR;
}

static void handle_packet(uint8_t cmd, uint8_t data)
{
    /*
     * 协议层 → 动作层的分界函数。
     *
     * 前置条件：状态机已确认收到完整一帧（AA/CMD/DATA/55 都正确），
     *           cmd 和 data 已从帧中提取出来。
     *
     * 目的：把协议解析和业务动作分开。状态机只负责"收到什么"，本函数只负责"做什么"。
     *       后续增加蜂鸣器、PWM、传感器命令时，只扩展这个函数，不碰状态机。
     *
     * CMD=0x01：控制板载 LED。DATA!=0 时点亮，DATA=0 时熄灭。
     *
     * 错误后果：如果 cmd 不是 0x01，函数什么都不做，LED 保持不变。
     *           这是设计意图——未定义的命令静默忽略，不产生副作用。
     */
    if (cmd == 0x01U) {
        led_set(data);
    }
}

static void packet_fsm_input(PacketState *state, uint8_t byte, uint8_t *cmd, uint8_t *data)
{
    /*
     * 协议状态机：每次处理 1 个字节，根据当前状态决定这个字节的含义。
     *
     * 前置条件：usart1_read_byte() 已返回一个有效字节，调用者保证 byte 是从 DR 读到的。
     *
     * 目的：把连续字节流变成结构化数据包。UART 硬件只负责给字节，本函数负责判断
     *       每个字节在当前帧中的角色（帧头/命令/数据/帧尾）。
     *
     * 状态切换逻辑：
     *   WAIT_HEAD → 收到 0xAA 时进入 WAIT_CMD，否则留在 WAIT_HEAD（丢弃无关字节）
     *   WAIT_CMD  → 保存 cmd，无条件进入 WAIT_DATA
     *   WAIT_DATA → 保存 data，无条件进入 WAIT_TAIL
     *   WAIT_TAIL → 收到 0x55 时执行 handle_packet()，无论正确与否都回到 WAIT_HEAD
     *
     * 错误后果：
     * - 帧尾错误：整帧丢弃不执行，但状态机回到 WAIT_HEAD，下一包不受影响。
     * - 如果在 WAIT_TAIL 后不回 WAIT_HEAD：下一包第一个字节被当成帧尾判断，
     *   状态机永久错位，之后所有帧都无法正确解析。
     */
    switch (*state) {
    case WAIT_HEAD:
        /*
         * 等包头 0xAA。
         * 只要不是包头，就丢弃。这样即使上电时从半包中间开始接收，
         * 也能在下一个 0xAA 到来时重新同步。
         */
        if (byte == 0xAAU) {
            *state = WAIT_CMD;
        }
        break;

    case WAIT_CMD:
        /*
         * 包头后第 1 个字节解释为命令码。
         * 这里先不判断命令是否合法，因为不同命令可能有不同数据含义。
         */
        *cmd = byte;
        *state = WAIT_DATA;
        break;

    case WAIT_DATA:
        /*
         * 命令后的 1 个字节是参数数据。
         * 本课固定长度 4 字节，所以收到 DATA 后就去等包尾。
         */
        *data = byte;
        *state = WAIT_TAIL;
        break;

    case WAIT_TAIL:
        /*
         * 包尾必须是 0x55。
         * 包尾正确才执行命令；包尾错误说明这一帧坏了，直接丢弃并重新等包头。
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
    uint8_t cmd = 0U;
    uint8_t data = 0U;

    system_clock_72mhz_init();
    pc13_led_init();
    usart1_init();

    while (1) {
        uint8_t byte = usart1_read_byte();
        packet_fsm_input(&state, byte, &cmd, &data);
    }
}