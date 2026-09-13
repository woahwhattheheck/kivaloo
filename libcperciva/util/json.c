#include <stdint.h>
#include <string.h>

#include "json.h"

static const uint8_t * skip_value(const uint8_t *, const uint8_t *);

/* Advance past whitespace, if any. */
static const uint8_t *
skip_ws(const uint8_t * buf, const uint8_t * end)
{

	/* Skip " \t\r\n". */
	while (buf < end) {
		if ((buf[0] != 0x09) && (buf[0] != 0x0A) &&
		    (buf[0] != 0x0D) && (buf[0] != 0x20))
			break;
		buf++;
	}

	/* Return the first non-whitespace character or the buffer end. */
	return (buf);
}

/* Is this a hexadecimal digit? */
static int
is_hex(uint8_t ch)
{

	return (((ch >= '0') && (ch <= '9')) ||
	    ((ch >= 'A') && (ch <= 'F')) ||
	    ((ch >= 'a') && (ch <= 'f')));
}

/* Advance past literal. */
static const uint8_t *
skip_literal(const uint8_t * buf, const uint8_t * end)
{

	/* MUST be "false", "null", or "true". */
	if (((end - buf) >= 5) && (memcmp(buf, "false", 5) == 0))
		return (&buf[5]);
	if (((end - buf) >= 4) && (memcmp(buf, "null", 4) == 0))
		return (&buf[4]);
	if (((end - buf) >= 4) && (memcmp(buf, "true", 4) == 0))
		return (&buf[4]);

	/* We do not have a literal here; return a pointer to the end. */
	return (end);
}

/* Advance past string. */
static const uint8_t *
skip_string(const uint8_t * buf, const uint8_t * end)
{
	uint8_t ch;
	int i;

	/* A JSON string must start with a quote. */
	if ((buf == end) || (buf[0] != '"'))
		return (end);
	buf++;

	/* Scan until we find a valid terminating quote. */
	while (buf < end) {
		ch = *buf++;
		if (ch == '"')
			return (buf);

		/* Unescaped control characters are not valid JSON string data. */
		if (ch < 0x20)
			return (end);

		/* Validate escape sequences. */
		if (ch == '\\') {
			if (buf == end)
				return (end);
			ch = *buf++;
			switch (ch) {
			case '"':
			case '\\':
			case '/':
			case 'b':
			case 'f':
			case 'n':
			case 'r':
			case 't':
				break;
			case 'u':
				if (end - buf < 4)
					return (end);
				for (i = 0; i < 4; i++) {
					if (!is_hex(buf[i]))
						return (end);
				}
				buf += 4;
				break;
			default:
				return (end);
			}
		}
	}

	/* Unterminated string. */
	return (end);
}

/* Advance past number. */
static const uint8_t *
skip_number(const uint8_t * buf, const uint8_t * end)
{

	/* Optional minus sign. */
	if ((buf < end) && (buf[0] == '-'))
		buf++;
	if (buf == end)
		return (end);

	/* Integer component: zero, or a non-zero digit followed by digits. */
	if (buf[0] == '0') {
		buf++;
		if ((buf < end) && (buf[0] >= '0') && (buf[0] <= '9'))
			return (end);
	} else if ((buf[0] >= '1') && (buf[0] <= '9')) {
		do {
			buf++;
		} while ((buf < end) && (buf[0] >= '0') &&
		    (buf[0] <= '9'));
	} else {
		return (end);
	}

	/* Optional fractional component requires at least one digit. */
	if ((buf < end) && (buf[0] == '.')) {
		buf++;
		if ((buf == end) || (buf[0] < '0') || (buf[0] > '9'))
			return (end);
		do {
			buf++;
		} while ((buf < end) && (buf[0] >= '0') &&
		    (buf[0] <= '9'));
	}

	/* Optional exponent requires at least one digit. */
	if ((buf < end) && ((buf[0] == 'e') || (buf[0] == 'E'))) {
		buf++;
		if ((buf < end) && ((buf[0] == '+') || (buf[0] == '-')))
			buf++;
		if ((buf == end) || (buf[0] < '0') || (buf[0] > '9'))
			return (end);
		do {
			buf++;
		} while ((buf < end) && (buf[0] >= '0') &&
		    (buf[0] <= '9'));
	}

	return (buf);
}

/* Advance past array. */
static const uint8_t *
skip_array(const uint8_t * buf, const uint8_t * end)
{

	/* Advance past the opening '[' and following whitespace. */
	buf++;
	buf = skip_ws(buf, end);

	/* Is this an empty array? */
	if (buf == end)
		return (end);
	if (buf[0] == ']')
		return (&buf[1]);

	/* Skip entries until we get to the end. */
	do {
		/* Skip a value. */
		buf = skip_value(buf, end);

		/* Skip optional whitespace. */
		buf = skip_ws(buf, end);

		/* Are we at the end? */
		if (buf == end)
			return (end);
		if (buf[0] == ']')
			return (&buf[1]);

		/* Otherwise we should have a comma. */
		if (*buf++ != ',')
			return (end);

		/* Skip optional whitespace before the next value. */
		buf = skip_ws(buf, end);
	} while (1);

	/* NOTREACHED */
}

/* Advance past object. */
static const uint8_t *
skip_object(const uint8_t * buf, const uint8_t * end)
{

	/* Advance past the opening '{' and following whitespace. */
	buf++;
	buf = skip_ws(buf, end);

	/* Is this an empty object? */
	if (buf == end)
		return (end);
	if (buf[0] == '}')
		return (&buf[1]);

	/* Skip entries until we get to the end. */
	do {
		/* Skip a string and optional whitespace. */
		buf = skip_string(buf, end);
		buf = skip_ws(buf, end);

		/* We should have a colon next. */
		if (buf == end)
			return (end);
		if (*buf++ != ':')
			return (end);

		/* Skip a whitespace, a value, and more whitespace. */
		buf = skip_ws(buf, end);
		buf = skip_value(buf, end);
		buf = skip_ws(buf, end);

		/* Are we at the end? */
		if (buf == end)
			return (end);
		if (buf[0] == '}')
			return (&buf[1]);

		/* Otherwise we should have a comma. */
		if (*buf++ != ',')
			return (end);

		/* Skip optional whitespace before the next key. */
		buf = skip_ws(buf, end);
	} while (1);

	/* NOTREACHED */
}

/* Advance past a JSON value. */
static const uint8_t *
skip_value(const uint8_t * buf, const uint8_t * end)
{

	/* If there's nothing here, return. */
	if (buf == end)
		return (end);

	/* Handle different types of objects. */
	switch (buf[0]) {
	case 'f':
	case 'n':
	case 't':
		/* This must be a literal.  Skip it. */
		return (skip_literal(buf, end));
	case '"':
		/* This must be a string.  Skip it. */
		return (skip_string(buf, end));
	case '[':
		/* This must be an array.  Skip it. */
		return (skip_array(buf, end));
	case '{':
		/* This must be an object.  Skip it. */
		return (skip_object(buf, end));
	default:
		/* A JSON number must start with '-' or a decimal digit. */
		if ((buf[0] == '-') ||
		    ((buf[0] >= '0') && (buf[0] <= '9')))
			return (skip_number(buf, end));

		/* We don't have a valid JSON value.  Return. */
		return (end);
	}
}

/* Advance to the end of the string.  Check if it matches. */
static const uint8_t *
match_str(const uint8_t * buf, const uint8_t * end, const char * s,
    int * foundit)
{
	char ch;

	/* The string matches... unless we notice that it doesn't match. */
	*foundit = 1;

	/* Scan through the string recording if it ever doesn't match. */
	do {
		if (buf == end)
			return (end);
		ch = (char)(*buf++);

		/* Have we hit the end of the string? */
		if (ch == '"') {
			if (s[0] != '\0')
				*foundit = 0;
			return (buf);
		}

		/* Escape character? */
		if (ch == '\\') {
			if (buf == end)
				return (end);
			switch (*buf++) {
			case '"':
				ch = '"';
				break;
			case '\\':
				ch = '\\';
				break;
			case '/':
				ch = '/';
				break;
			case 'b':
				ch = 0x08;
				break;
			case 'f':
				ch = 0x0C;
				break;
			case 'n':
				ch = 0x0A;
				break;
			case 'r':
				ch = 0x0D;
				break;
			case 't':
				ch = 0x09;
				break;
			case 'u':
				if (end - buf < 4)
					return (end);
				*foundit = 0;	/* Assume non-matching. */
				buf += 4;
				break;
			default:
				/* Invalid JSON. */
				*foundit = 0;
				return (end);
			}
		}

		/* Did this character match? */
		if (ch != s[0])
			*foundit = 0;

		/* Advance in the target if we haven't hit the end. */
		if (*s)
			s++;
	} while (1);

	/* NOTREACHED */
}

/* Helper for scanning for a character. */
#define SCAN(buf, end, ch) do {			\
	buf = skip_ws(buf, end);		\
	if (buf == end)				\
		return (end);			\
	if (*buf++ != ch)			\
		return (end);			\
} while (0)

/**
 * json_find(buf, end, s):
 * If there is a valid JSON object which starts at ${buf} and ends before or
 * at ${end} and said object contains a name/value pair with name ${s},
 * return a pointer to the associated value.  Otherwise, return ${end}.
 */
const uint8_t *
json_find(const uint8_t * buf, const uint8_t * end, const char * s)
{
	const uint8_t * value = end;
	int foundit;

	/* After optional whitespace there should be a '{'. */
	SCAN(buf, end, '{');

	/* An empty object cannot contain the requested name. */
	buf = skip_ws(buf, end);
	if (buf == end)
		return (end);
	if (buf[0] == '}')
		return (end);

	/* Scan the entire object, remembering the first matching value. */
	do {
		/* The next member must begin with a valid JSON string key. */
		if ((buf[0] != '"') || (skip_string(buf, end) == end))
			return (end);
		buf++;

		/* Is this the string we want? */
		buf = match_str(buf, end, s, &foundit);

		/* After optional whitespace we should have a ':'. */
		SCAN(buf, end, ':');

		/* Skip whitespace looking for the associated value. */
		buf = skip_ws(buf, end);

		/* Remember the first matching value, but keep validating. */
		if (foundit && (value == end))
			value = buf;

		/* Skip the value and any whitespace which follows it. */
		buf = skip_value(buf, end);
		buf = skip_ws(buf, end);
		if (buf == end)
			return (end);

		/* A closing brace completes a structurally valid object. */
		if (buf[0] == '}')
			return (value);

		/* Otherwise another member must follow a comma. */
		if (*buf++ != ',')
			return (end);
		buf = skip_ws(buf, end);
		if (buf == end)
			return (end);
	} while (1);

	/* NOTREACHED */
}
