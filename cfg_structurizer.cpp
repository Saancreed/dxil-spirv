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
#include "spirv_module.hpp"
#include "node_pool.hpp"
#include "node.hpp"
#include <algorithm>
#include <unordered_set>
#include <assert.h>

namespace DXIL2SPIRV
{
CFGStructurizer::CFGStructurizer(CFGNode *entry, CFGNodePool &pool_, SPIRVModule &module_)
    : entry_block(entry)
    , pool(pool_)
    , module(module_)
{
}

bool CFGStructurizer::run()
{
	recompute_cfg();

	split_merge_scopes();
	recompute_cfg();

	fprintf(stderr, "=== Structurize pass ===\n");
	structurize(0);

	recompute_cfg();

	fprintf(stderr, "=== Structurize pass ===\n");
	structurize(1);

	insert_phi();

	validate_structured();
	return true;
}

CFGNode *CFGStructurizer::get_entry_block() const
{
	return entry_block;
}

void CFGStructurizer::insert_phi()
{
	compute_dominance_frontier();

	for (auto *node : post_visit_order)
		for (auto &phi : node->ir.phi)
			phi_nodes.push_back({ node, &phi });

	// Resolve phi-nodes top-down since PHI nodes may depend on other PHI nodes.
	std::sort(phi_nodes.begin(), phi_nodes.end(), [](const PHINode &a, const PHINode &b) {
		return a.block->visit_order > b.block->visit_order;
	});

	for (auto &phi_node : phi_nodes)
		insert_phi(phi_node);
}

std::vector<IncomingValue>::const_iterator CFGStructurizer::find_incoming_value(
		const CFGNode *frontier_pred,
		const std::vector<IncomingValue> &incoming)
{
	// Find the incoming block which dominates frontier_pred and has the lowest post visit order.
	// There are cases where two or more blocks dominate, but we want the most immediate dominator.
	auto candidate = incoming.end();

	for (auto itr = incoming.begin(); itr != incoming.end(); ++itr)
	{
		if (itr->block->dominates(frontier_pred))
		{
			if (candidate == incoming.end() || itr->block->visit_order < candidate->block->visit_order)
				candidate = itr;
		}
	}

	return candidate;
}

void CFGStructurizer::insert_phi(PHINode &node)
{
	// We start off with N values defined in N blocks.
	// These N blocks *used* to branch to the PHI node, but due to our structurizer,
	// there might not be branch targets here anymore, primary example here is ladders.
	// In order to fix this we need to follow control flow from these values and insert phi nodes as necessary to link up
	// a set of values where dominance frontiers are shared.

	// First, figure out which subset of the CFG we need to work on.
	std::unordered_set<const CFGNode *> cfg_subset;
	const auto walk_op = [&](const CFGNode *n) -> bool {
		if (cfg_subset.count(n) || node.block == n)
			return false;
		else
		{
			cfg_subset.insert(n);
			return true;
		}
	};

	auto &incoming_values = node.phi->incoming;

	for (auto &incoming : incoming_values)
		incoming.block->walk_cfg_from(walk_op);

	fprintf(stderr, "\n=== CFG subset ===\n");
	for (auto *subset_node : cfg_subset)
		fprintf(stderr, "  %s\n", subset_node->name.c_str());
	fprintf(stderr, "=================\n");

	for (;;)
	{
		fprintf(stderr, "\n=== PHI iteration ===\n");
		// Advance the from blocks to get as close as we can to a dominance frontier.
		for (auto &incoming : incoming_values)
		{
			CFGNode *b = incoming.block;
			while (b->succ.size() == 1 && b->dominates(b->succ.front()))
				b = incoming.block = b->succ.front();
		}

		// We can check if all inputs are now direct branches, in this case, we can complete the PHI transformation.
		auto &preds = node.block->pred;
		bool need_phi_merge = false;
		for (auto &incoming : incoming_values)
		{
			if (std::find(preds.begin(), preds.end(), incoming.block) == preds.end())
			{
				need_phi_merge = true;
				break;
			}
		}

		if (!need_phi_merge)
		{
			fprintf(stderr, "Found PHI for %s.\n", node.block->name.c_str());
			break;
		}

		// Inside the CFG subset, find a dominance frontiers where we merge PHIs this iteration.
		CFGNode *frontier = nullptr;
		for (auto &incoming : incoming_values)
		{
			for (auto *candidate_frontier : incoming.block->dominance_frontier)
			{
				if (cfg_subset.count(candidate_frontier))
				{
					if (frontier == nullptr ||
					    candidate_frontier->visit_order > frontier->visit_order)
					{
						// Pick the earliest frontier in the CFG.
						// We want to merge top to bottom.
						frontier = candidate_frontier;
					}
				}
			}
		}

		assert(frontier);

		// A candidate dominance frontier is a place where we might want to place a PHI node in order to merge values.
		// For a successful iteration, we need to find at least one candidate where we can merge PHI.

		fprintf(stderr, "Testing dominance frontier %s ...\n", frontier->name.c_str());

		// Remove old inputs.
		for (auto *input : frontier->pred)
		{
			auto itr = find_incoming_value(input, incoming_values);
			assert(itr != incoming_values.end());

			// Do we remove the incoming value now or not?
			// If all paths from incoming value must go through frontier, we can remove it,
			// otherwise, we might still need to use the incoming value somewhere else.
			bool exists_path = itr->block->exists_path_in_cfg_without_intermediate_node(node.block, frontier);
			if (exists_path)
			{
				fprintf(stderr, "   ... keeping input in %s\n", itr->block->name.c_str());
			}
			else
			{
				fprintf(stderr, "   ... removing input in %s\n", itr->block->name.c_str());
				incoming_values.erase(itr);
			}
		}

		// We've handled this node now, remove it from consideration w.r.t. frontiers.
		cfg_subset.erase(frontier);

		// Replace with merged value.
		IncomingValue new_incoming = {};
		new_incoming.id = 0;
		new_incoming.block = frontier;
		incoming_values.push_back(new_incoming);
		fprintf(stderr, "=========================\n");
	}
}

void CFGStructurizer::compute_dominance_frontier()
{
	for (auto *node : post_visit_order)
		recompute_dominance_frontier(node);
}

void CFGStructurizer::build_immediate_dominators(CFGNode &entry)
{
	for (auto i = post_visit_order.size(); i; i--)
	{
		auto *block = post_visit_order[i - 1];
		block->recompute_immediate_dominator();
	}
}


void CFGStructurizer::reset_traversal()
{
	post_visit_order.clear();
	pool.for_each_node([](CFGNode &node) {
		node.visited = false;
		node.traversing = false;
		node.immediate_dominator = nullptr;

		if (!node.freeze_structured_analysis)
		{
			node.headers.clear();
			node.merge = MergeType::None;
			node.loop_merge_block = nullptr;
			node.loop_ladder_block = nullptr;
			node.selection_merge_block = nullptr;
		}

		if (node.succ_back_edge)
			node.succ.push_back(node.succ_back_edge);
		if (node.pred_back_edge)
			node.pred.push_back(node.pred_back_edge);
		node.succ_back_edge = nullptr;
		node.pred_back_edge = nullptr;
	});
}

void CFGStructurizer::visit(CFGNode &entry)
{
	entry.visited = true;
	entry.traversing = true;

	for (auto *succ : entry.succ)
	{
		if (succ->traversing)
		{
			// For now, only support one back edge.
			// DXIL seems to obey this.
			assert(!entry.succ_back_edge || entry.succ_back_edge == succ);
			entry.succ_back_edge = succ;

			// For now, only support one back edge.
			// DXIL seems to obey this.
			assert(!succ->pred_back_edge || succ->pred_back_edge == &entry);
			succ->pred_back_edge = &entry;
		}
		else if (!succ->visited)
			visit(*succ);
	}

	// Any back edges need to be handled specifically, only keep forward edges in succ/pred lists.
	// This avoids any infinite loop scenarios and needing to special case a lot of checks.
	if (entry.succ_back_edge)
	{
		auto itr = std::find(entry.succ.begin(), entry.succ.end(), entry.succ_back_edge);
		if (itr != entry.succ.end())
			entry.succ.erase(itr);
	}

	if (entry.pred_back_edge)
	{
		auto itr = std::find(entry.pred.begin(), entry.pred.end(), entry.pred_back_edge);
		if (itr != entry.pred.end())
			entry.pred.erase(itr);
	}

	entry.traversing = false;
	entry.visit_order = post_visit_order.size();
	post_visit_order.push_back(&entry);

	// Should be fed from frontend instead.
	entry.is_switch = entry.succ.size() > 2;
}


struct LoopBacktracer
{
	void trace_to_parent(CFGNode *header, CFGNode *block);
	std::unordered_set<CFGNode *> traced_blocks;
};

struct LoopMergeTracer
{
	explicit LoopMergeTracer(const LoopBacktracer &backtracer_)
	    : backtracer(backtracer_)
	{
	}

	void trace_from_parent(CFGNode *header);
	const LoopBacktracer &backtracer;
	std::unordered_set<CFGNode *> loop_exits;
	std::unordered_set<CFGNode *> traced_blocks;
};

void LoopBacktracer::trace_to_parent(CFGNode *header, CFGNode *block)
{
	if (block == header)
	{
		traced_blocks.insert(block);
		return;
	}

	if (traced_blocks.count(block) == 0)
	{
		traced_blocks.insert(block);
		for (auto *p : block->pred)
			trace_to_parent(header, p);
	}
}

void LoopMergeTracer::trace_from_parent(CFGNode *header)
{
	if (backtracer.traced_blocks.count(header) == 0)
	{
		loop_exits.insert(header);
		return;
	}

	for (auto *succ : header->succ)
	{
		if (traced_blocks.count(succ) == 0)
		{
			trace_from_parent(succ);
			traced_blocks.insert(succ);
		}
	}
}

void CFGStructurizer::merge_to_succ(CFGNode *node, unsigned index)
{
	node->succ[index]->headers.push_back(node);
	node->selection_merge_block = node->succ[index];
	node->merge = MergeType::Selection;
	fprintf(stderr, "Fixup selection merge %s -> %s\n", node->name.c_str(), node->selection_merge_block->name.c_str());
}

void CFGStructurizer::isolate_structured(std::unordered_set<CFGNode *> &nodes,
                                         const CFGNode *header,
                                         const CFGNode *merge)
{
	for (auto *pred : merge->pred)
	{
		if (pred != header && nodes.count(pred) == 0)
		{
			nodes.insert(pred);
			isolate_structured(nodes, header, pred);
		}
	}
}

std::vector<CFGNode *> CFGStructurizer::isolate_structured_sorted(const CFGNode *header,
                                                                  const CFGNode *merge)
{
	std::unordered_set<CFGNode *> nodes;
	isolate_structured(nodes, header, merge);

	std::vector<CFGNode *> sorted;
	sorted.reserve(nodes.size());

	for (auto *node : nodes)
		sorted.push_back(node);

	std::sort(sorted.begin(), sorted.end(), [](const CFGNode *a, const CFGNode *b) {
		return a->visit_order > b->visit_order;
	});
	return sorted;
}

bool CFGStructurizer::control_flow_is_escaping(const CFGNode *header, const CFGNode *node,
                                               const CFGNode *merge)
{
	if (node == merge)
		return false;

	// Any loop exits from continue block is not considered a break.
	if (node->succ_back_edge)
		return false;

	// If header dominates a block, which branches out to some merge block, where header does not dominate merge,
	// we have a "breaking" construct.
	for (auto *succ : node->succ)
	{
		if (succ == merge)
			return true;
		else if (header->dominates(succ))
		{
			if (control_flow_is_escaping(header, succ, merge))
				return true;
		}
	}

	return false;
}

void CFGStructurizer::fixup_broken_selection_merges(unsigned pass)
{
	// Here we deal with selection branches where one path breaks and one path merges.
	// This is common case for ladder blocks where we need to merge to the "true" merge block.
	// The selection header has two succs, but the merge block might only have one pred block,
	// which means it was not considered a merge candidate earlier in find_selection_merges().
	for (auto *node : post_visit_order)
	{
		if (node->succ.size() != 2)
			continue;
		if (node->merge != MergeType::None)
			continue;

		// A continue block will never need to merge execution, but it shouldn't have succ.size() == 2,
		// but rather succ.size() == 1 and a back edge.
		if (node->succ_back_edge)
			continue;

		bool dominates_a = node->dominates(node->succ[0]);
		bool dominates_b = node->dominates(node->succ[1]);

		bool merge_a_has_header = !node->succ[0]->headers.empty();
		bool merge_b_has_header = !node->succ[1]->headers.empty();

		if (dominates_a && !dominates_b && !merge_a_has_header)
		{
			// A is obvious candidate. B is a direct break/continue construct target most likely.
			merge_to_succ(node, 0);
		}
		else if (dominates_b && !dominates_a && !merge_b_has_header)
		{
			// B is obvious candidate. A is a direct break/continue construct target most likely.
			merge_to_succ(node, 1);
		}
		else if (dominates_a && dominates_b && !merge_a_has_header && merge_b_has_header)
		{
			// Not as obvious of a candidate, but this can happen if one path hits continue block,
			// and other path hits a ladder merge block.
			// For do/while(false) style loop, the loop body may dominate the merge block.
			merge_to_succ(node, 0);
		}
		else if (dominates_a && dominates_b && !merge_b_has_header && merge_a_has_header)
		{
			// Not as obvious of a candidate, but this can happen if one path hits continue block,
			// and other path hits a ladder merge block.
			// For do/while style loop, the loop body may dominate the merge block.
			merge_to_succ(node, 1);
		}
		else if (dominates_a && dominates_b && !merge_a_has_header && !merge_b_has_header)
		{
			// We could merge to both, no obvious merge point.
			// Figure out where execution reconvenes.
			// If we have a "break"-like construct inside a selection construct, we will not end up dominating the merge block.
			// This will be fixed up with ladder constructs later in first pass.

			// In second pass, we will have redirected any branches which escape through a ladder block.
			// If we find that one path of the selection construct must go through that ladder block, we know we have a break construct.
			CFGNode *merge = CFGStructurizer::find_common_post_dominator(node->succ);
			if (merge)
			{
				bool dominates_merge = node->dominates(merge);
				bool merges_to_continue = merge && merge->succ_back_edge;
				if (dominates_merge && !merge->headers.empty())
				{
					// Here we have a likely case where one block is doing a clean "break" out of a loop, and
					// the other path continues as normal, and then conditionally breaks in a continue block or something similar.
					bool a_path_is_break = control_flow_is_escaping(node, node->succ[0], merge);
					bool b_path_is_break = control_flow_is_escaping(node, node->succ[1], merge);
					if (a_path_is_break && b_path_is_break)
					{
						// Both paths break, so we never merge. Merge against Unreachable node if necessary ...
						node->merge = MergeType::Selection;
						node->selection_merge_block = nullptr;
						fprintf(stderr, "Merging %s -> Unreachable\n", node->name.c_str());
					}
					else if (b_path_is_break)
						merge_to_succ(node, 0);
					else
						merge_to_succ(node, 1);
				}
				else if (!merges_to_continue && (merge->headers.empty() || pass == 0))
				{
					// Happens first iteration. We'll have to split blocks, so register a merge target where we want it.
					// Otherwise, this is the easy case if we observe it in pass 1.
					// This shouldn't really happen though, as we'd normally resolve this earlier in find_selection_merges.
					assert(merge);
					node->selection_merge_block = merge;
					node->merge = MergeType::Selection;
					merge->headers.push_back(node);
					fprintf(stderr, "Merging %s -> %s\n", node->name.c_str(),
					        node->selection_merge_block->name.c_str());
				}
				else
				{
					// We don't dominate the merge block in pass 1. We cannot split blocks now.
					// Check to see which paths can actually reach the merge target without going through a ladder block.
					// If we don't go through ladder it means an outer scope will actually reach the merge node.
					// If we reach a ladder it means a block we dominate will make the escape.

					// Another case is when one path is "breaking" out to a continue block which we don't dominate.
					// We should not attempt to do ladder breaking here in pass 0 since it's unnecessary.

					bool a_path_is_break = control_flow_is_escaping(node, node->succ[0], merge);
					bool b_path_is_break = control_flow_is_escaping(node, node->succ[1], merge);
					if (a_path_is_break && b_path_is_break)
					{
						// Both paths break, so we never merge. Merge against Unreachable node if necessary ...
						node->merge = MergeType::Selection;
						auto *dummy_merge = pool.create_node();
						dummy_merge->ir.terminator.type = Terminator::Type::Unreachable;
						node->selection_merge_block = dummy_merge;
						dummy_merge->name = node->name + ".unreachable";
						fprintf(stderr, "Merging %s -> Unreachable\n", node->name.c_str());
					}
					else if (b_path_is_break)
						merge_to_succ(node, 0);
					else
						merge_to_succ(node, 1);
				}
			}
			else
			{
				// We likely had one side of the branch take an "exit", in which case there is no common post-dominator.
				bool a_dominates_exit = node->succ[0]->dominates_all_reachable_exits();
				bool b_dominates_exit = node->succ[1]->dominates_all_reachable_exits();
				if (!a_dominates_exit && b_dominates_exit)
					merge_to_succ(node, 0);
				else if (!b_dominates_exit && a_dominates_exit)
					merge_to_succ(node, 1);
				else
				{
					// Both paths lead to exit. Do we even need to merge here?
					// In worst case we can always merge to an unreachable node in the CFG.
					node->merge = MergeType::Selection;
					auto *dummy_merge = pool.create_node();
					dummy_merge->ir.terminator.type = Terminator::Type::Unreachable;
					node->selection_merge_block = dummy_merge;
					dummy_merge->name = node->name + ".unreachable";
				}
			}
		}
		else if (pass == 0)
		{
			// No possible merge target. Just need to pick whatever node is the merge block here.
			// Only do this in first pass, so that we can get a proper ladder breaking mechanism in place if we are escaping.
			CFGNode *merge = CFGStructurizer::find_common_post_dominator(node->succ);

			if (merge)
			{
				// Don't try to merge to our switch block.
				auto *inner_header = node->get_outer_header_dominator();
				bool conditional_switch_break =
						inner_header &&
						inner_header->merge == MergeType::Selection &&
						inner_header->selection_merge_block == merge;

				if (!conditional_switch_break)
				{
					node->selection_merge_block = merge;
					node->merge = MergeType::Selection;
					merge->headers.push_back(node);
					fprintf(stderr, "Merging %s -> %s\n", node->name.c_str(),
					        node->selection_merge_block->name.c_str());
				}
			}
			else
				fprintf(stderr, "Cannot find a merge target for block %s ...\n", node->name.c_str());
		}
	}
}

void CFGStructurizer::rewrite_selection_breaks(CFGNode *header, CFGNode *ladder_to)
{
	// Don't rewrite loops.
	if (header->pred_back_edge)
		return;

	// Don't rewrite switch blocks either.
	if (header->is_switch)
		return;

	std::unordered_set<CFGNode *> nodes;
	std::unordered_set<CFGNode *> construct;

	header->traverse_dominated_blocks([&](CFGNode *node) -> bool {
		if (nodes.count(node) == 0)
		{
			nodes.insert(node);
			if (node->succ.size() >= 2)
			{
				auto *outer_header = node->get_outer_selection_dominator();
				if (outer_header == header)
					construct.insert(node);
			}
			return true;
		}
		else
			return false;
	});

	for (auto *inner_block : construct)
	{
		fprintf(stderr, "Walking dominated blocks of %s, rewrite branches %s -> %s.ladder.\n",
		        inner_block->name.c_str(),
		        ladder_to->name.c_str(),
		        ladder_to->name.c_str());

		auto *ladder = pool.create_node();
		ladder->name = ladder_to->name + "." + inner_block->name + ".ladder";
		ladder->add_branch(ladder_to);
		ladder->ir.terminator.type = Terminator::Type::Branch;
		ladder->ir.terminator.direct_block = ladder_to;

		// Stop rewriting once we hit a merge block.
		inner_block->traverse_dominated_blocks_and_rewrite_branch(ladder_to, ladder, [inner_block](CFGNode *node) {
			return inner_block->selection_merge_block != node;
		});
		rewrite_selection_breaks(inner_block, ladder);
	}
}

void CFGStructurizer::split_merge_scopes()
{
	for (auto *node : post_visit_order)
	{
		// Setup a preliminary merge scope so we know when to stop traversal.
		// We don't care about traversing inner scopes, out starting from merge block as well.
		if (node->num_forward_preds() <= 1)
			continue;

		// The idom is the natural header block.
		auto *idom = node->immediate_dominator;
		assert(idom->succ.size() >= 2);

		if (idom->merge == MergeType::None)
		{
			idom->merge = MergeType::Selection;
			idom->selection_merge_block = node;
		}
		node->headers.push_back(idom);
	}

	for (auto *node : post_visit_order)
	{
		if (node->num_forward_preds() <= 1)
			continue;

		// Continue blocks can always be branched to, from any scope, so don't rewrite anything here.
		if (node->succ_back_edge)
			continue;

		// The idom is the natural header block.
		auto *idom = node->immediate_dominator;
		assert(idom->succ.size() >= 2);

		// Now we want to deal with cases where we are using this selection merge block as "goto" target for inner selection constructs.
		// Using a loop header might be possible,
		// but we will need to split up blocks to make sure that we don't end up with headers where the only branches
		// are either merges or breaks.

		// This case is relevant when we have something like:
		// A -> B -> C -> D -> M
		// A -> M
		// B -> M
		// C -> M
		// D -> M
		// We'll need intermediate blocks which merge each layer of the selection "onion".
		rewrite_selection_breaks(idom, node);
	}

	recompute_cfg();
}

void CFGStructurizer::recompute_cfg()
{
	reset_traversal();
	visit(*entry_block);
	build_immediate_dominators(*entry_block);
}

void CFGStructurizer::find_switch_blocks()
{
	for (auto index = post_visit_order.size(); index; index--)
	{
		auto *node = post_visit_order[index - 1];
		if (!node->is_switch)
			continue;

		auto *merge = find_common_post_dominator(node->succ);
		if (node->dominates(merge))
		{
			fprintf(stderr, "Switch merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(node),
			        node->name.c_str(), static_cast<const void *>(merge), merge->name.c_str());
			node->merge = MergeType::Selection;
			node->selection_merge_block = merge;
			merge->add_unique_header(node);
		}
		else
		{
			// We got a switch block where someone is escaping. Similar idea as for loop analysis.
			// Find a post-dominator where we ignore branches which are "escaping".
			auto *dominated_merge_target = find_common_post_dominator_with_ignored_break(node->succ, merge);
			if (node->dominates(dominated_merge_target))
			{
				node->merge = MergeType::Selection;
				node->selection_merge_block = merge;
				dominated_merge_target->add_unique_header(node);
				merge->add_unique_header(node);
			}
		}
	}
}

void CFGStructurizer::find_selection_merges(unsigned pass)
{
	for (auto *node : post_visit_order)
	{
		if (node->num_forward_preds() <= 1)
			continue;

		// If there are 2 or more pred edges, try to merge execution.

		// The idom is the natural header block.
		auto *idom = node->immediate_dominator;
		assert(idom->succ.size() >= 2);

		// Check for case fallthrough here. In this case, we do not have a merge scenario, just ignore.
		auto *inner_header = node->get_outer_selection_dominator();
		if (inner_header && inner_header->is_switch)
		{
			if (inner_header->selection_merge_block == node)
			{
				// We just found a switch block which we have already handled.
				continue;
			}

			if (std::find(inner_header->succ.begin(), inner_header->succ.end(), node) != inner_header->succ.end())
			{
				// Fallthrough.
				continue;
			}
		}

		for (auto *header : node->headers)
		{
			// If we have a loop header already associated with this block, treat that as our idom.
			if (header->visit_order > idom->visit_order)
				idom = header;
		}

		if (idom->merge == MergeType::None || idom->merge == MergeType::Selection)
		{
			// We just found a switch block which we have already handled.
			if (idom->is_switch)
				continue;

			// If the idom is already a selection construct, this must mean
			// we have some form of breaking construct inside this inner construct.
			// This fooled find_selection_merges() to think we had a selection merge target at the break target.
			// Fix this up here, where we rewrite the outer construct as a fixed loop instead.
			if (idom->merge == MergeType::Selection)
			{
				if (pass == 0)
				{
					idom->merge = MergeType::Loop;
					assert(idom->selection_merge_block);
					idom->loop_merge_block = idom->selection_merge_block;
					idom->selection_merge_block = nullptr;
					idom->freeze_structured_analysis = true;
					idom = create_helper_succ_block(idom);
				}
				else
					fprintf(stderr, "Mismatch headers in pass 1 ... ?\n");
			}

			idom->merge = MergeType::Selection;
			node->add_unique_header(idom);
			assert(node);
			idom->selection_merge_block = node;
			fprintf(stderr, "Selection merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(idom),
			        idom->name.c_str(), static_cast<const void *>(node), node->name.c_str());
		}
		else if (idom->merge == MergeType::Loop)
		{
			if (idom->loop_merge_block == node && idom->loop_ladder_block)
			{
				// We need to create an outer shell for this header since we need to ladder break to this node.
				auto *loop = create_helper_pred_block(idom);
				loop->merge = MergeType::Loop;
				loop->loop_merge_block = node;
				loop->freeze_structured_analysis = true;
				node->add_unique_header(loop);
				fprintf(stderr, "Loop merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(loop),
						loop->name.c_str(), static_cast<const void *>(node), node->name.c_str());
			}
			else if (idom->loop_merge_block != node)
			{
				auto *selection_idom = create_helper_succ_block(idom);
				// If we split the loop header into the loop header -> selection merge header,
				// then we can merge into a continue block for example.
				selection_idom->merge = MergeType::Selection;
				idom->selection_merge_block = node;
				node->add_unique_header(idom);
				fprintf(stderr, "Selection merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(selection_idom),
				        selection_idom->name.c_str(), static_cast<const void *>(node), node->name.c_str());
			}
		}
		else
		{
			// We are hosed. There is no obvious way to merge execution here.
			// This might be okay.
			fprintf(stderr, "Cannot merge execution for node %p (%s).\n", static_cast<const void *>(node),
			        node->name.c_str());
		}
	}
}

CFGStructurizer::LoopExitType CFGStructurizer::get_loop_exit_type(const CFGNode &header, const CFGNode &node) const
{
	// If there exists an inner loop which dominates this exit, we treat it as an inner loop exit.
	bool is_innermost_loop_header = header.is_innermost_loop_header_for(&node);
	if (header.dominates(&node) && node.dominates_all_reachable_exits())
	{
		if (is_innermost_loop_header)
			return LoopExitType::Exit;
		else
			return LoopExitType::InnerLoopExit;
	}

	if (header.dominates(&node))
	{
		if (is_innermost_loop_header)
		{
			// Even if we dominate node, we might not be able to merge to it.
			if (!header.can_loop_merge_to(&node))
				return LoopExitType::Escape;

			return LoopExitType::Merge;
		}
		else
			return LoopExitType::InnerLoopMerge;
	}
	else
		return LoopExitType::Escape;
}

CFGNode *CFGStructurizer::create_helper_pred_block(CFGNode *node)
{
	auto *pred_node = pool.create_node();
	pred_node->name = node->name + ".pred";

	// Fixup visit order later.
	pred_node->visit_order = node->visit_order;

	std::swap(pred_node->pred, node->pred);

	pred_node->immediate_dominator = node->immediate_dominator;
	node->immediate_dominator = pred_node;

	pred_node->retarget_pred_from(node);

	pred_node->add_branch(node);

	if (node == entry_block)
		entry_block = pred_node;

	pred_node->ir.terminator.type = Terminator::Type::Branch;
	pred_node->ir.terminator.direct_block = node;

	return pred_node;
}

CFGNode *CFGStructurizer::create_helper_succ_block(CFGNode *node)
{
	auto *succ_node = pool.create_node();
	succ_node->name = node->name + ".succ";

	// Fixup visit order later.
	succ_node->visit_order = node->visit_order;

	std::swap(succ_node->succ, node->succ);
	// Do not swap back edges, only forward edges.

	succ_node->ir.terminator = node->ir.terminator;
	node->ir.terminator.type = Terminator::Type::Branch;
	node->ir.terminator.direct_block = succ_node;

	succ_node->retarget_succ_from(node);
	succ_node->immediate_dominator = node;

	node->add_branch(succ_node);
	return succ_node;
}

#if 0
CFGNode *CFGStructurizer::find_common_dominated_merge_block(CFGNode *header)
{
	auto candidates = header->succ;
	std::vector<CFGNode *> next_nodes;

	const auto add_unique_next_node = [&](CFGNode *node) {
		if (header->dominates(node))
		{
			if (std::find(next_nodes.begin(), next_nodes.end(), node) == next_nodes.end())
				next_nodes.push_back(node);
		}
	};

	while (candidates.size() > 1)
	{
		// Sort candidates by post visit order.
		std::sort(candidates.begin(), candidates.end(), [](const CFGNode *a, const CFGNode *b) {
			return a->visit_order > b->visit_order;
		});

		// Now we look at the lowest post-visit order.
		// Before we traverse further, we need to make sure that all other blocks will actually reconvene with us somewhere.

		for (auto *succ : candidates.front()->succ)
			add_unique_next_node(succ);
		for (auto itr = candidates.begin() + 1; itr != candidates.end(); ++itr)
			add_unique_next_node(*itr);

		candidates.clear();
		std::swap(candidates, next_nodes);
	}

	return candidates.empty() ? nullptr : candidates.front();
}
#endif

CFGNode *CFGStructurizer::find_common_post_dominator(std::vector<CFGNode *> candidates)
{
	return find_common_post_dominator_with_ignored_break(std::move(candidates), nullptr);
}

CFGNode *CFGStructurizer::find_common_post_dominator_with_ignored_exits(const CFGNode *header)
{
	std::vector<CFGNode *> candidates;
	std::vector<CFGNode *> next_nodes;
	const auto add_unique_next_node = [&](CFGNode *node) {
		if (std::find(next_nodes.begin(), next_nodes.end(), node) == next_nodes.end())
			next_nodes.push_back(node);
	};

	// Ignore any exit paths.
	for (auto *succ : header->succ)
		if (!succ->dominates_all_reachable_exits())
			add_unique_next_node(succ);
	std::swap(next_nodes, candidates);

	while (candidates.size() != 1)
	{
		// Sort candidates by post visit order.
		std::sort(candidates.begin(), candidates.end(),
		          [](const CFGNode *a, const CFGNode *b) { return a->visit_order > b->visit_order; });

		for (auto *succ : candidates.front()->succ)
			add_unique_next_node(succ);
		for (auto itr = candidates.begin() + 1; itr != candidates.end(); ++itr)
			add_unique_next_node(*itr);

		candidates.clear();
		std::swap(candidates, next_nodes);
	}

	if (candidates.empty())
		return nullptr;
	else
		return candidates.front();
}

CFGNode *CFGStructurizer::find_common_post_dominator_with_ignored_break(std::vector<CFGNode *> candidates, const CFGNode *ignored_node)
{
	if (candidates.empty())
		return nullptr;

	std::vector<CFGNode *> next_nodes;
	const auto add_unique_next_node = [&](CFGNode *node) {
		if (node != ignored_node)
			if (std::find(next_nodes.begin(), next_nodes.end(), node) == next_nodes.end())
				next_nodes.push_back(node);
	};

	while (candidates.size() != 1)
	{
		// Sort candidates by post visit order.
		std::sort(candidates.begin(), candidates.end(),
		          [](const CFGNode *a, const CFGNode *b) { return a->visit_order > b->visit_order; });

		// We reached exit without merging execution, there is no common post dominator.
		if (candidates.front()->succ.empty())
			return nullptr;

		for (auto *succ : candidates.front()->succ)
			add_unique_next_node(succ);
		for (auto itr = candidates.begin() + 1; itr != candidates.end(); ++itr)
			add_unique_next_node(*itr);

		candidates.clear();
		std::swap(candidates, next_nodes);
	}

	if (candidates.empty())
		return nullptr;

	return candidates.front();
}

void CFGStructurizer::find_loops()
{
	for (auto index = post_visit_order.size(); index; index--)
	{
		// Visit in reverse order so we resolve outer loops first,
		// this lets us detect ladder-breaking loops.
		auto *node = post_visit_order[index - 1];

		if (node->freeze_structured_analysis)
		{
			// If we have a pre-created dummy loop for ladding breaking,
			// just propagate the header information and be done with it.
			if (node->merge == MergeType::Loop)
			{
				node->loop_merge_block->headers.push_back(node);
				continue;
			}
		}

		if (!node->has_pred_back_edges())
			continue;

		// There are back-edges here, this must be a loop header.
		node->merge = MergeType::Loop;

		// Now, we need to figure out which blocks belong in the loop construct.
		// The way to figure out a natural loop is any block which is dominated by loop header
		// and control flow passes to one of the back edges.

		// Unfortunately, it can be ambiguous which block is the merge block for a loop.
		// Ideally, there is a unique block which is the loop exit block, but if there are multiple breaks
		// there are multiple blocks which are not part of the loop construct.

		LoopBacktracer tracer;
		auto *pred = node->pred_back_edge;

		// Back-trace from here.
		// The CFG is reducible, so node must dominate pred.
		// Since node dominates pred, there is no pred chain we can follow without
		// eventually hitting node, and we'll stop traversal there.

		// All nodes which are touched during this traversal must be part of the loop construct.
		tracer.trace_to_parent(node, pred);

		LoopMergeTracer merge_tracer(tracer);
		merge_tracer.trace_from_parent(node);

		std::vector<CFGNode *> direct_exits;
		std::vector<CFGNode *> dominated_exit;
		std::vector<CFGNode *> inner_dominated_exit;
		std::vector<CFGNode *> non_dominated_exit;

		for (auto *loop_exit : merge_tracer.loop_exits)
		{
			auto exit_type = get_loop_exit_type(*node, *loop_exit);
			switch (exit_type)
			{
			case LoopExitType::Exit:
				direct_exits.push_back(loop_exit);
				break;

			case LoopExitType::InnerLoopExit:
				// It's not an exit for us, but the inner loop.
				break;

			case LoopExitType::Merge:
				dominated_exit.push_back(loop_exit);
				break;

			case LoopExitType::InnerLoopMerge:
				inner_dominated_exit.push_back(loop_exit);
				break;

			case LoopExitType::Escape:
				non_dominated_exit.push_back(loop_exit);
				break;
			}
		}

		// If we only have one direct exit, consider it our merge block.
		// Pick either Merge or Escape.
		if (direct_exits.size() == 1 && dominated_exit.empty() && non_dominated_exit.empty())
		{
			if (node->dominates(direct_exits.front()))
				std::swap(dominated_exit, direct_exits);
			else
				std::swap(non_dominated_exit, direct_exits);
		}

		if (dominated_exit.size() >= 2)
		{
			// Try to see if we can reduce the number of merge blocks to just 1.
			// This is relevant if we have various "clean" break blocks.
			auto *post_dominator = find_common_post_dominator(dominated_exit);
			if (std::find(dominated_exit.begin(), dominated_exit.end(), post_dominator) != dominated_exit.end())
			{
				dominated_exit.clear();
				dominated_exit.push_back(post_dominator);
			}
		}

		if (dominated_exit.empty() && non_dominated_exit.empty())
		{
			// There can be zero loop exits, i.e. infinite loop. This means we have no merge block.
			// We will invent a merge block to satisfy SPIR-V validator, and declare it as unreachable.
			node->loop_merge_block = nullptr;
			fprintf(stderr, "Loop without merge: %p (%s)\n", static_cast<const void *>(node), node->name.c_str());
		}
		else if (dominated_exit.size() == 1 && non_dominated_exit.empty() && inner_dominated_exit.empty())
		{
			// Clean merge.
			// This is a unique merge block. There can be no other merge candidate.
			node->loop_merge_block = dominated_exit.front();

			const_cast<CFGNode *>(node->loop_merge_block)->add_unique_header(node);
			fprintf(stderr, "Loop with simple merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(node),
			        node->name.c_str(), static_cast<const void *>(node->loop_merge_block),
			        node->loop_merge_block->name.c_str());
		}
		else if (dominated_exit.empty() && inner_dominated_exit.empty() && non_dominated_exit.size() == 1)
		{
			// Single-escape merge.
			// It is unique, but we need workarounds later.
			node->loop_merge_block = non_dominated_exit.front();

			const_cast<CFGNode *>(node->loop_merge_block)->add_unique_header(node);
			fprintf(stderr, "Loop with ladder merge: %p (%s) -> %p (%s)\n", static_cast<const void *>(node),
			        node->name.c_str(), static_cast<const void *>(node->loop_merge_block),
			        node->loop_merge_block->name.c_str());
		}
		else
		{
			// We have multiple blocks which are merge candidates. We need to figure out where execution reconvenes.
			std::vector<CFGNode *> merges;
			merges.reserve(inner_dominated_exit.size() + dominated_exit.size() + non_dominated_exit.size());
			merges.insert(merges.end(), inner_dominated_exit.begin(), inner_dominated_exit.end());
			merges.insert(merges.end(), dominated_exit.begin(), dominated_exit.end());
			merges.insert(merges.end(), non_dominated_exit.begin(), non_dominated_exit.end());
			CFGNode *merge = CFGStructurizer::find_common_post_dominator(std::move(merges));

			CFGNode *dominated_merge = nullptr;
			if (dominated_exit.size() > 1)
			{
				// Now, we might have Merge blocks which end up escaping out of the loop construct.
				// We might have to remove candidates which end up being break blocks after all.
				std::vector<CFGNode *> non_breaking_exits;
				non_breaking_exits.reserve(dominated_exit.size());
				for (auto *exit : dominated_exit)
					if (!control_flow_is_escaping(node, exit, merge))
						non_breaking_exits.push_back(exit);

				dominated_merge = CFGStructurizer::find_common_post_dominator(std::move(non_breaking_exits));
			}
			else
			{
				dominated_merge = CFGStructurizer::find_common_post_dominator(std::move(dominated_exit));
			}

			if (!dominated_merge)
			{
				fprintf(stderr, "There is no candidate for ladder merging.\n");
			}

			if (dominated_merge && !node->dominates(dominated_merge))
			{
				fprintf(stderr, "We don't dominate the merge target ...\n");
				dominated_merge = nullptr;
			}

			if (!merge)
			{
				fprintf(stderr, "Failed to find a common merge point ...\n");
			}
			else
			{
				node->loop_merge_block = merge;
				const_cast<CFGNode *>(node->loop_merge_block)->add_unique_header(node);

				if (node->can_loop_merge_to(merge))
				{
					// Clean merge.
					// This is a unique merge block. There can be no other merge candidate.
					fprintf(stderr, "Loop with simple multi-exit merge: %p (%s) -> %p (%s)\n",
					        static_cast<const void *>(node), node->name.c_str(),
					        static_cast<const void *>(node->loop_merge_block), node->loop_merge_block->name.c_str());
				}
				else
				{
					// Single-escape merge.
					// It is unique, but we need workarounds later.
					fprintf(stderr, "Loop with ladder multi-exit merge: %p (%s) -> %p (%s)\n",
					        static_cast<const void *>(node), node->name.c_str(),
					        static_cast<const void *>(node->loop_merge_block), node->loop_merge_block->name.c_str());

					if (dominated_merge)
					{
						fprintf(stderr, "    Ladder block: %p (%s)\n", static_cast<const void *>(dominated_merge),
						        dominated_merge->name.c_str());
					}

					// We will use this block as a ladder.
					node->loop_ladder_block = dominated_merge;
				}
			}
		}
	}
}

void CFGStructurizer::split_merge_blocks()
{
	for (auto *node : post_visit_order)
	{
		if (node->headers.size() <= 1)
			continue;

		// If this block was the merge target for more than one construct,
		// we will need to split the block. In SPIR-V, a merge block can only be the merge target for one construct.
		// However, we can set up a chain of merges where inner scope breaks to outer scope with a dummy basic block.
		// The outer scope comes before the inner scope merge.
		std::sort(node->headers.begin(), node->headers.end(),
		          [](const CFGNode *a, const CFGNode *b) -> bool { return a->dominates(b); });

		// Verify that scopes are actually nested.
		// This means header[N] must dominate header[M] where N > M.
		for (size_t i = 1; i < node->headers.size(); i++)
		{
			if (!node->headers[i - 1]->dominates(node->headers[i]))
				fprintf(stderr, "Scopes are not nested.\n");
		}

		if (node->headers[0]->loop_ladder_block)
		{
			fprintf(stderr, "Outer loop header needs ladder break.\n");
		}

		CFGNode *full_break_target = nullptr;

		// Start from innermost scope, and rewrite all escape branches to a merge block which is dominated by the loop header in question.
		// The merge block for the loop must have a ladder block before the old merge block.
		// This ladder block will break to outer scope, or keep executing the old merge block.
		for (size_t i = node->headers.size() - 1; i; i--)
		{
			// Find innermost loop header scope we can break to when resolving ladders.
			CFGNode *target_header = nullptr;
			for (size_t j = i; j; j--)
			{
				if (node->headers[j - 1]->merge == MergeType::Loop)
				{
					target_header = node->headers[j - 1];
					break;
				}
			}

			if (node->headers[i]->merge == MergeType::Loop)
			{
				auto *loop_ladder = node->headers[i]->loop_ladder_block;
				if (loop_ladder)
				{
					if (target_header)
					{
						// If we have a ladder block, there exists a merge candidate which the loop header dominates.
						// We create a ladder block before the merge block, which becomes the true merge block.
						// In this ladder block, we can detect with Phi nodes whether the break was "clean",
						// or if we had an escape edge.
						// If we have an escape edge, we can break to outer level, and continue the ladder that way.
						// Otherwise we branch to the existing merge block and continue as normal.
						// We'll also need to rewrite a lot of Phi nodes this way as well.
						auto *ladder = create_helper_pred_block(loop_ladder);
						ladder->is_ladder = true;

						std::unordered_set<const CFGNode *> normal_preds;
						for (auto *pred : ladder->pred)
							if (!pred->is_ladder)
								normal_preds.insert(pred);

						// Merge to ladder instead.
						node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(node, ladder);

						ladder->ir.terminator.type = Terminator::Type::Condition;
						ladder->ir.terminator.conditional_id = module.allocate_id();
						ladder->ir.terminator.false_block = loop_ladder;

						PHI phi;
						phi.id = ladder->ir.terminator.conditional_id;
						phi.type_id = module.get_builder().makeBoolType();
						for (auto *pred : ladder->pred)
						{
							IncomingValue incoming = {};
							incoming.block = pred;
							incoming.id = module.get_builder().makeBoolConstant(!normal_preds.count(pred));
							phi.incoming.push_back(incoming);
						}
						ladder->ir.phi.push_back(std::move(phi));

						// Ladder breaks out to outer scope.
						if (target_header->loop_ladder_block)
						{
							ladder->ir.terminator.true_block = target_header->loop_ladder_block;
							ladder->add_branch(target_header->loop_ladder_block);
						}
						else if (target_header->loop_merge_block)
						{
							ladder->ir.terminator.true_block = target_header->loop_merge_block;
							ladder->add_branch(target_header->loop_merge_block);
						}
						else
							fprintf(stderr, "No loop merge block?\n");
					}
					else if (loop_ladder->succ.size() == 1 && loop_ladder->succ.front() == node)
					{
						// We have a case where we're trivially breaking out of a selection construct.
						// We cannot directly break out of a selection construct, so our ladder must be a bit more sophisticated.
						// ladder-pre -> merge -> ladder-post -> selection merge
						//      \-------------------/
						auto *ladder_pre = create_helper_pred_block(loop_ladder);
						auto *ladder_post = create_helper_succ_block(loop_ladder);
						ladder_pre->add_branch(ladder_post);

						ladder_pre->ir.terminator.type = Terminator::Type::Condition;
						ladder_pre->ir.terminator.conditional_id = module.allocate_id();
						ladder_pre->ir.terminator.true_block = ladder_post;
						ladder_pre->ir.terminator.false_block = loop_ladder;

						ladder_pre->is_ladder = true;
						PHI phi;
						phi.id = ladder_pre->ir.terminator.conditional_id;
						phi.type_id = module.get_builder().makeBoolType();
						for (auto *pred : ladder_pre->pred)
						{
							IncomingValue incoming = {};
							incoming.block = pred;
							incoming.id = module.get_builder().makeBoolConstant(pred->is_ladder);
							phi.incoming.push_back(incoming);
						}
						ladder_pre->ir.phi.push_back(std::move(phi));
					}
					else if (full_break_target)
					{
						node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(node, full_break_target);
					}
					else
					{
						// Selection merge to this dummy instead.
						auto *new_selection_merge = create_helper_pred_block(node);

						// Inherit the headers.
						new_selection_merge->headers = node->headers;

						// This is now our fallback loop break target.
						full_break_target = node;

						auto *loop = create_helper_pred_block(node->headers[0]);

						// Reassign header node.
						assert(node->headers[0]->merge == MergeType::Selection);
						node->headers[0]->selection_merge_block = new_selection_merge;
						node->headers[0] = loop;

						loop->merge = MergeType::Loop;
						loop->loop_merge_block = node;
						loop->freeze_structured_analysis = true;

						node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(new_selection_merge, node);
						node = new_selection_merge;
					}
				}
				else
					fprintf(stderr, "No loop ladder candidate.\n");
			}
			else if (node->headers[i]->merge == MergeType::Selection)
			{
				if (target_header)
				{
					// Breaks out to outer available scope.
					if (target_header->loop_ladder_block)
						node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(node, target_header->loop_ladder_block);
					else if (target_header->loop_merge_block)
						node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(node, target_header->loop_merge_block);
					else
						fprintf(stderr, "No loop merge block?\n");
				}
				else if (full_break_target)
				{
					node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(node, full_break_target);
				}
				else
				{
					// Selection merge to this dummy instead.
					auto *new_selection_merge = create_helper_pred_block(node);

					// Inherit the headers.
					new_selection_merge->headers = node->headers;

					// This is now our fallback loop break target.
					full_break_target = node;

					auto *loop = create_helper_pred_block(node->headers[0]);

					// Reassign header node.
					assert(node->headers[0]->merge == MergeType::Selection);
					node->headers[0]->selection_merge_block = new_selection_merge;
					node->headers[0] = loop;

					loop->merge = MergeType::Loop;
					loop->loop_merge_block = node;
					loop->freeze_structured_analysis = true;

					node->headers[i]->traverse_dominated_blocks_and_rewrite_branch(new_selection_merge, node);
					node = new_selection_merge;
				}
			}
			else
				fprintf(stderr, "Invalid merge type.\n");
		}
	}
}

void CFGStructurizer::structurize(unsigned pass)
{
	find_loops();
	find_switch_blocks();
	find_selection_merges(pass);
	fixup_broken_selection_merges(pass);
	if (pass == 0)
		split_merge_blocks();
}

void CFGStructurizer::recompute_dominance_frontier(CFGNode *node)
{
	std::unordered_set<const CFGNode *> traversed;
	node->dominance_frontier.clear();
	recompute_dominance_frontier(node, node, traversed);
}

void CFGStructurizer::recompute_dominance_frontier(CFGNode *header, const CFGNode *node,
                                                   std::unordered_set<const CFGNode *> traversed)
{
	// Not very efficient, but it'll do for now ...
	if (traversed.count(node))
		return;
	traversed.insert(node);

	for (auto *succ : node->succ)
	{
		if (header->dominates(succ))
			recompute_dominance_frontier(header, succ, traversed);
		else
		{
			auto &frontier = header->dominance_frontier;
			if (std::find(frontier.begin(), frontier.end(), succ) == frontier.end())
				header->dominance_frontier.push_back(succ);
		}
	}
}

void CFGStructurizer::validate_structured()
{
	for (auto *node : post_visit_order)
	{
		if (node->headers.size() > 1)
		{
			fprintf(stderr, "Node %s has %u headers!\n", node->name.c_str(), unsigned(node->headers.size()));
			return;
		}

		if (node->merge == MergeType::Loop)
		{
			if (!node->dominates(node->loop_merge_block) && !node->loop_merge_block->pred.empty())
			{
				fprintf(stderr, "Node %s does not dominate its merge block %s!\n", node->name.c_str(),
				        node->loop_merge_block->name.c_str());
				return;
			}
		}
		else if (node->merge == MergeType::Selection)
		{
			if (!node->selection_merge_block)
				fprintf(stderr, "No selection merge block for %s\n", node->name.c_str());
			else if (!node->dominates(node->selection_merge_block) && !node->selection_merge_block->pred.empty())
			{
				fprintf(stderr, "Node %s does not dominate its selection merge block %s!\n", node->name.c_str(),
				        node->selection_merge_block->name.c_str());
				return;
			}
		}

		if (node->succ.size() >= 2 && node->merge == MergeType::None)
		{
			// This might not be critical.
			fprintf(stderr, "Node %s has %u successors, but no merge header.\n", node->name.c_str(),
			        unsigned(node->succ.size()));
		}
	}
	fprintf(stderr, "Successful CFG validation!\n");
}

void CFGStructurizer::traverse(BlockEmissionInterface &iface)
{
	// Make sure all blocks are known to the backend before we emit code.
	for (auto *block : post_visit_order)
		iface.register_block(block);

	// Need to emit blocks such that dominating blocks come before dominated blocks.
	for (auto index = post_visit_order.size(); index; index--)
	{
		auto *block = post_visit_order[index - 1];

		auto &merge = block->ir.merge_info;

		switch (block->merge)
		{
		case MergeType::Selection:
			merge.merge_block = block->selection_merge_block;
			if (merge.merge_block)
				iface.register_block(merge.merge_block);
			merge.merge_type = block->merge;
			iface.emit_basic_block(block);
			break;

		case MergeType::Loop:
			merge.merge_block = block->loop_merge_block;
			merge.merge_type = block->merge;
			merge.continue_block = block->pred_back_edge;
			if (merge.merge_block)
				iface.register_block(merge.merge_block);
			if (merge.continue_block)
				iface.register_block(merge.continue_block);

			iface.emit_basic_block(block);
			break;

		default:
			iface.emit_basic_block(block);
			break;
		}
	}
}
} // namespace DXIL2SPIRV