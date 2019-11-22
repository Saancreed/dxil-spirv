/*
 * Copyright 2019 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "cfg_structurizer.hpp"
#include "node.hpp"
#include "node_pool.hpp"
#include <stdio.h>
#include <string>
#include <unordered_map>
#include "spirv_module.hpp"

#include "spirv-tools/libspirv.hpp"
#include "spirv_glsl.hpp"

using namespace DXIL2SPIRV;

struct Emitter : BlockEmissionInterface
{
	void emit_basic_block(CFGNode *node) override;
	void register_block(CFGNode *node) override
	{
		if (node->id == 0)
			node->id = module.allocate_id();
	}

	SPIRVModule module;
};

void Emitter::emit_basic_block(CFGNode *node)
{
	auto &info = node->ir.merge_info;
	fprintf(stderr, "%u (%s):\n", node->id, node->name.c_str());

	// Emit opcodes here ...

	switch (info.merge_type)
	{
	case MergeType::Selection:
		fprintf(stderr, "    SelectionMerge -> %u\n", info.merge_block ? info.merge_block->id : 0);
		break;

	case MergeType::Loop:
		fprintf(stderr, "    LoopMerge -> %u, Continue <- %u\n", info.merge_block->id, info.continue_block ? info.continue_block->id : 0);
		break;

	default:
		break;
	}

#if 0
	for (auto *succ : node->succ)
		fprintf(stderr, " -> %s\n", succ->name.c_str());
	if (node->succ_back_edge)
		fprintf(stderr, " %s <- back edge\n", node->succ_back_edge->name.c_str());
#endif
}

static void print_spirv_assembly(const std::vector<uint32_t> &code)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	std::string str;
	if (!tools.Disassemble(code.data(), code.size(), &str))
		fprintf(stderr, "Failed to disassemble SPIR-V.\n");
	else
		fprintf(stderr, "\nSPIR-V:\n%s\n", str.c_str());
}

static void print_glsl(const std::vector<uint32_t> &code)
{
	try
	{
		spirv_cross::CompilerGLSL compiler(code);
		auto opts = compiler.get_common_options();
		opts.es = false;
		opts.version = 460;
		compiler.set_common_options(opts);
		auto str = compiler.compile();
		fprintf(stderr, "\n=== GLSL ===\n%s\n", str.c_str());
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "Failed to decompile to GLSL: %s.\n", e.what());
	}
}

int main()
{
	std::unordered_map<std::string, CFGNode *> block_metas;
	Emitter emitter;
	CFGNodePool pool;

	const auto get = [&](const std::string &name) -> CFGNode * {
		auto itr = block_metas.find(name);
		if (itr == block_metas.end())
		{
			auto &new_entry = block_metas[name];
			auto *node = pool.create_node();
			node->ir.terminator.type = Terminator::Type::Return;
			node->name = name;
			new_entry = node;
			return node;
		}
		else
			return itr->second;
	};

	const auto add_branch = [&](const char *from, const char *to) {
		auto *f = get(from);
		auto *t = get(to);
		f->add_branch(t);
		f->ir.terminator.type = Terminator::Type::Branch;
		f->ir.terminator.direct_block = t;
	};

	const auto add_selection = [&](const char *from, const char *to0, const char *to1) {
		auto *f = get(from);
		auto *t0 = get(to0);
		auto *t1 = get(to1);
		f->add_branch(t0);
		f->add_branch(t1);
		f->ir.terminator.type = Terminator::Type::Condition;
		f->ir.terminator.true_block = t0;
		f->ir.terminator.false_block = t1;
		f->ir.terminator.conditional_id = emitter.module.get_builder().makeBoolConstant(true, true);
		emitter.module.get_builder().addName(f->ir.terminator.conditional_id, (std::string(from) + "_sel").c_str());
	};

	const auto add_phi = [&](const char *phi, const std::vector<const char *> &from_nodes) {
		auto *p = get(phi);
		p->ir.phi.emplace_back();
		auto &phi_node = p->ir.phi.back();
		phi_node.type_id = emitter.module.get_builder().makeUintType(32);
		phi_node.id = emitter.module.allocate_id();
		emitter.module.get_builder().addName(phi_node.id, phi);

		for (auto &from : from_nodes)
		{
			IncomingValue value = {};
			value.block = get(from);
			value.id = emitter.module.get_builder().makeUintConstant(uint32_t(std::hash<std::string>()(from)));
			phi_node.incoming.push_back(value);
		}
	};

#if 0
	add_selection("entry", "b0", "exit");
	{
		add_selection("b0", "b1", "b2");
		{
			add_branch("b1", "exit");
		}
		{
			add_branch("b2", "exit");
		}
	}
#elif 0
	add_selection("entry", "l0", "exit");
	{
		add_selection("l0", "l0.true", "m0");
		{
			add_branch("l0.true", "m0");
		}
		add_branch("m0", "l1");
		add_selection("l1", "l1.true", "m1");
		{
			add_branch("l1.true", "m1");
		}
		add_branch("m1", "exit");
	}
#elif 0
	add_selection("entry", "l0", "exit");
	{
		add_selection("l0", "b0", "merge");
		{
			add_selection("b0", "c1", "b1");
			add_selection("b1", "b2", "c1.p");
			{
				add_branch("b2", "c1");
			}
			{
				//add_branch("b3", "c1.p");
			}
			add_branch("c1.p", "c1");
		}
		add_selection("c1", "l0", "merge");
		add_branch("merge", "exit");
	}
#elif 0
	add_selection("entry", "switch", "exit");
	add_switch("switch", { "case0", "case1", "default", "merge" });
	add_selection("case0", "exit", "merge");
	add_branch("case1", "merge");
	add_branch("default", "merge");
	add_branch("merge", "exit");
#elif 0
	add_selection("entry", "b0", "b1");
	{
		add_selection("b0", "b0.true", "b0.false");
		{
			add_branch("b0.true", "exit");
		}
		{
			add_branch("b0.false", "exit");
		}
		add_selection("b1", "b1.true", "b1.false");
		{
			add_branch("b1.true", "exit");
		}
		{
			add_branch("b1.false", "exit");
		}
	}
#elif 0
	add_selection("entry", "b0", "exit");
	{
		add_selection("b0", "l0", "exit");
		{
			add_selection("l0", "l1", "c0");
			{
				add_branch("l1", "l1.cond");
				add_selection("l1.cond", "exit", "c1");
				add_selection("c1", "l1", "m1");
				add_branch("m1", "c0");
			}
			add_selection("c0", "l0", "l0.exit");
			add_branch("l0.exit", "exit");
		}
	}
#elif 0
	add_selection("entry", "b0", "entry.exit");
	{
		add_selection("b0", "b1", "entry.exit");
		{
			add_selection("b1", "exit", "entry.exit");
		}
	}
	add_branch("entry.exit", "exit");
#elif 0
	add_selection("b0", "l0", "b0.exit");
	{
		add_selection("l0", "l1", "c0");
		{
			add_branch("l1", "l1.cond");
			add_selection("l1.cond", "l1.exit", "c1");
			{
				add_branch("l1.exit", "m1");
			}
			add_selection("c1", "l1", "m1");
			add_branch("m1", "c0");
		}
		add_selection("c0", "l0", "l0.exit");
		add_branch("l0.exit", "b0.exit");
	}
#elif 1
	add_branch("entry", "b0");
	add_selection("b0", "l0", "b0.exit");
	{
		add_selection("l0", "l1", "c0");
		{
			add_branch("l1", "l1.cond");
			add_selection("l1.cond", "b0.exit", "c1");
			add_selection("c1", "l1", "m1");
			add_branch("m1", "c0");
		}
		add_selection("c0", "l0", "l0.exit");
		add_branch("l0.exit", "b0.exit");
	}
#elif 1
	add_selection("a0", "b0", "exit");
	add_selection("b0", "b0.true", "b0.false");
	{
		add_selection("b0.true", "b1.true", "b1.false");
		{
			// Break out of selection construct.
			add_branch("b1.true", "exit");
		}
		{
			add_branch("b1.false", "b1.merge");
		}
		add_branch("b1.merge", "b0.merge");
	}
	{
		add_selection("b0.false", "b2.true", "b2.false");
		{
			add_branch("b2.true", "b2.merge");
		}
		{
			add_branch("b2.false", "b2.merge");
		}
		add_branch("b2.merge", "b0.merge");
	}
	add_branch("b0.merge", "exit");
#elif 0
	add_selection("b0", "body", "b0.exit");
	add_branch("body", "b0.exit");
#else
	add_selection("b0", "l0", "b0.exit");
	{
		add_selection("l0", "b1", "b0.exit");
		{
			add_selection("b1", "a", "b");
			{
				add_branch("a", "c0");
			}
			{
				add_branch("b", "c0");
			}
		}
		add_selection("c0", "l0", "l0.exit");
		add_branch("l0.exit", "b0.exit");
	}
#endif

	add_phi("b0.exit", { "b0", "l1.cond", "l0.exit" });

	CFGStructurizer traverser(get("entry"), pool, emitter.module);
	traverser.run();
	traverser.traverse(emitter);

	pool.for_each_node([](CFGNode &node) {
		node.userdata = nullptr;
		node.id = 0;
	});

	emitter.module.emit_entry_point(spv::ExecutionModelVertex, "main");
	emitter.module.emit_function_body(traverser);
	std::vector<uint32_t> code;
	emitter.module.finalize_spirv(code);

	print_spirv_assembly(code);
	print_glsl(code);
}