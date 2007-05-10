static inline int arch_prepare_suspend(void)
{
	return 0;
}

static inline void save_processor_state(void)
{
}

static inline void restore_processor_state(void)
{
}

#define suspend2_faulted (0)
#define clear_suspend2_fault() do { } while(0)
