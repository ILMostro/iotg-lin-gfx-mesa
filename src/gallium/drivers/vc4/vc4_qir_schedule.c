/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014-2015 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * @file vc4_qir_schedule.c
 *
 * The basic model of the list scheduler is to take a basic block, compute a
 * DAG of the dependencies from the bottom up, and make a list of the DAG
 * heads.  Heuristically pick a DAG head and schedule (remove) it, then put
 * all the parents that are now DAG heads into the list of things to
 * schedule.
 *
 * The goal of scheduling here, before register allocation and conversion to
 * QPU instructions, is to reduce register pressure by reordering instructions
 * to consume values when possible.
 */

#include "vc4_qir.h"

static bool debug;

struct schedule_node {
        struct list_head link;
        struct qinst *inst;

        struct schedule_node **children;
        uint32_t child_count;
        uint32_t child_array_size;
        uint32_t parent_count;

        /* Length of the longest (latency) chain from a DAG head to the this
         * instruction.
         */
        uint32_t delay;

        /* Longest time + latency_between(parent, this) of any parent of this
         * node.
         */
        uint32_t unblocked_time;
};

struct schedule_state {
        /* List of struct schedule_node *.  This starts out with all
         * instructions, and after dependency updates it's trimmed to be just
         * the DAG heads.
         */
        struct list_head worklist;

        uint32_t time;

        uint32_t *temp_writes;

        BITSET_WORD *temp_live;
};

/* When walking the instructions in reverse, we need to swap before/after in
 * add_dep().
 */
enum direction { F, R };

/**
 * Marks a dependency between two intructions, that @after must appear after
 * @before.
 *
 * Our dependencies are tracked as a DAG.  Since we're scheduling bottom-up,
 * the latest instructions with nothing left to schedule are the DAG heads,
 * and their inputs are their children.
 */
static void
add_dep(enum direction dir,
        struct schedule_node *before,
        struct schedule_node *after)
{
        if (!before || !after)
                return;

        assert(before != after);

        if (dir == R) {
                struct schedule_node *t = before;
                before = after;
                after = t;
        }

        for (int i = 0; i < after->child_count; i++) {
                if (after->children[i] == after)
                        return;
        }

        if (after->child_array_size <= after->child_count) {
                after->child_array_size = MAX2(after->child_array_size * 2, 16);
                after->children = reralloc(after, after->children,
                                           struct schedule_node *,
                                           after->child_array_size);
        }

        after->children[after->child_count] = before;
        after->child_count++;
        before->parent_count++;
}

static void
add_write_dep(enum direction dir,
              struct schedule_node **before,
              struct schedule_node *after)
{
        add_dep(dir, *before, after);
        *before = after;
}

struct schedule_setup_state {
        struct schedule_node **last_temp_write;
        struct schedule_node *last_sf;
        struct schedule_node *last_vary_read;
        struct schedule_node *last_vpm_read;
        struct schedule_node *last_vpm_write;
        struct schedule_node *last_tex_coord;
        struct schedule_node *last_tex_result;
        struct schedule_node *last_tlb;
        enum direction dir;

	/**
         * Texture FIFO tracking.  This is done top-to-bottom, and is used to
         * track the QOP_TEX_RESULTs and add dependencies on previous ones
         * when trying to submit texture coords with TFREQ full or new texture
         * fetches with TXRCV full.
         */
        struct {
                struct schedule_node *node;
                int coords;
        } tex_fifo[8];
        int tfreq_count; /**< Number of texture coords outstanding. */
        int tfrcv_count; /**< Number of texture results outstanding. */
        int tex_fifo_pos;
};

static void
block_until_tex_result(struct schedule_setup_state *state, struct schedule_node *n)
{
        add_dep(state->dir, state->tex_fifo[0].node, n);

        state->tfreq_count -= state->tex_fifo[0].coords;
        state->tfrcv_count--;

        memmove(&state->tex_fifo[0],
                &state->tex_fifo[1],
                state->tex_fifo_pos * sizeof(state->tex_fifo[0]));
        state->tex_fifo_pos--;
}

/**
 * Common code for dependencies that need to be tracked both forward and
 * backward.
 *
 * This is for things like "all VPM reads have to happen in order."
 */
static void
calculate_deps(struct schedule_setup_state *state, struct schedule_node *n)
{
        struct qinst *inst = n->inst;
        enum direction dir = state->dir;


        /* Add deps for temp registers and varyings accesses.  Note that we
         * ignore uniforms accesses, because qir_reorder_uniforms() happens
         * after this.
         */
        for (int i = 0; i < qir_get_op_nsrc(inst->op); i++) {
                switch (inst->src[i].file) {
                case QFILE_TEMP:
                        add_dep(dir,
                                state->last_temp_write[inst->src[i].index], n);
                        break;

                case QFILE_VARY:
                        add_write_dep(dir, &state->last_vary_read, n);
                        break;

                case QFILE_VPM:
                        add_write_dep(dir, &state->last_vpm_read, n);
                        break;

                default:
                        break;
                }
        }

        switch (inst->op) {
        case QOP_VARY_ADD_C:
                add_dep(dir, state->last_vary_read, n);
                break;

        case QOP_TEX_S:
        case QOP_TEX_T:
        case QOP_TEX_R:
        case QOP_TEX_B:
        case QOP_TEX_DIRECT:
                /* Texturing setup gets scheduled in order, because
                 * the uniforms referenced by them have to land in a
                 * specific order.
                 */
                add_write_dep(dir, &state->last_tex_coord, n);
                break;

        case QOP_TEX_RESULT:
                /* Results have to be fetched in order. */
                add_write_dep(dir, &state->last_tex_result, n);
                break;

        case QOP_TLB_COLOR_WRITE:
        case QOP_TLB_COLOR_READ:
        case QOP_TLB_Z_WRITE:
        case QOP_TLB_STENCIL_SETUP:
        case QOP_MS_MASK:
                add_write_dep(dir, &state->last_tlb, n);
                break;

        case QOP_TLB_DISCARD_SETUP:
                add_write_dep(dir, &state->last_sf, n);
                add_write_dep(dir, &state->last_tlb, n);
                break;

        default:
                break;
        }

        if (inst->dst.file == QFILE_VPM)
                add_write_dep(dir, &state->last_vpm_write, n);
        else if (inst->dst.file == QFILE_TEMP)
                add_write_dep(dir, &state->last_temp_write[inst->dst.index], n);

        if (qir_depends_on_flags(inst))
                add_dep(dir, state->last_sf, n);

        if (inst->sf)
                add_write_dep(dir, &state->last_sf, n);
}

static void
calculate_forward_deps(struct vc4_compile *c, void *mem_ctx,
                       struct list_head *schedule_list)
{
        struct schedule_setup_state state;

        memset(&state, 0, sizeof(state));
        state.last_temp_write = rzalloc_array(mem_ctx, struct schedule_node *,
                                              c->num_temps);
        state.dir = F;

        list_for_each_entry(struct schedule_node, n, schedule_list, link) {
                struct qinst *inst = n->inst;

                calculate_deps(&state, n);

                switch (inst->op) {
                case QOP_TEX_S:
                case QOP_TEX_T:
                case QOP_TEX_R:
                case QOP_TEX_B:
                case QOP_TEX_DIRECT:
                        /* If the texture coordinate fifo is full,
                         * block this on the last QOP_TEX_RESULT.
                         */
                        if (state.tfreq_count == 8) {
                                block_until_tex_result(&state, n);
                        }

                        /* If the texture result fifo is full, block
                         * adding any more to it until the last
                         * QOP_TEX_RESULT.
                         */
                        if (inst->op == QOP_TEX_S ||
                            inst->op == QOP_TEX_DIRECT) {
                                if (state.tfrcv_count == 4)
                                        block_until_tex_result(&state, n);
                                state.tfrcv_count++;
                        }

                        state.tex_fifo[state.tex_fifo_pos].coords++;
                        state.tfreq_count++;
                        break;

                case QOP_TEX_RESULT:
                        /* Results have to be fetched after the
                         * coordinate setup.  Note that we're assuming
                         * here that our input shader has the texture
                         * coord setup and result fetch in order,
                         * which is true initially but not of our
                         * instruction stream after this pass.
                         */
                        add_dep(state.dir, state.last_tex_coord, n);

                        state.tex_fifo[state.tex_fifo_pos].node = n;

                        state.tex_fifo_pos++;
                        memset(&state.tex_fifo[state.tex_fifo_pos], 0,
                               sizeof(state.tex_fifo[0]));
                        break;
                default:
                        assert(!qir_is_tex(inst));
                        break;
                }
        }
}

static void
calculate_reverse_deps(struct vc4_compile *c, void *mem_ctx,
                       struct list_head *schedule_list)
{
        struct schedule_setup_state state;

        memset(&state, 0, sizeof(state));
        state.dir = R;
        state.last_temp_write = rzalloc_array(mem_ctx, struct schedule_node *,
                                              c->num_temps);

        list_for_each_entry_rev(struct schedule_node, n, schedule_list, link) {
                calculate_deps(&state, n);
        }
}

static int
get_register_pressure_cost(struct schedule_state *state, struct qinst *inst)
{
        int cost = 0;

        if (inst->dst.file == QFILE_TEMP &&
            state->temp_writes[inst->dst.index] == 1)
                cost--;

        for (int i = 0; i < qir_get_op_nsrc(inst->op); i++) {
                if (inst->src[i].file == QFILE_TEMP &&
                    !BITSET_TEST(state->temp_live, inst->src[i].index)) {
                        cost++;
                }
        }

        return cost;
}

static bool
locks_scoreboard(struct qinst *inst)
{
        switch (inst->op) {
        case QOP_TLB_Z_WRITE:
        case QOP_TLB_COLOR_WRITE:
        case QOP_TLB_COLOR_WRITE_MS:
        case QOP_TLB_COLOR_READ:
                return true;
        default:
                return false;
        }
}

static struct schedule_node *
choose_instruction(struct schedule_state *state)
{
        struct schedule_node *chosen = NULL;

        list_for_each_entry(struct schedule_node, n, &state->worklist, link) {
                if (!chosen) {
                        chosen = n;
                        continue;
                }

                /* Prefer scheduling things that lock the scoreboard, so that
                 * they appear late in the program and we get more parallelism
                 * between shaders on multiple QPUs hitting the same fragment.
                 */
                if (locks_scoreboard(n->inst) &&
                    !locks_scoreboard(chosen->inst)) {
                        chosen = n;
                        continue;
                } else if (!locks_scoreboard(n->inst) &&
                           locks_scoreboard(chosen->inst)) {
                        continue;
                }

                /* If we would block on the previously chosen node, but would
                 * block less on this one, then then prefer it.
                 */
                if (chosen->unblocked_time > state->time &&
                    n->unblocked_time < chosen->unblocked_time) {
                        chosen = n;
                        continue;
                } else if (n->unblocked_time > state->time &&
                           n->unblocked_time > chosen->unblocked_time) {
                        continue;
                }

                /* If we can definitely reduce register pressure, do so
                 * immediately.
                 */
                int register_pressure_cost =
                        get_register_pressure_cost(state, n->inst);
                int chosen_register_pressure_cost =
                        get_register_pressure_cost(state, chosen->inst);

                if (register_pressure_cost < chosen_register_pressure_cost) {
                        chosen = n;
                        continue;
                } else if (register_pressure_cost >
                           chosen_register_pressure_cost) {
                        continue;
                }

                /* Otherwise, prefer instructions with the deepest chain to
                 * the end of the program.  This avoids the problem of
                 * "everything generates a temp, nothing finishes freeing one,
                 * guess I'll just keep emitting varying mul/adds".
                 */
                if (n->delay > chosen->delay) {
                        chosen = n;
                        continue;
                } else if (n->delay < chosen->delay) {
                        continue;
                }
        }

        return chosen;
}

static void
dump_state(struct vc4_compile *c, struct schedule_state *state)
{
        uint32_t i = 0;
        list_for_each_entry(struct schedule_node, n, &state->worklist, link) {
                fprintf(stderr, "%3d: ", i++);
                qir_dump_inst(c, n->inst);
                fprintf(stderr, " (%d cost)\n",
                        get_register_pressure_cost(state, n->inst));

                for (int i = 0; i < n->child_count; i++) {
                        struct schedule_node *child = n->children[i];
                        fprintf(stderr, "   - ");
                        qir_dump_inst(c, child->inst);
                        fprintf(stderr, " (%d parents)\n", child->parent_count);
                }
        }
}

/* Estimate of how many instructions we should schedule between operations.
 *
 * These aren't in real cycle counts, because we're just estimating cycle
 * times anyway.  QIR instructions will get paired up when turned into QPU
 * instructions, or extra NOP delays will have to be added due to register
 * allocation choices.
 */
static uint32_t
latency_between(struct schedule_node *before, struct schedule_node *after)
{
        if ((before->inst->op == QOP_TEX_S ||
             before->inst->op == QOP_TEX_DIRECT) &&
            after->inst->op == QOP_TEX_RESULT)
                return 100;

        return 1;
}

/** Recursive computation of the delay member of a node. */
static void
compute_delay(struct schedule_node *n)
{
        if (!n->child_count) {
                /* The color read needs to be scheduled late, to avoid locking
                 * the scoreboard early.  This is our best tool for
                 * encouraging that.  The other scoreboard locking ops will
                 * have this happen by default, since they are generally the
                 * DAG heads or close to them.
                 */
                if (n->inst->op == QOP_TLB_COLOR_READ)
                        n->delay = 1000;
                else
                        n->delay = 1;
        } else {
                for (int i = 0; i < n->child_count; i++) {
                        if (!n->children[i]->delay)
                                compute_delay(n->children[i]);
                        n->delay = MAX2(n->delay,
                                        n->children[i]->delay +
                                        latency_between(n, n->children[i]));
                }
        }
}

static void
schedule_instructions(struct vc4_compile *c, struct schedule_state *state)
{
        if (debug) {
                fprintf(stderr, "initial deps:\n");
                dump_state(c, state);
        }

        /* Remove non-DAG heads from the list. */
        list_for_each_entry_safe(struct schedule_node, n,
                                 &state->worklist, link) {
                if (n->parent_count != 0)
                        list_del(&n->link);
        }

        state->time = 0;
        while (!list_empty(&state->worklist)) {
                struct schedule_node *chosen = choose_instruction(state);
                struct qinst *inst = chosen->inst;

                if (debug) {
                        fprintf(stderr, "current list:\n");
                        dump_state(c, state);
                        fprintf(stderr, "chose: ");
                        qir_dump_inst(c, inst);
                        fprintf(stderr, " (%d cost)\n",
                                get_register_pressure_cost(state, inst));
                }

                state->time = MAX2(state->time, chosen->unblocked_time);

                /* Schedule this instruction back onto the QIR list. */
                list_del(&chosen->link);
                list_add(&inst->link, &c->instructions);

                /* Now that we've scheduled a new instruction, some of its
                 * children can be promoted to the list of instructions ready to
                 * be scheduled.  Update the children's unblocked time for this
                 * DAG edge as we do so.
                 */
                for (int i = chosen->child_count - 1; i >= 0; i--) {
                        struct schedule_node *child = chosen->children[i];

                        child->unblocked_time = MAX2(child->unblocked_time,
                                                     state->time +
                                                     latency_between(chosen,
                                                                     child));
                        child->parent_count--;
                        if (child->parent_count == 0)
                                list_add(&child->link, &state->worklist);
                }

                /* Update our tracking of register pressure. */
                for (int i = 0; i < qir_get_op_nsrc(inst->op); i++) {
                        if (inst->src[i].file == QFILE_TEMP)
                                BITSET_SET(state->temp_live, inst->src[i].index);
                }
                if (inst->dst.file == QFILE_TEMP) {
                        state->temp_writes[inst->dst.index]--;
                        if (state->temp_writes[inst->dst.index] == 0)
                                BITSET_CLEAR(state->temp_live, inst->dst.index);
                }

                state->time++;
        }
}

void
qir_schedule_instructions(struct vc4_compile *c)
{
        void *mem_ctx = ralloc_context(NULL);
        struct schedule_state state = { { 0 } };

        if (debug) {
                fprintf(stderr, "Pre-schedule instructions\n");
                qir_dump(c);
        }

        state.temp_writes = rzalloc_array(mem_ctx, uint32_t, c->num_temps);
        state.temp_live = rzalloc_array(mem_ctx, BITSET_WORD,
                                        BITSET_WORDS(c->num_temps));
        list_inithead(&state.worklist);

        /* Wrap each instruction in a scheduler structure. */
        list_for_each_entry_safe(struct qinst, inst, &c->instructions, link) {
                struct schedule_node *n = rzalloc(mem_ctx, struct schedule_node);

                n->inst = inst;
                list_del(&inst->link);
                list_addtail(&n->link, &state.worklist);

                if (inst->dst.file == QFILE_TEMP)
                        state.temp_writes[inst->dst.index]++;
        }

        /* Dependencies tracked top-to-bottom. */
        calculate_forward_deps(c, mem_ctx, &state.worklist);
        /* Dependencies tracked bottom-to-top. */
        calculate_reverse_deps(c, mem_ctx, &state.worklist);

        list_for_each_entry(struct schedule_node, n, &state.worklist, link)
                compute_delay(n);

        schedule_instructions(c, &state);

        if (debug) {
                fprintf(stderr, "Post-schedule instructions\n");
                qir_dump(c);
        }

        ralloc_free(mem_ctx);
}
