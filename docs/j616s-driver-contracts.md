# J616s driver contracts

Use this as the implementation checklist. [Bring-up evidence](j616s-bringup.md)
records the tests and limitations behind these contracts. A successful control
reply is not evidence of samples or completed frames.

## Admission and ownership

The tested target is Mac16,7/J616s, chip 0x6040, board 6. The recent proxy
captures use OS firmware 26.6.1, AOP RTKit version 12 and EPIC message/subheader
version 2. Native macOS 27/build 26A428 captures are separate reference evidence;
do not assume their firmware or phandles match the proxy boot.
The latest ADT records stage-two `mBoot-18000.161.9` and stage-one/system
`mBoot-20457.1.29`. DCP is already loaded and running. The stage-two version
therefore does not identify the inherited DCP executable. Its E914 nub UUID is
checked as an ADT property; no executable `LC_UUID` comparison has been made.
Bind the actual image or matching native client before selecting a command
profile.
A subsequent read-only TEXT capture preserves two equal CPU observations and
rechecks the CPU tables, inherited DART tables and controller. Its RTKit UUID
record matches E914 and reports `RTKit-3255.160.4.release`; the runtime TEXT
starts with instructions, so this is not an `LC_UUID` comparison. This binds
the observed code artifact, not firmware-side coherence or query support.

Resolve registers, DMA parents, mapper streams and firmware segments through
the current ADT. Read the active DART mode before using an inherited context
and preserve its tables and other streams. The tested new AOP/SIO mappings
use four levels and 42-bit addresses; inherited DCP stream 23 retains its
36-bit mode and captured root/leaf tables. SoC capability does not authorize
changing an active firmware context. A complete read-only capture covers its
root and five leaves; this does not qualify cache publication for that stream.
Its parameter register reports version 2.2, not version 3.
Conditional publication and BUSY-poll helpers passed private Normal-RAM tests
with actual mapping/core admission and full-page readbacks. Their success does
not establish ownership or coherent write permission for the firmware root
or permission to acknowledge the AFK buffer. A separate test completes one
version-2.2 range/STT command over a vacant 16 KiB DVA range, with zero faults
and unchanged inherited tables and context. A later experiment publishes one
owned leaf into vacant root slot 8 and completes that selective command. Raw
captures verify one root-word change and one additional full-page mapping,
preserving every inherited mapping. This establishes the tested CPU-visible
sequence, not hardware walker visibility, AFK buffer DMA or permission to
invalidate firmware-owned slots.
The subsequent fresh AFK exchange initializes the owned 16 KiB arena through
firmware DMA. Both 8 KiB rings have a measured 128-byte block stride and valid
zero pointers. Full arena readbacks and exact table-preservation checks pass;
that passive test does not start queues. Derive ring stride from the returned
header rather than assuming 64 bytes. A subsequent queue-startup test passes.
The observed display service uses AFKEPV2 on
channel/interface 3 and publishes the direct name `disp0-service`; do not send
legacy EPIC headers to it. RECV is an opaque notification; use the live header
for pointers. Service queries and scanout remain unqualified.

The source's `afkv2_open` sends a 24-byte inline packet: an eight-byte header
with interface 3 and length 16, followed by a zero-token message with type
`0x12`, category 0 and flags 1. The IOP queue header has channel and type zero.
For the observed empty TX ring this occupies one 128-byte block. Publish the
entry before TX WPTR=128, complete the producer barrier, then send
`0xa2000000000080` on endpoint `0x23`. The source opens the service before
acknowledging its RX announcement. Preserve firmware-owned pointer blocks;
do not clean an entire active ring after modifying one producer field.
This inline OPEN sequence now passes a fresh RAM-only test. TX RPTR and WPTR
both reach 128 and the first RX announcement is acknowledged. Full arena
readback differs only in the entry and expected pointer words; exact inherited
table comparisons and cold recovery pass. This establishes queue consumption,
not successful service binding; require a matching application response before
claiming that. OOB query buffers and display commands remain unqualified.

A later experiment publishes three adjacent 16 KiB mappings for the arena,
query TX and query RX pages. One vacant root word points to the retained private
leaf; two complete post-publication captures preserve all 9247 inherited
mappings and verify exactly three additions. The version-2.2 range command
completes over the resulting 48 KiB interval. These are CPU-visible table and
command checks; the two OOB pages have no functional DMA qualification yet.
Firmware consumes the first `GET_HPD` queue entry, but the four captured images
contain no matching reply. A subsequent diagnostic observes no DART fault, an
empty outbox and an all-zero owned RX page. Query dispatch and its response
contract remain unresolved. Do not infer unsupported HPD or a native command
timeout from the approximately one-second queue capture.
The experimental sequence above stops management negotiation at Hello/map;
it does not complete the upstream system-endpoint and AP/IOP power handshake
before starting EP23. Do not use it as a complete initialization recipe. The
source-backed next protocol gate is mandatory system handling and both power
states at ON before applications; its omission is not yet proved to be the
cause of the missing HPD reply.
The existing AFKEPV2 command implementation derives from T8152 work, and the
iBoot setter contract cites native 26A428 behavior. Neither establishes the
E914/25G76 command serializer. Bind the consumer or matching native client
before changing packet fields; do not transplant a legacy iBoot envelope into
the observed AFKEPV2 queue.
Inspection of the captured consumer confirms the current command/OOB layout
and HPD op/length. Its display callback returns without a reply unless the
display-service initialization state is set. OPEN sets a separate interface
flag and does not directly establish that state. The live owner state remains
unknown; validate its declared-DATA translation and CPU access before reading
it. Do not infer its value from queue consumption or change flags and tokens
without a matching consumer check.
The generic interface's callback is separate from the display owner. Its
offset 120 corresponds to offset 208 in the containing firmware object and
is installed by the event-source path, not OPEN.

A read-only diagnostic verifies the actual CPU descriptors for the declared
DATA owner slot at physical address `0x105ddbfb088`. This boot's region 4 ends
at `0x105db3bc000`; the slot is outside that carveout and has an identity
Normal-WB mapping. All captured CPU table bytes and inherited DART/controller
state pass postchecks, followed by cold watchdog recovery. The diagnostic
reads no DATA bytes and grants no permission to follow a firmware pointer.
Bind the constructor's store instruction against current TEXT and qualify
the exact DATA translation and read scope before observing the owner slot.

A subsequent bounded read matches the constructor instructions twice and
observes the same eight-byte owner slot three times: `0x9befa8`. CPU table,
inherited DART/controller and cold recovery checks pass. The DATA allocation
reservation stays intact; only its exact self-reservation is removed from the
local read plan, with all other overlaps retained. This pointer is not followed.
It lies outside the named firmware segment IOVA intervals, so the firmware's
own virtual-to-DART translation must be established before reading its state
byte at offset `0x51`. Do not add the DART base or DATA relocation bias by guess.
Neither the live display-readiness state nor the active interface's owner
relationship is established by this slot observation.

A later read-only capture observes the fixed firmware MMU fields after matching
their code references: configuration selector 3/address width 32, lower table
record `{0x13f9f80, 21, 0, 8192}`, allocation pool `{0xb05000, 200, 16}` and
table-address conversion delta `0x10008dfe000`. Three CPU observations per field
and all table/controller/recovery checks pass. These are software records, not
an active TTBR/TCR sample. In this epoch the owner slot contains `0xc60fa8`.
The firmware walker uses 4 KiB pages; do not apply the AP/DART's 16 KiB geometry
to it. Resolve the software table-base coordinate and partial-table indexing
before reading a descriptor or following the owner. No pointer walk is qualified.

An independently observed bootstrap relocation is `0x394000` in a later epoch.
The source maps named DATA at its linked IOVA plus that relocation, and the
observed conversion delta matches `TEXT.remap - relocation`. Thus a pointer
inside the relocated named DATA interval can be proposed as `DATA.physical`
plus its segment offset after separate fresh admission. This is not a general
heap or nonlinear root-view conversion. No readiness-byte read is qualified yet.

A changed management-only trial starts EP1 once after Hello/map. It receives
`0x00110100099ec000`, requesting 64 KiB at preallocated DVA `0x100099ec000`.
All inherited table/controller and recovery checks pass. No buffer memory is
read, adopted, mapped or acknowledged, and no application or AP ON command is
sent. Preallocation is not an unconditional echo-ACK capability. This proves
initial system traffic only; complete RTKit startup still requires handlers
for the remaining advertised mandatory endpoints and both power acknowledgments.

The first complete-startup attempt starts the advertised system set and sends
AP ON once. It validates and retains the entire inherited 64 KiB crashlog
descriptor, then stops on IOReport's request `0x0010400000000000`: a new 16 KiB
buffer with no supplied address. No address ACK or application command follows.
Watchdog recovery passes. Full management startup remains unqualified; prepare
one separately owned, zeroed, coherently published and mapped IOReport page
before acknowledging that exact request in a fresh epoch. Preserve the mapping
and backing until cold recovery, including on ambiguous publication.

The owned-page trial publishes one 16 KiB mapping and preserves the inherited
table bytes, but stops before its IOReport reply. A diagnostic repeat isolates
an ASC status change from `0xec` to `0xcc`: only the bit named `IDLE` in the
proxy client changes. CPU control remains `0x10`; the sampled DART fields are
unchanged and its fault register is zero. Both trials recover through the
watchdog. This is not completed management or firmware DMA qualification.
Keep ASC execution status distinct from static translation configuration;
the observed idle transition does not justify ignoring other status changes.

With only that idle bit admitted during management, the next trial completes
the request-correlated IOReport address reply `0x0010410010000000`. Firmware
then sends type `0xc` and type `8` notifications on EP4. The first echo ACK
completes; the second publication is ambiguous when the original eight-second
deadline expires. No power acknowledgment or final management post-proof is
qualified. Retain both buffer and tables through cold recovery; do not retry
an ambiguous ACK or treat this traffic as display-service readiness.

A subsequent trial replaces repeated host transfers of BootArgs and ADT with
exact native comparisons against retained private copies. Current code and CPU
mapping proofs, whole-buffer readbacks, and first/middle/last changed-byte cases
qualify both references. Ownership, CPU and watchdog checks remain in place.
The median full guard takes 3.7 ms. Within the unchanged eight-second phase,
nine IOReport notification ACKs complete, but no IOP or AP power ACK arrives.
Recovery passes; management completion and display queries remain unqualified.

Clean all new page tables, execute the publication barrier, invalidate the
affected stream and verify the entire translation before enabling its DMA.
Map firmware text read-only. These are owned mappings, not permission to map
an adjacent aperture or copy another SoC's addresses.

Arm the validated hardware watchdog before starting an IOP or publishing DMA
memory. Bound waits and check exceptions and DART faults. Record an operation
as pending before handing ownership to hardware. On ambiguous failure, stop
issuing commands and retain the allocation, tables and IOP until recovery.
Do not free memory merely because a command timed out. AOP shutdown does not
yet provide a reliable ownership-release acknowledgement.

The C-backed proxy API takes `dart_init(base, sid, dart_type, keep_pts=False)`;
the type remains the third Python argument. Persistent `dart_map` buffers must
be caller-owned target addresses, not byte strings passed for temporary upload.
With the updated m1n1 image, failed DART commands raise `ProxyRemoteError`.
Retain the backing memory after an uncertain map or unmap until recovery.

## Audio transport and mappings

Use the advertised `aop-audio` service on endpoint 0x22 after application queue
startup. `AudioAttachDevice`, `GetDeviceProp` and `SetDeviceProp` are defined in
[ipc.py](../proxyclient/m1n1/fw/aop/ipc.py). Calls use EPIC version 2. Reject
nonzero return codes and malformed reply lengths before interpreting payloads.

| Path | Tested mapping | Sample contract |
| --- | --- | --- |
| AOP firmware and endpoint buffers | DART stream 0 | External code boot argument uses the ADT remap address |
| LP warm-up ring | AOP DART stream 8 | 768,000-byte ring; producer report has 12-byte frames |
| HQ microphone | `admac-leap-ns`, mapper stream 10, logical channel 1 | Split RX0 bank 32; three-channel Float32LE, nominal 48 kHz |
| Headset microphone | `admac-base-ns`, mapper stream 8, RX2 | Mono 24-bit samples in 32-bit slots, nominal 48 kHz |

LP and headset stream-8 results come from separate tested sequences. They do
not establish concurrent ownership of that stream. J700's secure LEAP provider
and advertised proxy aperture are not J616s's DMA assignment.

### LP readiness required by the tested HQ sequence

1. Require `hpai` and `lpai` state property 200 to report `idle`, and `adpx`
   property 300 to report `(-1, 0)`. Stream 8 must be disabled with an invalid
   TTBR; idle frontend state alone does not establish memory ownership.
2. Allocate a zeroed 770,048-byte, 16 KiB-aligned backing buffer for the
   768,000-byte ring. Publish and verify its stream-8 mapping.
3. The tested mapping is identity: PA equals IOVA. SET `adpx` property 300
   with `<QQ` little-endian PA and 768,000. GET must return that same address
   and exactly 768,000. This setter can expose the
   address before later validation; retain ownership on every attempted SET.
4. Attach `lpai`, `pdm0` and `adpx`, and recheck the published buffer.
5. Request `lpai` `pw1 ` with `unk2=0`, attach `lai `, then request `runn` on
   `lai ` and `lpai` with `unk2=1`. Check both state readbacks. Preserve spaces
   in FourCC strings; use the 48-byte `PowerSetting` encoder for raw `lai ` data.
6. Observe a fresh REPORT/subtype 0x20 after arming the report handler. The
   tested payload is 0x68 bytes, starts with words 0x6c706169/0xc3000008, and
   contains a little-endian u64 counter at 0x40 and cursor at 0x60. Validate
   `cursor == ((counter + 1) * 12) % ring_bytes`.

Keep LP running and retain its ring throughout the tested HQ capture.
The last check establishes a producer notification, not an HP completion.
Do not substitute cached `hppx` enable flags for it. The legacy 631-byte PDM
configuration does not fit the observed 626-byte property; no new coefficient
upload was required by the working HQ capture.

### HQ one-shot capture

The public command is now qualified for both owned capture extents on the
pinned cold profile. From a recent compatible RAM proxy with cold AOP state:

```sh
M1N1DEVICE=/dev/ttyACM0 python proxyclient/experiments/aop_capture.py /tmp/hqai.wav
```

The default is 6 MiB (nominally 10.92 seconds); `--bytes 0x200000` selects the
qualified 3.64-second capture. It saves three-channel Float32 WAV, raw bytes,
metadata and a normalized single-channel PCM16 preview. Existing output stems
are refused before opening hardware. The preview records its gain/channel;
normalization is not calibration or proof of recognizable sound. Wait for
watchdog recovery before another capture. Changed firmware fingerprints,
RTKit versions, service identities or layouts fail closed.

Attach `hpai` and request `pw1 `. Require stream 10 to be unused and disabled
before installing its owned mapping. Prepare one owned non-repeating NS RX0
descriptor, start DMA, then request `hpai` `pwrd` with `unk2=1`. Check its
property-200 readback. The tested channel configuration is 32-bit bus width,
one-word frame, SRAM carveout `(0x800, 0x800)` and burst word `0xc00060`.
Use [admac.py](../proxyclient/m1n1/hw/admac.py) rather than recreating split-bank
address arithmetic.

Tested descriptor extents are 2 MiB and 6 MiB. The latter contains 524,288
complete three-channel frames, nominally 10.9227 seconds. Both cold captures
begin with 7,211 zero frames, about 150 ms. A 16 KiB transfer is shorter than
that interval at this format; initial silence is not evidence of a dead path.

Require the matching descriptor ID, zero residue, and descriptor/report rings
with EMPTY=1, FULL=0 and ERR=0. Other ring bits are not qualified error flags.
Require zero DART faults. Invalidate the complete owned buffer before each
download and
obtain two matching full copies while HP remains `pwrd`, before cleanup. Retain
partial-frame bytes in raw evidence; the 2 MiB extent has an eight-byte tail.
After copying, stop RX and reset its rings before returning HP through `pw1 `
to `idle`. Clock accuracy, continuous streaming and restart remain unqualified.

Keep the original three-channel float data. A mono average with software gain
is a listening preview, not calibration or beamforming. Never decode these
float words as signed integer PCM.

### Headset input

Use [CS42L84Input](../proxyclient/m1n1/hw/codecs/cs42l84_input.py) for the tested
input-only codec lifecycle. The caller owns board power/reset, the fresh
109-byte MCA profile, DMA preparation and receiver start/stop. The documented
`MIC_DET_CTL4.LATCH_TO_VP` initialization is required by the working capture;
matching the 32-bit TX framing alone was insufficient. Stop TX and RX before
restoring codec/controller state. This does not qualify playback or speakers.

### Headphone output reference

Native macOS 27 build 26A428 uses the separate `audio-codec-output` node
(phandle 442, DMA parent 455). Its current `coS ` profile selects `ms02` TX,
rising edge, delay 1, two channels with mask 3, 24-bit samples in 32-bit slots,
48 kHz and a 3.072 MHz clock. The TX routing mask is 4. A finite proxy test
completed stereo DMA and the user heard the separate left/right tones.
Simultaneous input/output and continuous playback remain unqualified.

The native CS42L84 setup routes `BUS_DAC_SRC` at 0x4001 to 0xed: DAC A takes
ASP RX channel 1 and DAC B takes channel 2. It enables both ASP RX channels
at 0x5020. The output DMA records use ID 4; input records use ID 5. Bind the
controller's direction and channel selection before submitting output DMA.

The native `PrepareIo` output branch switches away from RCO, configures the
common TDM bus, configures headphone routing and slots, then powers the
headphone path. Its post-DAC call through hardware field 0x48 notifies Mikey
headset detection (`mikeyChangingHPOUTEnabled`); it is not an IIS clock-enable
call. Preserve shared clock ownership when testing input and output together.
Qualify left and right separately, then verify stop and restoration. The
tested two-second transfer used left 440 Hz and right 880 Hz, 20 ms fades,
PCM peak 0.25 and DAC attenuation of 60 dB. The matching report, zero residue
and DART status, muted DAC stop and codec/controller restoration all passed.
The user confirmed audible output; no acoustic calibration follows from it.

### Speaker amplifiers

The master `audio-speaker` node links six SN012776 amplifiers. Provider order
is left woofer 1, right woofer 1, left woofer 2, right woofer 2, left tweeter,
right tweeter. Their bus/address pairs are `i2c1:38`, `i2c3:3b`, `i2c1:39`,
`i2c3:3c`, `i2c1:3a`, `i2c3:3d`. All six returned page 0, shutdown mode
0x1a and identity 0x30 after releasing the shared AP GPIO reset at pin 0x36.

A bounded proxy test configured all six while keeping them shut down. Native
configuration includes DC blocking, over-power protection, board BOP bytes,
test-page writes, per-amp RX/I/V slots and TX zero-drive masks. The first
shutdown transition includes the native test-page 0x64 wrapper and a 1 ms
dwell. Configure calls `setOperationalMode(false, true)`, which selects
protected shutdown 0x82. It does not select muted operation 0x81.

The test set digital volume to 0xc9 before configuration and expanded native
broadcasts one amp at a time. All 534 byte writes completed. Final page,
mode, identity and volume readbacks matched 0/0x82/0x30/0xc9 on each amp;
each I2C controller and the shared reset GPIO were restored. No speaker DMA
or output was enabled. These results qualify communication and configuration,
not protection efficacy, feedback decoding or audible playback.

The native `spkr` / `spS ` profile uses MCA group 0 and `ms00`: six 24-bit
playback samples in 32-bit slots, nominally 48 kHz. Feedback uses twelve
16-bit slots; its native sample-width field is zero, so the decoded sample
format remains unqualified. The ADT selects ADMAC TX0/RX1 and mapper stream
8. A subsequent cold control test attaches `spkr`, checks its exact 109-byte
profile before and after power changes, and returns it through `pw1 `,
`pwrd` with output flag 2, `pw1 ` and `idle`. All state readbacks match.

A finite zero-data TX0 test then completes 1,152,000 bytes using a dedicated
2 MiB stream-8 allocation at IOVA 0x80000000. Both complete payload reads are
zero before submission. The AP policy selects the supplied 32-bit DMA record,
preserves FRAME and the firmware-published adapter settings, and uses an owned
0x480-byte FIFO with 0x240/0x480 transfer/almost-full thresholds. The matching
report, zero residue, empty rings, zero DART faults, TX configuration restore,
frontend idle and watchdog recovery pass. All amps remain held in reset.
This qualifies finite silent DMA. A subsequent left-woofer pulse has audible
user confirmation; feedback remains untested.
Audio channel count alone does not establish an ADMAC FRAME encoding.

The tested left pulse enables only native slot 0 / `i2c1:38`, using 300 Hz,
peak 0.125, 20 ms fades and DVC 0x78 (60 dB attenuation). The native protection
configuration precedes muted mode 0x81 and a measured minimum 33 ms dwell.
Fresh profile, power, owned payload/translation and DMA-readiness checks precede
active mode 0x80. The matching completed report triggers mute before TX stops
or restores its fields. Cleanup restores minimum volume, uses the native
test-page 0x64 shutdown wrapper and 1 ms dwell, then restores controllers/GPIO.
The measured active interval is 1.219 seconds for the one-second payload.

The right diagnostic enables only slot 1 / `i2c3:3b`, with full protection on
that amp and the other five left in verified shutdown 0x1a. Its finite transfer,
cleanup and recovery pass; a repeat has user-confirmed audible output. DVC 0x65
is about
the midpoint of the documented inverted 201-step mixer range, not a calibrated
macOS volume-slider percentage. Full-array setup still has intermittent I2C
completion failures. Keep uncertain state for watchdog recovery and retain
XEN completion/error checks; unknown status bit 29 is not an acknowledgement.

A five-second paired first-woofer test at that codec setting is user-confirmed
audible. It uses an owned 6 MiB output allocation for 5,760,000 payload bytes.
The matching report, zero residue/DART faults, both post-report mute writes and
TX configuration restoration pass. A later I2C timeout in the amplifier
shutdown wrapper prevents complete cleanup; resources remain retained until
watchdog recovery. This qualifies paired playback, not reliable repeated
cleanup, all-six output, continuous streaming or feedback calibration.

All six drivers now have user-confirmed individual tone output. The second
left/right woofers use their native RX slots 2/3 and 300 Hz; the left/right
tweeters use slots 4/5 and 2 kHz. Every new test is limited to one second with
peak 0.125 and 20 ms fades. Full protection is reapplied to the selected amp;
the other five remain in their freshly verified factory shutdown state.
Matching TX reports, zero residue/DART faults, mute before TX stop, protected
shutdown, exact controller/GPIO restoration and watchdog recovery all pass.
DVC is 0x65 for the second woofers and left tweeter, 0x78 for the right tweeter.
These levels are finite test settings, not calibrated system volume.

The Python I2C helper clears only the observed status snapshot and rejects XIP
before writing it. Its direct calls are qualified on both idle speaker buses;
private bounded amp trials use the same clear policy with their stronger
ownership, error and XEN/no-XIP completion gates unchanged. The existing generic
transfer methods still reset FIFOs before this check, so the helper change
alone does not make their entire operation safe for concurrent clients.
Successful shutdown and four new playback cleanups do not establish that the
intermittent STOP fault is resolved. I/V decoding, feedback framing, acoustic
calibration, simultaneous all-six and continuous operation remain unfinished.


## ISP completed colour frames

Compose the existing [startup](../proxyclient/m1n1/fw/isp/isp_startup.py),
[sensor](../proxyclient/m1n1/fw/isp/isp_sensor.py),
[capture](../proxyclient/m1n1/fw/isp/isp_capture.py) and
[buffer](../proxyclient/m1n1/fw/isp/isp_buffer.py) components. The profile pins
the firmware/ADT layout. Check IMX958 identity and complete preset replies
before selecting the render route.

The repeated-capture tests use 1280x720 P010: two-plane YCbCr 4:2:0 with MSB-aligned
10-bit values in little-endian 16-bit words, stride 2560. Its aligned Y/UV
extents are 1,851,392/933,888 bytes, totaling 2,785,280. Use the returned layout,
not tightly packed width/height arithmetic. This is the driver's aligned
`output_geometry`, not an independently returned firmware allocation size.
BT.709 video-range conversion in
[isp_pixels.py](../proxyclient/m1n1/fw/isp/isp_pixels.py) produces a display PNG.

A cold 1920x1080 test now receives one completed P010 frame. Its stride is
3840 and aligned Y/UV extents are 0x3f8000/0x1fc000, totaling 0x5f4000.
Both full reads match; CH_STOP, zero faults and NOP pass. The trial queues one
owned output while retaining the native pool capacity of two and existing
allocation limits. This qualifies a completed still frame, not continuous
1080p or measured FPS. Repeated two-surface tests remain qualified at 720p.
The current profile advertises a 256 MiB shared aperture; the broker separately
limits retained allocations to 128 MiB, 128 allocations and 32 MiB per request.
Increasing one budget does not establish that the other limits can be raised.

Receive a completed-frame report and match both plane addresses, pool, plane
count and tag to exactly one submitted owned surface. Copy and validate before
requeue. Continue servicing allocation and terminal messages during bounded
downloads; an ACK or stable memory after stop does not establish completion.
Require CH_STOP and zero faults for clean stream stop. This does not qualify
firmware shutdown or allocation release: tested clients retain firmware
mappings, broker allocations and buffers until watchdog/cold recovery, even
after CH_STOP. Retain leases on ambiguous failure.
Intermittent streaming stalls, exposure/noise calibration and measured FPS
remain open. Do not implement guessed opcodes to compensate for a stall.

## PHY processor prerequisites

T6040 SuperSpeed remains unqualified. The saved PHY0 ADT references phandle
102, the RTBuddy-v2 nub under `/arm-io/acio-phy-cpu0`; its parent has phandle
101 and compatible `iop,mxwrap`. Preloaded/running properties describe
firmware metadata, not current execution or endpoint readiness.

The captured native MXWrap implementation uses these register-bank offsets:

| Register | ADT bank | Offset |
| --- | --- | --- |
| Inbox control | 0 | `0x110` |
| Outbox control | 0 | `0x114` |
| Inbox data, 16 bytes | 0 | `0x800` |
| Outbox data, 16 bytes | 0 | `0x830` |
| CPU run request | 1 | `0x28` |
| CPU idle status | 1 | `0x44` |

Bank 0 is `0x383148000`, bank 1 is `0x383140000`, each with a `0x4000`
aperture. Standard ASC mailbox offsets based at bank 1 coincide with the
native mailbox addresses, but its CPU-control/status layout differs.
Do not use generic ASC boot/shutdown on this processor.

One guarded passive snapshot found AFI, FAB2_SOC, ATC0_COMMON, ATC0_USB_AON
and ATC0_PHYMXWRAP power dependencies active. Both mailbox control words
returned zero without CPU exceptions; no FIFO data was consumed or message
sent. This does not establish a usable mailbox. Native outbox enable changes
control bit 0; mailbox initialization, processor ownership, firmware startup
and the subsequent PHY/PIPE transition still need qualification.

A separate guarded cold test applied native small-sleep and then big-sleep
requests. PHY reset control changed `0x4 -> 0x5 -> 0x7` and status changed
`0x100 -> 0x101 -> 0x103`, with zero CPU exceptions and successful cold
watchdog recovery. The clamp remained set. This qualifies those ordered
request/acknowledgment transitions only; host observation latency does
not establish the native one-millisecond poll timing or USB3 readiness.
A subsequent cold test released the clamp, with control `0x7 -> 0x3`, both
sleep acknowledgments retained and status still `0x103`. Readback and cold
recovery passed without CPU exceptions. Reset-busy remained set; no reset
request, firmware start or mailbox command was performed.
A following cold test cleared bit 0 of PHY0 register 39 offset `0x28`
(`0x383200028`), changing the request word `0x1 -> 0x0` and status
`0x103 -> 0x3`. Both sleep acknowledgments remained set; request/control
readback and cold recovery passed without CPU exceptions. This PHY register
is distinct from the processor's bank-1 CPU run request. Firmware startup
and USB3 readiness remain unqualified.
The native phase-A final control-bit write also passed: `0x3 -> 0xb`, with
status `0x3`, request word zero and successful cold recovery. These register
transitions do not qualify subsequent PLL tunables, crossbar programming,
firmware startup or PIPE/controller handoff.

A guarded ACE3 query of HPM0 logical status register `0x1a` returned
`0x1000b40d`: plug present and orientation bit 4 clear. Controller FIFOs were
empty before and after, and CPU exceptions stayed zero. The query used only
logical-register selection and reads, with no HPM wakeup/shutdown, role or VDM
commands. Recheck orientation after a reconnect; this snapshot does not prove
SuperSpeed cable capability.

A separate guarded HPM0 query of logical data-status register `0x5f`
requested up to 64 bytes and returned four: `f3 00 00 80`. The returned
length is authoritative; the remaining buffer was not read or padded.
Controller FIFOs were empty before and after the single query, and the
proxy completed it without CPU exceptions. Natural watchdog recovery
returned to the installed proxy and retired the temporary allocations.
This tests the ACE3 variable-length read path. The raw reply has not been
bound to native status fields or the PHY processor's tunables-ready bit;
it does not establish SuperSpeed readiness or enumeration.

A later guarded RAM test traversed all 164 compact records in the nine
present native mode-1 tuning tables, then applied the three crossbar writes,
common sleep override and eight override clears for each lane. All 20
crossbar/sleep-control masked readbacks matched, with zero CPU exceptions and
a responsive proxy. Cold watchdog recovery returned the installed proxy
without a physical restart. Each sleep override used the native one-microsecond delay;
extra proxy checks and readbacks mean this was not an exact native timing test.

The tuning helper skips unchanged commands and does not check retained bits
after writing. The experiment kept additional readback observations: 52
commands were unchanged, 110 writes matched and two differed only in bit 31
at AXI2AF offsets `0x40` and `0x48` (`0x80000016 -> 0x16`). These differences
remain unexplained; they are not proof of a strobe or successful hardware
configuration. Traversal completion does not establish PLL lock, processor
firmware startup, PIPE readiness or SuperSpeed enumeration. Crossbar selection
used a fresh HPM status query (`0x1000b41d`, orientation bit set), rather than
the older cable's saved orientation. No VDM or role command was sent.

The native USB interface uses mode 4. A later mode-4 test added its nine
fixed helper RMWs after the common/USB tables and reapplied the six AUSPLL
records before crossbar setup. All nine fixed masked readbacks matched; the
AUSPLL values were already unchanged. Modes 1 and 4 share the crossbar branch,
but mode 4 releases only the orientation-selected lane. All 12 crossbar/common
and selected-lane readbacks matched. Native RUN again produced a complete
RTKit version-12 Hello, with clear CPU exception checks and NOP. The firmware
tunables-done flag remained clear throughout the one-second window (`0x881`), so this does
not establish PLL lock, a ready PHY or a working SuperSpeed gadget.

A subsequent cold RAM test applied the native MXWrap RUN request `0x10` to
processor bank 1 offset `0x28`, after checking prepared control `0x2`, firmware
enable `0xa0003` and both enabled/empty mailbox controls. RUN readback was
`0x10`, processor status was `0x14e` and one outbox message became available.
One complete receive using upstream ASC word ordering decoded as endpoint 0
RTKit Hello, supporting version 12 only. Both raw words were recorded; the
outbox returned to empty, CPU exception checks stayed clear and NOP passed.
No Hello acknowledgment or other firmware command was sent. The firmware tunables-done flag
at `0x383000260` remained clear (`0x881`) throughout the one-second polling window.
This establishes first-message firmware communication, not full RTKit startup
or SuperSpeed. Native IRQ-provider startup remains outside this polling test.
A later HelloAck and complete two-segment map exchange advertises wire
endpoints 0, 1, 32, 33 and 34. The native service name `ACIOPHY0Endpoint1`
refers to wire endpoint 32: its suffix is the wire number minus 31. It must
not be confused with wire endpoint 1. CIO80 application startup and the roles
of wire endpoints 33/34 remain unqualified.

The native timeout diagnostic names bit 10 at `0x383000260` as
`ACIOPHY_TOP_BLK_UC_CTRL_REG.UC_TNBL_DONE`. It must not be treated as a general
PHY-ready or PLL-lock indication. The native phase-C path can continue after
that timeout when its debug-policy bit 31 is clear, releasing PHY reset by
setting register 39 offset 0 bit 4 and delaying 100 microseconds. The live
kernel policy value is not established under proxy; this continuation has
not yet been tested. Recording a timeout and following that path would not
establish firmware tunable completion.

A fresh macOS 27 reference qualifies 10 Gbps on this host, cable and port in
both plug orientations. PHY0's gadget moves from lane 1 to lane 0; removing
the external SSD clears PHY1's host lane without changing the gadget link or
the five sampled USB-core words. The retained native controller matches its
`usb-drd0` provider; its clock-scale field is 13.

Three individually gated native CIO reads return LFPS `0`, bandwidth `0` and
timer `0x3c20060c`, exactly matching the failing v39 RAM activation. Restoring
the older Asahi literals is therefore not justified as a missing native final
state. The compiled v39 activation already skips that setup helper. Its early
PIPE route suppresses USB3 resume, but this native provider selects a delegated
power-manager path that skips the generic direct resume branch. Trace that
delegated path before proposing an ordering change. No m1n1 SuperSpeed
transport is qualified.

Four separately gated native common-PHY observations give CFG `0x1182e210`,
lane mode `0x05041146`, crossbar `0x110` and UC status `0xc81`. This reference
uses lane 0 at 10 Gbps; the failing v39 sample uses the opposite orientation
and requests Gen1. The documented orientation masks explain the paired sleep
overrides and crossbar selector differences. Remaining CFG/lane differences
are not a write recipe. Lane bit 20 is clear in both, so forcing it is not
justified by the working reference.

## Native Linux USB handoff

USB0 has two T8110 DART banks in the ADT. Three RAM-only Linux trials
completed root-shell commands over high-speed CDC with both providers in
the DWC3 `iommus` property, following the upstream T8112 model:

| Bank | Aperture | Stream in the tested Linux model |
| --- | --- | --- |
| 0 | `0x382f00000`, size `0xc000` | 0 |
| 1 | `0x382f80000`, size `0xc000` | 1 |

Both providers use interrupt 1623 and the same low 32-bit DMA window. DWC3
uses interrupt 1619. The existing Apple DART driver supports multiple
providers and shared interrupts; this experiment did not change that driver.
The result does not identify which transactions use each bank.

Before handoff, check both banks in the current proxy session. The tested
bank0 state had no protection or lock, no enabled streams and no valid TTBRs;
all 16 TCRs were 1. Check it again immediately before the vector. Retain the
existing bank1 SID1 and controller retirement checks. Linux's ordinary DART
probe resets the bank, so an old snapshot cannot authorize a later handoff.

The first two trials used notification suppression, omitted empty-transfer
priming and deferred initial requests until the configuration status stage
completed. A later trial kept priming omitted and notifications suppressed,
disabled request deferral, and returned a complete fresh-nonce UID0 command
through the ordinary queue/kick path. It recovered through the original
watchdog. This qualifies one command with those controls, not sustained use.

Restoring empty priming failed configuration even with notifications
suppressed. Enabling notifications on the working skip-only setup also
failed. Increasing the notification allocation to 128 bytes confirmed DMA
alignment while retaining the 10-byte packet, but did not resolve the failure.
These comparisons do not establish its hardware cause. Normal notifications,
ECM and SuperSpeed remain unqualified. Installed images and partitions were
preserved.

## Other implementation boundaries

The Linux implementation draft is
[aurora-silicon/linux#231](https://github.com/aurora-silicon/linux/pull/231),
based on `aurora-wip`. It prepares ownership, profile and retirement checks
without enabling the unqualified camera or streaming audio paths. Use the
proxy evidence and route names above as implementation inputs; the draft's
host/build results do not establish native hardware operation.

CPU states 1/2, secondary MMU/dispatch and shared-memory coherence have live
evidence; higher states and sleep/hotplug do not. Reuse existing upstream SMP
fixes rather than duplicating them. MTP initialization, keyboard, touchpad and
haptic click paths have their ACK/readiness contracts in the bring-up document.
SEP communication and T6040 SuperSpeed CDC remain unqualified. No functional
driver recipe for either follows from a nearby SoC or a successful register read.

For SEP, distinguish attachment to already-booted firmware from ROM bootstrap.
The captured native source and J700's warm-registration reference use a retained
shared-memory descriptor on endpoint `0xfe`; discovery arrives asynchronously
on `0xfd`. The descriptor's address and size use 4 KiB units, independently of
the channel table's selected 16 KiB granule. The source-bound non-debug profile
with no standalone TXMB has ordinary entries PINC, FLAG, OPLA, IPIS and SCOT;
its backing allocation is twice the logical registered extent. This profile
planner does not qualify the current mapper, cache publication or a live send.
Do not substitute an AP physical address for the mapper-generated DVA or drop
channels merely because a different firmware worked with a smaller table.

The channel directory uses little-endian `<IIQ>` records: native numeric tag,
rounded channel size and 64-bit offset. The planner's `PINC` label maps to
`0x504e4943`, yielding wire bytes `CINP`; display labels must not be encoded
as tags. The terminator is `(0x6e756c6c, 0, 0)`. A source-only serializer now
passes golden-byte and native-data binding tests. It produces the directory
page only; channel payloads, allocation, mapper ownership and registration
remain unqualified.

Recent upstream IRC observations on macOS 26 also describe an already-running
SEP with initial `xars`/`xarm` endpoints and unavailable ROM access. Initializing
`xarm` is a separate persistent-state exchange, not a discovery query. Preserve
the existing xART data and boot identity; do not initialize that exchange with
fabricated storage or acknowledge an unimplemented write. These references
explain why retrying the unchanged ROM request is not the next bring-up step;
they do not establish J616s endpoint or service support.

## Protected GAPF status bank

A separate experimental 26A428 RAM image qualified one read from the original
`/chosen/dart-gapf-sr-region` resource on J616s. The ADT declares a 16 KiB
CARV page at `0x105ddc08000`; it lies outside normal managed RAM and the
observed region-id2/4 exclusions. The physical identity mapping is normally
cached. Do not treat an arbitrary physical read or a generic low-MMIO alias
as equivalent to the tested access.

The tested image excluded this page from cached/NC ordinary mappings, then
verified registered parent page tables, both translation regimes, Device-nGnRE
attributes and a read-only leaf. Actual read translation succeeded and write
translation faulted. One guarded 32-bit read of slice4 at page offset `0x10`
returned zero without CPU exceptions; natural watchdog recovery returned the
installed 480 Mb/s proxy. No guest or status-bank write was performed. This
qualifies narrow read accessibility, not write permission or native POWERUP.

The native constructor publishes initial value2 to this cell without checking
its previous value. The native GAPF descriptor prerequisites also matched a
separate read-only capture:64 slices, descriptor words1/0, selected control2,
and a32-bit selected range width3. A subsequent RAM-only constructor probe published initial value2 to the
original cell once. Guarded readback returned2 with no recorded exceptions;
the temporary Device write view was removed and its translation faulted,
while the original Device read-only view remained valid. Hardware GAPF
remained2, and natural watchdog recovery returned the paired cold proxy.
This qualifies the bounded initial0-to-2 publication, not a complete native
constructor, POWERUP's later value12 update or external consumers. The
experimental access helper and SPTM emulator are not part of this production
PR; they are not a driver initialization recipe.

A further bounded RAM probe initialized an empty 42-bit primary stage-2
tree and mapped the original page with Device-nGnRE, read-only and
execute-never attributes. The raw page descriptor was `0x600105ddc08747`;
its original PA and 16 KiB extent matched. No guest was entered, and cold
watchdog recovery passed. This qualifies the stage-2 descriptor, not guest
stage-1 cache attributes, combined translation or native POWERUP. The
experimental loader reservation and remap guard remain outside this PR.

## EL2 mapping publication during guest execution

On J616s with VHE enabled and TGE clear, EL1 TLB invalidation operates on
guest translations. Host EL2 page-table publication must invalidate EL2
translations explicitly. The T6040 path now uses `ALLE2IS` in this context.
The ordinary host context and other SoCs retain their existing operation.

A bounded native-context probe qualified metadata and owned tables, opened
one temporary Device write view of the original status page, verified
read/write translations, then removed the view and verified resealing.
Both PAR results were `0x04000105ddc08b00`, with zero recorded exceptions.
No status-cell or GAPF clock store was performed. Natural hardware-watchdog
recovery returned the cold proxy. This qualifies mapping publication and
removal; native POWERUP completion remains unqualified.
