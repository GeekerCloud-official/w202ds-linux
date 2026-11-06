# W202DS 板级设备树

`w202ds-server.dts` 只声明板级身份、保留项和 include 组合。硬件节点放到
`../Devices/<硬件>/devicetree/`，相机和 VDSP 分开；共同总线、
时钟、syscon、pinctrl、CPU 等在 `soc/`，全局内存布局在 `w202ds-memory.dtsi`。
同一 PMIC 的 regulator、FGU、ADC、codec、LED、马达等保持在该芯片的 DTSI 内。

来源是已运行 Server DTB 的反编译导出，原输入和去除的运行时字段见 `source.json`。
本次保留原有 numeric phandle、节点路径和 `__symbols__`；GPU 供电改用标签引用，
实际数值不变。GPU regulator 已在原导出树启用，不再依靠内核 `setup.c` 修补。
必须将这份 DTB 与分离后的内核配对，不能换回未修供电的原厂 DTB。

```sh
mkdir -p output/devicetree
dtc -I dts -O dtb -o output/devicetree/w202ds-server.dtb devicetree/w202ds-server.dts
```

DTSI 是合并片段；公共父总线只声明一次属性，各硬件片段仅重开父节点以挂接子节点。
跨硬件组合由顶层 DTS 负责，设备目录不 include 其他设备的私有 DTSI。
片段间仍有固定 phandle 依赖，目前可独立维护、组合编译，还不是任意主板可即插即用的 overlay。
只看到节点或 `status = "okay"` 不代表硬件驱动已可用，进度见各硬件 README。

`compare-dtb.py BASELINE.dtb CANDIDATE.dtb --report report.json` 按节点/属性原始值、
保留内存和 boot CPU 比较，忽略节点顺序与 DTB padding，任意差异返回非零。
从仓库根目录运行上述编译命令。主 DTS、`soc/`、内存布局及引用的各硬件 DTSI
一同发布；原始输入的哈希与去除的运行时字段见 `source.json`。

TODO：逐步把剩余数字 phandle 改为标签，核对上游 bindings；原厂命名/布局警告仍存在，
本次不是 schema 认证，也没有给未移植设备新增运行支持。
