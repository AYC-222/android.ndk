// Copyright (c) 2020 André Perez Maselco
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

#include "source/fuzz/transformation_add_bit_instruction_synonym.h"

#include "source/fuzz/fuzzer_util.h"
#include "source/fuzz/instruction_descriptor.h"

namespace spvtools {
namespace fuzz {

TransformationAddBitInstructionSynonym::TransformationAddBitInstructionSynonym(
    const spvtools::fuzz::protobufs::TransformationAddBitInstructionSynonym&
        message)
    : message_(message) {}

TransformationAddBitInstructionSynonym::TransformationAddBitInstructionSynonym(
    const uint32_t instruction_result_id,
    const std::vector<uint32_t>& fresh_ids) {
  message_.set_instruction_result_id(instruction_result_id);
  *message_.mutable_fresh_ids() =
      google::protobuf::RepeatedField<google::protobuf::uint32>(
          fresh_ids.begin(), fresh_ids.end());
}

bool TransformationAddBitInstructionSynonym::IsApplicable(
    opt::IRContext* ir_context,
    const TransformationContext& transformation_context) const {
  auto instruction =
      ir_context->get_def_use_mgr()->GetDef(message_.instruction_result_id());

  // TODO(https://github.com/KhronosGroup/SPIRV-Tools/issues/3557):
  //  Right now we only support certain operations. When this issue is addressed
  //  the following conditional can use the function |spvOpcodeIsBit|.
  // |instruction| must be defined and must be a supported bit instruction.
  if (!instruction || (instruction->opcode() != SpvOpBitwiseOr &&
                       instruction->opcode() != SpvOpBitwiseXor &&
                       instruction->opcode() != SpvOpBitwiseAnd)) {
    return false;
  }

  // TODO(https://github.com/KhronosGroup/SPIRV-Tools/issues/3792):
  //  Right now, only integer operands are supported.
  if (ir_context->get_type_mgr()->GetType(instruction->type_id())->AsVector()) {
    return false;
  }

  // TODO(https://github.com/KhronosGroup/SPIRV-Tools/issues/3791):
  //  This condition could be relaxed if the index exists as another integer
  //  type.
  // All bit indexes must be defined as 32-bit unsigned integers.
  uint32_t width = ir_context->get_type_mgr()
                       ->GetType(instruction->type_id())
                       ->AsInteger()
                       ->width();
  for (uint32_t i = 0; i < width; i++) {
    if (!fuzzerutil::MaybeGetIntegerConstant(ir_context, transformation_context,
                                             {i}, 32, false, false)) {
      return false;
    }
  }

  // |message_.fresh_ids.size| must have the exact number of fresh ids required
  // to apply the transformation.
  if (static_cast<uint32_t>(message_.fresh_ids().size()) !=
      GetRequiredFreshIdCount(ir_context, instruction)) {
    return false;
  }

  // All ids in |message_.fresh_ids| must be fresh.
  for (uint32_t fresh_id : message_.fresh_ids()) {
    if (!fuzzerutil::IsFreshId(ir_context, fresh_id)) {
      return false;
    }
  }

  return true;
}

void TransformationAddBitInstructionSynonym::Apply(
    opt::IRContext* ir_context,
    TransformationContext* transformation_context) const {
  auto bit_instruction =
      ir_context->get_def_use_mgr()->GetDef(message_.instruction_result_id());

  switch (bit_instruction->opcode()) {
    case SpvOpBitwiseOr:
    case SpvOpBitwiseXor:
    case SpvOpBitwiseAnd:
      AddBitwiseSynonym(ir_context, transformation_context, bit_instruction);
      break;
    default:
      assert(false && "Should be unreachable.");
      break;
  }

  ir_context->InvalidateAnalysesExceptFor(opt::IRContext::kAnalysisNone);
}

protobufs::Transformation TransformationAddBitInstructionSynonym::ToMessage()
    const {
  protobufs::Transformation result;
  *result.mutable_add_bit_instruction_synonym() = message_;
  return result;
}

uint32_t TransformationAddBitInstructionSynonym::GetRequiredFreshIdCount(
    opt::IRContext* ir_context, opt::Instruction* bit_instruction) {
  // TODO(https://github.com/KhronosGroup/SPIRV-Tools/issues/3557):
  //  Right now, only certain operations are supported.
  switch (bit_instruction->opcode()) {
    case SpvOpBitwiseOr:
    case SpvOpBitwiseXor:
    case SpvOpBitwiseAnd:
      return 4 * ir_context->get_type_mgr()
                     ->GetType(bit_instruction->type_id())
                     ->AsInteger()
                     ->width() -
             1;
    default:
      assert(false && "Unsupported bit instruction.");
      return 0;
  }
}

void TransformationAddBitInstructionSynonym::AddBitwiseSynonym(
    opt::IRContext* ir_context, TransformationContext* transformation_context,
    opt::Instruction* bit_instruction) const {
  // Fresh id iterator.
  auto fresh_id = message_.fresh_ids().begin();

  // |width| is the bit width of operands (8, 16, 32 or 64).
  const uint32_t width = ir_context->get_type_mgr()
                             ->GetType(bit_instruction->type_id())
                             ->AsInteger()
                             ->width();

  // |count| is the number of bits to be extracted and inserted at a time.
  const uint32_t count = fuzzerutil::MaybeGetIntegerConstant(
      ir_context, *transformation_context, {1}, 32, false, false);

  // |bitwise_ids| is the collection of OpBiwise* instructions that evaluate a
  // pair of extracted bits. Those ids will be used to insert the result bits.
  std::vector<uint32_t> bitwise_ids(width);

  for (uint32_t i = 0; i < width; i++) {
    // |offset| is the current bit index.
    uint32_t offset = fuzzerutil::MaybeGetIntegerConstant(
        ir_context, *transformation_context, {i}, 32, false, false);

    // |bit_extract_ids| are the two extracted bits from the operands.
    std::vector<uint32_t> bit_extract_ids;

    // Extracts the i-th bit from operands.
    for (auto operand = bit_instruction->begin() + 2;
         operand != bit_instruction->end(); operand++) {
      auto bit_extract =
          opt::Instruction(ir_context, SpvOpBitFieldUExtract,
                           bit_instruction->type_id(), *fresh_id++,
                           {{SPV_OPERAND_TYPE_ID, operand->words},
                            {SPV_OPERAND_TYPE_ID, {offset}},
                            {SPV_OPERAND_TYPE_ID, {count}}});
      bit_instruction->InsertBefore(MakeUnique<opt::Instruction>(bit_extract));
      fuzzerutil::UpdateModuleIdBound(ir_context, bit_extract.result_id());
      bit_extract_ids.push_back(bit_extract.result_id());
    }

    // Applies |bit_instruction| to the pair of extracted bits.
    auto bitwise =
        opt::Instruction(ir_context, bit_instruction->opcode(),
                         bit_instruction->type_id(), *fresh_id++,
                         {{SPV_OPERAND_TYPE_ID, {bit_extract_ids[0]}},
                          {SPV_OPERAND_TYPE_ID, {bit_extract_ids[1]}}});
    bit_instruction->InsertBefore(MakeUnique<opt::Instruction>(bitwise));
    fuzzerutil::UpdateModuleIdBound(ir_context, bitwise.result_id());
    bitwise_ids[i] = bitwise.result_id();
  }

  // The first two ids in |bitwise_ids| are used to insert the first two bits of
  // the result.
  uint32_t offset = fuzzerutil::MaybeGetIntegerConstant(
      ir_context, *transformation_context, {1}, 32, false, false);
  auto bit_insert = opt::Instruction(ir_context, SpvOpBitFieldInsert,
                                     bit_instruction->type_id(), *fresh_id++,
                                     {{SPV_OPERAND_TYPE_ID, {bitwise_ids[0]}},
                                      {SPV_OPERAND_TYPE_ID, {bitwise_ids[1]}},
                                      {SPV_OPERAND_TYPE_ID, {offset}},
                                      {SPV_OPERAND_TYPE_ID, {count}}});
  bit_instruction->InsertBefore(MakeUnique<opt::Instruction>(bit_insert));
  fuzzerutil::UpdateModuleIdBound(ir_context, bit_insert.result_id());

  // Inserts the remaining bits.
  for (uint32_t i = 2; i < width; i++) {
    offset = fuzzerutil::MaybeGetIntegerConstant(
        ir_context, *transformation_context, {i}, 32, false, false);
    bit_insert =
        opt::Instruction(ir_context, SpvOpBitFieldInsert,
                         bit_instruction->type_id(), *fresh_id++,
                         {{SPV_OPERAND_TYPE_ID, {bit_insert.result_id()}},
                          {SPV_OPERAND_TYPE_ID, {bitwise_ids[i]}},
                          {SPV_OPERAND_TYPE_ID, {offset}},
                          {SPV_OPERAND_TYPE_ID, {count}}});
    bit_instruction->InsertBefore(MakeUnique<opt::Instruction>(bit_insert));
    fuzzerutil::UpdateModuleIdBound(ir_context, bit_insert.result_id());
  }

  ir_context->InvalidateAnalysesExceptFor(opt::IRContext::kAnalysisNone);

  // Adds the fact that the last |bit_insert| instruction is synonymous of
  // |bit_instruction|.
  transformation_context->GetFactManager()->AddFactDataSynonym(
      MakeDataDescriptor(bit_insert.result_id(), {}),
      MakeDataDescriptor(bit_instruction->result_id(), {}));
}

}  // namespace fuzz
}  // namespace spvtools
