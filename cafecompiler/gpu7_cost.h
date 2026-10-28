#pragma once

// Coarse relative issue estimates for ranking unroll candidates. Four vector
// lanes share an ALU issue; scalar special functions occupy a full issue. Fetch
// weights approximate latency rather than predicting wall-clock cycles.
constexpr unsigned kGpu7BaseUnroll = 64;
constexpr unsigned kGpu7ExpandedUnroll = 255;
constexpr unsigned kGpu7UnrollCostPerIteration = 26; // NIR LOOP_UNROLL_LIMIT.

struct Gpu7Cost {
   double alu = 0, texture = 0, buffer = 0;
   unsigned unrollCost = 0;
};

static Gpu7Cost Gpu7Count(const exec_list &list)
{
   Gpu7Cost cost;
   foreach_list_typed(nir_cf_node, node, node, &list) {
      if (node->type == nir_cf_node_block) {
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            if (instr->type == nir_instr_type_alu) {
               auto alu = nir_instr_as_alu(instr);
               switch (alu->op) {
               case nir_op_mov:
               case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
               case nir_op_vec8: case nir_op_vec16: break;
               case nir_op_frcp: case nir_op_frsq: case nir_op_fsqrt:
               case nir_op_fexp2: case nir_op_flog2:
               case nir_op_fsin: case nir_op_fcos:
               case nir_op_fdot2: case nir_op_fdot3: case nir_op_fdot4:
                  cost.alu += alu->def.num_components; break;
               default: cost.alu += 0.25 * alu->def.num_components; break;
               }
            } else if (instr->type == nir_instr_type_tex) {
               cost.texture += 1;
            } else if (instr->type == nir_instr_type_intrinsic) {
               switch (nir_instr_as_intrinsic(instr)->intrinsic) {
               case nir_intrinsic_load_ssbo: case nir_intrinsic_load_global:
               case nir_intrinsic_load_local_shared_r600:
                  cost.buffer += 1; break;
               default: break;
               }
            }
         }
      } else if (node->type == nir_cf_node_if) {
         auto branch = nir_cf_node_as_if(node);
         auto a = Gpu7Count(branch->then_list), b = Gpu7Count(branch->else_list);
         cost.alu += std::max(a.alu, b.alu);
         cost.texture += std::max(a.texture, b.texture);
         cost.buffer += std::max(a.buffer, b.buffer);
         cost.unrollCost += a.unrollCost + b.unrollCost;
      } else if (node->type == nir_cf_node_loop) {
         auto loop = nir_cf_node_as_loop(node);
         auto body = Gpu7Count(loop->body);
         unsigned count = loop->info->exact_trip_count_known ? loop->info->max_trip_count : 1;
         cost.alu += count * body.alu;
         cost.texture += count * body.texture;
         cost.buffer += count * body.buffer;
         // Match NIR's 26-instruction cost budget per permitted iteration. A
         // loop already within the base limits remains for a structural reason;
         // increasing the limits cannot help it.
         const unsigned expanded = loop->info->instr_cost * count;
         const bool expandable = count > 1 && count <= kGpu7ExpandedUnroll &&
            (count > kGpu7BaseUnroll || expanded > kGpu7BaseUnroll * kGpu7UnrollCostPerIteration) &&
            expanded <= kGpu7ExpandedUnroll * kGpu7UnrollCostPerIteration &&
            loop->control != nir_loop_control_dont_unroll;
         cost.unrollCost += body.unrollCost + (expandable ? expanded : 0);
      }
   }
   return cost;
}

static Gpu7Cost Gpu7Measure(nir_shader *nir)
{
   auto impl = nir_shader_get_entrypoint(nir);
   nir_metadata_require(impl, nir_metadata_loop_analysis, 0, 0);
   return Gpu7Count(impl->body);
}

static double Gpu7Estimate(const Gpu7Cost &cost, const nir_shader *nir, unsigned gprs)
{
   // Default GX2 graphics allocations are VS=48 / PS=200. The native compute
   // setter budgets 248 GPRs per SIMD. Shared LDS without the clause-local proof
   // is serialized to one resident wave by our descriptor.
   const unsigned budget = nir->info.stage == MESA_SHADER_FRAGMENT ? 200 :
                           nir->info.stage == MESA_SHADER_VERTEX ? 48 : 248;
   const unsigned waves = nir->info.stage == MESA_SHADER_COMPUTE && nir->info.shared_size
      ? 1 : std::max(1u, budget / std::max(1u, gprs));
   const double fetch = 16 * cost.texture + 4 * cost.buffer;
   return std::max(cost.alu, fetch * (1.0 + 1.0 / waves));
}
