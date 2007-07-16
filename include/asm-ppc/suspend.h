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

#define toi_faulted (0)
#define clear_toi_fault() do { } while(0)
