# STM32 历史日志 CSV 导出工具

使用私有 Modbus 功能码 **0x41**，通过 RS485 按序号逐条读取 Flash 中的历史日志，并保存为 CSV。支持 Windows PowerShell 5.1，使用 .NET 串口接口，无需安装 Python 或第三方串口库。

固件提供 0x42 最新记录查询接口；本工具目前仍需手动指定起始序号和条数，没有接入 0x42，也没有自动查询最早有效序号的功能。批量导出是上位机连续发送单条请求，不是固件一次返回多条日志。

## 文件与测试资料

- `Start-Export.cmd`：双击启动入口，也可传入命令行参数。
- `Export-History.ps1`：请求组帧、回复校验、错误处理和 CSV 写入。
- `exports/`：运行后自动创建的本机输出目录，默认不纳入 Git。
- [原始 CSV 与测试记录](../../docs/test-records/history-export/README.md)：保留三份已提供的实机导出 CSV，包括 1000 条连续序号记录。
- [固件协议](../../docs/protocol.md)：0x41、0x42 及寄存器定义。

本次整理保留两个脚本的原始字节内容。Windows PowerShell 5.1 需要正确识别脚本中的中文，`Export-History.ps1` 原有的 UTF-8 BOM 已保留。

## 双击运行

1. 让开发板保持运行，连接 USB-RS485 转换器，并在设备管理器确认实际 COM 号。
2. 关闭串口助手等占用同一串口的程序。
3. 双击 `Start-Export.cmd`。
4. 输入串口号、起始序号和读取条数。
5. 在工具旁的 `exports` 目录查看 CSV，可用 Excel 打开。

脚本交互默认值为 **COM3、起始序号 17613、条数 10**。这些是原测试时的示例值，不代表你的设备当前保留范围；请按实际串口和有效日志序号填写。旧记录可能已被循环覆盖。起始序号支持十进制或 `0x` 前缀的十六进制。

## 命令行示例

在本工具目录打开 PowerShell，先读取一条已知有效记录：

```powershell
.\Start-Export.cmd -Port COM3 -StartSequence 17613 -Count 1
```

本仓库 1000 条 CSV 对应的请求范围为 65250～66249。下面命令使用相同范围；串口与仍可读取的记录范围请按设备实际状态调整：

```powershell
.\Start-Export.cmd -Port COM3 -StartSequence 65250 -Count 1000 -TimeoutMs 2000
```

脚本默认 `TimeoutMs=1000`。当前 FreeRTOS 固件历史读取接口的等待参数为 1500 ms，上面的 2000 ms 是复现时可采用的主站等待设置；它不是原 CSV 当次运行参数的记录，也不表示实测响应时间保证。

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `Port` | 交互默认 COM3 | 实际串口号 |
| `StartSequence` | 交互默认 17613 | 起始记录序号 |
| `Count` | 交互默认 10 | 连续请求的记录数量 |
| `SlaveAddress` | 1 | 从站地址，1～247 |
| `BaudRate` | 115200 | 波特率；8 数据位、无校验、1 停止位 |
| `TimeoutMs` | 1000 | 每次请求接收回复的等待预算，ms |
| `Retries` | 2 | 通信失败最多重试次数；含首次共最多 3 次尝试 |
| `IntervalMs` | 50 | 相邻记录请求间隔，ms |
| `ReferenceMv` | 3300 | ADC 原始值换算电压使用的参考值，mV |
| `OutputPath` | 自动命名 | 可指定输出 CSV 的绝对或相对路径 |
| `ContinueOnReadError` | 关闭 | 仅在收到设备异常 0x04 时记录错误行并继续 |
| `DecodeHex` | 不启用 | 离线解码一条正常回复，不打开串口 |

输出文件已存在时会拒绝覆盖。自定义 `OutputPath` 若指向仓库其他目录，应自行选择是否提交该文件。

## 校验、重试与错误

- 校验 CRC-16、回复长度、从站地址、功能码、字节数及记录序号；ADC 原始值须在 0～4095。
- 超时或无效回复会重试；设备返回的合法异常帧会直接记录为 `device_error`，不会按通信失败自动重试。
- 默认遇到错误停止，保留此前已导出的数据和当前错误行；文件按行刷新。
- `-ContinueOnReadError` 仅允许对设备异常 **0x04** 写入空数据的错误行后继续。当前固件未细分“记录不存在、已覆盖、CRC 无效、Flash I/O 失败”，因此工具也不推断具体原因。
- 固件异常 **0x06** 表示忙或存储读取等待超时；当前脚本会记录该异常并停止，需要稍后重新发起导出。
- CSV 的 `status=ok` 表示该记录最终读取与校验成功。脚本未逐行保存实际尝试次数，因此 1000 条 `ok` 不等于 1000 次请求均首次成功。

需要跳过明确返回 0x04 的记录时，可运行：

```powershell
.\Start-Export.cmd -Port COM3 -StartSequence 17613 -Count 100 -ContinueOnReadError
```

退出码：`0` 表示全部完成且无错误行；`1` 表示参数或运行失败；`2` 表示导出包含错误行或提前停止。

## CSV 字段

| 字段 | 含义 |
| --- | --- |
| `sequence` | uint32 记录序号 |
| `uptime_ms` | 日志中保存的本次开机毫秒数，不是真实日期时间 |
| `temperature_c` | 温度，℃；按 int16 解码后除以 100 |
| `humidity_rh_pct` | 相对湿度，%RH |
| `adc1_raw` / `adc2_raw` | 原始 ADC 计数；是否为均值取决于写入该条记录的固件版本 |
| `reference_mv` | 本次上位机换算采用的参考电压，默认 3300 mV |
| `adc1_mv` / `adc2_mv` | 按 `floor((raw × reference_mv + 2047) / 4095)` 换算的 mV |
| `status` | `ok`、`device_error` 或 `transport_error` |
| `error_code` / `error_message` | 错误码及说明；正常行为空 |

CSV 使用 UTF-8 BOM。出错行保留序号与错误状态，采样字段留空。当前记录不含 BootID，跨重启的 `uptime_ms` 可能重复；工具也不会把旧版固件保存的瞬时 ADC 值重新转换成采样均值。

## 离线验证

不连接设备即可解码已知正常回复：

```powershell
.\Start-Export.cmd -DecodeHex "01 41 10 00 00 44 CD 00 00 10 E0 0A BE 1E 03 04 0B 00 5B 66 AF"
```

预期序号 17613、开机时间 4320 ms、温度 27.50 ℃、湿度 76.83 %RH、ADC 原始值 1035 / 91。2026-11-20 已在 Windows PowerShell 5.1 下执行此示例，解码结果与原始 CSV 首行一致，退出码为 0。本次整理未连接串口进行新的实机导出。
