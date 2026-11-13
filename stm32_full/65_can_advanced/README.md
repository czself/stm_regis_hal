# 第 65 课：CAN 进阶

## 1. 本课到底在学什么

本课表面现象是：串口每秒打印一行 `fifo0=30 fifo1=10 rtr=10 other=0 ...`。每 10 轮检查一次，计数符合预期时 PC13 翻转一次；不符合时打印 `check failed`。

32 课已经让 CAN 能发能收，但用的是“全放行过滤器 + 轮询 FIFO + 纯数据帧”。本课补上真实总线必须会的四件事：

```text
1. 精确过滤器：0x123 只进 FIFO0，0x456 只进 FIFO1，0x789 被硬件丢弃
2. 接收中断：FIFO 有报文才进 ISR，CPU 不再死等
3. 远程帧：收到 RTR 帧后，用同一 ID 回数据帧
4. Bus-Off：读 ESR 的 TEC/REC/BOFF，必要时重新初始化
```

为了不依赖收发器，本课仍用内部回环模式。回环模式下报文会完整经过“发送邮箱 → 内部总线 → 过滤器 → FIFO”，所以过滤器和中断逻辑与真实总线一致。

## 2. 本课学习目标

学完本课，你应该能回答：

1. 32 位的掩码过滤器里，STID 为什么左移 21 位？
2. 掩码模式与列表模式有什么区别？各适合什么场景？
3. 为什么过滤器要分配 FIFO？不分理会怎样？
4. `FMPIE0/FMPIE1` 和 `RF0R.FMP0` 分别是什么？
5. CAN1_RX0 和 CAN1_RX1 的中断入口名是什么？
6. 远程帧的 DLC 表示什么？和普通数据帧有何不同？
7. 收到远程帧后，为什么要用同一 ID 回应？
8. TEC/REC 是什么？超过多少会进入 Bus-Off？
9. Bus-Off 后如何恢复？为什么要重新配置过滤器？
10. 内部回环模式能验证什么，不能验证什么？

## 3. 本课目录结构

```text
65_can_advanced/
├── README.md
├── reg/
│   ├── platformio.ini
│   └── src/main.c
└── hal/
    ├── platformio.ini
    └── src/main.c
```

```bash
pio run -d stm32_full/65_can_advanced/reg -t upload
pio run -d stm32_full/65_can_advanced/hal -t upload
```

## 4. 实验硬件

- BluePill
- ST-Link
- USB-TTL（看计数）
- 可选真实总线：两块 BluePill + 两个 TJA1050 收发器
  - PA11 → TJA1050 RXD，PA12 → TJA1050 TXD
  - CANH-CANH、CANL-CANL，两端 120Ω 终端电阻
  - 收发器 3.3V/5V 按模块要求供电

本课默认内部回环，不需要收发器；接真实总线时把 `CAN_MODE_LOOPBACK` 改成 `CAN_MODE_NORMAL`，并确保总线上至少有一个节点会 ACK。

## 5. 先建立一个最基本的脑图

```text
发送侧
  main 每 500ms 发四帧：
    0x123 数据帧  -> 过滤器 0 -> FIFO0
    0x789 数据帧  -> 没有过滤器匹配 -> 丢弃
    0x456 数据帧  -> 过滤器 1 -> FIFO1
    0x123 远程帧  -> 过滤器 0 -> FIFO0

接收侧
  FIFO0 中断：
    ID==0x123 -> fifo0_count++
       RTR=1   -> remote_count++，置 pending
    其他 ID    -> other_count++（用于发现过滤器漏配）
  FIFO1 中断：
    ID==0x456 -> fifo1_count++
    其他 ID    -> other_count++

主循环
  看到 pending -> 发 0x123 数据帧回应
  每 10 轮：
    fifo0 应 >= 30（数据 + 远程请求 + 回应）
    fifo1 应 >= 10
    remote 应 >= 10
    other 必须为 0
```

## 6. 核心名词解释

### 6.1 已学名词速查

| 名词 | 一句话提醒 |
|------|-----------|
| `BTR` | 位时序，本课 500kbps：36MHz/(4×18)（32 课） |
| 发送邮箱 | 3 个，`TIR/TDTR/TDLR/TDHR`（32 课） |
| `FMR/FM1R/FS1R/FA1R` | 过滤器主控/模式/尺度/激活（32 课） |
| `RF0R.FMP0/RFOM0` | FIFO0 报文数/释放当前报文（32 课） |
| `RIR/RDTR/RDLR/RDHR` | 接收邮箱寄存器（32 课） |

### 6.2 `掩码过滤器` 是什么

掩码过滤器由“ID 寄存器 + 掩码寄存器”组成：

- 掩码位为 1：这一位必须与 ID 相同
- 掩码位为 0：这一位不关心

本课要精确匹配 `0x123`，所以 11 位 ID 全要匹配：

```c
FR1 = 0x123 << 21;      // ID
FR2 = 0x7FF << 21;      // 掩码：11 位全比较
```

如果掩码写成 `0`，就退化成 32 课的全放行。

### 6.3 `列表过滤器` 是什么

列表模式把过滤器寄存器当成“ID 清单”，命中清单中任意一个就通过。适合“只收固定几个 ID”的场景，最多可列 2 个 32 位 ID 或 4 个 16 位 ID（每组）。

本课用掩码模式，是因为要演示“按位匹配”的原理；列表模式在 HAL 里只需把 `FilterMode` 改成 `CAN_FILTERMODE_IDLIST`。

### 6.4 `过滤器组` 和 `FIFO 分配` 是什么

F103 有 14 个过滤器组（0~13），每组可独立配置并分配到 FIFO0 或 FIFO1。

为什么要分配：过滤器只决定“收不收”，FIFO 决定“收到的报文放哪个队列”。如果过滤器激活了但没分配 FIFO，或者分配了但没激活，报文都不会进队列。

本课组 0 → FIFO0，组 1 → FIFO1，用两个不同的接收队列证明“过滤器确实生效”。

### 6.5 `接收中断` 是什么

CAN 控制器在 FIFO 有新报文时置位 `RF0R.FMP0`。使能 `IER.FMPIE0` 后，就会触发 `CAN1_RX0` 中断。

ISR 的职责：

1. 循环读 `FMP0`，直到 FIFO 空
2. 读 `RIR/RDTR/RDLR/RDHR`
3. 写 `RFOM0` 释放当前报文
4. 计数或置标志

不释放 `RFOM0` 的后果：`FMP0` 不减少，FIFO 满（3 帧）后新报文被丢弃，中断还会反复触发。

### 6.6 `远程帧` 是什么

远程帧（Remote Frame）用 `RIR.RTR=1` 标识，它没有数据，只表达“谁有这个 ID 的数据，请发出来”。

远程帧的 `DLC` 仍然有意义：它表示请求方希望回多少个字节，而不是实际携带的字节数。

回应方用同一 ID 发一个数据帧即可，CAN 控制器不会自动帮你回，必须软件发。

### 6.7 `TEC/REC` 是什么

- TEC：发送错误计数（Transmit Error Counter）
- REC：接收错误计数（Receive Error Counter）

每检测到一次发送错误 TEC 增加，成功发送减少；接收侧同理。它们位于 `ESR` 的高 16 位。

### 6.8 `Bus-Off` 是什么

当 TEC 超过 255，bxCAN 认为本节点“坏到会干扰总线”，自动进入 Bus-Off：

- 停止收发
- 邮箱和 FIFO 不再工作
- `ESR.BOFF=1`

常见原因：总线上没有其他节点 ACK、波特率不一致、CANH/CANL 短路或反接。

恢复步骤：请求进入初始化模式 → 重新配置 BTR/过滤器 → 退出初始化。

### 6.9 `内部回环` 是什么

`BTR.LBKM=1` 时，发送数据在控制器内部直接环回到接收路径，不经过 PA11/PA12 和收发器。

它能验证：邮箱、过滤器、FIFO、中断、远程帧逻辑。
它不能验证：收发器、终端电阻、总线仲裁、ACK、错误帧、真实 Bus-Off。

### 6.10 `SILENT/LOOPBACK` 组合模式是什么

bxCAN 支持：

- Normal：正常收发
- Loopback：内部回环
- Silent：只听不发
- Loopback + Silent：回环但不影响真实总线

调试真实总线时，Silent 模式适合“只听流量、不干扰总线”；本课不展开，但 HAL 的 `CAN_MODE_SILENT` 可以直接切换。

## 7. 寄存器版代码逐步讲解

### 7.1 已学步骤（快速过）

- 72MHz、SysTick、LED、USART1：与 63 课相同
- CAN GPIO、进入/退出初始化、BTR 500kbps：32 课相同
- 发送邮箱填写、FIFO 读取：32 课相同

### 7.2 新增步骤：两个精确过滤器

```c
CAN1->FMR |= CAN_FMR_FINIT;

CAN1->FA1R &= ~CAN_FA1R_FACT0;
CAN1->FS1R |= CAN_FS1R_FSC0;       // 32 位
CAN1->FM1R &= ~CAN_FM1R_FBM0;      // 掩码模式
CAN1->FFA1R &= ~CAN_FFA1R_FFA0;    // 到 FIFO0
CAN1->sFilterRegister[0].FR1 = 0x123 << 21;
CAN1->sFilterRegister[0].FR2 = 0x7FF << 21;
CAN1->FA1R |= CAN_FA1R_FACT0;
```

组 1 同理，只是 `FFA1R` 置位分配到 FIFO1，FR1 换成 `0x456 << 21`。

为什么要先清 `FA1R.FACT`：修改过滤器寄存器前必须先失活，否则写入可能被忽略。

### 7.3 新增步骤：使能 FIFO 中断

```c
CAN1->IER |= CAN_IER_FMPIE0 | CAN_IER_FMPIE1;

NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1);
NVIC_SetPriority(CAN1_RX1_IRQn, 1);
NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
NVIC_EnableIRQ(CAN1_RX1_IRQn);
```

注意中断入口名：

| FIFO | 中断向量 | 处理函数名 |
|------|---------|-----------|
| FIFO0 | `CAN1_RX0_IRQn` | `USB_LP_CAN1_RX0_IRQHandler` |
| FIFO1 | `CAN1_RX1_IRQn` | `CAN1_RX1_IRQHandler` |

F1 的 USB 和 CAN 共用中断向量，所以 RX0 的函数名带 `USB_LP_` 前缀。写成别的名字，链接不会报错，但中断永远进不去。

### 7.4 新增步骤：ISR 中区分数据帧/远程帧

```c
uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
uint8_t is_remote = (rir & CAN_RI0R_RTR) ? 1 : 0;
```

远程帧不读数据，只记录并置 `g_remote_pending`。真正的回应放在主循环，避免在 ISR 里等待发送邮箱空闲。

### 7.5 新增步骤：Bus-Off 监测

```c
if ((CAN1->ESR & CAN_ESR_BOFF) == 0U) return;
g_busoff_count++;
can1_enter_init_mode();
CAN1->BTR = ...;
can1_filter_init();
CAN1->IER |= FMPIE0 | FMPIE1;
can1_leave_init_mode();
```

为什么恢复后要重配过滤器：进入初始化模式不会自动恢复过滤器激活位，必须重新写。

## 8. HAL 版代码逐步讲解

### 8.1 已学步骤（快速过）

- `HAL_CAN_Init`、`HAL_CAN_Start`：32 课
- `HAL_CAN_AddTxMessage`、`HAL_CAN_GetRxMessage`：32 课

`HAL_CAN_Init()` 会把 `Prescaler/SyncJumpWidth/TimeSeg1/TimeSeg2` 换算后写入 `BTR.BRP/SJW/TS1/TS2`；`Mode=CAN_MODE_LOOPBACK` 对应 `BTR.LBKM=1`；`AutoBusOff` 对应 `MCR.ABOM`。

### 8.2 新增步骤：两个过滤器

```c
filter.FilterBank = 0;
filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
filter.FilterIdHigh = 0x123 << 5;
filter.FilterMaskIdHigh = 0x7FF << 5;
HAL_CAN_ConfigFilter(&hcan, &filter);

filter.FilterBank = 1;
filter.FilterFIFOAssignment = CAN_FILTER_FIFO1;
filter.FilterIdHigh = 0x456 << 5;
HAL_CAN_ConfigFilter(&hcan, &filter);
```

为什么是左移 5 而不是 21：HAL 的 `FilterIdHigh` 是 32 位过滤器的**高 16 位**，`(0x123<<21)>>16 = 0x123<<5`。

### 8.3 新增步骤：中断通知

```c
HAL_CAN_ActivateNotification(&hcan,
    CAN_IT_RX_FIFO0_MSG_PENDING |
    CAN_IT_RX_FIFO1_MSG_PENDING);
```

对应寄存器版的 `IER.FMPIE0/FMPIE1`。中断入口：

```c
void CAN1_RX0_IRQHandler(void) { HAL_CAN_IRQHandler(&hcan); }
void CAN1_RX1_IRQHandler(void) { HAL_CAN_IRQHandler(&hcan); }
```

`HAL_CAN_IRQHandler` 会读取 `RF0R/RF1R`、调用回调、清挂起位。

### 8.4 新增步骤：回调里读报文

```c
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan_ptr)
{
    while (HAL_CAN_GetRxFifoFillLevel(hcan_ptr, CAN_RX_FIFO0) > 0) {
        HAL_CAN_GetRxMessage(...);
        ...
    }
}
```

必须循环读到空，否则一次中断只处理一帧，FIFO 很快堆满。

### 8.5 新增步骤：远程帧发送

```c
header.RTR = CAN_RTR_REMOTE;
header.DLC = 2;
HAL_CAN_AddTxMessage(&hcan, &header, data, &mailbox);
```

HAL 会写 `TIR.RTR`，不发送数据段；`DLC` 仍然写入 `TDTR`。

## 9. 两个版本真正应该怎么学

寄存器版重点看：

```text
FMR/FM1R/FS1R/FA1R -> FR1/FR2 -> IER.FMPIE -> NVIC -> ISR -> RFOM
```

HAL 版重点看：

```text
ConfigFilter 两个 bank -> ActivateNotification -> HAL_CAN_IRQHandler -> 回调
```

过滤器位域是本节最容易混的地方：寄存器版是 `ID<<21`，HAL 版是 `ID<<5`，两者描述的是同一件事。

## 10. 检验问题清单

### 10.1 STID 为什么左移 21 位？

**答**：在 32 位过滤器寄存器中，标准帧 ID 占 bit31~21，所以 11 位 ID 要左移 21 位。

### 10.2 掩码为 0 表示什么？

**答**：所有位都不关心，等价于全放行。

### 10.3 过滤器不分配 FIFO 会怎样？

**答**：报文匹配后无处存放，不会进入任何队列，CPU 永远读不到。

### 10.4 ISR 不写 RFOM 会怎样？HAL 版由谁完成这一步？

**答**：`FMP0` 不减少，FIFO 满后丢新帧，中断反复触发。HAL 版的 `HAL_CAN_IRQHandler()` 在回调返回后释放 FIFO 槽位，`HAL_CAN_GetRxMessage()` 也会写 `RF0R.RFOM0`，对应寄存器版手动写 `RFOM0`。

### 10.5 CAN1_RX0 的处理函数名是什么？

**答**：`USB_LP_CAN1_RX0_IRQHandler`；FIFO1 是 `CAN1_RX1_IRQHandler`。

### 10.6 远程帧 DLC 的含义？

**答**：请求方希望回多少字节，不是实际数据长度。

### 10.7 收到远程帧后硬件会自动回数据吗？

**答**：不会，必须软件用同一 ID 发数据帧。

### 10.8 TEC 超过多少进入 Bus-Off？

**答**：255。

### 10.9 Bus-Off 后为什么要重配过滤器？

**答**：进入初始化模式后过滤器激活位需要重新设置，否则报文不会进 FIFO。

### 10.10 内部回环能验证 Bus-Off 吗？

**答**：不能。回环没有 ACK/仲裁/错误帧，不会产生 TEC 增长，Bus-Off 逻辑只能接真实总线验证。

## 11. 工程实现步骤

### 11.1 需求分析

用回环模式验证“过滤 + 中断 + 远程帧”三件事，并用计数自查过滤器是否按预期工作。

### 11.2 硬件核查

先不接收发器，确认 PA11/PA12 没有被其他外设占用；接真实总线时先测 CANH/CANL 电平和终端电阻。

### 11.3 寄存器路线

先配过滤器，再用轮询验证计数；确认过滤器正确后再打开中断，把轮询改成 ISR。这样出错时能区分“过滤器错”和“中断错”。

### 11.4 HAL 路线

用 `HAL_CAN_ConfigFilter` 配两个 bank，用 `ActivateNotification` 打开中断，回调里读到空。

### 11.5 工程思维

真实 CAN 项目里，过滤器和 ID 规划是协议设计的一部分。ID 分配要有文档，否则节点一多就无法排查“为什么这帧收不到”。

### 11.6 常见工程陷阱

- `ID<<21` 写成 `ID`，永远匹配不到
- 过滤器激活了但没分配 FIFO
- ISR 不释放 RFOM
- 中断函数名写错，链接通过但不进中断
- 真实总线只有一端有 120Ω，通信不稳定
- 波特率与其他节点不一致，TEC 快速增长进 Bus-Off

## 12. 运行现象

串口每秒输出：

```text
[CAN-ADV] loopback 500kbps, filter 0x123->FIFO0, 0x456->FIFO1
fifo0=30 fifo1=10 rtr=10 other=0 tec=0 rec=0 busoff=0
fifo0=60 fifo1=20 rtr=20 other=0 tec=0 rec=0 busoff=0
```

每 10 轮 PC13 翻转一次（每轮约 520ms，约 5 秒一次）。`other` 必须一直是 0；如果大于 0，说明过滤器漏放了其他 ID。

异常表现：

- `fifo0` 一直是 0：过滤器未激活/未分配 FIFO0/中断没进
- `fifo1` 一直是 0：组 1 配置漏了，或中断函数名写错
- `other` 增长：某个过滤器掩码写成了 0，变成全放行
- 接真实总线后 `tec` 快速增长：无节点 ACK 或波特率不一致

## 13. 常见问题排查

### 13.1 计数全为 0

先确认 `CAN1->IER.FMPIE0` 和 NVIC 使能；再用轮询读 `RF0R.FMP0`，判断是“报文没进 FIFO”还是“中断没进”。

### 13.2 只有 FIFO0 有数据

检查组 1 的 `FFA1R.FFA1` 是否置 1、`FA1R.FACT1` 是否置 1。

### 13.3 远程帧没有回应

确认 ISR 设置了 `g_remote_pending`，主循环确实调用了发送；确认回应帧 ID 与请求 ID 一致。

### 13.4 真实总线通信失败

用示波器看 CANH/CANL 差分波形；确认两端 120Ω；确认所有节点波特率一致，采样点接近。

### 13.5 频繁 Bus-Off

先降波特率到 125k 测试；检查接线和终端电阻；确认没有节点在错误的位时序下持续发错误帧。

## 14. 本课最核心的结论

1. 过滤器决定“收不收”，FIFO 分配决定“放哪里”，两者都要配。
2. 32 位过滤器的 STID 在 bit31~21，HAL 的高 16 位写法要左移 5。
3. FIFO 中断必须循环读到空，并释放 RFOM。
4. 远程帧没有数据，DLC 表示期望回包长度，必须软件回应。
5. TEC 超过 255 进入 Bus-Off，恢复要重新初始化并重配过滤器。
6. 回环模式能验证控制器逻辑，验证不了收发器和总线电气特性。
7. 计数字段是最好的自检工具：`other` 必须为 0。

## 15. 建议你现在怎么读这节课

先读 `can1_filter_init()`，确认两个 bank 的 FR1/FR2/FFA1R；再读两个 ISR，看它们如何区分 ID 和 RTR；最后读主循环的计数判断，理解“为什么 fifo0 每轮是 3 帧”。

## 16. 扩展练习

1. 把过滤器改成列表模式，只接受 `0x123` 和 `0x456`。
2. 用 16 位尺度配置过滤器，比较与 32 位写法的差异。
3. 接真实总线，用两块板做一收一发，观察 TEC/REC。
4. 故意拔掉一个节点的终端电阻，观察错误计数变化。
5. 实现 Silent 模式监听总线，不发送任何帧。

## 17. 下一课预告

- 上一课：[64_uart_ota](../64_uart_ota/README.md)
- 下一课：[66_lora_sx1278](../66_lora_sx1278/README.md)：换到 LoRa 物理层，学习 SX1278 的寄存器配置与收发流程。
