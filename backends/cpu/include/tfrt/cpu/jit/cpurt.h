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

//===- cpurt.h --------------------------------------------------*- C++ -*-===//
//
// Support library for implementing TFRT kernels that do JIT compilation using
// MLIR framework (generating kernels at runtime from hight level MLIR
// dialects, e.g. generating dense linear algebra kernels from Linalg dialect).
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_BACKENDS_CPU_JIT_CPURT_H_
#define TFRT_BACKENDS_CPU_JIT_CPURT_H_

#include <cstdint>
#include <type_traits>

#include "mlir/Dialect/Async/IR/AsyncTypes.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "tfrt/cpu/jit/async_runtime.h"
#include "tfrt/cpu/jit/async_runtime_api.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/msan.h"

namespace tfrt {

class ExecutionContext;
class Tensor;

namespace cpu {
namespace jit {

// Forward declare the result of compiling MLIR module to the executable.
class CompilationResult;

struct CompilationOptions {
  // Byte alignment for allocated memrefs. Depending on the compiler flags
  // Tensorflow requires tensors to be aligned on 16, 32 or 64 bytes.
  int alignment = 0;

  // The number of worker threads (host context concurrent work queue size) that
  // can be used for parallelizing compute intensive parts of the kernel.
  int num_worker_threads = 0;

  // LLVM optimization level when JIT compiling a kernel.
  Optional<llvm::CodeGenOpt::Level> jit_code_opt_level;

  // Register dialects that are allowed in the serialized module.
  llvm::function_ref<void(mlir::DialectRegistry&)> register_dialects;

  // Register a pass pipeline that lowers serialized module from high level
  // dialects to the dialects supported by the CPURT lowering to LLVM.
  llvm::function_ref<void(mlir::OpPassManager&)> register_pass_pipeline;
};

// Compiles a kernel defined by the serialized MLIR module to the executable
// compilation result.
Expected<CompilationResult> CompileKernelMlirModule(
    string_view mlir_module, string_view entrypoint,
    const CompilationOptions& opts);

//----------------------------------------------------------------------------//
// Types for passing compiled kernel arguments and passing back results.
//----------------------------------------------------------------------------//

struct MemrefDesc {
  void* data;
  ssize_t offset;
  SmallVector<ssize_t, 4> sizes;
  SmallVector<ssize_t, 4> strides;
};

// Verifies that the runtime buffer is compatible with the memref type (same
// rank and statically known dimensions are matched with the runtime
// dimensions).
Error VerifyMemrefOperand(mlir::MemRefType type, MemrefDesc memref);

// Converts tfrt Tensor to the Memref Descriptor and verifies that the Tensor
// value is compatible with the memref type.
Expected<MemrefDesc> ConvertTensorToMemrefDesc(mlir::MemRefType type,
                                               const Tensor& tensor);

//----------------------------------------------------------------------------//
// Conversions from compiled kernel results to the TFRT AsyncValues.
//----------------------------------------------------------------------------//

// Converts returned values of `async::TokenType` type to the async chains.
mlir::LogicalResult ReturnAsyncToken(RemainingResults results,
                                     unsigned result_index, mlir::Type type,
                                     void* result_ptr);

// Converts returned values of `async<memref<...>>` type to the async values
// of DenseHostTensor type.
mlir::LogicalResult ReturnAsyncMemrefAsDenseHostTensor(RemainingResults results,
                                                       unsigned result_index,
                                                       mlir::Type type,
                                                       void* result_ptr);

// Converts returned values of `memref<...>` type to the async values of
// DenseHostTensor type.
mlir::LogicalResult ReturnMemrefAsDenseHostTensor(RemainingResults results,
                                                  unsigned result_index,
                                                  mlir::Type type,
                                                  void* result_ptr);

// Converts returned memref values to Tensors using user provided Converter
// that must implement this concept:
//
// struct ConvertMemrefToTensor {
//   using ResultType = MyTensorType;  // must be movable
//
//   template <typename T, int rank>
//   static MyTensorType Convert(void* memref_ptr) {
//     auto* memref = static_cast<StridedMemRefType<T, rank>*>(memref_ptr);
//     return MyTensorType>(memref.basePtr, memref.data, ...);
//   }
// };
//
template <typename Converter>
mlir::LogicalResult ReturnStridedMemref(RemainingResults results,
                                        unsigned result_index, mlir::Type type,
                                        void* result_ptr) {
  using ResultType = typename Converter::ResultType;
  static_assert(std::is_move_constructible<ResultType>::value,
                "Conversion result type must be move constructible");

  // Check if the type is a valid memref.
  auto memref = type.dyn_cast<mlir::MemRefType>();
  if (!memref) return mlir::failure();

  // Dispatch to the correct extract function based on rank.
  auto rank_dispatch = [&](auto type_tag) {
    using T = decltype(type_tag);
    int64_t rank = memref.getRank();

    auto convert_and_emplace = [&](auto rank_tag) {
      constexpr int rank = decltype(rank_tag)::value;
      results.EmplaceAt<ResultType>(
          result_index, Converter::template Convert<T, rank>(result_ptr));
    };

    if (rank == 0)
      convert_and_emplace(std::integral_constant<int, 0>{});
    else if (rank == 1)
      convert_and_emplace(std::integral_constant<int, 1>{});
    else if (rank == 2)
      convert_and_emplace(std::integral_constant<int, 2>{});
    else if (rank == 3)
      convert_and_emplace(std::integral_constant<int, 3>{});
    else if (rank == 4)
      convert_and_emplace(std::integral_constant<int, 4>{});
    else if (rank == 5)
      convert_and_emplace(std::integral_constant<int, 5>{});
    else
      // TODO(ezhulenev): To simplify conversion from a void* pointer to memref
      // descriptor we rely on the StridedMemrefType<T, rank> and dispatch
      // only up to a fixed rank.
      results.EmitErrorAt(result_index,
                          StrCat("unsupported returned memref rank: ", rank));
  };

  // Dispatch based on the memref element type.
  auto element_type = memref.getElementType();
  if (element_type.isF32())
    rank_dispatch(float{});
  else
    results.EmitErrorAt(
        result_index,
        StrCat("unsupported returned memref element type: ", element_type));

  return mlir::success();
}

namespace internal {

// Adaptor that creates a function compatible with `ExtractAsyncValue` from
// the `Converter` concept compatible with `ReturnStridedMemref`.
template <typename Converter, typename T, int rank>
void Emplace(void* memref_ptr, AsyncValue* dst) {
  using ResultType = typename Converter::ResultType;
  dst->emplace<ResultType>(Converter::template Convert<T, rank>(memref_ptr));
}

}  // namespace internal

// Converts returned async memref values to Tensors using user provided
// Converter that must compatible with `ReturnStridedMemref` define above.
template <typename Converter>
mlir::LogicalResult ReturnAsyncStridedMemref(RemainingResults results,
                                             unsigned result_index,
                                             mlir::Type type,
                                             void* result_ptr) {
  using ResultType = typename Converter::ResultType;
  static_assert(std::is_move_constructible<ResultType>::value,
                "Conversion result type must be move constructible");

  auto value_type = type.dyn_cast<mlir::async::ValueType>();
  if (!value_type) return mlir::failure();

  // Load the pointer to the async value from a pointer to result storage.
  TFRT_MSAN_MEMORY_IS_INITIALIZED(result_ptr, sizeof(void*));
  void* ret = *reinterpret_cast<void**>(result_ptr);
  auto* value = static_cast<mlir::runtime::AsyncValue*>(ret);

  // We already verified that return value is an async value of memref.
  auto memref = value_type.getValueType().cast<mlir::MemRefType>();

  // Allocate constructed async value to be returned to the caller.
  auto dst = [&]() -> AsyncValue* {
    return results.AllocateAt<ResultType>(result_index).get();
  };

  // Dispatch to the correct extract function based on rank.
  auto rank_dispatch = [&](auto type_tag) {
    using T = decltype(type_tag);
    int64_t rank = memref.getRank();

    if (rank == 0)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 0>);
    else if (rank == 1)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 1>);
    else if (rank == 2)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 2>);
    else if (rank == 3)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 3>);
    else if (rank == 4)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 4>);
    else if (rank == 5)
      ExtractAsyncValue(value, dst(), internal::Emplace<Converter, T, 5>);
    else
      // TODO(ezhulenev): Because ExtractAsyncValue takes a llvm::function_ref
      // we can't pass a runtime arguments to emplace functions via lambda
      // capture, because the value might become available asynchronously and
      // this will lead to use after free. Consider adding an std::function
      // alternative for ranks higher then 5? Lambdas with small captures should
      // be stack allocated anyway, however it is implementation defined.
      results.EmitErrorAt(result_index,
                          StrCat("unsupported returned memref rank: ", rank));
  };

  // Dispatch based on the memref element type.
  auto element_type = memref.getElementType();
  if (element_type.isF32())
    rank_dispatch(float{});
  else
    results.EmitErrorAt(
        result_index,
        StrCat("unsupported returned memref element type: ", element_type));

  return mlir::success();
}

// Return value converter class allows to register custom functions for
// converting compiled kernel execution results to returned async values.
class ReturnValueConverter {
 public:
  explicit ReturnValueConverter(RemainingResults results);

  // Converts value `ret` of type `type` returned from the compiled function at
  // `result_index` return position using registered conversion functions, and
  // emplaces the result async value. If the conversion failed returns a failure
  // and sets the result async value to error.
  mlir::LogicalResult ReturnValue(unsigned result_index, mlir::Type type,
                                  void* ret) const;

  // Forward error to all remaining results.
  void EmitErrors(RCReference<ErrorAsyncValue>& error);

  // Adds a conversion function to this converter. Conversion callback must be
  // convertible to the `ConversionCallbackFn` function type:
  //   mlir::LogicalResult(RemainingResults, unsigned, mlir::Type, void*)
  //
  // Conversion function must return `success` if it successfully handled the
  // return type and set the result async value. If conversion function returns
  // `failure` converter will try the next conversion function.
  //
  // When attempting to convert a retuned value via 'ReturnValue', the most
  // recently added conversions will be invoked first.
  template <typename FnT>
  void AddConversion(FnT&& callback) {
    conversion_callbacks_.emplace_back(std::forward<FnT>(callback));
  }

 private:
  using ConversionCallbackFn = llvm::function_ref<mlir::LogicalResult(
      RemainingResults, unsigned, mlir::Type, void*)>;

  RemainingResults results_;
  SmallVector<ConversionCallbackFn, 4> conversion_callbacks_;
};

//----------------------------------------------------------------------------//
// Result of compiling MLIR module to executable kernel function.
//----------------------------------------------------------------------------//

// Constructs error async value from the `error` and returns it for all results.
void EmitErrors(RemainingResults results, Error error,
                const ExecutionContext& exec_ctx);
Error EmitErrors(ReturnValueConverter results, Error error,
                 const ExecutionContext& exec_ctx);

// TODO(ezhulenev): Compilation result does not need to keep MLIRContext alive,
// it only needs the entrypoint FunctionType. Implement a function to "clone"
// signature type into the new MLIRContext, because original context potentially
// can have large constant attribute that will waste the memory.
//
// Another option is to write custom type class to store signature type, because
// the number of supported types is relatively small.

class CompilationResult {
 public:
  // Forward declare struct defined below.
  struct ResultsMemoryLayout;
  struct CallFrame;

  CompilationResult(std::unique_ptr<mlir::MLIRContext> context,
                    std::unique_ptr<mlir::ExecutionEngine> engine,
                    mlir::FunctionType signature, string_view entrypoint,
                    ResultsMemoryLayout results_memory_layout)
      : context_(std::move(context)),
        engine_(std::move(engine)),
        signature_(signature),
        fptr_(*engine_->lookup(entrypoint)),
        results_memory_layout_(std::move(results_memory_layout)) {
    assert(fptr_ != nullptr && "entrypoint was not found");
  }

  // Initializes call frame by adding all operands as pointers to arguments
  // vector. Also allocates storage for returned values, which are passed to the
  // compiled kernel as return value arguments.
  //
  // See mlir::ExecutionEngine `packFunctionArguments` for the details.
  Error InitializeCallFrame(ArrayRef<MemrefDesc> operands,
                            CallFrame* call_frame) const;

  // Converts returned values owned by the callframe using provided value
  // converter. If result conversion fails emits error async value.
  Error ReturnResults(const ReturnValueConverter& results,
                      CallFrame* call_frame) const;

  // Executes compiled function with given operands. If operands passed at
  // runtime are not compatible with the compiled function signature, allocates
  // error async values for each returned value.
  Error Execute(ArrayRef<MemrefDesc> operands,
                const ReturnValueConverter& results,
                const ExecutionContext& exec_ctx) const;

  // Executes compiled function using user provided call frame.
  void Execute(const ExecutionContext& exec_ctx, CallFrame* call_frame) const;

  mlir::FunctionType signature() const;

  bool IsAsync() const { return results_memory_layout_.has_async_results; }

  // CallFrame provides a pointer-stable storage for packed function arguments
  // and storage for returned values.
  struct CallFrame {
    // Pointers to compiled kernel arguments.
    llvm::SmallVector<void*> args;

    // We use single block of memory to store compiled kernel results. We need
    // to be able to store pointers to async values and tokens, and strided
    // memrefs which at runtime are represented as StridedMemrefType<T, rank>.
    //
    // Currently we only need to provide result storage for pointers and memref
    // sizes and strides (int64_t type). If we'll need to support more complex
    // return types we'll have to be more careful about alignment requirements.
    static_assert(sizeof(uintptr_t) == sizeof(int64_t),
                  "uintptr_t size must be the same as int64_t");

    // Memory where the compiled kernel will write its results.
    llvm::SmallVector<uint8_t, 128> results;
  };

  // Requirements for the contiguous block of memory to store compiled function
  // results. When we invoke a compiled fuction we allocate a block of memory,
  // and pass pointers to pre-computed offsets as output arguments to the
  // function.
  struct ResultsMemoryLayout {
    bool has_async_results;             // true iff returns async results
    size_t size;                        // number of bytes required
    llvm::SmallVector<size_t> offsets;  // ofssets in the block of memory
  };

  // Verifies that all types in the entrypoint function signature are supported
  // at runtime and we know how to pass arguments and fetch results. Returns
  // a pre-computed layout for the function results. If some of the operands
  // or results are not supported returns an error.
  static Expected<ResultsMemoryLayout> VerifyEntrypointSignature(
      mlir::FunctionType signature);

 private:
  // Pointer to a compiled kernel function.
  using KernelFunctionPtr = void (*)(void**);

  std::unique_ptr<mlir::MLIRContext> context_;
  std::unique_ptr<mlir::ExecutionEngine> engine_;
  mlir::FunctionType signature_;
  KernelFunctionPtr fptr_;
  ResultsMemoryLayout results_memory_layout_;
};

//----------------------------------------------------------------------------//
// Cache all compilation results in the resource context owned by the host.
//----------------------------------------------------------------------------//

class CompilationResultCache {
 public:
  explicit CompilationResultCache(HostContext* host) : host_(host) {}
  AsyncValueRef<CompilationResult> Find(intptr_t key) const;
  AsyncValueRef<CompilationResult> Insert(intptr_t key,
                                          CompilationResult compilation_result);

 private:
  HostContext* host_;
  mutable tfrt::mutex mu_;
  llvm::DenseMap<intptr_t, AsyncValueRef<CompilationResult>> cache_
      TFRT_GUARDED_BY(mu_);
};

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt

#endif  // TFRT_BACKENDS_CPU_JIT_CPURT_H_
