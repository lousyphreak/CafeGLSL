# CafeGLSL - Shader Compiler for Wii U

CafeGLSL is a GLSL shader compiler for the Wii U, built on Mesa. You can use it to compile shaders on the fly inside your application, or compile them ahead of time on Windows, Linux or macOS.

**Why make this?**

By default, Wii U offers no way to compile shaders dynamically during runtime, yet many games, render API translation layers and other dependencies rely on this. This project is intended to fill this void.

## Current limitations

- No support for geometry or tessellation shaders.
- Native compute maps independent logical workgroups onto 64-lane hardware waves. Shared LDS lowering, global atomics and in-dispatch export reads are unsupported; oversized groups containing barriers or subgroup communication cannot be split.
- Graphics read-only storage buffers support scalar, aligned 32-bit loads with static bindings. See the [native buffer binding and byte-order ABI](cafecompiler/readonly-buffers.md); writes, atomics, and runtime length queries are unsupported.
- Wii U limitation: GX2 uses one uniform mode for both the vertex and pixel shader in each draw. You must select that mode before drawing. See [Important: choose loose uniforms or uniform blocks](#important-choose-loose-uniforms-or-uniform-blocks) below.

## Native compute library API

`CompileComputeShader(source, &result, log, sizeof(log), flags)` returns
`GX2_SHADER_MODE_COMPUTE_SHADER`, or `GLSL_SHADER_MODE_ERROR` with diagnostics.
The result is a `GLSLComputeShader`; bind its `shader` member with
`GX2SetComputeShader`. Release it with `FreeComputeShader` after GPU use ends.
The static API is available on host and Wii U. The standalone CLI accepts
vertex/pixel stages.

Compute always uses uniform blocks. Loose uniforms retain the existing block-zero
reflection and initial-value ABI. Block 15 is reserved for a 512-byte resource
layout table. For each binding 0..15, upload two little-endian `uvec4` values:

```
{ exportBaseDword, sizeDwords, rowPitchDwords, layerPitchDwords }
{ width, height, layers, 0 }
```

All writable resources use disjoint ranges of one native global export
allocation. Bind it with `GX2SetShaderExportBuffer`; repeated setter calls cannot
bind independent allocations simultaneously. Readonly SSBOs use the existing
DWORD VFETCH descriptors in the vertex texture bank. Compute vector buffer loads
are split into scalar fetches. `imageLoad` uses a readonly sampled texture alias
at the declared image binding. Publish a prior export and materialize its image
before binding that alias. Arbitrary read/write sharing within a kernel needs
uncached `READ_MEM` lowering and is rejected.

`result->resources` reflects each active buffer/image's binding, kind, format,
export element byte size and read/write use. Ordinary samplers remain in
`shader.samplerVars`. The caller owns layout dimensions, pitches, export ranges,
resource publication, and lifetime through normal GPU retirement.

RGBA32F/UI and R32F/UI export their native DWORDs. RGBA16F exports two packed
DWORDs per texel; half conversion is GPU integer code with round-to-nearest-even,
subnormal and infinity/NaN handling. RGBA8 exports one packed DWORD. R16F, R8 and
RG8 export a quantized float4 (16 bytes per texel) to avoid adjacent lanes racing
over sub-DWORD writes. These widened images require GPU format conversion at
each imageStore boundary. Packed images need a matching linear surface layout
before copying or sampling; tiled textures cannot alias arbitrary export bytes.
Image stores outside their declared dimensions are ignored.

The native invocation input is the flat dispatch work-item index in `R0.x`.
The descriptor retains the shader's logical `workGroupSizeX/Y/Z`. Compute
`F = ceil(workGroupSizeX * workGroupSizeY * workGroupSizeZ / 64)` and pass native
`GX2DispatchParams` counts `{logicalGroupCountX * F, logicalGroupCountY,
logicalGroupCountZ}`. Counts are still native big-endian. No extra metadata or
uniform supplies the logical dispatch dimensions: the shader fetches the native
dispatch descriptor at vertex resource 147 (SET_RESOURCE offset 0x865), divides
its X count by compile-time F, and retains its Y/Z counts.

The logical local index is `R0.x % (64 * F)` and the logical linear group index
is `R0.x / (64 * F)`. The shader expands local XYZ with its original declared
layout, expands group XYZ with the recovered logical dispatch dimensions, and
computes global IDs from those two values. Padding lanes with local index at
least the logical group size execute no shader work. This includes partial
waves and 1x1x1 groups. Groups of 64 retain the original mapping. Logical
16x16x1 groups use F=4; 64x1x1 and 8x8x1 groups use F=1.
Shared memory, barriers, subgroup communication and global
atomics are not made usable across physical waves by this mapping.

The native dispatch counts launch `X * Y * Z * 64` work-items.
`workGroupSizeX/Y/Z` and `over64Mode` do not change that launch size.
Compute stores use R700 MEM_EXPORT with DWORD indices and valid-pixel
predication.

Ordinary 2D/array gathers, including a single gather offset, lower to four
base-LOD texel-center samples in GLSL gather component order. Retain base-LOD
sampler limits for these gathers. Shadow, dynamic sampler-array selection,
explicit-LOD and four-offset gathers are diagnosed as unsupported.

`SerializeGFD` accepts `GFDFile::computeShaders` and writes native GFD 7.1 blocks
14/15 with the 0x84-byte target descriptor and the existing relocation format.
WUT's graphics GFD loader does not load these blocks; consumers must implement a
compute loader. Preserve the complete register image stored in the descriptor.
`over64Mode` and `wavesPerSimd` remain zero.
`GX2SetComputeShader` also installs each reflected loop constant in the shared
vertex/compute loop bank. CafeGLSL supplies `{offset=0, value=0x01000fff}`; preserve
that reflection when loading GFD. No duplicate raw loop programming is needed;
the native setter does not invent this default for a shader with no loop entries.

Compilation does not publish GPU caches. On Wii U, publish code with CPU_SHADER,
uniform/layout data with CPU|UNIFORM_BLOCK, and dispatch parameters with CPU
invalidation. Mode entry/exit, export publication and conversion, and graphics
state restoration belong to the renderer. Successful offline compilation is not
hardware execution validation.

## How to use

There are two ways to use this compiler.

If you need to compile shaders dynamically at runtime on the Wii U, use the
static library. You can also embed the desktop library in your own tools.

However, if you can compile all required shaders on your PC (as part of your build process, for example),
you can also use the standalone compiler to compile to .gsh files.

### Statically compile shaders to .gsh on PC (Windows, Linux or macOS):

#### Compilation:

1. Download a [precompiled release](https://github.com/Exzap/CafeGLSL/releases), [compile it yourself](BUILDING.md), or use [Docker](#docker).
2. Use the binary in `bin/` to compile your shaders to .gsh files. On Windows, use `glslcompiler.exe`.

```bash
# Compile as a pair: both use uniform block mode if either needs it
bin/glslcompiler -vs shader.vert -ps shader.frag -o shaders.gsh

# Compile single and multiple shaders separately: their uniform modes may differ
bin/glslcompiler -vs shader.vert -ps shader.frag -vs shader2.vert -ps shader2.frag -o shaders.gsh
```

Options:

```text
-vs <file>  compile a vertex shader
-ps <file>  compile a pixel shader
-o  <file>  write the results to a GFD (.gsh) file
-v          print the R600 disassembly

--uniform-mode <auto|register|block>
            auto      use registers when possible, blocks when needed (default)
            register  use registers, fail if blocks are needed
            block     use blocks, moving loose uniforms into a block
```

The first example shows the recommended way to compile shaders, since it guarantees that both will use the same uniform mode.
With the default `auto` mode, if one needs uniform blocks, the other uses block mode too.
The compiler warns if this moves loose uniforms out of registers, as you'll need to upload them at binding 0 using `GX2Set*UniformBlock` instead of `GX2Set*UniformReg`.

Only one vertex shader and one pixel shader compile as a pair. All other combinations compile separately, as in the second example.
When combining separately compiled shaders, make sure both use the same uniform mode.

#### Usage:

The output will be a .gsh file that you can use in your Wii U project.
Load a vertex and pixel shader pair with the WHB library:

```cpp
WHBGfxShaderGroup shaderGroup = {};
WHBGfxLoadGFDShaderGroup(&shaderGroup, 0, shaderFileBuffer.data());
```

Use `WHBGfxLoadGFDVertexShader()` and `WHBGfxLoadGFDPixelShader()` to load shaders
individually. The first argument selects the shader of that type, starting at 0.

The CLI prints the mode once for a pair, or once per shader in input order when compiled separately. For example:

```text
bin/glslcompiler.exe -vs shader.vert -ps shader.frag -o shaders.gsh
GX2_SHADER_MODE_UNIFORM_BLOCK
```

This means you'll need to call `GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK)` before drawing with these shaders.
See [Important: choose loose uniforms or uniform blocks](#important-choose-loose-uniforms-or-uniform-blocks) below,
especially if you compile the shaders separately.

### Dynamically compile shaders at runtime on the Wii U (static library):

#### Installation:

The Wii U release is laid out as a devkitPro portlib, so it can be extracted straight into the Wii U portlibs directory:

```bash
tar -xf <this-archive> -C /opt/devkitpro/portlibs/wiiu --strip-components=1
```

For applications built in Docker, see the [Wii U library image](#wii-u-library).

Then link it with CMake:

```cmake
find_package(cafeglsl REQUIRED)
target_link_libraries(myapp PRIVATE cafeglsl::cafeglsl)
```

With pkg-config:

```bash
powerpc-eabi-pkg-config --static --cflags --libs cafeglsl
```

The final link needs `-Wl,--gc-sections`. wut.specs already passes it, and the CMake package sets it as well.

#### Compilation & Usage:

```cpp
#include <cafeglsl/CafeGLSLCompiler.h>
```

Call `InitGLSLCompiler()` once, then `CompileShaderPair()` (or `CompileVertexShader()` and `CompilePixelShader()` separately) with the GLSL source.
These give you the `GX2VertexShader` and `GX2PixelShader` structs GX2 expects when drawing.
You can also save them to a .gsh file using `<cafeglsl/gfd.h>` and load them on later runs.

Call `DestroyGLSLCompiler()` when you no longer need the compiler.

We recommend compiling the vertex and pixel shaders together so they use the same shader mode:

```cpp
char log[4096];
GX2VertexShader *vertex = NULL;
GX2PixelShader *pixel = NULL;
GX2ShaderMode mode = CompileShaderPair(
    vertexSource, pixelSource, GLSL_COMPILE_AUTO, &vertex, &pixel,
    log, sizeof(log), GLSL_COMPILER_FLAG_NONE
);
```

The return value tells you which mode to use with `GX2SetShaderMode()` on the Wii U.
If compilation fails, it returns `GLSL_SHADER_MODE_ERROR` instead and writes the error message to `log`.
The log can also contain warnings when compilation succeeds.
See [Important: choose loose uniforms or uniform blocks](#important-choose-loose-uniforms-or-uniform-blocks) below for how the mode is chosen.

You can also compile shaders separately with `CompileVertexShader()` and `CompilePixelShader()`.
They take the same mode option and return the mode of that shader.
Free shaders when you are done with `FreeShaders(vertex, pixel)`, or use `FreeVertexShader()` and `FreePixelShader()` separately.

### Embed the compiler in a desktop application:

The desktop release includes `lib/libcafeglsl.a` and headers in
`include/cafeglsl/`. It uses the same compile functions as the Wii U library.
Both host and Wii U releases include CMake and pkg-config metadata with their
configured dependencies. Use `find_package(cafeglsl REQUIRED)` and link
`cafeglsl::cafeglsl`, or use `pkg-config --static --cflags --libs cafeglsl` on
the host. Add the installation prefix to `CMAKE_PREFIX_PATH` or its pkgconfig
directory to `PKG_CONFIG_PATH` as needed. The Meson installation and release
packager honor the configured library, header and binary directories.

To save shaders to a .gsh file, include `<cafeglsl/gfd.h>`, fill in a `GFDFile` and call `WriteGFD()`.
For in-memory output, call `SerializeGFD(file, align_blocks = false)` to receive
an owned `std::vector<uint8_t>` with the same GFD bytes. Shader pointers in
`GFDFile` are borrowed only during serialization; the returned bytes are independent.
Both headers are served by the same library.
You can also save shaders compiled on the Wii U and load them on later runs.

## Important: choose loose uniforms or uniform blocks

Just a small GLSL refresher: uniform values can be declared on their own
("loose uniforms") or grouped in a block ("uniform blocks"):

```glsl
uniform vec4 color;

layout(std140) uniform Material {
    vec4 color;
};
```

On the Wii U, the first form uses `GX2Set*UniformReg` to change its values when compiled in register mode.
The second form uses `GX2Set*UniformBlock` instead.

Note that GX2 uses `GX2_SHADER_MODE_UNIFORM_REGISTER` by default. To use uniform blocks,
you __have__ to select the uniform block mode before drawing:

```cpp
GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
```

If you don't use loose uniforms, you don't need to worry about their conversion.
You can compile all shaders with `GLSL_COMPILE_BLOCK` and keep GX2 in this mode,
including when one shader has no uniforms.

To go back to being able to call loose uniforms, you'll have to call
`GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER)` again.
The Wii U reads uniform values as zero when the wrong mode is selected.

This also means that you can't mix loose uniforms and uniform blocks in a single draw.
If you write the shaders yourself, move the loose uniforms into a block to avoid this issue.

If you cannot change the source code (e.g. translation layers), CafeGLSL can (when using auto/block compile modes)
move the loose uniforms into a `__cafe_loose_uniforms` uniform block at binding 0.
And to upload to the shader, you'll have to use `GX2Set*UniformBlock` instead.
Binding 0 must be free in any shader where loose uniforms need to be moved.
Use the uniform offsets and block sizes recorded in the compiled shader to
place the values in the buffer. This is definitely more of an advanced use-case.

Read-only local constant arrays are lowered to hidden loose uniforms so lookup
tables do not require per-invocation scratch memory. Their contents are stored in
the shader's `initialValues` table, including in serialized GFD files. Each entry
contains four raw 32-bit words at `offset` (in four-byte words). Preserve the bits
when copying integer data, even though GX2 declares the value fields as floats.
In block mode, initialize those rows in `__cafe_loose_uniforms` at binding 0 before
uploading the buffer; in register mode, upload them with `GX2Set*UniformReg`.
Reapply them whenever that storage is replaced or overwritten. Hidden constants
do not appear in `uniformVars` and need no application uniform handles.

Loose uniforms in register mode can contain at most 1024 scalar components.
For example, this allows 1024 `int` values or 256 `vec4` values. Automatic mode
selection also moves loose uniforms into a block when they exceed this limit.

All compile functions let you choose how to handle uniforms:

- `GLSL_COMPILE_AUTO` uses register mode when possible and uniform block mode
  when needed. When compiling a pair, both shaders use uniform block mode if
  either shader needs it.
- `GLSL_COMPILE_REGISTER` uses register mode. Compilation fails if a shader
  has uniform blocks or too many loose uniforms.
- `GLSL_COMPILE_BLOCK` uses uniform block mode, moving any loose uniforms into
  a block.

These are equivalent to the `--uniform-mode auto`, `--uniform-mode register`,
or `--uniform-mode block` options in the CLI.

If you rely on automatic uniform mode selection and compile shader stages separately,
you'll need to manage the issue of having both the vertex and pixel shaders share the same mode,
since the shader mode inside `GX2SetShaderMode()` is used for the entire draw call.

For example, a vertex shader without any uniforms, or only a loose `uniform vec4` (when using the "auto" compile mode) will use the register mode.
But then when you compile a pixel shader with a uniform block, it'll select uniform block mode.

Even when only the pixel shader uses uniforms, choosing the mode from the vertex shader alone would be wrong.
If your application does this, it will select register mode when the pixel shader needs uniform block mode.

Now, if you used `CompileShaderPair()`, in this example it would've moved the vertex shader's loose uniform into a block so both can be used together.
You'll still have to upload that value with `GX2SetVertexUniformBlock` instead of `GX2SetVertexUniformReg`.

If you compile them separately, recompile the vertex shader with `GLSL_COMPILE_BLOCK` to get the same result.

## Building from source

See [BUILDING.md](BUILDING.md) for dependencies and build instructions.

## Docker

### Standalone compiler

Compile shaders from the current directory (bash):

```bash
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" \
  ghcr.io/exzap/cafeglsl:latest \
  -vs shader.vert -ps shader.frag -o shaders.gsh
```

Compile shaders from the current directory (powershell):

```powershell
docker run --rm -v "${PWD}:/work" `
  ghcr.io/exzap/cafeglsl:latest `
  -vs shader.vert -ps shader.frag -o shaders.gsh
```

### Wii U library

Add the library to your application's Dockerfile:

```dockerfile
COPY --from=ghcr.io/exzap/cafeglsl-wiiu:latest /artifacts $DEVKITPRO
```

You might have to update the docker container's devkitPPC version if you're experiencing crashes, since devkitPro doesn't guarantee compatibility between compiler versions.

## Troubleshooting and contributing

If you have any problems using this, please open a GitHub issue. I'll try to help as much as I can.
I would also appreciate any help with this project and PRs are very welcome!

## License

For original Mesa code see [docs/license.rst](docs/license.rst).
Any additions by this fork are licensed under MIT.
Thanks to exjam for granting permission to use Decaf's libgfd code.
