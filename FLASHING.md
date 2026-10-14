# v0.6.0-test 恢复刷写

使用合并固件：

```powershell
esptool --chip esp32s3 --port COM4 --baud 921600 write-flash 0x0 firmware/mes_companion.ino.merged.bin
```

将 `COM4` 替换为实际端口。合并固件包含 bootloader、分区表、boot_app0 和主程序，不需要先执行 `erase-flash`。

如果要从源码重新编译，开发板参数为：

```text
esp32:esp32:esp32s3
CDCOnBoot=cdc
FlashMode=qio
FlashSize=16M
PartitionScheme=app3M_fat9M_16MB
PSRAM=opi
UploadMode=default
USBMode=hwcdc
```
