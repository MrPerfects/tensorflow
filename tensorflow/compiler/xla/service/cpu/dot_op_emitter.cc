/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/cpu/dot_op_emitter.h"

#include <memory>
#include <vector>

#include "absl/strings/str_cat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_runtime.h"
#include "tensorflow/compiler/xla/service/cpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/cpu/target_machine_features.h"
#include "tensorflow/compiler/xla/service/cpu/tiled_dot_emitter.h"
#include "tensorflow/compiler/xla/service/cpu/vector_support_library.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/llvm_ir/kernel_support_library.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/logging.h"

namespace xla {

using llvm_ir::SetToFirstInsertPoint;

namespace cpu {
DotOpEmitter::DotOpEmitter(const HloInstruction& dot,
                           const llvm_ir::IrArray& target_array,
                           const llvm_ir::IrArray& lhs_array,
                           const llvm_ir::IrArray& rhs_array,
                           const llvm_ir::IrArray* addend_array,
                           llvm::Value* executable_run_options_value,
                           llvm::IRBuilder<>* b,
                           const HloModuleConfig& hlo_module_config,
                           const TargetMachineFeatures& target_machine_features)
    : dot_(dot),
      target_array_(target_array),
      lhs_array_(lhs_array),
      rhs_array_(rhs_array),
      addend_array_(addend_array),
      executable_run_options_value_(executable_run_options_value),
      b_(b),
      hlo_module_config_(hlo_module_config),
      target_machine_features_(target_machine_features) {}

/* static */ Status DotOpEmitter::EmitDotOperation(
    const HloInstruction& dot, const llvm_ir::IrArray& target_array,
    const llvm_ir::IrArray& lhs_array, const llvm_ir::IrArray& rhs_array,
    const llvm_ir::IrArray* addend_array,
    llvm::Value* executable_run_options_value, llvm::IRBuilder<>* b,
    const HloModuleConfig& hlo_module_config,
    const TargetMachineFeatures& target_machine_features) {
  PrimitiveType type = target_array.GetShape().element_type();
  TF_RET_CHECK(F16 == type || F32 == type || F64 == type || C64 == type);
  DotOpEmitter dot_emitter(dot, target_array, lhs_array, rhs_array,
                           addend_array, executable_run_options_value, b,
                           hlo_module_config, target_machine_features);
  return dot_emitter.Emit();
}

bool DotOpEmitter::EmitSmallGemmIfProfitable(
    const DotOpEmitter::MatMultDims& mat_mult_dims) {
  if (ShouldUseMultiThreadedEigen()) {
    return false;
  }

  if (!EnableExperimentalLlvmIrGemm()) {
    // TODO(sanjoy):  We should make these numbers micro-arch specific.
    bool small_gemm = mat_mult_dims.k <= 128 &&
                      ((mat_mult_dims.m <= 32 && mat_mult_dims.n <= 128) ||
                       (mat_mult_dims.m <= 128 && mat_mult_dims.n <= 32));
    if (!small_gemm) {
      return false;
    }
  }

  if (mat_mult_dims.lhs_non_canonical || mat_mult_dims.rhs_non_canonical) {
    return false;
  }

  PrimitiveType primitive_type = dot_.shape().element_type();

  switch (primitive_type) {
    default:
      return false;

    case F32:
    case F64:
    case S32:
    case S64:
      break;
  }

  if (!(mat_mult_dims.lhs_column_major == mat_mult_dims.rhs_column_major &&
        mat_mult_dims.rhs_column_major == mat_mult_dims.target_column_major)) {
    return false;
  }

  llvm::Value* lhs = lhs_array_.GetBasePointer();
  llvm::Value* rhs = rhs_array_.GetBasePointer();
  llvm::Value* target = target_array_.GetBasePointer();
  int64 m = mat_mult_dims.m;
  int64 k = mat_mult_dims.k;
  int64 n = mat_mult_dims.n;

  if (mat_mult_dims.lhs_column_major) {
    std::swap(lhs, rhs);
    std::swap(m, n);
  }

  int64 size_bytes = m * n * ShapeUtil::ByteSizeOfPrimitiveType(primitive_type);
  b_->CreateMemSet(
      target, b_->getInt8(0), size_bytes,
      target_machine_features_.minimum_alignment_for_allocation(size_bytes));

  int64 max_target_vector_width =
      target_machine_features_.vector_register_num_elements(
          *b_->GetInsertBlock()->getParent(), primitive_type);

  int64 tile_size_m, tile_size_k, tile_size_n_in_vector_width;
  std::tie(tile_size_m, tile_size_k, tile_size_n_in_vector_width) =
      GetGemmTileSize();

  const bool enable_fast_math =
      hlo_module_config_.debug_options().xla_cpu_enable_fast_math();
  const bool optimize_for_size =
      options::OptimizeForSizeRequested(hlo_module_config_);

  EmitSmallGemm(
      /*scalar_type=*/primitive_type,
      /*m=*/m, /*k=*/k, /*n=*/n,
      /*max_vectorization_width=*/max_target_vector_width,
      /*max_vector_count=*/tile_size_n_in_vector_width,
      /*min_vectorization_width=*/std::min<int64>(4, max_target_vector_width),
      /*tile_size_m=*/tile_size_m, /*tile_size_k=*/tile_size_k, /*lhs=*/lhs,
      /*rhs=*/rhs, /*result=*/target, b_,
      /*enable_fast_math=*/enable_fast_math,
      /*optimize_for_size=*/optimize_for_size);

  return true;
}

bool DotOpEmitter::EmitLlvmIrDotIfProfitable() {
  if (dot_.shape().dimensions_size() != 2) {
    return false;
  }

  PrimitiveType primitive_type = dot_.shape().element_type();

  if (!primitive_util::IsFloatingPointType(primitive_type) &&
      !primitive_util::IsIntegralType(primitive_type)) {
    return false;
  }

  MatMultDims mat_mult_dims = GetMatMultDims();
  bool is_column_major_matrix_vector = false;
  bool is_row_major_matrix_vector = false;

  int64 m, k;
  bool swap_operands;

  if (mat_mult_dims.m == 1) {
    bool rhs_effectively_row_major =
        mat_mult_dims.rhs_non_canonical ^ !mat_mult_dims.rhs_column_major;
    if (rhs_effectively_row_major) {
      k = mat_mult_dims.k;
      m = mat_mult_dims.n;
      is_column_major_matrix_vector = true;
      swap_operands = true;
    } else {
      k = mat_mult_dims.k;
      m = mat_mult_dims.n;
      is_row_major_matrix_vector = true;
      swap_operands = true;
    }
  }

  if (mat_mult_dims.n == 1) {
    bool lhs_effectively_column_major =
        mat_mult_dims.lhs_non_canonical ^ mat_mult_dims.lhs_column_major;
    if (lhs_effectively_column_major) {
      m = mat_mult_dims.m;
      k = mat_mult_dims.k;
      is_column_major_matrix_vector = true;
      swap_operands = false;
    } else {
      m = mat_mult_dims.m;
      k = mat_mult_dims.k;
      is_row_major_matrix_vector = true;
      swap_operands = false;
    }
  }

  if (!is_column_major_matrix_vector && !is_row_major_matrix_vector) {
    return EmitSmallGemmIfProfitable(mat_mult_dims);
  }

  int64 tiling_factor = GetGemvTilingFactor();
  CHECK_GT(tiling_factor, 0);

  llvm::Value* result_op = target_array_.GetBasePointer();
  llvm::Value* lhs_op =
      swap_operands ? rhs_array_.GetBasePointer() : lhs_array_.GetBasePointer();
  llvm::Value* rhs_op =
      swap_operands ? lhs_array_.GetBasePointer() : rhs_array_.GetBasePointer();

  const bool enable_fast_math =
      hlo_module_config_.debug_options().xla_cpu_enable_fast_math();
  const bool optimize_for_size =
      options::OptimizeForSizeRequested(hlo_module_config_);

  const int target_vector_register_element_size =
      target_machine_features_.vector_register_num_elements(
          *b_->GetInsertBlock()->getParent(), primitive_type);

  // We may not always know the vector register size for the target we're
  // compiling against, in which case target_vector_register_element_size is 0.
  // In these cases we choose a default LLVM IR register size.
  const int kUnknownTargetVectorRegisterSize = 4;
  const int vector_register_element_size =
      target_vector_register_element_size == 0
          ? kUnknownTargetVectorRegisterSize
          : target_vector_register_element_size;

  if (is_column_major_matrix_vector) {
    VLOG(2) << "Emitting column major matrix-vector multiply with m = " << m
            << " and k = " << k;
    EmitColumnMajorGemv(
        /*scalar_type=*/primitive_type,
        /*tile_rows=*/vector_register_element_size, /*tile_cols=*/tiling_factor,
        /*m=*/m, /*k=*/k, /*lhs=*/lhs_op, /*rhs=*/rhs_op,
        /*addend=*/addend_array_ ? addend_array_->GetBasePointer() : nullptr,
        /*result=*/result_op, b_,
        /*enable_fast_math=*/enable_fast_math,
        /*optimize_for_size=*/optimize_for_size);
  } else {
    VLOG(2) << "Emitting row major matrix-vector multiply with m = " << m
            << " and k = " << k;
    EmitRowMajorGemv(
        /*scalar_type=*/primitive_type,
        /*tile_rows=*/tiling_factor,
        /*tile_cols=*/vector_register_element_size,
        /*m=*/m, /*k=*/k, /*lhs=*/lhs_op, /*rhs=*/rhs_op,
        /*addend=*/addend_array_ ? addend_array_->GetBasePointer() : nullptr,
        /*result=*/result_op, b_,
        /*enable_fast_math=*/enable_fast_math,
        /*optimize_for_size=*/optimize_for_size);
  }

  return true;
}

Status DotOpEmitter::Emit() {
  // The dot operation performs a sum of products over dimension 0 of the left
  // hand side operand and dimension 1 of the right hand side operand.
  //
  // Let the shapes of lhs and rhs be defined as below:
  //
  //   lhs = [L{n-1} x L{n-2} x ... L{0}]
  //   rhs = [R{m-1} x R{m-2} x ... R{0}]
  //
  // The sum-of-products dimension in the lhs has size L{0} and the dimension in
  // the rhs has size R{1}. Necessarily, then:
  //
  //   L{0} == R{1}
  //
  // The output of the operation has the following shape:
  //
  //   output = [L{n-1} x L{n-2} x ... L{1} x R{m-1} x R{m-2} x ... R{2} x R{0}]
  //
  // To perform the operation we construct a loop nest with one for-loop for
  // each dimension of the output. Inside this loop nest is another for-loop
  // which performs the sum-of-products (the reduction loop) before storing
  // the result in the output buffer.

  // This routine assumes that the dot operation is not in a parallelized
  // enclosing computation.
  CHECK(
      dot_.parent()->root_instruction()->outer_dimension_partitions().empty());

  const Shape& lhs_shape = lhs_array_.GetShape();
  const Shape& rhs_shape = rhs_array_.GetShape();

  if (ShapeUtil::IsScalar(lhs_shape) || ShapeUtil::IsScalar(rhs_shape)) {
    // If the operands are scalar, don't emit any loops.
    TF_RET_CHECK(ShapeUtil::IsScalar(lhs_shape) &&
                 ShapeUtil::IsScalar(rhs_shape));
    return EmitScalarDot();
  }

  if (EmitLlvmIrDotIfProfitable()) {
    return Status::OK();
  }

  CHECK_EQ(addend_array_, nullptr);

  if (PotentiallyImplementedAsEigenDot(dot_, target_machine_features_)) {
    return EmitCallToRuntime();
  }

  // Reduce along dimension 0 of the LHS and 1 of the RHS. Vectors are a special
  // case where the reduction dimension is 0 for both LHS and RHS. This results
  // in a vector dot product producing a scalar.
  int64 lhs_reduction_dimension =
      dot_.dot_dimension_numbers().lhs_contracting_dimensions(0);
  int64 rhs_reduction_dimension =
      dot_.dot_dimension_numbers().rhs_contracting_dimensions(0);

  // Verify the reduction dimension in the two operands are the same size.
  TF_RET_CHECK(lhs_shape.dimensions(lhs_reduction_dimension) ==
               rhs_shape.dimensions(rhs_reduction_dimension));

  bool lhs_reduction_along_minor_dimension =
      lhs_reduction_dimension == LayoutUtil::Minor(lhs_shape.layout(), 0);
  bool rhs_reduction_along_minor_dimension =
      rhs_reduction_dimension == LayoutUtil::Minor(rhs_shape.layout(), 0);

  // Create loop nests which loop through the LHS operand dimensions and the RHS
  // operand dimensions. The reduction dimension of the LHS and RHS are handled
  // in a separate innermost loop which performs the sum of products.
  llvm_ir::ForLoopNest loop_nest(llvm_ir::IrName(&dot_), b_);
  llvm_ir::IrArray::Index lhs_index = loop_nest.EmitOperandArrayLoopNest(
      lhs_array_, /*dimension_to_skip=*/lhs_reduction_dimension, "lhs");
  llvm_ir::IrArray::Index rhs_index = loop_nest.EmitOperandArrayLoopNest(
      rhs_array_, /*dimension_to_skip=*/rhs_reduction_dimension, "rhs");

  // Create the loop which does the sum of products reduction.
  //
  // The prevent_unrolling bit is working around a deficiency in LLVM's loop
  // vectorization pipeline, wherein in some cases unrolling a loop can prevent
  // effective vectorization.  Since we know that the IR we generate when
  // reducing across the minor dimension in both LHS and RHS is vectorized well
  // by the loop vectorizer, we block unrolling in that case to stop loop unroll
  // from messing up the vectorization.
  std::unique_ptr<llvm_ir::ForLoop> reduction_loop = loop_nest.AddLoop(
      0, lhs_shape.dimensions(lhs_reduction_dimension), "reduction",
      /*unroll_mode=*/
      (lhs_reduction_along_minor_dimension &&
       rhs_reduction_along_minor_dimension)
          ? xla::llvm_ir::UnrollMode::kNoUnroll
          : xla::llvm_ir::UnrollMode::kDefaultUnroll);

  // The final entry in the rhs and lhs indexes is the indvar of the
  // reduction loop.
  lhs_index[lhs_reduction_dimension] = reduction_loop->GetIndVarValue();
  rhs_index[rhs_reduction_dimension] = reduction_loop->GetIndVarValue();

  // For computing the sum of products we alloca a single location to store the
  // dot product result as we accumulate it within the reduction loop. After the
  // reduction loop we load the result and store into the output array.

  // Function entry basic block.
  // - Emit alloca for accumulator
  llvm::Function* func = reduction_loop->GetPreheaderBasicBlock()->getParent();
  SetToFirstInsertPoint(&func->getEntryBlock(), b_);
  llvm::Type* accum_type = target_array_.GetElementLlvmType();
  llvm::Value* accum_address =
      b_->CreateAlloca(accum_type, /*ArraySize=*/nullptr, "accum_address");

  // Preheader basic block of reduction loop:
  // - Initialize accumulator to zero.
  llvm::BasicBlock* preheader_bb = reduction_loop->GetPreheaderBasicBlock();
  b_->SetInsertPoint(preheader_bb->getTerminator());

  b_->CreateStore(llvm::Constant::getNullValue(accum_type), accum_address);

  // Body basic block of reduction loop:
  // - Load elements from lhs and rhs array.
  // - Multiply lhs-element and rhs-element.
  // - Load accumulator and add to product.
  // - Store sum back into accumulator.
  SetToFirstInsertPoint(reduction_loop->GetBodyBasicBlock(), b_);

  llvm::Value* lhs_element = lhs_array_.EmitReadArrayElement(lhs_index, b_);
  llvm::Value* rhs_element = rhs_array_.EmitReadArrayElement(rhs_index, b_);

  llvm::Value* accum = b_->CreateLoad(accum_address);
  llvm::Value* updated_accum;
  if (ShapeUtil::ElementIsComplex(lhs_shape)) {
    auto real = [&](llvm::Value* x) { return b_->CreateExtractValue(x, {0}); };
    auto imag = [&](llvm::Value* x) { return b_->CreateExtractValue(x, {1}); };
    llvm::Value* product_real =
        b_->CreateFSub(b_->CreateFMul(real(lhs_element), real(rhs_element)),
                       b_->CreateFMul(imag(lhs_element), imag(rhs_element)));
    llvm::Value* product_imag =
        b_->CreateFAdd(b_->CreateFMul(real(lhs_element), imag(rhs_element)),
                       b_->CreateFMul(imag(lhs_element), real(rhs_element)));
    updated_accum = b_->CreateInsertValue(
        accum, b_->CreateFAdd(real(accum), product_real), {0});
    updated_accum = b_->CreateInsertValue(
        updated_accum, b_->CreateFAdd(imag(accum), product_imag), {1});
  } else {
    llvm::Value* product = b_->CreateFMul(lhs_element, rhs_element);
    updated_accum = b_->CreateFAdd(accum, product);
  }
  b_->CreateStore(updated_accum, accum_address);

  // Exit basic block of reduction loop.
  // - Load accumulator value (the result).
  // - Store into output array.
  SetToFirstInsertPoint(reduction_loop->GetExitBasicBlock(), b_);

  llvm::Value* result = b_->CreateLoad(accum_address);

  // Create index into target address. The target index is the concatenation of
  // the rhs and lhs indexes with the reduction dimensions removed. The terms
  // from the rhs index are the lower dimensions in the index so we add them
  // first.
  llvm_ir::IrArray::Index target_index(lhs_index.GetType());
  for (int dimension = 0; dimension < lhs_index.size(); ++dimension) {
    if (dimension != lhs_reduction_dimension) {
      target_index.push_back(lhs_index[dimension]);
    }
  }
  for (int dimension = 0; dimension < rhs_index.size(); ++dimension) {
    if (dimension != rhs_reduction_dimension) {
      target_index.push_back(rhs_index[dimension]);
    }
  }

  target_array_.EmitWriteArrayElement(target_index, result, b_);

  // Set the IR builder insert point to the exit basic block of the outer most
  // loop.
  b_->SetInsertPoint(loop_nest.GetOuterLoopExitBasicBlock());

  return Status::OK();
}

Status DotOpEmitter::EmitScalarDot() {
  // A scalar dot is just a scalar multiply.
  llvm::Value* result;
  // Use the same index_type for all tensor accesses in the same kernel.
  llvm::Type* index_type = b_->getInt64Ty();
  llvm_ir::IrArray::Index element_index(index_type);
  llvm::Value* lhs_value =
      lhs_array_.EmitReadArrayElement(/*index=*/element_index, b_);
  llvm::Value* rhs_value =
      rhs_array_.EmitReadArrayElement(/*index=*/element_index, b_);
  if (ShapeUtil::ElementIsComplex(lhs_array_.GetShape())) {
    auto get_real = [&](llvm::Value* x) {
      return b_->CreateExtractValue(x, {0});
    };

    auto get_imag = [&](llvm::Value* x) {
      return b_->CreateExtractValue(x, {1});
    };

    llvm::Value* real = b_->CreateFSub(
        b_->CreateFMul(get_real(lhs_value), get_real(rhs_value)),
        b_->CreateFMul(get_imag(lhs_value), get_imag(rhs_value)));
    llvm::Value* imag = b_->CreateFAdd(
        b_->CreateFMul(get_real(lhs_value), get_imag(rhs_value)),
        b_->CreateFMul(get_imag(lhs_value), get_real(rhs_value)));
    result = llvm::ConstantAggregateZero::get(lhs_array_.GetElementLlvmType());
    result = b_->CreateInsertValue(result, real, {0});
    result = b_->CreateInsertValue(result, imag, {1});
  } else {
    result = b_->CreateFMul(lhs_value, rhs_value);
  }
  target_array_.EmitWriteArrayElement(/*index=*/element_index, result, b_);
  return Status::OK();
}

Status DotOpEmitter::EmitCallToRuntime() {
  // The signature of the Eigen runtime matmul function is:
  //
  //   (void)(void* run_options, float* out, float* lhs, float* rhs,
  //          int64 m, int64 n, int64 k, int32 transpose_lhs,
  //          int32 transpose_rhs);
  // The two transpose_... parameters are actually booleans, but we use int32
  // to avoid target-dependent calling convention details.

  bool multi_threaded = ShouldUseMultiThreadedEigen();
  bool use_mkl_dnn = hlo_module_config_.debug_options().xla_cpu_use_mkl_dnn();
  PrimitiveType type = target_array_.GetShape().element_type();
  llvm::Type* float_type;
  const char* fn_name;
  switch (type) {
    case F16:
      fn_name = multi_threaded
                    ? runtime::kEigenMatMulF16SymbolName
                    : runtime::kEigenSingleThreadedMatMulF16SymbolName;
      float_type = b_->getHalfTy();
      break;
    case F32:
      fn_name = multi_threaded
                    ? (use_mkl_dnn ? runtime::kMKLMatMulF32SymbolName
                                   : runtime::kEigenMatMulF32SymbolName)
                    : (use_mkl_dnn
                           ? runtime::kMKLSingleThreadedMatMulF32SymbolName
                           : runtime::kEigenSingleThreadedMatMulF32SymbolName);
      float_type = b_->getFloatTy();
      break;
    case F64:
      fn_name = multi_threaded
                    ? (use_mkl_dnn ? runtime::kMKLMatMulF64SymbolName
                                   : runtime::kEigenMatMulF64SymbolName)
                    : (use_mkl_dnn
                           ? runtime::kMKLSingleThreadedMatMulF64SymbolName
                           : runtime::kEigenSingleThreadedMatMulF64SymbolName);
      float_type = b_->getDoubleTy();
      break;
    default:
      return Unimplemented("Invalid type %s for dot operation",
                           PrimitiveType_Name(type));
  }

  llvm::Type* float_ptr_type = float_type->getPointerTo();
  llvm::Type* int64_type = b_->getInt64Ty();
  llvm::Type* int32_type = b_->getInt32Ty();
  llvm::Type* int8_ptr_type = b_->getInt8Ty()->getPointerTo();
  llvm::FunctionType* matmul_type = llvm::FunctionType::get(
      b_->getVoidTy(),
      {int8_ptr_type, float_ptr_type, float_ptr_type, float_ptr_type,
       int64_type, int64_type, int64_type, int32_type, int32_type},
      /*isVarArg=*/false);

  llvm::Function* function = b_->GetInsertBlock()->getParent();
  llvm::Module* module = function->getParent();

  llvm::Function* matmul_func = llvm::cast<llvm::Function>(
      module->getOrInsertFunction(fn_name, matmul_type));
  matmul_func->setCallingConv(llvm::CallingConv::C);
  matmul_func->setDoesNotThrow();
  matmul_func->setOnlyAccessesArgMemory();

  // The Eigen runtime function expects column-major layout. If the matrices are
  // row major, then use the following identity to compute the product:
  //
  //   (A x B)^T = B^T x A^T
  //
  // The connection between this identity and memory layout is that the
  // transpose operation can also be considered as an operation that changes the
  // memory layout of a matrix from row-major to column-major or vice versa.
  //
  // Effectively this involves swapping the 'lhs' with 'rhs' and 'm' with 'n'.

  MatMultDims mat_mult_dims = GetMatMultDims();

  CHECK_EQ(mat_mult_dims.lhs_column_major, mat_mult_dims.rhs_column_major);

  const llvm_ir::IrArray* lhs = &lhs_array_;
  const llvm_ir::IrArray* rhs = &rhs_array_;
  bool transpose_lhs = mat_mult_dims.lhs_non_canonical;
  bool transpose_rhs = mat_mult_dims.rhs_non_canonical;

  if (!mat_mult_dims.lhs_column_major) {
    std::swap(mat_mult_dims.m, mat_mult_dims.n);
    std::swap(lhs, rhs);
    std::swap(transpose_lhs, transpose_rhs);
  }

  b_->CreateCall(
      matmul_func,
      {b_->CreateBitCast(executable_run_options_value_, int8_ptr_type),
       b_->CreateBitCast(target_array_.GetBasePointer(), float_ptr_type),
       b_->CreateBitCast(lhs->GetBasePointer(), float_ptr_type),
       b_->CreateBitCast(rhs->GetBasePointer(), float_ptr_type),
       b_->getInt64(mat_mult_dims.m), b_->getInt64(mat_mult_dims.n),
       b_->getInt64(mat_mult_dims.k), b_->getInt32(transpose_lhs),
       b_->getInt32(transpose_rhs)});
  return Status::OK();
}

DotOpEmitter::MatMultDims DotOpEmitter::GetMatMultDims() const {
  CHECK_EQ(dot_.shape().dimensions_size(), 2);

  const Shape& lhs_shape = lhs_array_.GetShape();
  const Shape& rhs_shape = rhs_array_.GetShape();
  const DotDimensionNumbers& dim_nums = dot_.dot_dimension_numbers();

  return {
      /*m=*/lhs_shape.dimensions(1 - dim_nums.lhs_contracting_dimensions(0)),
      /*k=*/lhs_shape.dimensions(dim_nums.lhs_contracting_dimensions(0)),
      /*n=*/rhs_shape.dimensions(1 - dim_nums.rhs_contracting_dimensions(0)),
      /*lhs_column_major=*/LayoutUtil::Minor(lhs_shape.layout(), 0) == 0,
      /*lhs_non_canonical=*/dim_nums.lhs_contracting_dimensions(0) == 0,
      /*rhs_column_major=*/LayoutUtil::Minor(rhs_shape.layout(), 0) == 0,
      /*rhs_non_canonical=*/dim_nums.rhs_contracting_dimensions(0) == 1,
      /*target_column_major=*/
      LayoutUtil::Minor(target_array_.GetShape().layout(), 0) == 0};
}

// Return whether the given shape is rank 2.
static bool IsRank2(const Shape& shape) { return shape.rank() == 2; }

// In a gemm operation where output = lhs * rhs, check whether the given shapes
// are valid for the operation.
static bool AreValidGemmShapes(
    const Shape& lhs_shape, const Shape& rhs_shape, const Shape& output_shape,
    const TargetMachineFeatures& target_machine_features) {
  // The inputs and the output must
  // 1) be matrices with no padding, and
  // 2) have an allowed element type.
  PrimitiveType output_primitive_type = output_shape.element_type();
  if (!(output_primitive_type == F64 || output_primitive_type == F32 ||
        output_primitive_type == F16)) {
    return false;
  }

  if (!(IsRank2(lhs_shape) && IsRank2(rhs_shape) && IsRank2(output_shape))) {
    return false;
  }

  auto is_aligned = [&](const Shape& shape) {
    return GetMinimumAlignmentForArray(shape, target_machine_features) >=
           TargetMachineFeatures::kEigenExpectedTensorAlignment;
  };

  if (!is_aligned(lhs_shape) || !is_aligned(rhs_shape) ||
      !is_aligned(output_shape)) {
    return false;
  }

  return true;
}

bool PotentiallyImplementedAsEigenDot(
    const HloInstruction& hlo,
    const TargetMachineFeatures& target_machine_features) {
  // For certain types of Dot, we can call Eigen
  if (hlo.opcode() == HloOpcode::kDot) {
    const Shape& lhs_shape = hlo.operand(0)->shape();
    const Shape& rhs_shape = hlo.operand(1)->shape();

    if (ShapeUtil::IsZeroElementArray(lhs_shape) ||
        ShapeUtil::IsZeroElementArray(rhs_shape)) {
      return false;
    }

    if (ProfitableToImplementDotInTiledLlvmIr(hlo)) {
      return false;
    }

    // If gemm can accept the operand shapes, use it rather than a custom
    // kernel.
    if (AreValidGemmShapes(lhs_shape, rhs_shape, hlo.shape(),
                           target_machine_features)) {
      const DotDimensionNumbers& dim_numbers = hlo.dot_dimension_numbers();
      // The size of the reduction dimension should match. The shape inference
      // guarantees this invariant, so the check here is for programming
      // errors.
      CHECK_EQ(lhs_shape.dimensions(dim_numbers.lhs_contracting_dimensions(0)),
               rhs_shape.dimensions(dim_numbers.rhs_contracting_dimensions(0)));
      return true;
    }
  }

  return false;
}

// For vector-matrix dot products, it is always profitable to make the Rhs
// column major.
absl::optional<int64> ProfitableToMakeDotOperandColumnMajor(
    const HloInstruction& hlo) {
  if (hlo.opcode() == HloOpcode::kDot && hlo.shape().dimensions_size() == 2 &&
      hlo.shape().dimensions(0) == 1) {
    if (hlo.dot_dimension_numbers().rhs_contracting_dimensions(0) == 0) {
      return 1;
    }
    return {};
  }

  if (hlo.opcode() == HloOpcode::kFusion &&
      hlo.fusion_kind() == HloInstruction::FusionKind::kOutput) {
    auto* fusion_root =
        hlo.fused_instructions_computation()->root_instruction();
    if (fusion_root->opcode() != HloOpcode::kAdd) {
      return {};
    }

    for (auto* fusion_root_op : fusion_root->operands()) {
      if (fusion_root_op->opcode() != HloOpcode::kDot) {
        continue;
      }
      if (auto operand_num =
              ProfitableToMakeDotOperandColumnMajor(*fusion_root_op)) {
        auto* operand = fusion_root_op->operand(*operand_num);
        if (operand->opcode() == HloOpcode::kParameter &&
            operand->user_count() == 1) {
          return operand->parameter_number();
        }
      }
    }
  }

  return {};
}

bool ProfitableToImplementDotInTiledLlvmIr(const HloInstruction& dot) {
  // Any Matrix-Vector product of floating point or integral type, or
  // a transpose-dot fusion of the same can be lowered to a tiled LLVM
  // IR implementation.
  const Shape& shape = dot.shape();
  return shape.dimensions_size() == 2 &&
         (shape.dimensions(0) == 1 || shape.dimensions(1) == 1) &&
         (primitive_util::IsFloatingPointType(shape.element_type()) ||
          primitive_util::IsIntegralType(shape.element_type()));
}

}  // namespace cpu
}  // namespace xla
