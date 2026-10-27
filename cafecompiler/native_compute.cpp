#include "native_compute.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include "main/mtypes.h"
#include "util/format/u_formats.h"

namespace {
struct Lowering {
   gl_program *program;
   uint32_t *count;
   GLSLComputeResource *resources;
   bool graphics;
   std::string *error;
};

// Prove that a store addresses this lane's vec4, with a constant array base.
// GPU7 LDS cannot write a different lane or a dynamically selected owner slot.
bool OwnerOffset(nir_scalar s, uint32_t &base, uint32_t &scale)
{
   if (nir_scalar_is_const(s)) {
      base = nir_scalar_as_uint(s); scale = 0; return true;
   }
   if (nir_def_instr(s.def)->type == nir_instr_type_intrinsic) {
      if (nir_scalar_intrinsic_op(s) != nir_intrinsic_load_local_invocation_index)
         return false;
      base = 0; scale = 1; return true;
   }
   if (nir_def_instr(s.def)->type != nir_instr_type_alu) return false;
   auto op = nir_scalar_alu_op(s);
   if (op == nir_op_mov) return OwnerOffset(nir_scalar_chase_alu_src(s, 0), base, scale);
   if (op != nir_op_iadd && op != nir_op_imul && op != nir_op_ishl) return false;
   uint32_t a, x, b, y;
   if (!OwnerOffset(nir_scalar_chase_alu_src(s, 0), a, x) ||
       !OwnerOffset(nir_scalar_chase_alu_src(s, 1), b, y)) return false;
   if (op == nir_op_iadd) { base = a+b; scale = x+y; return true; }
   if (op == nir_op_ishl) {
      if (y || b >= 32) return false;
      base = a << b; scale = x << b; return true;
   }
   if (x && y) return false;
   base = a*b; scale = x*b+y*a; return true;
}

bool LowerLds(nir_builder *b, nir_instr *instr, void *data)
{
   auto &error = *static_cast<std::string *>(data);
   if (!error.empty() || instr->type != nir_instr_type_intrinsic) return false;
   auto intr = nir_instr_as_intrinsic(instr);
   bool store = intr->intrinsic == nir_intrinsic_store_shared;
   if (!store && intr->intrinsic != nir_intrinsic_load_shared) return false;
   b->cursor = nir_before_instr(instr);
   unsigned stride = b->shader->info.shared_size / (64*4);
   if (store) {
      uint32_t base, scale;
      if (!OwnerOffset({intr->src[1].ssa, 0}, base, scale) || scale != 16 ||
          intr->src[0].ssa->bit_size != 32) {
         error = "GPU7 LDS stores require a constant vec4 array indexed by gl_LocalInvocationIndex";
         return false;
      }
      base += nir_intrinsic_base(intr);
      if ((base % 1024) >= 16) {
         error = "GPU7 LDS stores cannot address another lane's storage"; return false;
      }
      unsigned component = (base & 15)/4;
      unsigned mask = nir_intrinsic_write_mask(intr) << component;
      nir_def *values[4];
      for (unsigned c = 0; c < 4; ++c)
         values[c] = mask & (1u << c) ? nir_channel(b, intr->src[0].ssa, c-component) : nir_imm_int(b, 0);
      auto out = nir_intrinsic_instr_create(b->shader, nir_intrinsic_store_local_shared_r600);
      out->num_components = 4;
      out->src[0] = nir_src_for_ssa(nir_vec(b, values, 4));
      out->src[1] = nir_src_for_ssa(nir_imm_int(b, (base/1024)*4));
      nir_intrinsic_set_write_mask(out, mask);
      nir_builder_instr_insert(b, &out->instr);
   } else {
      if (intr->def.bit_size != 32) {
         error = "GPU7 LDS loads require 32-bit components"; return false;
      }
      auto address = nir_iadd_imm(b, intr->src[0].ssa, nir_intrinsic_base(intr));
      auto element = nir_ushr_imm(b, address, 4);
      auto lane = nir_iand_imm(b, element, 63);
      auto row = nir_iadd(b, nir_imul_imm(b, nir_ushr_imm(b, lane, 2), stride),
                         nir_ishl_imm(b, nir_ushr_imm(b, element, 6), 2));
      auto read = nir_intrinsic_instr_create(b->shader, nir_intrinsic_load_local_shared_r600);
      read->num_components = 4;
      read->src[0] = nir_src_for_ssa(nir_vec2(b, nir_iand_imm(b, lane, 3), row));
      nir_def_init(&read->instr, &read->def, 4, 32);
      nir_builder_instr_insert(b, &read->instr);
      auto component = nir_iand_imm(b, nir_ushr_imm(b, address, 2), 3);
      nir_def *values[4];
      for (unsigned c = 0; c < intr->def.num_components; ++c)
         values[c] = nir_vector_extract(b, &read->def, nir_iadd_imm(b, component, c));
      nir_def_rewrite_uses(&intr->def, nir_vec(b, values, intr->def.num_components));
   }
   nir_instr_remove(instr);
   return true;
}

// R700 has no float16 conversion ALU opcode. Round IEEE binary32 to
// binary16 with integer operations, including subnormals, ties, NaNs and Inf.
nir_def *Half(nir_builder *b, nir_def *f)
{
   auto a = nir_iand_imm(b, f, 0x7fffffff);
   auto sign = nir_iand_imm(b, nir_ushr_imm(b, f, 16), 0x8000);
   auto exp = nir_ushr_imm(b, a, 23);
   auto normal = nir_iadd_imm(b, nir_ushr_imm(b,
      nir_iadd(b, a, nir_iadd_imm(b, nir_iand_imm(b, nir_ushr_imm(b, a, 13), 1), 0xfff)), 13), -0x1c000);
   auto shift = nir_isub(b, nir_imm_int(b, 126), exp);
   auto sig = nir_ior_imm(b, nir_iand_imm(b, a, 0x7fffff), 0x800000);
   auto bias = nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1), nir_iadd_imm(b, shift, -1)), -1);
   auto sub = nir_ushr(b, nir_iadd(b, sig,
      nir_iadd(b, bias, nir_iand_imm(b, nir_ushr(b, sig, shift), 1))), shift);
   auto small = nir_bcsel(b, nir_uge_imm(b, a, 0x33000000), sub, nir_imm_int(b, 0));
   auto finite = nir_bcsel(b, nir_uge_imm(b, a, 0x38800000), normal, small);
   finite = nir_bcsel(b, nir_uge_imm(b, a, 0x477ff000), nir_imm_int(b, 0x7c00), finite);
   auto result = nir_bcsel(b, nir_ult_imm(b, a, 0x7f800001), finite, nir_imm_int(b, 0x7e00));
   return nir_ior(b, sign, result);
}
nir_def *PackHalf(nir_builder *b, nir_def *pair)
{
   return nir_ior(b, Half(b, nir_channel(b, pair, 0)),
      nir_ishl_imm(b, Half(b, nir_channel(b, pair, 1)), 16));
}
nir_def *UnpackHalf(nir_builder *b, nir_def *packed)
{
   nir_def *out[2];
   for (unsigned i = 0; i < 2; ++i) {
      auto h = nir_iand_imm(b, nir_ushr_imm(b, packed, i * 16), 0xffff);
      auto mant = nir_iand_imm(b, h, 0x3ff);
      auto exp = nir_iand_imm(b, nir_ushr_imm(b, h, 10), 31);
      auto bits = nir_ior(b, nir_ishl_imm(b, mant, 13), nir_ishl_imm(b, nir_iadd_imm(b, exp, 112), 23));
      bits = nir_bcsel(b, nir_ieq_imm(b, exp, 31), nir_ior_imm(b, nir_ishl_imm(b, mant, 13), 0x7f800000), bits);
      bits = nir_bcsel(b, nir_ieq_imm(b, exp, 0), nir_fmul_imm(b, nir_u2f32(b, mant), 0x1p-24f), bits);
      out[i] = nir_ior(b, bits, nir_ishl_imm(b, nir_iand_imm(b, h, 0x8000), 16));
   }
   return nir_vec2(b, out[0], out[1]);
}

nir_def *PackUnorm(nir_builder *b, nir_def *rgba)
{
   auto bytes = nir_f2u32(b, nir_fround_even(b, nir_fmul_imm(b, nir_fsat(b, rgba), 255.0f)));
   auto out = nir_channel(b, bytes, 0);
   for (unsigned i = 1; i < 4; ++i)
      out = nir_ior(b, out, nir_ishl_imm(b, nir_channel(b, bytes, i), i * 8));
   return out;
}
nir_def *UnpackUnorm(nir_builder *b, nir_def *packed)
{
   nir_def *out[4];
   for (unsigned i = 0; i < 4; ++i)
      out[i] = nir_fmul_imm(b, nir_u2f32(b, nir_iand_imm(b, nir_ushr_imm(b, packed, i * 8), 255)), 1.0f/255.0f);
   return nir_vec(b, out, 4);
}

nir_def *Layout(nir_builder *b, unsigned slot, unsigned row)
{
   return nir_load_ubo(b, 4, 32, nir_imm_int(b, 15), nir_imm_int(b, slot * 32 + row * 16),
                       .align_mul = 16, .range_base = slot * 32 + row * 16, .range = 16);
}

bool Resource(Lowering &s, unsigned slot, unsigned kind, unsigned format,
              unsigned bytes, bool read, bool write)
{
   if (slot >= 16) { *s.error = "Native compute resource binding must be in 0..15"; return false; }
   for (unsigned i = 0; i < *s.count; ++i) {
      auto &r = s.resources[i];
      if (r.binding != slot) continue;
      if (r.kind != kind || r.format != format) {
         *s.error = "Native compute buffer/image bindings overlap";
         return false;
      }
      r.read |= read; r.write |= write;
      return true;
   }
   s.resources[(*s.count)++] = {slot, kind, format, bytes, read, write};
   return true;
}

unsigned GroupSize(const nir_shader *shader)
{
   return shader->info.workgroup_size[0] * shader->info.workgroup_size[1] *
          shader->info.workgroup_size[2];
}
unsigned NativeGroupSpan(const nir_shader *shader)
{
   return ((GroupSize(shader) + 63) / 64) * 64;
}
nir_def *LocalIndex(nir_builder *b)
{
   return nir_umod_imm(b, nir_load_vertex_id(b), NativeGroupSpan(b->shader));
}
nir_def *LocalId(nir_builder *b)
{
   auto idx = LocalIndex(b);
   auto *size = b->shader->info.workgroup_size;
   return nir_vec3(b, nir_umod_imm(b, idx, size[0]),
      nir_umod_imm(b, nir_udiv_imm(b, idx, size[0]), size[1]),
      nir_udiv_imm(b, idx, size[0]*size[1]));
}
nir_def *GroupCount(nir_builder *b)
{
   // The backend intrinsic fetches native dispatch counts from RID147.
   auto physical = nir_load_num_workgroups(b);
   unsigned waves = NativeGroupSpan(b->shader) / 64;
   return nir_vec3(b, nir_udiv_imm(b, nir_channel(b, physical, 0), waves),
                  nir_channel(b, physical, 1), nir_channel(b, physical, 2));
}
nir_def *GroupId(nir_builder *b)
{
   auto idx = nir_udiv_imm(b, nir_load_vertex_id(b), NativeGroupSpan(b->shader));
   auto dims = GroupCount(b);
   auto x = nir_channel(b, dims, 0), y = nir_channel(b, dims, 1);
   return nir_vec3(b, nir_umod(b, idx, x),
      nir_umod(b, nir_udiv(b, idx, x), y), nir_udiv(b, idx, nir_imul(b, x, y)));
}

bool Lower(nir_builder *b, nir_instr *instr, void *data)
{
   auto &s = *static_cast<Lowering *>(data);
   if (!s.error->empty()) return false;
   if (instr->type != nir_instr_type_intrinsic) return false;
   auto intr = nir_instr_as_intrinsic(instr);
   b->cursor = nir_before_instr(instr);
   nir_def *value = nullptr;
   if (!s.graphics && GroupSize(b->shader) > 64 &&
       (intr->intrinsic == nir_intrinsic_barrier ||
        (nir_intrinsic_infos[intr->intrinsic].flags & NIR_INTRINSIC_SUBGROUP))) {
      *s.error = "Native GPU7 cannot split logical workgroups containing barriers or subgroup communication across 64-lane waves";
      return false;
   }
   switch (intr->intrinsic) {
   case nir_intrinsic_load_local_invocation_index:
      value = LocalIndex(b);
      break;
   case nir_intrinsic_load_base_workgroup_id:
   case nir_intrinsic_load_base_global_invocation_id: value = nir_imm_zero(b, 3, intr->def.bit_size); break;
   case nir_intrinsic_load_local_invocation_id: value = LocalId(b); break;
   case nir_intrinsic_load_workgroup_id: value = GroupId(b); break;
   case nir_intrinsic_load_num_workgroups:
      if (NativeGroupSpan(b->shader) == 64) return false;
      value = GroupCount(b);
      break;
   case nir_intrinsic_load_global_invocation_id:
      value = nir_iadd(b, nir_imul(b, GroupId(b), nir_imm_ivec3(b,
         b->shader->info.workgroup_size[0], b->shader->info.workgroup_size[1],
         b->shader->info.workgroup_size[2])), LocalId(b)); break;
   case nir_intrinsic_barrier:
      if (nir_intrinsic_execution_scope(intr) > SCOPE_WORKGROUP ||
          (nir_intrinsic_memory_modes(intr) & ~nir_var_mem_shared)) {
         *s.error = s.graphics ? "Graphics image memory barriers are not implemented" :
            "Native GPU7 compute cannot synchronize groups or global memory";
         return false;
      }
      nir_instr_remove(instr); return true;
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
      *s.error = "Native R700 has no global atomic RMW instruction (MEM_REDUCT is wave-local)";
      return false;
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_store_ssbo:
   case nir_intrinsic_get_ssbo_size: {
      if (s.graphics) return false; // Existing readonly graphics SSBO path is independent.
      bool store = intr->intrinsic == nir_intrinsic_store_ssbo;
      bool querySize = intr->intrinsic == nir_intrinsic_get_ssbo_size;
      unsigned si = store ? 1 : 0;
      if (!nir_src_is_const(intr->src[si])) { *s.error = "Native compute requires static buffer bindings"; return false; }
      unsigned index = nir_src_as_uint(intr->src[si]);
      unsigned slot = s.program->sh.ShaderStorageBlocks[index]->Binding;
      if (!Resource(s, slot, GLSL_COMPUTE_BUFFER, GLSL_COMPUTE_FORMAT_BUFFER, 4, !store && !querySize, store)) return false;
      if (querySize) {
         value = nir_ishl_imm(b, nir_channel(b, Layout(b, slot, 0), 1), 2);
         break;
      }
      if (!store) {
         if (!(nir_intrinsic_access(intr) & ACCESS_NON_WRITEABLE)) {
            *s.error = "Native compute buffer reads require readonly blocks; in-dispatch export reads need uncached READ_MEM";
            return false;
         }
         // Existing readonly SSBO scalar lowering uses VFETCH resources.
         if (intr->def.bit_size != 32 || nir_intrinsic_align(intr) < 4) {
            *s.error = "Native buffer reads require DWORD-aligned 32-bit loads"; return false;
         }
         nir_def *components[4];
         for (unsigned c = 0; c < intr->def.num_components; ++c) {
            auto scalar = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
            scalar->num_components = scalar->def.num_components = 1;
            scalar->src[0] = nir_src_for_ssa(nir_imm_int(b, slot));
            scalar->src[1] = nir_src_for_ssa(nir_iadd_imm(b, intr->src[1].ssa, c * 4));
            nir_intrinsic_set_align(scalar, 4, 0);
            nir_builder_instr_insert(b, &scalar->instr);
            components[c] = &scalar->def;
         }
         nir_def_rewrite_uses(&intr->def, nir_vec(b, components, intr->def.num_components));
         nir_instr_remove(instr);
         return true;
      }
      auto layout = Layout(b, slot, 0);
      auto byteOffset = intr->src[2].ssa;
      auto payload = intr->src[0].ssa;
      if (payload->bit_size != 32 || nir_intrinsic_align(intr) < 4) {
         *s.error = "Native buffer exports require DWORD-aligned 32-bit stores";
         return false;
      }
      for (unsigned c = 0; c < payload->num_components; ++c) {
         if (!(nir_intrinsic_write_mask(intr) & (1u << c))) continue;
         auto offset = nir_iadd_imm(b, nir_ushr_imm(b, byteOffset, 2), c);
         nir_store_global(b, nir_channel(b, payload, c),
            nir_ishl_imm(b, nir_iadd(b, nir_channel(b, layout, 0), offset), 2), .write_mask = 1, .align_mul = 4);
      }
      nir_instr_remove(instr); return true;
   }
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_store: {
      auto var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
      if (!var || glsl_type_is_array(var->type) ||
          (glsl_get_sampler_dim(var->type) != GLSL_SAMPLER_DIM_2D &&
           glsl_get_sampler_dim(var->type) != GLSL_SAMPLER_DIM_3D)) {
         *s.error = "Native compute images require a static 2D, 2D-array or 3D binding"; return false;
      }
      const bool volume = glsl_get_sampler_dim(var->type) == GLSL_SAMPLER_DIM_3D;
      unsigned format, bytes;
      switch (var->data.image.format) {
      case PIPE_FORMAT_R32G32B32A32_FLOAT: format = GLSL_COMPUTE_FORMAT_RGBA32F; bytes = 16; break;
      case PIPE_FORMAT_R16G16B16A16_FLOAT: format = GLSL_COMPUTE_FORMAT_RGBA16F; bytes = 8; break;
      case PIPE_FORMAT_R32_FLOAT: format = GLSL_COMPUTE_FORMAT_R32F; bytes = 4; break;
      case PIPE_FORMAT_R16_FLOAT: format = GLSL_COMPUTE_FORMAT_R16F; bytes = 16; break;
      case PIPE_FORMAT_R8G8B8A8_UNORM: format = GLSL_COMPUTE_FORMAT_RGBA8; bytes = 4; break;
      case PIPE_FORMAT_R8G8_UNORM: format = GLSL_COMPUTE_FORMAT_RG8; bytes = 16; break;
      case PIPE_FORMAT_R8_UNORM: format = GLSL_COMPUTE_FORMAT_R8; bytes = 16; break;
      case PIPE_FORMAT_R32G32B32A32_UINT: format = GLSL_COMPUTE_FORMAT_RGBA32UI; bytes = 16; break;
      case PIPE_FORMAT_R32_UINT: format = GLSL_COMPUTE_FORMAT_R32UI; bytes = 4; break;
      case PIPE_FORMAT_R16G16_FLOAT: format = GLSL_COMPUTE_FORMAT_RG16F; bytes = 4; break;
      case PIPE_FORMAT_R32G32_FLOAT: format = GLSL_COMPUTE_FORMAT_RG32F; bytes = 8; break;
      default: *s.error = "Native compute image format has no export representation"; return false;
      }
      unsigned slot = var->data.binding;
      bool store = intr->intrinsic == nir_intrinsic_image_deref_store;
      bool load = intr->intrinsic == nir_intrinsic_image_deref_load;
      if (!Resource(s, slot, GLSL_COMPUTE_IMAGE, format, bytes, load, store)) return false;
      auto dims = Layout(b, slot, 1);
      if (load && s.graphics) {
         auto layout = Layout(b, slot, 0);
         auto coord = intr->src[1].ssa;
         auto x = nir_channel(b, coord, 0), y = nir_channel(b, coord, 1);
         auto z = volume || glsl_sampler_type_is_array(var->type) ? nir_channel(b, coord, 2) : nir_imm_int(b, 0);
         auto valid = nir_iand(b, nir_ult(b, x, nir_channel(b, dims, 0)),
            nir_iand(b, nir_ult(b, y, nir_channel(b, dims, 1)), nir_ult(b, z, nir_channel(b, dims, 2))));
         auto offset = nir_iadd(b, nir_channel(b, layout, 0), nir_iadd(b, nir_imul_imm(b, x, bytes / 4),
            nir_iadd(b, nir_imul(b, y, nir_channel(b, layout, 2)), nir_imul(b, z, nir_channel(b, layout, 3)))));
         // SCATTER reads are uncached and DWORD-addressed by the SFN backend.
         value = nir_load_global(b, bytes / 4, 32, nir_ishl_imm(b, offset, 2), .align_mul = 4);
         if (format == GLSL_COMPUTE_FORMAT_RGBA8) value = UnpackUnorm(b, value);
         else if (format == GLSL_COMPUTE_FORMAT_RGBA16F)
            value = nir_vec4(b, nir_channel(b, UnpackHalf(b, nir_channel(b, value, 0)), 0),
               nir_channel(b, UnpackHalf(b, nir_channel(b, value, 0)), 1),
               nir_channel(b, UnpackHalf(b, nir_channel(b, value, 1)), 0),
               nir_channel(b, UnpackHalf(b, nir_channel(b, value, 1)), 1));
         else if (format == GLSL_COMPUTE_FORMAT_RG16F) value = UnpackHalf(b, value);
         if (value->num_components < 4) {
            nir_def *components[4];
            for (unsigned c = 0; c < 4; ++c)
               components[c] = c < value->num_components ? nir_channel(b, value, c) :
                  nir_imm_int(b, c == 3 ? (format == GLSL_COMPUTE_FORMAT_R32UI ? 1 : 0x3f800000) : 0);
            value = nir_vec(b, components, 4);
         }
         value = nir_bcsel(b, valid, value, nir_imm_ivec4(b, 0, 0, 0, 0));
         break;
      }
      if (load) {
         if (!s.graphics && !(var->data.access & ACCESS_NON_WRITEABLE)) {
            *s.error = "Native compute imageLoad requires a readonly sampled alias; in-dispatch export reads are unsupported";
            return false;
         }
         unsigned nc = volume || glsl_sampler_type_is_array(var->type) ? 3 : 2;
         auto tex = nir_tex_instr_create(b->shader, 2);
         tex->op = nir_texop_txf;
         tex->sampler_dim = glsl_get_sampler_dim(var->type);
         tex->is_array = glsl_sampler_type_is_array(var->type);
         tex->dest_type = nir_intrinsic_dest_type(intr);
         tex->texture_index = slot;
         tex->coord_components = nc;
         tex->src[0].src_type = nir_tex_src_coord;
         auto coord = nir_trim_vector(b, intr->src[1].ssa, nc);
         if (volume)
            coord = nir_vec3(b, nir_channel(b, coord, 0), nir_channel(b, coord, 1),
                            nir_iadd(b, nir_channel(b, coord, 2), nir_channel(b, dims, 3)));
         tex->src[0].src = nir_src_for_ssa(coord);
         tex->src[1].src_type = nir_tex_src_lod;
         tex->src[1].src = nir_src_for_ssa(nir_imm_int(b, 0));
         nir_def_init(&tex->instr, &tex->def, 4, 32);
         nir_builder_instr_insert(b, &tex->instr);
         value = &tex->def;
         if (volume) {
            auto relative = intr->src[1].ssa;
            auto valid = nir_iand(b, nir_ult(b, nir_channel(b, relative, 0), nir_channel(b, dims, 0)),
               nir_iand(b, nir_ult(b, nir_channel(b, relative, 1), nir_channel(b, dims, 1)),
                            nir_ult(b, nir_channel(b, relative, 2), nir_channel(b, dims, 2))));
            value = nir_bcsel(b, valid, value, nir_imm_ivec4(b, 0, 0, 0, 0));
         }
         break;
      }
      if (!store) {
         value = nir_trim_vector(b, dims, intr->def.num_components);
         break;
      }
      auto coord = intr->src[1].ssa;
      auto layout = Layout(b, slot, 0);
      auto x = nir_channel(b, coord, 0), y = nir_channel(b, coord, 1);
      auto z = volume || glsl_sampler_type_is_array(var->type) ? nir_channel(b, coord, 2) : nir_imm_int(b, 0);
      auto valid = nir_iand(b, nir_ult(b, x, nir_channel(b, dims, 0)),
         nir_iand(b, nir_ult(b, y, nir_channel(b, dims, 1)), nir_ult(b, z, nir_channel(b, dims, 2))));
      auto offset = nir_iadd(b, nir_imul_imm(b, x, bytes / 4),
         nir_iadd(b, nir_imul(b, y, nir_channel(b, layout, 2)), nir_imul(b, z, nir_channel(b, layout, 3))));
      auto payload = intr->src[3].ssa;
      if (format == GLSL_COMPUTE_FORMAT_RGBA16F) {
         payload = nir_vec2(b, PackHalf(b, nir_channels(b, payload, 3)),
                              PackHalf(b, nir_channels(b, payload, 12)));
      } else if (format == GLSL_COMPUTE_FORMAT_RG16F) {
         payload = PackHalf(b, nir_channels(b, payload, 3));
      } else if (format == GLSL_COMPUTE_FORMAT_RG32F) {
         payload = nir_channels(b, payload, 3);
      } else if (format == GLSL_COMPUTE_FORMAT_RGBA8) {
         payload = PackUnorm(b, payload);
      } else if (format == GLSL_COMPUTE_FORMAT_R32F || format == GLSL_COMPUTE_FORMAT_R32UI) {
         payload = nir_channel(b, payload, 0);
      } else if (format == GLSL_COMPUTE_FORMAT_R16F) {
         auto rounded = UnpackHalf(b, PackHalf(b, nir_channels(b, payload, 3)));
         payload = nir_vec4(b, nir_channel(b, rounded, 0), nir_imm_float(b, 0), nir_imm_float(b, 0), nir_imm_float(b, 1));
      } else if (format == GLSL_COMPUTE_FORMAT_R8 || format == GLSL_COMPUTE_FORMAT_RG8) {
         payload = UnpackUnorm(b, PackUnorm(b, payload));
         payload = nir_vec4(b, nir_channel(b, payload, 0),
            format == GLSL_COMPUTE_FORMAT_RG8 ? nir_channel(b, payload, 1) : nir_imm_float(b, 0),
            nir_imm_float(b, 0), nir_imm_float(b, 1));
      }
      nir_push_if(b, valid);
      nir_store_global(b, payload, nir_ishl_imm(b, nir_iadd(b, nir_channel(b, layout, 0), offset), 2),
                       .write_mask = (1u << payload->num_components)-1, .align_mul = 4);
      nir_pop_if(b, nullptr);
      nir_instr_remove(instr); return true;
   }
   default: return false;
   }
   if (value) {
      nir_def_rewrite_uses(&intr->def, value);
      nir_instr_remove(instr);
      return true;
   }
   return false;
}
}

bool LowerNativeCompute(nir_shader *nir, gl_program *program, GLSLComputeShader &output, std::string &error)
{
   unsigned count = GroupSize(nir);
   // The GLSL linker has already lowered shared variables to explicit IO.
   if (nir->info.shared_size) {
      if (count != 64 || nir->info.shared_size > 16384) {
         error = "GPU7 LDS requires 64 lanes and at most sixteen owner-indexed vec4 arrays[64] (kernel has " +
                 std::to_string(count) + " lanes, " + std::to_string(nir->info.shared_size) + " shared bytes)";
         return false;
      }
      // Explicit IO excludes the last vec3's tail padding; hardware slots are vec4.
      nir->info.shared_size = (nir->info.shared_size + 1023) & ~1023u;
      nir_shader_instructions_pass(nir, LowerLds, nir_metadata_none, &error);
      if (!error.empty()) return false;
   }
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo) {
      if (var->data.binding == 15) { error = "Compute uniform block 15 is reserved for native resource layouts"; return false; }
   }
   Lowering state{program, &output.resourceCount, output.resources, false, &error};
   nir_shader_instructions_pass(nir, Lower, nir_metadata_none, &state);
   if (!error.empty()) return false;
   if (count % 64 != 0) {
      // Dispatch is rounded up to whole waves; padding lanes execute no shader work.
      auto impl = nir_shader_get_entrypoint(nir);
      nir_builder b = nir_builder_create(impl);
      b.cursor = nir_before_impl(impl);
      nir_push_if(&b, nir_uge_imm(&b, LocalIndex(&b), count));
      nir_jump(&b, nir_jump_return);
      nir_pop_if(&b, nullptr);
      nir_metadata_invalidate(nir);
      nir_lower_returns(nir);
   }
   nir_opt_dce(nir);
   nir_remove_dead_variables(nir, nir_var_image | nir_var_mem_ssbo, nullptr);
   return true;
}

bool LowerNativeImages(nir_shader *nir, gl_program *program, GLSLShaderResources &output, std::string &error)
{
   if (nir->info.num_images == 0) return true;
   nir_foreach_variable_with_modes(var, nir, nir_var_mem_ubo) {
      if (var->data.binding == 15) { error = "Graphics uniform block 15 is reserved for image layouts"; return false; }
   }
   Lowering state{program, &output.resourceCount, output.resources, true, &error};
   nir_shader_instructions_pass(nir, Lower, nir_metadata_none, &state);
   if (!error.empty()) return false;
   nir_opt_dce(nir);
   nir_remove_dead_variables(nir, nir_var_image, nullptr);
   return true;
}

// R700 has no GATHER4 opcode. Sample each footprint texel at its center at
// base LOD, retaining the original sampler's wrap and border behavior.
static bool LowerGather(nir_builder *b, nir_instr *instruction, void *data)
{
   if (instruction->type != nir_instr_type_tex) return false;
   auto tex = nir_instr_as_tex(instruction);
   if (tex->op != nir_texop_tg4) return false;
   auto &error = *static_cast<std::string *>(data);
   if (tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_shadow ||
       nir_tex_instr_has_explicit_tg4_offsets(tex) ||
       nir_tex_instr_src_index(tex, nir_tex_src_texture_offset) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_sampler_offset) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_lod) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_bias) >= 0) {
      error = "Native gather lowering requires a static 2D/array base-LOD gather with one optional offset";
      return false;
   }
   b->cursor = nir_before_instr(instruction);
   auto size = nir_tex_instr_create(b->shader, 1);
   size->op = nir_texop_txs;
   size->sampler_dim = GLSL_SAMPLER_DIM_2D;
   size->is_array = tex->is_array;
   size->texture_index = tex->texture_index;
   size->dest_type = nir_type_int32;
   size->src[0].src_type = nir_tex_src_lod;
   size->src[0].src = nir_src_for_ssa(nir_imm_int(b, 0));
   nir_def_init(&size->instr, &size->def, tex->is_array ? 3 : 2, 32);
   nir_builder_instr_insert(b, &size->instr);
   auto dim = nir_u2f32(b, nir_channels(b, &size->def, 3));
   auto uv = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
   auto base = nir_ffloor(b, nir_fadd_imm(b, nir_fmul(b, nir_channels(b, uv, 3), dim), -0.5));
   int offset = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   if (offset >= 0) base = nir_fadd(b, base, nir_i2f32(b, tex->src[offset].src.ssa));
   const int dx[] = {0, 1, 1, 0}, dy[] = {1, 1, 0, 0};
   nir_def *components[4];
   for (unsigned i = 0; i < 4; ++i) {
      auto sample = nir_tex_instr_create(b->shader, 2);
      sample->op = nir_texop_txl;
      sample->sampler_dim = GLSL_SAMPLER_DIM_2D;
      sample->is_array = tex->is_array;
      sample->coord_components = tex->is_array ? 3 : 2;
      sample->texture_index = tex->texture_index;
      sample->sampler_index = tex->sampler_index;
      sample->dest_type = tex->dest_type;
      auto center = nir_fadd(b, base, nir_imm_vec2(b, dx[i] + 0.5, dy[i] + 0.5));
      sample->src[0].src_type = nir_tex_src_coord;
      auto sampleCoord = nir_fdiv(b, center, dim);
      if (tex->is_array) sampleCoord = nir_vec3(b, nir_channel(b, sampleCoord, 0), nir_channel(b, sampleCoord, 1), nir_channel(b, uv, 2));
      sample->src[0].src = nir_src_for_ssa(sampleCoord);
      sample->src[1].src_type = nir_tex_src_lod;
      sample->src[1].src = nir_src_for_ssa(nir_imm_float(b, 0));
      nir_def_init(&sample->instr, &sample->def, 4, 32);
      nir_builder_instr_insert(b, &sample->instr);
      components[i] = nir_channel(b, &sample->def, tex->component);
   }
   nir_def_rewrite_uses(&tex->def, nir_vec(b, components, 4));
   nir_instr_remove(instruction);
   return true;
}

bool LowerNativeComputeGathers(nir_shader *nir, std::string &error)
{
   nir_shader_instructions_pass(nir, LowerGather, nir_metadata_control_flow, &error);
   return error.empty();
}
