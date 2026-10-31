#include "../../cafe_gx2.h"

#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_console.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

void
GX2SetComputeShader(const GX2ComputeShader *shader);
void
GX2SetShaderExportBuffer(void *buffer, uint32_t size);
void
GX2DispatchCompute(const uint32_t *groups);

static uint32_t sOutput[1024] __attribute__((aligned(256)));
static uint32_t sGroups[4] __attribute__((aligned(256))) = {2, 3, 2, 0};

static int
LoadShader(const char *path, GX2ComputeShader *shader)
{
   FILE *file = fopen(path, "rb");
   uint32_t header[8];
   int result = 0;
   if (!file) {
      return 0;
   }

   fseek(file, 32, SEEK_SET);
   while (fread(header, sizeof(header), 1, file) == 1) {
      if (header[4] == 14 && header[7] == 0) {
         GX2ComputeShader serialized;
         if (fread(&serialized, sizeof(serialized), 1, file) != 1) {
            break;
         }
         if (serialized.uniformBlockCount || serialized.uniformVarCount ||
             serialized.initialValueCount || serialized.samplerVarCount) {
            break;
         }
         *shader          = serialized;
         shader->program  = NULL;
         shader->loopVars = NULL;
         long end         = ftell(file) + header[5] - sizeof(*shader);
         if (shader->loopVarCount) {
            long start       = ftell(file) - sizeof(*shader);
            uint32_t offset  = (uint32_t)serialized.loopVars & 0xfffff;
            shader->loopVars = malloc(shader->loopVarCount * sizeof(GX2LoopVar));
            if (!shader->loopVars) {
               break;
            }
            fseek(file, start + offset, SEEK_SET);
            if (fread(shader->loopVars, sizeof(GX2LoopVar), shader->loopVarCount, file) != shader->loopVarCount) {
               break;
            }
         }
         fseek(file, end, SEEK_SET);
      } else if (header[4] == 15 && header[7] == 0) {
         shader->program = memalign(256, header[5]);
         if (shader->program) {
            result = fread(shader->program, header[5], 1, file) == 1;
         }
         break;
      } else if (header[4] == 1) {
         break;
      } else {
         fseek(file, header[5], SEEK_CUR);
      }
   }
   fclose(file);
   return result;
}

int
main(int argc, char **argv)
{
   GX2ComputeShader shader = {0};
   char path[512]          = "SD card not mounted";
   unsigned mismatches     = 0;
   int result              = 1;

   WHBProcInit();
   WHBLogCafeInit();
   void *commands        = memalign(256, 4 * 1024 * 1024);
   uint32_t attributes[] = {
      GX2_INIT_CMD_BUF_BASE, (uint32_t)commands,
      GX2_INIT_CMD_BUF_POOL_SIZE, 4 * 1024 * 1024,
      GX2_INIT_END};
   GX2Init(attributes);
   if (WHBMountSdCard()) {
      snprintf(path, sizeof(path), "%s/wiiu/apps/cafeglsl-compute/simple.gsh",
               WHBGetSdCardMountPath());
      if (argc > 1) {
         snprintf(path, sizeof(path), "%s", argv[1]);
      }
      if (LoadShader(path, &shader)) {
         for (unsigned i = 0; i < sizeof(sOutput) / sizeof(sOutput[0]); ++i) {
            sOutput[i] = __builtin_bswap32(0xdeadbeef);
         }
         DCFlushRange(sOutput, sizeof(sOutput));
         DCFlushRange(sGroups, sizeof(sGroups));
         GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader.program, shader.size);
         GX2SetShaderMode(GX2_SHADER_MODE_COMPUTE_SHADER);
         GX2SetComputeShader(&shader);
         GX2SetShaderExportBuffer(sOutput, sizeof(sOutput));
         GX2DispatchCompute(sGroups);
         GX2Invalidate(GX2_INVALIDATE_MODE_EXPORT_BUFFER, sOutput, sizeof(sOutput));
         GX2DrawDone();
         DCInvalidateRange(sOutput, sizeof(sOutput));
         unsigned width       = sGroups[0] * shader.workgroupSizeX;
         unsigned height      = sGroups[1] * shader.workgroupSizeY;
         unsigned invocations = width * height * sGroups[2] * shader.workgroupSizeZ;
         for (unsigned i = 0; i < sizeof(sOutput) / sizeof(sOutput[0]); ++i) {
            sOutput[i]        = __builtin_bswap32(sOutput[i]);
            uint32_t expected = 0xdeadbeef;
            if (i < invocations) {
               unsigned x     = i % width;
               unsigned y     = (i / width) % height;
               unsigned z     = i / (width * height);
               unsigned local = x % shader.workgroupSizeX + shader.workgroupSizeX *
                                                               (y % shader.workgroupSizeY + shader.workgroupSizeY * (z % shader.workgroupSizeZ));
               unsigned group = x / shader.workgroupSizeX + sGroups[0] *
                                                               (y / shader.workgroupSizeY + sGroups[1] * (z / shader.workgroupSizeZ));
               expected       = local + 64u * group + 65536u * sGroups[2];
            }
            mismatches += sOutput[i] != expected;
         }
         GX2SetShaderExportBuffer(NULL, 0);
         GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
         GX2DrawDone();
         result = 0;
      }
      WHBUnmountSdCard();
   }
   free(shader.program);
   free(shader.loopVars);

   GX2Shutdown();
   free(commands);
   WHBLogConsoleInit();
   WHBLogPrintf("CafeGLSL compute: %s", path);
   if (result) {
      WHBLogPrintf("Could not load the compute shader.");
   } else {
      WHBLogPrintf("Workgroup: %u,%u,%u", shader.workgroupSizeX, shader.workgroupSizeY, shader.workgroupSizeZ);
      WHBLogPrintf("Dispatch: %u,%u,%u", sGroups[0], sGroups[1], sGroups[2]);
      WHBLogPrintf("%s: %u mismatches", mismatches ? "FAIL" : "PASS", mismatches);
      for (unsigned i = 0; i < 64; i += 8) {
         WHBLogPrintf("%2u: %u %u %u %u %u %u %u %u", i,
                      sOutput[i], sOutput[i + 1], sOutput[i + 2], sOutput[i + 3],
                      sOutput[i + 4], sOutput[i + 5], sOutput[i + 6], sOutput[i + 7]);
      }
   }
   WHBLogPrintf("Press HOME to exit.");
   while (WHBProcIsRunning()) {
      WHBLogConsoleDraw();
      OSSleepTicks(OSMillisecondsToTicks(50));
   }
   WHBLogConsoleFree();
   WHBLogCafeDeinit();
   WHBProcShutdown();
   return result || mismatches;
}
