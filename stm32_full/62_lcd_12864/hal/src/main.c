#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * ============================================================================
 * HAL 版 12864B LCD (ST7920) + UART 系统信息显示
 * ============================================================================
 *
 * 功能：
 *   PC 通过串口发送系统信息（CPU/内存/自定义文字），STM32 接收后
 *   驱动 12864B LCD (ST7920 控制器) 显示。
 *
 * ██████  硬件连接 (ST7920 串行模式, PSB=GND) ██████
 *
 *   LCD Pin   LCD 丝印    STM32 引脚   说明
 *   ------   --------    ---------    ----
 *   1        VSS         GND          电源地
 *   2        VDD         5V           电源正（若模块支持 3.3V 则接 3.3V）
 *   3        V0          电位器抽头    对比度调节（10kΩ 电位器，两端接 VDD/GND）
 *   4        RS(CS)      PB0          片选（软件控制）
 *   5        R/W(SID)    PA7          SPI1 MOSI → 串行数据
 *   6        E(SCLK)     PA5          SPI1 SCK → 串行时钟
 *   15       PSB         GND          串行模式选择（低电平）
 *   17       RST         PB1          复位（低有效）
 *   19       LED_A       3.3V/5V      背光正极（串 10-100Ω 限流电阻）
 *   20       LED_K       GND          背光负极
 *
 * ██████  串口通信协议 ██████
 *
 *   文本模式帧:
 *     [0xAA] [0x01] [64 字节文本数据]
 *     - 64 字节 = 4 行 × 每行 16 字符 (ASCII)
 *     - 不足 16 字符的行用空格 (0x20) 填充
 *     - 总共 66 字节，通过 DMA 接收
 *
 *   位图模式帧（可选）:
 *     [0xAA] [0x02] [1024 字节位图数据]
 *     - 128×64 像素，每字节 8 个水平像素，MSB 在最左
 *
 * ██████  ST7920 串行协议 ██████
 *
 *   每字节分 3 次 SPI 传输:
 *     1. 同步字节: 0xF8(命令) 或 0xFA(数据)
 *     2. 高半字节: 0xF0 | (data >> 4)
 *     3. 低半字节: 0xF0 | (data & 0x0F)
 *
 *   这是 ST7920 特有的"3 线 SPI"协议，不是标准 SPI。
 */

/*---------------------------------------------------------------------------*
 * 常量定义
 *---------------------------------------------------------------------------*/

/* 帧协议 */
#define FRAME_HEADER1      0xAAU  /* 帧头第 1 字节 */
#define FRAME_MODE_TEXT    0x01U  /* 文本模式 */
#define FRAME_MODE_BITMAP  0x02U  /* 位图模式 */
#define FRAME_HEADER_SIZE  2U     /* 帧头大小 */
#define FRAME_TEXT_SIZE    64U    /* 文本帧数据: 4行×16字符 */
#define FRAME_BITMAP_SIZE  1024U  /* 位图帧数据: 128×64/8 */
#define FRAME_BUF_SIZE     1026U  /* 最大帧缓冲 = 2(头) + 1024(数据) */
#define FRAME_ACK          0x55U  /* STM32 已处理完当前帧，可接收下一帧 */

/* LCD 文本行显示位置 (ST7920 DDRAM 地址) */
#define LCD_LINE1_ADDR     0x80U  /* 第 1 行 */
#define LCD_LINE2_ADDR     0x90U  /* 第 2 行 */
#define LCD_LINE3_ADDR     0x88U  /* 第 3 行 */
#define LCD_LINE4_ADDR     0x98U  /* 第 4 行 */
#define LCD_LINE_WIDTH     16U    /* 每行字符数 */

/* ST7920 串行同步字节 */
#define LCD_CMD_SYNC       0xF8U  /* 指令同步字节 */
#define LCD_DATA_SYNC      0xFAU  /* 数据同步字节 */

/* ST7920 基本指令 */
#define LCD_CLEAR          0x01U  /* 清屏 */
#define LCD_HOME           0x02U  /* 光标归位 */
#define LCD_ENTRY_MODE     0x06U  /* 光标右移，显示不动 */
#define LCD_DISPLAY_ON     0x0CU  /* 开显示，关光标，不闪烁 */
#define LCD_DISPLAY_OFF    0x08U  /* 关显示 */
#define LCD_FUNC_BASIC     0x30U  /* 8 位接口，基本指令集 */
#define LCD_FUNC_EXT       0x34U  /* 8 位接口，扩展指令集，关图形 */
#define LCD_FUNC_EXT_GFX   0x36U  /* 8 位接口，扩展指令集，开图形 */

/* GPIO 引脚 */
#define LCD_CS_PIN         GPIO_PIN_0    /* PB0 → LCD RS(CS) */
#define LCD_CS_PORT        GPIOB
#define LCD_RST_PIN        GPIO_PIN_1    /* PB1 → LCD RST */
#define LCD_RST_PORT       GPIOB
#define LCD_CMD_DELAY_LOOPS 2000U        /* ST7920 写指令后的保守等待 */

/* LED */
#define LED_PIN            GPIO_PIN_13   /* PC13 → 板载 LED */

/*---------------------------------------------------------------------------*
 * 全局变量
 *---------------------------------------------------------------------------*/

static SPI_HandleTypeDef   hspi1;                        /* SPI1 句柄 */
static UART_HandleTypeDef  huart1;                       /* USART1 句柄 */
static DMA_HandleTypeDef   hdma_usart1_rx;               /* USART1 RX DMA 句柄 */

/*
 * 两阶段接收:
 *   阶段 1: UART 中断接收 2 字节帧头
 *   阶段 2: DMA 接收有效载荷 (64 或 1024 字节)
 */
static uint8_t  header_buf[FRAME_HEADER_SIZE];           /* 帧头缓冲 (2 字节) */
static volatile uint8_t header_idx = 0U;                 /* 帧头接收索引 */
static uint8_t  payload_buf[FRAME_BITMAP_SIZE];          /* 有效载荷缓冲 (最大 1024 字节) */
static volatile uint16_t payload_size = 0U;              /* 当前帧有效载荷大小 */
static volatile uint8_t  frame_ready = 0U;               /* 帧就绪标志 */
static volatile uint8_t  current_mode = 0U;              /* 当前帧模式 */

/*---------------------------------------------------------------------------*
 * 函数前置声明
 *---------------------------------------------------------------------------*/

/* 系统初始化 */
static void system_clock_72mhz_init(void);
static void led_init(void);
static void spi1_init(void);
static void usart1_dma_init(void);
static void error_handler(void);

/* LED 辅助 */
static void led_on(void);
static void led_off(void);
static void led_toggle(void);
static void frame_ack_send(void);
static void lcd_write_delay(void);

/* LCD ST7920 串行驱动 */
static void lcd_write_byte(uint8_t data, uint8_t is_cmd);
static void lcd_write_cmd(uint8_t cmd);
static void lcd_write_data(uint8_t data);
static void lcd_init(void);
static void lcd_set_xy(uint8_t line, uint8_t col);
static void lcd_write_line(uint8_t line, const char *str);
static void lcd_draw_bitmap(const uint8_t *bmp);

/* 帧处理 */
static void process_text_frame(const uint8_t *data);
static void process_bitmap_frame(const uint8_t *data);

/*---------------------------------------------------------------------------*
 * main —— 主函数
 *
 * 接收流程:
 *   1. UART 中断逐字节接收 2 字节帧头 (0xAA + mode)
 *   2. 根据 mode 确定 payload 大小 (文本 64 / 位图 1024)
 *   3. 启动 DMA 接收 payload
 *   4. DMA 完成后处理帧，返回步骤 1
 *---------------------------------------------------------------------------*/
int main(void)
{
    HAL_Init();
    system_clock_72mhz_init();
    led_init();
    spi1_init();
    lcd_init();

    /* 显示启动画面 */
    lcd_write_line(1, "  STM32 + 12864 ");
    lcd_write_line(2, "  LCD 12864B V2.0");
    lcd_write_line(3, "  Waiting for PC ");
    lcd_write_line(4, "  connection...  ");

    usart1_dma_init();

    /*
     * 阶段 1: 开启 UART 中断接收帧头第 1 字节
     * 后续在 USART1_IRQHandler 中处理
     */
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);

    while (1) {
        if (frame_ready) {
            frame_ready = 0U;

            if (current_mode == FRAME_MODE_TEXT) {
                process_text_frame(payload_buf);
                led_toggle();
            } else if (current_mode == FRAME_MODE_BITMAP) {
                process_bitmap_frame(payload_buf);
                led_toggle();
            }

            /*
             * 返回阶段 1: 重新开始接收帧头
             *
             * 先停止 DMA（清除 UART DMAR 位），再排空 UART 接收缓冲，
             * 避免残留数据导致帧同步错位。
             */
            HAL_UART_DMAStop(&huart1);

            /* 排空 USART 接收缓冲（丢弃 DMA 期间积攒的残余字节） */
            while (USART1->SR & USART_SR_RXNE) {
                (void)USART1->DR;
            }

            header_idx = 0U;
            __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
            frame_ack_send();
        }
    }
}

/*---------------------------------------------------------------------------*
 * DMA 接收完成回调 —— 阶段 2 完成
 *---------------------------------------------------------------------------*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        frame_ready = 1U;
    }
}

/*---------------------------------------------------------------------------*
 * 文本帧处理：将 64 字节显示到 LCD 的 4 行上
 *---------------------------------------------------------------------------*/
static void process_text_frame(const uint8_t *data)
{
    uint8_t i;
    char line[LCD_LINE_WIDTH + 1];  /* +1 for null terminator */

    /*
     * 4 行 × 16 字符 = 64 字节
     * 每行 16 字节对应 LCD 一整行
     */
    for (i = 0; i < 4U; i++) {
        memcpy(line, &data[i * LCD_LINE_WIDTH], LCD_LINE_WIDTH);
        line[LCD_LINE_WIDTH] = '\0';
        lcd_write_line((uint8_t)(i + 1U), line);
    }
}

/*---------------------------------------------------------------------------*
 * 位图帧处理：将 1024 字节位图写入 LCD GDRAM
 *---------------------------------------------------------------------------*/
static void process_bitmap_frame(const uint8_t *data)
{
    lcd_draw_bitmap(data);
}

/*---------------------------------------------------------------------------*
 * LCD 驱动：向 ST7920 发送一个字节（串行模式）
 *
 * ST7920 串行协议 (PSB=0):
 *   1. CS = 1 (片选有效)
 *   2. 发送同步字节: 0xF8(命令) / 0xFA(数据)
 *   3. 发送高半字节: b7-b4 在 d3-d0，b7-b4 固定为 1
 *   4. 发送低半字节: b3-b0 在 d3-d0，b7-b4 固定为 1
 *   5. CS = 0 (片选释放)
 *
 * 使用 SPI1 硬件：一次 SPI 发送 3 个字节
 *---------------------------------------------------------------------------*/
static void lcd_write_byte(uint8_t data, uint8_t is_data)
{
    uint8_t buf[3];

    /*
     * 构造 3 字节 SPI 帧:
     *   buf[0] = 同步字节
     *   buf[1] = 高半字节 (0xF0 | (data >> 4))
     *   buf[2] = 低半字节 (0xF0 | (data & 0x0F))
     *
     * ST7920 同步字节:
     *   LCD_CMD_SYNC  (0xF8): RS=0, RW=0 → 写指令
     *   LCD_DATA_SYNC (0xFA): RS=1, RW=0 → 写数据
     *
     * is_data = 0 → 写指令 (CMD_SYNC)；is_data = 1 → 写数据 (DATA_SYNC)。
     */
    buf[0] = is_data ? LCD_DATA_SYNC : LCD_CMD_SYNC;

    /*
     * ST7920 要求每个半字节前面有 5 个高电平 + RW + RS + 0
     * 组合后: b7-b4=1111, b3=RW=0, b2=RS, b1-b0 是数据的高/低两位
     *
     * 实际上半字节格式是: 0b11111xxx，其中 xxx 是 4 个数据位中的高半/低半
     * 所以 buf[1] = 0xF0 | ((data >> 4) & 0x0F)
     *     buf[2] = 0xF0 | (data & 0x0F)
     */
    buf[1] = 0xF0U | ((data >> 4) & 0x0FU);
    buf[2] = 0xF0U | (data & 0x0FU);

    /* ST7920 串行模式的 RS 引脚作为 CS 使用，高电平选中。 */
    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_SET);
    HAL_SPI_Transmit(&hspi1, buf, 3U, HAL_MAX_DELAY);
    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    lcd_write_delay();
}

static void lcd_write_cmd(uint8_t cmd)
{
    lcd_write_byte(cmd, 0U);  /* is_data = 0 → 写指令 */
}

static void lcd_write_data(uint8_t data)
{
    lcd_write_byte(data, 1U);  /* is_data = 1 → 写数据 */
}

/*---------------------------------------------------------------------------*
 * LCD 初始化 (ST7920 串行模式)
 *
 * ST7920 初始化时序:
 *   1. 上电等待 >40ms
 *   2. 发送两次 0x30 (功能设置: 8位+基本指令)
 *   3. 设置显示、清屏、输入模式
 *   4. 切换至扩展指令 + 图形模式
 *---------------------------------------------------------------------------*/
static void lcd_init(void)
{
    /*
     * 硬件复位:
     *   拉低 RST 至少 1ms，然后拉高，等待 >40ms
     */
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);
    HAL_Delay(50U);  /* 等待 LCD 内部初始化完成 */

    /*
     * 软件初始化序列
     * 注意：串行模式下，ST7920 上电后自动检测到串行模式 (PSB=0)
     * 但仍需发送 0x30 来完成内部初始化。
     */

    /* 功能设置: 8位接口 + 基本指令集 (必须发两次) */
    lcd_write_cmd(LCD_FUNC_BASIC);        /* 0x30 */
    HAL_Delay(1U);
    lcd_write_cmd(LCD_FUNC_BASIC);        /* 0x30 */
    HAL_Delay(1U);

    /* 开显示、关光标、不闪烁 */
    lcd_write_cmd(LCD_DISPLAY_ON);        /* 0x0C */
    HAL_Delay(1U);

    /* 清屏 */
    lcd_write_cmd(LCD_CLEAR);             /* 0x01 */
    HAL_Delay(15U);  /* 清屏需要较长时间 */

    /* 输入模式: 光标右移、显示不移动 */
    lcd_write_cmd(LCD_ENTRY_MODE);        /* 0x06 */

    /*
     * 进入扩展指令集 + 图形模式
     * LCD_FUNC_EXT (0x34): RE=1, DL=1, G=0 → 开扩展指令集，关图形
     * LCD_FUNC_EXT_GFX (0x36): RE=1, DL=1, G=1 → 开扩展指令集，开图形
     *
     * 先发 0x34 进入扩展模式，再发 0x36 开图形
     * 这样后续可直接在文本和图形模式之间切换:
     *   - 文本模式 (0x30): 用于显示 DDRAM 字符
     *   - 图形模式 (0x36): 用于写 GDRAM 绘图
     */
    lcd_write_cmd(LCD_FUNC_EXT);          /* 0x34 */
    HAL_Delay(1U);
    lcd_write_cmd(LCD_FUNC_EXT_GFX);      /* 0x36: 开图形 */

    /* 切回基本指令集，准备显示文字 */
    lcd_write_cmd(LCD_FUNC_BASIC);        /* 0x30 */
}

/*---------------------------------------------------------------------------*
 * LCD 设置光标位置
 *
 * @param line: 行号 1-4
 * @param col:  列号 0-15
 *
 * ST7920 DDRAM 地址 (12864 屏幕, 4行×16字符):
 *   第 1 行: 0x80 + col
 *   第 2 行: 0x90 + col
 *   第 3 行: 0x88 + col
 *   第 4 行: 0x98 + col
 *
 * 注意: 第 3 行和第 2 行的地址不是顺序连续的!
 * 这是因为 ST7920 内部 DDRAM 的物理映射方式。
 *---------------------------------------------------------------------------*/
static void lcd_set_xy(uint8_t line, uint8_t col)
{
    const uint8_t line_addr[4] = {
        LCD_LINE1_ADDR,  /* 0x80 */
        LCD_LINE2_ADDR,  /* 0x90 */
        LCD_LINE3_ADDR,  /* 0x88 */
        LCD_LINE4_ADDR   /* 0x98 */
    };

    if (line < 1U || line > 4U || col > 15U) {
        return;
    }

    lcd_write_cmd(line_addr[line - 1U] + col);
}

/*---------------------------------------------------------------------------*
 * LCD 在指定行写入字符串（自动截断或补空格）
 *---------------------------------------------------------------------------*/
static void lcd_write_line(uint8_t line, const char *str)
{
    uint8_t i;

    lcd_set_xy(line, 0U);

    for (i = 0U; i < LCD_LINE_WIDTH; i++) {
        if (str[i] == '\0') {
            /* 字符串结束，后面补空格 */
            lcd_write_data(' ');
        } else {
            lcd_write_data((uint8_t)str[i]);
        }
    }
}

/*---------------------------------------------------------------------------*
 * LCD 写入全屏位图 (128×64 = 1024 字节)
 *
 * ST7920 GDRAM 的寻址方式:
 *   - 垂直分为上下两半: 上半 (行 0-31), 下半 (行 32-63)
 *   - 水平分为 16 列 (每列 8 像素 = 1 字节)
 *   - 每次写操作发送 2 字节 (上半字节 + 下半字节), 列地址自动 +1
 *
 * 位图数据布局 (和常见取模方式一致):
 *   - 1024 字节 = 128 列 × 64 行 / 8 位/字节
 *   - 但 GDRAM 要求垂直两个半区交错写入
 *   - 因此将位图重新组织为 ST7920 需要的格式
 *
 * 操作流程:
 *   for y = 0 to 31:
 *     设置 GDRAM Y 地址 = y
 *     设置 GDRAM X 地址 = 0
 *     for x = 0 to 15:
 *       写上半字节 (行 y, 列 x)
 *       写下半字节 (行 y+32, 列 x)
 *
 * 位图输入格式 (1024 字节):
 *   byte[y * 16 + x] = 第 y 行, 第 x 列的 8 个水平像素
 *   其中 y = 0..63, x = 0..15
 *
 *   GDRAM 需要的是 16 字节/行 × 8 像素/字节 = 128 水平像素
 *   但按垂直交错方式写入。
 *---------------------------------------------------------------------------*/
static void lcd_draw_bitmap(const uint8_t *bmp)
{
    uint8_t y, x;

    /* 进入扩展指令集 + 图形模式 */
    lcd_write_cmd(LCD_FUNC_EXT);          /* 0x34 */
    lcd_write_cmd(LCD_FUNC_EXT_GFX);      /* 0x36: 扩展指令 + 开图形 */

    for (y = 0U; y < 32U; y++) {
        /* 设置 GDRAM Y 地址 (行 0-31) */
        lcd_write_cmd(0x80U | y);

        /* 设置 GDRAM X 地址 = 0 */
        lcd_write_cmd(0x80U);

        /*
         * 写入 16 对数据 (上半 + 下半)
         * 第一字节: bitmap[y][x]     → 上半屏 (行 y, 列 x)
         * 第二字节: bitmap[y+32][x]  → 下半屏 (行 y+32, 列 x)
         */
        for (x = 0U; x < 16U; x++) {
            lcd_write_data(bmp[y * 16U + x]);           /* 上半 */
            lcd_write_data(bmp[(y + 32U) * 16U + x]);   /* 下半 */
        }
    }

    /* 切回基本指令集 */
    lcd_write_cmd(LCD_FUNC_BASIC);        /* 0x30 */
}

/*---------------------------------------------------------------------------*
 * SPI1 初始化 (用于 LCD ST7920 通信)
 *
 * 配置:
 *   Mode: 主模式
 *   Direction: 单工发送 (只发不收)
 *   DataSize: 8 位
 *   CLKPolarity: 低 (CPOL=0, 空闲时 SCK 为低)
 *   CLKPhase: 第一个边沿采样 (CPHA=0)
 *   BaudRatePrescaler: /32 → 72MHz/32 = 2.25MHz
 *   NSS: 软件管理 (CS 由 PB0 手动控制)
 *
 * 注意: ST7920 串行模式虽然不是标准 SPI，
 * 但我们可以用 SPI 外设来产生 SCK 和 MOSI 信号。
 * CS 由 PB0 软件控制以满足 ST7920 的帧时序要求。
 *---------------------------------------------------------------------------*/
static void spi1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 开启时钟 */
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /*
     * PA5 = SCK (LCD E/SCLK)
     * PA7 = MOSI (LCD R/W/SID)
     * 复用推挽输出
     */
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    /*
     * PB0 = LCD CS (RS)
     * PB1 = LCD RST
     * 通用推挽输出
     */
    gpio.Pin = LCD_CS_PIN | LCD_RST_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* 初始状态: CS 低 (不选中), RST 高 */
    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET);

    /* SPI1 配置 */
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;                    /* 主模式 */
    hspi1.Init.Direction = SPI_DIRECTION_1LINE;           /* 单工发送 */
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;              /* 8 位 */
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;            /* CPOL=0 */
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;               /* CPHA=0 */
    hspi1.Init.NSS = SPI_NSS_SOFT;                        /* 软件 NSS */
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32; /* 2.25MHz */
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;               /* 高位先发 */
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7;

    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        error_handler();
    }
}

/*---------------------------------------------------------------------------*
 * USART1 + DMA 初始化 (用于接收 PC 数据)
 *
 * 配置:
 *   BaudRate: 921600 (高速，减少延迟)
 *   WordLength: 8 位
 *   StopBits: 1
 *   Parity: 无
 *   DMA RX: 循环模式，接收完整帧后由回调处理
 *
 * DMA 通道: USART1_RX → DMA1 Channel 5
 *---------------------------------------------------------------------------*/
static void usart1_dma_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* 开启时钟 */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /*
     * PA9 = USART1_TX (复用推挽)
     * PA10 = USART1_RX (浮空输入)
     */
    gpio.Pin = GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_10;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* USART1 配置 */
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 921600;                         /* 高速波特率 */
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        error_handler();
    }

    /*
     * DMA1 Channel 5 用于 USART1_RX
     *
     * STM32F103 DMA 通道映射:
     *   USART1_RX → DMA1 Channel 5
     *
     * 配置 DMA 为循环模式，使得完成一次接收后自动重新开始。
     * 实际上不设置循环，由软件在 Callback 中重启接收。
     */
    hdma_usart1_rx.Instance = DMA1_Channel5;
    hdma_usart1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc = DMA_PINC_DISABLE;      /* 外设地址不变 */
    hdma_usart1_rx.Init.MemInc = DMA_MINC_ENABLE;          /* 内存地址递增 */
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode = DMA_NORMAL;                 /* 普通模式 */
    hdma_usart1_rx.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK) {
        error_handler();
    }

    /* 关联 DMA 到 UART 句柄 */
    __HAL_LINKDMA(&huart1, hdmarx, hdma_usart1_rx);

    /*
     * 配置 DMA 中断
     * 接收完成中断的优先级设为中等（高于 SysTick，低于紧急中断）
     */
    HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 1U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

    /* USART1 中断（虽然主要用 DMA，但错误处理需要） */
    HAL_NVIC_SetPriority(USART1_IRQn, 2U, 0U);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

/*---------------------------------------------------------------------------*
 * LED 初始化 (PC13, 板载 LED)
 *---------------------------------------------------------------------------*/
static void led_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    gpio.Pin = LED_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_GPIO_WritePin(GPIOC, LED_PIN, GPIO_PIN_SET);  /* LED 灭 */
}

static void led_on(void)
{
    HAL_GPIO_WritePin(GPIOC, LED_PIN, GPIO_PIN_RESET);
}

static void led_off(void)
{
    HAL_GPIO_WritePin(GPIOC, LED_PIN, GPIO_PIN_SET);
}

static void led_toggle(void)
{
    HAL_GPIO_TogglePin(GPIOC, LED_PIN);
}

static void frame_ack_send(void)
{
    uint8_t ack = FRAME_ACK;
    (void)HAL_UART_Transmit(&huart1, &ack, 1U, 10U);
}

static void lcd_write_delay(void)
{
    volatile uint32_t i;

    for (i = 0U; i < LCD_CMD_DELAY_LOOPS; i++) {
        __NOP();
    }
}

/*---------------------------------------------------------------------------*
 * 系统时钟: 8MHz HSE → PLL ×9 → 72MHz
 *---------------------------------------------------------------------------*/
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

/*---------------------------------------------------------------------------*
 * 错误处理
 *---------------------------------------------------------------------------*/
static void error_handler(void)
{
    __disable_irq();

    /* 快速闪烁 LED 指示错误 */
    while (1) {
        led_on();
        HAL_Delay(100U);
        led_off();
        HAL_Delay(100U);
    }
}

/*---------------------------------------------------------------------------*
 * 中断服务函数
 *---------------------------------------------------------------------------*/

/*
 * SysTick 中断: 每 1ms 触发一次，由 HAL 库使用
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/*
 * DMA1 Channel 5 中断: USART1 RX DMA 完成或半完成
 */
void DMA1_Channel5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart1_rx);
}

/*
 * USART1 中断服务函数
 *
 * 阶段 1 (接收帧头):
 *   RXNE 中断触发 → 读 DR → 存入 header_buf
 *   收满 2 字节 → 检查帧头 → 确定 payload_size → 启动 DMA
 *
 * 阶段 2 (接收 payload):
 *   DMA 自动接收，完成后触发 DMA1_Channel5_IRQHandler
 *   USART1 中断主要用于错误处理 (ORE/FE/NE)
 */
void USART1_IRQHandler(void)
{
    uint8_t data;
    uint32_t sr = USART1->SR;

    /* 错误处理: ORE/FE/NE。先读 SR 再读 DR 才能可靠清除 F1 的错误标志。 */
    if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
        (void)USART1->DR;

        HAL_UART_DMAStop(&huart1);
        frame_ready = 0U;
        current_mode = 0U;
        payload_size = 0U;
        header_idx = 0U;
        __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
        return;
    }

    /* 阶段 1: 接收帧头 (不经过 HAL, 直接操作寄存器) */
    if (sr & USART_SR_RXNE) {
        data = (uint8_t)(USART1->DR & 0xFFU);

        if (header_idx < FRAME_HEADER_SIZE) {
            header_buf[header_idx] = data;
            header_idx++;

            if (header_idx >= FRAME_HEADER_SIZE) {
                /* 帧头收完 → 检查合法性 */
                if (header_buf[0] == FRAME_HEADER1) {
                    uint8_t mode = header_buf[1];

                    if (mode == FRAME_MODE_TEXT) {
                        current_mode = FRAME_MODE_TEXT;
                        payload_size = FRAME_TEXT_SIZE;
                    } else if (mode == FRAME_MODE_BITMAP) {
                        current_mode = FRAME_MODE_BITMAP;
                        payload_size = FRAME_BITMAP_SIZE;
                    } else {
                        /* 未知模式 → 重新同步 */
                        header_idx = 0U;
                        return;
                    }

                    /* 关闭 RXNE 中断，启动 DMA 接收 payload */
                    __HAL_UART_DISABLE_IT(&huart1, UART_IT_RXNE);
                    if (HAL_UART_Receive_DMA(&huart1, payload_buf, payload_size) != HAL_OK) {
                        header_idx = 0U;
                        current_mode = 0U;
                        payload_size = 0U;
                        __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
                    }
                } else {
                    /*
                     * 帧头第 1 字节不是 0xAA
                     * 可能是字节错位，丢弃当前字节，重新等待同步
                     */
                    header_buf[0] = header_buf[1];  /* 后字节前移 */
                    header_idx = 1U;                /* 保留后字节作为候选 */
                }
            }
        }
        /* else: 不在 header 阶段，由 DMA 处理，RXNE 不应触发 */
    }
}
