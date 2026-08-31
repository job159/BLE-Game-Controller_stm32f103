# CLAUDE.md — 開發守則

STM32F103C8T6 教學韌體（IMU-BLE Node）。設計文件：`Docs/architecture.md`。

## 建置

- 首次：CubeIDE 開啟 `STM32F103_E.ioc` → Generate Code → 最佳化設 `-Os` → Build。
  CubeMX 可再生產物（HAL、CMSIS、startup、msp/it/hal_conf、.ld、.mxproject）與 eMPL 不入版本庫；
  `.ioc`、`.project`/`.cproject`、`main.c`/`main.h`（含 USER CODE 掛載點）入庫。
- 主機端單元測試（純 C 中介層）：
  `cd tests/host && gcc -std=c11 -Wall -Wextra -I../../Core/Inc test_main.c ../../Core/Src/mw/{ringbuf,crc16,proto,button,xutil}.c -o run_tests && ./run_tests`

## 分層鐵律（改碼前先讀）

- 依賴只准往下：`app → mw → drivers → bsp → HAL`。
- 腳位 / HAL handle 只准出現在 `bsp/`（`bsp_board.h` 為唯一腳位表）與 CubeMX 檔。
- `mw/` 不得引用任何 HAL/硬體標頭（要保持 PC 可測）。
- 任務不得阻塞超過數 ms（排程器會記 overrun）；已知長操作需手動餵狗並註記。
- 全案零 malloc、printf 禁 `%f`（用 `mw/xutil.h` 的 fix100）。
- 執行期輸出（log/CLI）一律 ASCII 英文；註解用繁體中文。

## 協定變更

改 `mw/proto.h` 的 payload 時，必須同步：`tools/pyhost/protocol.py`、
`Docs/protocol.md`、`_Static_assert` 尺寸、Python 端 `SIZE` 測試。

## 慣例

- 回傳碼統一 `app_common.h`（0 = APP_OK，負值錯誤）。
- 新增持久化欄位只能附加在 `stor_record_t` 尾端（版本遷移相容）。
- ISR 共享資料走 ring buffer 或 PRIMASK 臨界區（`app_enter_critical`）。
