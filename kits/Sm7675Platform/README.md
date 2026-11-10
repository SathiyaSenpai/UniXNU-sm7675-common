# SM7675 GPU platform operations

## Remote-processor protocol cores

`remoteproc/` imports the unchanged MDT, SMEM, SMP2P, GLINK, QMP, RPMh,
QRTR and read-only FAT implementations from UniXNU-sheng
`460179565a018c741b93e0b1efa07c55d4de497a`. `upstream.json` records every
file hash. Uke's WCN6750 board worker owns the WPSS addresses, firmware,
PAS identity, interrupts and power policy. Importing these cores does not
change existing GMU, SMMU or LLCC operations and does not establish hardware
acceptance of WPSS or Wi-Fi.

The ShengGmu and ShengGpuSmmu interfaces are imported from UniXNU-sm8550-common
main a9f45dc60c035084b95463781538380f4c3606c9. Runtime symbols and source names
are preserved. Generated uke headers supply gen7_11_0 windows, firmware and votes.
Board votes and firmware selection remain in the device repository.

The OEM DT declares one nonsecure user route, SID 0. The sheng-only second
route is not leased on uke; secure SID 2 is not attached to an ordinary process VM. GMU uses SID 5. The OEM GPU ACTLR policy is 0x32b.
These are source inputs; the iPadOS 27 integration awaits device qualification.

## LLCC v5.0 GPU cache operations

`Sm7675Llcc.h` supplies bounded LLCC attribute validation, inactive blank-slice
initialization, activation and A7xx GBIF SCID routing. The caller supplies the
board's two slice IDs/capacities, MMIO callbacks and serialization with the GPU
stopped. Unknown policies and inconsistent banks are refused before writes.
Only the supplied slices' policy bits change; other clients retain their words.
Routing is programmed after per-bank activation/readback and restored if route
verification fails. There is no global flush or slice deactivation.

The uke board selects cliffs7 GPU9/3200KiB and GPUHTW11/256KiB, using its OEM
LLCC v50 register layout. Host MMIO fault checks pass. Uke CRC0394BE262DDF45AF readback proves
four-bank activation and GBIF routing from an initially blank/inactive state;
three restart readbacks preserve both. This accepts activation/routing only;
coherency stress, pixel checks and controlled frame-time gains remain unverified.

## 远端处理器共享资源

`remoteproc/Sm7675RemoteProc.cpp` 为 uke 的 WPSS 和 ADSP 提供同一个
IPCC 分发入口（AP 窗口 0x406000，SPI229）、串行的 AOSS QMP 和 AP RSC
DRV2/TCS1。每个远端独立保存投票，同地址取最大值，撤销其中一方时保留
另一方。投票未确认时阻止继续编程该 TCS。QMP 超时保留未确认消息，后续
请求必须先等它得到确认。SMEM 分配同时持有 AP 软件锁和固件硬件互斥锁。

协议编码继续使用已有 QcomQmp/QcomRpmh/QcomSmem 实现。原始 DT 中的
CX/MX、LCX/LMX 名称及等级由设备代码从 cmd-db 查询。调用者须先完成
`sm7675_remote_attach`，中断回调只记录事件并唤醒工作线程。

`tests/remoteproc_host.cpp` 对实际共享运行时代码执行寄存器模型测试，
覆盖两种 RSC 布局、混合门铃、并发 QMP、超时及投票撤销。运行设备仓的
`python3 scripts/check-uke-dsp-host.py --soc ../UniXNU-sm7675-common`。
主机测试不能证明 ADSP 与 WPSS 已在硬件共存。
