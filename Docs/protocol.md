# BLE 二進位通訊協定規格 v1

裝置（STM32F103）與主機（PC / 手機）之間經 nRF52832 透傳 UART 交換的封包格式。
韌體實作：`Core/Src/mw/proto.c`；主機實作：`tools/pyhost/protocol.py`。

## 1. 框架層

```
┌──────┬──────┬──────┬─────┬─────┬──────────────┬────────────┐
│ 0xAA │ 0x55 │ TYPE │ SEQ │ LEN │   PAYLOAD    │  CRC16 LE  │
└──────┴──────┴──────┴─────┴─────┴──────────────┴────────────┘
  SOF0   SOF1   1B     1B    1B     0..96 bytes     2 bytes
```

| 欄位 | 說明 |
|---|---|
| SOF | 固定 `AA 55`，重同步定位點 |
| TYPE | 訊息型別；bit7=0 裝置→主機、bit7=1 主機→裝置 |
| SEQ | 發送端遞增序號（0~255 環繞）；ACK 會回帶原命令 SEQ |
| LEN | PAYLOAD 位元組數（0~96） |
| CRC16 | CRC16-CCITT-FALSE（poly `0x1021`、init `0xFFFF`），涵蓋 **TYPE..PAYLOAD**，little-endian 存放 |

- 多位元組欄位一律 **little-endian**。
- 接收端遇到 CRC 錯誤或 LEN>96：丟棄並掃描下一個 `0xAA` 重新同步。
- CRC 驗證向量：`"123456789"` → `0x29B1`。

## 2. 訊息型別

### 裝置 → 主機

| TYPE | 名稱 | 週期/時機 | PAYLOAD |
|---|---|---|---|
| 0x01 | ATTITUDE | 預設 10Hz（可調 0~50） | 見 2.1 |
| 0x02 | SYSSTAT | 1Hz | 見 2.2 |
| 0x03 | EVENT | 事件發生時 | 見 2.3 |
| 0x04 | LOG | 保留 | ASCII 文字 |
| 0x05 | INFO | 回應 GET_INFO | 見 2.4 |
| 0x7F | ACK | 收到任何命令後 | 見 2.5 |

### 主機 → 裝置

| TYPE | 名稱 | PAYLOAD | 說明 |
|---|---|---|---|
| 0x80 | PING | 無 | 連線測試 |
| 0x81 | SET_RATE | `u8 hz` | 姿態回報頻率；0=停止 |
| 0x82 | RESET_CLICKS | 無 | 點擊計數歸零（立即落盤） |
| 0x83 | CAL_GYRO | 無 | 觸發陀螺儀校正（非同步，結果見 EVENT） |
| 0x84 | GET_INFO | 無 | 查詢裝置資訊 |

### 2.1 ATTITUDE（16B）

| offset | 型別 | 欄位 | 單位 |
|---|---|---|---|
| 0 | i16 | roll | 度 ×100 |
| 2 | i16 | pitch | 度 ×100 |
| 4 | i16 | yaw | 度 ×100 |
| 6 | i16 | temp | °C ×100 |
| 8 | u32 | click_count | 次 |
| 12 | u32 | uptime_ms | ms |

### 2.2 SYSSTAT（12B）

| offset | 型別 | 欄位 | 說明 |
|---|---|---|---|
| 0 | u8 | cpu_percent | 排程器量測的 CPU 使用率 |
| 1 | u8 | sys_flags | bit0 IMU ok、bit1 OLED ok、bit2 EEPROM ok、bit3 bridge |
| 2 | u16 | err_count | I2C+UART+CRC 錯誤總和（飽和） |
| 4 | u32 | boot_count | 開機次數（EEPROM 持久化） |
| 8 | u32 | uptime_s | 秒 |

### 2.3 EVENT（5B）：`u8 id` + `u32 arg`

| id | 事件 | arg |
|---|---|---|
| 1 | BOOT | 重置原因旗標（bit0 POR、bit1 PIN、bit2 SOFT、bit3 IWDG…） |
| 2 | BTN_CLICK | 累計點擊次數 |
| 3 | CLICKS_RESET | 0 |
| 4 | CAL_DONE | 0=成功、1=失敗（裝置晃動） |
| 5 | BTN_DOUBLE | 切換後的遙測頻率（0=關閉） |

### 2.4 INFO（24B）

| offset | 型別 | 欄位 |
|---|---|---|
| 0 | u8 | proto_ver |
| 1 | char[11] | fw_ver（NUL 填充） |
| 12 | u32[3] | MCU 96-bit UID |

### 2.5 ACK（3B）

| offset | 型別 | 欄位 |
|---|---|---|
| 0 | u8 | req_type（原命令 TYPE） |
| 1 | u8 | req_seq（原命令 SEQ） |
| 2 | u8 | status：0 OK / 1 ERR / 2 UNKNOWN / 3 BUSY |

## 3. 封包範例

`SET_RATE 20Hz`（seq=3）：

```
AA 55 81 03 01 14 [CRC_L CRC_H]
```

CRC 計算範圍 = `81 03 01 14`。

## 4. 設計備註（教學）

- **為何 payload 直接用 packed struct 記憶體映像？** 兩端（Cortex-M、x86）
  都是 little-endian，直接映像最省 CPU 與程式碼。代價是不能直接移植到
  big-endian 主機 —— 規格書明載位元組序即為此保險。跨平台嚴謹作法是
  逐欄位序列化，這裡選擇工程上更常見的輕量作法。
- **為何命令用 ACK 而非重傳協定？** BLE 透傳鏈路（UART + GATT notify）
  本身已有鏈路層可靠性；應用層 ACK 只負責「命令語意上的成敗」。
  遙測流容忍偶發丟包（下一筆 100ms 就到），不做重傳是刻意取捨。
- **長操作（校正）的非同步模式**：命令立即 ACK「受理」，結果由
  EVENT 回報 —— 避免主機端等待逾時與裝置端阻塞通訊任務。
