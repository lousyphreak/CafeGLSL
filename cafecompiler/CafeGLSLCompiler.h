#pragma once

#include <stdint.h>

#if defined(__WUT__) || defined(__WIIU__)
#include <gx2/shaders.h>
#else
#include "cafe_gx2.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GLSL_COMPILER_FLAG {
   GLSL_COMPILER_FLAG_NONE = 0,
   /* Legacy alias for GLSL_COMPILER_FLAG_PRINT_DISASSEMBLY_TO_STDERR. */
   GLSL_COMPILER_FLAG_GENERATE_DISASSEMBLY = 1 << 0,
   GLSL_COMPILER_FLAG_PRINT_DISASSEMBLY_TO_STDERR = 1 << 0,
} GLSL_COMPILER_FLAG;

typedef enum GLSLCompileMode {
   /* Use uniform registers when possible, blocks when needed. */
   GLSL_COMPILE_AUTO = 0,
   /* Fail if uniform blocks are needed. */
   GLSL_COMPILE_REGISTER = 1,
   /* Use uniform blocks, moving loose uniforms into a block. */
   GLSL_COMPILE_BLOCK = 2,
} GLSLCompileMode;

#define GLSL_SHADER_MODE_ERROR ((GX2ShaderMode)-1)

/* Initialize before compiling. Destroy when done. */
void InitGLSLCompiler(void);
void DestroyGLSLCompiler(void);
const char *GetGLSLCompilerVersion(void);

/* Return the mode for GX2SetShaderMode(), or GLSL_SHADER_MODE_ERROR on failure.
 * On failure, shader pointers are set to null and infoLogOut contains the error.
 * See the README's "Important: choose loose uniforms or uniform blocks" chapter. */
GX2ShaderMode CompileVertexShader(const char *source, GLSLCompileMode requestedMode,
                                        GX2VertexShader **shaderOut, char *infoLogOut,
                                        int infoLogMaxLength, GLSL_COMPILER_FLAG flags);
GX2ShaderMode CompilePixelShader(const char *source, GLSLCompileMode requestedMode,
                                       GX2PixelShader **shaderOut, char *infoLogOut,
                                       int infoLogMaxLength, GLSL_COMPILER_FLAG flags);
/* Compute resource/layout ABI version 1. Block 15 is reserved for 16 entries,
 * each two little-endian uvec4s: {baseDword, sizeDwords, rowPitchDwords,
 * layerPitchDwords}, {width, height, layers, 0}. All writable slots address the
 * one GX2 shader export allocation. Sampled inputs retain their native slots.
 * Images smaller than a DWORD use a widened, quantized float4 intermediate;
 * the renderer must convert it at the imageStore boundary before sampling.
 * shader.workGroupSizeX/Y/Z retain logical dimensions. With
 * F = ceil(X*Y*Z / 64), dispatch native {logicalGroupCountX*F, Ycount, Zcount}.
 * The compiled shader recovers logical counts/IDs and masks padding lanes.
 * Only independent logical groups may span multiple native waves; shared LDS,
 * barriers, subgroup communication, atomics and export-memory sharing are not
 * supplied by this mapping. over64Mode remains zero.
 */
typedef enum GLSLComputeResourceKind {
   GLSL_COMPUTE_BUFFER = 0,
   GLSL_COMPUTE_IMAGE = 1,
} GLSLComputeResourceKind;
typedef enum GLSLComputeFormat {
   GLSL_COMPUTE_FORMAT_BUFFER = 0,
   GLSL_COMPUTE_FORMAT_RGBA32F = 1,
   GLSL_COMPUTE_FORMAT_RGBA16F = 2,
   GLSL_COMPUTE_FORMAT_R32F = 3,
   GLSL_COMPUTE_FORMAT_R16F = 4,
   GLSL_COMPUTE_FORMAT_RGBA8 = 5,
   GLSL_COMPUTE_FORMAT_RG8 = 6,
   GLSL_COMPUTE_FORMAT_R8 = 7,
   GLSL_COMPUTE_FORMAT_RGBA32UI = 8,
   GLSL_COMPUTE_FORMAT_R32UI = 9,
} GLSLComputeFormat;
typedef struct GLSLComputeResource {
   uint32_t binding, kind, format, elementBytes;
   uint32_t read, write;
} GLSLComputeResource;
typedef struct GLSLComputeShader {
   GX2ComputeShader shader;
   uint32_t resourceCount;
   GLSLComputeResource resources[16];
} GLSLComputeShader;
GX2ShaderMode CompileComputeShader(const char *source, GLSLComputeShader **shaderOut,
                                  char *infoLogOut, int infoLogMaxLength,
                                  GLSL_COMPILER_FLAG flags);
void FreeComputeShader(GLSLComputeShader *shader);

/* Compile a pair with a shared uniform mode. Both sources are required.
 * Matching vertex outputs to pixel inputs is not checked. */
GX2ShaderMode CompileShaderPair(const char *vertexSource, const char *pixelSource,
                                      GLSLCompileMode requestedMode,
                                      GX2VertexShader **vertexOut, GX2PixelShader **pixelOut,
                                      char *infoLogOut, int infoLogMaxLength,
                                      GLSL_COMPILER_FLAG flags);

/* Results remain owned by the caller after compiler destruction. Compilation
 * writes CPU memory only; Wii U callers must establish shader-cache visibility
 * before binding programs and keep results alive until GPU use has completed.
 * Free shaders when done. Null shaders are accepted. */
void FreeVertexShader(GX2VertexShader *shader);
void FreePixelShader(GX2PixelShader *shader);
void FreeShaders(GX2VertexShader *vertex, GX2PixelShader *pixel);

#ifdef __cplusplus
}
#endif
