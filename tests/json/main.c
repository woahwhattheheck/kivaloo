#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

struct testcase {
	const char * json;
	const char * target;
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
	value = json_find(buf, end, t->target);
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

static int
test_reject(const struct testcase * t)
{
	const uint8_t * buf;
	const uint8_t * end;

	buf = (const uint8_t *)t->json;
	end = &buf[strlen(t->json)];
	if (json_find(buf, end, t->target) != end) {
		fprintf(stderr, "%s: malformed object accepted\n", t->description);
		return (-1);
	}

	return (0);
}

static int
test_nul_alias(void)
{
	const char * json = "{\"\\u0000\":3,\"\":4}";
	const uint8_t * buf;
	const uint8_t * end;
	const uint8_t * value;

	buf = (const uint8_t *)json;
	end = &buf[strlen(json)];
	value = json_find(buf, end, "");
	if ((value == end) || (value[0] != '4')) {
		fprintf(stderr, "escaped NUL matched empty key\n");
		return (-1);
	}

	return (0);
}

int
main(void)
{
	static const struct testcase tests[] = {
		{ "{\"prefix\":{\"a\":1, \"b\":2},\"target\":3}", "target",
		    "object space" },
		{ "{\"prefix\":{\"a\":1,\t\"b\":2},\"target\":3}", "target",
		    "object tab" },
		{ "{\"prefix\":{\"a\":1,\r\"b\":2},\"target\":3}", "target",
		    "object carriage return" },
		{ "{\"prefix\":{\"a\":1,\n\"b\":2},\"target\":3}", "target",
		    "object newline" },
		{ "{\"prefix\":[1, 2],\"target\":3}", "target", "array space" },
		{ "{\"prefix\":[1,\t2],\"target\":3}", "target", "array tab" },
		{ "{\"prefix\":[1,\r2],\"target\":3}", "target",
		    "array carriage return" },
		{ "{\"prefix\":[1,\n2],\"target\":3}", "target", "array newline" },
		{ "{\"target\":3,\"after\":{\"a\":1, \"b\":2}}", "target",
		    "valid member after target" },
		{ "{\"target\":3,\"target\":4}", "target", "first target wins" },
		{ "{\"\\u0074arget\":3}", "target", "ASCII unicode escape" },
		{ "{\"caf\\u00e9\":3}", "caf\xc3\xa9", "BMP unicode escape" },
		{ "{\"\\ud83d\\ude80\":3}", "\xf0\x9f\x9a\x80",
		    "surrogate-pair unicode escape" }
	};
	static const struct testcase rejects[] = {
		{ "{\"target\":garbage}", "target", "invalid target value" },
		{ "{\"target\":3,\"after\":}", "target", "invalid member after target" },
		{ "{\"target\":3,\"after\"", "target", "truncated member after target" },
		{ "{\"target\":3,}", "target", "trailing comma after target" }
	};
	size_t i;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (test_lookup(&tests[i]))
			return (1);
	}
	for (i = 0; i < sizeof(rejects) / sizeof(rejects[0]); i++) {
		if (test_reject(&rejects[i]))
			return (1);
	}
	if (test_nul_alias())
		return (1);

	return (0);
}
