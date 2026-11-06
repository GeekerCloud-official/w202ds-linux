# Linux 基带启动准备（实验中）

补齐独立 EFI 引导 Linux 后缺少的原厂 LK 准备步骤。代码属于基带 Linux 模块，
没有 EFI 源码或链接依赖。两个模块可单独构建，已纳入外层 Server 构建组；本机已通过显式启用的 Linux 运行服务加载。

## 已验证与限制

- `w202_avb_seed.ko`：2026-11-05 v3 在 p78 Server 实测，Trusty 日志确认
  `save vbmeta image for cp verify`、公钥匹配、AVB 校验 OK；DMA 结果读回为 0，
  NOP 返回 -15（完成）。普通 image 与 vbmeta 使用独立物理缓冲区。
- `w202_cp_prepare.ko`：复现 LK 的 PM/CH RAM 与 PHY 顶层电源准备后，原厂
  loader 可以完成此前的停止步骤并进入固件安全校验。
- 回收完整日志后确认新失败为PM SRAM内容损坏。SIPC非缓存SRAM改为Device映射后，
  64KiB读回完全一致，原厂nr_modem/nr_phy/pm_sys安全校验均通过。
- 一次全新独立EFI启动：运行服务自行准备AVB/CP并拨号，AT、SIM、附着、CID1及两个
  站点蜂窝HTTPS200通过，零失败服务。未测试物理断电与KDE。

v1 曾错误地在请求让出 CPU 后释放 DMA 内存，不保留为可运行实现。v2 没有收到有效
结果。这里的 AVB 源文件与 v3 已测源码逐字节一致；通过显式配置接入运行服务；模块字节保持v3测试版本。

## 构建输入与输出

通过外层 Devices 构建入口，在 CM2 使用匹配 M23 内核：

```sh
make -C Devices KDIR=/matched/kernel/output OUT=/new/output w202ds-modem-avb-seed
make -C Devices KDIR=/matched/kernel/output OUT=/new/output w202ds-modem-cp-prepare
```

分别输出一件 `.ko` 和 `build.json`、符号表、构建日志。AVB 模块需要匹配的
`w202ds-modem-trusty` provider；CP 准备模块仅依赖内核。构建不安装、不运行。

当前诊断输入由 `request_firmware` 读取 `w202-diag/vbmeta.bin`（64 KiB）及
`w202-diag/vboot-key-hash.bin`（32 字节）。输入来自本机原厂签名材料：仅在内存副本中
清除 vbmeta flags，恢复原厂 DHTB payload 哈希；没有写原分区、RPMB 或 rollback 接口。
文件保存在本设备 firmware/boot-prepare（历史资料路径：`../../firmware/boot-prepare/README.md`）；当前保留实验加载路径，以复用已测模块。

参数默认不执行：AVB 的 `run=1` 提交请求，CP 的 `prepare=1` 改变复位/电源状态。
只有从干净启动且 CP 未运行的诊断环境才测试；不能在正在运行的基带上重放。
AVB 的 `verify_result` 和 `live_result` 必须为 0，并核对安全日志。
未完成请求会固定模块、保留 DMA 内存到重启，不得强制卸载。

## TODO

- 物理断电后复测；通用发行包的输入与启用契约仍需收敛。
- 收敛 SMC 调用串行化、返回/超时处理和输入来源，完成生产实现审查。
- 持续核对原厂签名输入与实际固件版本，不在失败后绕过校验启动CP。

## 来源

板级 CP 步骤对照原厂 LK `pmic_arm7_RAM_active()` / `pubcp_boot_prepare()`；
AVB ABI 对照 `VbootVerifyInfo`、`FUNCTYPE_VBOOT_VERIFY_IMG`、`tee_common_call()`。
本机原厂 Trusty 只读反汇编确认普通 AVB 回调填充 KBC 所用 vbmeta 缓存。
没有修改或重新分发原厂安全固件；Linux 代码许可证见各源文件。

## 服务接入

`w202-modem-runtime initialize` 只在AT尚不能响应且CP未运行时调用准备步骤。
`W202_MODEM_BOOT_PREPARE=0` 是通用默认值；本机p78为1。模块放入现有modem模块目录，
加入SHA256SUMS；先验证AVB实时结果和采样结果均为0，再准备CP RAM/PHY，随后执行原厂loader。
已有响应的CP被直接沿用，不重复切电/复位。运行中不卸载这些模块。

匹配模块通过本地构建交付，编译产物不随源码上传；
详细证据见项目 archive/linux/uefi-modem-sram-20261105/README.md。
