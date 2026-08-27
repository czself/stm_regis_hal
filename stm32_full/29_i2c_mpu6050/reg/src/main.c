#include "stm32f1xx.h"

/*
 * 寄存器版：I2C 读写 MPU6050 寄存器。
 *
 * 前面 26 课已经学过 I2C 总线流程（START/STOP/ACK/ADDR 清除等），
 * 27 课学过软件 I2C 的 bit 级时序。
 *
 * 本课的新重点是"传感器内部寄存器访问"——
 * 和 26 课的 EEPROM 不同，MPU6050 是传感器，不是存储器件。
 * EEPROM 的"内部地址"是存储单元编号，MPU6050 的"内部地址"是功能寄存器编号。
 * 虽然 I2C 访问流程相同，但概念层完全不同。
 *
 * 本课的两步操作：
 * 1. 写 PWR_MGMT_1(0x6B)=0x00 —— 唤醒 MPU6050
 *    MPU6050 上电后默认睡眠（SLEEP bit=1），加速度计和陀螺仪不工作。
 *    这就像手机息屏——屏幕关了但 modem 还在，I2C 接口独立供电所以
 *    WHO_AM_I 仍可读，但测量数据不会更新。写 0x00 清掉 SLEEP bit 唤醒它。
 *
 * 2. 读 WHO_AM_I(0x75) —— 验证通信链路
 *    WHO_AM_I 是芯片的硬编码身份号，只读，永远是 0x68。
 *    它不需要任何配置，是最适合做 I2C 通信自检的寄存器。
 *    读到 0x68 → 供电、接线、地址、I2C 读流程四层都通了。
 *    读不到 0x68 → 问题一定在这四层之一。
 *
 * 为什么读寄存器必须"先写寄存器地址，再重复起始读数据"？
 *    I2C 总线上，器件地址选中"哪颗芯片"，寄存器地址选中"芯片内部哪个功能"。
 *    好比快递员先找到门牌号（器件地址），进门后才说去哪个房间（寄存器地址）。
 *    读操作时，主机必须先用写方向把"房间号"告诉 MPU6050，然后不释放总线，
 *    用重复起始切到读方向，MPU6050 才会把那个房间的数据发出来。
 *    如果跳过"写寄存器地址"这一步，MPU6050 不知道你想读哪个寄存器，
 *    会从它内部地址指针当前指向的寄存器开始发数据（通常是 0x00）。
 *
 * 单字节读的 ACK/STOP 时序为什么重要：
 *    I2C 读数据时，主机每收到一个字节就在第 9 个时钟发 ACK 或 NACK。
 *    ACK=再来一个，NACK=够了别发了。本课只读 1 个字节，必须发 NACK。
 *    STM32F103 的 I2C 外设要求 ACK 位在地址阶段完成前就配置好，
 *    所以代码顺序是：先清 ACK → 发重复起始+读地址 → STOP → 等 RXNE → 读 DR。
 *    顺序错了，轻则多读一个字节，重则总线 BUSY 不释放，下次通信卡死。
 */

#define MPU_ADDR_W 0xD0U
#define MPU_ADDR_R 0xD1U

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

static void pc13_toggle(void)
{
    if ((GPIOC->ODR & GPIO_ODR_ODR13) != 0U) {
        GPIOC->BRR = GPIO_BRR_BR13;
    } else {
        GPIOC->BSRR = GPIO_BSRR_BS13;
    }
}

static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- != 0U) {
        __NOP();
    }
}

/*
 * i2c1_init —— 初始化 I2C1 为 100kHz 标准模式主机（26 课已学，快速过）
 *
 * 关键计算回顾：
 *   PCLK1 = 36MHz → CR2.FREQ = 36
 *   CCR = PCLK1 / (2 × Fscl) = 36MHz / (2 × 100kHz) = 180
 *   TRISE = PCLK1_MHz + 1 = 36 + 1 = 37
 *
 * 先 SWRST 复位 I2C1，再清零 CR1，确保从干净状态开始配置。
 */
static void i2c1_init(void)
{
    /* 开启 GPIOB 和 I2C1 时钟。I2C1 在 APB1（PCLK1=36MHz），不是 APB2 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    /* PB6/PB7 配成复用开漏输出（不是推挽！I2C 必须开漏） */
    GPIOB->CRL &= ~(GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF6 |
                    GPIO_CRL_MODE7 |
                    GPIO_CRL_CNF7);

    GPIOB->CRL |= GPIO_CRL_MODE6 | GPIO_CRL_CNF6;
    GPIOB->CRL |= GPIO_CRL_MODE7 | GPIO_CRL_CNF7;

    /* 软件复位 I2C1，清除可能残留的旧状态 */
    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0U;
    /* 配置时序参数：FREQ=36MHz, CCR=180(100kHz), TRISE=37 */
    I2C1->CR2 = 36U;
    I2C1->CCR = 180U;
    I2C1->TRISE = 37U;
    /* 使能 I2C1 */
    I2C1->CR1 = I2C_CR1_PE;
}

static void i2c_start_and_address(uint8_t address)
{
    I2C1->CR1 |= I2C_CR1_START;
    while ((I2C1->SR1 & I2C_SR1_SB) == 0U) {
    }

    I2C1->DR = address;
    while ((I2C1->SR1 & I2C_SR1_ADDR) == 0U) {
    }

    (void)I2C1->SR1;
    (void)I2C1->SR2;
}

static void i2c_write_byte(uint8_t byte)
{
    while ((I2C1->SR1 & I2C_SR1_TXE) == 0U) {
    }

    I2C1->DR = byte;
}

/*
 * mpu_write_register —— 写 MPU6050 的一个寄存器
 *
 * 流程：START → 器件地址+写 → 寄存器地址 → 数据 → STOP
 *
 * 两层寻址：器件地址选中 MPU6050，寄存器地址告诉它"我要写哪个寄存器"。
 * 本课用它写 0x6B=0x00 唤醒 MPU6050（清 SLEEP bit）。
 */
static void mpu_write_register(uint8_t reg, uint8_t value)
{
    i2c_start_and_address(MPU_ADDR_W);  /* 选中 MPU6050，写方向 */
    i2c_write_byte(reg);                /* 告诉它：我要写寄存器 reg */
    i2c_write_byte(value);              /* 写入数据 */

    while ((I2C1->SR1 & I2C_SR1_BTF) == 0U) {
    }

    I2C1->CR1 |= I2C_CR1_STOP;          /* 传输结束 */
}

static uint8_t mpu_read_register(uint8_t reg)
{
    uint8_t value;

    /*
     * 第 1 段：写寄存器地址。
     *
     * 为什么要先用写方向？因为 MPU6050 需要知道你想读哪个内部寄存器。
     * 流程是：START → 器件地址+写(0xD0) → 寄存器地址(如 0x75) → 等 BTF。
     * 这里还不 STOP，因为后面要用重复起始继续读同一个器件。
     * 如果在这里 STOP，MPU6050 的内部地址指针会复位，读出来的不是你要的寄存器。
     */
    i2c_start_and_address(MPU_ADDR_W);
    i2c_write_byte(reg);
    while ((I2C1->SR1 & I2C_SR1_BTF) == 0U) {
    }

    /*
     * 单字节读取前关闭 ACK。
     *
     * 为什么必须在这里关而不是读完再关？因为 STM32F103 I2C 外设的 ACK 位
     * 必须在地址阶段完成之前就配置好，它会在收到数据字节后自动按当前 ACK 位
     * 发出 ACK 或 NACK。如果等读到 RXNE 再关 ACK，NACK 已经发出去了——
     * 而你发的是 ACK，MPU6050 以为你还要继续读，会多发出一个字节。
     * 多出来的字节没人读，总线就乱了。
     */
    I2C1->CR1 &= ~I2C_CR1_ACK;

    /*
     * 第 2 段：重复起始，切到读方向。
     *
     * 重复起始（RESTART）和普通 START 的区别：
     * 普通 START 是 STOP 之后重新开始，总线会被释放再占用。
     * 重复起始是不发 STOP 直接再发 START，总线不释放。
     * 对 MPU6050 来说，重复起始后它的内部地址指针不会复位，
     * 所以知道你还要继续读刚才指定的那个寄存器。
     */
    i2c_start_and_address(MPU_ADDR_R);

    /*
     * 地址阶段一完成，立刻设 STOP。
     *
     * 为什么这么急？因为 F103 的 I2C 外设需要在收到数据前就知道
     * "这是最后一个字节"。STOP 位告诉外设：收到这个字节后自动发 STOP。
     * 如果设太晚，外设不知道要发 STOP，总线保持 BUSY 状态。
     */
    I2C1->CR1 |= I2C_CR1_STOP;
    while ((I2C1->SR1 & I2C_SR1_RXNE) == 0U) {
    }

    value = (uint8_t)I2C1->DR;

    /* 恢复 ACK，为下次通信做准备 */
    I2C1->CR1 |= I2C_CR1_ACK;
    return value;
}

int main(void)
{
    system_clock_72mhz_init();
    pc13_led_init();
    i2c1_init();
    delay_cycles(720000U);

    mpu_write_register(0x6BU, 0x00U);

    while (1) {
        uint8_t id = mpu_read_register(0x75U);

        pc13_toggle();

        if (id == 0x68U) {
            delay_cycles(720000U);
        } else {
            delay_cycles(3600000U);
        }
    }
}