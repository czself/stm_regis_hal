#include "stm32f1xx.h"

/*
 * ============================================================================
 * 寄存器版 RTC（实时时钟）基础实验
 * ============================================================================
 *
 * ██████  本课核心知识点 ██████
 *
 * 1. RTC（Real-Time Clock）
 *    - RTC 不是普通定时器！它位于"备份域"（Backup Domain）
 *    - 备份域有独立的供电和时钟源，可以在主电源掉电后仍保持计时
 *    - 需要额外的配置：备份域访问权限、时钟源选择、寄存器同步
 *
 * 2. 备份域的概念
 *    - STM32F1 的备份域包含：RTC 计数器、备份寄存器（BKP）
 *    - 备份域寄存器和普通 APB 外设不同，写操作需要先解锁
 *    - 解锁方式：PWR->CR |= PWR_CR_DBP（Disable Backup domain Protection）
 *    - 如果不先解锁，对 RTC 相关寄存器的写操作会被忽略
 *
 * 3. RTC 时钟源
 *    - LSI（约 40kHz，内部低速，精度差）：不需要外部晶振
 *    - LSE（32.768kHz，外部低速，精度高）：需要外部晶振
 *    - HSE/128（外部分频后得到时钟）
 *    - 本课使用 LSI，因为不依赖外部硬件
 *
 * 4. RTC 寄存器同步
 *    - RTC 在备份域时钟域，APB 总线在另一个时钟域
 *    - 读取 RTC 寄存器前，必须等待同步完成
 *    - 通过检查 RTC->CRL.RSF（Registers Synchronization Flag）
 *    - 同步完成意味着 APB 侧读到的值是当前真实值
 *
 * 5. 配置模式（CNF）
 *    - 修改 RTC->PRL（预分频）和 RTC->CNT（计数器）时，必须先进入配置模式
 *    - CRL.CNF = 1：进入配置模式
 *    - CRL.CNF = 0：退出配置模式（此时寄存器更新生效）
 *    - 退出配置模式时需要等待 RTOFF 标志
 *
 * ██████  计数器读取一致性 ██████
 *
 * RTC->CNT 是 32 位，但分成 CNTH（高 16 位）和 CNTL（低 16 位）。
 * 如果只读一次，可能刚好在高低位翻转时读取，导致读到错误值。
 * 解决方法：连续读两次高 16 位，如果两次相等，说明高低位一致。
 * 这和 RTC 读取最佳实践（见 rtc_get_counter 函数）。
 *
 * ██████  Demo 演示现象 ██████
 *
 * - 使用 LSI 约 40kHz，配置预分频 40000-1 后得到约 1 秒节拍
 * - PC13 LED 每秒翻转一次（亮→灭或灭→亮）
 * - 注意：LSI 精度差，实际时间可能偏快或偏慢
 */

static void led_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;                    /* 打开 GPIOC 时钟 */
    GPIOC->CRH &= ~(GPIO_CRH_MODE13 | GPIO_CRH_CNF13);     /* 清除旧配置 */
    GPIOC->CRH |= GPIO_CRH_MODE13_1;                        /* 输出模式 2MHz */
    GPIOC->BSRR = GPIO_BSRR_BS13;                           /* 初始：LED 灭 */
}

static void led_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

/*
 * RTC 寄存器访问的辅助函数
 *
 * RTC 位于备份域，和 APB 总线是不同时钟域。
 * 修改 RTC 寄存器前必须等待"上一次写操作完成"（RTOFF = 1），
 * 然后进入配置模式（CNF = 1），写完寄存器后退出配置模式（CNF = 0）。
 *
 * RTOFF（RTC Operation OFF）= 1 表示没有正在进行的写操作，可以开始新写操作。
 */
static void rtc_wait_ready(void)
{
    /* 等待 RTC 完成上一次写操作 */
    while ((RTC->CRL & RTC_CRL_RTOFF) == 0U) {
    }
}

static void rtc_enter_config(void)
{
    rtc_wait_ready();          /* 确保上一次写操作已完成 */
    RTC->CRL |= RTC_CRL_CNF;  /* 进入配置模式，允许修改 PRL 和 CNT */
}

static void rtc_exit_config(void)
{
    RTC->CRL &= ~RTC_CRL_CNF; /* 退出配置模式 */
    rtc_wait_ready();          /* 等待写操作完成 */
}

/*
 * 读取 RTC 32 位计数器（保证高低位一致性）
 *
 * 为什么不能直接读？RTC->CNT 分成 CNTH（高 16 位）和 CNTL（低 16 位）。
 * 假设计数器从 0x0000FFFF → 0x00010000：
 *   如果先读 CNTH=0，然后低 16 位从 0xFFFF 溢出到 0x0000，
 *   再读 CNTL=0，结果会得到 0x00000000（错误！应该是 0x00010000）
 *
 * 解决方案（do-while 循环）：
 *   1. 读 CNTH → high1
 *   2. 读 CNTL → low
 *   3. 再读 CNTH → high2
 *   4. 如果 high1 == high2，说明这两次读 CNTH 期间没有溢出，数据有效
 *   5. 如果不相等，重新读取（循环概率极低）
 */
static uint32_t rtc_get_counter(void)
{
    uint32_t high1;
    uint32_t low;
    uint32_t high2;

    do {
        high1 = RTC->CNTH;
        low = RTC->CNTL;
        high2 = RTC->CNTH;
    } while (high1 != high2);

    return (high1 << 16) | low;
}

/*
 * RTC 初始化（使用 LSI 时钟源）
 *
 * 完整初始化序列：
 * 1. 打开 PWR 和 BKP 时钟
 * 2. 解锁备份域（PWR_CR.DBP = 1）
 * 3. 启动 LSI 并等待稳定
 * 4. 首次配置：复位备份域 → 选择 LSI → 使能 RTC → 配置预分频和计数器
 * 5. 已配置：仅等待同步
 */
static void rtc_lsi_init(void)
{
    /*
     * 第一步：打开 PWR 和 BKP 时钟，解锁备份域
     * PWR（Power Control）时钟：用于配置电源控制寄存器
     * BKP（Backup Registers）时钟：用于访问备份域寄存器
     */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;  /* 解锁备份域写保护，否则无法修改 RTC 相关寄存器 */

    /*
     * 第二步：启动 LSI 并等待稳定
     * LSI（Low Speed Internal）约 40kHz，精度不高但独立性强
     * LSIRDY = 1 表示 LSI 已稳定，可以作为时钟源
     */
    RCC->CSR |= RCC_CSR_LSION;
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
    }

    /*
     * 第三步：检查 RTC 是否已经使能
     * BDCR.RTCEN = 0：RTC 从未配置，需要完整初始化
     * BDCR.RTCEN = 1：RTC 已在运行，只等待同步
     *
     * 这个判断可以处理两种场景：
     * - 首次上电或备份域复位后
     * - 下载程序后 RTC 已在运行（上次配置保留在备份域）
     */
    if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
        /*
         * ████ 首次配置 ████
         *
         * BDRST（Backup Domain Reset）：复位整个备份域
         * 先置 1 再清 0，确保备份域处于干净状态
         * 复位后 RTC 计数器、预分频、备份寄存器全部清零
         */
        RCC->BDCR |= RCC_BDCR_BDRST;
        RCC->BDCR &= ~RCC_BDCR_BDRST;

        /*
         * 选择 LSI 作为 RTC 时钟源（RTCSEL = 01）
         * 使能 RTC（RTCEN = 1）
         */
        RCC->BDCR |= RCC_BDCR_RTCSEL_LSI;
        RCC->BDCR |= RCC_BDCR_RTCEN;

        /*
         * 等待 RTC 寄存器同步
         * 使能 RTC 后，APB 侧需要等待同步才能读到正确值
         * RSF = 1 表示同步完成
         */
        RTC->CRL &= ~RTC_CRL_RSF;  /* 清除同步标志 */
        while ((RTC->CRL & RTC_CRL_RSF) == 0U) {
        }

        /*
         * 进入配置模式，设置预分频和清零计数器
         *
         * PRL（Prescaler Reload）= 40000 - 1
         *   RTC 时钟 / (PRL + 1) = 40kHz / 40000 = 1Hz
         *   即每 1 秒计数器递增 1
         *
         * PRL 共 20 位：PRLL 是低 16 位，PRLH 是高 4 位
         * CNTH/CNTL = 计数器清零
         */
        rtc_enter_config();
        RTC->PRLH = 0U;           /* PRL 高位 = 0 */
        RTC->PRLL = 40000U - 1U;  /* PRL 低位 = 39999（40kHz / 40000 = 1Hz） */
        RTC->CNTH = 0U;           /* 计数器清零 */
        RTC->CNTL = 0U;
        rtc_exit_config();
    } else {
        /*
         * RTC 已经在运行，只需要等待同步
         * 可能是复位后 RTC 保留了上次的配置（备份域有电池供电）
         */
        RTC->CRL &= ~RTC_CRL_RSF;
        while ((RTC->CRL & RTC_CRL_RSF) == 0U) {
        }
    }
}

int main(void)
{
    uint32_t last_second;

    led_init();
    rtc_lsi_init();

    /*
     * 读取当前 RTC 计数器的值作为参考
     * 以后每读一次，如果值变化了，说明过了 1 秒（预分频 = 40000 → 1Hz）
     *
     * 主循环中没有 delay，因为 RTC 计数器是硬件自动递增的，
     * 程序只需要不断读取计数器值，和上次的值比较即可。
     * 这和用 TIM2 做周期任务不同：
     * - TIM2 是中断驱动（事件驱动）
     * - RTC 是轮询驱动（不断读取硬件计数器）
     */
    last_second = rtc_get_counter();

    while (1) {
        uint32_t now = rtc_get_counter();
        if (now != last_second) {
            last_second = now;
            led_toggle();  /* 每秒翻转一次 LED */
        }
    }
}
