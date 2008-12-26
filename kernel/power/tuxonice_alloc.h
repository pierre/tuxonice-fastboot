/*
 * kernel/power/tuxonice_alloc.h
 *
 * Copyright (C) 2008 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 */

#define TOI_WAIT_GFP (GFP_KERNEL | __GFP_NOWARN)
#define TOI_ATOMIC_GFP (GFP_ATOMIC | __GFP_NOWARN)

#define toi_kzalloc(FAIL, SIZE, FLAGS) (kzalloc(SIZE, FLAGS))
#define toi_kfree(FAIL, ALLOCN) (kfree(ALLOCN))

#define toi_get_free_pages(FAIL, FLAGS, ORDER) __get_free_pages(FLAGS, ORDER)
#define toi_get_free_page(FAIL, FLAGS) __get_free_page(FLAGS)
#define toi_get_zeroed_page(FAIL, FLAGS) get_zeroed_page(FLAGS)
#define toi_free_page(FAIL, ALLOCN) do { free_page(ALLOCN); } while (0)
#define toi__free_page(FAIL, PAGE) __free_page(PAGE)
#define toi_free_pages(FAIL, PAGE, ORDER) __free_pages(PAGE, ORDER)
#define toi_alloc_page(FAIL, MASK) alloc_page(MASK)
static inline int toi_alloc_init(void)
{
	return 0;
}

static inline void toi_alloc_exit(void) { }

static inline void toi_alloc_print_debug_stats(void) { }
