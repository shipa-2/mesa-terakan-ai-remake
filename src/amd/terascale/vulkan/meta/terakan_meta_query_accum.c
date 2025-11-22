/*
 * Copyright © 2025 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_meta.h"

#include "terakan_bo.h"
#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_draw.h"

#include "util/macros.h"
#include "util/u_endian.h"

#include <assert.h>

/* For testing, see `TERAKAN_DEVTEST_SPLIT_INDIRECT_BUFFER_AT_QUERY_BEGIN_END` and
 * `devtest_split_indirect_buffer_after_actions`.
 */

#define TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_ACCUMULATOR (TERAKAN_KCACHE_HW_BUFFERS_PER_STAGE - 1)
#define TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_BEGIN                                            \
   (TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_ACCUMULATOR - 1)
#define TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_END                                              \
   (TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_BEGIN - 1)

/* Indirect buffer end sample - indirect buffer beginning sample + accumulator.
 *
 * Subtraction upper = end upper - beginning upper - subb(end lower, beginning lower)
 * Subtraction lower = end lower - beginning lower
 * Result upper = subtraction upper + accumulator upper + addc(subtraction lower, accumulator lower)
 * Result lower = subtraction lower + accumulator lower
 *
 * Note that R8xx/R9xx constant cache restriction are pretty limiting (see the section 4.7.5
 * "Constant Register Read Port Restrictions" in the ISA references - ignore the example in the R8xx
 * documentation since it's outdated and applies to R6xx/R7xx which was less restrictive in this
 * area, the example was removed from the R9xx documentation). An instruction group may read
 * constants from up to 2 distinct XY or ZW pairs. Testing on Barts confirmed the presence of this
 * restriction - with an instruction pair referencing KC0[n].X, KC0[n].Z, KC1[n].X, KC1[n].Z (from 4
 * distinct XY or ZW pairs of kcache scalars rather than up to 2), the results for odd counters were
 * incorrect.
 *
 * 64-bit subtraction or addition is 3 independent operations:
 * - lower half (`LO` in the macro names here),
 * - borrow or carry (`B`/`C`),
 * - upper half without borrowing or carrying (`HI32`);
 * and one operation that depends on the result of the previous ones:
 * - upper half (`HIWB`/`HIWC`).
 * Therefore, one VLIW4 instruction group usually needs to perform 3 operations for the new counter,
 * and 1 upper part calculation for the previous counter.
 *
 * For Z pass queries, the most significant bit of a counter also indicates whether it's an actual
 * sample from an enabled render backend. Render backends that are not enabled via the
 * GB_BACKEND_MAP register don't write their Z pass counter sample values. This bit may be lost if
 * the hardware counter has overflown in the middle of the query.
 * For example: 0x8000000000000000 - 0x8000000000000001 + 0x8000000000000000 = 0x7FFFFFFFFFFFFFFF
 * The bit needs to be copied from a sample corresponding to that render backend, and that can be
 * done using the BFI instruction. In the macros here, this operation is named `FLAG`.
 *
 * Scheduling patterns used (square brackets denote operations that write UAV store operands, and
 * thus can't be moved to other vector lanes):
 *
 * VLIW4 Z pass (all counters in XY, need to restore the upper bit) - 2 subtractions, 2 additions:
 * | Sub 0 Lo | Sub 0 Hi32 | Sub 0 B |            | (2 kcache pairs per counter for sub Lo/Hi32/B)
 * | Sub 1 Lo | Sub 1 Hi32 | Sub 1 B | Sub 0 HiWB |
 * |[Add 0 Lo]| Add 0 Hi32 | Add 0 C | Sub 1 HiWB | (1 kcache pair per counter for add Lo/Hi32/C)
 * |[Add 1 Lo]| Add 1 Hi32 | Add 1 C | Add 0 HiWC |
 * |          |  [Flag 0]  |         | Add 1 HiWC | (1 kcache pair per counter for the flag bit)
 * |          |  [Flag 1]  |         |            |
 * | Sub 2 Lo | Sub 2 Hi32 | Sub 2 B |            |
 * ...
 *
 * VLIW4 statistics (even counters in XY, odd counters in ZW) - 2 subtractions, 2 additions:
 * | Sub 0 Lo | Sub 0 Hi32 | Sub 0 B  |            |
 * | Sub 1 Lo | Sub 1 Hi32 | Sub 1 B  | Sub 0 HiWB |
 * |[Add 0 Lo]| Add 0 Hi32 | Add 0 C  | Sub 1 HiWB |
 * | Add 1 C  |[Add 0 HiWC]|[Add 1 Lo]| Add 1 Hi32 |
 * | Sub 2 Lo | Sub 2 Hi32 | Sub 2 B  |[Add 1 HiWC]|
 * ...
 *
 * VLIW5 Z pass - 2 subtractions, 2 additions coissued with restoring the upper bit for the previous
 * 2 additions:
 * | Sub 0 Lo | Sub 0 Hi32 | Sub 0 B |            |            |
 * | Sub 1 Lo | Sub 1 Hi32 | Sub 1 B | Sub 0 HiWB |            |
 * |[Add 0 Lo]| Add 0 Hi32 | Add 0 C | Sub 1 HiWB |            |
 * |[Add 1 Lo]| Add 1 Hi32 | Add 1 C | Add 0 HiWC |            |
 * | Sub 2 Lo | Sub 2 Hi32 | Sub 2 B | Add 1 HiWC |            |
 * | Sub 3 Lo | Sub 3 Hi32 | Sub 3 B | Sub 2 HiWB |            |
 * |[Add 2 Lo]|  [Flag 0]  | Add 2 C | Sub 3 HiWB | Add 2 Hi32 |
 * |[Add 3 Lo]|  [Flag 1]  | Add 3 C | Add 2 HiWC | Add 3 Hi32 |
 * ...
 * | Sub 6 Lo | Sub 6 Hi32 | Sub 6 B | Add 5 HiWC |            |
 * | Sub 7 Lo | Sub 7 Hi32 | Sub 7 B | Sub 6 HiWB |            |
 * |[Add 6 Lo]|  [Flag 4]  | Add 6 C | Sub 7 HiWB | Add 6 Hi32 |
 * |[Add 7 Lo]|  [Flag 5]  | Add 7 C | Add 6 HiWC | Add 7 Hi32 |
 * |          |  [Flag 6]  |         | Add 7 HiWC |            |
 * |          |  [Flag 7]  |         |            |            |
 *
 * VLIW5 pipeline statistics - 4 subtractions in 4 groups, 4 additions in 3 groups:
 * | Sub  0 Lo | Sub  0 Hi32 | Sub  0 B  |             |             |
 * | Sub  1 Lo | Sub  1 Hi32 | Sub  1 B  | Sub  0 HiWB |             |
 * | Sub  2 Lo | Sub  2 Hi32 | Sub  2 B  | Sub  1 HiWB |             |
 * | Sub  3 Lo | Sub  3 Hi32 | Sub  3 B  | Sub  2 HiWB |             |
 * | Add  1 C  | Sub  3 HiWB |[Add  1 Lo]| Add  1 Hi32 | Add  0 Lo   |
 * |[Add  2 Lo]| Add  2 Hi32 | Add  2 C  |[Add  1 HiWC]| Add  0 Hi32 |
 * | Add  3 C  |[Add  2 HiWC]|[Add  3 Lo]| Add  3 Hi32 | Add  0 C    |
 * | Sub  4 Lo | Sub  4 Hi32 | Sub  4 B  |[Add  3 HiWC]| Add  0 HiWC |
 * ...
 * | Sub  8 Lo | Sub  8 Hi32 | Sub  8 B  |[Add  7 HiWC]| Add  4 HiWC |
 * | Sub  9 Lo | Sub  9 Hi32 | Sub  9 B  | Sub  8 HiWB |             |
 * | Sub 10 Lo | Sub 10 Hi32 | Sub 10 B  | Sub  9 HiWB |             |
 * | Add  9 C  | Sub 10 HiWB |[Add  9 Lo]| Add  9 Hi32 | Add  8 Hi32 |
 * |[Add 10 Lo]| Add 10 Hi32 | Add 10 C  |[Add  9 HiWC]| Add  8 C    |
 * |[Add  8 Lo]|[Add 10 HiWC]|           | [Offset 10] | Add  8 HiWC |
 */

/* Constant and UAV indexing for Z pass queries. Render backend counters have a stride of 2 qwords,
 * beginning and end samples for each render backend may or may not share the buffer with them being
 * interleaved, hence XY for the beginning and ZW for the end sample. The accumulator for a render
 * backend is in the lower qword (XY).
 */
#define TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_RB(counter)        (counter)
#define TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter)     'X'
#define TERAKAN_META_QUERY_ACCUM_COUNTER_END_SEL_LO_RB(counter) 'Z'

/* Constant and UAV indexing for statistics queries. Counters have a stride of 1 qword, beginning
 * and end sample structures are always stored separately.
 */
#define TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_STAT(counter)    ((counter) / 2)
#define TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ((counter) % 2 * 2)
#define TERAKAN_META_QUERY_ACCUM_COUNTER_END_SEL_LO_STAT(counter)                                  \
   TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter)

/* Using clause-temporary GPRs to avoid latency. Only up to 4 counters at once can have work started
 * and not completed for them, because there are only 4 temporary GPR vectors.
 */
#define TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter) (0x7F - (counter) % 4)

/* GPRs are used in these instructions only as the first operand, and when working with only one
 * counter on the vector units, there should be no read port conflicts, so always using VEC_012 and
 * SCL_210 (allowing coissuing work for another counter on the scalar unit without hitting the
 * scalar unit cycle restrictions for constants) bank swizzles, which both have the same encoding
 * (0).
 */

/* Individual instructions. */

/* Sub Lo - subtract the lower 32 bits.
 * R[counter_temp].X = SUB_INT KC[end][counter_vec][lo], KC[begin][counter_vec][lo]
 */
#define TERAKAN_META_QUERY_ACCUM_SUBLO(close, type, counter)                                       \
   TERAKAN_SHADER_OP2(close, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter), 'X', SUB_INT, EG, \
                      0x80 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),                 \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_END_SEL_LO_##type(counter),                 \
                      0xA0 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),                 \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter), VEC_012)

/* Sub Hi32 - subtract the upper 32 bits, without borrowing.
 * PV.Y = SUB_INT KC[end][counter_vec][hi], KC[begin][counter_vec][hi]
 */
#define TERAKAN_META_QUERY_ACCUM_SUBHI32(close, type, counter)                                     \
   TERAKAN_SHADER_OP2_NW(close, 'Y', SUB_INT, EG,                                                  \
                         0x80 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),              \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_END_SEL_LO_##type(counter) ^ 1,          \
                         0xA0 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),              \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 1, VEC_012)

/* Sub B - get the borrow.
 * PV.Z = SUBB_UINT KC[end][counter_vec][lo], KC[begin][counter_vec][lo]
 */
#define TERAKAN_META_QUERY_ACCUM_SUBB(close, type, counter)                                        \
   TERAKAN_SHADER_OP2_NW(close, 'Z', SUBB_UINT, EG,                                                \
                         0x80 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),              \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_END_SEL_LO_##type(counter),              \
                         0xA0 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),              \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter), VEC_012)

/* Sub HiWB - apply the borrow to the result of the upper 32 bits subtraction.
 * R[counter_temp].dst_chan = SUB_INT PV.Y, PV.Z
 */
#define TERAKAN_META_QUERY_ACCUM_SUBHIWB(close, counter, dst_chan)                                 \
   TERAKAN_SHADER_OP2(close, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter), dst_chan,         \
                      SUB_INT, EG, V_SQ_ALU_SRC_PV, 'Y', V_SQ_ALU_SRC_PV, 'Z', VEC_012)

/* Add Lo - add the lower 32 bits.
 * R[1 + counter_vec][lo] = ADD_INT R[counter_temp].X, KC[accum][counter_vec][lo]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDLO(close, type, counter)                                       \
   TERAKAN_SHADER_OP2(close, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),             \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter), ADD_INT, EG,        \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter), 'X',                     \
                      0x100 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),                \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter), VEC_012)

/* Add Hi32 - add the upper 32 bits, without borrowing.
 * Not writing (for the vector unit counter).
 * PV[hi] = ADD_INT R[counter_temp].src_hiwb_chan, KC[accum][counter_vec][hi]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDHI32_NW(close, type, counter, src_hiwb_chan)                   \
   TERAKAN_SHADER_OP2_NW(close, TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 1,       \
                         ADD_INT, EG, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter),          \
                         src_hiwb_chan,                                                            \
                         0x100 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),             \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 1, VEC_012)

/* Add Hi32 - add the upper 32 bits, without borrowing.
 * Overwriting either Y or W of the temporary register (for the scalar unit counter).
 * R[counter_temp][hi] = ADD_INT R[counter_temp].src_hiwb_chan, KC[accum][counter_vec][hi]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDHI32_TO_TEMP(type, counter, src_hiwb_chan)                     \
   TERAKAN_SHADER_OP2(true, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter),                    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 1, ADD_INT, EG,    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter), src_hiwb_chan,           \
                      0x100 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),                \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 1, SCL_210)

/* Add C - get the carry.
 * PV/PS[lo ^ 2] = ADDC_UINT R[counter_temp].X, KC[accum][counter_vec][lo]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDC(close, type, counter)                                        \
   TERAKAN_SHADER_OP2_NW(close, TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter) ^ 2,       \
                         ADD_INT, EG, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter), 'X',     \
                         0x100 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type(counter),             \
                         TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_##type(counter), VEC_012)

/* Add HiWC - apply the carry to the result of the upper 32 bits addition.
 * Writing to the temporary register (for Z pass queries, which need upper bit restoring).
 * Hi32 from PV or PS, carry from PV (for a vector unit counter, except when Flag and Hi32 are
 * coissued because Flag is vector-only).
 * R[counter_temp][hi] = ADD_INT PV/PS[hi], PV[lo ^ 2]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_RB(close, counter, hi32_prev)                         \
   TERAKAN_SHADER_OP2(close, TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter),                   \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 1, ADD_INT, EG,        \
                      V_SQ_ALU_SRC_##hi32_prev,                                                    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 1, V_SQ_ALU_SRC_PV,    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 2, VEC_012)

/* Add HiWC - apply the carry to the result of the upper 32 bits addition.
 * Writing to the result register (for statistics queries, which don't need upper bit restoring).
 * Hi32 from PV or PS (normally PV for statistics), carry from PV (for a vector unit counter).
 * R[1 + counter][hi] = ADD_INT PV/PS[hi], PV[lo ^ 2]
 */
#define TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(close, counter, hi32_prev)                       \
   TERAKAN_SHADER_OP2(close, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_STAT(counter),               \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 1, ADD_INT, EG,      \
                      V_SQ_ALU_SRC_##hi32_prev,                                                    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 1, V_SQ_ALU_SRC_PV,  \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 2, VEC_012)

/* Add HiWC - apply the carry to the result of the upper 32 bits addition.
 * Writing to the result register (for statistics queries, which don't need upper bit restoring).
 * Hi32 from the temporary register, carry from PS (for a scalar unit counter).
 * R[1 + counter][hi] = ADD_INT R[counter_temp][hi], PS
 */
#define TERAKAN_META_QUERY_ACCUM_ADDHIWC_SCL_STAT(counter)                                         \
   TERAKAN_SHADER_OP2(true, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_STAT(counter),                \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 1, ADD_INT, EG,      \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter),                          \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 1, V_SQ_ALU_SRC_PS,  \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_STAT(counter) ^ 2, SCL_210)

/* Flag - copy the upper bit to the final result from a render backend sample.
 * BFI_INT is vector-only.
 * R[1 + counter][hi] = BFI_INT L.literal_chan, R[counter_temp][hi], KC[accum][counter_vec][hi]
 */
#define TERAKAN_META_QUERY_ACCUM_FLAG_RB(close, counter, literal_chan)                             \
   TERAKAN_SHADER_OP3(close, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_RB(counter),                 \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 1, BFI_INT, EG,        \
                      V_SQ_ALU_SRC_LITERAL, literal_chan,                                          \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_TEMP_GPR(counter),                          \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 1,                     \
                      0x100 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_RB(counter),                    \
                      TERAKAN_META_QUERY_ACCUM_COUNTER_SEL_LO_RB(counter) ^ 1, VEC_012)
#define TERAKAN_META_QUERY_ACCUM_FLAG_LITERAL (((uint32_t)1 << 31) - 1)

/* clang-format off */

/* Instruction groups. */

#define TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(close, type, counter)                    \
   TERAKAN_META_QUERY_ACCUM_SUBLO(false, type, counter),                                           \
   TERAKAN_META_QUERY_ACCUM_SUBHI32(false, type, counter),                                         \
   TERAKAN_META_QUERY_ACCUM_SUBB(close, type, counter)

#define TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(close, type, counter,          \
                                                                    sub_hiwb_counter)              \
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(false, type, counter),                        \
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(close, sub_hiwb_counter, 'W')

/* If used for a statistics query, `add_hiwc_counter` must be odd. */
#define TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(close, type, counter,          \
                                                                    add_hiwc_counter,              \
                                                                    add_hiwc_hi32_prev)            \
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(false, type, counter),                        \
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_##type(close, add_hiwc_counter, add_hiwc_hi32_prev)

/* If used for a statistics query, `counter` must be even. */
#define TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC(close, type, counter, src_hiwb_chan)     \
   TERAKAN_META_QUERY_ACCUM_ADDLO(false, type, counter),                                           \
   TERAKAN_META_QUERY_ACCUM_ADDHI32_NW(false, type, counter, src_hiwb_chan),                       \
   TERAKAN_META_QUERY_ACCUM_ADDC(close, type, counter)

/* If used for a statistics query, `counter` must be even. */
#define TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(close, type, counter,          \
                                                                    src_hiwb_chan,                 \
                                                                    sub_hiwb_counter)              \
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC(false, type, counter, src_hiwb_chan),         \
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(close, sub_hiwb_counter, 'W')

/* If used for a statistics query, `counter` must be even, and `add_hiwc_counter` must be odd. */
#define TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(close, type, counter,          \
                                                                    src_hiwb_chan,                 \
                                                                    add_hiwc_counter,              \
                                                                    add_hiwc_hi32_prev)            \
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC(false, type, counter, src_hiwb_chan),         \
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_##type(close, add_hiwc_counter, add_hiwc_hi32_prev)

/* For Z pass queries, addition coissued with vector-only Flag, writing Hi32 to PS rather than PV.
 */

#define TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_SUBHIWB_T_ADDHI32_RB_6_QWORDS(            \
   counter, src_hiwb_chan, flag_counter, sub_hiwb_counter)                                         \
   TERAKAN_META_QUERY_ACCUM_ADDLO(false, RB, counter),                                             \
   TERAKAN_META_QUERY_ACCUM_FLAG_RB(false, flag_counter, 'X'),                                     \
   TERAKAN_META_QUERY_ACCUM_ADDC(false, RB, counter),                                              \
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(false, sub_hiwb_counter, 'W'),                                 \
   TERAKAN_META_QUERY_ACCUM_SUBHI32(true, RB, counter),                                            \
   TERAKAN_META_QUERY_ACCUM_FLAG_LITERAL,                                                          \
   0

#define TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_ADDHIWC_T_ADDHI32_RB_6_QWORDS(            \
   counter, src_hiwb_chan, flag_counter, add_hiwc_counter, add_hiwc_hi32_prev)                     \
   TERAKAN_META_QUERY_ACCUM_ADDLO(false, RB, counter),                                             \
   TERAKAN_META_QUERY_ACCUM_FLAG_RB(false, flag_counter, 'X'),                                     \
   TERAKAN_META_QUERY_ACCUM_ADDC(false, RB, counter),                                              \
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_RB(false, add_hiwc_counter, add_hiwc_hi32_prev),           \
   TERAKAN_META_QUERY_ACCUM_SUBHI32(true, RB, counter),                                            \
   TERAKAN_META_QUERY_ACCUM_FLAG_LITERAL,                                                          \
   0

/* For statistics queries only. `counter` must be odd. */
#define TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_SUBHIWB_Z_ADDLO_W_ADDHI32_STAT(close, counter,           \
                                                                         src_hiwb_chan,            \
                                                                         sub_hiwb_counter)         \
   TERAKAN_META_QUERY_ACCUM_ADDC(false, STAT, counter),                                            \
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(false, sub_hiwb_counter, 'Y'),                                 \
   TERAKAN_META_QUERY_ACCUM_ADDLO(false, STAT, counter),                                           \
   TERAKAN_META_QUERY_ACCUM_ADDHI32_NW(close, STAT, counter, src_hiwb_chan)

/* For statistics queries only. `counter` must be odd, and `add_hiwc_counter` must be even. */
#define TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(close, counter,           \
                                                                         src_hiwb_chan,            \
                                                                         add_hiwc_counter)         \
   TERAKAN_META_QUERY_ACCUM_ADDC(false, STAT, counter),                                            \
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(false, add_hiwc_counter, PV),                         \
   TERAKAN_META_QUERY_ACCUM_ADDLO(false, STAT, counter),                                           \
   TERAKAN_META_QUERY_ACCUM_ADDHI32_NW(close, STAT, counter, src_hiwb_chan)

#define TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(counter, add_hiwc_counter,           \
                                                              add_hiwc_hi32_prev)                  \
   TERAKAN_META_QUERY_ACCUM_FLAG_RB(false, counter, 'X'),                                          \
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_RB(true, add_hiwc_counter, add_hiwc_hi32_prev),            \
   TERAKAN_META_QUERY_ACCUM_FLAG_LITERAL,                                                          \
   0

#define TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(counter)                                       \
   TERAKAN_META_QUERY_ACCUM_FLAG_RB(true, counter, 'X'),                                           \
   TERAKAN_META_QUERY_ACCUM_FLAG_LITERAL,                                                          \
   0

/* Control flow instructions. */

#define TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(address, qword_count)                             \
   S_SQ_CF_ALU_WORD0_EXT_KCACHE_BANK2(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_ACCUMULATOR) |        \
      S_SQ_CF_ALU_WORD0_EXT_KCACHE_MODE2(V_SQ_CF_KCACHE_LOCK_1),                                   \
   EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_EXTENDED,                                                       \
   S_SQ_CF_WORD0_ADDR(address) |                                                                   \
      S_SQ_CF_ALU_WORD0_KCACHE_BANK0(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_END) |              \
      S_SQ_CF_ALU_WORD0_KCACHE_BANK1(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_BEGIN) |            \
      S_SQ_CF_ALU_WORD0_KCACHE_MODE0(V_SQ_CF_KCACHE_LOCK_1),                                       \
   S_SQ_CF_ALU_WORD1_KCACHE_MODE1(V_SQ_CF_KCACHE_LOCK_1) |                                         \
      S_SQ_CF_ALU_WORD1_COUNT((qword_count) - 1) | EG_V_SQ_CF_ALU_WORD1_SQ_CF_INST_ALU

#define TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(component_mask, vec4_count)                         \
   TERAKAN_SHADER_CF_UAV(true, STORE_RAW, 0, 0, 1, component_mask, true) |                         \
      S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_ARRAY_SIZE(sizeof(uint32_t) * 4) |                            \
      S_SQ_CF_ALLOC_EXPORT_WORD1_BURST_COUNT((vec4_count) - 1)

#define TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(component_mask, vec4_count)                         \
   TERAKAN_SHADER_CF_UAV(true, STORE_DWORD, 0, 0, 1, component_mask, true) |                       \
      S_SQ_CF_ALLOC_EXPORT_WORD1_BUF_ARRAY_SIZE(4) |                                               \
      S_SQ_CF_ALLOC_EXPORT_WORD1_BURST_COUNT((vec4_count) - 1)

/* clang-format on */

#define TERAKAN_META_QUERY_ACCUM_SHADER(name, type, counter_count)                                 \
   struct terakan_meta_shader const terakan_meta_query_accum_##name##_vs = {                       \
      .r8xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_query_accum_##name##_vs_r8xx,                                  \
            .program_size_bytes = sizeof(terakan_meta_query_accum_##name##_vs_r8xx),               \
            .static_registers =                                                                    \
               {                                                                                   \
                  .sq_pgm_resources =                                                              \
                     {                                                                             \
                        S_028860_NUM_GPRS(                                                         \
                           TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type((counter_count) - 1) + 2) | \
                           TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                                   \
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                    \
                     },                                                                            \
               },                                                                                  \
         },                                                                                        \
      .r9xx =                                                                                      \
         {                                                                                         \
            .program = terakan_meta_query_accum_##name##_vs_r9xx,                                  \
            .program_size_bytes = sizeof(terakan_meta_query_accum_##name##_vs_r9xx),               \
            .static_registers =                                                                    \
               {                                                                                   \
                  .sq_pgm_resources =                                                              \
                     {                                                                             \
                        S_028860_NUM_GPRS(                                                         \
                           TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_##type((counter_count) - 1) + 2) | \
                           TERAKAN_META_SQ_PGM_RESOURCES_COMMON,                                   \
                        TERAKAN_META_SQ_PGM_RESOURCES_2_COMMON,                                    \
                     },                                                                            \
               },                                                                                  \
         },                                                                                        \
      .kcache_needed = BITFIELD_BIT(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_END) |               \
                       BITFIELD_BIT(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_BEGIN) |             \
                       BITFIELD_BIT(TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_ACCUMULATOR),           \
   };

static uint32_t const terakan_meta_query_accum_zpass_1_rb_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(4, 10),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b11, 1),

   /* 3: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 4: ALU clause. */

   /* Subtraction. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(true, 0, 'W'),
   /* Addition. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC(true, RB, 0, 'W'),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_RB(true, 0, PV),
   /* Flag bit. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(0),
};

static uint32_t const terakan_meta_query_accum_zpass_1_rb_vs_r9xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(5, 10),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(0b11, 1),

   /* 3-4: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* Subtraction. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_SUBHIWB(true, 0, 'W'),
   /* Addition. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC(true, RB, 0, 'W'),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_RB(true, 0, PV),
   /* Flag bit. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(0),
};

TERAKAN_META_QUERY_ACCUM_SHADER(zpass_1_rb, RB, 1)

static uint32_t const terakan_meta_query_accum_zpass_2_rb_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(4, 10 * 2),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b11, 2),

   /* 3: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 4: ALU clause. */

   /* 0...1 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 1, 0),
   /* 0...1 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 0, 'W', 1),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 1, 'W', 0, PV),
   /* 0...1 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(0, 1, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(1),
};

static uint32_t const terakan_meta_query_accum_zpass_2_rb_vs_r9xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(5, 10 * 2),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(0b11, 2),

   /* 3-4: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* 0...1 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 1, 0),
   /* 0...1 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 0, 'W', 1),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 1, 'W', 0, PV),
   /* 0...1 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(0, 1, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(1),
};

TERAKAN_META_QUERY_ACCUM_SHADER(zpass_2_rb, RB, 2)

static uint32_t const terakan_meta_query_accum_zpass_4_rb_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(4, 10 * 4),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b11, 4),

   /* 3: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 4: ALU clause. */

   /* 0...1 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 1, 0),
   /* 0...1 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 0, 'W', 1),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 1, 'W', 0, PV),
   /* 2...3 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, RB, 2, 1, PV),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 3, 2),
   /* 2...3 additions, 0...1 flag bits. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_SUBHIWB_T_ADDHI32_RB_6_QWORDS(2, 'W', 0, 3),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_ADDHIWC_T_ADDHI32_RB_6_QWORDS(3, 'W', 1, 2, PS),
   /* 2...3 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(2, 3, PS),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(3),
};

/* No R9xx chips with a maximum of 4 render backends, provide an aligned dummy. */
static uint32_t const terakan_meta_query_accum_zpass_4_rb_vs_r9xx[] = {0, 0};

TERAKAN_META_QUERY_ACCUM_SHADER(zpass_4_rb, RB, 4)

static uint32_t const terakan_meta_query_accum_zpass_8_rb_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(4, 10 * 8),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b11, 8),

   /* 3: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 4: ALU clause. */

   /* 0...1 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 1, 0),
   /* 0...1 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 0, 'W', 1),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 1, 'W', 0, PV),
   /* 2...3 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, RB, 2, 1, PV),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 3, 2),
   /* 2...3 additions, 0...1 flag bits. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_SUBHIWB_T_ADDHI32_RB_6_QWORDS(2, 'W', 0, 3),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_ADDHIWC_T_ADDHI32_RB_6_QWORDS(3, 'W', 1, 2, PS),
   /* 4...5 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, RB, 4, 3, PS),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 5, 4),
   /* 4...5 additions, 2...3 flag bits. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_SUBHIWB_T_ADDHI32_RB_6_QWORDS(4, 'W', 2, 5),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_ADDHIWC_T_ADDHI32_RB_6_QWORDS(5, 'W', 3, 4, PS),
   /* 6...7 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, RB, 6, 5, PS),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 7, 6),
   /* 6...7 additions, 4...5 flag bits. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_SUBHIWB_T_ADDHI32_RB_6_QWORDS(6, 'W', 4, 7),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_FLAG_Z_ADDC_W_ADDHIWC_T_ADDHI32_RB_6_QWORDS(7, 'W', 5, 6, PS),
   /* 6...7 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(6, 7, PS),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(7),
};

static uint32_t const terakan_meta_query_accum_zpass_8_rb_vs_r9xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(5, 10 * 8),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(0b11, 8),

   /* 3-4: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* 0...1 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 0),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 1, 0),
   /* 0...1 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 0, 'W', 1),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 1, 'W', 0, PV),
   /* 0...1 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(0, 1, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(1),
   /* 2...3 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 2),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 3, 2),
   /* 2...3 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 2, 'W', 3),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 3, 'W', 2, PV),
   /* 2...3 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(2, 3, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(3),
   /* 4...5 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 4),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 5, 4),
   /* 4...5 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 4, 'W', 5),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 5, 'W', 4, PV),
   /* 4...5 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(4, 5, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(5),
   /* 6...7 subtractions. */
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, RB, 6),
   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, RB, 7, 6),
   /* 6...7 additions. */
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, RB, 6, 'W', 7),
   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, RB, 7, 'W', 6, PV),
   /* 6...7 flag bits. */
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_W_ADDHIWC_RB_3_QWORDS(6, 7, PV),
   TERAKAN_META_QUERY_ACCUM_Y_FLAG_RB_2_QWORDS(7),
};

TERAKAN_META_QUERY_ACCUM_SHADER(zpass_8_rb, RB, 8)

static uint32_t const terakan_meta_query_accum_pipelinestat_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(5, 8 * 11 + 2),

   /* 2-3: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b1111, 10 / 2),
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(false, 0, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_STAT(10),
                                        true, false),

   /* 4: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 5: ALU clause. */

   /* 0...3 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, STAT, 0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 1, 0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 2, 1),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 3, 2),

   /* 0...3 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_SUBHIWB_Z_ADDLO_W_ADDHI32_STAT(false, 1, 'W', 3),
   TERAKAN_META_QUERY_ACCUM_ADDLO(true, STAT, 0),

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(false, STAT, 2, 'W', 1, PV),
   TERAKAN_META_QUERY_ACCUM_ADDHI32_TO_TEMP(STAT, 0, 'W'),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(false, 3, 'Y', 2),
   TERAKAN_META_QUERY_ACCUM_ADDC(true, STAT, 0),

   /* 4...7 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(false, STAT, 4, 3, PV),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_SCL_STAT(0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 5, 4),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 6, 5),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 7, 6),

   /* 4...7 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_SUBHIWB_Z_ADDLO_W_ADDHI32_STAT(false, 5, 'W', 7),
   TERAKAN_META_QUERY_ACCUM_ADDLO(true, STAT, 4),

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(false, STAT, 6, 'W', 5, PV),
   TERAKAN_META_QUERY_ACCUM_ADDHI32_TO_TEMP(STAT, 4, 'W'),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(false, 7, 'Y', 5),
   TERAKAN_META_QUERY_ACCUM_ADDC(true, STAT, 4),

   /* 8...10 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(false, STAT, 8, 7, PV),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_SCL_STAT(4),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 9, 8),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 10, 9),

   /* 8...10 additions, 10 address. */

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_SUBHIWB_Z_ADDLO_W_ADDHI32_STAT(false, 9, 'W', 10),
   TERAKAN_META_QUERY_ACCUM_ADDHI32_TO_TEMP(STAT, 8, 'W'),

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(false, STAT, 10, 'Y', 9, PV),
   TERAKAN_META_QUERY_ACCUM_ADDC(true, STAT, 8),

   TERAKAN_META_QUERY_ACCUM_ADDLO(false, STAT, 8),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(false, 10, PV),
   TERAKAN_SHADER_OP2(false, 1 + 10 / 2, 'W', ADD_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                      VEC_012),
   TERAKAN_META_QUERY_ACCUM_ADDHIWC_SCL_STAT(8),
   2 * 10,
   0,
};

static uint32_t const terakan_meta_query_accum_pipelinestat_vs_r9xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(6, 8 * 11 + 2),

   /* 2-3: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(0b1111, 10 / 2),
   TERAKAN_SHADER_CF_UAV_COMBINED_STORE(true, 0, 1 + TERAKAN_META_QUERY_ACCUM_COUNTER_VEC_STAT(10),
                                        true, false),

   /* 4-5: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 6: ALU clause. */

   /* 0...1 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, STAT, 0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 1, 0),

   /* 0...1 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 0, 'W', 1),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 1, 'W', 0),

   /* 2...3 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, STAT, 2, 1, PV),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 3, 2),

   /* 2...3 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 2, 'W', 3),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 3, 'W', 2),

   /* 4...5 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, STAT, 4, 3, PV),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 5, 4),

   /* 4...5 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 4, 'W', 5),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 5, 'W', 4),

   /* 6...7 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, STAT, 6, 5, PV),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 7, 6),

   /* 6...7 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 6, 'W', 7),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 7, 'W', 6),

   /* 8...10 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_ADDHIWC(true, STAT, 8, 7, PV),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 9, 8),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 10, 9),

   /* 8...10 additions, 10 address. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 8, 'W', 9),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 9, 'W', 8),

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_ADDHIWC(true, STAT, 10, 'W', 9, PV),

   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(false, 10, PV),
   TERAKAN_SHADER_OP2(true, 1 + 10 / 2, 'W', ADD_INT, EG, 0, 'X', V_SQ_ALU_SRC_LITERAL, 'X',
                      VEC_012),
   2 * 10,
   0,
};

TERAKAN_META_QUERY_ACCUM_SHADER(pipelinestat, STAT, 11)

static uint32_t const terakan_meta_query_accum_streamoutstats_vs_r8xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(4, 8 * 2),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R8XX(0b1111, 2 / 2),

   /* 3: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_AND_END_R8XX,

   /* 4: ALU clause. */

   /* 0...1 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, STAT, 0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 1, 0),

   /* 0...1 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 0, 'W', 1),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 1, 'W', 0),

   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(true, 1, PV),
};

static uint32_t const terakan_meta_query_accum_streamoutstats_vs_r9xx[] = {
   /* 0-1: Accumulate. */
   TERAKAN_META_QUERY_ACCUM_CF_ALU_EXTENDED(5, 8 * 2),

   /* 2: Store. */
   TERAKAN_META_QUERY_ACCUM_CF_STORE_R9XX(0b1111, 2 / 2),

   /* 3-4: End. */
   TERAKAN_SHADER_CF_VS_DUMMY_EXPORT_DONE_R9XX,
   TERAKAN_SHADER_CF_END_R9XX,

   /* 5: ALU clause. */

   /* 0...1 subtractions. */

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB(true, STAT, 0),

   TERAKAN_META_QUERY_ACCUM_X_SUBLO_Y_SUBHI32_Z_SUBB_W_SUBHIWB(true, STAT, 1, 0),

   /* 0...1 additions. */

   TERAKAN_META_QUERY_ACCUM_X_ADDLO_Y_ADDHI32_Z_ADDC_W_SUBHIWB(true, STAT, 0, 'W', 1),

   TERAKAN_META_QUERY_ACCUM_X_ADDC_Y_ADDHIWC_Z_ADDLO_W_ADDHI32_STAT(true, 1, 'W', 0),

   TERAKAN_META_QUERY_ACCUM_ADDHIWC_VEC_STAT(true, 1, PV),
};

TERAKAN_META_QUERY_ACCUM_SHADER(streamoutstats, STAT, 2)

unsigned
terakan_meta_query_accum_begin(struct terakan_gfx_command_writer * const command_writer,
                               VkQueryType const query_type)
{
   enum terakan_meta_shader_index vs_index;
   unsigned dst_uav_dwords;
   switch (query_type) {
   case VK_QUERY_TYPE_OCCLUSION: {
      unsigned const max_render_backends_log2 =
         terakan_gfx_command_writer_physical_device(command_writer)
            ->chip_info.max_render_backends_log2;
      vs_index = TERAKAN_META_SHADER_QUERY_ACCUM_ZPASS_1_RB_VS + max_render_backends_log2;
      dst_uav_dwords = ((2 * 2) << max_render_backends_log2) - 2;
   } break;
   case VK_QUERY_TYPE_PIPELINE_STATISTICS: {
      vs_index = TERAKAN_META_SHADER_QUERY_ACCUM_PIPELINESTAT_VS;
      dst_uav_dwords = 2 * 11;
   } break;
   case VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT: {
      vs_index = TERAKAN_META_SHADER_QUERY_ACCUM_STREAMOUTSTATS_VS;
      dst_uav_dwords = 2 * 2;
   } break;
   default:
      assert(!"Unsupported query type");
      return 0;
   }

   terakan_meta_begin(command_writer, false);
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE,
                                        TERAKAN_HW_STATE_DRAW_INDEX_VGT_PRIMITIVE_TYPE,
                                        &command_writer->hw_state_draw.vgt_primitive_type,
                                        S_008958_PRIM_TYPE(V_008958_DI_PT_POINTLIST));
   terakan_hw_state_draw_set_vgt_num_instances(&command_writer->hw_state_draw, 1);
   terakan_meta_set_vs(command_writer, vs_index);
   terakan_meta_modify_state_draw_dword(
      command_writer, TERAKAN_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL,
      TERAKAN_HW_STATE_DRAW_INDEX_PA_CL_CLIP_CNTL, &command_writer->hw_state_draw.pa_cl_clip_cntl,
      S_028810_CLIP_DISABLE(1) | S_028810_DX_RASTERIZATION_KILL(1));
   terakan_meta_begin_cb(command_writer, 0xF, 0b0);

   command_writer->push_constants_state.up_to_date_push_constants_bound_to_stages &=
      ~VK_SHADER_STAGE_VERTEX_BIT;

   struct terakan_bo const * const accumulator =
      terakan_gfx_command_writer_device(command_writer)->query_accumulator_bo;
   terakan_hw_state_sqc_set_kcache_vs(
      &command_writer->hw_state_sqc, TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_ACCUMULATOR, 1,
      accumulator, (uint32_t)(accumulator->va >> TERAKAN_KCACHE_HW_LINE_BYTES_LOG2));

   return dst_uav_dwords;
}

void
terakan_meta_query_accum(
   struct terakan_gfx_command_writer * const command_writer,
   struct terakan_command_buffer_indirect_buffer_query_sample const * const ib_end_sample,
   struct terakan_command_buffer_indirect_buffer_query_sample const * const ib_begin_sample,
   struct terakan_bo const * const dst_uav_bo, uint64_t const dst_uav_va,
   unsigned const dst_uav_dwords)
{
   terakan_hw_state_sqc_set_kcache_vs(&command_writer->hw_state_sqc,
                                      TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_END, 1,
                                      ib_end_sample->bo, ib_end_sample->va_kcache_lines);
   terakan_hw_state_sqc_set_kcache_vs(&command_writer->hw_state_sqc,
                                      TERAKAN_META_QUERY_ACCUM_KCACHE_BUFFER_IB_BEGIN, 1,
                                      ib_begin_sample->bo, ib_begin_sample->va_kcache_lines);

   struct terakan_color_descriptor dst_uav = {
      .info = S_028C70_ENDIAN(UTIL_ARCH_BIG_ENDIAN ? TERASCALE_ENDIAN_SWAP_8IN32
                                                   : TERASCALE_ENDIAN_SWAP_NONE) |
              S_028C70_FORMAT(TERASCALE_FORMAT_INDEX_32) |
              S_028C70_NUMBER_TYPE(TERASCALE_FORMAT_NUMBER_TYPE_UINT) |
              TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_INFO_CONST_FIELDS,
      .attrib = TERAKAN_COLOR_DESCRIPTOR_BUFFER_UAV_ATTRIB,
   };
   uint32_t dst_uav_base_granularity_offset_bytes;
   terakan_color_descriptor_calculate_buffer_base_pitch_slice_dim_offset(
      &dst_uav, dst_uav_va, dst_uav_dwords, sizeof(uint32_t),
      terakan_gfx_command_writer_physical_device(command_writer),
      &dst_uav_base_granularity_offset_bytes);
   terakan_hw_state_draw_set_cb_color(&command_writer->hw_state_draw, 0, dst_uav_bo, &dst_uav, NULL,
                                      true);

   /* Make R0.X the index of the first counter within the aligned UAV. */
   terakan_meta_modify_state_draw_dword(command_writer, TERAKAN_STATE_DRAW_INDEX_VGT_INDEX_OFFSET,
                                        TERAKAN_HW_STATE_DRAW_INDEX_VGT_INDEX_OFFSET,
                                        &command_writer->hw_state_draw.vgt_index_offset,
                                        dst_uav_base_granularity_offset_bytes / sizeof(uint32_t));

   terakan_before_hw_draw(command_writer);

   uint32_t * packet = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_DRAW, 3);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DRAW_INDEX_AUTO, 3 - 2, 0);
   *packet++ = 1;
   *packet++ = S_0287F0_SOURCE_SELECT(V_0287F0_DI_SRC_SEL_AUTO_INDEX);
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}
