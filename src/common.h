#ifndef COMMON_H
#define COMMON_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int fd_t;
typedef unsigned char byte_t;
typedef char str_t;
typedef str_t const *strarg_t; // A string that belongs to someone else.
typedef signed char bool_t;
typedef off_t index_t;
typedef size_t count_t;
typedef int err_t;

#define numberof(x) (sizeof(x) / sizeof(*x))

// TODO: Where are these officially defined?
#define MIN(a, b) ({ \
	__typeof__(a) const __a = (a); \
	__typeof__(b) const __b = (b); \
	__a < __b ? __a : __b; \
})
#define MAX(a, b) ({ \
	__typeof__(a) const __a = (a); \
	__typeof__(b) const __b = (b); \
	__a > __b ? __a : __b; \
})

#define SUB_ZERO(a, b) ({ \
	__typeof__(a) const __a = (a); \
	__typeof__(b) const __b = (b); \
	__a > __b ? __a - __b : 0; \
})

#ifdef NDEBUG
#define assertf(x, fmt, ...) ((void)(x))
#else
#define assertf(x, fmt, ...) ({ \
	if(0 == (x)) { \
		fprintf(stderr, "%s:%d %s: assertion '%s' failed\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, #x); \
		fprintf(stderr, fmt, ##__VA_ARGS__); \
		fprintf(stderr, "\n"); \
		abort(); \
	} \
})
#endif

#define FREE(ptrptr) ({ \
	__typeof__(ptrptr) const __x = (ptrptr); \
	free(*__x); \
	*__x = NULL; \
})

// TODO: Log the query string if prepare fails.
#define QUERY(db, str) ({ \
	sqlite3_stmt *__stmt = NULL; \
	str_t const __str[] = (str); \
	int const __err = sqlite3_prepare_v2((db), __str, sizeof(__str), &__stmt, NULL); \
	if(SQLITE_OK != __err && SQLITE_DONE != __err && SQLITE_ROW != __err) fprintf(stderr, "%s:%d %s: %s (%d)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, sqlite3_errstr(__err), __err); \
	__stmt; \
})
#define STEP(stmt) ({ \
	int const __err = sqlite3_step(stmt); \
	if(SQLITE_OK != __err && SQLITE_DONE != __err && SQLITE_ROW != __err) fprintf(stderr, "%s:%d %s: %s (%d)\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, sqlite3_errstr(__err), __err); \
	__err; \
})
#define EXEC(stmt) ({ \
	sqlite3_stmt *const __stmt = (stmt); \
	int const status = STEP(__stmt); \
	(void)sqlite3_finalize(__stmt); \
	status; \
})

// Compares nul-terminated string `a` with substring of `blen` at `b`.
static bool_t substr(strarg_t const a, strarg_t const b, size_t const blen) {
	off_t i = 0;
	for(; i < blen; ++i) {
		if(a[i] != b[i]) return false;
		if(!a[i]) return false; // Terminated early.
	}
	if(a[i]) return false; // Terminated late.
	return true;
}
// Returns strlen(a) if `b` starts with `a`, otherwise 0.
static size_t prefix(strarg_t const a, strarg_t const b) {
	for(off_t i = 0; ; ++i) {
		if(!a[i]) return i;
		if(a[i] != b[i]) return 0;
	}
}

#endif
