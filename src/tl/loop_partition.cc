/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
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
  * \file loop_partition.cc
  * \brief Partition parallel loops onto threads
  */

#include "loop_partition.h"

#include <tvm/tir/stmt_functor.h>

namespace tvm {
namespace tl {

using namespace tir;

class BufferIndiceSimplify : public StmtExprMutator {
 public:
  BufferIndiceSimplify(arith::Analyzer *analyzer) : analyzer_(analyzer) {}
 private:
  PrimExpr VisitExpr_(const BufferLoadNode* node) final {
    auto visited = StmtExprMutator::VisitExpr_(node);
    auto n = visited.as<BufferLoad>().value();
    auto nptr = n.CopyOnWrite();
    nptr->indices = nptr->indices.Map([&] (const auto& e) { return analyzer_->Simplify(e); });
    return n;
  }
  Stmt VisitStmt_(const BufferStoreNode* node) final {
    auto visited = StmtExprMutator::VisitStmt_(node);
    auto n = visited.as<BufferStore>().value();
    auto nptr = n.CopyOnWrite();
    nptr->indices = nptr->indices.Map([&] (const auto& e) { return analyzer_->Simplify(e); });
    return n;
  }
  arith::Analyzer *analyzer_;
};

// Rewrite the parallel loop into a common loop, which is mapped to threads
For PartitionLoop(const ForNode* op, const Var& thread, arith::Analyzer* analyzer, const Fragment& loop_layout) {
  ICHECK(loop_layout.defined());
  ICHECK(thread.defined());
  int old_loop_depth = loop_layout->InputDim();
  int new_loop_depth = loop_layout->OutputDim();

  // Create the new loop iter var
  Array<Var> vars;
  for (int i = 0; i < new_loop_depth; i++) {
    Var var = Var(std::string{ char('i' + i) });
    vars.push_back(var);
  }
  vars.push_back(thread);

  // create the substitute map, and the loop body
  Map<Var, PrimExpr> vmap;
  Stmt body = GetRef<Stmt>(op);
  auto inv_loop = loop_layout->Inverse();
  auto indices = inv_loop->Forward(vars.Map([](const Var& v) { return PrimExpr(v); }));
  for (int i = 0; i < old_loop_depth; i++) {
    For loop = body.as<For>().value();
    vmap.Set(loop->loop_var, indices[i]);
    body = loop->body;
  }

  // substitute and re-construct the serial loop
  body = Substitute(body, vmap);
  for (int i = new_loop_depth - 1; i >= 0; i--) {
    body = For(vars[i], make_zero(vars[i]->dtype), inv_loop->InputShape()[i], ForKind::kSerial, body);
    analyzer->Bind(vars[i], Range(0, inv_loop->InputShape()[i]));
  }

  body = BufferIndiceSimplify(analyzer)(body);

  body = LoopPragmaUnroll(body);

  return body.as<For>().value();
}

class LoopPramaUnroller : public StmtExprMutator {
 public:
  LoopPramaUnroller() = default;
 private:
  Stmt VisitStmt_(const ForNode* node) final {
    if (node->kind == ForKind::kSerial) {
      For new_for = GetRef<For>(node);
      auto for_ptr = new_for.CopyOnWrite();
      for_ptr->annotations.Set("pragma_unroll_explicit", Bool(false));
      for_ptr->kind = ForKind::kUnrolled;
      return new_for;
    }
    return StmtExprMutator::VisitStmt_(node);
  }
};

class LoopPartitioner : public StmtExprVisitor {
 public:
  LoopPartitioner() = default;

  Fragment Partition(const ForNode* op, int num_thread) {
    this->VisitStmt_(op);
    int loop_size_full = 1;
    PrimExpr flattened = 0;
    for (size_t i = 0; i < loop_vars_.size(); i++) {
      auto ext_ptr = as_const_int(loop_vars_[i]->dom->extent);
      ICHECK(ext_ptr);
      int extent = *ext_ptr;
      loop_size_full *= extent;
      flattened = flattened * extent + loop_vars_[i]->var;
    }
    if (loop_size_full % num_thread != 0) {
      ICHECK("Not Implemented case.");
    }
    return Fragment(loop_vars_, { FloorDiv(flattened, num_thread) }, { FloorMod(flattened, num_thread) }, {});
  }

private:
  void VisitStmt_(const ForNode* node) final {
    if (node->kind == ForKind::kParallel) {
      body_ = node->body;
      loop_vars_.push_back(IterVar(Range::FromMinExtent(node->min, node->extent), node->loop_var, IterVarType::kDataPar));
    }
    StmtExprVisitor::VisitStmt_(node);
  }

  Stmt body_;
  PrimExpr flattened = 0;
  Array<IterVar> loop_vars_;
};

For PartitionLoop(const ForNode* op, const Var& thread, arith::Analyzer* analyzer, size_t num_thread) {
  LoopPartitioner partitioner;
  Fragment fragment = partitioner.Partition(op, num_thread);
  return PartitionLoop(op, thread, analyzer, fragment);
}

Stmt LoopPragmaUnroll(Stmt stmt){
  LoopPramaUnroller unroller;
  return unroller(stmt);
}

} // namespace tl
} // namespace tvm