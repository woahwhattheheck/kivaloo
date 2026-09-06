/* KEEL: exact-source unit regression for Tarsnap/kivaloo #335.
 * Compile with the selected checkout's kvlds/ and real pool.c. The target
 * file is included unchanged to make its static callbacks testable.
 * No daemon, networking, or persistent database is used.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btree_cleaning.c"

static uintptr_t tracked_group;
static unsigned group_frees;
static int fail_next_malloc;
static unsigned failures;

void *__real_malloc(size_t);
void __real_free(void *);

void *__wrap_malloc(size_t size)
{
    if (fail_next_malloc) {
        fail_next_malloc = 0;
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

void __wrap_free(void *p)
{
    if (p && (uintptr_t)p == tracked_group) {
        group_frees++;
        tracked_group = 0;
    }
    __real_free(p);
}

/* Distinct invariants; the runner pins expected baseline failures. */
enum { COUNT = 1, GROUP = 2, LINKS = 4, LOCK = 8,
       STATUS = 16, RECORD = 32, POSSIBLE = 64 };

static void check(int condition, unsigned bit, const char *label)
{
    if (!condition) {
        failures |= bit;
        fprintf(stderr, "INVARIANT %s\n", label);
    }
}

static struct cleaning_group *new_group(struct cleaner *c, size_t pending)
{
    struct cleaning_group *g = calloc(1, sizeof(*g));
    if (!g)
        exit(70);
    g->C = c;
    g->pending_fetches = pending;
    return g;
}

static void check_group(struct cleaner *c, struct cleaning_group *g,
    struct cleaning_group *left, struct cleaning_group *right,
    size_t remaining, size_t records)
{
    int should_free = remaining == 0 && records == 0;
    check(group_frees == (unsigned)should_free, GROUP, "group lifetime");
    if (should_free) {
        check(c->head == (left ? left : right), LINKS, "list head after unlink");
        if (left)
            check(left->next == right, LINKS, "predecessor successor");
        if (right)
            check(right->prev == left, LINKS, "successor predecessor");
    } else if (!group_frees) {
        check(c->head == (left ? left : g), LINKS, "live group list head");
        check(g->prev == left && g->next == right, LINKS, "live group neighbors");
        if (left)
            check(left->next == g, LINKS, "live predecessor link");
        if (right)
            check(right->prev == g, LINKS, "live successor link");
    }
    if (!group_frees) {
        size_t seen = 0;
        struct cleaning *previous = NULL;
        check(g->pending_fetches == remaining, COUNT, "pending fetch count");
        for (struct cleaning *p = g->head; p; p = p->next) {
            check(p->prev == previous && p->G == g && p->N->v.cstate == p,
                  RECORD, "cleaning record links");
            previous = p;
            if (++seen > 4) {
                check(0, RECORD, "cleaning record cycle");
                break;
            }
        }
        check(seen == records, RECORD, "cleaning record count");
    }
    check(c->pending_cleans == remaining + records, COUNT, "pending clean count");
    check(btree_cleaning_possible(c) == (remaining == 0 && records != 0),
          POSSIBLE, "ready group requires an actual cleaning record");
}

int main(int argc, char **argv)
{
    struct btree tree = {0};
    struct cleaner cleaner = {0};
    struct cleaning_group *group, *left = NULL, *right = NULL;
    struct node *nodes[4] = {0};
    size_t persistent_locks[4] = {0};
    size_t count, records = 0;
    const char *ops;
    int position;
    size_t observed_pending;
    unsigned observed_frees;

    /* C=clean success, D=dirty, S=shadow, F=one allocation failure.
     * position: 0=only group, 1=head, 2=middle, 3=tail. */
    if (argc != 3 || (count = strlen(argv[1])) < 1 || count > 4)
        return 64;
    ops = argv[1];
    position = atoi(argv[2]);
    if (position < 0 || position > 3 || strspn(ops, "CDSF") != count)
        return 64;
    if (!(tree.P = pool_init(16, offsetof(struct node, pool_cookie))))
        return 70;
    cleaner.T = &tree;
    cleaner.pending_cleans = count;
    group = new_group(&cleaner, count);
    if (position == 2 || position == 3)
        left = new_group(&cleaner, 1);
    if (position == 1 || position == 2)
        right = new_group(&cleaner, 1);
    group->prev = left;
    group->next = right;
    if (left)
        left->next = group;
    if (right)
        right->prev = group;
    cleaner.head = left ? left : group;
    tracked_group = (uintptr_t)group;

    for (size_t i = 0; i < count; i++) {
        void *evicted = NULL;
        if (!(nodes[i] = calloc(1, sizeof(*nodes[i]))))
            return 70;
        nodes[i]->type = NODE_TYPE_LEAF;
        nodes[i]->root = 1;
        nodes[i]->state = ops[i] == 'D' ? NODE_STATE_DIRTY :
                         ops[i] == 'S' ? NODE_STATE_SHADOW : NODE_STATE_CLEAN;
        /* Retain permanent root/state pins in addition to the callback pin.
         * These are isolated leaf fixtures, not a fabricated full B+Tree. */
        persistent_locks[i] = 1 + (nodes[i]->state != NODE_STATE_CLEAN);
        if (pool_rec_add(tree.P, nodes[i], &evicted) || evicted)
            return 70;
        for (size_t j = 0; j < persistent_locks[i]; j++)
            pool_rec_lock(tree.P, nodes[i]);
    }

    for (size_t i = 0; i < count; i++) {
        int rc;
        check(!group_frees, GROUP, "group alive before pending callback");
        if (group_frees)
            return 71;
        fail_next_malloc = ops[i] == 'F';
        rc = callback_clean(group, nodes[i]);
        if (group_frees)
            group = NULL;
        check(rc == (ops[i] == 'F' ? -1 : 0), STATUS, "callback status");
        check(!fail_next_malloc, STATUS, "allocation fault was consumed");
        if (ops[i] == 'C')
            records++;
        check(pool_rec_lockcount(tree.P, nodes[i]) ==
              persistent_locks[i] + (ops[i] == 'C'), LOCK, "callback lock balance");
        check((nodes[i]->v.cstate != NULL) == (ops[i] == 'C'), RECORD, "node cleaning state");
        check_group(&cleaner, group, left, right, count - i - 1, records);
    }

    /* Remove acquired records in callback order, i.e. reverse stack order;
     * exercise non-head removal as well as final-record removal. */
    for (size_t i = 0; i < count; i++) {
        if (ops[i] != 'C')
            continue;
        free_cstate(nodes[i]->v.cstate);
        if (group_frees)
            group = NULL;
        records--;
        check(nodes[i]->v.cstate == NULL, RECORD, "node state cleared");
        check(pool_rec_lockcount(tree.P, nodes[i]) == persistent_locks[i],
              LOCK, "record-release lock balance");
        check_group(&cleaner, group, left, right, 0, records);
    }
    observed_pending = cleaner.pending_cleans;
    observed_frees = group_frees;
    printf("{\"operations\":\"%s\",\"position\":%d,\"failure_mask\":%u,"
           "\"pending_cleans\":%zu,\"target_group_frees\":%u}\n",
           ops, position, failures, observed_pending, observed_frees);

    /* Test-only teardown AFTER all measured observations. Baseline's leaked
     * empty group is explicitly reclaimed here; this is not counted as a
     * production fix or hidden by sanitizer leak suppression. */
    tracked_group = 0;
    while (cleaner.head) {
        struct cleaning_group *next = cleaner.head->next;
        free(cleaner.head);
        cleaner.head = next;
    }
    for (size_t i = 0; i < count; i++) {
        while (pool_rec_lockcount(tree.P, nodes[i]) > 1)
            pool_rec_unlock(tree.P, nodes[i]);
        pool_rec_free(tree.P, nodes[i]);
        free(nodes[i]);
    }
    pool_free(tree.P);
    return failures ? 2 : 0;
}
