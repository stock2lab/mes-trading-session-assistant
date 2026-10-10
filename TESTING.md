# v0.5.0 场景测试

以下场景已通过开发板 COM4 实测：

- 普通日 09:29：`PREOPEN`，禁止开单
- 普通日 09:30：`TRADING`，允许开单
- 普通日 13:00：`WRAP_UP`，允许偶发交易
- 普通日 14:00：`LOCKED`，禁止开单
- 周末：`CLOSED`
- Good Friday：`CLOSED`
- Thanksgiving 12:59：`HOLIDAY`，允许开单
- Thanksgiving 13:00：`EARLY_CLOSED`，禁止开单
- 2026-03-06：纽约标准时间 `DST=0`
- 2026-03-09：纽约夏令时 `DST=1`
- `TEST,OFF`：成功退出并恢复真实模式

编译资源：

- 程序存储：1,380,935 bytes（43%）
- 全局内存：105,484 bytes（32%）
- 剩余动态内存：222,196 bytes

