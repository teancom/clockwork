#ifndef _SPEC_PARSER_H
#define _SPEC_PARSER_H

/* Parse contents of a file and return the top-level
   AST_OP_PROG node that contains all policy definitions */
struct template* parse_template(const char *path);

#endif