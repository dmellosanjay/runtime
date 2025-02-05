/*
 * Copyright 2021 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- cpurt_kernels.cc - CPURT kernels -----------------------------------===//
//
// This file defines the C++ kernels for the CPURT dialect.
//
//===----------------------------------------------------------------------===//

#include <sys/types.h>

#include <cstdint>
#include <memory>

#include "tfrt/cpu/jit/cpurt.h"
#include "tfrt/dtype/dtype.h"
#include "tfrt/host_context/async_dispatch.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/attribute_utils.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/execution_context.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/host_context/kernel_registry.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/rc_array.h"
#include "tfrt/support/ref_count.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/tensor.h"
#include "tfrt/tensor/tensor_shape.h"

namespace tfrt {
namespace cpu {
namespace jit {

// -------------------------------------------------------------------------- //
// Compile compilation unit attribute to an executable result.
// -------------------------------------------------------------------------- //

static AsyncValueRef<CompilationResult> Compile(
    CompilationUnitAttribute kernel, const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();

  // We only support functions nested in top level compiled module.
  if (kernel.nested_symbols().size() != 1)
    return EmitErrorAsync(
        exec_ctx, "compiled kernel must be referenced by one nested symbol");

  ResourceContext* res_ctx = exec_ctx.resource_context();
  auto* compilation_cache =
      res_ctx->GetOrCreateResource<CompilationResultCache>("cpurt.cache", host);

  // TODO(ezhulenev): Compute cache key based on the content of MLIR module.
  intptr_t key = exec_ctx.location().data;

  // Return compiled kernel from the cache.
  if (auto compiled = compilation_cache->Find(key)) return compiled;

  CompilationOptions opts;
  opts.num_worker_threads = host->GetNumWorkerThreads();

  string_view entrypoint = kernel.nested_symbols()[0];
  string_view module = kernel.serialized_operation();
  Expected<CompilationResult> compiled =
      CompileKernelMlirModule(module, entrypoint, opts);

  // Failed to compile kernel source.
  if (auto err = compiled.takeError())
    return EmitErrorAsync(exec_ctx, std::move(err));

  // Update the compilation cache and return the result.
  return compilation_cache->Insert(key, std::move(*compiled));
}

// -------------------------------------------------------------------------- //
// Execute compiled CPURT kernels.
// -------------------------------------------------------------------------- //

static Error ConvertTensorOperandsToMemrefDesc(
    mlir::FunctionType signature, RepeatedArguments<Tensor> operands,
    SmallVectorImpl<MemrefDesc>* memrefs) {
  assert(memrefs->empty() && "memrefs must be empty");
  memrefs->reserve(operands.size());

  for (unsigned i = 0; i < operands.size(); ++i) {
    auto memref_ty = signature.getInput(i).dyn_cast<mlir::MemRefType>();
    if (!memref_ty)
      return MakeStringError("expected memref operand at #", i,
                             ", got: ", signature.getInput(i));

    auto memref = ConvertTensorToMemrefDesc(memref_ty, operands[i]);
    if (auto err = memref.takeError()) return err;
    memrefs->push_back(*memref);
  }

  return Error::success();
}

static void Execute(Argument<CompilationResult> compilation_result,
                    Argument<Chain> in_chain,
                    RepeatedArguments<Tensor> operands,
                    RemainingResults results,
                    const ExecutionContext& exec_ctx) {
  // Extract Memrefs from Tensor operands.
  SmallVector<MemrefDesc, 4> memrefs;
  if (auto err = ConvertTensorOperandsToMemrefDesc(
          compilation_result->signature(), operands, &memrefs))
    return EmitErrors(results, std::move(err), exec_ctx);

  // If execution failed errors will be automatically allocated for all results.
  ReturnValueConverter converter(results);
  converter.AddConversion(ReturnAsyncMemrefAsDenseHostTensor);
  converter.AddConversion(ReturnAsyncToken);
  if (auto err = compilation_result->Execute(memrefs, converter, exec_ctx))
    return;

  // Keep operands alive if we have unavailable results.
  RunWhenReady(results.values(),
               [operands = RCArray<AsyncValue>(operands.values())] {});
}

void RegisterCpuRuntimeKernels(KernelRegistry* registry) {
  registry->AddKernel("cpurt.compile", TFRT_KERNEL(Compile));
  registry->AddKernel("cpurt.execute", TFRT_KERNEL(Execute));
}

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
