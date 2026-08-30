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

### Storage image views of a single array slice

`shader_access.*.storage_image.*` failed 216 of its 1470 supported cases, and the
failing leaves were exact: `1d_base_slice`, `2d_base_slice`, and every `cube`
leaf. Everything with `_array` in the name passed in full, including
`1d_array_base_slice`, `2d_array_base_slice` and `cube_array_base_slice`, as did
plain `1d`, `2d`, `3d` and their `base_mip` variants.

A probe settled it directly: a four-layer array image, a
`VK_IMAGE_VIEW_TYPE_2D` storage view of one layer, a compute shader writing a
sentinel, and a read-back of all four layers. The write landed on layer zero for
every `baseArrayLayer` from 0 to 3.

The colour descriptor was right -- `terakan_image_create_color_descriptor`
encodes the slice into `CB_COLOR*_VIEW`'s `SLICE_START` -- but the view type
chose `RESOURCE_TYPE`, and `VK_IMAGE_VIEW_TYPE_2D` asked for `TEXTURE2D`. Under a
non-array resource type the hardware has no slice to select, so the slice fields
mean nothing and every write collapsed onto the first slice. Array views were
unaffected because they ask for `TEXTURE2DARRAY` and supply the layer in the
coordinate's Z.

Supplying `Z = 0` alone changed nothing, which is what ruled out the coordinate
as the sole cause and pointed at the resource type. Single-slice views now use
the array resource type, bounded to one slice by `SLICE_MAX`, and the UAV
coordinate builder always emits Z -- zero for a non-array view, and the cube face
for a cube one, which it had been dropping entirely.

**The group is now at 0 failures, from 216** -- 1470 passing of 1470 supported.
The sampled `binding_model` run went from 566/57 to 573/50, and the whole of
`shader_access` is clean; what remains is `descriptorset_random`.

Rendering was the risk, since render target views share the colour descriptor. A
14459-case run over `pipeline.monolithic.render_to_image`,
`renderpass.suballocation.attachment_allocation` and sampled `api.image_clearing`
and `api.copy_and_blit` gives 6103 passing and 716 failing both before and after,
with an identical set of failing cases.

`terakan_storage_image_base_slice` writes through a non-array view of each of the
four layers in turn, checking that the other three keep their own sentinel clear
values. An array view written in the same dispatch is the negative control: it
selected its slice correctly before the fix, so a change that moved the slice by
the wrong amount or applied the offset twice shows up there rather than passing
silently. The test fails against the unfixed driver.

### Dynamically indexed arrays of uniform texel buffers

The constant-index fix left the other half. A dynamically uniform index --
`texelFetch(src[gl_WorkGroupID.x], 0)` -- still read element zero from all four
elements.

The driver's lowering was doing its part: it turns a non-constant array index into
`nir_tex_src_texture_offset`, and instrumenting confirmed the offset arrived in
the backend as a real register. `TexInstr::emit_buf_txf` then dropped it, because
it read only `src.sampler_offset`:

```c
PRegister tex_offset = nullptr;
if (src.sampler_offset)
   tex_offset = shader.emit_load_to_register(src.sampler_offset);
```

That is what GL produces, where samplers are combined with textures. A sampled
buffer has no sampler at all, so under Vulkan the only offset it can carry is the
texture one, and the fetch fell back to the array's base resource slot. The
buffer fetch now prefers `texture_offset` and keeps `sampler_offset` as the
fallback, which leaves the GL path unchanged.

This was found while looking for the cause of `descriptorset_random`, whose
failures concentrated on the dynamically indexed variants: `constant` passed 67 of
104 while `unifindexed` passed 267 of 848 and `dynindexed` 310 of 848.

**`descriptorset_random` went from 1000 passing / 1752 failing to 1565 / 1187**
over its full 35148 cases. `api.buffer_view` is at 411 passing and 0 failing, and
every `binding_model` group other than `descriptorset_random` is now clean.

`terakan_texture_array` reads both index forms, since they reach the hardware by
different routes -- a resource slot resolved at compile time against an index
register loaded at run time -- and each had its own way of losing the index.

### Update after bind, withdrawn

`descriptorset_random` was still failing 1187 of its 2752 supported cases after
the indexing fixes, and slicing it gave a clean answer. Holding everything else
constant, the simplest configuration passed 52 of 52 without update after bind and
15 of 52 with it.

A probe confirmed the mechanism directly: bind a descriptor set, record the
command buffer, then rewrite the descriptors, and the shader reads the old ones.

There is no descriptor heap to point the hardware at. TeraScale takes its
resources as SQ resource constants, and `terakan_CmdBindDescriptorSets` reads the
contents of the descriptor set and writes them into the command stream as register
state. That is a snapshot taken at record time, and the six
`descriptorBinding*UpdateAfterBind` features were advertised over it.

Supporting it would mean recording, for every descriptor written into the stream
from an update-after-bind binding, where in the indirect buffer it landed, and
rewriting those dwords from the set's current contents at every submission --
every submission, because a command buffer may be submitted more than once. That
is a real design and not a small one. Until it exists the features are withdrawn.

**`descriptorset_random` went to 2103 passing / 649 failing, from 1565 / 1187**,
with the not-supported count unchanged at 32396 -- the tests do not skip when the
feature is absent, they stop asking for it. Update after bind is now neutral in
the results, failing 23% of cases with the flag and 23% without.

What remains is sampled images combined with non-constant indexing:
`sampledimg` with `constant` passes 104 of 104, while `unifindexed` fails 42% and
`dynindexed` 35%. Without sampled images the same cells fail 17% and 6%.

### 3D blits with a linear filter

The full `blit_image` group was run for the first time -- 129193 cases -- and
failed 273. All 273 were `all_formats.color.3d` with `linear_stripes_z`, the
variants whose content varies along depth, and nothing else in the group failed
at all.

The blit path sampled every source as a 2D array and selected the nearest slice.
That is right for an array, where there is nothing between two layers, and wrong
for a 3D image, where depth is a continuous axis and `VK_FILTER_LINEAR` has to
interpolate along it.

Three things were missing and all three were needed:

- the source is described as a 3D resource rather than a 2D array;
- the sampler's `Z_FILTER` is set to linear -- it is a field of its own, and a
  linear XY filter does not touch it;
- the pixel shader takes a depth coordinate, which the hand-written 2D one has no
  way to accept.

The last is the first meta shader written as NIR because it was needed rather than
as a demonstration. It reads its constants straight out of the constant cache
through `load_ubo_vec4`, naming the hardware buffer, which meant recording kcache
and sampler use in `terakan_meta_nir_compile` alongside the resource slots.

A 3D resource descriptor has no equivalent of `BASE_ARRAY` -- the slices of a
tiled 3D image are interleaved, so a depth range cannot be selected by re-basing
-- so it covers the whole mip and the depth coordinate is normalized over that,
exactly as X and Y are normalized over the mip's width and height. Normalizing
over the region's own depth range instead was the first attempt, and it broke the
five partial-range cases of `blit_image.simple_tests` while fixing the rest.

**`blit_image` is now at 0 failures over all 129193 cases**, 19136 passing of
19136 supported. Disabling the depth filter brings back exactly 273 failures, the
same number.

`terakan_blit_3d_depth_filter` blits a two-slice source into four destination
slices, so the destination slices land at source depths 0.25, 0.75, 1.25 and 1.75
and the middle two must be quarter and three-quarter mixes. The same blit with
`VK_FILTER_NEAREST` is the negative control: it must keep producing the two
unmixed source values, so a driver that ignores the filter fails one half or the
other. It fails against the unfixed driver.

### Why the shader resolve fallback still does not work

`resolve_image` is at 33 failures, and they are now fully accounted for. 24 --
`whole_array_image`, `whole_array_image_one_region`, `layer_copy_before_resolving`
and `copy_with_regions_before_resolving` -- fail before the resolve is reached, at
the intermediate multisample `vkCmdCopyImage` those groups perform first; the
driver's own log shows a one-layer source copied into five different layers of a
five-layer destination as five regions, which the whole-surface CP DMA path cannot
serve and which cross-slice byte copying cannot serve either, because of the bank
rotation between slices. The other 9 -- `partial` and `with_regions` -- fail at the
resolve itself, because their source and destination regions sit at different
offsets and CB_RESOLVE reads and writes one coordinate stream.

The offset case was attempted through the shader path and the attempt is recorded
here because it narrowed a note that had been vague. A source-offset constant and
three averaging shaders were written; with them every non-integer resolve failed,
including cases that pass through the fixed function. Three measurements isolated
the cause:

- with the offset constant stubbed to zero, the results were unchanged, so the
  constant plumbing is not at fault;
- with the averaging shader reduced to a single fetch of sample zero, 52 of the
  102 supported cases passed -- the fetch, the float export and the destination
  binding all work;
- with two fetches of *the same* sample averaged together, 53 passed, so emitting
  more than one fetch is fine too.

What fails is fetching a sample whose index is not zero. On Evergreen,
`LowerTexToBackend::lower_txf_ms` does not fetch the sample directly: it reads
FMASK first and uses the value to translate the sample index into a fragment
index. The meta resolve binds only the colour resource, with nothing set up for
that first read, which is the specific form of the older note that the shader
resolve "did not decode direct sample coordinates correctly". Making it work means
giving the meta path the FMASK binding the application path already has, not
writing a different shader; the shaders themselves were reverted rather than left
in place not working.

### Clearing three-component formats

`vkCmdClearColorImage` had no path for three-component formats and returned
without doing anything, silently, for every one of them. Measured over the
three-component subset of `dEQP-VK.api.image_clearing` -- 19698 cases -- that was
9700 passing and 1800 failing, and every failure was this.

These formats have no hardware equivalent, so each component is stored as its own
surfel and the image is three times as wide in surfels as in texels; the colour
target cannot express that. The clear now takes the route the 3x copy already
used: a UAV, one fragment per texel, three stores at 3x + 0, 1 and 2. No modulo is
needed anywhere, because the fragment's X already counts texels. The shader is
built as NIR.

Two things had to be got right beyond the shader.

A scaled format holds an integer read as a value rather than as a fraction, and
the clear value for one arrives in the integer members of `VkClearColorValue`.
`util_format_pack_rgba` dispatches on `util_format_is_pure_uint`/`_sint`, which
are false for scaled formats, so it read those integers as floats -- and a small
integer read as a float bit pattern is a denormal, which packs to zero. Every
scaled component cleared to zero until the source was converted first.

A 3D image has one array layer and a stack of depth slices, and the subresource's
`layerCount` describes the array, not the depth. Taking it at face value cleared
only the first slice, which left all 72 of the 3D cases failing while every other
shape passed.

**The subset is now at 10300 passing and 1200 failing, from 9700 / 1800** -- 600
fixed with no case moving the other way.

The 1200 that remained after this were exactly the one- and two-byte-surfel scaled
formats, and they were not clear failures at all. A probe settled it: the image's
own memory held the correct bytes -- `11 22 33` repeated across the width for a
clear of (17, 34, 51) -- while `vkCmdCopyImageToBuffer` on the same image returned
`00 ff ff` repeating, which is exactly what the tests report. That is a separate
defect in the copy, fixed next; with it fixed **the subset is at 11500 passing and
0 failing**, from 9700 / 1800.

`terakan_clear_expand_3x` therefore reads the image's memory directly rather than
copying it out, since a test built on the copy would fail for a reason unrelated
to what it tests. It checks the first row holds the three components in order
across the width, and that the number of bytes that changed equals three surfels
per texel over every texel of every slice -- which catches a clear that stopped
after one row or one slice, and equally one that ran past the end. It fails
against the unfixed driver.

### Copying three-component images with small surfels

The 3x copy shader fetched all three components at once, as `8_8_8`, `16_16_16` or
`32_32_32`. Only the last of those works. This hardware returns completely invalid
values for three-component 8- and 16-bit buffer fetches -- the limitation already
recorded for `api.buffer_view` and `vertex_input`, and one that
`terakan_nir_load_raw_resource_buffer` asserts against, so the driver knew about it
in one place while relying on it in another.

Every copy of a one- or two-byte-surfel three-component image therefore read
nonsense, and because `api.image_clearing` verifies by copying the image out, it
also made correct clears of those formats look broken.

The components are now fetched one at a time, which avoids the format entirely.
The fetch format is part of the instruction rather than of the descriptor, so
there is one shader per surfel size; all three are built as NIR, and the source
descriptor becomes a plain byte-addressed buffer. Requesting the load at the
surfel's own width produced a `u2u8`, which the backend does not accept, so it is
loaded as a single component of the right format and kept 32-bit throughout.

**The 8- and 16-bit three-component subset of `api.copy_and_blit` went from 24800
passing / 232 failing to 25032 / 0**, and the three-component `api.image_clearing`
subset from 9700 / 1800 to 11500 / 0. Nothing moved the other way; the one
remaining failure in the wider run, a BC3 block reinterpretation, fails identically
before and after.

`terakan_copy_expand_3x` checks each surfel size twice. A clear followed by a
read-back isolates the image-to-buffer direction against a value the clear test
covers independently, and a pattern uploaded and read back exercises both
directions with every surfel of the image distinct, so a copy landing on the wrong
surfel shows as a displaced value rather than a plausible one. It fails against the
unfixed driver.

### What is left in image clearing, and what it is not

With the three-component work done, `dEQP-VK.api.image_clearing` was run in full
for the first time: **29925 passing, 155 failing** of 45636. Every remaining
failure is `clear_depth_stencil_image`, and they split into two defects that were
localized but not fixed.

**Multi-range clears lost the earlier aspect, and now do not.** CTS's
`multiple_subresourcerange` variants pass two ranges over the same subresource, one
naming only stencil and one only depth. The driver's log showed both draws issued
with the right controls -- the stencil draw with `STENCIL_ENABLE` and `REPLACE`,
the depth draw with `Z_WRITE_ENABLE` and stencil off -- and the result had the
depth right and the stencil zero. Reversing the order in which the ranges were
processed made both correct, so the second draw was destroying what the first
wrote, and not symmetrically: depth drawn first survived a following stencil draw.

Four explanations were tested and excluded. A `FLUSH_INV_DB_DATA` with a partial
flush between the two draws changes nothing, so it is not a stale depth cache.
Re-emitting `DB_STENCILREFMASK` per range changes nothing, and setting its write
mask to zero for a depth-only range changes nothing either. Binding only the aspect
a range clears changes nothing. And the stencil ops of the depth draw are `KEEP`
with `STENCIL_ENABLE` clear, so on paper it cannot touch stencil at all.

What does work is not issuing the second draw. One draw clearing both aspects of a
subresource gives the right result, and it is what the two draws were meant to add
up to, so the clear now visits each subresource once with the union of the aspects
every range asks of it, extending a draw over consecutive layers that want the same
aspects. **`clear_depth_stencil_image` went from 297 passing / 153 failing to 350 /
100.** One case changed the other way in the group run,
`dedicated_allocation...d16_unorm_64x11`; run on its own it fails on the unmodified
driver too, so it is the extent defect below surfacing differently rather than a
regression.

The mechanism behind the interference is still not established, and the fix avoids
it rather than explaining it. `terakan_depth_stencil_clear_ranges` covers the three
orderings but does **not** fail against the unfixed driver: the interference could
not be reproduced outside CTS at either 32x32 or 256x256, with either
`D32_SFLOAT_S8_UINT` or `D16_UNORM_S8_UINT`, with the image filled by an earlier
clear first, or with sampled usage added. The CTS group is what carries the
evidence.

**The rest do not fail deterministically, which is the finding.** `1x33`, `64x11`,
`33x128`, `32x29x3` and others read back as zero from a single-range clear that the
log shows being issued with the right extent and a descriptor whose
`DB_DEPTH_SIZE` and `DB_DEPTH_SLICE` decode correctly -- but **two runs of the same
1098-case list disagree on seven of them**. So this is not addressing. It is
ordering or visibility: the clear's writes are sometimes not where the read-back
looks.

Instrumenting the tiling mode of every clear and correlating it with the result
gives a clean split -- **2D-tiled surfaces pass 94 of 94, 1D-tiled ones pass 256 of
356** -- but 1D tiling here is a proxy for a small image rather than a cause, and a
small image is where a tile still sitting in the DB caches is most likely to be
read before it lands. Forcing depth surfaces to stay 2D-tiled fixes an individual
case and nine of the group; setting `tile_split`, `bankw`, `bankh` and `mtilea` for
1D surfaces as the reference r600 driver does unconditionally fixes nothing.

Two things about the barrier path came out of this. `terakan_barrier` consumes
`post_depth_stencil_image_copy_write_barrier_actions` on a transfer-stage barrier,
and **nothing in the driver was setting it** -- its
`VK_PIPELINE_STAGE_2_CLEAR_BIT` branch does emit the DB flush, but a Vulkan 1.0
barrier after `vkCmdClearDepthStencilImage` names `VK_PIPELINE_STAGE_TRANSFER_BIT`,
not that. The clear now sets it. That is correct by construction and it does not
move the numbers, so it is kept on the strength of the requirement rather than of
the test, as the query flush is.

And emitting the same DB flush unconditionally at the end of the clear instead
makes things dramatically worse -- 100 passing and 350 failing, close to inverted
-- so whatever is wrong is not simply a missing flush, and that flush is
destructive where it was tried.

Bumping the surface alignment for every depth image to 64KB changes nothing
either, which rules out the base address landing where DB cannot use it -- the
hypothesis the `core` and `dedicated_allocation` twins disagreeing suggested.

The remaining obstacle is that **none of this reproduces outside CTS.** A probe
that clears a depth image and copies it back was run at the failing extents with
the failing formats, with the image filled by an earlier clear, with the image
filled by a buffer copy from a zeroed staging buffer -- which is what CTS's
`preClearImage` does, and the only structural difference that was left -- and with
CTS's own barrier shapes. It passes every time. 96 of the 100 failures are stable
across two full runs, so they are not a coin flip either. Whatever CTS does that
makes them fail has not been identified, and until it is there is nothing to fix
against.

### The render area of a mip level with a depth attachment

`dEQP-VK.pipeline.monolithic.render_to_image` was run for the first time and failed
**422 of its 1245 supported cases**. The split was exact: every failure was a
`mipmap` case, and every `mipmap` case with a depth or stencil attachment failed,
while `small` and `huge` -- which are single-level -- passed 685 of 685, and the
colour-only `mipmap` cases passed too.

`vk_image_view::extent` is already the extent of the view's own mip level; the
common runtime sets it from `vk_image_mip_level_extent(image, base_mip_level)`.
The render pass minified it *again* by that same level when clamping the render
area to the depth attachment, so a pass targeting mip N was scissored to a 2^N-th
of the level and everything outside that corner was never drawn. Only the depth
path did this -- the colour one takes its bound from the descriptor's `DIM`, which
is built from the level, and was right.

The disabled depth test is what identified it. Forcing `Z_ENABLE` off did not make
the case pass, which ruled out the obvious reading that the depth buffer held
garbage and the test was rejecting fragments, and left the render area as the only
thing a depth attachment contributes to the colour path.

**`render_to_image` is now at 0 failures, from 422**, 1245 passing of 1245
supported. Across a 14459-case run over `render_to_image`,
`renderpass.suballocation.attachment_allocation` and sampled `api.image_clearing`
and `api.copy_and_blit`, this and the clear work in the same session take the
totals from 6103 passing / 716 failing to **6796 / 23**.

`terakan_render_area_mip` needs no shaders to see it: a load-op clear is itself
scissored by the render area, so a render pass that only clears its colour
attachment at mip 2 of a 64x64 image, with a depth attachment bound at the same
level, already exposes the whole bound. The level is filled with another value
first, so a partially cleared level cannot read as a fully cleared one. Against the
unfixed driver it reports the first unwritten texel at x = 4, which is 16 >> 2.

### More than one clip distance

`dEQP-VK.clipping` was run for the first time and failed 67 of its 100 supported
cases. `clipping.user_defined` accounted for 59 of them, and the split named the
defect on its own: `clip_distance.vert.1` passed and `.2` through `.8` failed,
`clip_distance_dynamic_index` passed at one and two and failed above, and every
`clip_cull_distance` case failed. Exactly one clip distance worked.

The registers were right -- `PA_CL_VS_OUT_CNTL` came out as `0x00400003` for two
distances, which is `CLIP_DIST_ENA_0|1` with `CCDIST0_VEC_ENA` -- and so was the
backend's own write mask. The assembly showed what was wrong:

```
Translate EXPORT POS 1 R1.z___
Translate EXPORT POS 2 R1._w__
```

Two position exports for the two distances of one array. The backend gives every
store to a position slot its own export and takes the next free slot each time, so
the second distance landed in POS2 -- `CCDIST1`, the slot for distances four to
seven, whose enable is not even set -- and had no effect. Under GL the array
arrives vectorized into a single store and the counter happens to be right; under
Vulkan `gl_ClipDistance` is a compact array and stays one store per element, which
neither `nir_opt_vectorize_io`, `nir_opt_vectorize_io_vars`,
`nir_lower_clip_cull_distance_to_vec4s` nor `nir_lower_clip_cull_distance_array_vars`
merges -- the last returns true while leaving the compact variable alone.

The slot now follows the location rather than the arrival order, so both stores
export to POS1 with complementary component masks and the hardware composes them.

**`clipping` is at 100 passing and 0 failing, from 33 / 67.** An 11145-case survey
across twenty groups shows 67 fixed and none broken.

`terakan_clip_distance` covered one distance, which is the only case that ever
worked; it now writes two, so only the quadrant where both are non-negative
survives. A driver honouring only the first leaves half the target drawn instead of
a quarter, and that is what it reports against the unfixed backend.

### Reading an input attachment

`dEQP-VK.renderpasses` was sampled for the first time and the failures concentrated
on one thing. Running
`renderpasses.renderpass1.suballocation.formats` in full -- 6450 cases -- gave 3789
passing and **2544 failing**, and every failure was an `input` attachment case:
`clear`, `load` and `dont_care` attachments passed 501 of 501 between them.

Within the input cases the split was exact. Every shape whose subpass draws while
reading an input attachment failed completely -- `draw`, `clear_draw`,
`draw_use_input_aspect` and `clear_draw_use_input_aspect` were 0 of 318 each --
while every shape that only clears passed in full, and the `self_dep` shapes sat at
exactly half.

`subpassLoad` carries no coordinate of its own: SPIR-V gives `OpImageRead` on
`SubpassData` a constant zero, and the fragment's own position is what it is meant
to read. `nir_lower_input_attachments` is what supplies that position, and the
driver was not running it -- so every input attachment read returned the texel at
(0, 0) for every fragment. It now runs, before
`nir_lower_sysvals_to_varyings`, so the `gl_FragCoord` access it introduces becomes
the varying the backend expects like any other.

**That subgroup is now at 6333 passing and 0 failing.** An 11145-case survey across
twenty groups went from 169 failures to 89, with none broken.

`terakan_input_attachment` fills the source attachment with a value that differs
per texel and in every channel, so a read that ignores the position produces one
value everywhere. Against the unfixed driver it reports `(1,2,3,255)` for every
texel -- the value at (0, 0) -- starting at texel (1, 0), because the first texel is
deliberately not special and a driver reading only (0, 0) still matches there.

### Line width

`PA_SU_LINE_CNTL` was emitted as a constant, with a `TODO: Expose line width
configuration` beside it, while `wideLines` was advertised as supported. Neither
the pipeline's `lineWidth` nor `vkCmdSetLineWidth` reached the hardware, so every
line came out one pixel wide.

`dEQP-VK.dynamic_state` -- 671 cases, small enough to run whole -- failed 13 of its
122 supported ones, and all thirteen were line width: the eight
`monolithic.line_width` shapes that switch between a static and a dynamic width,
`monolithic.rs_state.line_width`, and the four `monolithic.image` cases.

The register is now a settable entry rather than a constant, fed from the
pipeline's static width or from the dynamic state, the same way line stipple
already was. The field counts eighths of a pixel, which is why the old constant
read `1 << 3`.

**`dynamic_state` is now at 122 passing and 0 failing.** The 11145-case survey went
from 89 failures to 76, with none broken.

`terakan_line_width` draws a horizontal line and counts the covered rows, which is
the width. Width one is checked alongside width four, because the eighths encoding
makes a scaling mistake easy and a four arriving as thirty-two or as a half would
both show up. Against the unfixed driver both width-four cases report one row.

### Vertex attribute divisors that are not powers of two

The instance index is divided by the attribute divisor in the fetch shader, with
the multiply-high algorithm `util_compute_fast_udiv_info` describes. The
multiplication *is* the division -- the pre-shift, the increment and the post-shift
only condition its input and output -- and the driver requested each of those
surrounding operations while never requesting the multiplication itself. The index
came out shifted rather than divided.

Powers of two take a separate path that shifts on purpose, so they were right, and
that is why it went unnoticed. `dEQP-VK.draw.renderpass.instanced` passed all 96 of
its cases at each of the divisors 0, 1, 2 and 4, and failed all 96 at divisor 20:

```
divisor=0    96 pass    0 fail
divisor=1    96 pass    0 fail
divisor=2    96 pass    0 fail
divisor=4    96 pass    0 fail
divisor=20    0 pass   96 fail
```

**That group is now at 528 passing and 0 failing, from 431 / 97.**

`terakan_attrib_divisor` draws one line per instance at the instance's own row,
coloured from an attribute the divisor selects, so a wrong quotient shows as a row
holding another element's colour. Divisors one and two are checked alongside three
because they exercise the path that already worked: a change breaking division
outright rather than only the non-power-of-two case moves them too. Against the
unfixed driver instance 2 at divisor 3 reads element 1, which is `2 >> 1`.

### Depth and stencil resolve by minimum and maximum

`renderpasses.renderpass2.depth_stencil_resolve` was run in full -- 18003 cases --
and gave 3273 passing and **684 failing**. The tested aspect's resolve mode decides
it: `zero` passes 1080 of 1080 and `none` 630 of 630, while `min` fails 270 of 1050
and `max` 390 of 1050.

Every depth case returned the same value whatever the mode and the sample count:

```
min, 2 samples: got 0.0399939, expected 0.02
max, 2 samples: got 0.0399939, expected 0.04   <- passes
min, 4 samples: got 0.0399939, expected 0.02
max, 4 samples: got 0.0399939, expected 0.32
min, 8 samples: got 0.0399939, expected 0.02
max, 8 samples: got 0.0399939, expected 0.32
```

0.0399939 is one sample's depth. The two cases that passed -- depth `max` at two
samples, stencil `min` -- passed because that one sample happened to be the answer
there.

Two readings of this were recorded and both were wrong. The first blamed the reduce
programs' sample encoding: they issue a direct `SQ_TEX_INST_LD` with the sample index
in the coordinate's W, while `LowerTexToBackend::lower_txf_ms` emits a two-step form
on Evergreen. Rewriting the twelve programs as NIR with `txf_ms` produced exactly the
two-step assembly and changed nothing, so the rewrite was reverted. The second reading
concluded from that that per-sample depth fetch does not work on this hardware at all.

A probe settled it. Four full-screen triangles, each with `pSampleMask = 1 << i` and a
push-constant depth of `0.1 + 0.2 * i`, write four distinct depths into one 4-sample
`D32_SFLOAT` image; a compute shader reads them back with
`texelFetch(sampler2DMS, ivec2(0, 0), s)`:

```
sample  written  read
  0     0.100    0.100000
  1     0.300    0.300000
  2     0.500    0.500000
  3     0.700    0.700000
```

Per-sample depth fetch works, in both encodings, and the resolve shaders were never
the defect. **The defect is on the producing side: the driver never ran the CTS
fragment shader per sample.** The shader these cases use is

```glsl
float sampleIndex = float(gl_SampleID);
...
gl_FragDepth = value / 100.0;
```

and it does not set `sampleShadingEnable`. Section "Sample Shading" of the Vulkan
1.4.349 specification makes per-sample invocation mandatory anyway when the entry
point's interface includes `SampleId` or `SamplePosition`, but
`terakan_vk_pipeline_graphics_fragment_shading_init_with_rasterization` derived
`PS_ITER_SAMPLES` from `state->ms->sample_shading_enable` alone. The shader therefore
ran once per fragment, every sample of the fragment received sample 0's depth, and the
reduce then folded four identical values. 0.0399939 is sample 0's depth, which is why
it was the answer for every mode.

The fix reads `SYSTEM_VALUE_SAMPLE_ID` and `SYSTEM_VALUE_SAMPLE_POS` out of the NIR
into `terakan_shader_impl::fs::per_sample_invocation`, and forces
`db_eqaa_ps_iter_max_invocation_samples_log2` to 0 -- one sample per invocation -- once
the fragment shader has been translated. It has to happen there and not in the state
setup, because the fragment shading state is built from the create info before any
shader is compiled.

On a 984-case sample of the group's `min`/`max` cases this goes from 113 passing and
135 failing to **248 passing and 0 failing**. On a 2641-case sample across
`pipeline.monolithic.multisample`, `renderpasses`, `glsl.builtin_var` and `multiview`
it goes from 904/119 to **952/71**, with a per-case diff confirming 48 fixed and zero
regressed. The reverted NIR reduce programs were re-tested on top of the fix and are
still unnecessary: the hand-written ones give the identical 248/0.

The coverage gap that hid this is closed by `terakan_sample_id_depth`, which is the
probe turned into a driver test: one draw, no `sampleShadingEnable`, `gl_FragDepth`
written from `gl_SampleID`, and all four samples read back with
`texelFetch(sampler2DMS, ...)`. It covers both halves the resolve needs, unlike
`terakan_depth_msaa_fetch`, which clears its image to a single value and expects every
sample to equal it -- so it never distinguished samples and passed with the sample
index ignored. Against the unfixed driver `terakan_sample_id_depth` reads 0.1 from all
four samples, which is exactly the shape of the CTS failures.

### Integer colour resolve and the RTV metadata cache

`renderpasses.renderpass1.suballocation.multisample_resolve` was run in full -- 2600 cases --
and gave 678 passing and **330 failing**. The format axis was perfectly clean: every
`uint` and `sint` format failed and every unorm, snorm, srgb and float format passed,
at every sample count, resolve level and base layer. The message was always
"Different attachments were resolved to different values", and the resolved images
held whole 8x8 tiles of the pre-render contents in places that differed from one
attachment to the next.

Integer formats are exactly the ones that take the shader path:
`VK_RESOLVE_MODE_AVERAGE` is not what Vulkan asks for there, so `CB_RESOLVE` is
replaced by a draw that fetches sample zero. Forcing every format down the shader path
with a temporary environment variable reproduced the inconsistency on
`r8g8b8a8_unorm` too, which took the format out of the picture and left the path.

Adding barrier actions one bit at a time before the shader draw identified a single
one: **`FLUSH_INV_CB_RTV_META`**. The fixed function consumes the source through CB,
which keeps the RTV metadata coherent by itself; the shader samples the source as a
texture and bypasses CB, so FMASK and CMASK have to reach memory first. Without that,
the fetch decodes tiles against metadata as it stood before the render pass.

The flush is emitted only on the shader path. Every two-sample case in the group now
passes -- 330 failures down to **287** -- and on the 2641-case multisample and
renderpass sample the count goes 71 to **50**, 21 fixed with none regressed.

Four and eight samples still failed after that, now as "Resolve produced unexpected
values" for exactly the sample masks that include sample 1. A probe -- a 4-sample
`R8G8B8A8_UINT` attachment written under a `gl_SampleMask` push constant, read back
both per sample and through `vkCmdResolveImage` -- passed every mask, which excluded
the fetch, `gl_SampleMask`, the resolve shader, the image size and the render pass
resolve attachment in turn. The one thing it did not match was the image's usage: the
CTS creates its multisample images with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` and
nothing else. Dropping `VK_IMAGE_USAGE_SAMPLED_BIT` from the probe reproduced the
failure exactly, including masks 1, 3, 5 and 7 resolving to half render value and half
clear value.

The cause was already written down next to the code that caused it.
`terakan_image_create_color_descriptor` enables `COMPRESSION` and `FAST_CLEAR` for a
multisample colour image that is not sampled, because compressed CB writes leave the
planes the CB did not write untouched and a `TXF_MS` fetch reads them anyway. The
condition was the image's `SAMPLED` usage -- but an integer format is sampled whether
or not the application asks for it, since Vulkan resolves one by selecting a sample
and `CB_RESOLVE` can only average, so `terakan_CmdResolveImage2` fetches it as a
texture. A render pass resolve attachment reaches that path with no usage bit
involved.

Treating an integer number type as sampled takes the group to **1008 passing and 0
failing** of 2600, from 678/330 where it started. On the 2641-case multisample and
renderpass sample it goes 50 to **25**, 25 fixed with none regressed; everything left
there is `pipeline.monolithic.multisample`. The cost is the fast-clear and compression
bandwidth on integer multisample attachments only.

### The multisample depth and stencil clear rasterized single-sample

`pipeline.monolithic.multisample.misc` -- the `VK_EXT_multisampled_render_to_single_sampled`
test file run without the extension -- was run in full, 1390 cases, giving 505 passing
and **594 failing**. 522 of the failures are "Incorrect multisampled rendering for
stencil attachment" and the other 72 name the stencil attachment alongside colour
attachment 3. The cause was `vkCmdClearDepthStencilImage` rasterizing single-sample;
the route to it is kept below because seven plausible explanations were excluded first,
and each exclusion is a probe worth not repeating.

The format axis is absolute: `d16_unorm` passes 268 of 268, and every format with a
stencil aspect fails -- `d24_unorm_s8_uint` 81/205, `d32_sfloat_s8_uint` 75/198,
`s8_uint` 81/191. Crossing format with the resolve mode shows it is not a resolve
defect: the stencil formats fail at roughly the same rate with `ds_resolve_max`,
`ds_resolve_sample_zero`, and no depth/stencil resolve at all. The `basic` and
`input_attachments` subgroups fail every stencil case, and the deterministic
`.default` variants fail 144 of 144.

The resolved stencil image the test logs is noise -- 255, 21, 1, 0, 175, 239 and more
across one 65x55 image, where two values are expected.

Excluded, each by a probe that passes:

- Multisample stencil rendering and per-sample fetch. A 4-sample `S8_UINT` attachment
  written under a `gl_SampleMask` push constant with `VK_STENCIL_OP_REPLACE` reads back
  correctly for every mask through `texelFetch(usampler2DMS, ...)`.
- Single-sample stencil rendering and sampling, the same probe at one sample through
  `usampler2D`.
- The multisample stencil clear. With a clear value of 0x33 and partial coverage, the
  covered samples hold the reference and the uncovered ones hold 0x33.
- The image's usage flags, which is what the integer colour resolve turned on: this
  file creates its depth/stencil images with `SAMPLED`, `TRANSFER_SRC`, `TRANSFER_DST`,
  `DEPTH_STENCIL_ATTACHMENT` and `INPUT_ATTACHMENT`.
- The back-face stencil write mask left stale by the application. Setting
  `DB_STENCILREFMASK_BF` alongside the front mask in the resolve changes nothing.
- The depth/stencil resolve not running at all. A debug line added to
  `terakan_EndRendering` under `TERAKAN_DEBUG_RENDER` shows it running with the right
  aspect mask, modes and area.

- The stencil resolve itself in the shape these tests use. A probe with a colour
  attachment and a 4-sample `D32_SFLOAT_S8_UINT` attachment in one `vkCmdBeginRendering`
  pass, the fragment shader writing colour, `gl_FragDepth` and `gl_SampleMask`, the
  stencil resolved by `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` and read back as a `usampler2D`,
  gives the right value uniformly over the whole surface for every mask -- including at
  65x64, the odd non-aligned framebuffer size these tests use.
- Per-region scissored draws with their own stencil references, which is what `basic`
  does and what has no depth analogue. Four pipelines, one per quadrant, with references
  0x10, 0x20, 0x30 and 0x40 read back exactly those values in their quadrants.

- `VK_STENCIL_OP_INCREMENT_AND_CLAMP`, which is the operation these tests use and which
  none of the earlier probes did. Clearing to 0x33 and incrementing gives 0x34.

The one thing every one of those probes had in common was
`VK_ATTACHMENT_LOAD_OP_CLEAR`. These tests set `clearBeforeRenderPass`: they clear the
attachment with `vkCmdClearDepthStencilImage` and then use
`VK_ATTACHMENT_LOAD_OP_LOAD`, so that the area outside the render area can be verified
too. Adding that to the probe reproduced the failure immediately, and printing the
surface showed what it was:

```
stencil cleared to 0x33 = 51 before the pass, sample mask 0, nothing drawn
 51  51  51  51  51  51  51  54  54  54  54  54  54
 54  54  54  54  54  54  54  54  54  54  54  54  54
 51  51  51  51  51  51  51  54  54  54  54  54  54
 54  54  54  54  54  54  54  54  54  54  54  54  54
 ...
```

The clear reached one quarter of the 4-sample surface, in the tile-interleaved pattern
the samples are laid out in; the rest kept whatever was in memory.
`terakan_CmdClearDepthStencilImage` builds its
`terakan_meta_config_draw_begin_options` without `msaa_num_samples_log2`, so its
rectangle rasterizes single-sample whatever the image is -- unlike the colour clear and
the attachment clear, both of which set it from `image->vk.samples`.

Setting it takes `pipeline.monolithic.multisample.misc` from 505/594 to **1099 passing
and 0 failing** of 1390. `api.image_clearing` is unchanged -- a 15212-case sample still
fails the same 32, all of them the known single-sample small-extent ones -- and on the
2641-case multisample and renderpass sample the count goes 25 to **7**, 18 fixed with
none regressed.

`terakan_depth_stencil_clear_multisample` covers it. A render pass with no draws clears
the image to one value through its load operation, which does reach every sample;
`vkCmdClearDepthStencilImage` then clears it to another; and a compute shader reads
every sample of every texel through a `sampler2DMS` and a `usampler2DMS`. The load
operation is what makes the failure deterministic rather than a read of whatever was in
memory: against the unfixed driver the test reports **3072 of 4096 samples** still
holding the first value, which is exactly three quarters.

### Custom sample locations at one sample

A 2010-case run over `pipeline.monolithic.multisample`'s `sample_locations_ext`,
`std_sample_locations`, `min_sample_shading`, `multisample_shader_builtin` and
`sample_mask` gave 1130 passing and **214 failing**, and the sample count separates
them completely: `samples_1` fails 159 of 183 while `samples_2`, `samples_4` and
`samples_8` pass everything but one case each. The message is "Multisample pattern
doesn't seem to change between passes" -- the test renders twice with different
locations and gets the same picture.

The state reaches the hardware. A probe rasterizing a triangle whose right edge falls
at x = 4.25 of an 8-wide single-sample target, with the packet dumped as it was
emitted, shows `PA_SC_AA_CONFIG` carrying `MAX_SAMPLE_DIST(7)`,
`PA_SC_MODE_CNTL_0.MSAA_ENABLE` set -- the driver already sets it for one sample with
a non-centre location -- and `PA_SC_AA_SAMPLE_LOCS_0` holding `0x09`, which is the
correct encoding of x = 0.0625. The pixel at x = 4 stays uncovered anyway, and it stays
uncovered with the sample at 0.0625, 0.25, 0.5 and 0.9375 alike, in x and in y. The
rasterizer uses the pixel centre at `MSAA_NUM_SAMPLES = 0` and there is no other
register to move it with.

`VkPhysicalDeviceSampleLocationsPropertiesEXT::sampleLocationSampleCounts` is a bitmask
of the sample counts that support custom locations, so one sample is dropped from it.
The CTS throws `NotSupportedError` for a count that is not in it, and the group goes to
1106 passing and **52 failing**: 162 failures become not-supported, 24 cases that
passed become not-supported too -- they are one-sample cases whose locations happened
to land near the centre -- and nothing regresses.

### gl_FragCoord and input attachments under sample shading

That left 45 `min_sample_shading` failures of 60, rising with the fraction: `min_0_75`
and `min_1_0` failed every case. Two defects were behind them, and the CTS shader names
both:

```glsl
uint sampleId = gl_SampleID;
fragColor = vec4(fract(gl_FragCoord.xy), 0.0, 1.0);
```

**`gl_FragCoord` was the pixel centre in every invocation.** Section "Sample Shading"
of the Vulkan 1.4.349 specification puts `FragCoord` at the sample's position when the
shader runs per sample, and `SPI_PS_IN_CONTROL_0.POSITION_SAMPLE` is what selects that;
it was set only when the shader declared the position `sample`-qualified, which GLSL
cannot do for `gl_FragCoord`. A shader reading `SampleId` or `SamplePosition` always
runs per sample, so it now sets `POSITION_SAMPLE` too. A probe writing
`fract(gl_FragCoord.xy)` in sixteenths into a 4-sample attachment reads back
`(8, 8)` four times without the change and `(6, 2)`, `(14, 6)`, `(2, 10)`, `(10, 14)`
with it -- the standard four-sample pattern.

**A multisample input attachment was compressed.** These images are created with
`COLOR_ATTACHMENT | TRANSFER_SRC | INPUT_ATTACHMENT` and no `SAMPLED`, so
`terakan_image_create_color_descriptor` enabled `COMPRESSION` and `FAST_CLEAR` on them
-- but `nir_lower_input_attachments` turns `subpassLoad` into a texture fetch, and a
multisample one into `txf_ms`, which is exactly the plane-by-plane read compression
breaks. The CTS extracts each sample with `subpassLoad(imageMS, sampleId)`, and the
planes the CB never wrote came back as zero, which the test reports as "Got uncovered
pixel, where covered samples were expected". Treating `INPUT_ATTACHMENT` usage as
sampled, the way integer formats already are, removes all 12 of those.

Together those took the group from 45 failures to 29, all of them "Got less unique
colors than requested through minSampleShading" and still rising with the fraction.

The third one was in the shader above too, in what it does *not* do: `sampleId` is
never used, so NIR drops the `SampleId` read and `per_sample_invocation` comes out
false. The shader still runs per sample -- `minSampleShading` alone arranges that --
but the translation cannot see pipeline state, so `POSITION_SAMPLE` was not set. It is
now patched into `SPI_PS_IN_CONTROL_0` after the fragment shader is translated, next to
where the per-sample invocation itself is decided; that is safe because SPI supplies
the position in the same GPRs whichever location it is taken at, and the shader belongs
to one pipeline. **45 failures down to 8.**

The eight that remain are all two samples with `minSampleShading` at 0.75 or 1.0, and
they are hardware. At two samples `POSITION_SAMPLE` does not move `FragCoord`: the
probe reads `(8, 8)` from both samples with the standard locations and reads `(8, 8)`
from both again with custom locations placed at 0.0625 and 0.9375, while the same probe
at four samples returns the four standard positions exactly. The registers are right in
both cases -- `POSITION_ENA` and `POSITION_SAMPLE` set, `PA_SC_MODE_CNTL_1.PS_ITER_SAMPLE`
set, `PA_SC_AA_CONFIG` carrying `MSAA_NUM_SAMPLES = 1` and `MAX_SAMPLE_DIST = 4` -- and
the shader really does run per sample there, since `gl_SampleID` reads back 0 and 1.
Only the position stays at the centre.

Across the 2010-case sample-shading and sample-location run this is 52 failures down to
**15**, with 37 fixed and none regressed, and all fifteen are that same two-sample
behaviour: 8 `min_sample_shading` at two samples, and the other seven are
`sample_locations_ext.verify_interpolation.samples_2*` and
`std_sample_locations.verify_interpolation.samples_2_invariable`, which check that
interpolation happens at the sample and so need exactly the capability that is missing.
Four and eight samples pass every one of them. The 2641-case multisample and renderpass
sample stays at **0**, down from 119 where this work started.

### A GPU hang sampling three-channel 96bpp images

A stride sample of the whole case list -- every 380th of 3279369, 8630 cases -- aborted
after 1132 with `VK_ERROR_DEVICE_LOST`. The case it died on was
`pipeline.monolithic.image.suballocation.sampling_type.combined.view_type.1d_array.format.r32g32b32_sfloat.count_1.size.127x1_array_of_6`,
and it reproduces on its own every time. Slicing it:

| case | result |
|---|---|
| `1d_array` `r32g32b32_sfloat` `128x1_array_of_6` | device lost |
| `1d_array` `r32g32b32_sfloat` `1x1_array_of_6` | pass |
| `1d_array` `r32g32b32a32_sfloat` `128x1_array_of_6` | pass |
| `1d` `r32g32b32_sfloat` `128x1` | pass |
| `2d` `r32g32b32_sfloat` `32x32` | device lost |

So it is the three-channel 96-bit format, not the view type or the size.

`terascale_formats.py` disables `supports_sq_texture_fetch` for `3_3_2`, `10_11_11`,
`11_11_10`, `8_8_8` and `16_16_16` -- and did not disable it for `32_32_32`, the one
remaining member of the unpacked three-channel set. `FMT_32_32_32` is a vertex fetch
format, not a texture one; a three-channel unpacked format is one texel per three
surfels, so the surface's row is three times as wide in surfels as the descriptor's
texel pitch says, and the fetch walks the surface as something it is not. The comment
in `terakan_format.c` had already noticed the asymmetry from the other side --
"`r32g32b32_sfloat` -- the only one of the set the driver advertises as a sampled image
at all, the 8- and 16-bit ones having no texture fetch" -- without following it to the
hang.

Disabling it makes the class self-consistent, and the cases report
`NotSupported (Unsupported format for sampling: VK_FORMAT_R32G32B32_SFLOAT)`. The cost
is the five nearest-filter blits of that format that used to pass through the sampled
path; the expand-3x copy and clear paths are unaffected, since they address the surface
as raw surfels through their own buffer descriptors rather than as a texture.

The 8630-case survey now runs to the end: **877 passing, 15 failing**, and nothing
hangs. The largest cluster left in it is six `pipeline.monolithic.sampler` cases, and
the rest are singletons across `descriptorset_random`, `image.mutable`, `subgroups`,
`draw.dynamic_rendering` and `image.image_size`.

### Border colours on narrow integer formats

`pipeline.monolithic.sampler` was sampled at every sixth case -- 30340 of 182035 --
and gave 3404 passing and **406 failing**. Two causes account for all of them and
neither overlaps the other.

221 are `1d_unnormalized` and `2d_unnormalized`, which fail every case they run:
`S_03C000_FORCE_UNNORMALIZED` is only applied when the chip is r9xx, and nothing
normalizes the coordinates for anything older, so `unnormalizedCoordinates` silently
does nothing on Evergreen. The pipeline layout already tracks
`shader_immutable_samplers_unnormalized_coordinates` per stage, which is the shape a
NIR lowering would want, but no lowering exists. That one is still open.

The other 163 are all `clamp_to_border`, and the format axis is exact: every 8-bit and
16-bit `uint`/`sint` format fails, `a2r10g10b10_uint_pack32` fails, the stencil aspect
of every combined depth/stencil format fails, and every 32-bit integer format and every
normalized, float or packed format passes. The fixed
`SQ_TEX_BORDER_COLOR_OPAQUE_BLACK` and `_WHITE` deliver 1.0 as a float, which is what
`VK_BORDER_COLOR_FLOAT_OPAQUE_*` asks for; the integer variants have to deliver the
integer 1, and at 8 and 16 bits the fixed white reads back as something else.

`terakan_hw_config_sqk` already emits the per-sampler border colour registers for the
`REGISTER` type, so pointing `VK_BORDER_COLOR_INT_OPAQUE_BLACK` and `_WHITE` at them
with `{0, 0, 0, 1}` and `{1, 1, 1, 1}` written out is all the sampler needed. Doing that
**lost the device**: it was the first time the path had ever been reached, and it
emitted `PKT3_SET_CTL_CONST` with `TERAKAN_CTL_CONST_OFFSET(0x00A400)`, which subtracts
the control-constant base of 0x3CFF0 from a smaller address and underflows. These are
configuration registers -- Gallium r600 emits them with `radeon_set_config_reg_seq` --
and with the packet corrected to `PKT3_SET_CONFIG_REG` the same run gives **1044
passing and 0 failing** of the 3030 `clamp_to_border` cases, from 163 failing.

Over the whole 30340-case sample this is 406 failures down to **243**, 163 fixed with
none regressed, and every one of the 243 is an unnormalized-coordinate case.

Custom border colours are still not implemented. `VK_EXT_custom_border_color` also needs
the value converted into the view's format and swizzle before it reaches the register,
which is what `evergreen_convert_border_color` does in Gallium r600 and which the
sampler alone cannot do, since it does not know the view.

### Image format properties ignored the requested usage

A second stride sample -- every 120th case, wsi excluded -- aborted again, this time
with a segmentation fault rather than a lost device, on
`texture.swizzle.component_mapping.color.r8g8b8a8_sscaled_2d_pot_rgba`. The backtrace
is inside the CTS, in `vk::createShaderModule` reading a program binary that was never
built, and radv reports the same case `NotSupported` and skips it. So the question was
what Terakan says that radv does not.

`vkGetPhysicalDeviceFormatProperties` agrees with the intent of the code: `SSCALED` and
`USCALED` report `TRANSFER_SRC | TRANSFER_DST` and nothing else, which is the
deliberate, measured reduction recorded next to it -- copying them works because a copy
moves bits without interpreting them, and everything that interprets the value fails.
radv reports no image features at all for them.

`vkGetPhysicalDeviceImageFormatProperties` was the difference. It rejected a format
whose feature mask was empty, and then never checked the mask against the **usage** that
was asked for, which the specification requires: each usage names the format feature it
depends on. Querying `R8G8B8A8_SSCALED` with `VK_IMAGE_USAGE_SAMPLED_BIT` returned
success, so the CTS built the image, went on to ask for a shader that was never
generated for it, and took the run down.

Every usage that names a feature is now checked: transfer source and destination,
sampled, storage, colour attachment, depth/stencil attachment, and input attachment,
which needs either the colour or the depth/stencil attachment feature rather than a
single one. `TRANSIENT_ATTACHMENT` names none and is left alone. The case now reports
`NotSupported (Format not supported: VK_FORMAT_R8G8B8A8_SSCALED)`, and
`api.info.unsupported_image_usage` -- which checks exactly this correspondence and had
been failing `linear.input_attachment_a2r10g10b10_sscaled_pack32` -- is clean. Over
`api.info.unsupported_image_usage` and `api.info.image_format_properties*` together,
4592 cases give 4210 passing and no driver failure; the twelve that are not passing are
`InternalError (Unknown image format)` from the CTS itself on multi-planar YCbCr
formats.

The first version of the check regressed ten
`image.extended_usage_bit_compatibility.image_format_properties*` cases, which is the
rule it had missed: `VK_IMAGE_CREATE_EXTENDED_USAGE_BIT` says the usage is deliberately
not required to be supported by the image's own format, because a view of a compatible
format provides it, and it is what `BLOCK_TEXEL_VIEW_COMPATIBLE` is used with. Exempting
it takes the regressions to zero.

That group also moves some cases from passing to not-supported, and they were passing
vacuously. It scans for a compatible view format that supports the usage with no flags
and skips when none does; while every format claimed every usage, one was always found.
The compatible set of a block-compressed format is other block-compressed formats, none
of which supports storage here, so those now skip -- which is the test's own gate
working on accurate data.

The 27033-case survey, which had been aborting, now runs to the end, and the three
versions of the check read 62, 52 and finally **51 failing** of 2874 passing, with no
case going from passing to failing at any step.

### A render pass resolve used the image's format, not the view's

`dEQP-VK.image.mutable` was run in full -- 10118 cases -- and gave 9584 passing and
**534 failing**. Two axes separate them completely. Every failure is a
`draw_copy_resolve` variant, and within those the format pair decides it:

| image format | view format | pass | fail |
|---|---|---:|---:|
| integer | integer | 160 | 104 |
| integer | normalized or float | 146 | 202 |
| normalized or float | integer | 120 | 228 |
| normalized or float | normalized or float | 348 | 0 |

An integer format anywhere in the pair fails; neither being integer always passes. That
is the resolve's own split -- `CB_RESOLVE` averages, Vulkan resolves an integer format
by selecting a sample, so integer formats take the shader path -- and the group is
exactly the one that renders through a *view* whose format differs from the image's.

`terakan_EndRendering` recorded only the two `VkImage` handles for a colour resolve and
called `terakan_CmdResolveImage2`, which reads the number type from
`image->format_info`. Vulkan resolves according to the attachment's view, so a `unorm`
image rendered through a `uint` view was averaged and a `uint` image rendered through a
`unorm` view was sample-selected, each the opposite of what was asked for.

The resolve now takes the two view formats, with `VK_FORMAT_UNDEFINED` meaning the
image's own -- which is what `vkCmdResolveImage` passes, since it resolves images and
has no view. **`image.mutable` goes to 10118 of 10118 passing.**

### The signed 2-bit-alpha 10_10_10 formats, the integer half

The exclusion recorded for `a2r10g10b10_snorm_pack32` and `a2b10g10r10_snorm_pack32` --
a two-bit signed channel is degenerate and the family does not survive the texture path
-- had only ever named the `SNORM` members. The `SINT` ones are the same family and were
still advertised in full.

Over the 3264 `pipeline.monolithic.image` cases of the `a2*_pack32` formats,
`a2r10g10b10_sint_pack32` passed 10 and **failed 194**, while
`a2r10g10b10_uint_pack32` and `a2r10g10b10_unorm_pack32` passed all 204 of theirs with
no failures. radv zeroes the image features of both `SINT` members exactly as it does
the `SNORM` ones.

Adding them to the exclusion takes the group to **0 failing**; the ten that had passed
become not-supported. Only the image features are withheld: the texel buffer half was
measured before assuming, and `a2r10g10b10_sint_pack32` passes all five of its
`api.buffer_view` uniform texel buffer cases, so it keeps them, as it keeps vertex
buffer support.

### Multisample depth and stencil copying

`dEQP-VK.api.copy_and_blit.core.depth_stencil_msaa_copy` was run in full -- 432 cases,
216 of them supported -- and **every one of the 216 failed**. `vkCmdCopyImage` of a
multisample depth or stencil aspect had no path at all: the colour fast path requires a
colour aspect and the meta draw below it cannot do multisample.

A depth or stencil aspect is a plane of its own, with its own offset, tiling and slice
size, so copying one aspect's slices moves exactly that aspect and leaves the other
where it was. That is what makes a byte copy safe here where the colour path needs the
whole surface: there it has to carry FMASK and CMASK along with the samples they
describe, and here there is no depth metadata at all, since Terakan does not implement
HTILE. The samples of a slice are interleaved inside it, so a slice-sized copy carries
all of them without needing to know how.

Two things came out of building it, both worth not rediscovering.

A level's offset is already measured from the image's base, with the aspect's offset
folded into it by `terakan_image_surface_aspect_compute`. Adding the aspect offset again
put the stencil plane of a 64x64 `d16_unorm_s8_uint` image at 0x8000 in a 0x6000-byte
surface and **lost the device**.

And a tiled slice's layout depends on its index, so the bytes of one slice do not decode
as another. Copying layer 2 into layer 3 of a 2D-tiled 64x64x5 `d32_sfloat` image runs
and then reads back the destination's previous contents. The path is restricted to
copies that stay on the same layer index; without that it would silently write wrong
data for a legal operation, which is worse than not handling it.

**216 failures down to 72.** All 144 `whole` cases pass; `partial`, which needs a
sub-rectangle no byte copy can express, and `array_to_array`, which is exactly the
layer-index case, remain and need the meta draw the `TODO` in
`terakan_meta_copy_image.c` describes. A 7562-case sample of `resolve_image` and
`image_to_image` is unchanged at 4 failures, all of them the known offset-resolve
residue.

### gl_VertexIndex counted the base twice

`dEQP-VK.draw.*.indexed_draw` failed **every one of its 144 supported cases**, in both the
render pass and the dynamic rendering variants, including the plainest
`draw_indexed_triangle_list`. The stride sample of the whole suite had only ever caught
three of them.

The images say what it is. The geometry is right -- a 76x76 square where the reference
has 77x77, one pixel of edge -- and the colour is wrong: red where blue is expected. The
test's vertex shader explains itself:

```glsl
gl_Position = in_position;
if (gl_VertexIndex == in_refVertexIndex)
    out_color = in_color;   // blue
else
    out_color = vec4(1.0, 0.0, 0.0, 1.0);
```

Every vertex is blue and every pixel came out red, so `gl_VertexIndex` never matched the
vertex it belonged to.

Terakan puts the draw's base into `VGT_INDX_OFFSET` and fetches attributes with
`SQ_VTX_FETCH_NO_INDEX_OFFSET`, which is the choice `terakan_vertex_input.c` documents:
"In Vulkan and OpenGL, `gl_VertexIndex` or `gl_VertexID` includes the base, so it's more
straightforward to use `VGT_INDX_OFFSET`". R0.X therefore already carries the base, and
SFN returns R0.X for `load_vertex_id`.

`terakan_shader_nir_options_init` set `vertex_id_zero_based` for Evergreen and newer,
copying classic r600 -- which sets it because OpenGL's `gl_VertexID` is zero-based and it
fetches with `SQ_VTX_FETCH_VERTEX_DATA` instead. With it set, `nir_lower_system_values`
rewrites `load_vertex_id` into `load_vertex_id_zero_base + load_first_vertex`, and
`load_first_vertex` is the driver constant holding the same base. The base was added a
second time.

Clearing it takes the group to **144 of 144 passing**. `terakan_shader_generation_test`
asserted the old value and is corrected with it, which is what caught the flag being
generation-dependent rather than simply wrong.

After the fix the whole `dEQP-VK.draw` group stands at 3362 passing and 107 failing of
29392; there is no clean before-figure for the whole group, because that baseline run was
abandoned once it became clear it would cost more than it was worth. The 480-case
`indexed_draw` list was measured both ways and is the evidence, and a 27033-case stride
survey goes 36 failures to **32** with none new.


### Indirect draws whose parameters a compute shader writes

Of the 107 failures left in `dEQP-VK.draw` after the vertex index fix, **79 are
`indirect_draw.sequential_data_from_compute` and `indirect_draw.indexed_data_from_compute`**
-- 29 in the render pass variants and 50 across the three command buffer ones. The
remaining 28 are 12 `shader_draw_parameters`, 9 `implicit_sample_shading` and a few
others.

The mechanism is certain from the code alone. `terakan_vk_cmd_draw_indirect` reads the
indirect buffer through `buffer->bo->mapping` at **record** time, to pull `firstVertex`
and `firstInstance` out and put them in `VGT_INDX_OFFSET`, `SQ_VTX_START_INST_LOC` and
the driver constants. These tests record a compute dispatch that fills that same buffer
and then the draw; the dispatch runs at **submit** time, so the read is guaranteed to
see whatever was in the buffer beforehand.

The fix is not a small one, because it runs into why the CPU read is there at all.
Evergreen's `DRAW_INDIRECT` and `DRAW_INDEX_INDIRECT` make the command processor load
`SQ_VTX_BASE_VTX_LOC` and `SQ_VTX_START_INST_LOC` from the buffer by itself -- but
Terakan deliberately does not use those. It puts the base in `VGT_INDX_OFFSET` and
fetches with `SQ_VTX_FETCH_NO_INDEX_OFFSET` so that R0.X *is* `gl_VertexIndex`, which is
what Vulkan wants and what the entry above turned out to depend on. `VGT_INDX_OFFSET` is
a context register the command processor does not load from memory, and Evergreen has no
packet to copy a dword from memory into one.

So an indirect draw wants the other arrangement: fetch with `SQ_VTX_FETCH_VERTEX_DATA`
so the hardware-loaded base reaches attribute fetching, leave `VGT_INDX_OFFSET` at zero,
and have the shader add the base to R0.X for `gl_VertexIndex` -- which is exactly what
`vertex_id_zero_based` produces, with the base reaching the shader through a CP DMA of
those two dwords from the indirect buffer into the push constant buffer. That is two
fetch shader variants and a per-draw copy, chosen by whether the draw is indirect. It is
a design fork rather than a patch, and it is the next thing this group needs.

### The logic op applied to formats it does not apply to

`pipeline.monolithic.logic_op_na_formats` failed **228 of its 256 supported cases**,
while `pipeline.monolithic.logic_op` passed all 160 of its. The split is the group's own
name: the failing formats are every float and sRGB one -- `r16g16_sfloat`,
`r16g16b16a16_sfloat`, `r32g32_sfloat`, `r32g32b32a32_sfloat`, `r8g8b8a8_srgb`,
`b8g8r8a8_srgb`, `r16_sfloat`, `r32_sfloat` -- and the passing group is the integer ones.

Section "Logical Operations" of the Vulkan 1.4.349 specification says they "are applied
only for signed and unsigned integer and normalized integer framebuffers" and "are not
applied to floating-point or sRGB format color attachments". The pipeline set
`CB_COLOR_CONTROL.ROP3` from `logicOpEnable` alone.

`ROP3` is one field for the whole colour block, so this can only be decided per draw.
It is now switched off when no enabled attachment is a format the operation applies to,
which is exactly the measured case. A framebuffer mixing an integer attachment with a
float one keeps the operation, as it did before: the hardware cannot apply it per
attachment, and switching it off there would take it away from the attachments that are
entitled to it, so mixed framebuffers are left no worse than they were.

**228 failures to 0**, with the 160 integer cases still passing.

### The descriptor set layout support query answered for itself

`dEQP-VK.api.maintenance3_check` failed **143 of its 178 supported cases**. Two separate
things were wrong with `terakan_GetDescriptorSetLayoutSupport`, and the group separates
them cleanly.

It never wrote `VkDescriptorSetVariableDescriptorCountLayoutSupport`. The structure kept
whatever the caller had left in it, which the CTS reports as "Nonzero
maxVariableDescriptorCount when using no variable descriptor counts". The specification
says the field is zero when the layout has no variable-sized binding and otherwise holds
the largest count that binding may be given.

And `supported` was decided by a total-descriptor bound of the query's own -- 128 --
which the layout creation path knows nothing about. Creation validates each binding
against the hardware's register spaces, so the two disagreed: the
`support_count_*_create_layout` cases build the layout the query called supported and
create it, and got `VK_ERROR_VALIDATION_FAILED`.

Both are answered by asking creation. `supported` is now whether
`terakan_CreateDescriptorSetLayout` accepts the layout, which cannot disagree with
creation because it is creation; and `maxVariableDescriptorCount` is found by binary
search over the variable binding's count, bounded by the device's own
`maxPerSetDescriptors` -- about seven throwaway layouts for a query that is not on any
hot path. The test sets the binding to exactly the reported count and creates it, so a
value creation would reject is not an answer.

**143 failures to 0.**

### imageSize on a storage image

`dEQP-VK.image.image_size` failed **every one of its 96 supported cases**, and not by
producing a wrong number: the compute pipeline failed to create. With shader debugging on,
SFN says `Unsupported instruction: deref_var (image image2D)` -- nothing lowered
`image_deref_size`, so the image deref reached the backend.

A storage image is bound as an SQ resource as well as a UAV, which the format code calls
out size queries as one of the reasons for, so `imageSize` is the same
`TEX_GET_TEXTURE_RESINFO` the sampled path already uses. The binding lowering now builds
that texture instruction for `image_deref_size` and `image_deref_samples`, resolving the
binding exactly as it resolves one for an ordinary texture instruction. **96 failures to
24.**

The 24 left split by view type, and the split names the second defect: `1d_array` and
`2d_array` failed exactly their one-layer cases -- `1x1`, `7x1`, `1x1x1`, `7x1x1` -- and
passed every multi-layer one, reporting a layer count of **zero**.
`terakan_image_create_resource_descriptor` described a view of one array layer as a
non-array image, on the grounds that the addressing is the same. It is not the same to a
size query: the hardware reports a layer count only for an array descriptor. Keeping the
array dimensionality the caller asked for takes it to **12**.

The last 12 are `cube_array`, which report zero layers for a different reason again. Their
descriptor is `SQ_TEX_DIM_CUBEMAP`, and the hardware does not report a layer count for a
cube either. SFN knows this -- its `txs` leaves the third component unwritten for a cube
array and reads the count from Gallium's texture-info constant buffer, which Terakan does
not have -- and relabelling the query as a 2D array does not help, because the descriptor
is still a cube. A storage cube array does not need cube addressing at all, so the way out
is a second resource descriptor for it, shaped as a 2D array of six slices per cube; that
is a change to the image view rather than to the query, and it is what these need.

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
