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
