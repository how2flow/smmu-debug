# Zephyr SMMU verification — feasibility analysis

Companion document to the Linux mirror under `../linux/`. Captures
what an SMMUv3 verification effort can realistically achieve on the
Zephyr side, assuming a downstream test framework is available under
`apps/` (the standard pattern for SoC bring-up tests on Zephyr).

This is a feasibility note, not an implementation. No Zephyr code is
mirrored here yet. When the implementation lands it should sit under
`zephyr/apps/<your-test-framework>/...` paths mirroring the Linux
layout.

## Summary

Zephyr SMMU verification is **possible but materially different in
character** from the Linux side. Two distinct scopes exist depending
on whether a vendor PCIe RC driver port is available:

- **Scope A — bare-metal only** (no PCIe driver assumed). Direct SMMU
  MMIO access from the test framework. Verifies identity readback,
  RO write integrity, control register handshakes (CR0/CR0ACK, GBPA),
  and end-to-end command-path liveness via CMDQ ring + CMD_SYNC.
  This is the right scope for silicon bring-up validation in BL2.

- **Scope B — with vendor PCIe driver ported** (and Zephyr's upstream
  NVMe driver alive on the resulting bus). Indirect verification at
  the level of "Linux mounted NVMe without error": bring SMMU up,
  install a single stream-table entry for the NVMe stream ID,
  exercise NVMe I/O, classify pass/fail per STE configuration
  (Bypass → I/O must succeed, Abort → I/O must fail). This covers
  TC-08 (bypass/abort) more decisively than the Linux side can.

The two scopes are additive. Scope A is reachable independently;
Scope B is gated on the PCIe driver port and is the higher-value
target.

Zephyr verification **complements** the Linux side. The Linux driver
verifies the control surface and translation correctness via
`iommu_iova_to_phys`. Zephyr verifies bring-up sequence integrity
and SMMU gating behaviour under real traffic. Neither covers the
other's strengths.

## Context — why a separate document

The Linux mirror (`../linux/`) provides a debugfs-driven driver for
SMMU control-surface verification using the kernel IOMMU API. That
driver intentionally never touches SMMU MMIO directly; all work is
mediated through `iommu_domain_alloc`, `iommu_attach_device`,
`iommu_map`, etc.

Zephyr has no IOMMU subsystem at all. There is no `iommu.h`, no
`iommu_domain`, no helper to walk page tables, no notion of an IOMMU
group. The only path from a test application to the SMMU is direct
`sys_read32` / `sys_write32` against MMIO. This forces a
fundamentally different test strategy: where Linux exercises API
behaviour, Zephyr exercises HW bring-up sequence correctness.

Treating the two efforts as one verification driver would obscure
this distinction. They are mirrored side-by-side as siblings.

## Environment survey

| Item                          | Expected state |
|---|---|
| Zephyr version                | 3.7 LTS or compatible |
| Downstream test framework     | `SOC_TEST_DEFINE` style — registration macro, PASS/FAIL/SKIP enum, `SOC_TEST_REG_RD32/WR32` |
| Target board variants         | one or more boards covering the SoC, including a BL2 variant for silicon bring-up |
| GIC v3 / ITS drivers          | present (Zephyr upstream) |
| PCIe host driver              | upstream generic `pcie_controller` DT-COMPAT is present; vendor RC glue is what Scope B depends on |
| NVMe driver                   | present (`drivers/disk/nvme/`); depends on `PCIE` + MSI-X |
| SMMU / IOMMU driver           | **absent** (no IOMMU subsystem in upstream Zephyr) |
| IOMMU API                     | **absent** |
| Execution level               | BL2 variants run S-EL1 / EL3; non-BL2 variants run EL1 NS |
| Platform Kconfig flags        | `SOC_TEST_PLATFORM_{SILICON,EMULATOR,VIRTUALIZER}` style flags allow per-platform skip |

The downstream framework is assumed to expose the following surface:

```c
#define SOC_TEST_DEFINE(name, group, fn, flags)
#define SOC_TEST_CHECK(cond, msg)
#define SOC_TEST_CHECK_EQ(actual, expected, msg)
#define SOC_TEST_REG_RD32(addr)
#define SOC_TEST_REG_WR32(addr, val)
enum soc_test_result { SOC_TEST_PASS, SOC_TEST_FAIL, SOC_TEST_SKIP };
```

If the framework names differ, the tests are framework-agnostic in
logic and can be retargeted.

## Scope A — bare-metal SMMU verification

Reachable today; no dependencies beyond an SMMU MMIO base address.

### Test list

| Test name              | What it verifies                                    | Cross-ref to TC |
|---|---|---|
| `smmu_idr`             | `SMMU_IDR0..5` readback non-zero, fields consistent | TC-01 |
| `smmu_iidr`            | `SMMU_IIDR` non-zero (implementer/revision present) | TC-01 |
| `smmu_aidr`            | `SMMU_AIDR` reports a supported architecture rev    | TC-01 |
| `smmu_pidr_cidr`       | `SMMU_PIDR0..7` / `CIDR0..3` JEP-106 signature      | TC-01 |
| `smmu_ro_integrity`    | Write attempts to IDR / IIDR / AIDR leave RO bits unchanged | TC-01 (RAZ/WI) |
| `smmu_cr0_handshake`   | `CR0 = 0` followed by `CR0ACK` poll converges       | TC-02 |
| `smmu_gbpa_abort`      | `GBPA.Update + ABORT` handshake latches cleanly     | TC-02 |
| `smmu_cmdq_sync`       | Statically allocated CMDQ + `CMD_SYNC` round-trip   | TC-06 (the meat) |

The first four are extremely cheap (~10 lines each) and catch gross
silicon-integration mistakes early — wrong MMIO base, clocks/resets
not enabled, IDR fields not matching the expected feature set.

`smmu_ro_integrity` and the handshake tests exercise the only
fully-isolated SMMU behaviour reachable without setting up a master:
the global control mirrors and the GBPA abort latch.

`smmu_cmdq_sync` is the unit-of-real-work for this scope:

1. Allocate a CMDQ ring as a static `__aligned(SMMU_CMDQ_ALIGN)` array.
2. Program `SMMU_CMDQ_BASE_LO/HI`, init `PROD = CONS = 0`.
3. Set `CR0.CMDQEN`, poll `CR0ACK.CMDQEN`.
4. Build a `CMD_SYNC` (with `CS = NONE`, since we'll poll).
5. Advance `CMDQ_PROD.WR`.
6. Poll `CMDQ_CONS.RD` until it catches up, with a generous timeout.
7. PASS if consumer advances to the submitted index without error in
   `GERROR`.

This proves that the entire command-path infrastructure — queue base
programming, enable handshake, command consumption, sync semantics —
is wired correctly in this silicon. Failure here is a hard bring-up
blocker. It is also the right place to surface `GERROR.CMDQ_ERR` and
queue-write abort bits via dedicated check messages.

### Limitations of Scope A

- No verification that **translation** actually works (no master is
  issuing transactions).
- No event-queue verification (no faults produced; event-queue
  programming alone is covered as part of the bring-up setup but
  cannot be observed to consume entries).
- No verification of the PCIe RC's iommu-map binding (there is no
  binding to verify in this scope).

## Scope B — indirect verification via NVMe (vendor PCIe ported)

Reachable only once a vendor PCIe RC driver exists in Zephyr. Builds
on the upstream NVMe driver (`drivers/disk/nvme/`) to produce real
PCIe DMA traffic that traverses the SMMU.

### Verification logic

The Linux side proves "SMMU operates correctly" indirectly by
observing that NVMe enumeration, mount, and I/O succeed while the
IOMMU group is assigned and the kernel log is fault-free. The Zephyr
port reproduces this exact reasoning with one strict tightening: in
Zephyr we control the SMMU bring-up explicitly, so we can isolate
the SMMU-on / SMMU-off / SMMU-blocking conditions deliberately.

| STE for NVMe SID | Expected NVMe I/O | Conclusion if met |
|---|---|---|
| (SMMU disabled — baseline) | succeeds | I/O path itself works; nothing said about SMMU |
| `STE.Config = bypass`      | succeeds | SMMU is in the data path and passes traffic correctly |
| `STE.Config = abort`       | **fails (timeout)** | SMMU correctly gates the same traffic |

If all three observations hold, SMMU traversal is operational under
real load. The Bypass → Abort transition is the missing leg from the
Linux verification, and is what makes Zephyr genuinely additive.

### Test list

| Test name                | STE configuration | Expected NVMe I/O |
|---|---|---|
| `smmu_nvme_disabled`     | (SMMU disabled, baseline)  | succeed |
| `smmu_nvme_bypass`       | bypass for NVMe SID        | succeed |
| `smmu_nvme_abort`        | abort for NVMe SID         | timeout |

### Test skeleton

```c
static enum soc_test_result test_smmu_bypass(void)
{
    smmu_disable();                                  /* clean reset state */
    smmu_program_cmdq(cmdq_buf, CMDQ_LOG2SIZE);
    smmu_program_strtab(ste_buf, STRTAB_LOG2SIZE);

    smmu_ste_bypass(&ste_buf[NVME_SID]);             /* single STE */
    smmu_enable_cmdq();
    smmu_cmd_cfgi_ste(NVME_SID);
    smmu_cmd_sync();                                 /* fence */
    smmu_enable_translation();                       /* CR0.SMMUEN */

    int ret = nvme_smoke_test();                     /* identify + 4 KiB read */

    SOC_TEST_CHECK(ret == 0, "NVMe I/O failed with SMMU bypass STE");
    return SOC_TEST_PASS;
}

static enum soc_test_result test_smmu_abort(void)
{
    smmu_disable();
    smmu_program_cmdq(cmdq_buf, CMDQ_LOG2SIZE);
    smmu_program_strtab(ste_buf, STRTAB_LOG2SIZE);

    smmu_ste_abort(&ste_buf[NVME_SID]);
    smmu_enable_cmdq();
    smmu_cmd_cfgi_ste(NVME_SID);
    smmu_cmd_sync();
    smmu_enable_translation();

    int ret = nvme_smoke_test();                     /* expected to time out */

    SOC_TEST_CHECK(ret != 0, "NVMe I/O succeeded with SMMU abort STE");
    return SOC_TEST_PASS;
}
```

### Data flow

```
[NVMe SQ doorbell] --writes--> [NVMe controller in EP]
                                         |
                                         | DMA at PA from NVMe driver
                                         v
                              [PCIe RC: tags transaction with RID]
                                         |
                                         | iommu-map applies RID -> SID
                                         v
                                  [SMMU TCU]
                                         |
                          +--------------+--------------+
                          |              |              |
                       bypass        translate         abort
                          |              |              |
                          v              v              v
                       [DRAM]   [walk via CD/PT]   [fault, no transit]
                          |              |              |
                       I/O ok       I/O ok        I/O timeout
                                   if PT 1:1
```

Scope B only exercises the bypass and abort legs. Translate-via-PT
is left to the Linux side, where the io-pgtable layer builds and
maintains the page tables.

### Why bypass + abort is enough

The user-level conclusion we want — "SMMU is wired into the data
path and behaves correctly" — does not need PT translation to be
exercised in Zephyr. It needs:

1. Traffic with the SMMU enabled and **configured to pass** must
   succeed. This proves the SMMU is not a no-op fall-through (which
   could mask a broken SMMU that happens to bypass everything
   anyway).
2. The *same* traffic with the SMMU configured to **block** must
   fail. This proves the SMMU is actually in the path and consulted.

Bypass and abort STEs are sufficient and they isolate the binary
question cleanly. Translate-via-PT would muddy the signal: a failure
could be a PT bug rather than an SMMU bug.

## Architecture — what makes Zephyr workable

A few facts about the BL2 / Zephyr execution model make Scope B
tractable in a way it isn't in Linux:

- Zephyr typically runs with an identity MMU (virt == phys) or no
  MMU at all. DMA buffers are static arrays in BSS; their physical
  address is their virtual address. No `dma_map_single` plumbing
  needed.
- BL2 runs in S-EL1 / EL3, so the full SMMU MMIO including secure
  registers is reachable. Tests run before Linux has had a chance
  to bind anything.
- The SMMU starts in reset / disabled state. The test owns it
  entirely. After test completion the test should set `CR0 = 0` and
  poll `CR0ACK` so the next boot stage finds the SMMU in a clean
  state.
- There is no concurrency: the test is the only thread, no preemption
  concerns around CMDQ pointer updates.

In Linux, none of these hold. The kernel owns the SMMU exclusively
once it has probed, the earlier boot stages have typically already
configured the SMMU one way or another, and the test driver has to
coexist with the kernel's view. Hence the very different test
design.

## Work estimates

### Scope A

| Component                                                       | Lines |
|---|---|
| SMMUv3 register / structure definitions header                  | ~120 |
| SoC base-address addition (`SOC_SMMU_*_BASE`)                   | ~5 |
| Test source (8 tests + helpers)                                  | ~350 |
| **Subtotal**                                                     | **~475** |

The CMDQ_SYNC test is the heaviest single component; the four
identity readback tests collectively take ~50 lines.

### Scope B (added on top of Scope A)

| Component                                                        | Lines |
|---|---|
| NVMe-driven SMMU tests (3 tests + helper)                        | ~250 |
| `nvme_smoke_test()` helper                                       | ~80 inside the file |
| **Subtotal**                                                     | **~330** |

Total of A + B: ~800 lines of new test code. Excludes the vendor
PCIe RC driver port itself, which is a separate and much larger
piece of work outside this analysis.

## Layout (proposed, when implemented)

```
zephyr/
└── apps/<test-framework>/        (downstream consumer path placeholder)
    ├── Kconfig.note              edits to register the new tests
    ├── CMakeLists.txt.note       list new sources
    └── src/
        ├── soc/ip/
        │   └── smmuv3.h          register offsets, STE/CD layout
        ├── soc/<vendor>/<soc>/
        │   └── <soc>_regs.h.add  SOC_SMMU_*_BASE addition
        └── tests/
            ├── test_smmu.c       Scope A: 8 tests
            └── test_smmu_nvme.c  Scope B: 3 tests
```

## Linux ↔ Zephyr complementarity matrix

| TC family                              | Linux side                          | Zephyr side                         |
|---|---|---|
| TC-01 capability discovery             | indirect (kernel log / sysfs)       | **direct, focused readback**         |
| TC-02 disable/abort handshake          | not exercised (kernel-owned)        | **direct handshake test**            |
| TC-03 queue/STRTAB bring-up            | indirect (probe sequence)           | **bring-up is the test**             |
| TC-04 interrupt routing                | observed via `/proc/interrupts`     | exercise IRQ_CTRL/ACK                |
| TC-06 CMDQ progress                    | ✓ (every map/unmap)                 | **CMD_SYNC round-trip**              |
| TC-07 S1 attach + translation          | ✓ (`iova2phys`)                     | bypass STE only (no PT)              |
| TC-08 bypass / abort                   | partial (default restore only)      | **bypass + abort STE, indirect via NVMe** |
| TC-09 event queue / fault              | not exercised                       | not exercised in either scope        |
| TC-15 PMCG                             | gated on PMCG DT info               | gated on PMCG DT info                |
| TC-17 nested S1+S2                     | requires KVM/iommufd                | not feasible                         |
| TC-18 HTTU                             | requires iommufd                    | not feasible                         |
| TC-19 MPAM                             | requires driver patch               | not feasible                         |

The strong items in each column are non-overlapping. Together they
cover TC-01 through TC-08 to a defensible standard.

## Hard limitations on the Zephyr side

These remain out of reach even in Scope B:

- **Page-table translation correctness** (TC-07 walk verification).
  Zephyr has no io-pgtable equivalent. Building and validating
  multi-level page tables by hand is a separate, large effort and is
  better left to the Linux side where the kernel provides it for
  free.
- **PMCG-based perf observation** (TC-15). Same DT/SoC-info gap as
  on the Linux side; not a Zephyr-specific limit.
- **PASID / SSID** (TC-14). Zephyr's NVMe driver does not use PASID;
  there is no PASID-capable master to exercise.
- **Nested translation, dirty tracking, MPAM** (TC-17/18/19). All
  require infrastructure that simply does not exist in Zephyr.

## Practical caveats

A few items that will need attention during implementation, listed
here so they aren't surprises:

- **Cache coherency.** SMMU STE / CD / CMDQ tables placed in normal
  cacheable BSS must be coherent with the SMMU's view, or the SMMU
  walker has to be told to do non-coherent walks. The Linux driver
  inherits coherency settings from DT (`dma-coherent`); in Zephyr we
  need to either rely on platform coherency or pin tables in
  non-cacheable memory via the MPU/MMU map. Decide per-board.
- **NVMe interrupt path in BL2.** NVMe completion uses MSI-X. In a
  BL2 S-EL1 environment, MSI delivery requires GICv3 ITS to be set
  up in a way compatible with secure execution. LPI delivery is
  Group 1 NS only in GICv3, so the `smmu_nvme_*` tests may need to
  live on a non-BL2 board variant — or the test stub may need to
  use NVMe polling mode (CQ head doorbell polling) to avoid the
  MSI question entirely.
- **Stream Table size.** Even a "single STE" test allocates the
  full linear table sized to cover the highest SID in use. If the
  iommu-map range is large (e.g., 64K SIDs), a linear table at full
  granularity is large. A two-level table is the right answer; this
  is non-trivial in hand-written bring-up code. Alternative: shrink
  the SID range in the Zephyr-only DT to keep the linear table
  small.
- **`CR0.SMMUEN` is sticky on some implementations.** Reset to
  disabled state at the end of each test, regardless of outcome, so
  the next test starts deterministically.
- **Abort test timing.** The "NVMe I/O times out" assertion requires
  the timeout to be tight enough to keep the test under the
  framework's per-test budget but long enough that the absence of
  completion is a real signal (not a slow path). Measure the
  success-case latency first, then set the abort timeout to a
  comfortable multiple of that.

## Recommended implementation order

1. **Scope A, readback tests first** (`smmu_idr`, `smmu_iidr`,
   `smmu_aidr`, `smmu_pidr_cidr`, `smmu_ro_integrity`). Lowest risk,
   exercises the framework wiring, immediately useful as silicon
   bring-up signal.
2. **Scope A, handshake tests** (`smmu_cr0_handshake`,
   `smmu_gbpa_abort`). Still no memory allocation, just register
   writes with poll loops. Catches a different class of integration
   bugs.
3. **Scope A, `smmu_cmdq_sync`**. First test that allocates a
   HW-visible memory region. Once this passes, all of CMDQ wiring
   is proven.
4. **(Gate) Vendor PCIe RC driver port** to Zephyr. Out of scope
   here.
5. **Scope B, `smmu_nvme_disabled`** baseline. NVMe smoke-test
   without touching SMMU — proves the NVMe path itself works before
   we add SMMU on top.
6. **Scope B, `smmu_nvme_bypass`**. The headline result.
7. **Scope B, `smmu_nvme_abort`**. The complement that closes the
   gating leg.

After step 6 the Zephyr side has reached the "Linux mount succeeded"
verification standard, and after step 7 has surpassed it on
TC-08-style gating.

## Status

Scope A is implementable now. Scope B is gated on the vendor PCIe
RC driver port being available in Zephyr; no estimate is given here
for that piece because it is not an SMMU verification artefact.

When implementation begins, this document should be updated to point
at the actual sources and to note any deviation from the recommended
plan above.
