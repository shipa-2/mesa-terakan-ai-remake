# R700 compute and the CF index question

Audit: 2026-09-05. This is documentation evidence, not a hardware result.

## Evergreen index selection

AMD's [Evergreen ISA revision 1.1a](https://docs.amd.com/api/khub/documents/lMd~Rb1b_AOs0nYgx3YxHA/content),
printed pages 9-219 and 9-220 (PDF pages 329 and 330), specifies that SET_CF_IDX0/1 obtains
the sequencer AR value from the first active pixel and clamps it to 0..255. This does not
mean unconditional lane zero.

Those descriptions prohibit combining the operation with waterfalling, MOVA_INT or LDS,
and prohibit setting both index registers in one VLIW instruction. They do not explicitly
ban ALU_PUSH_BEFORE or execution inside a divergent region. The first prohibition does
not itself specify whether its scope is an instruction group or an entire clause; do
not turn it into a proven clause-level scheduling rule.

Consequently, the reported MOVA_INT / SET_CF_IDX / PRED_SETE_INT sequence needs its actual
VLIW grouping and active-mask transitions inspected. These pages answer the lane-selection
question but do not prove why the measured divergent case retains the preceding index.
No change to split_address_loads or the RAT scheduler follows from this audit alone.

## R700 is not an Evergreen compute register variant

AMD's [R700 ISA revision 1.0a](https://docs.amd.com/api/khub/documents/2ZrqL_eSnIV39R0_tg_OwQ/content),
sections 3.4.2 and 9.1 (printed pages 3-9 and 9-25), describes MEM_EXPORT scatter access
to a shared linear buffer, with per-thread addresses and restrictions on cross-thread
visibility. This is not an Evergreen RAT descriptor interface. Searching this document
for SET_CF_IDX produced no match; that absence alone is not proof of an opcode boundary.

The independent local encoding evidence is `src/gallium/drivers/r600/r600_isa.c`:
SET_CF_IDX0/1 have no R600/R700 encoding, MEM_RAT has none either, while MEM_EXPORT has
R700 opcode 0x3a. `r600_pipe.c` exposes compute only above R700 and initializes the
Evergreen compute atom only in the later-generation branch. Therefore classic Gallium
does not supply the requested ready-made R700 dispatch replacement.

The remaining implementation gate is a documented R700 work-launch and memory-export
path, including bounds, synchronization and readback. Reusing Evergreen DISPATCH_DIRECT,
RAT writes or LS state is not justified by successful CB/TC copies. Keep submit guarded;
do not claim a Vulkan dispatch by substituting an unrelated graphics copy test.
