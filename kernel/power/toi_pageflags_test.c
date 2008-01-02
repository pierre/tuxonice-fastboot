/*
 * TuxOnIce pageflags tester.
 */

#include "linux/module.h"
#include "linux/bootmem.h"
#include "linux/sched.h"
#include "linux/dyn_pageflags.h"

DECLARE_DYN_PAGEFLAGS(test_map);

static char *bits_on(void)
{
	char *page = (char *) get_zeroed_page(GFP_KERNEL);
	unsigned long index = get_next_bit_on(&test_map, max_pfn + 1);
	int pos = 0;

	while (index <= max_pfn) {
		pos += snprintf_used(page + pos, PAGE_SIZE - pos - 1, "%d ",
				index);
		index = get_next_bit_on(&test_map, index);
	}

	return page;
}

static __init int do_check(void)
{
	unsigned long index;
	int step = 1, steps = 100;

	allocate_dyn_pageflags(&test_map, 0);

	for (index = 1; index < max_pfn; index++) {
		char *result;
		char compare[100];

		if (index > (max_pfn / steps * step)) {
			printk(KERN_INFO "%d/%d\r", step, steps);
			step++;
		}


		if (!pfn_valid(index))
			continue;

		clear_dyn_pageflags(&test_map);
		set_dynpageflag(&test_map, pfn_to_page(0));
		set_dynpageflag(&test_map, pfn_to_page(index));

		sprintf(compare, "0 %lu ", index);

		result = bits_on();

		if (strcmp(result, compare)) {
			printk(KERN_INFO "Expected \"%s\", got \"%s\"\n",
					result, compare);
		}

		free_page((unsigned long) result);
		schedule();
	}

	free_dyn_pageflags(&test_map);
	return 0;
}

#ifdef MODULE
static __exit void check_unload(void)
{
}

module_init(do_check);
module_exit(check_unload);
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("Pageflags testing");
MODULE_LICENSE("GPL");
#else
late_initcall(do_check);
#endif
