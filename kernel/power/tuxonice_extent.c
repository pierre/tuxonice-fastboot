/* 
 * kernel/power/tuxonice_extent.c
 * 
 * Copyright (C) 2003-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * Distributed under GPLv2.
 * 
 * These functions encapsulate the manipulation of storage metadata. For
 * pageflags, we use dynamically allocated bitmaps.
 */

#include <linux/module.h>
#include <linux/suspend.h>
#include "tuxonice_modules.h"
#include "tuxonice_extent.h"
#include "tuxonice_ui.h"
#include "tuxonice.h"

/* suspend_get_extent
 *
 * Returns a free extent. May fail, returning NULL instead.
 */
static struct extent *suspend_get_extent(void)
{
	struct extent *result;
	
	if (!(result = kmalloc(sizeof(struct extent), S2_ATOMIC_GFP)))
		return NULL;

	result->minimum = result->maximum = 0;
	result->next = NULL;

	return result;
}

/* suspend_put_extent_chain.
 *
 * Frees a whole chain of extents.
 */
void suspend_put_extent_chain(struct extent_chain *chain)
{
	struct extent *this;

	this = chain->first;

	while(this) {
		struct extent *next = this->next;
		kfree(this);
		chain->num_extents--;
		this = next;
	}
	
	chain->first = chain->last_touched = NULL;
	chain->size = 0;
}

/* 
 * suspend_add_to_extent_chain
 *
 * Add an extent to an existing chain.
 */
int suspend_add_to_extent_chain(struct extent_chain *chain, 
		unsigned long minimum, unsigned long maximum)
{
	struct extent *new_extent = NULL, *start_at;

	/* Find the right place in the chain */
	start_at = (chain->last_touched && 
		    (chain->last_touched->minimum < minimum)) ?
		chain->last_touched : NULL;

	if (!start_at && chain->first && chain->first->minimum < minimum)
		start_at = chain->first;

	while (start_at && start_at->next && start_at->next->minimum < minimum)
		start_at = start_at->next;

	if (start_at && start_at->maximum == (minimum - 1)) {
		start_at->maximum = maximum;

		/* Merge with the following one? */
		if (start_at->next &&
		    start_at->maximum + 1 == start_at->next->minimum) {
			struct extent *to_free = start_at->next;
			start_at->maximum = start_at->next->maximum;
			start_at->next = start_at->next->next;
			chain->num_extents--;
			kfree(to_free);
		}

		chain->last_touched = start_at;
		chain->size+= (maximum - minimum + 1);

		return 0;
	}

	new_extent = suspend_get_extent();
	if (!new_extent) {
		printk("Error unable to append a new extent to the chain.\n");
		return 2;
	}

	chain->num_extents++;
	chain->size+= (maximum - minimum + 1);
	new_extent->minimum = minimum;
	new_extent->maximum = maximum;
	new_extent->next = NULL;

	chain->last_touched = new_extent;

	if (start_at) {
		struct extent *next = start_at->next;
		start_at->next = new_extent;
		new_extent->next = next;
	} else {
		if (chain->first)
			new_extent->next = chain->first;
		chain->first = new_extent;
	}

	return 0;
}

/* suspend_serialise_extent_chain
 *
 * Write a chain in the image.
 */
int suspend_serialise_extent_chain(struct suspend_module_ops *owner,
		struct extent_chain *chain)
{
	struct extent *this;
	int ret, i = 0;
	
	if ((ret = suspendActiveAllocator->rw_header_chunk(WRITE, owner,
		(char *) chain,
		2 * sizeof(int))))
		return ret;

	this = chain->first;
	while (this) {
		if ((ret = suspendActiveAllocator->rw_header_chunk(WRITE, owner,
				(char *) this,
				2 * sizeof(unsigned long))))
			return ret;
		this = this->next;
		i++;
	}

	if (i != chain->num_extents) {
		printk(KERN_EMERG "Saved %d extents but chain metadata says there "
			"should be %d.\n", i, chain->num_extents);
		return 1;
	}

	return ret;
}

/* suspend_load_extent_chain
 *
 * Read back a chain saved in the image.
 */
int suspend_load_extent_chain(struct extent_chain *chain)
{
	struct extent *this, *last = NULL;
	int i, ret;

	if ((ret = suspendActiveAllocator->rw_header_chunk(READ, NULL,
		(char *) chain,	2 * sizeof(int)))) {
		printk("Failed to read size of extent chain.\n");
		return 1;
	}

	for (i = 0; i < chain->num_extents; i++) {
		this = kmalloc(sizeof(struct extent), S2_ATOMIC_GFP);
		if (!this) {
			printk("Failed to allocate a new extent.\n");
			return -ENOMEM;
		}
		this->next = NULL;
		if ((ret = suspendActiveAllocator->rw_header_chunk(READ, NULL,
				(char *) this, 2 * sizeof(unsigned long)))) {
			printk("Failed to an extent.\n");
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

/* suspend_extent_state_next
 *
 * Given a state, progress to the next valid entry. We may begin in an
 * invalid state, as we do when invoked after extent_state_goto_start below.
 *
 * When using compression and expected_compression > 0, we let the image size
 * be larger than storage, so we can validly run out of data to return.
 */
unsigned long suspend_extent_state_next(struct extent_iterate_state *state)
{
	if (state->current_chain == state->num_chains)
		return 0;

	if (state->current_extent) {
		if (state->current_offset == state->current_extent->maximum) {
			if (state->current_extent->next) {
				state->current_extent = state->current_extent->next;
				state->current_offset = state->current_extent->minimum;
			} else {
				state->current_extent = NULL;
				state->current_offset = 0;
			}
		} else
			state->current_offset++;
	}

	while(!state->current_extent) {
		int chain_num = ++(state->current_chain);

		if (chain_num == state->num_chains)
			return 0;

		state->current_extent = (state->chains + chain_num)->first;

		if (!state->current_extent)
			continue;

		state->current_offset = state->current_extent->minimum;
	}

	return state->current_offset;
}

/* suspend_extent_state_goto_start
 *
 * Find the first valid value in a group of chains.
 */
void suspend_extent_state_goto_start(struct extent_iterate_state *state)
{
	state->current_chain = -1;
	state->current_extent = NULL;
	state->current_offset = 0;
}

/* suspend_extent_start_save
 *
 * Given a state and a struct extent_state_store, save the current
 * position in a format that can be used with relocated chains (at
 * resume time).
 */
void suspend_extent_state_save(struct extent_iterate_state *state,
		struct extent_iterate_saved_state *saved_state)
{
	struct extent *extent;

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

/* suspend_extent_start_restore
 *
 * Restore the position saved by extent_state_save.
 */
void suspend_extent_state_restore(struct extent_iterate_state *state,
		struct extent_iterate_saved_state *saved_state)
{
	int posn = saved_state->extent_num;

	if (saved_state->chain_num == -1) {
		suspend_extent_state_goto_start(state);
		return;
	}

	state->current_chain = saved_state->chain_num;
	state->current_extent = (state->chains + state->current_chain)->first;
	state->current_offset = saved_state->offset;

	while (posn--)
		state->current_extent = state->current_extent->next;
}

#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(suspend_add_to_extent_chain);
EXPORT_SYMBOL_GPL(suspend_put_extent_chain);
EXPORT_SYMBOL_GPL(suspend_load_extent_chain);
EXPORT_SYMBOL_GPL(suspend_serialise_extent_chain);
EXPORT_SYMBOL_GPL(suspend_extent_state_save);
EXPORT_SYMBOL_GPL(suspend_extent_state_restore);
EXPORT_SYMBOL_GPL(suspend_extent_state_goto_start);
EXPORT_SYMBOL_GPL(suspend_extent_state_next);
#endif
