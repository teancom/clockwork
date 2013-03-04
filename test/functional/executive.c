/*
  Copyright 2011-2013 James Hunt <james@niftylogic.com>

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

#include <stdio.h>

#include "../../exec.h"

/** executive - a test utility

  executive tests the exec submodule to verify that it
  properly runs commands and gathers output from stdout
  and / or stderr.

  Usage is straightforward: the first argument determines
  what output streams are captured.  All remaining arguments
  are interpreted as a single command string to exec via
  `/bin/sh -c`.  All gathered output is printed to stdout,
  prefixed by the source.  If nothing bad happens, executive
  exits 0.
 */

int main(int argc, char **argv)
{
	int rc;
	char *std_out = strdup("<not captured>\n");
	char *std_err = strdup(std_out);

	if (argc != 3) {
		printf("USAGE: executive <mode> \"command --to -run\"\n"
		       "\n"
		       "Where <mode> is one of:"
		       "\n"
		       "  stdout - Capture only standard output\n"
		       "  stderr - Capture only standard error\n"
		       "  both   - stdout + stderr\n"
		       "  none   - Capture nothing\n");
		return 1;
	}

	if (strcmp(argv[1], "stdout") == 0) {
		free(std_out);
		rc = exec_command(argv[2], &std_out, NULL);

	} else if (strcmp(argv[1], "stderr") == 0) {
		free(std_err);
		rc = exec_command(argv[2], NULL, &std_err);

	} else if (strcmp(argv[1], "both") == 0) {
		free(std_out); free(std_err);
		rc = exec_command(argv[2], &std_out, &std_err);

	} else if (strcmp(argv[1], "none") == 0) {
		rc = exec_command(argv[2], NULL, NULL);

	} else {
		printf("INVALID MODE: %s\n", argv[1]);
		return 2;
	}

	if (rc != 0) {
		printf("BAD RETURN CODE: %u\n", rc);
		return 3;
	}
	printf("STDOUT:%sSTDERR:%s", std_out, std_err);
	return 0;
}
