/*
 * kernel/power/tuxonice_extent.h
 *
 * Copyright (C) 2003-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * It contains declarations related to extents. Extents are
 * TuxOnIce's method of storing some of the metadata for the image.
 * See tuxonice_extent.c for more info.
 *
 */

#include "tuxonice_modules.h"

#ifndef EXTENT_H
#define EXTENT_H

struct extent {
	unsigned long minimum, maximum;
	struct extent *next;
};

struct extent_chain {
	int size; /* size of the chain ie sum (max-min+1) */
	int num_extents;
	struct extent *first, *last_touched;
};

struct extent_iterate_state {
	struct extent_chain *chains;
	int num_chains;
	int current_chain;
	struct extent *current_extent;
	unsigned long current_offset;
};

struct extent_iterate_saved_state {
	int chain_num;
	int extent_num;
	unsigned long offset;
};

#define toi_extent_state_eof(state) ((state)->num_chains == (state)->current_chain)

/* Simplify iterating through all the values in an extent chain */
#define toi_extent_for_each(extent_chain, extentpointer, value) \
if ((extent_chain)->first) \
	for ((extentpointer) = (extent_chain)->first, (value) = \
			(extentpointer)->minimum; \
	     ((extentpointer) && ((extentpointer)->next || (value) <= \
				 (extentpointer)->maximum)); \
	     (((value) == (extentpointer)->maximum) ? \
		((extentpointer) = (extentpointer)->next, (value) = \
		 ((extentpointer) ? (extentpointer)->minimum : 0)) : \
			(value)++))

void toi_put_extent_chain(struct extent_chain *chain);
int toi_add_to_extent_chain(struct extent_chain *chain, 
		unsigned long minimum, unsigned long maximum);
int toi_serialise_extent_chain(struct toi_module_ops *owner,
		struct extent_chain *chain);
int toi_load_extent_chain(struct extent_chain *chain);

/* swap_entry_to_extent_val & extent_val_to_swap_entry: 
 * We are putting offset in the low bits so consecutive swap entries
 * make consecutive extent values */
#define swap_entry_to_extent_val(swp_entry) (swp_entry.val)
#define extent_val_to_swap_entry(val) (swp_entry_t) { (val) }

void toi_extent_state_save(struct extent_iterate_state *state,
		struct extent_iterate_saved_state *saved_state);
void toi_extent_state_restore(struct extent_iterate_state *state,
		struct extent_iterate_saved_state *saved_state);
void toi_extent_state_goto_start(struct extent_iterate_state *state);
unsigned long toi_extent_state_next(struct extent_iterate_state *state);
#endif
