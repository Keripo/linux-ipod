#ifndef __MICROBLAZE_MMU_CONTEXT_H__
#define __MICROBLAZE_MMU_CONTEXT_H__

#define destroy_context(mm)		((void)0)
#define init_new_context(tsk,mm)	0
#define switch_mm(prev,next,tsk,cpu)	((void)0)
#define activate_mm(prev,next)		((void)0)
#define enter_lazy_tlb(mm,tsk,cpu)	((void)0)

#endif /* __MICROBLAZE_MMU_CONTEXT_H__ */