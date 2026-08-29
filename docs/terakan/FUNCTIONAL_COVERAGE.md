# Terakan functional coverage

Last updated: 2026-08-23. Hardware under test: AMD CAICOS (`1002:6779`, R9xx)
and PALM/Wrestler (`1002:9807`, R8xx).
Reference device: AMD Radeon RX 6800 XT using RADV.

This document records the focused tests used while investigating Godot and
DXVK rendering faults. It is a development snapshot, not a Vulkan 1.1
conformance claim. Terakan reports conformance version `0.0.0.0` and dangerous
tests are excluded until their resource-limit handling is fixed.

## Current implementation changes

The current development cycle adds or changes the following behavior:

- SSBO runtime-array length queries are lowered to an indexed Evergreen buffer
  size query instead of leaving `get_ssbo_size` for the Gallium path.
- Reads from an SSBO that is writable in the same shader use a returning UAV
  operation, avoiding stale vertex-fetch data after RAT writes.
- compute image reads participate in UAV tracking;
- compute completion events are emitted before CB/UAV cache flushes, including
  queue semaphore and fence signaling;
- legacy `ALL_TRANSFER` barriers consume the same deferred copy hazards as the
  synchronization2 `COPY` stage;
- graphics/compute IB transitions replay destination read-cache invalidations;
- context-register packet mode follows the active hardware mode rather than
  merely the last bound application pipeline;
- `NumWorkgroups` and the attempted `BaseWorkgroup` implementation use Terakan
  driver constants instead of Gallium's private info buffer;
- runtime compute dumps include exact Vulkan descriptor and push-constant
  layouts, and the shader-corpus test accepts these `.spv` dumps;
- optional graphics SPIR-V, selected NIR and bytecode dumps are available for
  application-level diagnosis;
- the dynamic SSBO regression now covers large offsets, sparse screen
  positions, indexed and non-indexed triangle draws, a depth prepass, depth
  reuse, intervening depth targets and transfer-to-uniform synchronization;
- the image-copy regression includes `R32_UINT` and full-size HDR/R32 paths.
- compute wave allocation uses the SQ quad-pipe count with the two-pipe
  Evergreen minimum rather than deriving it solely from render backends;
- a reset command writer explicitly starts in graphics mode, preventing stale
  recycled state from skipping the first graphics-to-compute transition;
- SFN places a workgroup barrier in separate scheduling blocks. LDS operations
  are still pseudo-instructions while dependency chains are created, so this
  prevents the scheduler from moving the barrier before a preceding LDS write
  or after a following LDS read, including inside dynamically uniform control
  flow.

## Focused results

### Expanded safe CTS matrix

An expanded CAICOS run on Vulkan CTS 1.4.6.1 completed 1071 unattended-safe
cases outside the original basic-compute list. Follow-up fixes and reruns also
covered the selected clear and basic copy/blit matrices:

| Batch | Pass | Fail | NotSupported | Warning |
|---|---:|---:|---:|---:|
| info, version, smoke, command buffers and descriptors | 90 | 3 | 78 | 0 |
| object lifetime, multithreading, buffer fill/update and pipeline lifetime | 433 | 0 | 269 | 3 |
| basic image copy and blit, after typed/mirrored blit fixes | 104 | 56 | 35 | 0 |
| selected clear operations, after 3D descriptor fix | 281 | 180 | 444 | 0 |

The API/property run exposed four Vulkan 1.1 reporting gaps. Fixed:

- interpolation offset range and precision are now reported correctly;
- `VK_KHR_image_format_list`, required by the advertised
  `VK_KHR_swapchain_mutable_format`, is now exposed.

Still open:

- `multiview` remains unexposed even though CTS requires it for the reported
  Vulkan 1.1 API version.

All 46 basic `image_to_image` cases, including partial, NPOT, linear, general,
depth and stencil variants, pass. The typed and mirrored 2D blit failures were
caused by using `CopyImage` for format-converting or mirrored operations and by
discarding the sign of mirrored coordinate transforms. After fixing those
paths, 58 of 149 blit cases pass, 56 fail and 35 are unsupported. The remaining
failures were concentrated in 3D blits.

The CTS binary was believed to be unavailable, so the 3D blit work was driven
from the driver code and a readback test instead. That belief was wrong: a built
`deqp-vk` from VK-GL-CTS 1.4.6.1 sits beside the repository on the development
machine and runs against Terakan through `bin/terakan-run`. It has since been
used on the resolve group (see below); the blit batches below still describe the
state from before that was noticed and have not been rerun. `terakan_blit_3d`
blits a 3D image whose every slice holds a distinct colour and reports which
source slice each destination slice received, for a minified, a magnified and a
mirrored depth range, then blits a four-layer 2D array. Against the code before
the fix all four groups fail: the minified and mirrored ranges hand every
destination slice the source's first slice, the magnified range leaves six of
its eight destination slices unwritten, and three of the four array layers
receive layer zero.

The blit batch has now been rerun, and the 3D blit fix holds: `blit_image.simple_tests`
goes from 58 passing and 56 failing to **114 passing and none failing**, with the
same 35 unsupported. The failures that were "concentrated in 3D blits" are gone.

With the CTS binary still unavailable, `terakan_blit_format_matrix` covers the
specific format-matrix gap the acceptance criteria name explicitly:
`VK_FORMAT_R8G8B8A8_UNORM` to `VK_FORMAT_B8G8R8A8_UNORM` (a format-converting
blit between formats with a different channel order in memory), the same pair
mirrored on X (combining the format conversion with a reversed
source/destination axis), and an identity `VK_FORMAT_R32_UINT` blit (a single
32-bit-channel format, a different pixel shader export/texture fetch shape
from the 8-bit-per-channel packed formats every other blit test here uses).
All three pass on real CAICOS hardware. This is real, previously-missing
regression coverage, not a bug found and fixed -- the full per-format matrix
CTS would exercise remains unverified, though now for want of a run rather than
for want of a binary.

### Copy and blit under CTS

A 2778-case batch covering `blit_image.simple_tests`, `image_to_image.simple_tests`,
`dimensions`, `array`, `cube` and `3d_images`, and the whole of `buffer_to_image`,
`image_to_buffer`, `buffer_to_buffer` and `buffer_to_depthstencil`: **1146 passed,
0 failed, 1632 unsupported.** Per group:

| Group | Passed | Failed | Unsupported |
|---|---:|---:|---:|
| `blit_image.simple_tests` | 114 | 0 | 35 |
| `image_to_image.simple_tests` | 23 | 0 | 0 |
| `image_to_image.dimensions` | 672 | 0 | 992 |
| `image_to_image.array` | 7 | 0 | 2 |
| `image_to_image.cube` | 6 | 0 | 0 |
| `image_to_image.3d_images` | 6 | 0 | 0 |
| `buffer_to_image` | 82 | 0 | 33 |
| `image_to_buffer` | 184 | 0 | 570 |
| `buffer_to_buffer` | 8 | 0 | 0 |
| `buffer_to_depthstencil` | 44 | 0 | 0 |

`image_to_image.dimensions` is the interesting one: 672 passing cases of varying
source and destination extents and offsets, which is the boundary coverage the
acceptance criteria ask for, and none of it fails.

### The per-format matrix, sampled

`blit_image.all_formats` (129044 cases) and `image_to_image.all_formats` (50984)
are too large to run whole, so they were sampled deterministically -- every 43rd
and every 17th case respectively, 6002 in total. **1854 passed, 204 failed, 3944
unsupported.** The two groups could hardly differ more:

| Group | Passed | Failed | Unsupported |
|---|---:|---:|---:|
| `image_to_image.all_formats` | 1439 | 0 | 1561 |
| `blit_image.all_formats` | 415 | 204 | 2383 |

Copying between images is clean across the whole sampled matrix. Blitting is not,
and the failures are not scattered: **every `*_USCALED` and `*_SSCALED` format
fails every case it is run on.** Twenty such formats appear in the sample --
`r8_uscaled`, `r8_sscaled`, `r8g8_uscaled`, `r8g8_sscaled`, `r8g8b8a8_uscaled`,
`r8g8b8a8_sscaled`, `b8g8r8a8_uscaled`, `b8g8r8a8_sscaled`,
`a8b8g8r8_uscaled_pack32`, `a8b8g8r8_sscaled_pack32`, `r16_uscaled`,
`r16_sscaled`, `r16g16_uscaled`, `r16g16_sscaled`, `r16g16b16a16_uscaled`,
`r16g16b16a16_sscaled`, `a2r10g10b10_uscaled_pack32`,
`a2r10g10b10_sscaled_pack32`, `a2b10g10r10_uscaled_pack32`,
`a2b10g10r10_sscaled_pack32` -- with 145 failures and not one pass between them.
They are advertised as blittable and are not.

SCALED image support has since been withdrawn -- see the TODO row -- taking the
sample to **415 passing and 69 failing** for blits with nothing that passed
before now skipped, and surfacing 12 previously-passing
`image_to_image.all_formats` failures, every one of them a three-component
SCALED format that the driver copies through its 3x-expansion path. Net across
the sample: 204 failures to 81.

Then the 69 turned out not to be a long tail after all. Split by image type they
were 1D 12 passing and none failing, 2D 366 and 23, and **3D 1 and 43** -- so 3D
blits were failing almost outright while looking like scattered per-format
failures in aggregate. Instrumenting one showed the driver collecting four copy
regions with source offsets 12,12,12 / 4,4,4 / 8,8,8 / 12,12,12 where CTS asked
for 0,0,0 / 4,4,4 / 8,8,8 / 12,12,12: the loop over the scaled regions was using
`copies[0]` as scratch space while probing whether a region was convertible, so
the first accumulated copy was overwritten by the last convertible one. 3D was
worst hit because its region sets mix scaled and unscaled regions, which the 2D
and 1D sets mostly do not -- and which is why `blit_image.simple_tests` passed
throughout and said nothing about it.

The signed 2_10_10_10 formats were then withdrawn from the texture path as well.
`a2r10g10b10_snorm_pack32` and `a2b10g10r10_snorm_pack32` failed every blit that
filters or converts, both of their `generate_mipmaps` cases, and both of their
`uniform_texel_buffer` cases, which is the same family radv zeroes all image
features for. Copying and vertex fetch of them measure clean and stay -- the
latter at 4 passes to 0 in `vertex_input`.

The same sweep found something it did not fix: `a2r10g10b10_sint_pack32` and
`a2r10g10b10_sscaled_pack32` fail `vertex_input` outright, 0 of 2 and 0 of 4,
while passing images and texel buffers. That is the opposite split from SNORM and
is still open.

Linear filtering was then withdrawn for unpacked three-component formats. Every
remaining blit failure used `VK_FILTER_LINEAR` -- nearest failed nothing at all,
220 cases across all three image types -- and within that `r32g32b32_sfloat`
failed 5 of its 6 linear blits while passing all 5 nearest ones. It is the only
unpacked three-component format advertised as a sampled image at all, the 8- and
16-bit ones having no texture fetch.

That leaves **18 failures, and both remaining causes are now explained**:

- 12 `image_to_image` cases, all three-component SCALED copies through the
  3x-expansion path.
- 6 blits that are 3D and linear, 6 of the 17 such cases. The driver documents
  the reason itself, in `terakan_meta_blit_depth_source_slice`: a 3D blit source
  is sampled as a 2D array, which has no depth filter, so the depth axis picks
  the nearest slice whichever filter was asked for. The 11 that pass are the ones
  whose depth mapping is one to one, where nearest and linear agree. Correcting
  it means sampling the source as a true 3D texture, which the hand-written blit
  pixel shader cannot currently do.

With all of that the sample stands at **18 failures**, from 204 where it started,
and **`api.buffer_view` fails nothing at all**, from 120. The breakdown after the
region fix, before the 2_10_10_10 withdrawal, was:

| Group | Passed | Failed |
|---|---:|---:|
| `blit_image.color` | 428 | 17 |
| `blit_image.generate_mipmaps` | 36 | 2 |
| `blit_image.depth_stencil` | 1 | 0 |
| `image_to_image.all_formats` | 1439 | 12 |

The 12 `image_to_image` failures are the three-component SCALED copies noted
above. The 19 blit failures left are the genuine long tail: spread over many
formats at one or two cases each, with the same formats passing in other
combinations.

`terakan_blit_mixed_regions` locks the region handling in with three regions --
two convertible, one halving, the first convertible one not last -- and reports
whether a wrong destination holds the sentinel or another region's source.

The remaining 59 failures are much thinner: `r32g32b32_sfloat` fails 5 of 11,
the two `a2*10*_snorm_pack32` formats fail 2 of 6 and 2 of 7, and the rest are
single cases spread across formats that otherwise pass, so those look like
specific tiling or filter combinations rather than a whole format being wrong.
Failures are spread evenly over the tiling and filter suffixes
(`general_optimal_nearest` 24, `general_general_linear` 22,
`general_general_nearest` 21, `optimal_optimal_linear` 21, and so on), which is
what a per-format rather than per-path problem looks like.

### Image clearing under CTS

A sampled run of `dEQP-VK.api.image_clearing` (every 15th case, 3042 of 45636)
failed 162. They decompose into three unrelated causes, none of which is a
one-line fix, and all three are recorded here rather than half-investigated:

**117 are three-component formats.** `vkCmdClearColorImage` returns without doing
anything for them -- see the `TODO(Triang3l): 3x-expanded format clearing` guard
in `terakan_meta_clear.c`, which bails whenever the format's bytes per block is
not a power of two. Another silent no-op. The driver has 3x-expansion machinery
for copies, but it is hand-assembled R8xx/R9xx bytecode issuing three UAV stores
per texel, so a clear variant means writing more of the same by hand. 78 of the
117 are also SCALED formats, which is the same set already withdrawn from
sampling.

Three-component formats are now the single largest cause found anywhere: 117 here,
20 in `buffer_view.access.uniform_texel_buffer`, and 12 in
`image_to_image.all_formats`, so 149 failures across three groups.

**35 are multisample and every one is an integer format, and they are not clear
failures at all.** All 143 passing multisample clears are UNORM, SRGB or float,
and sample count does not discriminate (failures appear at 2x, 4x and 8x alike),
which looked at first like a clear bug specific to integer multisample targets.
It is not. These tests read the multisample image back by resolving it, and
`terakan_meta_resolve.c` returns early without doing anything for integer
formats, because CB_RESOLVE averages samples while an integer resolve has to
select one. Instrumenting that early return shows it firing during a failing
case, and the same formats pass at a single sample, so the clear itself is not
implicated.

Two hypotheses were tested and dropped before that one. Colour compression and
fast clear are enabled for multisample images that are not sampled, which is the
only structural difference between the multisample and single-sample paths;
disabling both left all 35 still failing. The differing symptoms between formats
-- red and blue swapped for `a8b8g8r8_uint_pack32`, components halved and
duplicated for `r8g8b8a8_uint`, nothing written for `r8_uint` -- are simply
whatever stale contents the destination happened to hold, not a component mapping
pattern.

So these 35 belonged with the integer resolve gap, not with clearing, and closing
that closed them: **all 35 now pass**. The colour sample-zero resolve shader was
written as NIR rather than assembled by hand -- the first use of that path -- and
`terakan_meta_resolve.c` now takes it for integer formats instead of returning
without doing anything.

**10 are depth/stencil clears**, all returning zero. Two markers separate them:
width 1 (`1x33`, failing 5 of 6) and multiple subresource ranges in one call
(failing 5 of 8). One 3D stencil case has neither.

### Uniform texel buffers

`dEQP-VK.api.buffer_view` was run for the first time and failed 120 of 1004, with
96 of the 97 `access.uniform_texel_buffer` format cases among them. `texelFetch`
on a uniform texel buffer returned zero for every format, silently.

A sampled buffer reaches `terakan_nir_lower_bindings` as an ordinary texture
instruction, and the tex path asked for a `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE`
binding. A uniform texel buffer is not one, so the type check failed and the
fetch was lowered to a null descriptor, which returns zero by design. Nothing
else was wrong: the buffer view builds a correct resource descriptor, the
descriptor set stores it, and the fetch lowers to `load_buffer_resource_r600`
with the right format.

That took the group to 20 failures, almost entirely three-component formats --
`r8g8b8`, `b8g8r8`, `r16g16b16` in every signedness -- which the driver already
recorded as unfetchable from a buffer, and went on advertising anyway.
`dEQP-VK.pipeline.*.vertex_input` settled it from the other direction with an
exact split: every variant reading all three components failed, every
`missing_components` variant passed, 40 and 40 with nothing in between. Both uses
are now withdrawn for 8_8_8, 16_16_16 and 16_16_16_FLOAT, which is narrower than
the existing `TERASCALE_FORMATS_EXPAND_3X` mask because `r32g32b32` passes both
and stays.

**`api.buffer_view` is now at 2 failures, from 120.** The two are
`a2r10g10b10_snorm_pack32` and `a2b10g10r10_snorm_pack32`, a separate
four-component packed problem. The three-component `vertex_input` subset went
from 40 failures to none.

`terakan_uniform_texel_buffer` covers R32_UINT and R8G8B8A8_UINT, checking the
single-channel expansion to (x, 0, 0, 1) and four-channel byte order, and reports
the all-zero case specifically. It fails with that diagnostic against the
unfixed driver.

### Arrays of uniform texel buffers

`dEQP-VK.binding_model` was sampled for the first time -- 4904 cases, 558 passing
and 65 failing -- and the failures split cleanly. 84 of the 108 supported
`shader_access.*.uniform_texel_buffer.*.descriptor_array` cases failed while
arrays of combined image samplers passed all 51 of theirs.

A minimal probe reproduced it exactly: four `VkBufferView`s of one buffer at four
offsets, bound as `uniform utextureBuffer src[4]`, and all four `texelFetch`
calls returned element zero. The compiled NIR showed why -- all four fetches
carried texture index 2, the binding's base, with nothing added.

The cause is in core NIR rather than the driver. `nir_chase_binding` collects the
array indices of a descriptor array only when the descriptor is an image or a
sampler:

```c
bool is_image = glsl_type_is_image(type) || glsl_type_is_sampler(type);
```

A separate texture -- GLSL's `textureBuffer` and `texture2D`, SPIR-V's
`OpTypeImage` with `Sampled=1` and no sampler attached -- is `GLSL_TYPE_TEXTURE`
and matches neither, so the array deref chases back to the binding with
`num_indices == 0`. `terakan_nir_get_binding` then sees no array index, and every
element resolves to the same hardware resource slot.

A uniform texel buffer is the only descriptor type with no combined
image-sampler form, which is why it was the one that could not avoid the
separate-texture path and the only one that showed the bug.
`terakan_nir_chase_binding` wraps the helper and performs the same walk for the
type it skips.

**The group is now at 0 failures, from 84** -- 108 passing of 108 supported. The
sampled `binding_model` run went from 558/65 to 566/57 with no case changing in
the other direction.

`terakan_texture_array` covers it, with a non-array texel buffer in the same
descriptor set as the negative control: it has no array index to lose, so it
stays correct under the bug and would only break if the fix disturbed the
per-binding resource base. The test fails against the unfixed driver.

### Colour resolve under CTS

`dEQP-VK.api.copy_and_blit.core.resolve_image`, 240 cases, run against Terakan
for the first time: 15 passed, 87 failed, 138 unsupported. Two of the causes
were self-inflicted and are fixed, taking the group to 33 passed and 69 failed.

The fixed-function compatibility check demanded equal source and destination
surface dimensions, which was an assumption rather than a measurement --
CB_RESOLVE only requires the region to sit at the same offset in both. Requiring
the region to fit inside each surface instead took `diff_image_size` from 9
passing and 18 failing to 27 passing and none failing. Differing array layers
were excluded because Evergreen shares one per-draw array-slice select across
every bound colour buffer; the destination is now moved to meet the source by
shifting its base address, which works whenever the destination carries no
colour metadata.

Multisample `vkCmdCopyImage` has since been given the whole-surface case, taking
the group to **69 passing and 33 failing** -- the exact mirror of where it stood
before. Every group that copies a multisample image whole before resolving it
now passes. The 33 that remain are 24 multisample copies that are *not* the whole
of two identical surfaces (a subregion, a single layer, differing layouts), which
still take the meta-draw path and are still skipped, and the 9 `partial` and
`with_regions` cases that need differing resolve offsets.

The decomposition below describes the state before that change:

- 60 cases are gated on multisample `vkCmdCopyImage`, which is still a silent
  no-op. 20 of them say so directly ("Intermediate verification failed for
  coordinates (0, 0) sample 0" -- the intermediate copy never happened) and 40
  fail later at the final compare. These are every `*_copy_before_resolving*`
  group plus `whole_array_image` and `whole_array_image_one_region`, both of
  which also copy the multisample image before resolving it.
- 9 cases (`partial`, `with_regions`) need differing source and destination
  offsets, which one coordinate stream cannot express. Those want the shader
  resolve path, disabled in `terakan_meta_resolve.c`.

So multisample `vkCmdCopyImage` is worth 60 of the 69, on top of being an
application-visible silent no-op in its own right.

`terakan_copy_image_subresource` covers the vkCmdCopyImage subresource
gaps the "remaining copy, blit and resolve" acceptance criteria name
explicitly: a partial-extent copy at non-zero source and destination
offsets landing in a different array layer, a copy between two different
mip levels of the same image, and a copy region spanning multiple array
layers at once. The source image is filled per level/layer with a value
encoding its own mip, layer and (x, y) position and the destination
starts filled with a sentinel, so a readback distinguishes a
misplaced/mis-sized copy from a merely wrong value. All four cases pass
on real CAICOS hardware, repeated eight times with zero failures given
the earlier retracted "deterministic" MSAA claim above making single-run
results untrustworthy on their own. Multisampled vkCmdCopyImage itself is
still not implemented -- `terakan_meta_copy_image.c` has no sample-count
guard before its single-sample-shaped meta-draw copy path -- but
`terakan_copy_image_multisample_noop_test` establishes what actually
happens today: the destination is left completely untouched (6/6 repeat
runs), not corrupted, most likely because a #MemoryIntegrity-style check
inside the meta-draw's descriptor creation rejects the source's mismatched
dimensionality (bound as plain 2D_ARRAY regardless of its real sample
count) and the copy loop silently skips the region. Implementing real MSAA
copying remains comparable in scope to the FMASK/CMASK work; this test
just locks in that the current gap is a silent no-op rather than the
silent corruption it looked like it could be, so a future unrelated change
cannot regress into that worse failure mode unnoticed.

`terakan_storage_format_matrix` walks 28 storage-image formats spanning every
class Terakan advertises -- UNORM, SNORM, packed 10/11-bit, 16- and 32-bit
float, and UINT/SINT at 8/16/32 bits, one to four channels -- storing a known
value through a formatless storage image and loading the same texel back in a
single dispatch (a coherent image plus memoryBarrierImage(), which is what
makes a same-invocation store-then-load of one address well defined). All 28
are advertised as storage images and all 28 round-trip correctly on real
CAICOS hardware, none skipped. Only the channels a format actually has are
compared, since a one-channel format loads as (r, 0, 0, 1), and tolerances are
per format. Together with terakan_storage_image_atomic this closes the
load/store and atomic halves of the storage-image coverage item; what remains
there is shaderStorageImageMultisample.

`terakan_stencil_only_render` closes the long-open "stencil-only render
targets on combined depth/stencil images" P0 item, which two earlier
investigation passes had failed to reproduce at all. Two changes in approach
made it reproduce deterministically, both of them things TODO.md had recorded
as untried: drawing through the rasterizer's STENCIL_REPLACE path rather than
relying on a bare LOAD_OP_CLEAR (a clear does not go through the same DB
base-address programming a draw does, which is why the earlier passes saw
nothing), and running many renders back to back inside one command buffer with
a different stencil reference each. A four-way control matrix then isolated it
exactly: stencil-only failed with both dynamic and static stencil references,
and binding a depth attachment alongside passed in both -- purely "no depth
attachment bound".

The cause was in terakan_hw_config_draw_set_db_depth_stencil_buffer(), which
stored the two aspects' DB base addresses swapped whenever depth was not bound,
so the stencil-only emit path wrote the depth plane's address into
DB_STENCIL_READ_BASE/DB_STENCIL_WRITE_BASE and every stencil write landed in
the depth plane. Register-level instrumentation confirmed it directly before
anything was changed. The test is a verified negative control: against the
unfixed driver its stencil-only pass fails 15/16 iterations while its
depth-bound pass still passes, and both pass after the fix.

`terakan_color_msaa_fetch_2x`/`_4x`/`_8x` cover per-sample reads of a
multisample COLOUR image, which now pass at all three sample counts on real
CAICOS hardware. Each sample is given its own distinct colour by a draw
confined to that sample through the pipeline's sample mask, so a fetch that
lands on the wrong plane is caught, not just one that lands outside the
surface. Three real bugs were fixed to get there, all described in
[TODO.md](TODO.md): the 2x FMASK identity constant used a 1-bit-per-sample
field where the hardware uses 2 bits, the identity initialization never ran
when a render pass performed the initial layout transition, and CB colour
compression/fast clear was enabled even for sampled images, letting the CB
leave fragment planes unwritten that a texture fetch then read directly.

`terakan_color_resolve_multivalued_2x`/`_4x`/`_8x` cover the resolve half of
the same item. Every other resolve test in this suite resolves a surface whose
samples all hold the same value, which passes for VK_RESOLVE_MODE_AVERAGE no
matter what the hardware does with the individual samples. This one gives each
sample its own colour and checks the resolved result against the actual
arithmetic mean, with the colours chosen so the mean equals no individual
sample's value, so a resolve that returns one sample instead of averaging is
caught. It also deliberately omits VK_IMAGE_USAGE_SAMPLED_BIT so the
multisample image keeps CB compression and fast clear enabled, making it the
only coverage of the compressed CB write and CB_RESOLVE path. All three
sample counts pass.

An earlier version of this test cleared every sample to the same colour. That
version could only detect an out-of-range plane index, and under it the wrong
FMASK constants scored *better* than the correct ones, because they pointed
more samples at plane 0 -- the only plane a fast clear writes. The per-sample
colours are what made the real encoding legible.

The original probing notes follow.

`terakan_color_msaa_fetch_test` probed the FMASK/CMASK item's per-sample
colour read path directly for the first time and found, then partly fixed,
a real bug: the shader's FMASK decode (`lower_txf_ms` in
`sfn_instr_tex.cpp`) uses a fixed 4-bit-per-sample nibble field regardless
of sample count, but the 2x/4x identity-fill constants in
`terakan_barrier.c` didn't follow that convention (only 8x's already did).
Fixing the constants took per-sample reads from 100% wrong to roughly 25%
wrong on CAICOS, and the full regression suite stayed green with the fix
applied. Chasing the remaining ~25% surfaced a deeper problem -- run-to-run
instability including one fence-wait timeout -- pointing at the identity
fill not reliably covering the tiled FMASK surface (likely a bank-rotation
or macro-tile addressing mismatch), which needs real further tiling
research and was not pushed further once it showed a hang risk. See the
detailed account in [TODO.md](TODO.md). The test is committed with a
`--samples=N` selector but deliberately not wired into
`bin/terakan-test`'s required-green suite.

`terakan_color_resolve_subresource` is the first test in this suite to
exercise a COLOR attachment multisample resolve at all -- prior coverage
was depth/stencil only. It found a real bug: resolving into a
destination array layer other than the multisample source's own array
layer did not fail to write the requested layer, it silently wrote the
resolved color into the SOURCE's layer of the destination instead,
corrupting whatever was there. This looks like Evergreen's CB_RESOLVE
sharing one per-draw array-slice-select state across both bound color
buffers rather than addressing each RTV's slice independently. The fix,
in `terakan_meta_resolve_region_is_fixed_function_compatible`
(`terakan_meta_resolve.c`), requires a matching source/destination array
layer the same way it already required matching extents and offsets, so
a cross-layer resolve region is now skipped -- like any other
CB_RESOLVE-incompatible region -- instead of corrupting the wrong layer.
Three cases pass on real CAICOS hardware, repeated six times with zero
failures: a non-zero mip level with an offset render area, a non-zero
array layer shared identically by source and destination (the only
array-layer combination CB_RESOLVE supports), and a regression check
that a mismatched-layer resolve now leaves the sentinel intact in both
the requested layer and the source's own layer of the destination.
Cross-layer color resolve itself remains genuinely unsupported -- there
is no shader fallback available for it -- just safely so rather than
silently corrupting data.

The initial selected clear batch passed its first 64 1D and 2D color cases, then
`dEQP-VK.api.image_clearing.core.clear_color_image.3d.optimal.single_layer.r8g8b8a8_unorm`
returned `VK_ERROR_DEVICE_LOST`. The kernel rejected the command stream with:

```text
evergreen_cs_track_validate_texture: mipmap bo base 4793344 not aligned with 4096
[drm:radeon_cs_ioctl [radeon]] *ERROR* Invalid command stream !
```

The fix aligns the separately addressed 3D mip chain and uses each origin mip
level's actual array mode in its resource descriptor. The formerly fatal case
and the full selected color-clear matrix now pass without device loss. The 180
remaining clear failures are depth/stencil subresource and partial-attachment
semantics, not command-stream rejection.

### Local Terakan suite

The wrapper currently reports all four CPU tests passing:

```text
terakan_sfn_lowering
terakan_hw_config_loop_constants
terakan_descriptor_buffer
terakan_vertex_input
```

All nineteen CAICOS GPU tests pass:

```text
terakan_image_array_copy
terakan_instance_dynamic_ssbo
terakan_bc6_cube
terakan_bc6_cube_single_level_views
terakan_bc6_array_view
terakan_compute_loop
terakan_formatless_image_store
terakan_extended_format_storage_image
terakan_storage_image_atomic
terakan_storage_format_matrix
terakan_dynamic_offset_bounds
terakan_clip_distance
terakan_depth_readback
terakan_depth_msaa_fetch
terakan_color_msaa_fetch_2x
terakan_color_msaa_fetch_4x
terakan_color_msaa_fetch_8x
terakan_color_resolve_multivalued_2x
terakan_color_resolve_multivalued_4x
terakan_color_resolve_multivalued_8x
terakan_depth_resolve
terakan_depth_stencil_resolve
terakan_stencil_readback
terakan_stencil_only_render
terakan_stencil_fetch
terakan_stencil_msaa_fetch
terakan_frame_chain
terakan_frame_chain_compute
terakan_frame_chain_depth
terakan_frame_chain_ssbo
terakan_frame_chain_multi_size
terakan_frame_chain_multi_pipeline
terakan_dynamic_rendering
terakan_blit_3d
terakan_blit_format_matrix
terakan_copy_image_subresource
terakan_copy_image_multisample_noop
terakan_color_resolve_subresource
terakan_depth_resolve_subresource
terakan_resolve_modes_2x
terakan_resolve_modes_4x
terakan_resolve_modes_8x
terakan_stencil_resolve_modes_2x
terakan_stencil_resolve_modes_4x
terakan_stencil_resolve_modes_8x
terakan_physical_device_properties
terakan_terascale_1_enumeration
```

`terakan_compute_loop` now inserts a compute shader-write to shader-read/write
dependency between its 12 dispatches. Its iterative CPU oracle passes on the
RX 6800 XT with RADV and on both CAICOS and PALM with Terakan; guard regions
also remain intact. The earlier failure was caused by the test omitting the
Vulkan dependency while assuming every dispatch observed the preceding write.

### Vulkan CTS basic compute

A safe list of 77 `dEQP-VK.compute.pipeline.basic` cases produced:

| Result | Count | Meaning |
|---|---:|---|
| Pass | 48 | Correct readback or CTS result |
| Fail | 0 | No failures in the safe list |
| NotSupported | 29 | Feature or queue not advertised |

All 48 tests that actually execute pass. SSBO reads and
writes, runtime arrays, atomics, shared variables, ordinary workgroup and
command barriers, image load/store, compute/image transitions and
`indirect_after_base_dispatch` pass.

The same current development build was copied without recompilation to an
R8xx PALM/Wrestler system and produced the identical `48 Pass / 0 Fail / 29
NotSupported` result. This includes SSBO and UBO access, LDS/shared variables,
shared atomics, command and workgroup barriers, storage images, large
image/SSBO copies and indirect dispatch. The older system-installed Terakan
build on PALM failed these paths, so results from that package are not
representative of the current tree.

The previous failures `write_ssbo_array` and `webgl_spirv_loop` now pass.

Do not run `max_local_size_x`, `max_local_size_y` or `max_local_size_z` on the
current driver. They previously caused a GPU lockup or device loss and are not
part of the percentage above.

### Synchronization, WSI and render-pass probes

All 13 selected single-queue producer/consumer hazards pass:

- clear image to copy, compute and fragment consumers;
- compute image writes to copy, compute and fragment consumers;
- draw writes to copy, compute and fragment consumers;
- fragment storage-image writes to copy, compute and fragment consumers;
- clear attachments to fragment sampling.

Both Wayland swapchain rendering cases pass. Five CTS custom-resolve cases are
`NotSupported` because `customResolve` is not advertised; this is not counted
as a failed implementation. `VK_KHR_depth_stencil_resolve` is now advertised
with `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` for depth and stencil, so those CTS
cases are worth re-running.

### Confirmed failures

The previous state-dependent `branch_past_barrier` failure was traced to SFN
scheduling rather than cross-dispatch hardware state. Before LDS pseudo-ops
were split into ALU instructions, the dependency builder could not connect a
barrier to the preceding write; the scheduler consequently moved
`GROUP_BARRIER` to the start of the trigger shader. Separate scheduling blocks
now preserve `LDS_WRITE -> GROUP_BARRIER -> LDS_READ`. The formerly failing
two-test trigger and the complete safe list pass on both R8xx and R9xx.

`dEQP-VK.compute.pipeline.device_group.dispatch_base` still fails. Terascale's
`VGT_COMPUTE_START` does not directly provide Vulkan `DispatchBase` semantics,
so a driver-constant base is now lowered into the shader. The constants are
correct on the CPU side, but repeated segmented dispatches still behave as if
the GPU observes a zero or stale base. The simpler
`indirect_after_base_dispatch` regression passes because the indirect dispatch
is explicitly reset to base zero.

`dEQP-VK.compute.pipeline.device_group.device_index` also fails, indicating
that `gl_DeviceIndex` still needs an explicit Terakan lowering. Both failures
are low priority for ordinary single-GPU games, but are required for complete
Vulkan 1.1 device-group behavior.

The same three tests pass on the RX 6800 XT with RADV, confirming that the test
inputs and expected values are valid.

## Unsupported capability groups

The 29 basic-compute `NotSupported` results consist primarily of 21
`shaderReplicatedComposites` cases from newer Vulkan functionality. The rest
require one of the following:

- a second or compute-only queue;
- `VK_EXT_robustness2`;
- shader FP64, FP16 or 16-bit storage operations;
- a newer SPIR-V version.

Terakan also intentionally leaves geometry and tessellation shaders,
vertex-stage stores and atomics, extended storage-image formats, multisample
storage images, image gather extensions, clip/cull distances,
Int16/Int64/Float64, sparse
resources and depth bounds disabled. Vulkan 1.1 allows these feature bits to be
false, but they do not have equal game impact:

| Capability group | Game importance | Estimated complexity |
|---|---:|---:|
| Storage-image/UAV formats, formatless access and multisample access | 4/5 | 4/5 |
| Shader clip/cull distances and extended image gather | 4/5 | 3/5 |
| Geometry shaders | 4/5 | 5/5 |
| Vertex-pipeline stores and atomics | 4/5 | 4/5 |
| Tessellation shaders | 3/5 | 5/5 |
| FP16/Int16 storage and arithmetic | 2/5 | 4/5 |
| FP64/Int64, sparse resources and depth bounds | 1/5 | 4-5/5 |

Single-sample formatless storage-image reads and writes are enabled separately.
The focused CAICOS test uses both corresponding SPIR-V capabilities, compares
transfer-initialized reads and independently generated writes against exact
17x13 `R32_UINT` oracles, and verifies trailing guards.

The 21 replicated-composite cases, maintenance5, custom resolve and specialized
queue topology are not Vulkan 1.1 or ordinary D3D11 game blockers. They should
not displace the shader, UAV, MSAA and control-flow work above.

## Application evidence

User-observed testing after the compute, descriptor and synchronization work:

- Hangover Gallery renders correctly;
- Fused 240 renders correctly, with earlier menu-lighting faults resolved;
- Buckshot Roulette no longer has a stable correct result and can show a
  corrupted frame or strobe between incorrect backgrounds.

Because focused image hazards and WSI pass, the remaining Buckshot fault is
more likely to be in uncovered graphics shader/control-flow, blending,
render-pass dependency, descriptor update or format behavior than in the
already tested basic image barriers.

Session findings (2026-08-23, CAICOS): the strobing main-menu background is
not cross-process VRAM reuse — content stayed a consistent magenta/black/white
block pattern regardless of what ran on the GPU immediately before (tested
against both vkcube and a fresh Fused 240 run, neither of which resembles the
corruption). The large (988x1156 and similar non-power-of-two) decoration
images upload through `terakan_CmdCopyBufferToImage2` as ~300 separate 64x64
RTV draws per image; forcing an unconditional `PARTIAL_FLUSH_CP_THROUGH_PS` +
`FLUSH_INV_CB_RTV_DATA` + `INV_TC` barrier after every chunk draw removed the
block-grid corruption pattern from a single captured frame, but the live
strobe was unchanged, meaning the fault recurs every frame rather than once at
upload time. This points away from the one-time buffer-to-image upload path
and toward a per-frame hazard (most likely a compute-driven screen/background
effect racing its CB or RAT cache visibility with a later sampled read),
consistent with the still-open cache/barrier coherency item in
[TODO.md](TODO.md). Not yet root-caused; needs a frame-by-frame trace
(`TERAKAN_DEBUG_RENDER`/`TERAKAN_DEBUG_RAT`/`TERAKAN_DEBUG_QUEUE_IBS`)
correlated against a screen recording rather than single-shot screenshots,
which cannot reliably catch the bad frame.

Since then `terakan_frame_chain` was added to test that shape directly:
twenty-four frames of produce, sample, render and copy in one command buffer.
It now runs with six variants — a render pass, a compute dispatch,
(`--depth`) a depth-only render pass whose result is sampled the way a shadow
map is, (`--compute-ssbo`) a compute dispatch that writes both a storage
image and a storage buffer, with the sampling pass reading the image into RGB
and the buffer into alpha so a stale read of either is independently visible,
(`--multi-size`) a second, independently sized 4x4 chain recorded right
after the main 16x16 one within the same per-frame block, with its own
colour range disjoint from the main chain's so a leak between the two sizes
is unambiguous, and (`--compute-multi-pipeline`) six distinct `VkPipeline`
objects sharing one shader module bound round-robin across frames instead of
rebinding the same pipeline every time. All six pass, closing the "mix
storage buffers with storage images", "several render target sizes in the
same frame" and "many distinct compute pipelines rather than one" gaps this
note used to name -- every concrete gap it originally listed. What real
applications do beyond this composition shape remains open, but nothing
specific is left unaddressed here.

Buckshot Roulette was retested after the stencil-aspect view swizzle and the
multisample `MIP_ADDRESS` fixes landed, and the symptom changed: it now renders
real graphics, where before the image was a grid of magenta and black blocks.
What remains is a flicker between a black background and white with red dots.
The block corruption is therefore explained by the sampling fixes; the flicker
is not, and is still consistent with the open cache/barrier coherency item.

A `TERAKAN_DEBUG_RENDER` trace of that run shows what its frames are made of,
which is useful for aiming further work: per frame roughly seventeen depth-only
passes (4096x4096, 512x512, 256x256 and 128x128 shadow maps plus a 960x540
depth prepass), a three-attachment 960x540 pass, several single-attachment
960x540 passes, and one 1920x1080 pass. Depth-only passes dominate by a wide
margin, which is what prompted the `--depth` producer above.

Only a person watching the screen can judge the flicker: single screenshots
repeatedly failed to catch a bad frame.

## Reproduction

Use the development ICD explicitly and disable implicit layers:

```bash
export VK_DRIVER_FILES="$PWD/build-vulkan/src/amd/terascale/vulkan/terascale_devenv_icd.x86_64.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export VK_LOADER_LAYERS_DISABLE='~implicit~'
export MESA_SHADER_CACHE_DISABLE=true
```

Run CTS from its Vulkan module build directory so Amber resources resolve:

```bash
cd ../VK-GL-CTS-1.4.6.1/build-vulkan/external/vulkancts/modules/vulkan
./deqp-vk --deqp-case='dEQP-VK.compute.pipeline.basic.branch_past_barrier'
```

Running the CTS binary from the Mesa source directory produces a
`ResourceError` for Amber files and is not a valid driver result.
