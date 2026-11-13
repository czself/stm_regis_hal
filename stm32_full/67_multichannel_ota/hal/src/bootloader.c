#include "stm32f1xx_hal.h"
#include <string.h>

/*
 * HAL 版：UART / CAN / LoRa 三通道 OTA Bootloader。
 *
 * 逻辑与寄存器版完全相同，底层换成 HAL：
 *   UART -> HAL_UART_Transmit/Receive
 *   CAN  -> HAL_CAN_Init/ConfigFilter/AddTxMessage/GetRxMessage
 *   LoRa -> HAL_SPI_TransmitReceive + HAL_GPIO
 *   Flash -> HAL_FLASH_Unlock/Ex_Erase/Program/Lock
 *
 * 仍然要自己完成的“HAL 边界”：
 *   - 通道选择：HAL 不会告诉你哪个通道先来数据
 *   - CAN 分片重组：8 字节帧拼成协议流
 *   - LoRa 包序号检查
 *   - 镜像大小/CRC32/合法跳转地址校验
 */

#define APP_START_ADDR      0x08008000UL
#define APP_END_ADDR        0x08010000UL
#define APP_MAX_SIZE        (APP_END_ADDR - APP_START_ADDR)
#define OTA_PAGE_SIZE       1024U
#define OTA_CHUNK_MAX       32U
#define OTA_WAIT_WINDOW_MS  5000U
#define OTA_IDLE_ABORT_MS   15000U

#define FRAME_SYNC1         0xAAU
#define FRAME_SYNC2         0x55U

#define CMD_HELLO           0x01U
#define CMD_DATA            0x02U
#define CMD_END             0x03U
#define CMD_ABORT           0x04U

#define ST_OK               0x00U
#define ST_ERR_CRC16        0x01U
#define ST_ERR_LEN          0x02U
#define ST_ERR_SEQ          0x03U
#define ST_ERR_STATE        0x04U
#define ST_ERR_FLASH        0x05U
#define ST_ERR_SIZE         0x06U
#define ST_ERR_VERIFY       0x07U

#define CH_NONE             0U
#define CH_UART             1U
#define CH_CAN              2U
#define CH_LORA             3U

#define CAN_HOST_ID         0x6A1U
#define CAN_TARGET_ID       0x6A2U

#define LR_REG_FIFO         0x00U
#define LR_REG_OP_MODE      0x01U
#define LR_REG_FRF_MSB      0x06U
#define LR_REG_FRF_MID      0x07U
#define LR_REG_FRF_LSB      0x08U
#define LR_REG_PA_CONFIG    0x09U
#define LR_REG_OCP          0x0BU
#define LR_REG_LNA          0x0CU
#define LR_REG_FIFO_ADDR    0x0DU
#define LR_REG_FIFO_TX_BASE 0x0EU
#define LR_REG_FIFO_RX_BASE 0x0FU
#define LR_REG_FIFO_RX_CUR  0x10U
#define LR_REG_IRQ_FLAGS    0x12U
#define LR_REG_RX_NB_BYTES  0x13U
#define LR_REG_MODEM1       0x1DU
#define LR_REG_MODEM2       0x1EU
#define LR_REG_PREAMBLE_MSB 0x20U
#define LR_REG_PREAMBLE_LSB 0x21U
#define LR_REG_PAYLOAD_LEN  0x22U
#define LR_REG_MODEM3       0x26U
#define LR_REG_SYNC_WORD    0x39U
#define LR_REG_DIO_MAPPING  0x40U
#define LR_REG_VERSION      0x42U

typedef enum {
    RX_WAIT_SYNC1 = 0,
    RX_WAIT_SYNC2,
    RX_CMD,
    RX_SEQ_L,
    RX_SEQ_H,
    RX_LEN_L,
    RX_LEN_H,
    RX_PAYLOAD,
    RX_CRC_L,
    RX_CRC_H
} RxState;

typedef struct {
    RxState state;
    uint8_t cmd;
    uint16_t seq;
    uint16_t len;
    uint16_t payload_index;
    uint16_t crc_calc;
    uint16_t crc_recv;
    uint8_t payload[OTA_CHUNK_MAX];
} OtaParser;

static UART_HandleTypeDef huart1;
static CAN_HandleTypeDef hcan;
static SPI_HandleTypeDef hspi1;

static OtaParser g_parser;
static uint8_t g_channel = CH_NONE;
static uint8_t g_can_ok = 0U;
static uint8_t g_lora_ok = 0U;
static uint8_t g_lora_rx_ready = 0U;

static uint32_t g_image_size = 0U;
static uint32_t g_image_crc = 0U;
static uint32_t g_write_offset = 0U;
static uint16_t g_expected_seq = 0U;
static uint8_t g_ota_started = 0U;

static uint8_t g_can_rx_seq = 0U;
static uint8_t g_can_tx_seq = 0U;
static uint8_t g_lora_rx_seq = 0U;
static uint8_t g_lora_rx_packet[64];

static void system_clock_72mhz_init(void);
static void led_pc13_init(void);
static void uart1_init(void);
static void uart1_puts(const char *text);
static void uart1_put_u32(uint32_t value);
static uint16_t crc16_update(uint16_t crc, uint8_t byte);
static uint32_t crc32_update(uint32_t crc, uint8_t byte);
static uint32_t crc32_flash(uint32_t address, uint32_t length);
static uint8_t flash_erase_app_area(uint32_t size);
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length);
static void ota_parser_init(void);
static void ota_parser_feed(uint8_t byte);
static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len);
static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status);
static void can_init(void);
static void can_send_bytes(const uint8_t *data, uint16_t length);
static uint8_t can_poll_byte_input(void);
static void spi1_init(void);
static uint8_t spi1_transfer(uint8_t tx);
static void lora_write_reg(uint8_t address, uint8_t value);
static uint8_t lora_read_reg(uint8_t address);
static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length);
static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length);
static void lora_set_opmode(uint8_t mode);
static uint8_t lora_init(void);
static void lora_enter_rx(void);
static void lora_send_bytes(const uint8_t *data, uint16_t length);
static uint8_t lora_poll_byte_input(void);
static void channel_send_bytes(const uint8_t *data, uint16_t length);
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

static void uart1_puts(const char *text)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)text, (uint16_t)strlen(text), 200U);
}

static void uart1_put_u32(uint32_t value)
{
    char digits[10];
    uint8_t count = 0U;

    if (value == 0U) {
        uart1_puts("0");
        return;
    }
    while ((value > 0U) && (count < 10U)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        char ch = digits[--count];
        HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1U, 20U);
    }
}

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= (uint16_t)byte << 8;
    for (i = 0U; i < 8U; i++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((crc << 1) ^ 0x1021U);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
    uint8_t i;

    crc ^= byte;
    for (i = 0U; i < 8U; i++) {
        if ((crc & 1U) != 0U) {
            crc = (crc >> 1) ^ 0xEDB88320U;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

static uint32_t crc32_flash(uint32_t address, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t i;

    for (i = 0U; i < length; i++) {
        crc = crc32_update(crc, *(volatile uint8_t *)(address + i));
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint8_t flash_erase_app_area(uint32_t size)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;
    uint32_t pages = (size + OTA_PAGE_SIZE - 1U) / OTA_PAGE_SIZE;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = APP_START_ADDR;
    erase.NbPages = pages;

    return (HAL_FLASHEx_Erase(&erase, &page_error) == HAL_OK) ? 1U : 0U;
}

/*
 * 按半字写一段数据。
 * STM32F1 只能 16 位编程，最后一个字节要补 0xFF 组成半字，
 * 否则奇数字节长度的固件最后一包会写失败。
 */
static uint8_t flash_program_buffer(uint32_t address, const uint8_t *data, uint16_t length)
{
    uint16_t i;

    for (i = 0U; i < length; i += 2U) {
        uint16_t value;

        if ((i + 1U) < length) {
            value = (uint16_t)data[i] | ((uint16_t)data[i + 1U] << 8);
        } else {
            value = (uint16_t)data[i] | 0xFF00U;
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              address + i,
                              (uint64_t)value) != HAL_OK) {
            return 0U;
        }
    }
    return 1U;
}

static void channel_send_bytes(const uint8_t *data, uint16_t length)
{
    if (g_channel == CH_UART) {
        HAL_UART_Transmit(&huart1, (uint8_t *)data, length, 500U);
    } else if (g_channel == CH_CAN) {
        can_send_bytes(data, length);
    } else if (g_channel == CH_LORA) {
        lora_send_bytes(data, length);
    }
}

static void ota_send_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[7U + OTA_CHUNK_MAX + 2U];
    uint16_t index = 0U;
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    frame[index++] = FRAME_SYNC1;
    frame[index++] = FRAME_SYNC2;
    frame[index++] = cmd;
    crc = crc16_update(crc, cmd);

    frame[index++] = (uint8_t)(seq & 0xFFU);
    frame[index++] = (uint8_t)((seq >> 8) & 0xFFU);
    crc = crc16_update(crc, (uint8_t)(seq & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((seq >> 8) & 0xFFU));

    frame[index++] = (uint8_t)(len & 0xFFU);
    frame[index++] = (uint8_t)((len >> 8) & 0xFFU);
    crc = crc16_update(crc, (uint8_t)(len & 0xFFU));
    crc = crc16_update(crc, (uint8_t)((len >> 8) & 0xFFU));

    for (i = 0U; i < len; i++) {
        frame[index++] = payload[i];
        crc = crc16_update(crc, payload[i]);
    }

    frame[index++] = (uint8_t)(crc & 0xFFU);
    frame[index++] = (uint8_t)((crc >> 8) & 0xFFU);

    channel_send_bytes(frame, index);
}

static void ota_send_status(uint8_t cmd, uint16_t seq, uint8_t status)
{
    ota_send_frame((uint8_t)(cmd | 0x80U), seq, &status, 1U);
}

static void ota_handle_command(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    if (cmd == CMD_HELLO) {
        uint32_t image_size;
        uint32_t image_crc;

        if (g_ota_started != 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (len != 8U) {
            ota_send_status(cmd, seq, ST_ERR_LEN);
            return;
        }

        image_size = (uint32_t)payload[0] |
                     ((uint32_t)payload[1] << 8) |
                     ((uint32_t)payload[2] << 16) |
                     ((uint32_t)payload[3] << 24);
        image_crc = (uint32_t)payload[4] |
                    ((uint32_t)payload[5] << 8) |
                    ((uint32_t)payload[6] << 16) |
                    ((uint32_t)payload[7] << 24);

        if ((image_size == 0U) || (image_size > APP_MAX_SIZE)) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        if (HAL_FLASH_Unlock() != HAL_OK) {
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }
        if (flash_erase_app_area(image_size) == 0U) {
            HAL_FLASH_Lock();
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }

        g_image_size = image_size;
        g_image_crc = image_crc;
        g_write_offset = 0U;
        g_expected_seq = 0U;
        g_ota_started = 1U;

        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    if (cmd == CMD_DATA) {
        if (g_ota_started == 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (seq != g_expected_seq) {
            /*
             * 上一包的 ACK 可能在串口/CAN/LoRa 上丢了，上位机会重发同一包。
             * 此时数据已经写过，不能再写一次，只补一个 ACK。
             */
            if ((g_expected_seq != 0U) &&
                (seq == (uint16_t)(g_expected_seq - 1U))) {
                ota_send_status(cmd, seq, ST_OK);
                return;
            }
            ota_send_status(cmd, seq, ST_ERR_SEQ);
            return;
        }
        if ((len == 0U) || (len > OTA_CHUNK_MAX)) {
            ota_send_status(cmd, seq, ST_ERR_LEN);
            return;
        }
        if ((g_write_offset + len) > g_image_size) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        if (flash_program_buffer(APP_START_ADDR + g_write_offset, payload, len) == 0U) {
            ota_send_status(cmd, seq, ST_ERR_FLASH);
            return;
        }

        g_write_offset += len;
        g_expected_seq++;
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    if (cmd == CMD_END) {
        uint32_t crc;

        if (g_ota_started == 0U) {
            ota_send_status(cmd, seq, ST_ERR_STATE);
            return;
        }
        if (g_write_offset != g_image_size) {
            ota_send_status(cmd, seq, ST_ERR_SIZE);
            return;
        }

        crc = crc32_flash(APP_START_ADDR, g_image_size);
        if (crc != g_image_crc) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        /*
         * CRC 只证明“内容和上位机一致”，不证明“这个镜像能运行”。
         * 再查一次向量表，防止把错误文件写进 app 区后直接跳死。
         */
        if (app_is_valid() == 0U) {
            ota_send_status(cmd, seq, ST_ERR_VERIFY);
            return;
        }

        HAL_FLASH_Lock();
        g_ota_started = 0U;
        ota_send_status(cmd, seq, ST_OK);
        HAL_Delay(200U);
        jump_to_app();
        return;
    }

    if (cmd == CMD_ABORT) {
        HAL_FLASH_Lock();
        g_ota_started = 0U;
        ota_send_status(cmd, seq, ST_OK);
        return;
    }

    ota_send_status(cmd, seq, ST_ERR_STATE);
}

static void ota_parser_init(void)
{
    g_parser.state = RX_WAIT_SYNC1;
    g_parser.cmd = 0U;
    g_parser.seq = 0U;
    g_parser.len = 0U;
    g_parser.payload_index = 0U;
    g_parser.crc_calc = 0xFFFFU;
    g_parser.crc_recv = 0U;
}

/*
 * 逐字节状态机。
 * WAIT_SYNC1 会丢弃所有非 0xAA 字节，因此可以从半包或噪声中重新同步；
 * 长度/CRC16 错误只回 NACK，不推进 Flash 写入状态。
 */
static void ota_parser_feed(uint8_t byte)
{
    switch (g_parser.state) {
    case RX_WAIT_SYNC1:
        if (byte == FRAME_SYNC1) {
            g_parser.state = RX_WAIT_SYNC2;
        }
        break;

    case RX_WAIT_SYNC2:
        if (byte == FRAME_SYNC2) {
            g_parser.state = RX_CMD;
            g_parser.crc_calc = 0xFFFFU;
        } else {
            g_parser.state = RX_WAIT_SYNC1;
        }
        break;

    case RX_CMD:
        g_parser.cmd = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_SEQ_L;
        break;

    case RX_SEQ_L:
        g_parser.seq = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_SEQ_H;
        break;

    case RX_SEQ_H:
        g_parser.seq |= (uint16_t)byte << 8;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_LEN_L;
        break;

    case RX_LEN_L:
        g_parser.len = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        g_parser.state = RX_LEN_H;
        break;

    case RX_LEN_H:
        g_parser.len |= (uint16_t)byte << 8;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        if (g_parser.len > OTA_CHUNK_MAX) {
            ota_send_status(g_parser.cmd, g_parser.seq, ST_ERR_LEN);
            ota_parser_init();
            break;
        }
        g_parser.payload_index = 0U;
        g_parser.state = (g_parser.len == 0U) ? RX_CRC_L : RX_PAYLOAD;
        break;

    case RX_PAYLOAD:
        g_parser.payload[g_parser.payload_index++] = byte;
        g_parser.crc_calc = crc16_update(g_parser.crc_calc, byte);
        if (g_parser.payload_index >= g_parser.len) {
            g_parser.state = RX_CRC_L;
        }
        break;

    case RX_CRC_L:
        g_parser.crc_recv = byte;
        g_parser.state = RX_CRC_H;
        break;

    case RX_CRC_H:
        g_parser.crc_recv |= (uint16_t)byte << 8;
        if (g_parser.crc_recv != g_parser.crc_calc) {
            ota_send_status(g_parser.cmd, g_parser.seq, ST_ERR_CRC16);
        } else {
            ota_handle_command(g_parser.cmd, g_parser.seq,
                               g_parser.payload, g_parser.len);
        }
        ota_parser_init();
        break;

    default:
        ota_parser_init();
        break;
    }
}

static void can_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    CAN_FilterTypeDef filter = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_CAN1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_12;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_11;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    hcan.Instance = CAN1;
    hcan.Init.Prescaler = 4U;
    hcan.Init.Mode = CAN_MODE_NORMAL;
    hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan.Init.TimeSeg1 = CAN_BS1_13TQ;
    hcan.Init.TimeSeg2 = CAN_BS2_4TQ;
    hcan.Init.TimeTriggeredMode = DISABLE;
    hcan.Init.AutoBusOff = ENABLE;
    hcan.Init.AutoWakeUp = DISABLE;
    hcan.Init.AutoRetransmission = ENABLE;
    hcan.Init.ReceiveFifoLocked = DISABLE;
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        return;
    }

    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = 0U;
    filter.FilterIdLow = 0U;
    filter.FilterMaskIdHigh = 0U;
    filter.FilterMaskIdLow = 0U;
    filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filter.FilterActivation = ENABLE;

    if (HAL_CAN_ConfigFilter(&hcan, &filter) != HAL_OK) {
        return;
    }
    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        return;
    }

    g_can_ok = 1U;
}

static void can_send_bytes(const uint8_t *data, uint16_t length)
{
    uint16_t offset = 0U;

    while (offset < length) {
        CAN_TxHeaderTypeDef header = {0};
        uint8_t frame[8];
        uint16_t remain = (uint16_t)(length - offset);
        uint8_t count = (remain > 7U) ? 7U : (uint8_t)remain;
        uint32_t mailbox = 0U;
        uint8_t i;

        frame[0] = g_can_tx_seq++;
        for (i = 0U; i < 7U; i++) {
            frame[1U + i] = (i < count) ? data[offset + i] : 0xFFU;
        }

        header.StdId = CAN_TARGET_ID;
        header.ExtId = 0U;
        header.IDE = CAN_ID_STD;
        header.RTR = CAN_RTR_DATA;
        header.DLC = 8U;
        header.TransmitGlobalTime = DISABLE;

        if (HAL_CAN_AddTxMessage(&hcan, &header, frame, &mailbox) != HAL_OK) {
            return;
        }
        offset += count;
    }
}

static uint8_t can_poll_byte_input(void)
{
    CAN_RxHeaderTypeDef header = {0};
    uint8_t data[8];

    if (HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0U) {
        return 0U;
    }

    if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &header, data) != HAL_OK) {
        return 0U;
    }

    if ((header.StdId != CAN_HOST_ID) || (header.DLC != 8U)) {
        return 0U;
    }

    if (data[0] != g_can_rx_seq) {
        ota_parser_init();
    }
    g_can_rx_seq = (uint8_t)(data[0] + 1U);

    for (uint8_t i = 1U; i < 8U; i++) {
        ota_parser_feed(data[i]);
    }
    return 1U;
}

static void spi1_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7U;

    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        error_handler();
    }
}

static uint8_t spi1_transfer(uint8_t tx)
{
    uint8_t rx = 0U;

    if (HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1U, 100U) != HAL_OK) {
        error_handler();
    }
    return rx;
}

static void lora_write_reg(uint8_t address, uint8_t value)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address & 0x7FU);
    (void)spi1_transfer(value);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static uint8_t lora_read_reg(uint8_t address)
{
    uint8_t value;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address | 0x80U);
    value = spi1_transfer(0x00U);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
    return value;
}

static void lora_write_burst(uint8_t address, const uint8_t *data, uint8_t length)
{
    uint8_t i;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address & 0x7FU);
    for (i = 0U; i < length; i++) {
        (void)spi1_transfer(data[i]);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static void lora_read_burst(uint8_t address, uint8_t *data, uint8_t length)
{
    uint8_t i;

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    (void)spi1_transfer(address | 0x80U);
    for (i = 0U; i < length; i++) {
        data[i] = spi1_transfer(0x00U);
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

static void lora_set_opmode(uint8_t mode)
{
    lora_write_reg(LR_REG_OP_MODE, (uint8_t)(0x80U | mode));
}

static uint8_t lora_init(void)
{
    uint8_t version;

    spi1_init();

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_Delay(20U);

    version = lora_read_reg(LR_REG_VERSION);
    if (version != 0x12U) {
        return 0U;
    }

    lora_set_opmode(0x00U);
    HAL_Delay(10U);

    lora_write_reg(LR_REG_FRF_MSB, 0x6CU);
    lora_write_reg(LR_REG_FRF_MID, 0x40U);
    lora_write_reg(LR_REG_FRF_LSB, 0x00U);
    lora_write_reg(LR_REG_PA_CONFIG, 0xFFU);
    lora_write_reg(LR_REG_OCP, 0x2BU);
    lora_write_reg(LR_REG_LNA, 0x23U);
    lora_write_reg(LR_REG_MODEM1, 0x72U);
    lora_write_reg(LR_REG_MODEM2, 0x74U);
    lora_write_reg(LR_REG_MODEM3, 0x04U);
    lora_write_reg(LR_REG_PREAMBLE_MSB, 0x00U);
    lora_write_reg(LR_REG_PREAMBLE_LSB, 0x08U);
    lora_write_reg(LR_REG_SYNC_WORD, 0x12U);
    lora_write_reg(LR_REG_FIFO_TX_BASE, 0x00U);
    lora_write_reg(LR_REG_FIFO_RX_BASE, 0x00U);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    lora_set_opmode(0x01U);
    return 1U;
}

static void lora_enter_rx(void)
{
    lora_set_opmode(0x01U);
    lora_write_reg(LR_REG_DIO_MAPPING, 0x00U);
    lora_write_reg(LR_REG_FIFO_RX_BASE, 0x00U);
    lora_write_reg(LR_REG_PAYLOAD_LEN, 64U);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(0x05U);
    g_lora_rx_ready = 1U;
}

static void lora_send_bytes(const uint8_t *data, uint16_t length)
{
    uint8_t packet[64];
    uint8_t count = (uint8_t)((length > 63U) ? 63U : length);
    uint32_t start;

    packet[0] = 0U;
    for (uint8_t i = 0U; i < count; i++) {
        packet[1U + i] = data[i];
    }

    lora_set_opmode(0x01U);
    lora_write_reg(LR_REG_DIO_MAPPING, 0x40U);
    lora_write_reg(LR_REG_FIFO_ADDR, 0x00U);
    lora_write_reg(LR_REG_FIFO_TX_BASE, 0x00U);
    lora_write_burst(LR_REG_FIFO, packet, (uint8_t)(count + 1U));
    lora_write_reg(LR_REG_PAYLOAD_LEN, (uint8_t)(count + 1U));
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
    lora_set_opmode(0x03U);

    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < 2000U) {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_SET) {
            break;
        }
    }
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    if (g_lora_rx_ready != 0U) {
        lora_enter_rx();
    } else {
        lora_set_opmode(0x01U);
    }
}

static uint8_t lora_poll_byte_input(void)
{
    uint8_t flags;
    uint8_t length;
    uint8_t current;
    uint8_t seq;

    if (g_lora_rx_ready == 0U) {
        lora_enter_rx();
    }

    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) != GPIO_PIN_SET) {
        return 0U;
    }

    flags = lora_read_reg(LR_REG_IRQ_FLAGS);
    if (((flags & 0x40U) == 0U) || ((flags & 0x20U) != 0U)) {
        lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
        return 0U;
    }

    length = lora_read_reg(LR_REG_RX_NB_BYTES);
    if ((length == 0U) || (length > 64U)) {
        lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);
        return 0U;
    }

    current = lora_read_reg(LR_REG_FIFO_RX_CUR);
    lora_write_reg(LR_REG_FIFO_ADDR, current);
    lora_read_burst(LR_REG_FIFO, g_lora_rx_packet, length);
    lora_write_reg(LR_REG_IRQ_FLAGS, 0xFFU);

    seq = g_lora_rx_packet[0];
    if (seq != g_lora_rx_seq) {
        ota_parser_init();
    }
    g_lora_rx_seq = (uint8_t)(seq + 1U);

    for (uint8_t i = 1U; i < length; i++) {
        ota_parser_feed(g_lora_rx_packet[i]);
    }
    return 1U;
}

static uint8_t app_is_valid(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);

    if ((app_sp < 0x20000000U) || (app_sp > 0x20005000U)) {
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

static void jump_to_app(void)
{
    uint32_t app_sp = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset = *(volatile uint32_t *)(APP_START_ADDR + 4U);
    uint32_t i;

    __disable_irq();

    HAL_UART_DeInit(&huart1);
    if (g_can_ok != 0U) {
        (void)HAL_CAN_Stop(&hcan);
    }
    HAL_SPI_DeInit(&hspi1);

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
    uint32_t last_activity_tick;

    HAL_Init();
    system_clock_72mhz_init();
    led_pc13_init();
    uart1_init();
    ota_parser_init();

    can_init();
    g_lora_ok = lora_init();

    uart1_puts("\r\n[OTA67-HAL] multi-channel bootloader\r\n");
    uart1_puts("[OTA67-HAL] can=");
    uart1_put_u32(g_can_ok);
    uart1_puts(" lora=");
    uart1_put_u32(g_lora_ok);
    uart1_puts(" chunk=32B\r\n");

    start_tick = HAL_GetTick();
    last_activity_tick = start_tick;

    while (g_channel == CH_NONE) {
        uint8_t ch = 0U;

        if (HAL_UART_Receive(&huart1, &ch, 1U, 1U) == HAL_OK) {
            g_channel = CH_UART;
            uart1_puts("[OTA67-HAL] channel=UART\r\n");
            ota_parser_init();
            ota_parser_feed(ch);
            last_activity_tick = HAL_GetTick();
            break;
        }

        if ((g_can_ok != 0U) && (can_poll_byte_input() != 0U)) {
            g_channel = CH_CAN;
            uart1_puts("[OTA67-HAL] channel=CAN\r\n");
            last_activity_tick = HAL_GetTick();
            break;
        }

        if ((g_lora_ok != 0U) && (lora_poll_byte_input() != 0U)) {
            g_channel = CH_LORA;
            uart1_puts("[OTA67-HAL] channel=LoRa\r\n");
            lora_enter_rx();
            last_activity_tick = HAL_GetTick();
            break;
        }

        if ((HAL_GetTick() - start_tick) >= OTA_WAIT_WINDOW_MS) {
            if (app_is_valid() != 0U) {
                uart1_puts("[OTA67-HAL] no channel, jump\r\n");
                HAL_Delay(20U);
                jump_to_app();
            }
            start_tick = HAL_GetTick();
        }
    }

    while (1) {
        if (g_channel == CH_UART) {
            uint8_t ch = 0U;
            if (HAL_UART_Receive(&huart1, &ch, 1U, 10U) == HAL_OK) {
                ota_parser_feed(ch);
                last_activity_tick = HAL_GetTick();
            }
        } else if (g_channel == CH_CAN) {
            if (can_poll_byte_input() != 0U) {
                last_activity_tick = HAL_GetTick();
            }
        } else if (g_channel == CH_LORA) {
            if (lora_poll_byte_input() != 0U) {
                last_activity_tick = HAL_GetTick();
            }
        }

        if ((HAL_GetTick() - last_activity_tick) >= OTA_IDLE_ABORT_MS) {
            if (g_ota_started != 0U) {
                g_ota_started = 0U;
                HAL_FLASH_Lock();
                uart1_puts("[OTA67-HAL] idle timeout, stay\r\n");
            } else if (app_is_valid() != 0U) {
                uart1_puts("[OTA67-HAL] idle, jump\r\n");
                HAL_Delay(20U);
                jump_to_app();
            }
            last_activity_tick = HAL_GetTick();
        }
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
