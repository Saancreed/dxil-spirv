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

#include "dxil_nvshader.hpp"
#include "logging.hpp"

namespace dxil_spv
{
uint32_t nvshader_num_instructions_from_opcode(NvShaderOpcode opcode)
{
	switch (opcode)
	{
	case NvShaderOpcode::NV_EXTN_OP_GET_SPECIAL:
		return 1;
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_TRACE_RAY:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_MAKE_HIT:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_MAKE_HIT_WITH_RECORD_INDEX:
		return 2;
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_MAKE_MISS:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_INVOKE:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_IS_MISS:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_INSTANCE_ID:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_INSTANCE_INDEX:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_PRIMITIVE_INDEX:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_GEOMETRY_INDEX:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_HIT_KIND:
		return 1;
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_RAY_DESC:
		return 8;
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_ATTRIBUTES:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_SHADER_TABLE_INDEX:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_LOAD_LOCAL_ROOT_TABLE_CONSTANT:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_IS_HIT:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_IS_NOP:
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_MAKE_NOP:
		return 1;
	case NvShaderOpcode::NV_EXTN_OP_RT_GET_CLUSTER_ID:
	case NvShaderOpcode::NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID:
	case NvShaderOpcode::NV_EXTN_OP_RT_GET_COMMITTED_CLUSTER_ID:
		return 1;
	case NvShaderOpcode::NV_EXTN_OP_HIT_OBJECT_GET_CLUSTER_ID:
		return 1;
	default:
		LOGE("Unsupported NvShader opcode: %u.\n", opcode);
		return 0;
	}
}
}