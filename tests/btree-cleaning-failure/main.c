/*
 * Regression for callback_clean() failure accounting.
 *
 * Compile the real btree_cleaning.c and force allocation of the per-node
 * cleaning state to fail.  The abandoned node must release its pending clean,
 * unlock exactly once, and free the now-empty cleaning group.
 */
#include <sys/time.h>

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../kvlds/btree.h"
#include "../../kvlds/node.h"

static size_t unlock_calls;
static size_t free_calls;
static int fail_alloc;

static void * test_malloc(size_t);
static void test_free(void *);

/*
 * btree_cleaning.c only needs these three btree_node operations.  Suppress
 * btree_node.h so the fixture can provide deterministic stand-ins without a
 * page pool.
 */
#define BTREE_NODE_H_
static void btree_node_unlock(struct btree *, struct node *);
static int btree_node_descend(struct btree *, struct node *,
    int (*)(void *, struct node *), void *);
static struct node * btree_node_dirty(struct btree *, struct node *);

#define malloc test_malloc
#define free test_free
#include "../../kvlds/btree_cleaning.c"
#undef free
#undef malloc

static void *
test_malloc(size_t len)
{

	if (fail_alloc)
		return (NULL);
	return (malloc(len));
}

static void
test_free(void * ptr)
{

	free_calls++;
	free(ptr);
}

static void
btree_node_unlock(struct btree * T, struct node * N)
{

	assert(T != NULL);
	assert(N != NULL);
	unlock_calls++;
}

static int
btree_node_descend(struct btree * T, struct node * N,
    int (* callback)(void *, struct node *), void * cookie)
{

	(void)T;
	(void)N;
	(void)callback;
	(void)cookie;
	return (-1);
}

static struct node *
btree_node_dirty(struct btree * T, struct node * N)
{

	(void)T;
	(void)N;
	return (NULL);
}

void *
events_timer_register(int (* callback)(void *), void * cookie,
    const struct timeval * timeout)
{

	(void)callback;
	(void)cookie;
	(void)timeout;
	return ((void *)1);
}

void
events_timer_cancel(void * cookie)
{

	(void)cookie;
}

int
events_run(void)
{

	return (0);
}

void
libcperciva_warn(const char * format, ...)
{

	(void)format;
}

void
libcperciva_warnx(const char * format, ...)
{

	(void)format;
}

int
main(void)
{
	struct btree T = {0};
	struct cleaner C = {0};
	struct node N = {0};
	struct cleaning_group * CG;

	if ((CG = calloc(1, sizeof(*CG))) == NULL)
		return (1);

	C.T = &T;
	C.head = CG;
	C.pending_cleans = 1;

	CG->C = &C;
	CG->pending_fetches = 1;

	N.type = NODE_TYPE_LEAF;
	N.state = NODE_STATE_CLEAN;

	fail_alloc = 1;
	assert(callback_clean(CG, &N) == -1);
	fail_alloc = 0;

	assert(C.pending_cleans == 0);
	assert(C.head == NULL);
	assert(unlock_calls == 1);
	assert(free_calls == 1);

	puts("PASS: callback_clean failure releases pending clean and group");
	return (0);
}
