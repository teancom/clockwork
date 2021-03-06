/*
  Copyright 2011-2014 James Hunt <james@jameshunt.us>

  This file is part of Clockwork.

  Clockwork is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Clockwork is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Clockwork.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LIB_GEAR_H
#define _LIB_GEAR_H

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h> /* for offsetof(3) macro */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include "../cw.h"

/**
  Variable-length Character String
 */
struct string {
	size_t len;     /* length of string in characters */
	size_t bytes;   /* length of buffer (string->raw) in bytes */
	size_t blk;     /* block size for buffer expansion */

	char *raw;      /* the string buffer */
	char *p;        /* internal pointer to NULL-terminator of raw */
};

/**
  A String List

  A stringlist is a specialized data structure designed for
  efficient storage and management of a list of strings.  It
  wraps around the C representation as a NULL-terminated array
  of character pointers, and extends the idiom with memory
  management methods and "standard" string list manipulation
  routines.

  ### Basic Usage ############################################

  Stringlist allocation and de-allocation is performed through
  the @stringlist_new and @stringlist_free functions.  These two
  functions allocate memory from the heap, and then free that
  memory.

  <code>
  // allocate a new, empty stringlist
  struct stringlist *lst = stringlist_new(NULL);

  // do something interesting with the list...
  stringlist_add(fruit, "pear");
  stringlist_add(fruit, "apple");
  stringlist_add(fruit, "cantaloupe");

  // free memory allocated by stringlist_new and stringlist_add
  stringlist_free(lst);
  </code>

  The first argument to @stringlist_new is a C-style `char**`
  string array, and can be used to "seed" the stringlist with
  starting values.  For example, to deal with command-line
  arguments in main() as a stringlist:

  <code>
  int main(int argc, char **argv)
  {
    struct stringlist *args = stringlist_new(argv)

    // ....
  }
  </code>

  It is not possible to set up a stringlist allocated on the stack.
 */
struct stringlist {
	size_t   num;      /* number of actual strings */
	size_t   len;      /* number of memory slots for strings */
	char   **strings;  /* array of NULL-terminated strings */
};

#define SPLIT_NORMAL  0x00
#define SPLIT_GREEDY  0x01

/**
  Callback function signature for sort comparisons.

  An sl_comparator function takes two arguments (adjacent strings
  in a stringlist) and returns an integer less than or greater than
  zero if the first value should come before or after the second.
 */
typedef int (*sl_comparator)(const void*, const void*);

struct path {
	char     *buf;
	ssize_t   n;
	size_t    len;
};

struct string* string_new(const char *str, size_t block);
void string_free(struct string *s);

int string_append(struct string *s, const char *str);
int string_append1(struct string *s, char c);
int string_interpolate(char *buf, size_t len, const char *src, const cw_hash_t *ctx);

#define for_each_string(l,i) for ((i)=0; (i)<(l)->num; (i)++)

int STRINGLIST_SORT_ASC(const void *a, const void *b);
int STRINGLIST_SORT_DESC(const void *a, const void *b);

struct stringlist* stringlist_new(char** src);
struct stringlist* stringlist_dup(struct stringlist *orig);
void stringlist_free(struct stringlist *list);
void stringlist_sort(struct stringlist *list, sl_comparator cmp);
void stringlist_uniq(struct stringlist *list);
int stringlist_search(const struct stringlist *list, const char *needle);
int stringlist_add(struct stringlist *list, const char *value);
int stringlist_add_all(struct stringlist *dst, const struct stringlist *src);
int stringlist_remove(struct stringlist *list, const char *value);
int stringlist_remove_all(struct stringlist *dst, struct stringlist *src);
struct stringlist* stringlist_intersect(const struct stringlist *a, const struct stringlist *b);
int stringlist_diff(struct stringlist *a, struct stringlist *b);
char* stringlist_join(struct stringlist *list, const char *delim);
struct stringlist* stringlist_split(const char *str, size_t len, const char *delim, int opt);

struct path* path_new(const char *path);
void path_free(struct path *path);
const char *path(struct path *path);
int path_canon(struct path *path);
int path_push(struct path *path);
int path_pop(struct path *path);

#endif
