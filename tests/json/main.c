#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

struct testcase {
	const char * json;
	const char * description;
};

struct named_testcase {
	const char * json;
	const char * target;
	char expected;
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

static int
test_named_lookup(const struct named_testcase * t)
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
	if ((value >= end) || (value[0] != (uint8_t)t->expected)) {
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
	if (json_find(buf, end, "target") != end) {
		fprintf(stderr, "%s: malformed object accepted\n", t->description);
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
		{ "{\"prefix\":[1,\n2],\"target\":3}", "array newline" },
		{ "{\"prefix\":-0.5e+2,\"target\":3}",
		    "valid strict number" },
		{ "{\"prefix\":\"line\\n\\u0041\",\"target\":3}",
		    "valid escaped string" },
		{ "{\"target\":3,\"after\":{\"a\":1, \"b\":2}}",
		    "valid member after target" },
		{ "{\"target\":3,\"target\":4}", "first target wins" }
	};
	static const struct named_testcase named_tests[] = {
		{ "{\"\\u0074arget\":3}", "target", '3', "ASCII unicode escape" },
		{ "{\"caf\\u00e9\":3}", "caf\xc3\xa9", '3', "BMP unicode escape" },
		{ "{\"\\ud83d\\ude80\":3}", "\xf0\x9f\x9a\x80", '3',
		    "surrogate-pair unicode escape" },
		{ "{\"\\u0000\":3,\"\":4}", "", '4',
		    "escaped NUL does not alias empty key" }
	};
	static const struct testcase rejects[] = {
		{ "{\"target\":garbage}", "invalid target value" },
		{ "{\"target\":+}", "bare plus target" },
		{ "{\"target\":01}", "leading zero target" },
		{ "{\"target\":1.}", "missing fraction digit" },
		{ "{\"target\":1e}", "missing exponent digit" },
		{ "{\"target\":\"\\q\"}", "invalid string escape" },
		{ "{\"target\":\"\\u12xz\"}", "invalid unicode escape" },
		{ "{\"target\":\"bad\nstring\"}", "raw string newline" },
		{ "{\"target\":3,\"after\":+}", "invalid trailing number" },
		{ "{\"target\":3,\"after\":{bad\":1}}",
		    "invalid nested object key" },
		{ "{\"target\":3,\"after\":}", "invalid member after target" },
		{ "{\"target\":3,\"after\"", "truncated member after target" },
		{ "{\"target\":3,}", "trailing comma after target" },
		{ "{\"\\uZZZZ\":0,\"target\":3}",
		    "invalid unicode escape before target" }
	};
	size_t i;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (test_lookup(&tests[i]))
			return (1);
	}
	for (i = 0; i < sizeof(named_tests) / sizeof(named_tests[0]); i++) {
		if (test_named_lookup(&named_tests[i]))
			return (1);
	}
	for (i = 0; i < sizeof(rejects) / sizeof(rejects[0]); i++) {
		if (test_reject(&rejects[i]))
			return (1);
	}

	return (0);
}
