#pragma once

#include "CafeGLSLCompiler.h"

#include <cstdint>
#include <string>
#include <vector>

struct GFDFile {
   std::vector<const GX2VertexShader *> vertexShaders;
   std::vector<const GX2PixelShader *> pixelShaders;
   std::vector<const GX2ComputeShader *> computeShaders;
};

std::vector<uint8_t> SerializeGFD(const GFDFile &file, bool align_blocks = false);
bool WriteGFD(const GFDFile &file, const std::string &path, bool align_blocks = false);
