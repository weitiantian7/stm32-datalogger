# STM32 + FreeRTOS 数据采集与 Modbus 通信终端

面向温湿度和双路模拟量监测的嵌入式终端：实时采集、OLED 显示、温度回差报警，通过 RS485 接收上位机读写请求，并将采集日志与设备配置保存到外部 SPI Flash。

当前源码目标为 **STM32F103C8 / Cortex-M3**，使用 **STM32 HAL、FreeRTOS 原生 API 和 Keil MDK**。工程原名保留为 `freeRTOSProject1`，方便直接打开现有工程。

## 版本导航

- 当前分支：FreeRTOS 版，包含采集、显示、存储与 Modbus 四个业务任务。
- [裸机版 baremetal-v1.0](https://github.com/weitiantian7/stm32-datalogger/tree/baremetal-v1.0)：保留主循环调度版本及其原有 Keil 工程，可用于对照实现。

## 功能与实现

| 功能 | 源码中的实现 |
| --- | --- |
| 温湿度 | SHT30，I²C 地址 0x44，每秒读取，分别校验温湿度 CRC-8 |
| 模拟量 | ADC1 扫描通道 8/9，TIM3 TRGO 触发，DMA 循环搬运；每路 100 点形成均值窗口 |
| 显示与报警 | OLED 共享 I²C1；显示任务按 200 ms 周期运行；温度上限与回差可配置，PB11 低电平触发蜂鸣器 |
| 通信 | USART1 + RS485 半双工，115200 / 8N1；支持 0x03、0x04、0x06、0x10，含 CRC-16、异常响应及接收错误恢复路径 |
| 日志 | W25Q32，32 字节记录槽，CRC、最后写入的提交标记、写后回读；按扇区循环覆盖，上电扫描恢复追加位置 |
| 参数 | A/B 两个独立扇区，版本、代次、CRC、提交标记；显式触发保存，启动时选取有效配置 |
| 多任务 | 采集、显示、存储、Modbus 四个业务任务；日志写入、配置保存与历史读取统一经队列交给存储任务；使用互斥量、事件组及中断任务通知 |

这些是源码实现说明。采样速率、刷新周期均为配置或调度目标，实际误差、响应时间、连续运行和断电恢复结果需结合硬件测试记录。当前资料未附实测报告、演示视频或接线照片。

## 默认配置

- 从站地址：1。
- UART：115200 bit/s，8 数据位，无校验，1 停止位。
- 温度报警阈值：31.00 ℃；回差：1.00 ℃。
- 日志间隔：5 秒，可配置为 1～60 秒。
- 系统时钟：内部 HSI 8 MHz；FreeRTOS tick：100 Hz；RTOS 堆配置：8 KiB。

配置依据 `DeviceConfig.c`、`usart.c`、`main.c` 和 `FreeRTOSConfig.h`。Flash 中已有有效配置时，启动会恢复保存值。

## 系统组织

```text
SHT30 / ADC + DMA -> AcquisitionTask -> SensorData 快照 -> DisplayTask -> OLED / 蜂鸣器
                             |
                             +-> 存储请求队列 -> StorageTask -> W25Q32 日志 / 配置

PC 主站 <-> RS485 / USART1 <-> ModbusTask -> 寄存器 / 配置接口
```

历史读取扩展 0x41 通过 `Storage_ReadHistory()` 提交请求，由 `StorageTask` 统一执行 Flash 读取，使用回复队列和请求编号匹配结果。请求格式、等待超时与异常响应见 [协议说明](docs/protocol.md)。当前提供按序号读取单条历史的固件接口，尚未附 CSV 批量导出上位机工具。

## 目录导航

```text
App/          四个业务任务、共享数据与资源
Hardware/     传感器、Flash、协议、日志与参数模块
Core/         HAL 外设初始化、中断、FreeRTOS 配置
freeRTOS/     FreeRTOS 内核、Cortex-M3 移植与 heap_4
Drivers/      STM32 HAL、CMSIS 核心与 STM32F1 设备支持
MDK-ARM/      Keil 工程、启动文件与 RTE 配置
docs/         接线、协议、任务设计与验收记录模板
```

阅读入口：`Core/Src/main.c` → `App/AcquisitionTask.c` / `App/ModbusTask.c` / `App/StorageTask.c` → `Hardware/` 中对应模块。

## 构建与下载

1. 安装 Keil MDK 与 STM32F1 设备包。现有 `.uvprojx` 记录 ARM Compiler **5.06 update 5 build 528**、`Keil.STM32F1xx_DFP.2.2.0`；`.ioc` 记录 CubeMX **6.11.1**、STM32Cube F1 **1.8.7**。这是工程元数据，并非本次重新构建验证的版本组合。
2. 打开 `MDK-ARM/freeRTOSProject1.uvprojx`，确认目标器件与实物一致，并选用工程对应的 ARM Compiler 5。
3. 执行 Rebuild，记录编译结果和代码/RAM 占用。仓库保留所引用的源码和头文件，不需要复制原工程的 `.o`、`.axf` 等产物。
4. 在调试设置中选择实际使用的 SWD 调试器，核对下载算法，下载到开发板。
5. 按 [硬件接线](docs/hardware.md) 连接外设，上位机选择 115200 / 8N1、从站地址 1，先读取 8 个输入寄存器。

`.ioc` 用于保留配置来源。直接重新生成可能影响手工添加的 FreeRTOS、业务文件和工程配置，修改后应检查差异。首次复现优先打开现有 Keil 工程。

本次仓库整理只进行了文件完整性与工程引用路径检查；未在本机执行 Keil 编译、烧录或硬件测试。

## 演示顺序

1. 展示 OLED 的温湿度和两路电压，调整两路模拟输入并观察读数。
2. 主站用 0x04 读实时数据，用 0x03 读配置。
3. 用 0x06 修改温度阈值，演示报警触发与回差解除。
4. 独立向保持寄存器 0x0004 写入 0xA55A，轮询保存状态；重启后核对参数。
5. 演示传感器断开后的无效显示和恢复过程，保留实际结果。

## 项目资料

- [硬件与引脚](docs/hardware.md)
- [通信协议与寄存器](docs/protocol.md)
- [任务设计与存储布局](docs/architecture.md)
- [验收记录模板及待确认事项](docs/validation.md)
- [第三方组件说明](THIRD_PARTY.md)

原始工程中的业务文件保持原字节内容。整理时移除了编译输出、本机界面状态，以及本工程未引用的 CMSIS DSP/NN 示例与文档；所需 HAL、CMSIS 核心、设备支持和原有许可证已保留。
