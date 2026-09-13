#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

struct testcase {
	const char * json;
	const char * description;
};

static int
test_lookup(const struct testcase * t)
{
	const uint8_t * buf;
	const uint8_t * end;
	const uint8_t * value;

	buf = (const uint8_t *)t->json;
	end = &buf[strlen(t->json)];
	value = json_find(buf, end, "target");
	if (value == end) {
		fprintf(stderr, "%s: target not found\n", t->description);
		return (-1);
	}
	if ((value >= end) || (value[0] != '3')) {
		fprintf(stderr, "%s: wrong target value\n", t->description);
		return (-1);
	}

	return (0);
}

int
main(void)
{
	static const struct testcase tests[] = {
		{ "{\"prefix\":{\"a\":1, \"b\":2},\"target\":3}",
		    "object space" },
		{ "{\"prefix\":{\"a\":1,\t\"b\":2},\"target\":3}",
		    "object tab" },
		{ "{\"prefix\":{\"a\":1,\r\"b\":2},\"target\":3}",
		    "object carriage return" },
		{ "{\"prefix\":{\"a\":1,\n\"b\":2},\"target\":3}",
		    "object newline" },
		{ "{\"prefix\":[1, 2],\"target\":3}", "array space" },
		{ "{\"prefix\":[1,\t2],\"target\":3}", "array tab" },
		{ "{\"prefix\":[1,\r2],\"target\":3}",
		    "array carriage return" },
		{ "{\"prefix\":[1,\n2],\"target\":3}", "array newline" }
	};
	size_t i;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (test_lookup(&tests[i]))
			return (1);
	}

	return (0);
}
