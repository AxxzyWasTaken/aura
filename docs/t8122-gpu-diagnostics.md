# T8122 GPU discovery from Linux

This personal fork supports AI-assisted M3 Air development. Its first step is
metadata discovery, not GPU activation. The M3 Pro resource and firmware
constants are not evidence for T8122.

Append this option when constructing a **stage-2** boot.bin:

```
chosen.asahi,t8122-gpu-diag=1
```

On T8122 only, after mandatory device-tree fixups, m1n1 copies selected fields
from the iBoot ADT into `/chosen/asahi,t8122-gpu-diagnostics`. It reads ADT RAM
only: no GPU registers, power changes, firmware reads, memory reservations or
GPU enablement. The exported metadata is not a driver handoff.

The exporter accepts only the exact NUL-terminated string `1`. It creates a
private FDT copy, bounds each property to 32 KiB and the total payload to 48 KiB,
and publishes only a complete export. Missing SGX/ASC, exhausted FDT capacity,
allocation failure or oversized data leaves the original FDT unchanged and
allows boot to continue. Existing exports are left untouched. Other chips do
not run this exporter. It does not require a GPU alias in the kernel DTB.

The explicit property allowlist covers register/range metadata, interrupt and
power routing, PMGR devices, performance tables, and GPU/UAT memory-region
descriptions. It never copies arbitrary ADT contents or firmware images.
Missing optional properties stay absent. `complete` means the export finished;
it does not mean all information needed for bring-up exists.

`schema-version` is a big-endian FDT u32. Properties prefixed `adt,` preserve
their raw ADT encoding, normally little-endian. They are deliberately not named
`reg`, `ranges`, or `compatible`, so consumers do not mistake them for bindings.

After an approved installation and reboot:

```sh
python3 tools/read-t8122-gpu-diagnostics.py > /path/outside/repo/t8122-gpu.json
```

The reader verifies the schema and completion marker, preserves the raw bytes,
and labels numeric views by width and endianness without guessing their units.
Keep machine-derived output local. Use the ranges and PMGR data to ground the
T8122 DT before adding any hardware startup code.

Validation:

```sh
sh tests/run-t8122-gpu-diagnostics.sh
python3 tests/test_t8122_gpu_reader.py
make -j4 BUILDSTD=1
```

The host test uses synthetic ADT and real libfdt, including byte-for-byte
rollback across capacity boundaries. In a ptrace-based sandbox, LeakSanitizer
may require `ASAN_OPTIONS=detect_leaks=0`; ASan and UBSan remain enabled.

Before replacing boot.bin, preserve the current stage-2 image independently
and prepare restoration from a known-working environment. A fallback GRUB
kernel cannot recover a failure in m1n1 that occurs before GRUB. Do not alter
macOS/recovery partitions, stage 1, or boot policy for this experiment.

## Decode captured resources locally

```sh
python3 tools/t8122_gpu_resources.py /path/outside/repo/t8122-gpu.json \
  > /path/outside/repo/t8122-gpu-resources.json
python3 tests/test_t8122_gpu_resources.py
```

The decoder uses the layouts in `proxyclient/m1n1/adt.py` and `src/pmgr.c`:
64-bit little-endian arm-io range triples, register pairs, 48-byte PMGR
records, and frequency/voltage pairs. It checks full register extents against
arm-io ranges, follows GPU power-gate parent chains, and resolves state-register
addresses only for nonvirtual devices. Truncation, conflicting translations,
cycles, missing parents, invalid indices, and inconsistent counts are rejected.
Performance values retain their raw units. IRQs retain ADT ordering and the
validity mask; this does not assign mailbox IRQ roles.

The decoder does not establish the mailbox layout, GPU core topology, thermal
model or firmware ABI. It does not turn ADT performance tables into a safe
power model, reserve runtime RAM or enable Linux GPU nodes. Keep both its input
and its machine-derived output outside git. Tests use synthetic records only.

## Exporter revision 2

Revision 2 retains schema 1 and the same opt-in. It adds explicitly allowlisted
SGX thermal/control properties, ASC and optional firmware-nub segment names and
ranges, chosen DRAM bounds and root address-cell widths. Segment descriptions
are ADT metadata, not copied firmware bytes. Missing optional nodes/fields stay
absent; they do not establish firmware compatibility.

`exporter-revision` is a big-endian u32. `usable-memory` is a big-endian u64
base/size pair from the current kboot RAM bounds. All `adt,*` fields retain the
original raw encoding. No GPU MMIO, power transitions, firmware reads or Linux
GPU-node enablement are performed. Property/aggregate size limits and atomic
publication remain unchanged.

The reader supports both the original export and revision 2. When segment
metadata is present, the resource decoder checks physical extents against DRAM
and usable RAM, rejects overlaps with the four UAT regions, validates virtual
extents, and compares optional nub metadata. It does not validate TrustZone
carveouts or firmware contents, reserve memory, or identify a firmware ABI.
The 13-versus-14 pstate relationship is checked against the existing parser's
N-active-states-plus-one-off-state convention, without creating power limits.
