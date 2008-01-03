#ifndef _PMR_SUPPORT_H_
#define _PMR_SUPPORT_H_

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>


#define die(fmt, args...) do { fprintf(stderr, "pmr: " fmt, ## args); exit(1); } while(0)

#define dieerror(fmt, args...) do { fprintf(stderr, "pmr: " fmt ": %s\n", ## args, strerror(errno)); exit(1); } while(0)


#define MIN(x, y) ((x) <= (y) ? (x) : (y))
#define MAX(x, y) ((x) >= (y) ? (x) : (y))


#define darray_append(n, nallocated, array, item) do { \
	assert((n) >= 0); \
	assert((nallocated) >= 0); \
	assert((n) <= (nallocated)); \
					\
	if ((n) == (nallocated)) { \
		if ((nallocated) == 0) \
			(nallocated) = 1; \
		(nallocated) *= 2; \
		assert((nallocated) > 0); \
		(array) = realloc((array), (nallocated) * sizeof((array)[0])); \
		if ((array) == NULL) { \
			die("No memory for darray elements\n"); \
		} \
	} \
		\
	(array)[(n)] = (item); \
	(n)++; \
} while (0)


int skipws(const char *s, int i);
int skipnws(const char *s, int i);

#endif
