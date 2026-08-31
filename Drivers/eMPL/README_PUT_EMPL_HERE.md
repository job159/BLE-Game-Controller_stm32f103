# Drivers/eMPL/ — InvenSense eMPL 放置處

啟用 DMP（`app_config.h` 設 `APP_USE_MPU_DMP = 1`）前，請將以下官方檔案複製到本目錄：

```
inv_mpu.c
inv_mpu.h
inv_mpu_dmp_motion_driver.c
inv_mpu_dmp_motion_driver.h
dmpKey.h
dmpmap.h
```

來源：InvenSense「Embedded Motion Driver (eMPL) 5.1 / 6.12」，
或各開發板廠商（正點原子、野火等）教學範例內附的相同檔案。

放入後仍需兩處小修改（將平台相依區塊替換為 `#include "drivers/empl_port.h"`），
完整步驟見 [Docs/dmp_porting.md](../../Docs/dmp_porting.md)。

> 本目錄位於 CubeIDE 預設編譯範圍（Drivers/）內，放入 .c 檔即會參與建置；
> 未啟用 `APP_USE_MPU_DMP` 前請勿放入，以免未修改的原始檔造成編譯錯誤。
