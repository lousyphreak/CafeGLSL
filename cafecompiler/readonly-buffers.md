# Read-only graphics buffer ABI

CafeGLSL accepts `readonly` storage blocks in vertex and pixel shaders. The
implemented access is a DWORD-aligned scalar 32-bit load with a statically known
block binding. Writes, atomics, runtime array length, dynamically selected blocks,
and other load widths are unsupported. This is a buffer-fetch implementation;
it does not bind GX2's single global shader export buffer.

Bindings 0–15 use the same per-stage resource bank as textures. A texture and a
read-only buffer cannot occupy the same binding in one shader. Samplers without
an explicit binding are allocated around the buffer bindings. Storage-buffer
members are not reflected as uniforms. Callers must retain the declared buffer
bindings.

The caller binds an R700 **valid buffer** descriptor at the shader binding:

| Descriptor word | Value |
| --- | --- |
| 0 | Physical byte address of the bound range |
| 1 | Range size in bytes minus one |
| 2 | `STRIDE=4`, `DATA_FORMAT=32` (13), `NUM_FORMAT_ALL=INT` (1), unsigned components, endian mode below |
| 3 | Memory request size (GX2 uniform buffers use 1) |
| 4, 5 | 0 |
| 6 | `TYPE=VALID_BUFFER` (3 in bits 31:30) |

Word 2 uses the R700 fields: stride in bits 18:8, format in 25:20,
number format in 27:26, signedness in bit 28, and endian swap in 31:30.
The descriptor is stage-local: pixel resource register offset `7 * binding`,
vertex resource register offset `0x460 + 7 * binding`, relative to
`SQ_TEX_RESOURCE_WORD0` in a `SET_RESOURCE` packet.

The compiler emits `VFETCH` in a TEX clause, no vertex/instance index offset,
`USE_CONST_FIELDS=1`, and a four-byte mega-fetch. It converts the GLSL byte offset
to a DWORD index. The resource's four-byte stride supplies the final byte
address. The resource size supplies the native fetch bounds. No CPU mirror,
per-draw repacking, global export-memory rebinding, or CPU/GPU wait is involved.

## Byte order

* Ordinary native big-endian vertex words and 32-bit indices: `ENDIAN_8IN32` (2).
  A float's original 32-bit representation reaches `uintBitsToFloat` unchanged.
* Native big-endian 16-bit indices: `ENDIAN_8IN16` (1). A memory pair containing
  indices `0x1234, 0x5678` reaches the shader as `0x56781234`. Thus the existing
  `word & 0xffff` / `word >> 16` operations select consecutive indices in order.

Endianness belongs to the bound descriptor, not the shader or the buffer slot.
The same compiled shader can read either type without recompilation. Byte ranges
must follow the declared DWORD load alignment; the shader must only issue reads
inside its bound range. A final packed index word therefore needs four bytes in
the supplied range.

CPU-written data must already be published before the draw. Invalidate the GPU
texture cache for the buffer range before fetches, including after GPU writes.
Retain/touch the original allocation through normal draw retirement; descriptor
binding does not change allocation ownership.

The R700 ISA's `VTX_WORD2` specifies that instruction endian control is ignored
when `USE_CONST_FIELDS` is set, enabling the per-resource byte-order selection:
[AMD R700 ISA](https://www.amd.com/system/files/TechDocs/R700-Family_Instruction_Set_Architecture.pdf).
