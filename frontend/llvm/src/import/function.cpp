/*******************************************************************************
 *
 * \file
 * \brief Translate LLVM functions into AR functions
 *
 * Author: Maxime Arthaud
 *         Nija Shi
 *         Arnaud Venet
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2011-2018 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <deque>

#include <boost/container/flat_map.hpp>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Transforms/Utils/Local.h>

#include <ikos/frontend/llvm/import/exception.hpp>

#include "bundle.hpp"
#include "constant.hpp"
#include "function.hpp"
#include "type.hpp"

namespace ikos {
namespace frontend {
namespace import {

ar::Code* FunctionImporter::translate_body() {
  /// Set _llvm_return_bb, _llvm_unreachable_bb and _llvm_ehresume_bb
  this->mark_special_blocks();

  // Translate parameters
  this->translate_parameters();

  // Translate control flow graph
  this->translate_control_flow_graph();

  return this->_body;
}

void FunctionImporter::mark_special_blocks() {
  llvm::SmallVector< llvm::BasicBlock*, 2 > llvm_return_blocks;
  llvm::SmallVector< llvm::BasicBlock*, 2 > llvm_unreachable_blocks;
  llvm::SmallVector< llvm::BasicBlock*, 2 > llvm_ehresume_blocks;

  for (llvm::BasicBlock& bb : *this->_llvm_fun) {
    if (llvm::isa< llvm::ReturnInst >(bb.getTerminator())) {
      llvm_return_blocks.push_back(&bb);
    } else if (llvm::isa< llvm::UnreachableInst >(bb.getTerminator())) {
      llvm_unreachable_blocks.push_back(&bb);
    } else if (llvm::isa< llvm::ResumeInst >(bb.getTerminator())) {
      llvm_ehresume_blocks.push_back(&bb);
    }
  }

  this->_llvm_entry_bb = &this->_llvm_fun->getEntryBlock();

  if (llvm_return_blocks.size() == 1) {
    this->_llvm_return_bb = llvm_return_blocks[0];
  } else if (llvm_return_blocks.empty()) {
    this->_llvm_return_bb = nullptr;
  } else {
    std::ostringstream buf;
    buf << "function @" << this->_ar_fun->name()
        << " has more than one exit block (use the -mergereturn pass?)";
    throw ImportError(buf.str());
  }

  if (llvm_unreachable_blocks.size() == 1) {
    this->_llvm_unreachable_bb = llvm_unreachable_blocks[0];
  } else if (llvm_unreachable_blocks.empty()) {
    this->_llvm_unreachable_bb = nullptr;
  } else {
    std::ostringstream buf;
    buf << "function @" << this->_ar_fun->name()
        << " has more than one unreachable block (use the -mergereturn pass?)";
    throw ImportError(buf.str());
  }

  if (llvm_ehresume_blocks.size() == 1) {
    this->_llvm_ehresume_bb = llvm_ehresume_blocks[0];
  } else if (llvm_ehresume_blocks.empty()) {
    this->_llvm_ehresume_bb = nullptr;
  } else {
    std::ostringstream buf;
    buf << "function @" << this->_ar_fun->name()
        << " has more than one ehresume block (use the -mergereturn pass?)";
    throw ImportError(buf.str());
  }
}

void FunctionImporter::mark_variable_mapping(llvm::Value* llvm_val,
                                             ar::Variable* ar_var) {
  // Set name
  if (llvm_val->hasName()) {
    ar_var->set_name(llvm_val->getName().str());
  }

  // Add pointer to frontend object
  ar_var->set_frontend(llvm_val);

  // Add in the mapping
  this->_variables.try_emplace(llvm_val, ar_var);
}

void FunctionImporter::translate_parameters() {
  // Internal variables for parameters are automatically created by
  // ar::Function::create(). Here, we just need to store the mapping with
  // mark_variable_mapping.
  auto param_it = this->_llvm_fun->arg_begin();
  auto param_et = this->_llvm_fun->arg_end();
  auto ar_param_it = this->_ar_fun->param_begin();
  for (; param_it != param_et; ++param_it, ++ar_param_it) {
    llvm::Argument* llvm_param = &*param_it;
    ar::InternalVariable* ar_param = *ar_param_it;
    this->mark_variable_mapping(llvm_param, ar_param);
  }
}

void FunctionImporter::translate_control_flow_graph() {
  // Translate all basic blocks
  translate_basic_blocks();

  // Handle phi nodes
  // Add assignments in input blocks of BasicBlockTranslations
  translate_phi_nodes();

  // Set the predecessors/successors
  link_basic_blocks();
}

void FunctionImporter::translate_basic_blocks() {
  std::deque< llvm::BasicBlock* > worklist;

  // Start at the entry block
  worklist.push_back(this->_llvm_entry_bb);

  while (!worklist.empty()) {
    // Pop the front element
    llvm::BasicBlock* bb = worklist.front();
    worklist.pop_front();

    // If already translated
    if (this->_blocks.find(bb) != this->_blocks.end()) {
      continue;
    }

    // Translate the basic block
    this->translate_basic_block(bb);

    // Add successors in the worklist
    for (auto it = succ_begin(bb), et = succ_end(bb); it != et; ++it) {
      worklist.push_back(*it);
    }
  }
}

void FunctionImporter::translate_basic_block(llvm::BasicBlock* llvm_bb) {
  ikos_assert(this->_blocks.find(llvm_bb) == this->_blocks.end());

  // Create the main ar::BasicBlock
  ar::BasicBlock* ar_main_bb = ar::BasicBlock::create(this->_body);

  // Set name
  if (llvm_bb->hasName()) {
    ar_main_bb->set_name(llvm_bb->getName().str());
  }

  // Add pointer to frontend object
  ar_main_bb->set_frontend(llvm_bb);

  // Initialize a BasicBlockTranslation
  auto bb_translation =
      std::make_unique< BasicBlockTranslation >(llvm_bb, ar_main_bb);

  // Set the entry block
  if (llvm_bb == this->_llvm_entry_bb) {
    bb_translation->mark_entry_block();
  }

  // Translate instructions
  for (llvm::Instruction& inst : *llvm_bb) {
    this->translate_instruction(bb_translation.get(), &inst);
  }

  // Set exit/unreachable/ehresume blocks
  if (llvm_bb == this->_llvm_return_bb) {
    bb_translation->mark_exit_block();
  }
  if (llvm_bb == this->_llvm_unreachable_bb) {
    bb_translation->mark_unreachable_block();
  }
  if (llvm_bb == this->_llvm_ehresume_bb) {
    bb_translation->mark_ehresume_block();
  }

  // Add it in the map
  this->_blocks.try_emplace(llvm_bb, std::move(bb_translation));
}

void FunctionImporter::translate_phi_nodes() {
  // Iterate over llvm basic blocks instead of this->_blocks,
  // because we want a deterministic output (for testing purposes).
  for (llvm::BasicBlock& bb : *this->_llvm_fun) {
    auto it = this->_blocks.find(&bb);
    if (it == this->_blocks.end()) {
      continue;
    }
    BasicBlockTranslation* bb_translation = it->second.get();
    this->translate_phi_nodes(bb_translation, &bb);
  }
}

void FunctionImporter::translate_phi_nodes(
    BasicBlockTranslation* bb_translation, llvm::BasicBlock* bb) {
  for (llvm::Instruction& inst : *bb) {
    if (auto phi = llvm::dyn_cast< llvm::PHINode >(&inst)) {
      this->translate_phi_late(bb_translation, phi);
    }
  }
}

void FunctionImporter::link_basic_blocks() {
  // Iterate over llvm basic blocks instead of this->_blocks,
  // because we want a deterministic output (for testing purposes).
  for (llvm::BasicBlock& bb : *this->_llvm_fun) {
    auto it = this->_blocks.find(&bb);
    if (it == this->_blocks.end()) {
      continue;
    }
    BasicBlockTranslation* bb_translation = it->second.get();
    this->link_basic_block(bb_translation);
  }
}

void FunctionImporter::link_basic_block(BasicBlockTranslation* bb_translation) {
  llvm::BasicBlock* llvm_block = bb_translation->source;

  for (const BasicBlockOutput& output : bb_translation->outputs) {
    // Connect this output to the right basic block
    ar::BasicBlock* ar_block = output.block;
    llvm::BasicBlock* llvm_succ = output.succ;

    if (llvm_succ == nullptr) {
      // No successor (ret, resume, unreachable, etc.)
      continue;
    }

    // Destination basic block translation
    BasicBlockTranslation* succ_translation = this->_blocks[llvm_succ].get();

    if (succ_translation->inputs.empty()) {
      // No input blocks (probably because there is no phi instruction),
      // Connect it to the main basic block
      ar_block->add_successor(succ_translation->main);
    } else {
      ikos_assert(succ_translation->inputs.find(llvm_block) !=
                  succ_translation->inputs.end());
      ar::BasicBlock* ar_succ = succ_translation->inputs[llvm_block];
      ar_block->add_successor(ar_succ);
    }
  }
}

void FunctionImporter::translate_instruction(
    BasicBlockTranslation* bb_translation, llvm::Instruction* inst) {
  // If we have more than one output block, merge them.
  //
  // A few exceptions are CmpInst, BinaryOperator and BranchInst.
  // We want to avoid a diamond shape in the graph:
  //
  //    A
  //  /   \ 
  // B     C
  //  \   /
  //    D
  //
  // With this shape, we would lose precision in the analysis,
  // because of abstract join operations.
  if (bb_translation->outputs.size() > 1 && !llvm::isa< llvm::CmpInst >(inst) &&
      !llvm::isa< llvm::BinaryOperator >(inst) &&
      !llvm::isa< llvm::BranchInst >(inst)) {
    bb_translation->merge_outputs();
  }

  if (auto alloca = llvm::dyn_cast< llvm::AllocaInst >(inst)) {
    this->translate_alloca(bb_translation, alloca);
  } else if (auto store = llvm::dyn_cast< llvm::StoreInst >(inst)) {
    this->translate_store(bb_translation, store);
  } else if (auto load = llvm::dyn_cast< llvm::LoadInst >(inst)) {
    this->translate_load(bb_translation, load);
  } else if (auto call = llvm::dyn_cast< llvm::CallInst >(inst)) {
    this->translate_call(bb_translation, call);
  } else if (auto invoke = llvm::dyn_cast< llvm::InvokeInst >(inst)) {
    this->translate_invoke(bb_translation, invoke);
  } else if (auto bitcast = llvm::dyn_cast< llvm::BitCastInst >(inst)) {
    this->translate_bitcast(bb_translation, bitcast);
  } else if (auto cast = llvm::dyn_cast< llvm::CastInst >(inst)) {
    this->translate_cast(bb_translation, cast);
  } else if (auto gep = llvm::dyn_cast< llvm::GetElementPtrInst >(inst)) {
    this->translate_getelementptr(bb_translation, gep);
  } else if (auto binary_op = llvm::dyn_cast< llvm::BinaryOperator >(inst)) {
    this->translate_binary_operator(bb_translation, binary_op);
  } else if (auto cmp = llvm::dyn_cast< llvm::CmpInst >(inst)) {
    this->translate_cmp(bb_translation, cmp);
  } else if (auto br = llvm::dyn_cast< llvm::BranchInst >(inst)) {
    this->translate_branch(bb_translation, br);
  } else if (auto ret = llvm::dyn_cast< llvm::ReturnInst >(inst)) {
    this->translate_return(bb_translation, ret);
  } else if (auto phi = llvm::dyn_cast< llvm::PHINode >(inst)) {
    this->translate_phi(bb_translation, phi);
  } else if (auto extractvalue =
                 llvm::dyn_cast< llvm::ExtractValueInst >(inst)) {
    this->translate_extractvalue(bb_translation, extractvalue);
  } else if (auto insertvalue = llvm::dyn_cast< llvm::InsertValueInst >(inst)) {
    this->translate_insertvalue(bb_translation, insertvalue);
  } else if (auto unreachable = llvm::dyn_cast< llvm::UnreachableInst >(inst)) {
    this->translate_unreachable(bb_translation, unreachable);
  } else if (auto landingpad = llvm::dyn_cast< llvm::LandingPadInst >(inst)) {
    this->translate_landingpad(bb_translation, landingpad);
  } else if (auto resume = llvm::dyn_cast< llvm::ResumeInst >(inst)) {
    this->translate_resume(bb_translation, resume);
  } else if (llvm::isa< llvm::SelectInst >(inst)) {
    throw ImportError(
        "select instruction not supported (use the -lower-select pass?)");
  } else if (llvm::isa< llvm::SwitchInst >(inst)) {
    throw ImportError(
        "switch instruction not supported (use the -lowerswitch pass?)");
  } else {
    std::ostringstream buf;
    buf << "unsupported llvm::Instruction (opcode: " << inst->getOpcodeName()
        << ")";
    throw ImportError(buf.str());
  }
}

void FunctionImporter::translate_alloca(BasicBlockTranslation* bb_translation,
                                        llvm::AllocaInst* alloca) {
  // Translate types
  check_import(alloca->getType()->getElementType() ==
                   alloca->getAllocatedType(),
               "unexpected allocated type for llvm::AllocaInst");
  auto var_type = ar::cast< ar::PointerType >(this->infer_type(alloca));
  ar::Type* allocated_type = var_type->pointee();

  // Translate local variable
  ar::LocalVariable* var = ar::LocalVariable::create(this->_ar_fun,
                                                     var_type,
                                                     alloca->getAlignment());
  this->mark_variable_mapping(alloca, var);

  // Translate array size
  auto array_size_type = ar::IntegerType::size_type(this->_bundle);
  auto array_size = this->translate_cast_integer_value(bb_translation,
                                                       alloca->getArraySize(),
                                                       array_size_type);

  auto stmt = ar::Allocate::create(var, allocated_type, array_size);
  stmt->set_frontend< llvm::Value >(alloca);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_store(BasicBlockTranslation* bb_translation,
                                       llvm::StoreInst* store) {
  // Translate pointer
  ar::Value* pointer = this->translate_value(bb_translation,
                                             store->getPointerOperand(),
                                             nullptr);
  auto ptr_type = ar::cast< ar::PointerType >(pointer->type());

  // Translate stored value
  ar::Value* value = this->translate_value(bb_translation,
                                           store->getValueOperand(),
                                           ptr_type->pointee());

  auto stmt = ar::Store::create(pointer,
                                value,
                                store->getAlignment(),
                                store->isVolatile());
  stmt->set_frontend< llvm::Value >(store);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_load(BasicBlockTranslation* bb_translation,
                                      llvm::LoadInst* load) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(load));
  this->mark_variable_mapping(load, var);

  // Translate pointer
  ar::PointerType* ptr_type = ar::PointerType::get(this->_context, var->type());
  ar::Value* pointer = this->translate_value(bb_translation,
                                             load->getPointerOperand(),
                                             ptr_type);

  auto stmt =
      ar::Load::create(var, pointer, load->getAlignment(), load->isVolatile());
  stmt->set_frontend< llvm::Value >(load);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_call(BasicBlockTranslation* bb_translation,
                                      llvm::CallInst* call) {
  if (auto intrinsic = llvm::dyn_cast< llvm::IntrinsicInst >(call)) {
    this->translate_intrinsic_call(bb_translation, intrinsic);
    return;
  }

  // Add a explicit cast for the return value, if needed
  const bool force_return_cast = true;

  // If this is a direct call, force exact types of arguments
  // Otherwise, it's a call on a function pointer, we allow implicit casts
  // (signed/unsigned and between pointer types)
  const bool force_args_cast =
      llvm::isa< llvm::Function >(call->getCalledValue());

  this->translate_call_helper(bb_translation,
                              call,
                              force_return_cast,
                              force_args_cast,
                              [](ar::InternalVariable* result,
                                 ar::Value* called,
                                 const std::vector< ar::Value* >& arguments) {
                                return ar::Call::create(result,
                                                        called,
                                                        arguments);
                              });
}

void FunctionImporter::translate_intrinsic_call(
    BasicBlockTranslation* bb_translation, llvm::IntrinsicInst* call) {
  ar::IntegerType* si8_ty = ar::IntegerType::si8(this->_context);
  ar::PointerType* void_ptr_ty = ar::PointerType::get(this->_context, si8_ty);
  ar::IntegerType* size_ty = ar::IntegerType::size_type(this->_bundle);

  if (_ctx.bundle_imp->ignore_intrinsic(call->getIntrinsicID())) {
    return; // ignored intrinsic (llvm.dbg.value, etc.)
  } else if (auto memcpy = llvm::dyn_cast< llvm::MemCpyInst >(call)) {
    ar::Value* dest = this->translate_value(bb_translation,
                                            memcpy->getRawDest(),
                                            void_ptr_ty);

    ar::Value* src = this->translate_value(bb_translation,
                                           memcpy->getRawSource(),
                                           void_ptr_ty);

    ar::Value* length =
        this->translate_value(bb_translation, memcpy->getLength(), size_ty);

    auto stmt = ar::MemoryCopy::create(this->_bundle,
                                       dest,
                                       src,
                                       length,
                                       memcpy->getParamAlignment(0),
                                       memcpy->getParamAlignment(1),
                                       memcpy->isVolatile());
    stmt->set_frontend< llvm::Value >(memcpy);
    bb_translation->add_statement(std::move(stmt));
  } else if (auto memmove = llvm::dyn_cast< llvm::MemMoveInst >(call)) {
    ar::Value* dest = this->translate_value(bb_translation,
                                            memmove->getRawDest(),
                                            void_ptr_ty);

    ar::Value* src = this->translate_value(bb_translation,
                                           memmove->getRawSource(),
                                           void_ptr_ty);

    ar::Value* length =
        this->translate_value(bb_translation, memmove->getLength(), size_ty);

    auto stmt = ar::MemoryMove::create(this->_bundle,
                                       dest,
                                       src,
                                       length,
                                       memmove->getParamAlignment(0),
                                       memmove->getParamAlignment(1),
                                       memmove->isVolatile());
    stmt->set_frontend< llvm::Value >(memmove);
    bb_translation->add_statement(std::move(stmt));
  } else if (auto memset = llvm::dyn_cast< llvm::MemSetInst >(call)) {
    ar::Value* dest = this->translate_value(bb_translation,
                                            memset->getRawDest(),
                                            void_ptr_ty);

    ar::Value* value =
        this->translate_value(bb_translation, memset->getValue(), si8_ty);

    ar::Value* length =
        this->translate_value(bb_translation, memset->getLength(), size_ty);

    auto stmt = ar::MemorySet::create(this->_bundle,
                                      dest,
                                      value,
                                      length,
                                      memset->getDestAlignment(),
                                      memset->isVolatile());
    stmt->set_frontend< llvm::Value >(memset);
    bb_translation->add_statement(std::move(stmt));
  } else if (call->getIntrinsicID() == llvm::Intrinsic::vastart) {
    ar::Value* operand = this->translate_value(bb_translation,
                                               call->getArgOperand(0),
                                               void_ptr_ty);

    auto stmt = ar::VarArgStart::create(this->_bundle, operand);
    stmt->set_frontend< llvm::Value >(call);
    bb_translation->add_statement(std::move(stmt));

    // Note that there is no intrinsic for VarArgGet.
    // There is a special instruction va_arg,
    // but it is never generated by clang.
    // Instead, clang generates load instructions that are ABI-specific.
  } else if (call->getIntrinsicID() == llvm::Intrinsic::vaend) {
    ar::Value* operand = this->translate_value(bb_translation,
                                               call->getArgOperand(0),
                                               void_ptr_ty);

    auto stmt = ar::VarArgEnd::create(this->_bundle, operand);
    stmt->set_frontend< llvm::Value >(call);
    bb_translation->add_statement(std::move(stmt));
  } else if (call->getIntrinsicID() == llvm::Intrinsic::vacopy) {
    ar::Value* dest = this->translate_value(bb_translation,
                                            call->getArgOperand(0),
                                            void_ptr_ty);
    ar::Value* src = this->translate_value(bb_translation,
                                           call->getArgOperand(1),
                                           void_ptr_ty);

    auto stmt = ar::VarArgCopy::create(this->_bundle, dest, src);
    stmt->set_frontend< llvm::Value >(call);
    bb_translation->add_statement(std::move(stmt));
  } else {
    this->translate_call_helper(bb_translation,
                                call,
                                /*force_return_cast = */ true,
                                /*force_args_cast = */ true,
                                [](ar::InternalVariable* result,
                                   ar::Value* called,
                                   const std::vector< ar::Value* >& arguments) {
                                  return ar::Call::create(result,
                                                          called,
                                                          arguments);
                                });
  }
}

void FunctionImporter::translate_invoke(BasicBlockTranslation* bb_translation,
                                        llvm::InvokeInst* invoke) {
  // Do not add an explicit cast, we want invoke to be the last statement
  const bool force_return_cast = false;

  // If this is a direct call, force exact types of arguments
  // Otherwise, it's a call on a function pointer, we allow implicit casts
  // (signed/unsigned and between pointer types)
  const bool force_args_cast =
      llvm::isa< llvm::Function >(invoke->getCalledValue());

  // Translate the invoke
  //
  // Use bb_translation->main as the normal and exception dest for now,
  // it will be updated later in BasicBlockTranslation::add_invoke_branching()
  this->translate_call_helper(bb_translation,
                              invoke,
                              force_return_cast,
                              force_args_cast,
                              [=](ar::InternalVariable* result,
                                  ar::Value* called,
                                  const std::vector< ar::Value* >& arguments) {
                                return ar::Invoke::create(result,
                                                          called,
                                                          arguments,
                                                          bb_translation->main,
                                                          bb_translation->main);
                              });

  // Add output basic blocks
  bb_translation->add_invoke_branching(invoke->getNormalDest(),
                                       invoke->getUnwindDest());
}

template < typename CallInstType, typename CreateStmtFun >
void FunctionImporter::translate_call_helper(
    BasicBlockTranslation* bb_translation,
    CallInstType* call,
    bool force_return_cast,
    bool force_args_cast,
    CreateStmtFun create_stmt) {
  // Translate called value
  ar::Value* called =
      this->translate_value(bb_translation, call->getCalledValue(), nullptr);
  auto called_type = ar::cast< ar::PointerType >(called->type());
  auto fun_type = ar::cast< ar::FunctionType >(called_type->pointee());

  const bool has_return_value = !call->getType()->isVoidTy();

  // Sanity check
  ikos_assert(call->getType()->isVoidTy() ==
              fun_type->return_type()->is_void());

  // Translate result variable
  ar::InternalVariable* var = nullptr;
  if (has_return_value) {
    var = ar::InternalVariable::create(this->_body,
                                       force_return_cast
                                           ? this->infer_type(call)
                                           : fun_type->return_type());
    this->mark_variable_mapping(call, var);
  }

  // Result of the ar::Call
  // If we need a cast, this is a temporary variable
  ar::InternalVariable* result = var;
  if (has_return_value && force_return_cast &&
      fun_type->return_type() != var->type()) {
    result = ar::InternalVariable::create(this->_body, fun_type->return_type());
    result->set_frontend< llvm::Value >(call);
  }

  // Translate parameters
  std::vector< ar::Value* > arguments;
  arguments.reserve(call->getNumArgOperands());

  for (unsigned i = 0; i < call->getNumArgOperands(); i++) {
    llvm::Value* arg = call->getArgOperand(i);

    if (i < fun_type->num_parameters() &&
        (force_args_cast || (llvm::isa< llvm::Constant >(arg) &&
                             !llvm::isa< llvm::GlobalValue >(arg)))) {
      ar::Type* arg_type = fun_type->param_type(i);
      arguments.push_back(this->translate_value(bb_translation, arg, arg_type));
    } else {
      arguments.push_back(this->translate_value(bb_translation, arg, nullptr));
    }
  }

  auto stmt = create_stmt(result, called, arguments);
  stmt->template set_frontend< llvm::Value >(call);
  bb_translation->add_statement(std::move(stmt));

  // Add a cast from result to var, if required
  if (has_return_value && force_return_cast &&
      fun_type->return_type() != var->type()) {
    this->add_bitcast(bb_translation, var, result);
  }
}

void FunctionImporter::translate_bitcast(BasicBlockTranslation* bb_translation,
                                         llvm::BitCastInst* bitcast) {
  if ((bitcast->getSrcTy()->isPointerTy() &&
       bitcast->getDestTy()->isPointerTy()) ||
      (bitcast->getSrcTy()->isFloatingPointTy() &&
       bitcast->getDestTy()->isIntegerTy()) ||
      (bitcast->getSrcTy()->isIntegerTy() &&
       bitcast->getDestTy()->isFloatingPointTy())) {
    // Translate result variable
    ar::InternalVariable* var =
        ar::InternalVariable::create(this->_body, this->infer_type(bitcast));
    this->mark_variable_mapping(bitcast, var);

    // Translate operand
    ar::Value* operand =
        this->translate_value(bb_translation, bitcast->getOperand(0), nullptr);

    // Create statement
    auto stmt =
        ar::UnaryOperation::create(ar::UnaryOperation::Bitcast, var, operand);
    stmt->set_frontend< llvm::Value >(bitcast);
    bb_translation->add_statement(std::move(stmt));
  } else {
    throw ImportError("unexpected llvm::BitCastInst");
  }
}

static ar::UnaryOperation::Operator convert_unary_op(
    llvm::Instruction::CastOps op, ar::Signedness sign) {
  switch (op) {
    case llvm::Instruction::Trunc:
      if (sign == ar::Unsigned) {
        return ar::UnaryOperation::UTrunc;
      } else {
        return ar::UnaryOperation::STrunc;
      }
    case llvm::Instruction::ZExt:
      return ar::UnaryOperation::ZExt;
    case llvm::Instruction::SExt:
      return ar::UnaryOperation::SExt;
    case llvm::Instruction::FPToUI:
      return ar::UnaryOperation::FPToUI;
    case llvm::Instruction::FPToSI:
      return ar::UnaryOperation::FPToSI;
    case llvm::Instruction::UIToFP:
      return ar::UnaryOperation::UIToFP;
    case llvm::Instruction::SIToFP:
      return ar::UnaryOperation::SIToFP;
    case llvm::Instruction::FPTrunc:
      return ar::UnaryOperation::FPTrunc;
    case llvm::Instruction::FPExt:
      return ar::UnaryOperation::FPExt;
    case llvm::Instruction::PtrToInt:
      if (sign == ar::Unsigned) {
        return ar::UnaryOperation::PtrToUI;
      } else {
        return ar::UnaryOperation::PtrToSI;
      }
    case llvm::Instruction::IntToPtr:
      if (sign == ar::Unsigned) {
        return ar::UnaryOperation::UIToPtr;
      } else {
        return ar::UnaryOperation::SIToPtr;
      }
    case llvm::Instruction::BitCast:
      return ar::UnaryOperation::Bitcast;
    case llvm::Instruction::AddrSpaceCast:
      throw ImportError("unsupported cast llvm::Instruction::AddrSpaceCast");
    default:
      ikos_unreachable("unreachable");
  }
}

void FunctionImporter::translate_cast(BasicBlockTranslation* bb_translation,
                                      llvm::CastInst* cast) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(cast));
  this->mark_variable_mapping(cast, var);

  ar::Signedness sign = ar::Signed;
  ar::Type* src_type = nullptr;  // required type for the operand (or null)
  ar::Type* dest_type = nullptr; // type of the statement result (or null)
  ar::Value* operand = nullptr;  // operand (null if not yet translated)

  // Note that dest_type might be different from var->type(),
  // in this case we need to add a cast.

  switch (cast->getOpcode()) {
    case llvm::Instruction::Trunc: {
      // No sign requirements, use inferred signedness to avoid casts
      sign = ar::cast< ar::IntegerType >(var->type())->sign();
      src_type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
      dest_type = var->type();
    } break;
    case llvm::Instruction::ZExt: {
      sign = ar::Unsigned;
      src_type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
      dest_type = _ctx.type_imp->translate_type(cast->getDestTy(), sign);
    } break;
    case llvm::Instruction::SExt: {
      sign = ar::Signed;
      src_type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
      dest_type = _ctx.type_imp->translate_type(cast->getDestTy(), sign);
    } break;
    case llvm::Instruction::FPToUI: {
      sign = ar::Unsigned;
      src_type = nullptr;
      dest_type = _ctx.type_imp->translate_type(cast->getDestTy(), sign);
    } break;
    case llvm::Instruction::FPToSI: {
      sign = ar::Signed;
      src_type = nullptr;
      dest_type = _ctx.type_imp->translate_type(cast->getDestTy(), sign);
    } break;
    case llvm::Instruction::UIToFP: {
      sign = ar::Unsigned;
      src_type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
      dest_type = nullptr;
    } break;
    case llvm::Instruction::SIToFP: {
      sign = ar::Signed;
      src_type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
      dest_type = nullptr;
    } break;
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt: {
      src_type = dest_type = nullptr;
    } break;
    case llvm::Instruction::PtrToInt: {
      // No sign requirements, use inferred signedness to avoid casts
      sign = ar::cast< ar::IntegerType >(var->type())->sign();
      src_type = nullptr; // No sign requirement on source type
      dest_type = var->type();
    } break;
    case llvm::Instruction::IntToPtr: {
      // No sign requirements, use inferred signedness of the operand
      operand =
          this->translate_value(bb_translation, cast->getOperand(0), nullptr);
      sign = ar::cast< ar::IntegerType >(operand->type())->sign();
      src_type = operand->type();
      dest_type = nullptr;
    } break;
    default: {
      std::ostringstream buf;
      buf << "unexpected llvm::CastInst (opcode: " << cast->getOpcodeName()
          << ")";
      throw ImportError(buf.str());
    }
  }

  // Translate operand
  if (operand == nullptr) {
    operand =
        this->translate_value(bb_translation, cast->getOperand(0), src_type);
  }

  // Result of the ar::UnaryOperation
  // If we need a cast, this is a temporary variable
  ar::InternalVariable* result = var;
  if (dest_type != nullptr && dest_type != var->type()) {
    result = ar::InternalVariable::create(this->_body, dest_type);
    result->set_frontend< llvm::Value >(cast);
  }

  // Create statement
  auto stmt =
      ar::UnaryOperation::create(convert_unary_op(cast->getOpcode(), sign),
                                 result,
                                 operand);
  stmt->set_frontend< llvm::Value >(cast);
  bb_translation->add_statement(std::move(stmt));

  // Add a cast from result to var, if required
  if (dest_type != nullptr && dest_type != var->type()) {
    this->add_bitcast(bb_translation, var, result);
  }
}

void FunctionImporter::translate_getelementptr(
    BasicBlockTranslation* bb_translation, llvm::GetElementPtrInst* gep) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(gep));
  this->mark_variable_mapping(gep, var);

  // Translate base
  ar::Value* pointer =
      this->translate_value(bb_translation, gep->getPointerOperand(), nullptr);

  // Translate operands
  std::vector< ar::PointerShift::Term > terms;
  terms.reserve(gep->getNumOperands() - 1);

  // Preferred type for operands
  auto size_type = ar::IntegerType::size_type(this->_bundle);

  for (auto it = llvm::gep_type_begin(gep), et = llvm::gep_type_end(gep);
       it != et;
       ++it) {
    llvm::Value* op = it.getOperand();

    if (llvm::StructType* struct_type = it.getStructTypeOrNull()) {
      // Shift to get a struct field
      llvm::APInt value = llvm::cast< llvm::ConstantInt >(op)->getValue();
      ikos_assert(value.getBitWidth() <= 64 &&
                  value.getZExtValue() <=
                      std::numeric_limits< unsigned >::max());
      auto uint_value = static_cast< unsigned >(value.getZExtValue());
      uint64_t offset = this->_llvm_data_layout.getStructLayout(struct_type)
                            ->getElementOffset(uint_value);

      ar::IntegerConstant* ar_op =
          ar::IntegerConstant::get(this->_context,
                                   size_type,
                                   ar::MachineInt(offset,
                                                  size_type->bit_width(),
                                                  size_type->sign()));
      terms.emplace_back(ar::MachineInt(1,
                                        size_type->bit_width(),
                                        size_type->sign()),
                         ar_op);
    } else {
      // Shift in a sequential type
      uint64_t size =
          this->_llvm_data_layout.getTypeAllocSize(it.getIndexedType());
      ar::Type* preferred_type =
          llvm::isa< llvm::Constant >(op)
              ? _ctx.type_imp->translate_type(op->getType(), ar::Unsigned)
              : nullptr;
      ar::Value* ar_op =
          this->translate_value(bb_translation, op, preferred_type);
      terms.emplace_back(ar::MachineInt(size,
                                        size_type->bit_width(),
                                        size_type->sign()),
                         ar_op);
    }
  }

  // Create statement
  auto stmt = ar::PointerShift::create(var, pointer, terms);
  stmt->set_frontend< llvm::Value >(gep);
  bb_translation->add_statement(std::move(stmt));
}

/// \brief Return the signedness of an instruction, based on nsw and nuw flags
static ar::Signedness sign_from_wraps(llvm::Instruction* inst) {
  if (inst->hasNoUnsignedWrap() && !inst->hasNoSignedWrap()) {
    return ar::Unsigned;
  } else if (inst->hasNoSignedWrap() && !inst->hasNoUnsignedWrap()) {
    return ar::Signed;
  } else if (inst->hasNoSignedWrap() && inst->hasNoUnsignedWrap()) {
    // This is only introduced by aggressive LLVM optimization passes.
    // There is no way to get the original attribute, so "signed" is just a
    // random guess.
    return ar::Signed;
  } else {
    // In C, overflow on signed operations (add,sub) are undefined behaviors,
    // and overflow on unsigned operations are implemetation defined.
    // That means operations without nuw or nsw flags are necessary
    // unsigned operations.
    return ar::Unsigned;
  }
}

static ar::BinaryOperation::Operator convert_int_bin_op(
    llvm::BinaryOperator::BinaryOps op, ar::Signedness sign) {
  if (sign == ar::Unsigned) {
    switch (op) {
      case llvm::Instruction::Add:
        return ar::BinaryOperation::UAdd;
      case llvm::Instruction::Sub:
        return ar::BinaryOperation::USub;
      case llvm::Instruction::Mul:
        return ar::BinaryOperation::UMul;
      case llvm::Instruction::UDiv:
        return ar::BinaryOperation::UDiv;
      case llvm::Instruction::URem:
        return ar::BinaryOperation::URem;
      case llvm::Instruction::Shl:
        return ar::BinaryOperation::UShl;
      case llvm::Instruction::LShr:
        return ar::BinaryOperation::ULShr;
      case llvm::Instruction::AShr:
        return ar::BinaryOperation::UAShr;
      case llvm::Instruction::And:
        return ar::BinaryOperation::UAnd;
      case llvm::Instruction::Or:
        return ar::BinaryOperation::UOr;
      case llvm::Instruction::Xor:
        return ar::BinaryOperation::UXor;
      default:
        ikos_unreachable("unreachable");
    }
  } else {
    switch (op) {
      case llvm::Instruction::Add:
        return ar::BinaryOperation::SAdd;
      case llvm::Instruction::Sub:
        return ar::BinaryOperation::SSub;
      case llvm::Instruction::Mul:
        return ar::BinaryOperation::SMul;
      case llvm::Instruction::SDiv:
        return ar::BinaryOperation::SDiv;
      case llvm::Instruction::SRem:
        return ar::BinaryOperation::SRem;
      case llvm::Instruction::Shl:
        return ar::BinaryOperation::SShl;
      case llvm::Instruction::LShr:
        return ar::BinaryOperation::SLShr;
      case llvm::Instruction::AShr:
        return ar::BinaryOperation::SAShr;
      case llvm::Instruction::And:
        return ar::BinaryOperation::SAnd;
      case llvm::Instruction::Or:
        return ar::BinaryOperation::SOr;
      case llvm::Instruction::Xor:
        return ar::BinaryOperation::SXor;
      default:
        ikos_unreachable("unreachable");
    }
  }
}

static ar::BinaryOperation::Operator convert_float_bin_op(
    llvm::BinaryOperator::BinaryOps op) {
  switch (op) {
    case llvm::Instruction::FAdd:
      return ar::BinaryOperation::FAdd;
    case llvm::Instruction::FSub:
      return ar::BinaryOperation::FSub;
    case llvm::Instruction::FMul:
      return ar::BinaryOperation::FMul;
    case llvm::Instruction::FDiv:
      return ar::BinaryOperation::FDiv;
    case llvm::Instruction::FRem:
      return ar::BinaryOperation::FRem;
    default:
      ikos_unreachable("unreachable");
  }
}

void FunctionImporter::translate_binary_operator(
    BasicBlockTranslation* bb_translation, llvm::BinaryOperator* inst) {
  llvm::Type* llvm_type = inst->getType();

  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(inst));
  this->mark_variable_mapping(inst, var);

  if (llvm_type->isIntegerTy()) {
    // Integer binary operation
    ar::Signedness sign = ar::Signed;
    ar::IntegerType* stmt_type = nullptr; // type of the operands (or null)
    ar::Value* left = nullptr;  // left operand (null if not yet translated)
    ar::Value* right = nullptr; // right operand (null if not yet translated)

    // Guess the type
    switch (inst->getOpcode()) {
      case llvm::Instruction::Add:
      case llvm::Instruction::Sub:
      case llvm::Instruction::Mul: {
        sign = sign_from_wraps(inst);
      } break;
      case llvm::Instruction::UDiv:
      case llvm::Instruction::URem: {
        sign = ar::Unsigned;
      } break;
      case llvm::Instruction::SDiv:
      case llvm::Instruction::SRem: {
        sign = ar::Signed;
      } break;
      case llvm::Instruction::Shl:
      case llvm::Instruction::LShr:
      case llvm::Instruction::AShr:
      case llvm::Instruction::And:
      case llvm::Instruction::Or:
      case llvm::Instruction::Xor: {
        // No sign requirements, use signedness of first non-constant operand
        if (!llvm::isa< llvm::Constant >(inst->getOperand(0))) {
          left = this->translate_value(bb_translation,
                                       inst->getOperand(0),
                                       nullptr);
          stmt_type = ar::cast< ar::IntegerType >(left->type());
        } else {
          right = this->translate_value(bb_translation,
                                        inst->getOperand(1),
                                        nullptr);
          stmt_type = ar::cast< ar::IntegerType >(right->type());
        }
        sign = stmt_type->sign();
      } break;
      default: {
        ikos_unreachable("unreachable");
      }
    }

    if (stmt_type == nullptr) {
      stmt_type = ar::cast< ar::IntegerType >(
          _ctx.type_imp->translate_type(llvm_type, sign));
    }

    // Translate operands
    if (left == nullptr) {
      left =
          this->translate_value(bb_translation, inst->getOperand(0), stmt_type);
    }
    if (right == nullptr) {
      right =
          this->translate_value(bb_translation, inst->getOperand(1), stmt_type);
    }

    // Result of the ar::BinaryOperation
    // If we need a cast, this is a temporary variable
    ar::InternalVariable* result = var;
    if (stmt_type != var->type()) {
      result = ar::InternalVariable::create(this->_body, stmt_type);
      result->set_frontend< llvm::Value >(inst);
    }

    // Add the no-wrap flag
    bool no_wrap = false;
    if (auto wrapping_inst =
            llvm::dyn_cast< llvm::OverflowingBinaryOperator >(inst)) {
      no_wrap = wrapping_inst->hasNoSignedWrap() ||
                wrapping_inst->hasNoUnsignedWrap();
    }

    // Add the exact flag
    bool exact = false;
    if (auto exact_inst = llvm::dyn_cast< llvm::PossiblyExactOperator >(inst)) {
      exact = exact_inst->isExact();
    }

    // Create statement
    auto stmt =
        ar::BinaryOperation::create(convert_int_bin_op(inst->getOpcode(), sign),
                                    result,
                                    left,
                                    right,
                                    no_wrap,
                                    exact);
    stmt->set_frontend< llvm::Value >(inst);
    bb_translation->add_statement(std::move(stmt));

    // Add a cast from result to var, if required
    if (stmt_type != var->type()) {
      this->add_bitcast(bb_translation, var, result);
    }
  } else if (llvm_type->isFloatingPointTy()) {
    ar::Value* left =
        this->translate_value(bb_translation, inst->getOperand(0), nullptr);
    ar::Value* right =
        this->translate_value(bb_translation, inst->getOperand(1), nullptr);

    ikos_assert(left->type() == var->type());

    // Create statement
    auto stmt =
        ar::BinaryOperation::create(convert_float_bin_op(inst->getOpcode()),
                                    var,
                                    left,
                                    right);
    stmt->set_frontend< llvm::Value >(inst);
    bb_translation->add_statement(std::move(stmt));
  } else {
    std::ostringstream buf;
    buf << "unexpected llvm::BinaryOperator (opcode: " << inst->getOpcodeName()
        << ")";
    throw ImportError(buf.str());
  }
}

static ar::Comparison::Predicate convert_int_predicate(
    llvm::CmpInst::Predicate pred, ar::Signedness sign) {
  if (sign == ar::Signed) {
    switch (pred) {
      case llvm::CmpInst::ICMP_EQ:
        return ar::Comparison::SIEQ;
      case llvm::CmpInst::ICMP_NE:
        return ar::Comparison::SINE;
      case llvm::CmpInst::ICMP_SGT:
        return ar::Comparison::SIGT;
      case llvm::CmpInst::ICMP_SGE:
        return ar::Comparison::SIGE;
      case llvm::CmpInst::ICMP_SLT:
        return ar::Comparison::SILT;
      case llvm::CmpInst::ICMP_SLE:
        return ar::Comparison::SILE;
      default:
        ikos_unreachable("unreachable");
    }
  } else {
    switch (pred) {
      case llvm::CmpInst::ICMP_EQ:
        return ar::Comparison::UIEQ;
      case llvm::CmpInst::ICMP_NE:
        return ar::Comparison::UINE;
      case llvm::CmpInst::ICMP_UGT:
        return ar::Comparison::UIGT;
      case llvm::CmpInst::ICMP_UGE:
        return ar::Comparison::UIGE;
      case llvm::CmpInst::ICMP_ULT:
        return ar::Comparison::UILT;
      case llvm::CmpInst::ICMP_ULE:
        return ar::Comparison::UILE;
      default:
        ikos_unreachable("unreachable");
    }
  }
}

static ar::Comparison::Predicate convert_ptr_predicate(
    llvm::CmpInst::Predicate pred) {
  switch (pred) {
    case llvm::CmpInst::ICMP_EQ:
      return ar::Comparison::PEQ;
    case llvm::CmpInst::ICMP_NE:
      return ar::Comparison::PNE;
    case llvm::CmpInst::ICMP_UGT:
      return ar::Comparison::PGT;
    case llvm::CmpInst::ICMP_UGE:
      return ar::Comparison::PGE;
    case llvm::CmpInst::ICMP_ULT:
      return ar::Comparison::PLT;
    case llvm::CmpInst::ICMP_ULE:
      return ar::Comparison::PLE;
    default:
      ikos_unreachable("unreachable");
  }
}

static ar::Comparison::Predicate convert_float_predicate(
    llvm::CmpInst::Predicate pred) {
  switch (pred) {
    case llvm::CmpInst::FCMP_OEQ:
      return ar::Comparison::FOEQ;
    case llvm::CmpInst::FCMP_OGT:
      return ar::Comparison::FOGT;
    case llvm::CmpInst::FCMP_OGE:
      return ar::Comparison::FOGE;
    case llvm::CmpInst::FCMP_OLT:
      return ar::Comparison::FOLT;
    case llvm::CmpInst::FCMP_OLE:
      return ar::Comparison::FOLE;
    case llvm::CmpInst::FCMP_ONE:
      return ar::Comparison::FONE;
    case llvm::CmpInst::FCMP_ORD:
      return ar::Comparison::FORD;
    case llvm::CmpInst::FCMP_UNO:
      return ar::Comparison::FUNO;
    case llvm::CmpInst::FCMP_UEQ:
      return ar::Comparison::FUEQ;
    case llvm::CmpInst::FCMP_UGT:
      return ar::Comparison::FUGT;
    case llvm::CmpInst::FCMP_UGE:
      return ar::Comparison::FUGE;
    case llvm::CmpInst::FCMP_ULT:
      return ar::Comparison::FULT;
    case llvm::CmpInst::FCMP_ULE:
      return ar::Comparison::FULE;
    case llvm::CmpInst::FCMP_UNE:
      return ar::Comparison::FUNE;
    case llvm::CmpInst::FCMP_FALSE:
    case llvm::CmpInst::FCMP_TRUE: {
      std::ostringstream buf;
      buf << "unsupported llvm::CmpInst predicate: "
          << llvm::CmpInst::getPredicateName(pred).str() << ")";
      throw ImportError(buf.str());
    }
    default:
      ikos_unreachable("unreachable");
  }
}

void FunctionImporter::translate_cmp(BasicBlockTranslation* bb_translation,
                                     llvm::CmpInst* cmp) {
  llvm::Type* llvm_type = cmp->getOperand(0)->getType();

  if (cmp->isIntPredicate() && llvm_type->isIntegerTy()) {
    // Integer comparison
    ar::Signedness sign = ar::Signed;
    ar::IntegerType* ar_type = nullptr;
    ar::Value* left = nullptr;
    ar::Value* right = nullptr;

    if (cmp->isSigned()) {
      sign = ar::Signed;
    } else if (cmp->isUnsigned()) {
      sign = ar::Unsigned;
    } else {
      // Use signedness of the first non-constant operand
      if (!llvm::isa< llvm::Constant >(cmp->getOperand(0))) {
        left =
            this->translate_value(bb_translation, cmp->getOperand(0), nullptr);
        ar_type = ar::cast< ar::IntegerType >(left->type());
      } else {
        right =
            this->translate_value(bb_translation, cmp->getOperand(1), nullptr);
        ar_type = ar::cast< ar::IntegerType >(right->type());
      }
      sign = ar_type->sign();
    }

    if (ar_type == nullptr) {
      ar_type = ar::cast< ar::IntegerType >(
          _ctx.type_imp->translate_type(llvm_type, sign));
    }

    // Translate operands
    if (left == nullptr) {
      left = this->translate_value(bb_translation, cmp->getOperand(0), ar_type);
    }
    if (right == nullptr) {
      right =
          this->translate_value(bb_translation, cmp->getOperand(1), ar_type);
    }

    // Translate result
    ar::InternalVariable* result =
        ar::InternalVariable::create(this->_body, this->infer_type(cmp));
    this->mark_variable_mapping(cmp, result);

    // Create statement
    auto pred = convert_int_predicate(cmp->getPredicate(), sign);
    auto stmt = ar::Comparison::create(pred, left, right);
    stmt->set_frontend< llvm::Value >(cmp);
    bb_translation->add_comparison(result, std::move(stmt));
  } else if ((cmp->isIntPredicate() && llvm_type->isPointerTy()) ||
             cmp->isFPPredicate()) {
    // Translate operands
    ar::Value* left =
        this->translate_value(bb_translation, cmp->getOperand(0), nullptr);
    ar::Value* right =
        this->translate_value(bb_translation, cmp->getOperand(1), nullptr);

    // Translate result
    ar::InternalVariable* result =
        ar::InternalVariable::create(this->_body, this->infer_type(cmp));
    this->mark_variable_mapping(cmp, result);

    // Create statement
    ar::Comparison::Predicate pred;
    if (llvm_type->isPointerTy()) {
      pred = convert_ptr_predicate(cmp->getPredicate());
    } else {
      pred = convert_float_predicate(cmp->getPredicate());
    }
    auto stmt = ar::Comparison::create(pred, left, right);
    stmt->set_frontend< llvm::Value >(cmp);
    bb_translation->add_comparison(result, std::move(stmt));
  } else {
    std::ostringstream buf;
    buf << "unexpected llvm::CmpInst (predicate: "
        << llvm::CmpInst::getPredicateName(cmp->getPredicate()).str() << ")";
    throw ImportError(buf.str());
  }
}

void FunctionImporter::translate_branch(BasicBlockTranslation* bb_translation,
                                        llvm::BranchInst* br) {
  if (br->isUnconditional()) {
    bb_translation->add_unconditional_branching(br, br->getSuccessor(0));
  } else {
    // Translate condition (Get the associated ar::Variable)
    llvm::Value* condition = br->getCondition();

    if (llvm::isa< llvm::Instruction >(condition) ||
        llvm::isa< llvm::Argument >(condition)) {
      auto it = this->_variables.find(condition);
      check_import(it != this->_variables.end(),
                   "conditiof of llvm::BranchInst hasn't been translated");
      auto var = ar::cast< ar::InternalVariable >(it->second);

      // Add branch
      bb_translation->add_conditional_branching(br, var);
    } else if (auto cst = llvm::dyn_cast< llvm::ConstantInt >(condition)) {
      bb_translation->add_unconditional_branching(br,
                                                  br->getSuccessor(
                                                      cst->isZero() ? 1 : 0));
    } else {
      throw ImportError("unexpected condition for llvm::BranchInst");
    }
  }
}

void FunctionImporter::translate_return(BasicBlockTranslation* bb_translation,
                                        llvm::ReturnInst* ret) {
  // Translate operand
  ar::Value* operand = nullptr;

  if (ret->getNumOperands() > 0) {
    operand = this->translate_value(bb_translation,
                                    ret->getReturnValue(),
                                    this->_ar_fun->type()->return_type());
  }

  // Create statement
  auto stmt = ar::ReturnValue::create(operand);
  stmt->set_frontend< llvm::Value >(ret);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_phi(BasicBlockTranslation* /*bb_translation*/,
                                     llvm::PHINode* phi) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(phi));
  this->mark_variable_mapping(phi, var);

  // We will add the assignments later,
  // in translate_phi_late, called by translate_phi_nodes().
}

static bool is_valid_bitcast(ar::Type* from, ar::Type* to) {
  return (from->is_pointer() && to->is_pointer()) ||
         (from->is_integer() && to->is_integer() &&
          ar::cast< ar::IntegerType >(from)->bit_width() ==
              ar::cast< ar::IntegerType >(to)->bit_width());
}

void FunctionImporter::translate_phi_late(BasicBlockTranslation* bb_translation,
                                          llvm::PHINode* phi) {
  auto result = ar::cast< ar::InternalVariable >(this->_variables[phi]);

  for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
    llvm::Value* llvm_value = phi->getIncomingValue(i);
    llvm::BasicBlock* llvm_bb = phi->getIncomingBlock(i);

    // Create an ar::BasicBlock
    ar::BasicBlock* ar_bb = bb_translation->input_basic_block(llvm_bb);

    // Translate the incoming value
    ar::Value* ar_value = nullptr;

    if (llvm::isa< llvm::Constant >(llvm_value) &&
        !llvm::isa< llvm::GlobalValue >(llvm_value)) {
      ar_value =
          this->translate_value(bb_translation, llvm_value, result->type());
    } else {
      ar_value = this->translate_value(bb_translation, llvm_value, nullptr);
    }

    if (ar_value->type() == result->type()) {
      // Use an assignment
      auto stmt = ar::Assignment::create(result, ar_value);
      stmt->set_frontend< llvm::Value >(phi);
      ar_bb->push_back(std::move(stmt));
    } else if (is_valid_bitcast(ar_value->type(), result->type())) {
      // Use a bitcast
      auto stmt = ar::UnaryOperation::create(ar::UnaryOperation::Bitcast,
                                             result,
                                             ar_value);
      stmt->set_frontend< llvm::Value >(phi);
      ar_bb->push_back(std::move(stmt));
    } else {
      throw ImportError("unexpected ar::Type in translate_phi_late()");
    }
  }
}

void FunctionImporter::translate_extractvalue(
    BasicBlockTranslation* bb_translation, llvm::ExtractValueInst* inst) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(inst));
  this->mark_variable_mapping(inst, var);

  // Translate aggregate
  ar::Value* aggregate = this->translate_value(bb_translation,
                                               inst->getAggregateOperand(),
                                               nullptr);

  // Translate offset
  llvm::Type* indexed_type = inst->getAggregateOperand()->getType();
  ar::IntegerConstant* offset =
      this->translate_indexes(indexed_type, inst->idx_begin(), inst->idx_end());

  // Create statement
  auto stmt = ar::ExtractElement::create(var, aggregate, offset);
  stmt->set_frontend< llvm::Value >(inst);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_insertvalue(
    BasicBlockTranslation* bb_translation, llvm::InsertValueInst* inst) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(inst));
  this->mark_variable_mapping(inst, var);

  // Translate aggregate
  ar::Value* aggregate = this->translate_value(bb_translation,
                                               inst->getAggregateOperand(),
                                               nullptr);

  // Translate offset
  llvm::Type* indexed_type = inst->getAggregateOperand()->getType();
  ar::IntegerConstant* offset =
      this->translate_indexes(indexed_type, inst->idx_begin(), inst->idx_end());

  // Translate element
  ar::Value* element = this->translate_value(bb_translation,
                                             inst->getInsertedValueOperand(),
                                             nullptr);

  // Create statement
  auto stmt = ar::InsertElement::create(var, aggregate, offset, element);
  stmt->set_frontend< llvm::Value >(inst);
  bb_translation->add_statement(std::move(stmt));
}

ar::IntegerConstant* FunctionImporter::translate_indexes(
    llvm::Type* indexed_type,
    llvm::ExtractValueInst::idx_iterator begin,
    llvm::ExtractValueInst::idx_iterator end) {
  ar::ZNumber offset(0);

  for (auto it = begin; it != end; ++it) {
    unsigned idx = *it;

    if (auto struct_type = llvm::dyn_cast< llvm::StructType >(indexed_type)) {
      offset += this->_llvm_data_layout.getStructLayout(struct_type)
                    ->getElementOffset(idx);
    } else if (auto seq_type =
                   llvm::dyn_cast< llvm::SequentialType >(indexed_type)) {
      ar::ZNumber element_size(
          this->_llvm_data_layout.getTypeAllocSize(seq_type->getElementType()));
      offset += element_size * idx;
    } else {
      ikos_unreachable("unexpected indexed type");
    }

    auto comp_type = llvm::cast< llvm::CompositeType >(indexed_type);
    indexed_type = comp_type->getTypeAtIndex(idx);
  }

  auto size_type = ar::IntegerType::size_type(this->_bundle);
  return ar::IntegerConstant::get(this->_context,
                                  size_type,
                                  ar::MachineInt(offset,
                                                 size_type->bit_width(),
                                                 size_type->sign()));
}

void FunctionImporter::translate_unreachable(
    BasicBlockTranslation* bb_translation, llvm::UnreachableInst* unreachable) {
  auto stmt = ar::Unreachable::create();
  stmt->set_frontend< llvm::Value >(unreachable);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_landingpad(
    BasicBlockTranslation* bb_translation, llvm::LandingPadInst* landingpad) {
  // Translate result variable
  ar::InternalVariable* var =
      ar::InternalVariable::create(this->_body, this->infer_type(landingpad));
  this->mark_variable_mapping(landingpad, var);

  // Create statement
  auto stmt = ar::LandingPad::create(var);
  stmt->set_frontend< llvm::Value >(landingpad);
  bb_translation->add_statement(std::move(stmt));
}

void FunctionImporter::translate_resume(BasicBlockTranslation* bb_translation,
                                        llvm::ResumeInst* resume) {
  // Translate operand
  auto operand = ar::cast< ar::InternalVariable >(
      this->translate_value(bb_translation, resume->getOperand(0), nullptr));

  // Create statement
  auto stmt = ar::Resume::create(operand);
  stmt->set_frontend< llvm::Value >(resume);
  bb_translation->add_statement(std::move(stmt));
}

ar::Value* FunctionImporter::translate_constant(
    BasicBlockTranslation* bb_translation,
    llvm::Constant* cst,
    ar::Type* type) {
  return _ctx.constant_imp->translate_constant(cst, type, bb_translation->main);
}

ar::Value* FunctionImporter::translate_value(
    BasicBlockTranslation* bb_translation, llvm::Value* value, ar::Type* type) {
  if (auto cst = llvm::dyn_cast< llvm::Constant >(value)) {
    return this->translate_constant(bb_translation, cst, type);
  } else if (llvm::isa< llvm::Instruction >(value) ||
             llvm::isa< llvm::Argument >(value)) {
    // This value as been translated before
    auto it = this->_variables.find(value);
    ikos_assert_msg(it != this->_variables.end(),
                    "value hasn't been translated yet");

    ar::Variable* var = it->second;

    if (type == nullptr || var->type() == type) {
      return var;
    } else {
      // Add a cast from var->type() to type
      return this->add_bitcast(bb_translation, var, type);
    }
  } else if (auto inline_asm = llvm::dyn_cast< llvm::InlineAsm >(value)) {
    return this->translate_inline_asm(inline_asm, type);
  } else {
    throw ImportError("unexpected llvm::Value in translate_value()");
  }
}

ar::InlineAssemblyConstant* FunctionImporter::translate_inline_asm(
    llvm::InlineAsm* inline_asm, ar::Type* type) {
  // If no specific type is needed, just use translate_type(cst->getType())
  if (type == nullptr) {
    type = _ctx.type_imp->translate_type(inline_asm->getType(), ar::Signed);
  }

  return ar::InlineAssemblyConstant::get(this->_context,
                                         ar::cast< ar::PointerType >(type),
                                         inline_asm->getAsmString());
}

ar::InternalVariable* FunctionImporter::add_bitcast(
    BasicBlockTranslation* bb_translation, ar::Variable* var, ar::Type* type) {
  ikos_assert(type != nullptr);

  // Create an internal variable containing the result of the cast
  auto result = ar::InternalVariable::create(this->_body, type);
  result->set_frontend(*var);

  return this->add_bitcast(bb_translation, result, var);
}

ar::InternalVariable* FunctionImporter::add_bitcast(
    BasicBlockTranslation* bb_translation,
    ar::InternalVariable* result,
    ar::Variable* operand) {
  if (!is_valid_bitcast(operand->type(), result->type())) {
    throw ImportError("unexpected ar::Type in add_bitcast()");
  }

  auto stmt =
      ar::UnaryOperation::create(ar::UnaryOperation::Bitcast, result, operand);
  if (operand->has_frontend()) {
    stmt->set_frontend(*operand);
  } else if (result->has_frontend()) {
    stmt->set_frontend(*result);
  }
  bb_translation->add_statement(std::move(stmt));

  return result;
}

ar::Value* FunctionImporter::translate_cast_integer_value(
    BasicBlockTranslation* bb_translation,
    llvm::Value* value,
    ar::IntegerType* type) {
  ikos_assert(type != nullptr);

  if (auto cst = llvm::dyn_cast< llvm::Constant >(value)) {
    return _ctx.constant_imp->translate_cast_integer_constant(cst, type);
  } else if (llvm::isa< llvm::Instruction >(value) ||
             llvm::isa< llvm::Argument >(value)) {
    // This value as been translated before
    auto it = this->_variables.find(value);
    ikos_assert_msg(it != this->_variables.end(),
                    "value hasn't been translated yet");

    ar::Variable* var = it->second;

    if (type == nullptr || var->type() == type) {
      return var;
    } else {
      // Add integer casts from var->type() to type
      return this->add_integer_casts(bb_translation, var, type);
    }
  } else {
    throw ImportError(
        "unexpected llvm::Value in translate_cast_integer_value()");
  }
}

ar::InternalVariable* FunctionImporter::add_integer_casts(
    BasicBlockTranslation* bb_translation,
    ar::Variable* var,
    ar::IntegerType* type) {
  ikos_assert(type != nullptr);

  ar::Value* cur_var = var;
  auto cur_type = ar::cast< ar::IntegerType >(cur_var->type());

  // Truncate or extend
  if (cur_type->bit_width() != type->bit_width()) {
    auto res_type = ar::IntegerType::get(this->_context,
                                         type->bit_width(),
                                         cur_type->sign());
    auto res_var = ar::InternalVariable::create(this->_body, res_type);
    res_var->set_frontend(*var);

    ar::UnaryOperation::Operator op;
    if (cur_type->bit_width() < type->bit_width()) {
      if (cur_type->is_signed()) {
        op = ar::UnaryOperation::SExt;
      } else {
        op = ar::UnaryOperation::ZExt;
      }
    } else {
      if (cur_type->is_signed()) {
        op = ar::UnaryOperation::STrunc;
      } else {
        op = ar::UnaryOperation::UTrunc;
      }
    }
    auto stmt = ar::UnaryOperation::create(op, res_var, cur_var);
    stmt->set_frontend(*var);
    bb_translation->add_statement(std::move(stmt));

    cur_type = res_type;
    cur_var = res_var;
  }

  // Sign convertion (bitcast)
  if (cur_type->sign() != type->sign()) {
    auto res_type = type;
    auto res_var = ar::InternalVariable::create(this->_body, res_type);
    res_var->set_frontend(*var);

    auto stmt = ar::UnaryOperation::create(ar::UnaryOperation::Bitcast,
                                           res_var,
                                           cur_var);
    stmt->set_frontend(*var);
    bb_translation->add_statement(std::move(stmt));

    cur_var = res_var;
  }

  return ar::cast< ar::InternalVariable >(cur_var);
}

ar::Type* FunctionImporter::infer_type(llvm::Value* value) {
  // Check for llvm.dbg.declare and llvm.dbg.addr
  if (auto alloca = llvm::dyn_cast< llvm::AllocaInst >(value)) {
    llvm::TinyPtrVector< llvm::DbgInfoIntrinsic* > dbg_addrs =
        llvm::FindDbgAddrUses(alloca);
    auto dbg_addr =
        std::find_if(dbg_addrs.begin(),
                     dbg_addrs.end(),
                     [](llvm::DbgInfoIntrinsic* dbg) {
                       return dbg->getExpression()->getNumElements() == 0;
                     });

    if (dbg_addr != dbg_addrs.end()) {
      llvm::DILocalVariable* di_var = (*dbg_addr)->getVariable();
      auto di_type = llvm::cast_or_null< llvm::DIType >(di_var->getRawType());

      // Aggressive optimizations can mess debug information.
      // If _allow_debug_info_mismatch is true, check
      // TypeImporter::match_di_type() before using any debug info.
      if (!alloca->isArrayAllocation() &&
          (!this->_allow_debug_info_mismatch ||
           _ctx.type_imp->match_di_type(di_type, alloca->getAllocatedType()))) {
        ar::Type* pointee =
            _ctx.type_imp->translate_di_type(di_type,
                                             alloca->getAllocatedType());
        return ar::PointerType::get(this->_context, pointee);
      } else if (alloca->isArrayAllocation() &&
                 (!this->_allow_debug_info_mismatch ||
                  _ctx.type_imp->match_di_type(di_type, alloca->getType()))) {
        return _ctx.type_imp->translate_di_type(di_type, alloca->getType());
      }
    }
  }

  // Check for llvm.dbg.value
  llvm::SmallVector< llvm::DbgValueInst*, 1 > dbg_values;
  llvm::findDbgValues(dbg_values, value);
  auto dbg_value =
      std::find_if(dbg_values.begin(),
                   dbg_values.end(),
                   [](llvm::DbgValueInst* dbg) {
                     return dbg->getExpression()->getNumElements() == 0;
                   });

  if (dbg_value != dbg_values.end()) {
    llvm::DILocalVariable* di_var = (*dbg_value)->getVariable();
    auto di_type = llvm::cast_or_null< llvm::DIType >(di_var->getRawType());

    if (!this->_allow_debug_info_mismatch) {
      return _ctx.type_imp->translate_di_type(di_type, value->getType());
    } else {
      // Aggressive optimizations can mess debug information.
      // Check TypeImporter::match_di_type() before using any debug info

      if (_ctx.type_imp->match_di_type(di_type, value->getType())) {
        return _ctx.type_imp->translate_di_type(di_type, value->getType());
      } else if (auto alloca = llvm::dyn_cast< llvm::AllocaInst >(value)) {
        if (_ctx.type_imp->match_di_type(di_type, alloca->getAllocatedType())) {
          ar::Type* pointee =
              _ctx.type_imp->translate_di_type(di_type,
                                               alloca->getAllocatedType());
          return ar::PointerType::get(this->_context, pointee);
        }
      }
    }
  }

  // Use a heuristic to find a correct type
  boost::container::flat_map< ar::Type*, unsigned > hints;

  for (auto it = value->use_begin(), et = value->use_end(); it != et; ++it) {
    llvm::Use& use = *it;
    TypeHint hint = this->infer_type_hint_use(use);

    if (hint.ignore()) {
      continue;
    }

    hints[hint.type] += hint.score;
  }

  if (hints.empty()) {
    // No hints
    return this->infer_default_type(value);
  } else {
    // Find the type with the biggest score
    auto it =
        std::max_element(hints.begin(), hints.end(), [](auto& a, auto& b) {
          return a.second < b.second;
        });
    return it->first;
  }
}

ar::Type* FunctionImporter::infer_default_type(llvm::Value* value) {
  // No hints were found
  // Fallback to translate_type() and prefer signed integers
  ar::Signedness preferred = ar::Signed;

  if (auto call = llvm::dyn_cast< llvm::CallInst >(value)) {
    // Use the type of the returned value, if it's a direct call
    llvm::Value* called = call->getCalledValue();
    if (auto fun = llvm::dyn_cast< llvm::Function >(called)) {
      return _ctx.bundle_imp->translate_function(fun)->type()->return_type();
    }
  } else if (auto cast = llvm::dyn_cast< llvm::CastInst >(value)) {
    preferred = (cast->getOpcode() == llvm::Instruction::ZExt ||
                 cast->getOpcode() == llvm::Instruction::FPToUI)
                    ? ar::Unsigned
                    : ar::Signed;
  }

  return _ctx.type_imp->translate_type(value->getType(), preferred);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use(
    llvm::Use& use) {
  llvm::Value* user = use.getUser();

  if (auto alloca = llvm::dyn_cast< llvm::AllocaInst >(user)) {
    return this->infer_type_hint_use_alloca(use, alloca);
  } else if (auto store = llvm::dyn_cast< llvm::StoreInst >(user)) {
    return this->infer_type_hint_use_store(use, store);
  } else if (auto load = llvm::dyn_cast< llvm::LoadInst >(user)) {
    return this->infer_type_hint_use_load(use, load);
  } else if (auto call = llvm::dyn_cast< llvm::CallInst >(user)) {
    return this->infer_type_hint_use_call(use, call);
  } else if (auto invoke = llvm::dyn_cast< llvm::InvokeInst >(user)) {
    return this->infer_type_hint_use_invoke(use, invoke);
  } else if (auto cast = llvm::dyn_cast< llvm::CastInst >(user)) {
    return this->infer_type_hint_use_cast(use, cast);
  } else if (auto gep = llvm::dyn_cast< llvm::GetElementPtrInst >(user)) {
    return this->infer_type_hint_use_getelementptr(use, gep);
  } else if (auto binary_op = llvm::dyn_cast< llvm::BinaryOperator >(user)) {
    return this->infer_type_hint_use_binary_operator(use, binary_op);
  } else if (auto cmp = llvm::dyn_cast< llvm::CmpInst >(user)) {
    return this->infer_type_hint_use_cmp(use, cmp);
  } else if (auto br = llvm::dyn_cast< llvm::BranchInst >(user)) {
    return this->infer_type_hint_use_branch(use, br);
  } else if (auto ret = llvm::dyn_cast< llvm::ReturnInst >(user)) {
    return this->infer_type_hint_use_return(use, ret);
  } else if (auto phi = llvm::dyn_cast< llvm::PHINode >(user)) {
    return this->infer_type_hint_use_phi(use, phi);
  } else if (llvm::isa< llvm::ExtractValueInst >(user)) {
    return TypeHint(); // no hint
  } else if (llvm::isa< llvm::InsertValueInst >(user)) {
    return TypeHint(); // no hint
  } else if (llvm::isa< llvm::ResumeInst >(user)) {
    return TypeHint(); // no hint
  } else if (llvm::isa< llvm::SelectInst >(user)) {
    throw ImportError(
        "select instruction not supported (use the -lower-select pass?)");
  } else if (llvm::isa< llvm::SwitchInst >(user)) {
    throw ImportError(
        "switch instruction not supported (use the -lowerswitch pass?)");
  } else if (auto inst = llvm::dyn_cast< llvm::Instruction >(user)) {
    std::ostringstream buf;
    buf << "unsupported llvm::Instruction in infer_type_hint_use() (opcode: "
        << inst->getOpcodeName() << ")";
    throw ImportError(buf.str());
  } else {
    throw ImportError("unexpected user in infer_type_hint_use()");
  }
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_alloca(
    llvm::Use& use, llvm::AllocaInst* alloca) {
  // Alloca array size has to be unsigned
  ikos_assert(use.getOperandNo() == 0);
  ikos_ignore(use);
  llvm::Type* llvm_type = alloca->getArraySize()->getType();
  ar::Type* ar_type = _ctx.type_imp->translate_type(llvm_type, ar::Unsigned);
  return TypeHint(ar_type, 5);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_store(
    llvm::Use& use, llvm::StoreInst* store) {
  if (use.getOperandNo() == 0) {
    // value is the stored value
    TypeHint hint = this->infer_type_hint_operand(store->getPointerOperand());
    if (!hint.ignore()) {
      hint.type = ar::cast< ar::PointerType >(hint.type)->pointee();
    }
    return hint;
  } else if (use.getOperandNo() == 1) {
    // value is the pointer operand
    TypeHint hint = this->infer_type_hint_operand(store->getValueOperand());
    if (!hint.ignore()) {
      hint.type = ar::PointerType::get(this->_context, hint.type);
    }
    return hint;
  } else {
    ikos_unreachable("unreachable");
  }
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_load(
    llvm::Use& /*use*/, llvm::LoadInst* load) {
  // value is the pointer operand
  TypeHint hint = this->infer_type_hint_operand(load);
  if (!hint.ignore()) {
    hint.type = ar::PointerType::get(this->_context, hint.type);
  }
  return hint;
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_call(
    llvm::Use& use, llvm::CallInst* call) {
  return this->infer_type_hint_use_call_helper(use, call);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_invoke(
    llvm::Use& use, llvm::InvokeInst* invoke) {
  return this->infer_type_hint_use_call_helper(use, invoke);
}

template < typename CallInstType >
FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_call_helper(
    llvm::Use& use, CallInstType* call) {
  if (use.getOperandNo() >= call->getNumArgOperands()) {
    // Called function pointer
    return TypeHint();
  }

  llvm::Function* called = call->getCalledFunction();

  if (called) {
    // Direct call
    ar::Function* ar_fun = _ctx.bundle_imp->translate_function(called);

    if (ar_fun == nullptr) {
      // Ignored intrinsic call (such as dbg.declare)
      return TypeHint();
    }

    if (ar_fun->is_var_arg() &&
        use.getOperandNo() >= ar_fun->num_parameters()) {
      // Variable argument, ignore
      return TypeHint();
    }

    ar::Type* ar_type = ar_fun->type()->param_type(use.getOperandNo());

    // Compute a score
    llvm::DISubprogram* dbg = called->getSubprogram();
    unsigned score = (dbg == nullptr) ? 10 : 1000;

    return TypeHint(ar_type, score);
  }

  // Indirect call
  return TypeHint();
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_cast(
    llvm::Use& /*use*/, llvm::CastInst* cast) {
  ar::Signedness sign = ar::Signed;

  switch (cast->getOpcode()) {
    case llvm::Instruction::Trunc: {
      return TypeHint(); // no hint
    }
    case llvm::Instruction::ZExt: {
      sign = ar::Unsigned;
    } break;
    case llvm::Instruction::SExt: {
      sign = ar::Signed;
    } break;
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI: {
      return TypeHint(); // no hint
    }
    case llvm::Instruction::UIToFP: {
      sign = ar::Unsigned;
    } break;
    case llvm::Instruction::SIToFP: {
      sign = ar::Signed;
    } break;
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
    case llvm::Instruction::PtrToInt: {
      return TypeHint(); // no hint
    }
    case llvm::Instruction::IntToPtr: {
      sign = ar::Unsigned;
    } break;
    case llvm::Instruction::BitCast: {
      return TypeHint(); // no hint
    }
    default: {
      std::ostringstream buf;
      buf << "unexpected llvm::CastInst (opcode: " << cast->getOpcodeName()
          << ")";
      throw ImportError(buf.str());
    }
  }

  ar::Type* type = _ctx.type_imp->translate_type(cast->getSrcTy(), sign);
  return TypeHint(type, 5);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_getelementptr(
    llvm::Use& /*use*/, llvm::GetElementPtrInst* /*gep*/) {
  // GetElementPtr does not add any restriction on its operand
  // The first operand can be a pointer on any type
  // The other operands can be integers of any signedness and bit-width
  return TypeHint();
}

FunctionImporter::TypeHint FunctionImporter::
    infer_type_hint_use_binary_operator(llvm::Use& use,
                                        llvm::BinaryOperator* inst) {
  ar::Signedness sign = ar::Signed;
  unsigned score = 5;

  switch (inst->getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul: {
      sign = sign_from_wraps(inst);
    } break;
    case llvm::Instruction::UDiv:
    case llvm::Instruction::URem: {
      sign = ar::Unsigned;
    } break;
    case llvm::Instruction::SDiv:
    case llvm::Instruction::SRem: {
      sign = ar::Signed;
    } break;
    case llvm::Instruction::Shl: {
      return TypeHint(); // no hint
    }
    case llvm::Instruction::LShr: {
      if (use.getOperandNo() == 0) {
        sign = ar::Unsigned;
      } else {
        return TypeHint(); // no hint
      }
    } break;
    case llvm::Instruction::AShr: {
      if (use.getOperandNo() == 0) {
        sign = ar::Signed;
      } else {
        return TypeHint(); // no hint
      }
    } break;
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor: {
      // prefer unsigned types for bitwise operators
      sign = ar::Unsigned;
      score = 1;
    } break;
    case llvm::Instruction::FRem:
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
    case llvm::Instruction::FMul:
    case llvm::Instruction::FDiv: {
      return TypeHint(); // no hint, sign is irrelevant
    }
    default: {
      ikos_unreachable("unreachable");
    }
  }

  llvm::Type* llvm_type = inst->getOperand(use.getOperandNo())->getType();
  ar::Type* ar_type = _ctx.type_imp->translate_type(llvm_type, sign);
  return TypeHint(ar_type, score);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_cmp(
    llvm::Use& use, llvm::CmpInst* cmp) {
  llvm::Type* llvm_type = cmp->getOperand(use.getOperandNo())->getType();

  if (cmp->isIntPredicate() && llvm_type->isIntegerTy()) {
    // Integer comparison
    if (cmp->isSigned()) {
      ar::Type* ar_type = _ctx.type_imp->translate_type(llvm_type, ar::Signed);
      return TypeHint(ar_type, 5);
    } else if (cmp->isUnsigned()) {
      ar::Type* ar_type =
          _ctx.type_imp->translate_type(llvm_type, ar::Unsigned);
      return TypeHint(ar_type, 5);
    } else {
      // Use the other operand type as a hint
      TypeHint hint = this->infer_type_hint_operand(
          cmp->getOperand(1 - use.getOperandNo()));
      hint.set_score(2);
      return hint;
    }
  } else if (cmp->isIntPredicate() && llvm_type->isPointerTy()) {
    // Pointer comparison
    // Use the other operand type as a hint
    TypeHint hint =
        this->infer_type_hint_operand(cmp->getOperand(1 - use.getOperandNo()));
    hint.set_score(2);
    return hint;
  } else if (cmp->isFPPredicate()) {
    return TypeHint(); // no hint
  } else {
    std::ostringstream buf;
    buf << "unexpected llvm::CmpInst (predicate: "
        << llvm::CmpInst::getPredicateName(cmp->getPredicate()).str() << ")";
    throw ImportError(buf.str());
  }
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_branch(
    llvm::Use& use, llvm::BranchInst* br) {
  // Condition operand
  ikos_assert(br->isConditional());
  ikos_ignore(use);
  ikos_assert(use.getOperandNo() == 0);
  llvm::Value* cond = br->getCondition();

  // Prefer unsigned
  ar::Type* type = _ctx.type_imp->translate_type(cond->getType(), ar::Unsigned);
  return TypeHint(type, 2);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_return(
    llvm::Use& /*use*/, llvm::ReturnInst* /*ret*/) {
  return TypeHint(this->_ar_fun->type()->return_type(), 5);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_use_phi(
    llvm::Use& /*use*/, llvm::PHINode* phi) {
  return this->infer_type_hint_operand(phi);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_operand(
    llvm::Value* value) {
  if (auto gv = llvm::dyn_cast< llvm::GlobalVariable >(value)) {
    return this->infer_type_hint_operand_global_variable(gv);
  } else if (auto gv_alias = llvm::dyn_cast< llvm::GlobalAlias >(value)) {
    return this->infer_type_hint_operand(gv_alias->getAliasee());
  } else if (auto fun = llvm::dyn_cast< llvm::Function >(value)) {
    return this->infer_type_hint_operand_function(fun);
  } else if (auto inst = llvm::dyn_cast< llvm::Instruction >(value)) {
    return this->infer_type_hint_operand_instruction(inst);
  } else if (auto arg = llvm::dyn_cast< llvm::Argument >(value)) {
    return this->infer_type_hint_operand_argument(arg);
  } else if (llvm::isa< llvm::Constant >(value)) {
    // Cannot deduce sign information from constants
    return TypeHint();
  } else {
    throw ImportError("unexpected llvm::Value in infer_type_hint_operand()");
  }
}

FunctionImporter::TypeHint FunctionImporter::
    infer_type_hint_operand_global_variable(llvm::GlobalVariable* gv) {
  // Return the ar::GlobalVariable type
  ar::GlobalVariable* ar_gv = _ctx.bundle_imp->translate_global_variable(gv);

  // Compute a score
  llvm::SmallVector< llvm::DIGlobalVariableExpression*, 1 > dbgs;
  gv->getDebugInfo(dbgs);
  unsigned score = dbgs.empty() ? 10 : 1000;

  return TypeHint(ar_gv->type(), score);
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_operand_function(
    llvm::Function* fun) {
  // Return the pointer on the ar::Function type
  ar::Function* ar_fun = _ctx.bundle_imp->translate_function(fun);
  ikos_assert(ar_fun != nullptr);

  ar::Type* ar_type = ar::PointerType::get(this->_context, ar_fun->type());

  // Compute a score
  llvm::DISubprogram* dbg = fun->getSubprogram();
  unsigned score = (dbg == nullptr) ? 10 : 1000;

  return TypeHint(ar_type, score);
}

FunctionImporter::TypeHint FunctionImporter::
    infer_type_hint_operand_instruction(llvm::Instruction* inst) {
  // If already translated, use it as a hint
  auto it = this->_variables.find(inst);
  if (it != this->_variables.end()) {
    return TypeHint(it->second->type(), 2);
  } else {
    // TODO(marthaud): use this->infer_type() ? It could cause an infinite
    // recursion.
    return TypeHint(); // no hint
  }
}

FunctionImporter::TypeHint FunctionImporter::infer_type_hint_operand_argument(
    llvm::Argument* arg) {
  // Return the type of the ar::InternalVariable
  auto ar_arg =
      ar::cast< ar::InternalVariable >(this->_variables.find(arg)->second);

  // Compute a score
  llvm::DISubprogram* dbg = this->_llvm_fun->getSubprogram();
  unsigned score = (dbg == nullptr) ? 10 : 1000;

  return TypeHint(ar_arg->type(), score);
}

BasicBlockTranslation::BasicBlockTranslation(llvm::BasicBlock* source_,
                                             ar::BasicBlock* main_)
    : source(source_), main(main_), outputs{BasicBlockOutput(main)} {}

void BasicBlockTranslation::mark_entry_block() {
  this->main->code()->set_entry_block(this->main);
}

void BasicBlockTranslation::mark_exit_block() {
  check_import(this->outputs.size() == 1,
               "exit block has more than one output");
  this->main->code()->set_exit_block(this->outputs[0].block);
}

void BasicBlockTranslation::mark_unreachable_block() {
  check_import(this->outputs.size() == 1,
               "unreachable block has more than one output");
  this->main->code()->set_unreachable_block(this->outputs[0].block);
}

void BasicBlockTranslation::mark_ehresume_block() {
  check_import(this->outputs.size() == 1,
               "ehresume block has more than one output");
  this->main->code()->set_ehresume_block(this->outputs[0].block);
}

ar::BasicBlock* BasicBlockTranslation::input_basic_block(
    llvm::BasicBlock* llvm_bb) {
  auto it = this->inputs.find(llvm_bb);
  if (it != this->inputs.end()) {
    return it->second;
  }

  // Create basic block
  ar::BasicBlock* ar_bb = ar::BasicBlock::create(this->main->code());

  // Add edge
  ar_bb->add_successor(this->main);

  // Add in the input list
  this->inputs.emplace(llvm_bb, ar_bb);

  return ar_bb;
}

void BasicBlockTranslation::merge_outputs() {
  if (this->outputs.size() < 2) {
    return;
  }

  ar::BasicBlock* dest = ar::BasicBlock::create(this->main->code());

  for (auto it = this->outputs.begin(), et = this->outputs.end(); it != et;
       ++it) {
    ar::BasicBlock* bb = it->block;
    ikos_assert(it->succ == nullptr);

    this->internals.push_back(bb);
    bb->add_successor(dest);
  }

  this->outputs.clear();
  this->outputs.emplace_back(BasicBlockOutput(dest));
}

void BasicBlockTranslation::add_statement(
    std::unique_ptr< ar::Statement > stmt) {
  if (this->outputs.size() == 1) {
    // Move the statement in the only output
    ar::BasicBlock* bb = this->outputs[0].block;
    bb->push_back(std::move(stmt));
  } else {
    // Copy the statement in all the outputs
    for (const auto& output : this->outputs) {
      output.block->push_back(stmt->clone());
    }
  }
}

void BasicBlockTranslation::add_comparison(
    ar::InternalVariable* var, std::unique_ptr< ar::Comparison > cmp) {
  // TODO(marthaud): Add an option that merges the outputs if outputs.size() > 1

  if (this->outputs.size() == 1) {
    ar::BasicBlock* bb = this->outputs[0].block;
    this->internals.push_back(bb);
    this->outputs.clear();

    ar::Comparison* cmp_ptr = cmp.get();
    this->add_comparison_output_bb(bb, std::move(cmp), var, true);
    this->add_comparison_output_bb(bb, cmp_ptr->inverse(), var, false);
  } else {
    std::vector< BasicBlockOutput > prev_outputs;
    std::swap(prev_outputs, this->outputs);
    this->outputs.reserve(prev_outputs.size());

    for (const auto& output : prev_outputs) {
      ar::BasicBlock* bb = output.block;
      this->internals.push_back(bb);
      this->add_comparison_output_bb(bb, cmp->clone(), var, true);
      this->add_comparison_output_bb(bb, cmp->inverse(), var, false);
    }
  }
}

/// \brief Create an ar::Assignment `var = value`
static std::unique_ptr< ar::Assignment > create_bool_assignment(
    ar::Context& ctx, ar::InternalVariable* var, bool value) {
  auto type = ar::cast< ar::IntegerType >(var->type());
  ikos_assert_msg(type->bit_width() == 1, "invalid bit-width for boolean");

  ar::IntegerConstant* cst = ar::IntegerConstant::get(ctx, type, value ? 1 : 0);
  return ar::Assignment::create(var, cst);
}

void BasicBlockTranslation::add_comparison_output_bb(
    ar::BasicBlock* src,
    std::unique_ptr< ar::Statement > cmp,
    ar::InternalVariable* var,
    bool value) {
  auto frontend = cmp->frontend< llvm::Value >();

  // Create basic block
  ar::BasicBlock* dest = ar::BasicBlock::create(src->code());

  // Push comparison
  dest->push_back(std::move(cmp));

  // Push assignment
  std::unique_ptr< ar::Assignment > assign =
      create_bool_assignment(src->context(), var, value);
  assign->set_frontend(frontend);
  dest->push_back(std::move(assign));

  // Add edge
  src->add_successor(dest);

  // Add in the output list
  this->outputs.emplace_back(BasicBlockOutput(dest));
}

void BasicBlockTranslation::add_unconditional_branching(
    llvm::BranchInst* /*br*/, llvm::BasicBlock* succ) {
  for (auto& output : this->outputs) {
    output.succ = succ;
  }
}

void BasicBlockTranslation::add_conditional_branching(
    llvm::BranchInst* br, ar::InternalVariable* cond) {
  llvm::BasicBlock* true_succ = br->getSuccessor(0);
  llvm::BasicBlock* false_succ = br->getSuccessor(1);

  // Check if the condition variable is the result of a CmpInst
  bool has_assign_preds =
      std::all_of(this->outputs.begin(),
                  this->outputs.end(),
                  [=](const BasicBlockOutput& output) {
                    ar::BasicBlock* bb = output.block;
                    return !bb->empty() &&
                           ar::isa< ar::Assignment >(bb->back()) &&
                           ar::cast< ar::Assignment >(bb->back())->result() ==
                               cond &&
                           ar::isa< ar::IntegerConstant >(
                               ar::cast< ar::Assignment >(bb->back())
                                   ->operand());
                  });

  if (has_assign_preds) {
    // In this case, just set the successor accordingly

    // Remove assignment if the variable is only used for the branching
    // statement
    llvm::Value* llvm_condition = br->getCondition();
    bool remove_assign =
        llvm_condition->hasOneUse() && *llvm_condition->user_begin() == br;

    for (auto& output : this->outputs) {
      ar::BasicBlock* bb = output.block;
      auto assign = ar::cast< ar::Assignment >(bb->back());
      auto cst = ar::cast< ar::IntegerConstant >(assign->operand());

      if (cst->value() == 0) {
        output.succ = false_succ;
      } else {
        output.succ = true_succ;
      }

      if (remove_assign) {
        bb->pop_back();
      }
    }
  } else {
    // Otherwise, add comparisons
    std::vector< BasicBlockOutput > prev_outputs;
    std::swap(prev_outputs, this->outputs);
    this->outputs.reserve(2 * prev_outputs.size());

    for (const auto& output : prev_outputs) {
      ar::BasicBlock* bb = output.block;
      this->internals.push_back(bb);

      this->add_conditional_output_bb(br, bb, true_succ, cond, true);
      this->add_conditional_output_bb(br, bb, false_succ, cond, false);
    }
  }
}

/// \brief Create an ar::Comparison `var == value`
static std::unique_ptr< ar::Comparison > create_bool_cmp(
    ar::Context& ctx, ar::InternalVariable* var, bool value) {
  auto type = ar::cast< ar::IntegerType >(var->type());
  ikos_assert_msg(type->bit_width() == 1, "invalid bit-width for boolean");

  ar::IntegerConstant* cst = ar::IntegerConstant::get(ctx, type, value ? 1 : 0);
  return ar::Comparison::create(type->is_signed() ? ar::Comparison::SIEQ
                                                  : ar::Comparison::UIEQ,
                                var,
                                cst);
}

void BasicBlockTranslation::add_conditional_output_bb(
    llvm::BranchInst* br,
    ar::BasicBlock* src,
    llvm::BasicBlock* llvm_dest,
    ar::InternalVariable* cond,
    bool value) {
  // Create basic block
  ar::BasicBlock* ar_dest = ar::BasicBlock::create(src->code());

  // Remove assignment if the variable is only used for the branching statement
  llvm::Value* llvm_condition = br->getCondition();
  bool remove_assign =
      llvm_condition->hasOneUse() && *llvm_condition->user_begin() == br;

  // Add assignment for the result of the comparison
  if (!remove_assign) {
    std::unique_ptr< ar::Comparison > cmp =
        create_bool_cmp(src->context(), cond, value);
    cmp->set_frontend< llvm::Value >(llvm_condition);
    ar_dest->push_back(std::move(cmp));
  }

  // Add edge
  src->add_successor(ar_dest);

  // Add in the output list
  this->outputs.emplace_back(BasicBlockOutput(ar_dest, llvm_dest));
}

void BasicBlockTranslation::add_invoke_branching(
    llvm::BasicBlock* normal_dest, llvm::BasicBlock* exception_dest) {
  if (this->outputs.size() == 1) {
    ar::BasicBlock* bb = this->outputs[0].block;
    this->internals.push_back(bb);
    this->outputs.clear();

    auto invoke = ar::cast< ar::Invoke >(bb->back());
    this->add_invoke_normal_output_bb(bb, invoke, normal_dest);
    this->add_invoke_exception_output_bb(bb, invoke, exception_dest);
  } else {
    std::vector< BasicBlockOutput > prev_outputs;
    std::swap(prev_outputs, this->outputs);
    this->outputs.reserve(2 * prev_outputs.size());

    for (const auto& output : prev_outputs) {
      ar::BasicBlock* bb = output.block;
      this->internals.push_back(bb);

      auto invoke = ar::cast< ar::Invoke >(bb->back());
      this->add_invoke_normal_output_bb(bb, invoke, normal_dest);
      this->add_invoke_exception_output_bb(bb, invoke, exception_dest);
    }
  }
}

void BasicBlockTranslation::add_invoke_normal_output_bb(
    ar::BasicBlock* src, ar::Invoke* invoke, llvm::BasicBlock* llvm_dest) {
  // Create basic block
  ar::BasicBlock* ar_dest = ar::BasicBlock::create(src->code());

  // Add edge
  src->add_successor(ar_dest);

  // Add in the output list
  this->outputs.emplace_back(BasicBlockOutput(ar_dest, llvm_dest));

  // Set invoke normal destination
  invoke->set_normal_dest(ar_dest);
}

void BasicBlockTranslation::add_invoke_exception_output_bb(
    ar::BasicBlock* src, ar::Invoke* invoke, llvm::BasicBlock* llvm_dest) {
  // Create basic block
  ar::BasicBlock* ar_dest = ar::BasicBlock::create(src->code());

  // Add edge
  src->add_successor(ar_dest);

  // Add in the output list
  this->outputs.emplace_back(BasicBlockOutput(ar_dest, llvm_dest));

  // Set invoke exception destination
  invoke->set_exception_dest(ar_dest);
}

} // end namespace import
} // end namespace frontend
} // end namespace ikos
