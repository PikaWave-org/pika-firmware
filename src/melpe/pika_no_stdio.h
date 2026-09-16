/* Not upstream. Added by pika - see PROVENANCE.md.
 *
 * Two shims, both here for one reason: nothing in the codec may reach
 * newlib's stdio. A single fprintf or assert links vfprintf, and vfprintf
 * drags in malloc and _sbrk - about 40KB, and a heap, in an image that
 * deliberately has neither. It would also undo the point of the fixed arena
 * that replaced malloc in mat_lib.c.
 *
 * Both shims keep the check and drop only the printing.
 */

#pragma once

#include <assert.h>

/* The codec asserts in three places: two upstream sanity checks in
 * math_lib.c, and the arena bounds check in mat_lib.c. __builtin_trap is a
 * single undefined instruction that lands in the fault handler, costs two
 * bytes and needs no library. These fire only on a real bug, so trapping
 * beats compiling them out and continuing on corrupt audio. */
#undef assert
#define assert(cond)						\
	do {							\
		if (!(cond))					\
			__builtin_trap();			\
	} while (0)

/* A diagnostic upstream prints when it finds an unstable LSP filter. The
 * caller handles that case regardless, so the message is the only thing
 * lost. Define MELPE_DIAGNOSTICS on a host build to get it back. */
#ifdef MELPE_DIAGNOSTICS
#include <stdio.h>
#define MELPE_DIAG(msg)		fprintf(stderr, "%s", (msg))
#else
#define MELPE_DIAG(msg)		((void) 0)
#endif
