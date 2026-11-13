# 第 64 课：串口 OTA 升级

## 1. 本课到底在学什么

本课表面现象是：在 PC 上运行 `ota_upload.py`，把 `app.bin` 通过串口发给板子，进度跑完后板子自动跳转到新固件，串口打印新的 `[APP-OTA]` 版本信息。

真正要学的是：**Bootloader 如何安全地把一段外部数据写进自己的 Flash，并在校验通过后才交出控制权**。完整链路：

```text
PC 读取 app.bin
  -> 计算整个镜像的 CRC32
  -> HELLO：告诉 Bootloader“大小 + CRC32”
  -> Bootloader 检查大小，擦除 app 区，回 ACK
  -> DATA：按 128 字节分包，每包带序号和 CRC16
  -> Bootloader 写 Flash，回 ACK；写失败/序号错回 NACK
  -> END：Bootloader 重算整片 CRC32
  -> 一致 -> 回 ACK -> 跳转新 app
  -> 不一致 -> 回 NACK，留在 Bootloader
```

63 课解决“跳过去”，本课解决“写进去”。OTA 的本质不是“传文件”，而是**带校验、可重传、能失败**的固件更新协议。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 为什么升级前要先发 HELLO，而不是直接发数据？
2. 为什么每包要用 CRC16，整片还要再用 CRC32？
3. 为什么协议要设计成“发一包等一个 ACK”？
4. Flash 为什么必须先擦除再写？擦除单位是什么？
5. 最后一包长度为奇数时，为什么写入要补 `0xFF`？
6. 序号 `SEQ` 有什么用？收到重复包应该怎么处理？
7. END 阶段校验失败时，为什么不能跳转？
8. 升级中途 PC 断开，Bootloader 应该怎么处理？
9. HAL 版 `HAL_FLASHEx_Erase()` 对应寄存器版哪些动作？
10. 为什么升级完成后要等 ACK 再跳转？

## 3. 本课目录结构

```text
64_uart_ota/
├── README.md
├── reg/
│   ├── platformio.ini          # env:bootloader / env:app
│   ├── ld/app.ld
│   ├── src/
│   │   ├── bootloader.c        # OTA 协议 + Flash 写入
│   │   └── app.c               # 升级目标，收到 'r' 可复位
│   └── tools/
│       └── ota_upload.py       # PC 上位机
└── hal/
    ├── platformio.ini
    ├── ld/app.ld
    ├── src/
    │   ├── bootloader.c
    │   └── app.c
    └── tools/
        └── ota_upload.py
```

```bash
# 1. 烧 Bootloader
pio run -d stm32_full/64_uart_ota/reg -e bootloader -t upload

# 2. 编译 app，得到 .pio/build/app/firmware.bin
pio run -d stm32_full/64_uart_ota/reg -e app

# 3. 安装 pyserial 并升级
pip install pyserial
python stm32_full/64_uart_ota/reg/tools/ota_upload.py \
       --port /dev/ttyUSB0 \
       --file stm32_full/64_uart_ota/reg/.pio/build/app/firmware.bin
```

## 4. 实验硬件

- BluePill（64KB Flash / 20KB SRAM）
- ST-Link：烧 Bootloader 和第一次 app
- USB-TTL：PA9←模块 RX，PA10→模块 TX，GND 共地
- 串口：115200 8N1
- 可选：USB-TTL 的 DTR 接 NRST，配合 `--dtr-reset` 自动复位

第一次必须用 ST-Link 烧一个 app，之后才能用 OTA 更新。空片只能通过下载器救回。

## 5. 先建立一个最基本的脑图

```text
协议帧
  AA 55 | CMD | SEQ(2) | LEN(2) | PAYLOAD | CRC16(2)

HELLO  CMD=0x01  PAYLOAD = size(4) + crc32(4)
DATA   CMD=0x02  PAYLOAD = 最多 128 字节
END    CMD=0x03  PAYLOAD = 空
ABORT  CMD=0x04  PAYLOAD = 空

应答帧
  AA 55 | CMD|0x80 | SEQ(2) | 0001 | STATUS | CRC16

STATUS
  0x00 OK
  0x01 CRC16 错
  0x02 长度错
  0x03 序号错
  0x04 状态错（还没 HELLO 就发 DATA）
  0x05 Flash 操作失败
  0x06 镜像大小错
  0x07 整片 CRC32 校验失败

Bootloader 主循环
  -> 5 秒内等 HELLO
  -> 收到：校验大小 -> 解锁 -> 擦除 app 区 -> 回 ACK
  -> 循环收 DATA：校验 CRC16/SEQ/LEN -> 半字写 Flash -> 回 ACK
  -> 收到 END：比对写入长度 -> 重算 CRC32 -> 回 ACK -> 跳转
```

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词 | 一句话提醒 |
|------|-----------|
| `FLASH->KEYR` | 写 `0x45670123`、`0xCDEF89AB` 解锁（38 课） |
| `FLASH->CR.PER/STRT/PG` | 页擦除/启动/编程使能（38 课） |
| `FLASH->SR.BSY` | 擦写忙标志，必须轮询清零（38 课） |
| `USART1` | PA9/PA10，115200 时 `BRR=0x0271`（21 课） |
| `状态机` | 按当前状态决定字节含义，可从半包中恢复（25 课） |
| `SRAM/Flash 分区` | app 从 `0x08008000` 开始（63 课） |

### 6.2 `OTA` 是什么

OTA（Over-The-Air）原指无线升级，在嵌入式里泛指“不拆机、不用下载器更新固件”。本课用串口做物理层，但协议层与 CAN/LoRa 版本一致，可以迁移。

它解决的问题是：现场设备装进机柜后，ST-Link 够不到，只能通过设备已有的通信口升级。

### 6.3 `镜像` 是什么

镜像就是编译出来的完整固件二进制，包含向量表、代码、常量。对 STM32 来说，镜像必须被烧到 app 区起点，且第 0 个字是栈顶、第 4 个字节是 Reset_Handler。

本课传输的是 `.bin`，不是 `.hex`。`.hex` 带地址记录，解析更复杂；`.bin` 是纯数据，配合固定 app 起点最直接。

### 6.4 `CRC16` 是什么

CRC16 是 16 位循环冗余校验，用来发现“这一帧在传输中是否被干扰”。本课用 CCITT-FALSE：

- 多项式 `0x1021`
- 初值 `0xFFFF`
- 不反射、不异或输出

它只覆盖 `CMD/SEQ/LEN/PAYLOAD`，不覆盖帧头 `AA 55`。帧头的作用是同步，不参与校验。

### 6.5 `CRC32` 是什么

CRC32 用来校验**整个镜像**。本课用 IEEE 802.3 反射算法（与 Python `zlib.crc32()` 一致），多项式 `0xEDB88320`，初值 `0xFFFFFFFF`，结果再异或 `0xFFFFFFFF`。

为什么两套校验都要：

- CRC16 保证“每一包”正确，错了只重传这一包
- CRC32 保证“整片”正确，防止包序错、重复包、Flash 写入异常等累积错误

### 6.6 `停止-等待协议` 是什么

发送方发一包，必须等到应答才能发下一包。优点是逻辑简单、错误定位明确；缺点是吞吐受往返时延限制。

本课选择它，是因为教学重点在“可靠写入”，不是极限速度。115200 下 32KB 固件大约几十秒，完全可以接受。

### 6.7 `擦除-编程` 是什么

Flash 的物理特性决定：

- 写入只能把 `1` 变成 `0`
- 擦除会把整页恢复成 `0xFF`
- 所以每次写新数据前必须先擦除对应页

本课在 HELLO 阶段一次性擦除镜像覆盖的所有页，之后 DATA 只做编程。这样避免每包都擦除，降低出错概率。

### 6.8 `半字编程` 是什么

STM32F1 的 Flash 编程单位是半字（16 位）。本课最后一包如果只有奇数个字节，必须补一个 `0xFF` 组成完整半字：

```c
value = (uint16_t)data[i] | 0xFF00U;
```

不补的后果：最后一个字节写不进去，整片 CRC32 校验失败。

### 6.9 `序号 SEQ` 是什么

每条 DATA 带一个从 0 递增的序号。Bootloader 只在 `SEQ == 期望值` 时写入，重复包或乱序包直接回 NACK。

为什么需要：串口可能因为重传产生重复包。如果重复写同一段 Flash，第二次写会失败（未擦除区域不能重复编程），表现为“重传反而把升级搞坏”。

### 6.10 `超时与回滚` 是什么

Bootloader 在两种情况下会放弃本次升级：

- 5 秒内没有 HELLO：认为不是升级流程，若 app 合法就跳转
- 升级中途 15 秒没有新数据：锁定 Flash，留在 Bootloader，等待重新 HELLO

关键原则：**升级未完成时绝不跳转**。因为 app 区可能只写了一半，跳过去必死。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

- 72MHz 时钟、SysTick 1ms、PC13、USART1：与 63 课相同
- Flash 解锁/上锁、等待 BSY、页擦除、半字编程：38 课已讲

### 7.2 新增步骤：HELLO 处理

```c
image_size = payload[0] | payload[1]<<8 | payload[2]<<16 | payload[3]<<24;
image_crc  = payload[4] | ... ;
if ((image_size == 0) || (image_size > APP_MAX_SIZE)) {
    ota_send_status(cmd, seq, ST_ERR_SIZE);
    return;
}
```

为什么先检查大小：如果 PC 发错文件或传了 hex，size 可能远大于 32KB。先拒绝，避免擦除到 Bootloader 区。

随后：

```c
flash_unlock();
flash_erase_app_area(image_size);
g_ota_started = 1;
ota_send_status(cmd, seq, ST_OK);
```

擦除循环只擦 `image_size` 覆盖的页，不会碰 `0x08000000~0x08007FFF`。

### 7.3 新增步骤：DATA 处理

```c
if (seq != g_expected_seq) { NACK(SEQ); return; }
if ((len == 0) || (len > 128)) { NACK(LEN); return; }
if ((g_write_offset + len) > g_image_size) { NACK(SIZE); return; }
if (!flash_program_buffer(APP_START + g_write_offset, payload, len)) { NACK(FLASH); return; }
g_write_offset += len;
g_expected_seq++;
```

顺序很重要：先校验，再写 Flash，最后更新状态。任何一步失败都不能推进 `write_offset`。

### 7.4 新增步骤：END 处理

```c
if (g_write_offset != g_image_size) { NACK(SIZE); return; }
crc = crc32_flash(APP_START_ADDR, g_image_size);
if (crc != g_image_crc) { NACK(VERIFY); return; }
flash_lock();
ota_send_status(cmd, seq, ST_OK);
delay_ms(200);
jump_to_app();
```

先回 ACK 再跳转：PC 必须知道“校验通过”，否则它会以为失败并重发 END。等 200ms 是为了让最后一个字节从移位寄存器发完。

### 7.5 新增步骤：协议解析器

10 个状态逐字节推进：

```text
WAIT_SYNC1 -> WAIT_SYNC2 -> CMD -> SEQ_L -> SEQ_H
-> LEN_L -> LEN_H -> PAYLOAD... -> CRC_L -> CRC_H -> 分发
```

`WAIT_SYNC1` 会丢弃所有非 `0xAA` 字节，所以从半包中间上电也能自恢复。

### 7.6 新增步骤：HAL 版的 Flash 映射

| 寄存器版 | HAL 版 |
|---------|--------|
| `FLASH->KEYR` 两把钥匙 | `HAL_FLASH_Unlock()` |
| 设置 PER/AR/STRT + 等 BSY | `HAL_FLASHEx_Erase(&erase, &page_error)` |
| 设置 PG + 半字写 + 等 BSY | `HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, value)` |
| 置 LOCK | `HAL_FLASH_Lock()` |

HAL 版仍然要自己算页数、校验大小、比对 CRC32。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

- `HAL_Init`、时钟、GPIO、UART：00/03/21 课
- `HAL_UART_Receive(..., 100U)`：带超时接收，用于主循环轮询

### 8.2 新增步骤：发送一整帧

HAL 版先把应答帧拼进 `frame[]`，再一次性 `HAL_UART_Transmit()`。这样比逐字节发送少了函数调用开销，也避免半帧被其他中断打断。

### 8.3 新增步骤：Flash 擦除页数

```c
uint32_t pages = (size + OTA_PAGE_SIZE - 1U) / OTA_PAGE_SIZE;
erase.NbPages = pages;
```

向上取整，保证最后一个不满 1KB 的页也被擦除。

### 8.4 新增步骤：错误返回

HAL 版在每个 Flash 操作后检查返回值，失败回 `ST_ERR_FLASH` 并上锁。这是与 38 课“最小 Demo 不检查返回值”的重要区别：OTA 必须知道写失败。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
HELLO -> 解锁 -> 页擦除 -> DATA -> 半字编程 -> END -> CRC32 -> 跳转
```

HAL 版重点看：

```text
同样的状态机 + HAL_FLASHEx_Erase/Program 的返回值处理
```

协议部分两版逐行一致，可以只看一版；Flash 部分对照看，理解 HAL 封装了哪些寄存器动作。

## 10. 检验问题清单

### 10.1 为什么先 HELLO 再 DATA？

**答**：HELLO 提供镜像大小和 CRC32，让 Bootloader 在写之前检查空间并一次性擦除，避免边写边擦。

### 10.2 只有 CRC32 不够吗？

**答**：不够。CRC32 只能判断最终结果，无法定位哪一包出错；CRC16 让错误包可以立即重传。

### 10.3 重复收到同一 DATA 包会怎样？

**答**：如果是最新一包的重传（`seq == expected-1`），只补一个 ACK，不重复写 Flash；更早的包回 `ST_ERR_SEQ`。

### 10.4 最后一包是奇数长度怎么办？

**答**：补 `0xFF` 组成半字写入，否则最后一个字节丢失，CRC32 失败。

### 10.5 END 校验失败能跳转吗？

**答**：不能。此时 app 区内容不可信，跳过去可能 HardFault；应留在 Bootloader 等重新升级。

### 10.6 升级中途断电会怎样？

**答**：app 区不完整，Bootloader 下次上电检查向量表通常不合法，会停在 Bootloader，等待重新升级。

### 10.7 为什么擦除只擦 app 区？

**答**：擦除会整页变成 `0xFF`。如果地址算错擦到 `0x08000000`，正在运行的 Bootloader 会把自己擦掉。

### 10.8 HAL 的 `HAL_FLASHEx_Erase` 对应哪些寄存器动作？

**答**：对应解锁后的 PER/AR/STRT 设置、BSY 等待和错误标志处理。

### 10.9 为什么 ACK 要在跳转前发？

**答**：PC 需要确认校验通过；如果先跳转，串口可能已被反初始化，ACK 发不出去。

### 10.10 上位机为什么要支持重试？

**答**：串口可能丢字节、USB 转串口可能抖动；没有重试，任何一次干扰都会让整个升级失败。

## 11. 工程实现步骤

### 11.1 需求分析

实现一个可重复、可失败、可观察的串口 OTA：HELLO 擦除、DATA 写入、END 校验、成功后跳转。

### 11.2 硬件核查

先确认 ST-Link 能烧、USB-TTL 能收发。用 63 课的 app 验证跳转正常，再开始改 Bootloader。

### 11.3 寄存器路线

先实现解析器和 ACK，再实现擦除，最后实现写入和 CRC32。每加一步就用串口打印状态，避免一次性堆完再调。

### 11.4 HAL 路线

HAL 版重点练习返回值检查。Flash 失败必须回 NACK，不能静默继续。

### 11.5 工程思维

OTA 的关键不是“传得快”，而是“失败可控”：

- 任何异常都不能跳转到不完整的 app
- 任何写入失败都要让上位机知道
- 升级前要确认镜像大小不超过分区
- 升级后要能通过版本号确认新固件真的运行

### 11.6 常见工程陷阱

- 擦除范围算错，擦掉 Bootloader
- 最后一包奇数长度未补 `0xFF`
- 重复包导致二次编程失败
- END 校验失败仍跳转
- 上位机没有重试，丢一包就全盘失败
- 升级完成后立刻跳转，ACK 没发完

## 12. 运行现象

正常升级日志：

```text
固件：.../firmware.bin
大小：1180 字节
CRC32：0x1A2B3C4D
串口：/dev/ttyUSB0 @ 115200

[1/3] 等待 Bootloader 并发送 HELLO
  HELLO OK，app 区已擦除

[2/3] 发送固件数据
  进度 100%  1180/1180 字节
  传输完成，用时 1.6s，平均 0.7 KB/s

[3/3] 请求校验并跳转
  校验通过，Bootloader 已跳转到新固件
  新固件串口输出：
  [APP-OTA] v1.0 running at 0x08008000
```

板端同时打印：

```text
[BOOT-OTA] STM32F103 UART OTA bootloader v1.0
[BOOT-OTA] app: 0x08008000, max 32KB, chunk 128B
[BOOT-OTA] waiting HELLO 5s...
[OTA] hello: size=1180 bytes
[OTA] erasing app area
[OTA] verify ok, jump to new app
```

异常表现：

- `未握手成功`：没在 5 秒窗口内复位，或串口接反
- `第 N 包失败：序号错误`：上位机重发导致，检查 Bootloader 是否复位过
- `END 失败：整片 CRC32 校验失败`：Flash 写入不完整或镜像被截断
- 升级后仍打印旧版本：跳转失败或烧到了旧 app

## 13. 常见问题排查

### 13.1 一直等不到 HELLO

用示波器/逻辑分析仪看 PA10 是否有数据；确认波特率 115200；确认发送的帧头是 `AA 55`。

### 13.2 HELLO 返回大小错误

检查上位机是否把 `.hex` 当 `.bin` 发送。`.hex` 是 ASCII 文本，大小通常更大。

### 13.3 某包一直 NACK

打印 `STATUS`。`0x05` 是 Flash 写失败，通常是擦除不完整或地址越界；`0x03` 是序号不连续。

### 13.4 升级完成但 app 不运行

检查 `APP_START_ADDR`、`ld/app.ld` 和烧录地址是否一致；用调试器读 `0x08008000` 的前两个字。

### 13.5 升级后串口无输出

新 app 可能没设置 `SCB->VTOR`，或时钟初始化失败。先用 ST-Link 烧同一份 app 验证。

## 14. 本课最核心的结论

1. OTA 是协议问题，不是“串口传文件”。
2. HELLO 先确定大小和 CRC32，再统一擦除。
3. 每包 CRC16 + 整片 CRC32，分别解决“单包错误”和“整体错误”。
4. 停止-等待 + 序号，让丢包和重传可控。
5. Flash 必须先擦后写，写单位是半字，奇数长度要补 `0xFF`。
6. 校验失败绝不跳转。
7. HAL 版必须检查 Flash API 返回值。
8. 升级成功的唯一证据是新固件的版本输出。

## 15. 建议你现在怎么读这节课

先读第 5 章帧格式，再用纸笔画出 HELLO/DATA/END 的时序；然后读寄存器版的 `ota_handle_command()`；最后对照 Python 脚本的 `request()`，确认两端 CRC 和字节序完全一致。

## 16. 扩展练习

1. 把 CHUNK 改成 64 或 256，观察传输时间和内存占用。
2. 增加 `CMD_VERIFY`，让 PC 主动查询当前 app 的 CRC32。
3. 在 app 里实现“收到 `r` 后复位”（本课已提供），配合脚本完成第二次升级。
4. 增加断点续传：记录已写入偏移，掉电后从断点继续。
5. 给 END 增加“版本号”字段，Bootloader 打印升级前后的版本。

## 17. 下一课预告

- 上一课：[63_bootloader_iap](../63_bootloader_iap/README.md)
- 下一课：[65_can_advanced](../65_can_advanced/README.md)：把传输通道换成 CAN，学习过滤器、中断、远程帧和 Bus-Off。
