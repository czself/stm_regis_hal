#include "stm32f1xx_hal.h"

/*
 * 本文件是"HAL版 bxCAN 内部回环实验"。
 *
 * 本课目标：
 * 1. 使用 HAL 初始化 CAN1 为内部回环模式
 * 2. 配置一个全放行过滤器
 * 3. 周期性发送标准帧，再从 FIFO0 读回校验
 *
 * 与寄存器版的对应关系：
 *   HAL_CAN_Init()      → CAN1->MCR + CAN1->BTR（含 LBKM）
 *   HAL_CAN_ConfigFilter() → 过滤器各寄存器（FMR/FA1R/FS1R 等）
 *   HAL_CAN_AddTxMessage() → 写 TIR/TDTR/TDLR/TDHR + 置 TXRQ
 *   HAL_CAN_GetRxMessage() → 读 RIR/RDTR/RDLR/RDHR + 写 RFOM0
 *
 * 理解寄存器版有助于理解 HAL 函数内部在做什么。
 */

#define CAN_STD_ID    0x123U
#define CAN_DLC       2U
#define CAN_PERIOD_MS 1000U

static CAN_HandleTypeDef hcan;

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void led_on(void);
static void led_toggle(void);
static void can_gpio_init(void);
static void can1_init(void);
static void can1_filter_init(void);
static void error_handler(void);

/*
 * main —— 主函数
 *
 * CAN 内部回环流程：
 *   1. HAL_CAN_Start() — 启动 CAN
 *   2. 填充 TxHeader（ID、DLC、IDE、RTR）
 *   3. HAL_CAN_AddTxMessage() — 添加到发送邮箱
 *   4. HAL_CAN_GetRxFifoFillLevel() — 检查 FIFO0
 *   5. HAL_CAN_GetRxMessage() — 从 FIFO0 取回
 *   6. 校验 ID/DLC/数据 → 控制 LED
 *
 * 注意：如果连续收到 FIFO 满（HAL_CAN_AddTxMessage 成功但
 * HAL_CAN_GetRxFifoFillLevel 一直为 0），先检查过滤器是否激活、
 * 是否分配到 FIFO0、模式是否为 LOOPBACK。内部回环最常出错的
 * 不是物理连线而是配置遗漏。
 */
int main(void)
{
    CAN_TxHeaderTypeDef tx_header = {0};
    CAN_RxHeaderTypeDef rx_header = {0};
    uint8_t tx_data[8] = {0xA5U, 0x3CU, 0U, 0U, 0U, 0U, 0U, 0U};
    uint8_t rx_data[8] = {0};
    uint32_t tx_mailbox = 0U;

    HAL_Init();

    system_clock_72mhz_init();
    led_pc13_init();
    can_gpio_init();
    can1_init();
    can1_filter_init();

    /*
     * HAL_CAN_Start() —— 启动 CAN 通信
     *
     * 内部做了什么：
     *   1. 清除 MCR.SLEEP（唤醒）
     *   2. 等待 MSR.SLAK = 0（退出睡眠模式）
     *   3. 清除 MCR.INRQ 退出初始化（如果在初始化模式中）
     *
     * 对应寄存器版：can1_leave_init_mode() 的部分操作。
     */
    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        error_handler();
    }

    /*
     * 填充发送报文头
     *
     * CAN_TxHeaderTypeDef 各成员：
     *   StdId：标准帧 ID（11 位）
     *   ExtId：扩展帧 ID（本课不用）
     *   IDE：ID 类型（CAN_ID_STD = 标准帧）
     *   RTR：帧类型（CAN_RTR_DATA = 数据帧）
     *   DLC：数据长度（0~8）
     *   TransmitGlobalTime：时间触发模式（本课关闭）
     */
    tx_header.StdId = CAN_STD_ID;
    tx_header.ExtId = 0U;
    tx_header.IDE = CAN_ID_STD;
    tx_header.RTR = CAN_RTR_DATA;
    tx_header.DLC = CAN_DLC;
    tx_header.TransmitGlobalTime = DISABLE;

    while (1) {
        /*
         * HAL_CAN_AddTxMessage() —— 添加报文到发送邮箱
         *
         * 参数：
         *   1. CAN 句柄
         *   2. 报文头指针（ID、DLC 等）
         *   3. 数据缓冲区指针
         *   4. 输出参数：使用的邮箱编号
         *
         * 内部对应寄存器版 can1_send_std_data() 中的：
         *   写 TIR（含 ID 左移 21 位）
         *   写 TDTR（DLC）
         *   写 TDLR/TDHR（数据）
         *   置 TXRQ（请求发送）
         */
        if (HAL_CAN_AddTxMessage(&hcan, &tx_header, tx_data, &tx_mailbox) != HAL_OK) {
            led_on();
            HAL_Delay(CAN_PERIOD_MS);
            continue;
        }

        /*
         * HAL_CAN_GetRxFifoFillLevel() —— 查询 FIFO 中的报文数
         *
         * 对应寄存器版：读取 RF0R.FMP0
         *   返回值 = 0：FIFO 空
         *   返回值 > 0：FIFO 中有报文
         *
         * 本课用轮询方式等待回环报文。
         */
        while (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
        }

        /*
         * HAL_CAN_GetRxMessage() —— 从 FIFO 取出报文
         *
         * 参数：
         *   1. CAN 句柄
         *   2. FIFO 编号
         *   3. 接收报文头指针（输出：ID、DLC 等）
         *   4. 接收数据缓冲区指针
         *
         * 内部对应寄存器版 can1_receive_fifo0() 中的：
         *   读 RIR/RDTR/RDLR/RDHR
         *   写 RF0R.RFOM0（释放 FIFO 槽位）
         */
        if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &rx_header, rx_data) != HAL_OK) {
            led_on();
            HAL_Delay(CAN_PERIOD_MS);
            continue;
        }

        /* 校验 */
        if ((rx_header.StdId == CAN_STD_ID) &&
            (rx_header.IDE == CAN_ID_STD) &&
            (rx_header.DLC == CAN_DLC) &&
            (rx_data[0] == tx_data[0]) &&
            (rx_data[1] == tx_data[1])) {
            led_toggle();
        } else {
            led_on();
        }

        /* 交替测试数据 */
        if (tx_data[0] == 0xA5U) {
            tx_data[0] = 0x5AU;
            tx_data[1] = 0xC3U;
        } else {
            tx_data[0] = 0xA5U;
            tx_data[1] = 0x3CU;
        }

        HAL_Delay(CAN_PERIOD_MS);
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

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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

static void led_on(void)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
}

static void led_toggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
}

static void can_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA12 = CAN_TX：复用推挽输出 */
    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PA11 = CAN_RX：输入 */
    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

/*
 * can1_init —— HAL 版 CAN1 初始化
 *
 * 各配置项与寄存器版的对应关系：
 *
 * Prescaler = 4        → BTR.BRP = 3（实际值 4 分频）
 * Mode = LOOPBACK       → BTR.LBKM = 1（内部回环）
 * SyncJumpWidth = 1TQ   → BTR.SJW = 0（实际 1 tq）
 * TimeSeg1 = 13TQ       → BTR.TS1 = 12（实际 13 tq）
 * TimeSeg2 = 4TQ        → BTR.TS2 = 3（实际 4 tq）
 *
 * 位时序计算：
 *   tq 总数 = 1 + 13 + 4 = 18
 *   CAN 速率 = 36MHz / (4 × 18) = 500kbps
 *
 * Prescaler 填 0 会导致分频比为 1，CAN 速率变为 36MHz / 18 = 2Mbps，
 * 远超出 F103 bxCAN 最大 1Mbps 规格，通信不可靠。
 * TimeSeg1/TimeSeg2 分配不均（如 TS1 太小 TS2 太大）会导致采样点
 * 位置偏差，真实总线上会因位同步错误产生 CRC 校验失败。
 */
static void can1_init(void)
{
    __HAL_RCC_CAN1_CLK_ENABLE();

    hcan.Instance = CAN1;
    hcan.Init.Prescaler = 4U;
    hcan.Init.Mode = CAN_MODE_LOOPBACK;
    hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan.Init.TimeSeg1 = CAN_BS1_13TQ;
    hcan.Init.TimeSeg2 = CAN_BS2_4TQ;
    hcan.Init.TimeTriggeredMode = DISABLE;
    hcan.Init.AutoBusOff = DISABLE;
    hcan.Init.AutoWakeUp = DISABLE;
    hcan.Init.AutoRetransmission = ENABLE;
    hcan.Init.ReceiveFifoLocked = DISABLE;
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        error_handler();
    }
}

/*
 * can1_filter_init —— HAL 版配置全放行过滤器
 *
 * 各配置项与寄存器版的对应关系：
 *
 * FilterBank = 0            → 使用过滤器组 0
 * FilterMode = IDMASK       → FM1R.FBM0 = 0（掩码模式）
 * FilterScale = 32BIT       → FS1R.FSC0 = 1（32 位）
 * FilterIdHigh/Low = 0      → sFilterRegister[0].FR1 = 0
 * FilterMaskIdHigh/Low = 0  → sFilterRegister[0].FR2 = 0
 * FilterFIFOAssignment = 0  → FFA1R.FFA0 = 0（分配到 FIFO0）
 * FilterActivation = ENABLE → FA1R.FACT0 = 1（激活）
 *
 * 全 0 掩码 = 任何 ID 都放行。
 *
 * 如果 FilterActivation 不设为 ENABLE，过滤器不工作，
 * 所有报文（包括回环报文）都不会进入 FIFO0。
 * 此时 HAL_CAN_GetRxFifoFillLevel 永远返回 0，
 * 主循环会卡在 while 等待中。
 */
static void can1_filter_init(void)
{
    CAN_FilterTypeDef filter = {0};

    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0x0000;
    filter.FilterIdLow = 0x0000;
    filter.FilterMaskIdHigh = 0x0000;
    filter.FilterMaskIdLow = 0x0000;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;

    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK) {
        error_handler();
    }
}

static void error_handler(void)
{
    __disable_irq();
    while (1) {
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}
