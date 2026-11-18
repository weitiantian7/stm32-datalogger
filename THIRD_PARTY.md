# 第三方组件说明

本仓库包含自动生成代码、第三方基础组件与应用模块。整理仓库不改变原文件中的版权和许可声明。

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| STM32 HAL | Drivers/STM32F1xx_HAL_Driver | 保留原 LICENSE.txt 与文件头 |
| CMSIS 核心及 STM32F1 设备支持 | Drivers/CMSIS | 保留核心头文件、设备支持及对应 LICENSE.txt |
| FreeRTOS Kernel | freeRTOS | 文件头标记 V11.3.1，保留各文件原有 MIT 许可声明 |
| STM32CubeMX 生成代码 | Core、.ioc、部分 MDK-ARM 文件 | 保留原生成文件和声明，工程中包含 USER CODE 内容 |
| 应用与模块 | App、Hardware | 实际借鉴或复用的 OLED、字库、驱动等来源，请由项目作者核实后补充署名及适用许可 |

当前没有为整个仓库新增统一许可证，避免将第三方组件重新标为个人作品。项目作者可在核实自己代码与所用组件的来源后，为自己拥有权利的部分明确许可。
