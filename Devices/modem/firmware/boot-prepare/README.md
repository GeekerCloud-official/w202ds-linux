# Linux 基带启动的 AVB 输入

`vbmeta.bin` 为本机原厂顶层 vbmeta 的 64 KiB 内存副本。按原厂 LK 的方式将内存 flags
还原为 0 后，16 KiB原始 DHTB payload 的哈希匹配；原始分区没有写入。
`vboot-key-hash.bin` 为 A 槽原厂 LK 证书中的公钥哈希，同时与该 vbmeta 内嵌公钥
SHA256 相符。它是公钥哈希，不是私钥，也不包含 NV、IMEI 或个体校准。

当前实验模块读取 `/lib/firmware/w202-diag/vbmeta.bin` 和
`/lib/firmware/w202-diag/vboot-key-hash.bin`，复制这两个匹配输入后才可启用
`W202_MODEM_BOOT_PREPARE=1`。保留原实验文件名以复用已实测的模块字节。
这些输入只适用于已核对的本机原厂固件组合；不能替代其它设备的签名材料。

2026-11-05 已通过 Trusty 的公钥、顶层 AVB 及后续 nr_modem/nr_phy/pm_sys 校验。
模块、Linux 服务和验证范围见 ../../source/boot_prepare/README.md。
