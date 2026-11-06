# W202DS M23 VPU 移植

仅在 CM2 对固定 `6.18.35-w202ds-m23-wifi` 内核输出构建。原始厂商参考保持在
`../qogirn6pro-vendor/`，不修改成可直接加载的假象。

当前已验证的基础层：

- `w202_vpu_power.c`：四个 genpd provider，严格核对本板 PMU 地址/位掩码；共享顶层
  始终保持开启，不写显示共用 PLL。三个核心各自按请求上电，30 ms有界等待及失败回滚。
  consumer持有模块引用；退出逆序撤销 provider、subdomain和genpd，不留延迟工作。
- `w202_vpu_probe.c`：通过真正的 genpd 虚拟consumer和时钟引用读VAU寄存器，卸载释放
  所有引用。不写reset、IOMMU配置、任务或共享QoS寄存器；只用于研究，不是编解码驱动。

2026-11-03在p78 Server实测通过provider单独装卸、三个核心独立上电/寄存器读取/关闭、
活跃consumer阻止provider卸载、最终无genpd/模块残留。三个VAU版本均0x100，初始配置8，
核心和AHB时钟均256 MHz。显示共享顶层始终为on；Wi-Fi、蜂窝、10秒息屏继续工作。

新增 `w202_vpu_iommu.c`：仅匹配本板三核心VAU v1，按DT窗口映射4 KiB页，保留64位
物理地址并校验范围、对齐、溢出及PTE冲突；配置读回、解映射和显式retire正常退出。
`w202_vpu_dma_test.c`已在三个真实codec设备上通过单页、非连续SG、24个高于4 GiB页的
页表对照、VAU配置恢复和解映射清空测试。早期版本加载期间持有三核心电源/时钟；
当前版本通过 runtime PM 在空闲时释放。

`w202_vpu_buffers.c/.h`已补完整标准DMA-BUF get/attach/map及逆序释放，按文件拥有映射，
不读取ION私有结构。`w202_vpu_map_test`研究前端仅开放GET/FREE，其余codec命令失败。
三个真实VPU设备已导入Panfrost导出的64 KiB缓冲，核对每核48个高于4 GiB的页次，
通过fd别名/关闭、不同文件上下文误释放拒绝、重复释放、copyout失败回滚、close/SIGKILL清理。
文件和映射最终均为0，研究模块正常卸载；详见
DMA-BUF阶段（历史资料路径：`../../../../../archive/linux/server-vpu-dmabuf-20261103-v4/README.md`）。

新增 `w202_vpu_codec_main.c`：原生 arm64 `/dev/sprd_vsp`，独占文件与 MMIO VMA
生命周期，真实 IRQ、有界完成等待、只复位解码子核并恢复 VAU。close 先复位再释放
DMA-BUF；失败则保留上下文/缓冲并固定模块，禁止新 owner。诊断状态记录映射数、
自动释放、copyout 回滚、IRQ、reset、timeout、fault 和隔离状态。

`test-vendor-avc.c` 在独立原厂 Bionic 依赖闭包内调用实际 AVC 库；每次把剩余码流
装入对齐 BSM 缓冲，不直接递增硬件基址。真实 Panfrost PRIME fd 导入 VPU，标准
DMA-BUF CPU 同步后读取输出。全 IDR、I/P、I/P/B 三组共 20 帧与独立软件解码逐字节
匹配，包括 B 帧重排和末尾两帧 drain。`make-avc-fixture.py` 生成确定性码流/参考，
`package-vpu-runtime.py` 只打包实际 ELF 依赖，不复制 modem/NV 或块节点。
用户态 AVC 库本身是从本机只读提取的专有库。

`test-codec-api.c` 已实测独占、mmap 边界/执行权限、VMA 保留、copyout 回滚、
ioctl 状态顺序、204 ms 无 IRQ 超时，以及 enabled/等待完成期间 SIGKILL 的复位与清理。
随后视频解码成功，退出 opens/maps=0，正常卸载全部研究模块，taint 未增加。
见 AVC/退出阶段（历史资料路径：`../../../../../archive/linux/server-vpu-core-20261103-v5/README.md`）。

`w202_vpu_iommu.c` 现已使用250 ms runtime autosuspend，关闭前禁止翻译，恢复时重建
VAU页表寄存器并刷新。map/unmap在关闭时只更新coherent RAM，不在原子上下文唤醒硬件。
只读state不会唤醒；`live_regs=0`时寄存器字段无效。
`w202_vpu_iommu_get/put` 提供进程上下文电源引用；原生解码文件/VMA持有该引用和
物理解码设备自身的runtime PM引用。无owner时IRQ禁用，最后关闭先reset/清理再释放。
三个空闲核心真实断电、VMA持续供电、SIGKILL清理及三组20帧跨休眠/恢复后的像素对照
均已通过，正常卸载后无provider残留。见
空闲runtime PM阶段（历史资料路径：`../../../../../archive/linux/server-vpu-pm-20261103-v6/README.md`）。

`w202-avc-worker.c` 把客户端扩展为流式 Annex B 输入/标准 Y4M 输出，由 Linux FFmpeg
仅解封装、插入 AUD 后送入。8 MiB 限制作用于单个 access unit，已解除原测试程序的
总输入 1 MiB 限制。24 个重建帧按实际 bind/unbind 引用复用；MB 元数据属于工厂库
内部 DPB slot，单独分配并保留至释放，不能按重建帧共享。实际汇编确认 bind 的 header
参数，修正反编译伪代码漏参。`H264DecGetInfo` 给出 coded stride/height 与裁剪窗口，
输出按 crop 提取并把 NV12 转为 planar YUV420。

三组 MP4 共450帧逐帧匹配独立参考，最大1920×1080；每组仅45个DMA缓冲，
bind/unbind数平衡。Server 命令和服务现已安装、enable，并纳入新的固定输入包。
stdin/stdout隔离诊断文字，命令 TERM 对堵塞的子进程组限时退出，内核close负责复位/清理。
忙时拒绝停止、正常卸载、取消后恢复解码均通过。见
流式运行与交付（历史资料路径：`../../../../../archive/linux/server-vpu-runtime-20261103-v7/README.md`）。

历史研究包不自动安装或加载。常规播放器/V4L2、动态尺寸、其他 codec、编码器、
32位 compat、长时稳定性和系统suspend/resume仍待完成。主动制造硬件电源/reset失败和
确认处于实际在途DMA时的进程终止尚未验证；输出阻塞时的TERM不等同于该项。
构建、原始/失败日志及实机证据见
power/VAU阶段（历史资料路径：`../../../../../archive/linux/server-vpu-20261103-v2/README.md`）和
IOMMU阶段（历史资料路径：`../../../../../archive/linux/server-vpu-iommu-20261103-v3/README.md`）。
