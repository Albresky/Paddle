// Copyright (c) 2024 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/cinn/optim/schedule_block_dce_pass.h"

#include "paddle/cinn/ir/ir_mutator.h"
#include "paddle/cinn/ir/stmt_visitors.h"

namespace cinn {
namespace optim {
using ir::stmt::_Block_;
using ir::stmt::Alloc;
using ir::stmt::BlockRef;
using ir::stmt::Evaluate;
using ir::stmt::For;
using ir::stmt::Free;
using ir::stmt::IfThenElse;
using ir::stmt::Let;
using ir::stmt::Schedule;
using ir::stmt::StmtRef;
using ir::stmt::Store;

class DSBNamesCollectorInStmt : public ir::stmt::StmtVisitor<> {
 public:
  explicit DSBNamesCollectorInStmt(
      std::unordered_set<std::string>* dead_schedule_block_names,
      std::unordered_set<std::string>* output_names)
      : dead_schedule_block_names_(dead_schedule_block_names),
        output_names_(output_names) {}

  void operator()(const BlockRef& block) {
    dead_schedule_block_names_->clear();
    ir::stmt::StmtVisitor<>::VisitBlock(block);
  }

 private:
  void VisitStmt(const IfThenElse& stmt) override {
    VisitBlock(stmt->true_case());
    if (stmt->false_case().defined()) {
      VisitBlock(stmt->false_case());
    }
  }

  void VisitStmt(const For& stmt) override { VisitBlock(stmt->body()); }

  void VisitStmt(const Schedule& stmt) override { VisitBlock(stmt->body()); }

  void VisitStmt(const Let& stmt) override {
    UpdateDeadScheduleBlocks(stmt->body());
  }

  void VisitStmt(const Store& stmt) override {
    UpdateDeadScheduleBlocks(stmt->value());
  }

  void VisitStmt(const Evaluate& stmt) override {}

  void VisitStmt(const Alloc& stmt) override {}

  void VisitStmt(const Free& stmt) override {}

  void UpdateDeadScheduleBlocks(const ir::Expr& expr) {
    std::unordered_set<std::string> load_buffer_names;
    std::unordered_set<std::string> load_tensor_names;
    auto InsertLoadTensorAndBufferNames = [&](const ir::Expr* x) -> void {
      if (const ir::Load* load = x->As<ir::Load>()) {
        load_buffer_names.insert(load->tensor.as_tensor()->buffer->name);
        load_tensor_names.insert(load->tensor.as_tensor()->name);
      }
    };
    InsertLoadTensorAndBufferNames(&expr);

    auto IsShareBufferWithLoadedTensor =
        [&](const ir::_Tensor_* tensor) -> bool {
      return load_buffer_names.count(tensor->buffer->name) > 0;
    };
    auto IsLoadedTensor = [&](const ir::_Tensor_* tensor) -> bool {
      return load_tensor_names.count(tensor->name) > 0;
    };
    auto IsOutputTensor = [&](const ir::_Tensor_* tensor) -> bool {
      return output_names_->count(tensor->name) > 0;
    };
    auto IsDeadStore = [&](const ir::Store* store) -> bool {
      const ir::_Tensor_* tensor = store->tensor.as_tensor();
      return !IsOutputTensor(tensor) && !IsLoadedTensor(tensor) &&
             !IsShareBufferWithLoadedTensor(tensor);
    };
    auto InsertDeadStoreName = [&](const ir::Expr* x) -> void {
      const ir::Store* store = x->As<ir::Store>();
      if (store != nullptr && IsDeadStore(store)) {
        VLOG(6) << "Find dead schedule block names: \n"
                << store->tensor.as_tensor()->name;
        dead_schedule_block_names_->insert(store->tensor.as_tensor()->name);
      }
    };
    InsertDeadStoreName(&expr);
  }

  std::unordered_set<std::string>* dead_schedule_block_names_;
  std::unordered_set<std::string>* output_names_;
};

class ScheduleBlockDCE {
 public:
  explicit ScheduleBlockDCE(const std::vector<std::string>& output_names)
      : output_names_(output_names.begin(), output_names.end()) {}

  void operator()(BlockRef block) {
    DSBNamesCollectorInStmt collector(&dead_schedule_block_names_,
                                      &output_names_);
    collector(block);
    while (!dead_schedule_block_names_.empty()) {
      VisitBlock(block);
      DSBNamesCollectorInStmt collector(&dead_schedule_block_names_,
                                        &output_names_);
    }
  }

 private:
  void VisitBlock(BlockRef block) {
    const auto& stmts = block->stmts();
    std::unordered_set<int> need_remove_ids;
    for (int i = 0; i < block->stmts().size(); ++i) {
      if ((stmts[i].isa<Schedule>() && VisitStmt(stmts[i].as<Schedule>())) ||
          (stmts[i].isa<IfThenElse>() &&
           VisitStmt(stmts[i].as<IfThenElse>())) ||
          (stmts[i].isa<For>() && VisitStmt(stmts[i].as<For>()))) {
        need_remove_ids.insert(i);
      }
    }

    if (!need_remove_ids.empty()) {
      std::vector<StmtRef> new_stmts;
      for (int i = 0; i < block->stmts().size(); ++i) {
        VLOG(6) << "[TEST] Remove dead schedule block: \n" << i << "\n";
        if (need_remove_ids.count(i) == 0) {
          new_stmts.push_back(block->stmts()[i]);
        }
      }
      block->set_stmts(new_stmts);
    }
  }

  bool VisitStmt(IfThenElse stmt) {
    VisitBlock(stmt->true_case());
    if (stmt->false_case().defined()) {
      VisitBlock(stmt->false_case());
    }
    return (IsEmptyIf(stmt));
  }

  bool VisitStmt(For stmt) {
    VisitBlock(stmt->body());
    return (IsEmptyBlock(stmt->body()));
  }

  bool VisitStmt(const Schedule& stmt) {
    return !stmt->block_fields().empty() &&
           dead_schedule_block_names_.count(stmt->name()) > 0;
  }

  bool IsEmptyStmt(const StmtRef& stmt) {
    if (stmt->block_fields().empty()) return false;
    for (const BlockRef& block : stmt->block_fields()) {
      if (!IsEmptyBlock(block)) return false;
    }
    return true;
  }

  bool IsEmptyBlock(const BlockRef& block) {
    if (block->stmts().empty()) return false;
    for (const StmtRef& stmt : block->stmts()) {
      if (!IsEmptyStmt(stmt)) return false;
    }
    return true;
  }

  bool IsEmptyIf(const IfThenElse& stmt) {
    if (stmt->false_case().defined()) {
      return IsEmptyBlock(stmt->true_case()) &&
             IsEmptyBlock(stmt->false_case());
    }
    return IsEmptyBlock(stmt->true_case());
  }

 private:
  std::unordered_set<std::string> dead_schedule_block_names_;
  std::unordered_set<std::string> output_names_;
};

LogicalResult EliminateDeadScheduleBlockPass::Run(BlockRef stmt) {
  EliminateDeadScheduleBlock(stmt);
  return LogicalResult::success();
}

std::unique_ptr<BlockPass> CreateEliminateDeadScheduleBlockPass(
    const std::vector<std::string>& output_names) {
  return std::make_unique<EliminateDeadScheduleBlockPass>(output_names);
}

void EliminateDeadScheduleBlockPass::EliminateDeadScheduleBlock(
    BlockRef block) {
  ScheduleBlockDCE eliminator(this->output_names);
  eliminator(block);
}

}  // namespace optim
}  // namespace cinn
