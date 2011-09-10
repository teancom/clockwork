/*
  Copyright 2011 James Hunt <james@jameshunt.us>

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

#ifndef STRING_H
#define STRING_H

#include "clockwork.h"
#include "hash.h"

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

struct string* string_new(const char *str, size_t block);
void string_free(struct string *s);

int string_append(struct string *s, const char *str);
int string_append1(struct string *s, char c);
int string_interpolate(char *buf, size_t len, const char *src, const struct hash *ctx);

#endif
