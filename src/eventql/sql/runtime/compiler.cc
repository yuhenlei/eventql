/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <assert.h>
#include <stdlib.h>
#include <eventql/util/RegExp.h>
#include <eventql/sql/parser/astnode.h>
#include <eventql/sql/parser/token.h>
#include <eventql/sql/qtree/QueryTreeUtil.h>
#include <eventql/sql/runtime/compiler.h>
#include <eventql/sql/runtime/symboltable.h>
#include <eventql/sql/runtime/LikePattern.h>
#include <eventql/sql/svalue.h>

namespace csql {

ScopedPtr<vm::Program> Compiler::compile(
    Transaction* ctx,
    RefPtr<ValueExpressionNode> node,
    SymbolTable* symbol_table) {
  std::unique_ptr<vm::Program> output;
  auto rc = compile(ctx, node, symbol_table, &output);
  if (!rc.isSuccess()) {
    RAISE(kRuntimeError, rc.getMessage());
  }

  return std::move(output);
}

ReturnCode Compiler::compile(
    Transaction* ctx,
    RefPtr<ValueExpressionNode> node,
    SymbolTable* symbol_table,
    std::unique_ptr<vm::Program>* output) {
  auto program = mkScoped(new vm::Program());
  program->return_type = node->getReturnType();
  program->method_call = vm::EntryPoint(0);

  /* compile root expression */
  auto rc = compileExpression(node.get(), program.get(), symbol_table);
  if (!rc.isSuccess()) {
    return rc;
  }

  program->instructions.emplace_back(vm::X_RETURN, intptr_t(0));

  /* compile aggregate/accumulate subexpression */
  auto aggr_expr = QueryTreeUtil::findAggregateExpression(node.get());
  if (aggr_expr) {
    auto symbol = symbol_table->lookup(aggr_expr->getSymbol());
    if (!symbol) {
      return ReturnCode::errorf(
          "ERUNTIME",
          "symbol not found: $0",
          aggr_expr->getSymbol());
    }

    auto aggr_fun = symbol->getFunction();
    program->method_accumulate = vm::EntryPoint(program->instructions.size());
    program->instance_reset = aggr_fun->vtable.reset;
    program->instance_init = aggr_fun->vtable.init;
    program->instance_free = aggr_fun->vtable.free;
    program->instance_merge = aggr_fun->vtable.merge;
    program->instance_savestate = aggr_fun->vtable.savestate;
    program->instance_loadstate = aggr_fun->vtable.loadstate;
    program->instance_storage_size = aggr_fun->instance_size;

    for (auto e : aggr_expr->arguments()) {
      auto rc = compileExpression(e.get(), program.get(), symbol_table);
      if (!rc.isSuccess()) {
        return rc;
      }
    }

    program->instructions.emplace_back(
        vm::X_CALL_INSTANCE,
        intptr_t(aggr_fun->vtable.accumulate));

    program->instructions.emplace_back(vm::X_RETURN, intptr_t(0));
  }

  *output = std::move(program);
  return ReturnCode::success();
}

ReturnCode Compiler::compileExpression(
    const ValueExpressionNode* node,
    vm::Program* program,
    SymbolTable* symbol_table) {
  if (dynamic_cast<const ColumnReferenceNode*>(node)) {
    return compileColumnReference(
        dynamic_cast<const ColumnReferenceNode*>(node),
        program,
        symbol_table);
  }

  if (dynamic_cast<const LiteralExpressionNode*>(node)) {
    return compileLiteral(
        dynamic_cast<const LiteralExpressionNode*>(node),
        program,
        symbol_table);
  }

  if (dynamic_cast<const IfExpressionNode*>(node)) {
    return compileIfExpression(
        dynamic_cast<const IfExpressionNode*>(node),
        program,
        symbol_table);
  }

  if (dynamic_cast<const CallExpressionNode*>(node)) {
    return compileMethodCall(
        dynamic_cast<const CallExpressionNode*>(node),
        program,
        symbol_table);
  }

  return ReturnCode::error(
      "ERUNTIME",
      "internal error: can't compile expression");
}

ReturnCode Compiler::compileColumnReference(
    const ColumnReferenceNode* node,
    vm::Program* program,
    SymbolTable* symbol_table) {
  auto col_idx = node->columnIndex();
  assert(col_idx != size_t(-1));

  program->instructions.emplace_back(
      vm::X_INPUT,
      intptr_t(col_idx),
      node->getReturnType());

  return ReturnCode::success();
}

ReturnCode Compiler::compileLiteral(
    const LiteralExpressionNode* node,
    vm::Program* program,
    SymbolTable* symbol_table) {
  const auto& val = node->value();
  auto static_alloc = program->static_storage.alloc(val.getSize());
  memcpy(static_alloc, val.getData(), val.getSize());

  program->instructions.emplace_back(
      vm::X_LITERAL,
      intptr_t(static_alloc),
      node->getReturnType());

  return ReturnCode::success();
}

ReturnCode Compiler::compileIfExpression(
    const IfExpressionNode* node,
    vm::Program* program,
    SymbolTable* symbol_table) {
  {
    auto rc = compileExpression(node->conditional().get(), program, symbol_table);
    if (!rc.isSuccess()) {
      return rc;
    }
  }

  auto jump_idx = program->instructions.size();
  program->instructions.emplace_back(vm::X_CJUMP, intptr_t(0));

  {
    auto rc = compileExpression(node->falseBranch().get(), program, symbol_table);
    if (!rc.isSuccess()) {
      return rc;
    }
  }

  program->instructions[jump_idx].arg0 = program->instructions.size() + 1;
  program->instructions.emplace_back(vm::X_JUMP, intptr_t(0));
  jump_idx = program->instructions.size() - 1;

  {
    auto rc = compileExpression(node->trueBranch().get(), program, symbol_table);
    if (!rc.isSuccess()) {
      return rc;
    }
  }

  program->instructions[jump_idx].arg0 = program->instructions.size();

  return ReturnCode::success();
}

ReturnCode Compiler::compileMethodCall(
    const CallExpressionNode* node,
    vm::Program* program,
    SymbolTable* symbol_table) {
  auto entry = symbol_table->lookup(node->getSymbol());
  if (!entry) {
    return ReturnCode::errorf(
        "ERUNTIME",
        "symbol not found: $0",
        node->getSymbol());
  }

  auto fun = entry->getFunction();
  switch (fun->type) {

    case FN_PURE:
      for (auto e : node->arguments()) {
        auto rc = compileExpression(e.get(), program, symbol_table);
        if (!rc.isSuccess()) {
          return rc;
        }
      }

      program->instructions.emplace_back(
          vm::X_CALL_PURE,
          intptr_t(fun->vtable.call));
      break;

    case FN_AGGREGATE:
      program->instructions.emplace_back(
          vm::X_CALL_INSTANCE,
          intptr_t(fun->vtable.get));
      break;

  }

  return ReturnCode::success();
}

} // namespace csql

