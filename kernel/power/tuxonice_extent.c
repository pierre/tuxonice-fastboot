/*
 * kernel/power/tuxonice_extent.c
 *
 * Copyright (C) 2003-2008 Nigel Cunningham (nigel at tuxonice net)
 *
 * Distributed under GPLv2.
 *
 * These functions encapsulate the manipulation of storage metadata.
 */

#include <linux/suspend.h>
#include "tuxonice_modules.h"
#include "tuxonice_extent.h"
#include "tuxonice_alloc.h"
#include "tuxonice_ui.h"
#include "tuxonice.h"

/**
 * toi_get_extent - return a free extent
 *
 * May fail, returning NULL instead.
 **/
static struct hibernate_extent *toi_get_extent(void)
{
	return (struct hibernate_extent *) toi_kzalloc(2,
			sizeof(struct hibernate_extent), TOI_ATOMIC_GFP);
}

/**
 * toi_put_extent_chain - free a whole chain of extents
 * @chain:	Chain to free.
 **/
void toi_put_extent_chain(struct hibernate_extent_chain *chain)
{
	struct hibernate_extent *this;

	this = chain->first;

	while (this) {
		struct hibernate_extent *next = this->next;
		toi_kfree(2, this);
		chain->num_extents--;
		this = next;
	}

	chain->first = NULL;
	chain->last_touched = NULL;
	chain->size = 0;
}
EXPORT_SYMBOL_GPL(toi_put_extent_chain);

/**
 * toi_add_to_extent_chain - add an extent to an existing chain
 * @chain:	Chain to which the extend should be added
 * @start:	Start of the extent (first physical block)
 * @end:	End of the extent (last physical block)
 *
 * The chain information is updated if the insertion is successful.
 **/
int toi_add_to_extent_chain(struct hibernate_extent_chain *chain,
		unsigned long start, unsigned long end)
{
	struct hibernate_extent *new_ext = NULL, *cur_ext = NULL;

	/* Find the right place in the chain */
	if (chain->last_touched && chain->last_touched->start < start)
		cur_ext = chain->last_touched;
	else if (chain->first && chain->first->start < start)
		cur_ext = chain->first;

	if (cur_ext) {
		while (cur_ext->next && cur_ext->next->start < start)
			cur_ext = cur_ext->next;

		if (cur_ext->end == (start - 1)) {
			struct hibernate_extent *next_ext = cur_ext->next;
			cur_ext->end = end;

			/* Merge with the following one? */
			if (next_ext && cur_ext->end + 1 == next_ext->start) {
				cur_ext->end = next_ext->end;
				cur_ext->next = next_ext->next;
				toi_kfree(2, next_ext);
				chain->num_extents--;
			}

			chain->last_touched = cur_ext;
			chain->size += (end - start + 1);

			return 0;
		}
	}

	new_ext = toi_get_extent();
	if (!new_ext) {
		printk(KERN_INFO "Error unable to append a new extent to the "
				"chain.\n");
		return -ENOMEM;
	}

	chain->num_extents++;
	chain->size += (end - start + 1);
	new_ext->start = start;
	new_ext->end = end;

	chain->last_touched = new_ext;

	if (cur_ext) {
		new_ext->next = cur_ext->next;
		cur_ext->next = new_ext;
	} else {
		if (chain->first)
			new_ext->next = chain->first;
		chain->first = new_ext;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(toi_add_to_extent_chain);

/**
 * toi_serialise_extent_chain - write a chain in the image
 * @owner:	Module writing the chain.
 * @chain:	Chain to write.
 **/
int toi_serialise_extent_chain(struct toi_module_ops *owner,
		struct hibernate_extent_chain *chain)
{
	struct hibernate_extent *this;
	int ret, i = 0;

	ret = toiActiveAllocator->rw_header_chunk(WRITE, owner, (char *) chain,
			2 * sizeof(int));
	if (ret)
		return ret;

	this = chain->first;
	while (this) {
		ret = toiActiveAllocator->rw_header_chunk(WRITE, owner,
				(char *) this, 2 * sizeof(unsigned long));
		if (ret)
			return ret;
		this = this->next;
		i++;
	}

	if (i != chain->num_extents) {
		printk(KERN_EMERG "Saved %d extents but chain metadata says "
			"there should be %d.\n", i, chain->num_extents);
		return 1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(toi_serialise_extent_chain);

/**
 * toi_load_extent_chain - read back a chain saved in the image
 * @chain:	Chain to load
 *
 * The linked list of extents is reconstructed from the disk. chain will point
 * to the first entry.
 **/
int toi_load_extent_chain(struct hibernate_extent_chain *chain)
{
	struct hibernate_extent *this, *last = NULL;
	int i, ret;

	/* Get the next page */
	ret = toiActiveAllocator->rw_header_chunk_noreadahead(READ, NULL,
			(char *) chain, 2 * sizeof(int));
	if (ret) {
		printk(KERN_ERR "Failed to read the size of extent chain.\n");
		return 1;
	}

	for (i = 0; i < chain->num_extents; i++) {
		this = toi_kzalloc(3, sizeof(struct hibernate_extent),
				TOI_ATOMIC_GFP);
		if (!this) {
			printk(KERN_INFO "Failed to allocate a new extent.\n");
			return -ENOMEM;
		}
		this->next = NULL;
		/* Get the next page */
		ret = toiActiveAllocator->rw_header_chunk_noreadahead(READ,
				NULL, (char *) this, 2 * sizeof(unsigned long));
		if (ret) {
			printk(KERN_INFO "Failed to read an extent.\n");
			return 1;
		}
		if (last)
			last->next = this;
		else
			chain->first = this;
		last = this;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(toi_load_extent_chain);

/**
 * toi_extent_state_next - go to the next extent
 *
 * Given a state, progress to the next valid entry. We may begin in an
 * invalid state, as we do when invoked after extent_state_goto_start below.
 *
 * When using compression and expected_compression > 0, we let the image size
 * be larger than storage, so we can validly run out of data to return.
 **/
unsigned long toi_extent_state_next(struct toi_extent_iterate_state *state)
{
	if (state->current_chain == state->num_chains)
		return 0;

	if (state->current_extent) {
		if (state->current_offset == state->current_extent->end) {
			if (state->current_extent->next) {
				state->current_extent =
					state->current_extent->next;
				state->current_offset =
					state->current_extent->start;
			} else {
				state->current_extent = NULL;
				state->current_offset = 0;
			}
		} else
			state->current_offset++;
	}

	while (!state->current_extent) {
		int chain_num = ++(state->current_chain);

		if (chain_num == state->num_chains)
			return 0;

		state->current_extent = (state->chains + chain_num)->first;

		if (!state->current_extent)
			continue;

		state->current_offset = state->current_extent->start;
	}

	return state->current_offset;
}
EXPORT_SYMBOL_GPL(toi_extent_state_next);

/**
 * toi_extent_state_goto_start - reinitialize an extent chain iterator
 * @state:	Iterator to reinitialize
 **/
void toi_extent_state_goto_start(struct toi_extent_iterate_state *state)
{
	state->current_chain = -1;
	state->current_extent = NULL;
	state->current_offset = 0;
}
EXPORT_SYMBOL_GPL(toi_extent_state_goto_start);

/**
 * toi_extent_state_save - save state of the iterator
 * @state:		Current state of the chain
 * @saved_state:	Iterator to populate
 *
 * Given a state and a struct hibernate_extent_state_store, save the current
 * position in a format that can be used with relocated chains (at
 * resume time).
 **/
void toi_extent_state_save(struct toi_extent_iterate_state *state,
		struct hibernate_extent_iterate_saved_state *saved_state)
{
	struct hibernate_extent *extent;

	saved_state->chain_num = state->current_chain;
	saved_state->extent_num = 0;
	saved_state->offset = state->current_offset;

	if (saved_state->chain_num == -1)
		return;

	extent = (state->chains + state->current_chain)->first;

	while (extent != state->current_extent) {
		saved_state->extent_num++;
		extent = extent->next;
	}
}
EXPORT_SYMBOL_GPL(toi_extent_state_save);

/**
 * toi_extent_state_restore - restore the position saved by extent_state_save
 * @state:		State to populate
 * @saved_state:	Iterator saved to restore
 **/
void toi_extent_state_restore(struct toi_extent_iterate_state *state,
		struct hibernate_extent_iterate_saved_state *saved_state)
{
	int posn = saved_state->extent_num;

	if (saved_state->chain_num == -1) {
		toi_extent_state_goto_start(state);
		return;
	}

	state->current_chain = saved_state->chain_num;
	state->current_extent = (state->chains + state->current_chain)->first;
	state->current_offset = saved_state->offset;

	while (posn--)
		state->current_extent = state->current_extent->next;
}
EXPORT_SYMBOL_GPL(toi_extent_state_restore);
