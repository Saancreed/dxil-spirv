/* Copyright (c) 2025 Krzysztof Bogacki
*
* SPDX-License-Identifier: MIT
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <stdint.h>
#ifdef HAVE_LLVMBC
#include "instruction.hpp"
#include "module.hpp"
#include "value.hpp"
#else
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#endif

namespace dxil_spv
{
enum NvShaderOpcode
{
	NV_EXTN_OP_NONE = 0,
	NV_EXTN_OP_GET_SPECIAL = 19,
	NV_EXTN_OP_HIT_OBJECT_TRACE_RAY = 67,
	NV_EXTN_OP_HIT_OBJECT_MAKE_HIT = 68,
	NV_EXTN_OP_HIT_OBJECT_MAKE_HIT_WITH_RECORD_INDEX = 69,
	NV_EXTN_OP_HIT_OBJECT_MAKE_MISS = 70,
	NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD = 71,
	NV_EXTN_OP_HIT_OBJECT_INVOKE = 72,
	NV_EXTN_OP_HIT_OBJECT_IS_MISS = 73,
	NV_EXTN_OP_HIT_OBJECT_GET_INSTANCE_ID = 74,
	NV_EXTN_OP_HIT_OBJECT_GET_INSTANCE_INDEX = 75,
	NV_EXTN_OP_HIT_OBJECT_GET_PRIMITIVE_INDEX = 76,
	NV_EXTN_OP_HIT_OBJECT_GET_GEOMETRY_INDEX = 77,
	NV_EXTN_OP_HIT_OBJECT_GET_HIT_KIND = 78,
	NV_EXTN_OP_HIT_OBJECT_GET_RAY_DESC = 79,
	NV_EXTN_OP_HIT_OBJECT_GET_ATTRIBUTES = 80,
	NV_EXTN_OP_HIT_OBJECT_GET_SHADER_TABLE_INDEX = 81,
	NV_EXTN_OP_HIT_OBJECT_LOAD_LOCAL_ROOT_TABLE_CONSTANT = 82,
	NV_EXTN_OP_HIT_OBJECT_IS_HIT = 83,
	NV_EXTN_OP_HIT_OBJECT_IS_NOP = 84,
	NV_EXTN_OP_HIT_OBJECT_MAKE_NOP = 85,
	NV_EXTN_OP_RT_GET_CLUSTER_ID = 93,
	NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID = 94,
	NV_EXTN_OP_RT_GET_COMMITTED_CLUSTER_ID = 95,
	NV_EXTN_OP_HIT_OBJECT_GET_CLUSTER_ID = 96,
};

enum NvShaderGetSpecialSubOpCode
{
	NV_SPECIALOP_GLOBAL_TIMER_LO = 9,
	NV_SPECIALOP_GLOBAL_TIMER_HI = 10,
};

struct NvShaderInstruction
{
	const llvm::CallInst *initiating_inst;
	const llvm::CallInst *original_inst;
	UnorderedMap<uint32_t, const llvm::Value *> inputs;
	NvShaderOpcode opcode;
	uint32_t phase;
};

struct NvShaderDeferredHitObjectInstruction
{
	NvShaderOpcode opcode;
	union
	{
		const llvm::Value *miss_shader_index;
		const llvm::Value *instance_index;
	};
	const llvm::Value *geometry_index;
	const llvm::Value *primitive_index;
	const llvm::Value *hit_kind;
	union
	{
		const llvm::Value *hit_group_record_index;
		const llvm::Value *ray_contribution_to_hit_group_index;
	};
	const llvm::Value *multiplier_for_geometry_contribution_to_hit_group_index;
	const llvm::Value *hit_object_handle;
	const llvm::Value *attributes;
};

uint32_t nvshader_num_instructions_from_opcode(NvShaderOpcode opcode);
}