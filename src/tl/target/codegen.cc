/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file target/codegen.cc
 */

#include "codegen.h"

#include <tvm/arith/analyzer.h>
#include <tvm/runtime/registry.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "../op/op.h"

namespace tvm {
namespace codegen {

CodeGenTL::CodeGenTL() { restrict_keyword_ = "__restrict__"; }

void CodeGenTL::PrintFuncPrefix(std::ostream& os) { os << "extern \"C\" __global__ "; }

class LaunchConfigExtractor : public tir::StmtVisitor {
 private:
  void VisitStmt_(const AttrStmtNode* op) final {
    if (op->attr_key == tir::attr::thread_extent) {
      IterVar iv = Downcast<IterVar>(op->node);
      if (iv->var->name_hint == "threadIdx.x" || iv->thread_tag == "threadIdx.x") {
        threadIdx_x_ext = op->value;
      } else if (iv->var->name_hint == "threadIdx.y" || iv->thread_tag == "threadIdx.y") {
        threadIdx_y_ext = op->value;
      } else if (iv->var->name_hint == "threadIdx.z" || iv->thread_tag == "threadIdx.z") {
        threadIdx_z_ext = op->value;
      }
    }
    StmtVisitor::VisitStmt_(op);
  }

 public:
  PrimExpr threadIdx_x_ext = Integer(1);
  PrimExpr threadIdx_y_ext = Integer(1);
  PrimExpr threadIdx_z_ext = Integer(1);
};

void CodeGenTL::PrintExtraAttrs(const PrimFunc& f) {
  LaunchConfigExtractor extractor;
  extractor(f->body);
  arith::Analyzer analyzer;
  PrimExpr threadIdx_ext = analyzer.Simplify(extractor.threadIdx_x_ext * extractor.threadIdx_y_ext *
                                             extractor.threadIdx_z_ext);
  if (const IntImmNode* const threadIdx_ext_int = threadIdx_ext.as<IntImmNode>()) {
    if (threadIdx_ext_int->value == 1) {
      // unable to extract the number of threads per block, hence directly return
      return;
    }
    stream << " __launch_bounds__(" << threadIdx_ext_int->value << ")";
  }
}

std::string CodeGenTL::Finish() {
  decl_stream << "#include <tl_templates/gemm.h>\n";
  decl_stream << "#include <tl_templates/copy.h>\n";
  decl_stream << "#include <tl_templates/reduce.h>\n";
  decl_stream << "#include <tl_templates/threadblock_swizzle.h>\n";
  decl_stream << "\n";
  return CodeGenC::Finish();
}

void CodeGenTL::VisitStmt_(const tir::ForNode* op) {
  if (op->kind == tir::ForKind::kUnrolled) {
    PrintIndent();
    stream << "#pragma unroll\n";
  }
  std::string extent = PrintExpr(arith::Analyzer().Simplify(op->extent + op->min));
  PrintIndent();
  std::string vid = AllocVarID(op->loop_var.get());
  std::string start = PrintExpr(op->min);
  stream << "for (";
  PrintType(op->loop_var.dtype(), stream);
  stream << ' ' << vid << " = " << start << "; " << vid << " < " << extent << "; ++" << vid
         << ") {\n";
  int for_scope = BeginScope();
  PrintStmt(op->body);
  this->EndScope(for_scope);
  PrintIndent();
  stream << "}\n";
}

void CodeGenTL::BindThreadIndex(const IterVar& iv) {
  ICHECK(!var_idmap_.count(iv->var.get()));
  var_idmap_[iv->var.get()] = CastFromTo(iv->thread_tag, DataType::UInt(32), iv->var.dtype());
}

void CodeGenTL::PrintType(DataType t, std::ostream& os) {  // NOLINT(*)
  int lanes = t.lanes();
  if (t.is_handle()) {
    ICHECK(t.is_scalar()) << "do not yet support vector types";
    os << "void*";
    return;
  }

  if (t.is_void()) {
    os << "void";
    return;
  }

  bool fail = false;
  if (t.is_float()) {
    switch (t.bits()) {
      case 16:
        if (t.is_scalar()) {
          os << "half_t";
        } else if (lanes <= 8) {
          // Emit CUDA code to access fp16 vector elements.
          //
          // half4 is stored as uint2
          //
          // h4.x is emitted as *(half2*)(&(u2.x)).x
          // h4.y is emitted as *(half2*)(&(u2.x)).y
          // h4.z is emitted as *(half2*)(&(u2.y)).x
          // h4.w is emitted as *(half2*)(&(u2.y)).y
          //
          ICHECK_EQ(lanes % 2, 0) << "only support even lane for half type";
          os << "uint" << lanes / 2;
        } else {
          fail = true;
        }
        break;
      case 32:
        if (lanes <= 4) {
          os << "float";
        } else if (lanes <= 8) {
          // Emit CUDA code to access fp32 vector elements for 4 < lanes <= 8.
          //
          // float8 is stored as ulonglong4
          //
          // f8.v1 is emitted as *(float2*)(&(ul4.x)).x
          // f8.v2 is emitted as *(float2*)(&(ul4.x)).y
          //
          ICHECK_EQ(lanes % 2, 0) << "only support even lane for float type with lanes > 4";
          os << "ulonglong" << lanes / 2;
        } else {
          fail = true;
        }
        break;
      case 64:
        os << "double";
        break;
      default:
        fail = true;
        break;
    }
    if (!fail && (t.is_scalar() || t.bits() == 16)) return;
    if (!fail && (lanes > 4 && lanes <= 8 && t.bits() == 32)) return;
    if (!fail && (lanes >= 2 && lanes <= 4)) {
      os << lanes;
      return;
    }
  } else if (t.is_bfloat16()) {
    if (t.is_scalar()) {
      os << "bfloat16_t";
    } else if (lanes <= 8) {
      ICHECK_EQ(lanes % 2, 0) << "only support even lane for half type";
      os << "uint" << lanes / 2;
    } else {
      fail = true;
    }
    if (!fail) return;
  } else if (t.is_float8()) {
    if (t.is_scalar()) {
      os << "unsigned char";  // __nv_fp8_storage_t is an alias of unsigned char
    } else if (lanes == 2) {
      os << "unsigned short int";  // __nv_fp8x2_storage_t is an alias of unsigned short
    } else if (lanes == 4) {
      os << "unsigned int";  // __nv_fp8x4_storage_t is an alias of unsigned int
    } else {
      fail = true;
    }
    if (!fail) return;
  } else if (t == DataType::Bool()) {
    os << "bool";
    return;
  } else if (t.is_vector_bool()) {
    // CUDA does not support bool vectors.
    // Use ushort vectors to represent instead.
    int n = t.lanes();
    if (n <= 4) {
      os << "ushort" << n;
      return;
    }
  } else if (t.is_uint() || t.is_int()) {
    if (t.is_uint()) {
      os << "u";
    }
    switch (t.bits()) {
      case 1: {
        if (t.is_scalar()) {
          os << "int";
          return;
        } else if (t.lanes() == 8) {
          os << "int8_t";
          return;
        } else if (t.lanes() == 16) {
          os << "int16_t";
          return;
        } else if (t.lanes() == 32) {
          os << "int";
          return;
        } else {
          LOG(FATAL) << "Cannot convert type " << t << " to CUDA type!";
        }
      }
      case 4: {
        if (t.is_scalar()) {
          os << "int";
          return;
        } else if (t.lanes() == 4) {
          os << "int16_t";
          return;
        } else if (t.lanes() == 8) {
          // directly 8 4-bit int in integer.
          os << "int";
          return;
        } else if (t.lanes() == 16) {
          os << "int2";
          return;
        } else if (t.lanes() == 32) {
          os << "int4";
          return;
        } else if (t.lanes() == 64) {
          os << "int8";
          return;
        } else {
          LOG(FATAL) << "Cannot convert type " << t << " to CUDA type!";
        }
      }
      case 8: {
        if (t.lanes() == 4) {
          // directly 4 8 bit int in integer.

          // We use int for int8x4 instead of char4 because using char4 is
          // likely to produce extra instructions to pack four int8 elements
          // into 32-bit data.
          os << "int";
          return;
        } else if (t.lanes() == 8) {
          os << "int2";
          return;
        } else if (t.lanes() == 16) {
          os << "int4";
          return;
        } else if (!t.is_uint() && t.is_scalar()) {
          os << "signed char";
          break;
        } else {
          os << "char";
          break;
        }
      }
      case 16: {
        if (t.is_scalar()) {
          os << "short";
        } else if (t.lanes() <= 4) {
          os << "short" << lanes;
        } else if (t.lanes() <= 8) {
          // Emit CUDA code to access int16 vector elements.
          //
          // short4 is stored as int2
          //
          // s4.x is emitted as *(short2*)(&(i2.x)).x
          // s4.y is emitted as *(short2*)(&(i2.x)).y
          // s4.z is emitted as *(short2*)(&(i2.y)).x
          // s4.w is emitted as *(short2*)(&(i2.y)).y
          //
          ICHECK_EQ(t.lanes() % 2, 0) << "only support even lane for shorT type with lanes > 4";
          os << "int" << t.lanes() / 2;
        } else {
          fail = true;
        }
        if (!fail) {
          return;
        }
        break;
      }
      case 32: {
        if (t.is_scalar()) {
          os << "int";
        } else if (t.lanes() <= 4) {
          os << "int" << t.lanes();
        } else if (t.lanes() <= 8) {
          // Emit CUDA code to access int32 vector elements for 4 < lanes <= 8.
          //
          // int8 is stored as longlong4
          //
          // i8.v1 is emitted as *(int2*)(&(l4.x)).x
          // i8.v2 is emitted as *(int2*)(&(l4.x)).y
          //
          ICHECK_EQ(lanes % 2, 0) << "only support even lane for int32 type with lanes > 4";
          os << "longlong" << lanes / 2;
        } else {
          fail = true;
        }
        if (!fail) {
          return;
        }
        break;
      }
      case 64: {
        if (t.is_scalar()) {
          os << "int64_t";
        } else if (t.lanes() == 2) {
          os << "longlong2";
        } else if (t.lanes() == 3) {
          os << "longlong3";
        } else if (t.lanes() == 4) {
          os << "longlong4";
        }
        return;
      }
      default:
        fail = true;
        break;
    }
    if (!fail && lanes == 1) {
      return;
    }
    if (!fail && (lanes >= 2 && lanes <= 4)) {
      os << lanes;
      return;
    }
  }
  LOG(FATAL) << "Cannot convert type " << t << " to CUDA type";
}

void CodeGenTL::PrintVecBinaryOp(const std::string& op, DataType t, PrimExpr lhs, PrimExpr rhs,
                                 std::ostream& os) {  // NOLINT(*)
  // Delcare the result.
  std::string sret = name_supply_->FreshName("_");
  this->PrintIndent();
  this->PrintType(t, stream);
  stream << ' ' << sret << ";\n";
  int ssa_scope = BeginScope();
  {
    // Unpack into individual ops.
    std::string vlhs = SSAGetID(PrintExpr(lhs), lhs.dtype());
    std::string vrhs = SSAGetID(PrintExpr(rhs), rhs.dtype());

    for (int i = 0, lanes = t.lanes(); i < lanes; ++i) {
      std::ostringstream value_temp;
      if (isalpha(op[0])) {
        value_temp << op << "(";
        PrintVecElemLoad(vlhs, lhs.dtype(), i, value_temp);
        value_temp << ", ";
        PrintVecElemLoad(vrhs, rhs.dtype(), i, value_temp);
        value_temp << ")";
      } else {
        value_temp << "(";
        PrintVecElemLoad(vlhs, lhs.dtype(), i, value_temp);
        value_temp << op;
        PrintVecElemLoad(vrhs, rhs.dtype(), i, value_temp);
        value_temp << ")";
      }
      PrintVecElemStore(sret, t, i, value_temp.str());
    }
  }
  EndScope(ssa_scope);
  os << sret;
}

void CodeGenTL::PrintVecElemLoad(const std::string& vec, DataType t, int i,
                                 std::ostream& os) {  // NOLINT(*)
  if (t.is_scalar()) {
    os << vec;
    return;
  }

  static const char access[] = {'x', 'y', 'z', 'w'};
  ICHECK(i >= 0 && i < (t.bits() == 8 ? 16 : (t.bits() == 16 || t.bits() == 32) ? 8 : 4));
  if (t.bits() == 8 && (t.is_int() || t.is_uint())) {
    std::string type_name = t.is_int() ? "char" : "unsigned char";
    if (t.lanes() == 2 || t.lanes() == 3) {
      os << vec << "." << access[i % t.lanes()];
    } else {
      std::string ac = t.lanes() == 4 ? vec : (vec + "." + access[i / 4]);
      os << "((" << type_name << ")(" << ac << " >> " << i % 4 * 8 << "))";
    }
  } else if (t.is_float16()) {
    os << "((half2*)(&(" << vec << "." << access[i / 2] << ")))->" << access[i % 2];
  } else if (t.is_bfloat16()) {
    os << "((nv_bfloat162*)(&(" << vec << "." << access[i / 2] << ")))->" << access[i % 2];
  } else if (t.lanes() > 4 && t.lanes() <= 8) {
    std::string type_name;
    if (t.bits() == 16) {
      if (t.is_int()) {
        type_name = "short";
      } else if (t.is_uint()) {
        type_name = "ushort";
      }
    } else if (t.bits() == 32) {
      if (t.is_int()) {
        type_name = "int";
      } else if (t.is_uint()) {
        type_name = "uint";
      } else if (t.is_float()) {
        type_name = "float";
      }
    }
    ICHECK(!type_name.empty());
    os << "((" << type_name << "2*)(&(" << vec << "." << access[i / 2] << ")))->" << access[i % 2];
  } else {
    os << vec << "." << access[i];
  }
}

void CodeGenTL::PrintVecElemStore(const std::string& vec, DataType t, int i,
                                  const std::string& value) {
  this->PrintIndent();
  static const char access[] = {'x', 'y', 'z', 'w'};
  ICHECK(i >= 0 && i < (t.bits() == 8 ? 16 : (t.bits() == 16 || t.bits() == 32) ? 8 : 4));
  if (t.bits() == 8 && (t.is_int() || t.is_uint())) {
    if (t.lanes() == 2 || t.lanes() == 3) {
      stream << vec << '.' << access[i % t.lanes()] << "="
             << "(" << value << ");\n";
    } else {
      std::string ac = t.lanes() == 4 ? vec : (vec + "." + access[i / 4]);
      stream << ac << "=";
      // Do not read the first undef lane.
      if (i != 0) {
        stream << ac << " & ~(0x000000ff << " << i % 4 * 8 << ") |";
      }
      stream << "(" << value << " << " << i % 4 * 8 << ");\n";
    }
  } else if (t.is_float16()) {
    stream << "((half2*)(&(" << vec << "." << access[i / 2] << ")))->" << access[i % 2] << " = "
           << value << ";\n";
  } else if (t.is_bfloat16()) {
    stream << "((nv_bfloat162*)(&(" << vec << "." << access[i / 2] << ")))->" << access[i % 2]
           << " = " << value << ";\n";
  } else if (t.lanes() > 4 && t.lanes() <= 8) {
    std::string type_name;
    if (t.bits() == 16) {
      if (t.is_int()) {
        type_name = "short";
      } else if (t.is_uint()) {
        type_name = "ushort";
      }
    } else if (t.bits() == 32) {
      if (t.is_int()) {
        type_name = "int";
      } else if (t.is_uint()) {
        type_name = "uint";
      } else if (t.is_float()) {
        type_name = "float";
      }
    }
    ICHECK(!type_name.empty());
    stream << "((" << type_name << "2*)(&(" << vec << "." << access[i / 2] << ")))->"
           << access[i % 2] << " = " << value << ";\n";
  } else {
    stream << vec << "." << access[i] << " = " << value << ";\n";
  }
}

void CodeGenTL::PrintStorageSync(const CallNode* op) {
  const std::string& sync = op->args[0].as<StringImmNode>()->value;
  if (sync == "warp") {
    // DO nothing.
  } else if (sync == "shared" || sync == "shared.dyn") {
    this->PrintIndent();
    this->stream << "__syncthreads();\n";
  }
}

void CodeGenTL::PrintStorageScope(const std::string& scope, std::ostream& os) {  // NOLINT(*)
  ICHECK_NE(scope, "global") << "Cannot allocate global memory when targeting CUDA. You must pass "
                                "all global arrays as input instead";
  if (scope == "shared") {
    os << "__shared__ ";
  } else if (scope == "shared.dyn") {
    os << "extern __shared__ ";
  }
}

std::string CodeGenTL::CastFromTo(std::string value, DataType from, DataType target) {
  if (from == target) return value;
  std::ostringstream os;
  os << "((";
  this->PrintType(target, os);
  os << ")";
  if (from.is_float16() && (target.is_int() || target.is_uint()) && target.bits() == 8) {
    os << "(";
    if (target.is_uint()) {
      os << "u";
    }
    os << "int)";
  }
  os << value << ")";
  return os.str();
}

void CodeGenTL::VisitExpr_(const CastNode* op, std::ostream& os) {
  DataType from_ty = op->value.dtype();
  DataType target_ty = op->dtype;
  ICHECK_EQ(target_ty.lanes(), from_ty.lanes());

  // Emit simple C-style type conversion.
  if (from_ty.is_scalar()) return CodeGenC::VisitExpr_(op, os);

  // We could emit make_float4 like calls, but the emitted code looks
  // too compact to read. Emit this as vectorized unary ops.
  std::string sret = name_supply_->FreshName("_");
  this->PrintIndent();
  this->PrintType(target_ty, stream);
  stream << ' ' << sret << ";\n";
  {
    std::string src = SSAGetID(PrintExpr(op->value), from_ty);
    for (int i = 0, lanes = from_ty.lanes(); i < lanes; ++i) {
      std::ostringstream val;
      val << "(";
      PrintType(target_ty.element_of(), val);
      val << ")(";
      PrintVecElemLoad(src, from_ty, i, val);
      val << ")";
      PrintVecElemStore(sret, target_ty, i, val.str());
    }
  }
  os << sret;
}

void CodeGenTL::PrintCallExtern(Type ret_type, String global_symbol, const Array<PrimExpr>& args,
                                bool skip_first_arg, std::ostream& os) {  // NOLINT(*)
  DataType ret_dtype = GetRuntimeDataType(ret_type);
  if (ret_dtype.is_vector()) {
    //
    // Emit an unsupported vector call
    //
    // v = intrin_f((float4*)A[0], (float4*)B[0])
    //
    // as
    //
    // float4 __ret;
    // {
    //   float4 __arg0 = ((float4*)A)[0];
    //   float4 __arg1 = ((float4*)B)[0];
    //   __ret.x = intrin_f(__arg0.x, __arg1.x);
    //   __ret.y = intrin_f(__arg0.y, __arg1.y);
    //   __ret.z = intrin_f(__arg0.z, __arg1.z);
    //   __ret.w = intrin_f(__arg0.w, __arg1.w);
    // }
    // v = __ret;
    //
    // Declare the result vector.
    std::string sret = name_supply_->FreshName("_");
    this->PrintIndent();
    this->PrintType(ret_dtype, stream);
    stream << ' ' << sret << ";\n";
    {
      // Load arguments.
      std::vector<std::string> sargs;
      size_t arg_begin = static_cast<size_t>(skip_first_arg);
      for (size_t i = arg_begin; i < args.size(); ++i) {
        std::string val = SSAGetID(PrintExpr(args[i]), args[i].dtype());
        sargs.push_back(std::move(val));
      }

      // Emit a scalar call for each lane.
      for (int i = 0; i < ret_dtype.lanes(); ++i) {
        std::ostringstream scall;
        scall << global_symbol << "(";
        for (size_t j = 0; j < sargs.size(); ++j) {
          if (j > 0) scall << ", ";
          PrintVecElemLoad(sargs[j], args[arg_begin + j].dtype(), i, scall);
        }
        scall << ")";
        PrintVecElemStore(sret, ret_dtype, i, scall.str());
      }
    }
    os << sret;
  } else {
    CodeGenC::PrintCallExtern(ret_type, global_symbol, args, skip_first_arg, os);
  }
}

void CodeGenTL::VisitExpr_(const CallNode* op, std::ostream& os) {
  if (op->op.same_as(builtin::ptx_cp_async())) {
    std::string dst = this->PrintExpr(op->args[0]);
    std::string dst_offset = this->PrintExpr(op->args[1]);
    std::string src = this->PrintExpr(op->args[2]);
    std::string src_offset = this->PrintExpr(op->args[3]);
    std::string size = this->PrintExpr(op->args[4]);
    // use size of argument list to indicate whether or not to use predicated cp.async
    if (op->args.size() == 5) {
      this->PrintIndent();
      this->stream << "tl::cp_async_gs<" << size << ">(" << dst << "+" << dst_offset << ", " << src
                   << "+" << src_offset << ");\n";
    } else {
      std::string condition = this->PrintExpr(op->args[5]);
      this->PrintIndent();
      this->stream << "tl::cp_async_gs_conditional<" << size << ">(" << dst << "+" << dst_offset
                   << ", " << src << "+" << src_offset << ", " << condition << ");\n";
    }
  } else if (op->op.same_as(builtin::ptx_commit_group())) {
    this->PrintIndent();
    this->stream << "tl::cp_async_commit();\n";
  } else if (op->op.same_as(builtin::ptx_wait_group())) {
    this->PrintIndent();
    int n = Downcast<IntImm>(op->args[0])->value;
    this->stream << "tl::cp_async_wait<" << n << ">();\n";
  } else {
    CodeGenC::VisitExpr_(op, os);
  }
}

void CodeGenTL::VisitStmt_(const AttrStmtNode* op) {
  if (op->attr_key == tir::attr::async_commit_queue_scope) {
    const IntImmNode* queue_id = op->value.as<IntImmNode>();
    ICHECK(queue_id && queue_id->value == 0) << "For CUDA, the index of an async queue must be 0.";
    this->VisitStmt(op->body);
    auto commit_group = Call(DataType::Void(), builtin::ptx_commit_group(), {});
    this->VisitExpr(commit_group, this->stream);
    return;
  } else if (op->attr_key == tir::attr::async_wait_queue_scope) {
    auto wait_attrs = GetAsyncWaitAttributes(op);
    auto queue_id = wait_attrs.first.as<IntImmNode>();
    ICHECK(queue_id && queue_id->value == 0) << "For CUDA, the index of an async queue must be 0.";
    auto wait_cnt = wait_attrs.second;
    auto wait_group = Call(DataType::Void(), builtin::ptx_wait_group(), {wait_cnt});
    this->VisitExpr(wait_group, this->stream);
    auto inner = op->body.as<AttrStmtNode>();
    ICHECK(inner);
    this->VisitStmt(inner->body);
    return;
  } else if (op->attr_key == "threadblock_swizzle_pattern") {
    this->PrintIndent();
    const StringImmNode* pattern = op->value.as<StringImmNode>();
    ICHECK(pattern);
    this->stream << "const dim3 blockIdx = " << pattern->value << "();\n";
    this->VisitStmt(op->body);
    return;
  }
  CodeGenC::VisitStmt_(op);
}

void CodeGenTL::VisitStmt_(const AllocateNode* op) {
  ICHECK(!is_zero(op->condition));
  std::string vid = AllocVarID(op->buffer_var.get());

  this->PrintIndent();
  std::string scope = GetPtrStorageScope(op->buffer_var);
  PrintStorageScope(scope, stream);
  PrintType(op->dtype, stream);

  if (scope == "shared.dyn") {
    stream << ' ' << vid << "[];\n";
  } else {
    size_t constant_size = op->ConstantAllocationSize();
    ICHECK_GT(constant_size, 0) << "Can only handle constant size stack allocation for now";

    if ((op->dtype == DataType::Int(4) || op->dtype == DataType::UInt(4) ||
         op->dtype == DataType::Int(1)) &&
        scope == "shared") {
      constant_size = constant_size / (32 / op->dtype.bits());
    }
    stream << ' ' << vid << '[' << constant_size << "];\n";
  }

  RegisterHandleType(op->buffer_var.get(), op->dtype);
  this->PrintStmt(op->body);
}

void CodeGenTL::VisitExpr_(const RampNode* op, std::ostream& os) {
  CHECK_LE(op->lanes, 4) << "ValueError: Ramp of more than 4 lanes is not allowed.";
  os << "(make_";
  PrintType(op->dtype, os);
  os << "(";
  for (int i = 0; i < op->lanes; i++) {
    os << "(" << PrintExpr(op->base) << ")"
       << "+(" << PrintExpr(op->stride) << "*" << i << ")";
    if (i != op->lanes - 1) os << ", ";
  }
  os << "))";
}

void CodeGenTL::VisitExpr_(const BroadcastNode* op, std::ostream& os) {  // NOLINT(*)
  if ((op->dtype.is_int() || op->dtype.is_uint()) && op->dtype.bits() == 8 && op->lanes == 4) {
    // make_int8x4
    const int64_t* p = as_const_int(op->value);
    ICHECK(p);
    int64_t v = *p & 0xFF;
    v = (v << 24) | (v << 16) | (v << 8) | v;
    if (op->dtype.is_uint()) {
      os << "(uint)" << v;
    } else {
      os << "(int)" << v;
    }
    return;
  }

  if (op->dtype.is_float16()) {
    std::string v = PrintExpr(op->value);
    os << "make_";
    PrintType(op->dtype, os);
    os << '(';
    for (int i = 0; i < op->lanes / 2; ++i) {
      if (i != 0) os << ", ";
      os << "__pack_half2(" << v << ", " << v << ")";
    }
    os << ')';
    return;
  }

  if (op->dtype.is_bfloat16()) {
    std::string v = PrintExpr(op->value);
    os << "make_";
    PrintType(op->dtype, os);
    os << '(';
    for (int i = 0; i < op->lanes / 2; ++i) {
      if (i != 0) os << ", ";
      os << "__pack_nv_bfloat162(" << v << ", " << v << ")";
    }
    os << ')';
    return;
  }

  if (op->dtype.is_float() && op->dtype.bits() == 32 && op->dtype.lanes() == 8) {
    std::string v = PrintExpr(op->value);
    os << "make_ulonglong4(";
    for (int i = 0; i < 4; ++i) {
      if (i != 0) os << ", ";
      os << "*(unsigned long long*)&make_float2(" << v << ", " << v << ")";
    }
    os << ')';
    return;
  }

  if ((op->dtype.is_int() || op->dtype.is_uint()) && op->dtype.bits() == 4) {
    bool fail = false;
    const int64_t* p = as_const_int(op->value);
    ICHECK(p);
    int64_t v = *p & 0xF;

    if (op->lanes == 4) {
      v = (v << 12) | (v << 8) | (v << 4) | v;
      if (op->dtype.is_uint()) {
        os << "(uint16_t)" << v;
      } else {
        os << "(int16_t)" << v;
      }
    } else {
      v = (v << 28) | (v << 24) | (v << 20) | (v << 16) | (v << 12) | (v << 8) | (v << 4) | v;
      if (op->lanes == 8) {
        if (op->dtype.is_uint()) {
          os << "(uint)" << v;
        } else {
          os << "(int)" << v;
        }
      } else if (op->lanes == 16 || op->lanes == 32) {
        os << "make_";
        PrintType(op->dtype, os);
        os << '(';
        for (int i = 0; i < op->lanes / 8; ++i) {
          if (i != 0) os << ", ";
          if (op->dtype.is_uint()) {
            os << "(uint)" << v;
          } else {
            os << "(int)" << v;
          }
        }
        os << ')';
      } else {
        fail = true;
      }
    }

    if (!fail) {
      return;
    }
  }

  std::string v = PrintExpr(op->value);
  os << "make_";
  PrintType(op->dtype, os);
  os << '(';
  for (int i = 0; i < op->lanes; ++i) {
    if (i != 0) os << ", ";
    os << v;
  }
  os << ')';
}

inline void PrintConst(const FloatImmNode* op, std::ostream& os, CodeGenTL* p) {  // NOLINT(*)
  // Type code is kBFloat
  if (op->dtype.is_bfloat16()) {
    os << "bfloat16_t";
    os << '(' << std::scientific << op->value << 'f' << ')';
    return;
  }
  // Type code is kFloat
  switch (op->dtype.bits()) {
    case 64:
    case 32: {
      std::ostringstream temp;
      if (std::isinf(op->value)) {
        if (op->value < 0) {
          temp << "-";
        }
        temp << ((op->dtype.bits() == 32) ? "CUDART_INF_F" : "CUDART_INF");
      } else if (std::isnan(op->value)) {
        temp << ((op->dtype.bits() == 32) ? "CUDART_NAN_F" : "CUDART_NAN");
      } else {
        temp << std::scientific << op->value;
        if (op->dtype.bits() == 32) temp << 'f';
      }
      p->MarkConst(temp.str());
      os << temp.str();
      break;
    }
    case 16: {
      os << "half_t" << '(';
      FloatImm const_f32 = FloatImm(DataType::Float(32), op->value);
      PrintConst(const_f32.get(), os, p);
      os << ')';
      break;
    }
    default:
      LOG(FATAL) << "Bad bit-width for float: " << op->dtype << "\n";
  }
}

void CodeGenTL::VisitExpr_(const FloatImmNode* op, std::ostream& os) {  // NOLINT(*)
  PrintConst(op, os, this);
}

void CodeGenTL::HandleVolatileLoads(const std::string& value, const BufferLoadNode* op,
                                    std::ostream& os) {
  // Cast away volatile qualifier for fp16 types. That is, only loads and
  // stores are volatile. The loaded objects are not marked as volatile.
  //
  if ((op->dtype.is_float16() || op->dtype.is_bfloat16()) && IsVolatile(op->buffer->data.get())) {
    os << "(";
    PrintType(op->dtype, os);
    os << ")(" << value << ")";
  } else {
    os << value;
  }
}

void CodeGenTL::PrintVecElemLoadExpr(DataType t, int i, const std::string& value,
                                     std::ostream& os) {
  ICHECK_GT(t.lanes(), 1);
  if (t.bits() == 8 && (t.is_int() || t.is_uint())) {
    if (!(t.lanes() == 2 || t.lanes() == 3)) {
      if (i != 0) {
        os << "|";
      }
      os << "((0x000000ff << " << i * 8 << ") & (" << value << " << " << i * 8 << "))";
      return;
    }
  }

  if (t.is_float16()) {
    if (i == 0) {
      os << "make_";
      PrintType(t, os);
      os << '(';
    }
    if (i % 2 == 0) {
      os << "__pack_half2(" << value;
    } else {
      os << "," << value << ")";
      if (i != t.lanes() - 1) {
        os << ",";
      } else {
        os << ")";
      }
    }
    return;
  }

  if (t.is_bfloat16()) {
    if (i == 0) {
      os << "make_";
      PrintType(t, os);
      os << '(';
    }
    if (i % 2 == 0) {
      os << "__pack_bfloat162(" << value;
    } else {
      os << "," << value << ")";
      if (i != t.lanes() - 1) {
        os << ",";
      } else {
        os << ")";
      }
    }
    return;
  }

  if (i == 0) {
    os << "make_";
    PrintType(t, os);
    os << "(";
  }
  os << value;
  if (i != t.lanes() - 1) {
    os << ",";
  } else {
    os << ")";
  }
  return;
}

}  // namespace codegen
}  // namespace tvm
