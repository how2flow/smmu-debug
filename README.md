# smmu-debug

Patches for runtime verification of ARM SMMUv3 control surface and
PCIe device translation paths via the standard kernel IOMMU API.
Replaces register-poke and JTAG-based workflows with a small set of
debugfs primitives that can be composed from shell scripts.

A single delivery target is mirrored here:

- `linux/` — Linux kernel modules under `drivers/misc/smmu-debug/`
  providing debugfs interfaces (`attach`, `map`, `unmap`, `iova2phys`,
  `dma_memcpy`, …).

The Linux side has one consumer driver sharing a small framework:

- **smmu-test-pcie** — binds (manually) to a PCIe endpoint sitting
  behind an SMMUv3, exercises the IOMMU API control surface
  (domain alloc/attach, map/unmap, invalidation), verifies hardware
  translation via `iommu_iova_to_phys`, and optionally drives a real
  DMA via `dmaengine` when a memcpy-capable channel exists in the
  same IOMMU group.

Bench drivers can be added later under the same framework.

## How it works

### Common path

```
echo 1 > .../attach
        ↓
iommu_paging_domain_alloc(dev) + iommu_attach_device(domain, dev)
        ↓
SMMU driver programs STE / CD; CMDQ sync issued; ack polled

echo "<iova> <size> [prot] [buf_id]" > .../map
        ↓
iommu_map(domain, iova, buf_pa, size, prot)
        ↓
SMMU page tables updated; targeted invalidation issued via CMDQ

echo <iova> > .../iova2phys; cat .../iova2phys
        ↓
iommu_iova_to_phys(domain, iova) walks live page tables
        ↓
Returns the physical address SMMU would translate to

echo "<src_iova> <dst_iova> <size>" > .../dma_memcpy
        ↓
dma_request_channel(MEMCPY, filter=same_iommu_group)
        ↓
dmaengine memcpy: DMA addresses = caller-supplied IOVAs
        ↓
SMMU translates via the attached UNMANAGED domain
```

### What gets verified

1. **Control surface** — attach/map/unmap progress through CMDQ
   without timeout or error (TC-06 equivalent).
2. **Translation correctness** — `iova2phys` exposes the live
   translation walked by SMMU page tables; matches the IOVA→PA
   mapping installed via `iommu_map` (TC-07 equivalent).
3. **Invalidation effect** — after `unmap`, a follow-up `iova2phys`
   on the same IOVA returns 0, confirming TLB/walk-cache
   invalidation took effect.
4. **End-to-end DMA via SMMU** — when `dma_memcpy` succeeds, a real
   bus DMA crossed the SMMU using the mappings installed in this
   domain. Useful where a memcpy channel exists in the bound
   device's IOMMU group; on PCIe topologies whose RC-internal eDMA
   sits in a separate group, this path returns ENODEV and the
   verification stops at point 3.

The driver never touches SMMU MMIO directly. All work goes through
the IOMMU API and dmaengine, so kernel state stays consistent and a
test session does not require reboot. The framework is portable
across SoCs that expose SMMUv3 + PCIe + an IOMMU-translated dmaengine
provider in matching IOMMU groups.

## Layout

```
linux/
├── arch/arm64/boot/dts/vendor/target/
│   └── smmu-test.dtsi                # PCIe RC enablement + iommu-map
└── drivers/misc/
    ├── Makefile.add                  # one line to append to drivers/misc/Makefile
    └── smmu-debug/                   # framework + nested consumer
        ├── Makefile                  # CONFIG_ARM_SMMU_V3 gated; descends into smmu-test/
        ├── smmu-debug.h              # public API
        ├── smmu-debug.c              # /sys/kernel/debug/smmu/ root + per-bus subdirs
        └── smmu-test/                # per-bus consumers
            ├── Makefile              # ccflags -I.. picks up smmu-debug.h
            └── smmu-test-pcie.c      # PCI driver; UNMANAGED domain on bound EP
```

Resulting debugfs tree at runtime (after binding to a PCIe endpoint):

```
/sys/kernel/debug/smmu/
└── pcie/
    └── pcie-test0/
        ├── info        # attached state, mappings, dma_chan, counters
        ├── attach      # alloc UNMANAGED domain + iommu_attach_device
        ├── detach      # detach + free domain (and release dma_chan)
        ├── map         # iommu_map  "<iova> <size> [prot] [buf_id]"
        ├── unmap       # iommu_unmap "<iova> <size>"
        ├── iova2phys   # rw; write IOVA → read shows iommu_iova_to_phys result
        └── dma_memcpy  # "<src_iova> <dst_iova> <size>" → eDMA/DMAC memcpy
```

The framework root `/sys/kernel/debug/smmu/` is created eagerly at
boot (built-in module init) so that "module loaded" can be confirmed
even before any consumer driver binds.

## Apply

From the target kernel tree root:

```bash
SMMU_USER=smmu-debug/linux
VENDOR=<vendor>          # SoC vendor directory
TARGET=<target>          # SoC target directory
BOARD_DTS=arch/arm64/boot/dts/$VENDOR/$TARGET/<your-board>.dts

# 1. Copy the framework (carries smmu-test/ inside)
cp -r $SMMU_USER/drivers/misc/smmu-debug ./drivers/misc/

# 2. Copy the DT fragment (substitute the real vendor/target path)
cp $SMMU_USER/arch/arm64/boot/dts/vendor/target/smmu-test.dtsi  \
   ./arch/arm64/boot/dts/$VENDOR/$TARGET/

# 3. Append one line to drivers/misc/Makefile
cat $SMMU_USER/drivers/misc/Makefile.add >> ./drivers/misc/Makefile

# 4. Add include line to the board DTS you want (manual):
#       #include "smmu-test.dtsi"
#    Edit smmu-test.dtsi first to match your SoC's SMMU and PCIe
#    DT labels and the iommu-map SID ranges your SMMU has reserved
#    for the PCIe Root Complexes.
```

## Dependencies

Build gates (per-file):

| Component | Kconfig |
|---|---|
| Framework (`smmu-debug.o`) | `CONFIG_ARM_SMMU_V3` |
| PCIe consumer (`smmu-test-pcie.o`) | `CONFIG_ARM_SMMU_V3` + `CONFIG_PCI` |

The framework directory `smmu-debug/` is gated by `CONFIG_ARM_SMMU_V3`
in `drivers/misc/Makefile` (single line) and recursively descends
into the `smmu-test/` consumer. The PCIe consumer adds a `CONFIG_PCI`
gate inside `smmu-test/Makefile` so the framework still builds on
non-PCI SoCs (future platform-bus consumers can sit alongside).

Also required:
- `CONFIG_DEBUG_FS=y`
- A DT with an `arm,smmu-v3` node and a PCIe RC mapped into it via
  `iommu-map`
- `dma_memcpy` only succeeds when at least one memcpy-capable
  dmaengine channel exists in the same IOMMU group as the bound
  PCIe device. On RC-internal eDMA topologies this is typically not
  the case; control-surface tests work regardless.

## DT node

### smmu-test.dtsi (PCIe RC enablement)
```dts
&smmu {
    status = "okay";
};

&pcie0 {
    /* <rid-base, &iommu, sid-base, length> */
    iommu-map = <0x0 &smmu 0x0 0x10000>;
};

&pcie1 {
    iommu-map = <0x0 &smmu 0x10000 0x10000>;
};
```

Notes:
- `&smmu`, `&pcie0`, `&pcie1` are placeholder labels. Substitute the
  exact DT labels from your SoC's base DTS (some SoCs label the
  SMMU as `&smmu_pcie`, `&smmu_peri`, `&smmu0`, etc.).
- The SID-base values (here `0x0` and `0x10000`) must come from the
  SMMU's reserved-SID assignment for each PCIe Root Complex. Adjust
  to match your SoC's integration.
- `iommu-map` covers all 16-bit RIDs from each Root Complex so any
  endpoint plugged into the slot becomes SMMU-translated
  automatically.
- An `iommus = <&smmu ...>` property on the RC node would let
  the RC platform device's own master traffic (including the
  integrated eDMA exposed via dw-edma) reach the SMMU. On
  designs where a downstream PCIe device's RID maps to the same
  SID, this collides at insert time
  (`stream NNN already in tree`) — pick a non-overlapping SID or
  omit the property and accept that eDMA traffic bypasses SMMU.

## Runtime

### smmu-test-pcie (control-surface verification)

```bash
dmesg | grep smmu-debug
# smmu-debug: /sys/kernel/debug/smmu/ ready

# Pick a PCIe endpoint behind the SMMU. Sysfs alternative to lspci:
for d in /sys/bus/pci/devices/*; do
    bdf=$(basename $d)
    v=$(cat $d/vendor); p=$(cat $d/device)
    drv=$(readlink $d/driver 2>/dev/null | xargs -r basename)
    echo "$bdf  ${v#0x}:${p#0x}  drv=${drv:-<none>}"
done

# Take ownership: unbind from the original driver if any, then bind here.
# (Do NOT do this on a boot disk.)
echo "<vendor_hex> <device_hex>" > /sys/bus/pci/drivers/smmu-test-pcie/new_id
echo "<bdf>" > /sys/bus/pci/drivers/<orig_driver>/unbind   # if drv != <none>
echo "<bdf>" > /sys/bus/pci/drivers/smmu-test-pcie/bind

dmesg | tail
# smmu-test-pcie 0000:01:00.0: smmu-test-pcie ready: buf0_pa=... buf1_pa=... size=65536

DIR=/sys/kernel/debug/smmu/pcie/pcie-test0

cat $DIR/info
# device:      0000:01:00.0
# iommu_group: 0          ← assigned via iommu-map; non-negative = SMMU sees it
# attached:    no

# Attach an UNMANAGED paging domain (alloc + iommu_attach_device).
echo 1 > $DIR/attach

# Map two regions (src buffer = 0, dst buffer = 1, RW = prot 3)
echo "0x100000 0x4000 3 0" > $DIR/map
echo "0x200000 0x4000 3 1" > $DIR/map

# Verify translation walks
echo 0x100000 > $DIR/iova2phys
cat   $DIR/iova2phys
# iova:0x100000 phys:0x<buffer0_pa>

# Verify invalidation: unmap, then iova2phys returns 0
echo "0x100000 0x4000" > $DIR/unmap
echo 0x100000 > $DIR/iova2phys
cat   $DIR/iova2phys
# iova:0x100000 phys:0x0

# Optional: drive a real DMA through SMMU (only works when a
# memcpy-capable dmaengine channel shares this device's IOMMU
# group; on RC-internal eDMA this returns "no matching dma channel")
echo "0x100000 0x200000 0x4000" > $DIR/dma_memcpy
cat $DIR/info | grep dma_last
# dma_last:    PASS, size=16384, status=ok

# Tear down
echo 1 > $DIR/detach
```

The driver does not own a DMA producer of its own. It exercises the
SMMU control surface and translation correctness; full end-to-end
DMA verification requires that the SoC topology lands a memcpy-capable
dmaengine channel in the bound device's IOMMU group. Where that does
not hold, the control-surface verification still proves CMDQ progress,
STE/CD programming, page-table updates and invalidation effect.

## Layering

The driver intentionally provides only **operation primitives**. Test
*scenarios* — stress, churn, bulk mapping, negative cases, regression
suites — are composed in shell on top:

```bash
# Stress: 1000 attach/detach cycles
for i in $(seq 1 1000); do
    echo 1 > $DIR/attach
    echo 1 > $DIR/detach
done

# Churn: many simultaneous mappings
for i in $(seq 0x100000 0x10000 0x10F0000); do
    echo "$i 0x4000 3 0" > $DIR/map
done
cat $DIR/info  # mappings: 16

# Negative: double map → -EEXIST
echo 1 > $DIR/attach
echo "0x100000 0x4000 3 0" > $DIR/map ; echo $?   # 0
echo "0x100000 0x4000 3 0" > $DIR/map ; echo $?   # 17 (EEXIST)
echo 1 > $DIR/detach
```

Scenarios that need a separate framework — fault classification,
domain-mode switching (IDENTITY/BLOCKED), Stage-2 nested, MPAM,
dirty tracking — are best served by `vfio-pci` + an `iommufd`-aware
userspace tool, since they need controlled DMA observation that an
in-kernel driver bound to the device cannot easily provide.
