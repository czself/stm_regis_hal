#include "stm32f1xx.h"

/*
 * 本文件是"寄存器版 bxCAN 内部回环实验"。
 *
 * 本课目标：
 * 1. 配置 CAN1 工作在内部回环模式（不需要外部收发器）
 * 2. 发送一帧标准帧（ID=0x123, DLC=2, data=0xA5/0x3C）
 * 3. 从 FIFO0 里读回，校验 ID、DLC、数据
 * 4. 建立对发送邮箱、过滤器、FIFO0 的基本认识
 *
 * CAN vs UART/SPI/I2C 核心区别：
 *   UART/SPI/I2C：字节流或寄存器读写，点对点或主从
 *   CAN：报文总线，靠报文 ID 表达含义和优先级，多节点共享总线
 *
 * 本课使用"内部回环模式（Loopback Mode）"：
 *   CAN 控制器内部将 TX 数据直接环回到 RX 路径。
 *   报文会经历完整控制器路径：发送邮箱 → 总线（内部）→ 过滤器 → FIFO0
 *   但不依赖外部收发器和 CANH/CANL 总线。
 *   这是学习 CAN 控制器最干净的起点。
 *
 * bxCAN 的基本概念：
 *   - 发送邮箱：3 个（Mailbox 0/1/2），临时存放待发送报文
 *   - 接收 FIFO：2 个（FIFO0/FIFO1），存放已接收报文
 *   - 过滤器：14 个（Filter Bank 0~13），决定收哪些报文
 *   - 位时序（BTR）：决定 CAN 通信速率（500kbps）
 */

#define CAN_STD_ID        0x123U         /* 标准帧 ID（11 位） */
#define CAN_DLC           2U             /* 数据长度码 */
#define CAN_PERIOD_MS     1000U
#define CAN_TIMEOUT_COUNT 100000U

static volatile uint32_t g_ms_ticks = 0U;

static void system_clock_72mhz_init(void);
static void systick_init(void);
static void delay_ms(uint32_t ms);
static void led_pc13_init(void);
static void led_on(void);
static void led_off(void);
static void led_toggle(void);
static void can_gpio_init(void);
static uint8_t can1_enter_init_mode(void);
static uint8_t can1_leave_init_mode(void);
static void can1_filter_init_accept_all(void);
static uint8_t can1_init(void);
static uint8_t can1_send_std_data(uint16_t std_id, const uint8_t *data, uint8_t len);
static uint8_t can1_receive_fifo0(uint16_t *std_id, uint8_t *len, uint8_t *data);

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

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

    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
}

static void systick_init(void)
{
    SysTick->LOAD = 72000U - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk |
                    SysTick_CTRL_ENABLE_Msk;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms_ticks;
    while ((g_ms_ticks - start) < ms) {
    }
}

static void led_pc13_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);
    GPIOC->CRH |= GPIO_CRH_MODE13_1;
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_on(void)
{
    GPIOC->BRR = GPIO_BRR_BR13;
}

static void led_off(void)
{
    GPIOC->BSRR = GPIO_BSRR_BS13;
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        led_on();
    } else {
        led_off();
    }
}

/*
 * can_gpio_init —— 配置 CAN1 引脚
 *
 * CAN1 的默认引脚映射：
 *   PA12 → CAN1_TX（发送）→ 复用推挽输出
 *   PA11 → CAN1_RX（接收）→ 输入（浮空）
 *
 * CAN_TX 用推挽（CNF=10）而不是开漏，因为 CAN_TX 是 STM32→收发器的
 * 单向数字输出，不存在多设备竞争。I2C 才需要开漏防冲突。
 * 如果错配成开漏（CNF=11），TX 信号在推高时依赖上拉电阻，
 * 波形上升沿变缓，可能不满足收发器输入要求。如果错配成通用推挽
 * 而非复用推挽（CNF=00），引脚由 GPIO 输出寄存器控制，CAN 控制器
 * 无法驱动总线。
 *
 * 虽然本课使用内部回环模式不需要外部引脚，
 * 但仍按真实 CAN 引脚方式初始化，便于后续无缝过渡到总线实验。
 */
static void can_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;  /* CAN1 挂在 APB1 */

    /* PA12 = CAN_TX：复用推挽输出 */
    GPIOA->CRH &= ~(GPIO_CRH_MODE12 | GPIO_CRH_CNF12);
    GPIOA->CRH |= GPIO_CRH_MODE12;        /* MODE12 = 11 */
    GPIOA->CRH |= GPIO_CRH_CNF12_1;       /* CNF12 = 10 */

    /* PA11 = CAN_RX：浮空输入 */
    GPIOA->CRH &= ~(GPIO_CRH_MODE11 | GPIO_CRH_CNF11);
    GPIOA->CRH |= GPIO_CRH_CNF11_0;       /* CNF11 = 01 */
}

/*
 * can1_enter_init_mode —— 请求进入 CAN 初始化模式
 *
 * 在 F103 的 bxCAN 中，很多关键配置（如 BTR、过滤器）
 * 只能在初始化模式下修改。所以进入/退出初始化模式是一个基本操作。
 *
 * 流程：
 *   1. 写 MCR.INRQ = 1（请求进入初始化模式）
 *   2. 等待 MSR.INAK = 1（硬件确认已进入）
 *
 * INRQ（Initialization Request，MCR bit 0）：
 *   0 = 正常模式，1 = 请求初始化
 * INAK（Initialization Acknowledge，MSR bit 0）：
 *   0 = 不在初始化模式，1 = 已进入初始化模式
 *
 * 返回：1 = 成功，0 = 超时
 */
static uint8_t can1_enter_init_mode(void)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;

    CAN1->MCR |= CAN_MCR_INRQ;

    while (((CAN1->MSR & CAN_MSR_INAK) == 0U) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 1U : 0U;
}

/*
 * can1_leave_init_mode —— 退出初始化模式
 *
 * 流程：
 *   1. 清 MCR.INRQ = 0
 *   2. 等待 MSR.INAK = 0（硬件确认已退出）
 */
static uint8_t can1_leave_init_mode(void)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;

    CAN1->MCR &= ~CAN_MCR_INRQ;

    while (((CAN1->MSR & CAN_MSR_INAK) != 0U) && (timeout > 0U)) {
        timeout--;
    }

    return (timeout > 0U) ? 1U : 0U;
}

/*
 * can1_filter_init_accept_all —— 配置"全放行"过滤器
 *
 * 过滤器是 CAN 独有的概念。UART/SPI/I2C 都没有类似机制。
 *
 * 为什么需要过滤器？
 *   在 CAN 总线上，所有节点的报文都会被所有节点收到。
 *   但大多数节点只关心特定 ID 的报文。
 *   过滤器可以硬件筛选——不匹配的报文直接丢弃，CPU 无需参与。
 *
 * 本课配置"全放行"：所有报文都允许通过。
 *
 * 过滤器配置涉及多个寄存器：
 *   FMR（Filter Master Register）：过滤器主控
 *     FINIT（bit 0）= 1：进入过滤器初始化模式
 *   FA1R（Filter Active Register）：激活状态
 *     FACT0（bit 0）= 1：激活过滤器 0
 *   FS1R（Filter Scale Register）：尺度
 *     FSC0（bit 0）= 1：32 位尺度
 *   FM1R（Filter Mode Register）：模式
 *     FBM0（bit 0）= 0：掩码模式（ID + 掩码匹配）
 *   FFA1R（Filter FIFO Assignment）：FIFO 分配
 *     FFA0（bit 0）= 0：过滤器 0 分配给 FIFO0
 *   sFilterRegister[n]：过滤器组寄存器
 *     FR1：32 位 ID 或掩码配置
 *     FR2：32 位掩码或 ID 配置
 *
 * 全放行配置方法：
 *   掩码全 0：任何 ID 都匹配（FR1=0, FR2=0）
 */
static void can1_filter_init_accept_all(void)
{
    /* 进入过滤器初始化模式 */
    CAN1->FMR |= CAN_FMR_FINIT;

    /* 关闭过滤器 0 */
    CAN1->FA1R &= ~CAN_FA1R_FACT0;

    /* 32 位尺度 */
    CAN1->FS1R |= CAN_FS1R_FSC0;

    /* 掩码模式（不是列表模式） */
    CAN1->FM1R &= ~CAN_FM1R_FBM0;

    /* 分配到 FIFO0 */
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0;

    /* 全 0 → 全部放行 */
    CAN1->sFilterRegister[0].FR1 = 0x00000000U;
    CAN1->sFilterRegister[0].FR2 = 0x00000000U;

    /* 激活过滤器 0 */
    CAN1->FA1R |= CAN_FA1R_FACT0;

    /* 退出过滤器初始化模式 */
    CAN1->FMR &= ~CAN_FMR_FINIT;
}

/*
 * can1_init —— 初始化 CAN1（内部回环模式，500kbps）
 *
 * 初始化流程：
 *   1. 请求进入初始化模式
 *   2. 配置 BTR（位时序 + 回环模式）
 *   3. 配置全放行过滤器
 *   4. 退出初始化模式
 *
 * BTR（Bit Timing Register）详解：
 *   CAN 通信速率由 PCLK1、分频器（BRP）、时间段（TS1/TS2）共同决定。
 *
 *   本课配置：
 *     PCLK1 = 36MHz（CAN1 挂在 APB1）
 *     BRP（Baud Rate Prescaler）= 4（BTR[9:0] = 3）
 *     TS1（Time Segment 1）= 13 tq（BTR[19:16] = 12）
 *     TS2（Time Segment 2）= 4 tq（BTR[22:20] = 3）
 *     SJW（Sync Jump Width）= 1 tq（默认）
 *
 *   计算公式：
 *     每 bit 的 tq 数 = 1 (Sync) + TS1 + TS2 = 1 + 13 + 4 = 18
 *     CAN 速率 = PCLK1 / (BRP × tq 总数) = 36MHz / (4 × 18) = 500kbps
 *
 *   如果 BRP/TS1/TS2 算错，CAN 控制器和外部节点的位采样点不匹配，
 *   会导致总线错误（REC/TEC 递增），最终进入 Bus Off 状态。
 *   本课内部回环不需要和其他节点同步，即使 BTR 有微小偏差也能
 *   正常通信。这是内部回环的"宽容"特性——真实总线必须精确计算。
 *
 *   LBKM（Loopback Mode，BTR bit 30）：
 *     内部回环模式，TX 内部连接到 RX，不经过外部引脚。
 *     报文会经过完整的发送邮箱 → 过滤器 → FIFO 路径。
 *
 * 注意：BTR 只能在初始化模式下修改（INRQ=1, INAK=1）。
 *   如果在普通模式下写 BTR，写入被忽略，CAN 时序不变。
 */
static uint8_t can1_init(void)
{
    if (can1_enter_init_mode() == 0U) {
        return 0U;
    }

    /*
     * MCR 配置：
     * 本课使用最简单的配置，只保留 INRQ=1。
     * 其他特性（时间触发、自动唤醒、自动重传等）全部关闭。
     */
    CAN1->MCR = CAN_MCR_INRQ;

    /*
     * BTR 配置：
     *   LBKM（bit 30）= 1：内部回环模式
     *   BRP[9:0]（bit 0-9）= 3：分频系数 = 4（实际值 = BRP + 1）
     *   TS1[3:0]（bit 16-19）= 12：时间段 1 = 13 tq
     *   TS2[2:0]（bit 20-22）= 3：时间段 2 = 4 tq
     *
     * 为什么这些位要"减 1"？
     *   因为硬件实际值 = 寄存器值 + 1
     *   BRP=3 → 4 分频
     *   TS1=12 → 13 tq
     *   TS2=3 → 4 tq
     */
    CAN1->BTR = CAN_BTR_LBKM |                    /* 内部回环模式 */
                (3U << CAN_BTR_BRP_Pos) |          /* BRP = 4 */
                (12U << CAN_BTR_TS1_Pos) |         /* TS1 = 13 tq */
                (3U << CAN_BTR_TS2_Pos);           /* TS2 = 4 tq */

    /* 配置全放行过滤器 */
    can1_filter_init_accept_all();

    if (can1_leave_init_mode() == 0U) {
        return 0U;
    }

    return 1U;
}

/*
 * can1_send_std_data —— 发送一帧标准数据帧
 *
 * 发送邮箱（Tx Mailbox）：
 *   F103 的 bxCAN 有 3 个发送邮箱（编号 0/1/2）。
 *   每个邮箱可以存放一帧待发送的报文。
 *   本课使用邮箱 0（最简单的做法）。
 *
 * 标准帧的邮箱寄存器（以邮箱 0 为例）：
 *   sTxMailBox[0].TIR（Tx Identifier Register）：
 *     STID[10:0]（bit 31-21）：标准帧 ID
 *     IDE（bit 2）：ID 扩展位（0=标准帧，1=扩展帧）
 *     TXRQ（bit 0）：发送请求（写 1 启动发送）
 *   sTxMailBox[0].TDTR（Tx Data Length & Time Register）：
 *     DLC[3:0]（bit 0-3）：数据长度（0~8）
 *   sTxMailBox[0].TDLR（Tx Data Low Register）：
 *     存放 data[0]~data[3]
 *   sTxMailBox[0].TDHR（Tx Data High Register）：
 *     存放 data[4]~data[7]
 *
 * 为什么 STID 要左移 21 位？
 *   在 TIR 寄存器中，标准帧 ID 存放在 bit 31~21。
 *   所以需要把 11 位的 std_id 左移到 bit 21~31。
 *   如果忘记左移或位移量算错，ID 字段放错位置，
 *   接收方读到的 ID 和发送方写入的不同，校验必然失败。
 *   本课内部回环中表现为 LED 点亮不翻转。
 *
 * 为什么先等 TSR.TME0 再写？
 *   邮箱 0 正在发送上一帧时不能写入新报文。
 *   TSR.TME0=1 保证邮箱空闲。不等待直接写入会破坏正在发送的报文，
 *   导致 CRC 错误或数据混乱。
 *
 * 参数：
 *   std_id：11 位标准帧 ID
 *   data：数据缓冲区指针
 *   len：数据长度（1~8）
 * 返回：1 = 发送成功，0 = 失败
 */
static uint8_t can1_send_std_data(uint16_t std_id, const uint8_t *data, uint8_t len)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;
    uint32_t tir;

    if ((data == 0) || (len == 0U) || (len > 8U)) {
        return 0U;
    }

    /*
     * 等待邮箱 0 空闲
     * TSR.TME0（Transmit Mailbox Empty）= 1 表示邮箱 0 可写。
     */
    while (((CAN1->TSR & CAN_TSR_TME0) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    if (timeout == 0U) {
        return 0U;
    }

    /*
     * 构建 TIR：
     *   STID 左移 21 位到 bit 31~21
     *   不设 IDE（=0）→ 标准帧
     *   不设 RTR（=0）→ 数据帧
     */
    tir = ((uint32_t)std_id << 21);
    CAN1->sTxMailBox[0].TIR = tir;

    /*
     * DLC = 数据长度。
     * 本课主循环传入 CAN_DLC=2，只发送 data[0] 和 data[1]。
     * 不能因为 CAN 最大 8 字节就无条件读取 data[2]~data[7]，
     * 否则调用者只准备 2 字节缓冲区时会越界读。
     */
    CAN1->sTxMailBox[0].TDTR = len;

    /*
     * 数据写入：
     * TDLR：低 4 字节 [3][2][1][0]
     * TDHR：高 4 字节 [7][6][5][4]
     *
     * 先清零，再按 len 逐字节填入。这样既符合 DLC 的真实含义，
     * 也让初学者看到 CAN 的数据寄存器只是 8 个字节的打包容器。
     */
    CAN1->sTxMailBox[0].TDLR = 0U;
    CAN1->sTxMailBox[0].TDHR = 0U;

    if (len > 0U) {
        CAN1->sTxMailBox[0].TDLR |= (uint32_t)data[0];
    }
    if (len > 1U) {
        CAN1->sTxMailBox[0].TDLR |= ((uint32_t)data[1] << 8);
    }
    if (len > 2U) {
        CAN1->sTxMailBox[0].TDLR |= ((uint32_t)data[2] << 16);
    }
    if (len > 3U) {
        CAN1->sTxMailBox[0].TDLR |= ((uint32_t)data[3] << 24);
    }
    if (len > 4U) {
        CAN1->sTxMailBox[0].TDHR |= (uint32_t)data[4];
    }
    if (len > 5U) {
        CAN1->sTxMailBox[0].TDHR |= ((uint32_t)data[5] << 8);
    }
    if (len > 6U) {
        CAN1->sTxMailBox[0].TDHR |= ((uint32_t)data[6] << 16);
    }
    if (len > 7U) {
        CAN1->sTxMailBox[0].TDHR |= ((uint32_t)data[7] << 24);
    }

    /* TXRQ = 1：请求发送 */
    CAN1->sTxMailBox[0].TIR |= CAN_TI0R_TXRQ;

    return 1U;
}

/*
 * can1_receive_fifo0 —— 从 FIFO0 接收一帧报文
 *
 * FIFO（First In First Out）：
 *   接收 FIFO 用于暂存已接收的报文。
 *   CPU 从 FIFO 中按先进先出的顺序读取报文。
 *   F103 有 2 个 FIFO，每个 FIFO 可存 3 帧报文。
 *
 * FIFO 状态寄存器 RF0R：
 *   FMP0[1:0]（bit 0-1）：FIFO0 中的报文数（0~3）
 *   RFOM0（bit 5）：释放 FIFO0 输出邮箱（写 1 释放当前报文）
 *
 * 接收邮箱寄存器（以 FIFO0 为例）：
 *   sFIFOMailBox[0].RIR：收到的 ID
 *   sFIFOMailBox[0].RDTR：收到的 DLC
 *   sFIFOMailBox[0].RDLR：收到的低 4 字节
 *   sFIFOMailBox[0].RDHR：收到的高 4 字节
 *
 * 为什么不释放 FIFO 会出问题？
 *   RF0R.FMP0 表示 FIFO0 中的报文数。
 *   如果不写 RFOM0 释放，FMP0 不会减少。
 *   当 FIFO 满（3 帧）时，新报文会被丢弃。
 *
 * 参数：
 *   std_id：输出参数，收到的标准帧 ID
 *   len：输出参数，收到的数据长度
 *   data：输出参数，收到的数据缓冲区（至少 8 字节）
 * 返回：1 = 接收成功，0 = 失败
 */
static uint8_t can1_receive_fifo0(uint16_t *std_id, uint8_t *len, uint8_t *data)
{
    uint32_t timeout = CAN_TIMEOUT_COUNT;
    uint32_t rir, rdtr, rdlr, rdhr;

    if ((std_id == 0) || (len == 0) || (data == 0)) {
        return 0U;
    }

    /* 等待 FIFO0 中有报文 */
    while (((CAN1->RF0R & CAN_RF0R_FMP0_Msk) == 0U) && (timeout > 0U)) {
        timeout--;
    }
    if (timeout == 0U) {
        return 0U;
    }

    /* 读取报文寄存器 */
    rir = CAN1->sFIFOMailBox[0].RIR;
    rdtr = CAN1->sFIFOMailBox[0].RDTR;
    rdlr = CAN1->sFIFOMailBox[0].RDLR;
    rdhr = CAN1->sFIFOMailBox[0].RDHR;

    /* 解析标准帧 ID（右移 21 位） */
    *std_id = (uint16_t)(rir >> 21);
    *len = (uint8_t)(rdtr & 0x0FU);

    /* 解析数据 */
    data[0] = (uint8_t)(rdlr & 0xFFU);
    data[1] = (uint8_t)((rdlr >> 8) & 0xFFU);
    data[2] = (uint8_t)((rdlr >> 16) & 0xFFU);
    data[3] = (uint8_t)((rdlr >> 24) & 0xFFU);
    data[4] = (uint8_t)(rdhr & 0xFFU);
    data[5] = (uint8_t)((rdhr >> 8) & 0xFFU);
    data[6] = (uint8_t)((rdhr >> 16) & 0xFFU);
    data[7] = (uint8_t)((rdhr >> 24) & 0xFFU);

    /* 释放 FIFO0 当前报文（槽位释放） */
    CAN1->RF0R |= CAN_RF0R_RFOM0;

    return 1U;
}

/*
 * main —— 主函数
 *
 * 内部回环数据流：
 *   CPU → 发送邮箱 0 → CAN 控制器内部回路
 *   → 过滤器 0（全放行）→ FIFO0 → CPU 读取
 *
 * 初始化流程：时钟 → SysTick → LED → CAN GPIO → CAN 控制器
 * 主循环：发帧 → 收帧 → 校验 → 控制 LED
 */
int main(void)
{
    uint8_t tx_data[8] = {0xA5U, 0x3CU, 0U, 0U, 0U, 0U, 0U, 0U};
    uint8_t rx_data[8] = {0};
    uint8_t rx_len = 0U;
    uint16_t rx_id = 0U;

    system_clock_72mhz_init();
    systick_init();
    led_pc13_init();
    can_gpio_init();

    if (can1_init() == 0U) {
        led_on();
        /*
         * CAN 初始化失败时停在这里。
         * 这样 LED 会保持故障指示状态，后面的发送/接收演示不会带着错误配置继续运行。
         */
        while (1) {
        }
    }

    while (1) {
        /* 发送一帧标准帧 */
        if (can1_send_std_data(CAN_STD_ID, tx_data, CAN_DLC) == 0U) {
            led_on();
            delay_ms(CAN_PERIOD_MS);
            continue;
        }

        /* 从 FIFO0 接收回环报文 */
        if (can1_receive_fifo0(&rx_id, &rx_len, rx_data) == 0U) {
            led_on();
            delay_ms(CAN_PERIOD_MS);
            continue;
        }

        /* 校验 ID、DLC、数据 */
        if ((rx_id == CAN_STD_ID) &&
            (rx_len == CAN_DLC) &&
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

        delay_ms(CAN_PERIOD_MS);
    }
}
