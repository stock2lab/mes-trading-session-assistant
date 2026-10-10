# v0.5.0 恢复刷写

```powershell
esptool --chip esp32s3 --port COM4 --baud 921600 write-flash 0x0 firmware/mes_companion.ino.merged.bin
```

将 COM4 替换为实际端口。合并固件包含 bootloader、分区表、boot_app0 和主程序，不需要执行 `erase-flash`。

