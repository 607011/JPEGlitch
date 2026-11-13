#ifndef JPEGLITCH_TEST_UTIL_H
#define JPEGLITCH_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>

static int jpeglitch_test_failures = 0;

#define CHECK(cond)                                                                         \
	do {                                                                                  \
		if (!(cond)) {                                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			jpeglitch_test_failures++;                                                  \
		}                                                                             \
	} while (0)

#define TEST_EXIT() return jpeglitch_test_failures ? EXIT_FAILURE : EXIT_SUCCESS

#endif
