/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#include <linux/sched.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/spinlock.h>
#include <linux/page-flags.h>
#include <asm/bug.h>
#include "ctree.h"
#include "extent_io.h"
#include "locking.h"

/*
 * locks the per buffer mutex in an extent buffer.  This uses adaptive locks
 * and the spin is not tuned very extensively.  The spinning does make a big
 * difference in almost every workload, but spinning for the right amount of
 * time needs some help.
 *
 * In general, we want to spin as long as the lock holder is doing btree
 * searches, and we should give up if they are in more expensive code.
 */

int btrfs_tree_lock(struct extent_buffer *eb)
{
	int i;

	if (mutex_trylock(&eb->mutex))
		return 0;
	for (i = 0; i < 512; i++) {
		cpu_relax();
		if (mutex_trylock(&eb->mutex))
			return 0;
	}
	cpu_relax();
	mutex_lock_nested(&eb->mutex, BTRFS_MAX_LEVEL - btrfs_header_level(eb));
	return 0;
}

int btrfs_try_tree_lock(struct extent_buffer *eb)
{
	return mutex_trylock(&eb->mutex);
}

int btrfs_tree_unlock(struct extent_buffer *eb)
{
	mutex_unlock(&eb->mutex);
	return 0;
}

int btrfs_tree_locked(struct extent_buffer *eb)
{
	return mutex_is_locked(&eb->mutex);
}

/*
 * btrfs_search_slot uses this to decide if it should drop its locks
 * before doing something expensive like allocating free blocks for cow.
 */
int btrfs_path_lock_waiting(struct btrfs_path *path, int level)
{
	int i;
	struct extent_buffer *eb;
	for (i = level; i <= level + 1 && i < BTRFS_MAX_LEVEL; i++) {
		eb = path->nodes[i];
		if (!eb)
			break;
		smp_mb();
		if (!list_empty(&eb->mutex.wait_list))
			return 1;
	}
	return 0;
}

