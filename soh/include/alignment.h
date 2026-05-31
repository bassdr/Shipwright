#ifndef ALIGNMENT_H
#define ALIGNMENT_H

#define ALIGN8(val) (((val) + 7) & ~7)
#define ALIGN16(val) (((val) + (uintptr_t)0xFu) & ~(uintptr_t)0xFu)
#define ALIGN32(val) (((val) + (uintptr_t)0x1Fu) & ~(uintptr_t)0x1Fu)
#define ALIGN64(val) (((val) + (uintptr_t)0x3Fu) & ~(uintptr_t)0x3Fu)
#define ALIGN256(val) (((val) + (uintptr_t)0xFFu) & ~(uintptr_t)0xFFu)

#ifdef __GNUC__
#define ALIGNED8 __attribute__ ((aligned (8)))
#else
#define ALIGNED8
#endif

#endif
