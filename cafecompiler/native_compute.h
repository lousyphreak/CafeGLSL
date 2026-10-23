#pragma once
#include "CafeGLSLCompiler.h"
#include <string>
struct nir_shader;
struct gl_program;
bool LowerNativeCompute(nir_shader *, gl_program *, GLSLComputeShader &, std::string &);

bool LowerNativeComputeGathers(nir_shader *, std::string &);
