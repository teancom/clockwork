%{
/**

  grammar.y - Reentrant (pure) Bison LALR Parser

  This file defines the productions necessary to interpret
  tokens found by the lexical analyzer, and subsquently build
  a valid abstract syntax tree to describe policy generators.

 */
#include "private.h"
%}

/*
  To get a reentrant Bison parser, we have to use the special
  '%pure-parser' directive.  Documentation on the 'net seems to
  disagree about whether this should be %pure-parser (with a hyphen)
  or %pure_parser (with an underscore).

  I have found %pure-parser to work just fine.  JRH */
%pure-parser

/* Define the lexical tokens used by the grammar.
      These definitions will be available to the lexer via the
      grammar.h header file, which is generated by bison */ 
%token T_KEYWORD_POLICY
%token T_KEYWORD_IF
%token T_KEYWORD_UNLESS
%token T_KEYWORD_ELSE
%token T_KEYWORD_MAP
%token T_KEYWORD_IS
%token T_KEYWORD_NOT

/* These token definitions identify the expected type of the lvalue.
   The name 'string' comes from the union members of the YYSTYPE
   union, defined in private.h

   N.B.: I deliberately do not use the %union construct provided by
   bison, opting to define the union myself in private.h.  If one of
   the possible lvalue types is not a basic type (like char*, int, etc.)
   then lexer is required to include the necessary header files. */
%token <string> T_IDENTIFIER
%token <string> T_FACT
%token <string> T_QSTRING
%token <string> T_NUMERIC

/* Define the lvalue types of non-terminal productions.
   These definitions are necessary so that the $1..$n and $$ "magical"
   variables work in the generated C code. */
%type <node> policies            /* AST_OP_PROG */
%type <node> policy              /* AST_OP_POLICY */
%type <node> blocks              /* AST_OP_PROG */
%type <node> block               /* depends on type of block */
%type <node> conditional         /* AST_OP_IF_* */
%type <node> resource            /* AST_OP_RES_* */
%type <node> attributes          /* AST_OP_PROG */
%type <node> attribute_spec      /* AST_OP_SET_ATTRIBUTE */

%type <branch> conditional_test

%type <string>  value
%type <strings> value_list
%type <strings> explicit_value_list
%type <string> qstring

%type <map>         conditional_inline
%type <string_hash> mapped_value_set
%type <string_pair> mapped_value
%type <string>      mapped_value_default
%{
/* grammar_impl.c contains several static routines that only make sense
   within the context of a parser.  They deal with interim representations
   of abstract syntax trees, like if branches and map constructs.  They
   exist in a separate C file to keep this file clean and focues. */
#include "grammar_impl.c"
%}

%%

policies:
	{ ((spec_parser_context*)ctx)->root = ast_new(AST_OP_PROG, NULL, NULL); }
	| policies policy
	{ ast_add_child(((spec_parser_context*)ctx)->root, $2); }
	;

policy: T_KEYWORD_POLICY qstring '{' blocks '}'
      { $$ = ast_new(AST_OP_DEFINE_POLICY, $2, NULL);
        ast_add_child($$, $4); }
      ;

blocks:
      { $$ = ast_new(AST_OP_PROG, NULL, NULL); }
      | blocks block
      { ast_add_child($$, $2); }
      ;

block: resource
     | conditional
     ;

resource: T_IDENTIFIER value '{' attributes '}'
	{ $$ = parser_new_resource_node($1, $2);
	  ast_add_child($$, $4); }
	;

attributes:
	  { $$ = ast_new(AST_OP_PROG, NULL, NULL); }
	  | attributes attribute_spec
	  { ast_add_child($$, $2); }
	  ;

attribute_spec: T_IDENTIFIER ':' value
	      { $$ = ast_new(AST_OP_SET_ATTRIBUTE, $1, $3); }
	      | T_IDENTIFIER ':' conditional_inline
	      { $3->attribute = $1;
	        $$ = map_expand_nodes($3); }
	      ;

value: qstring 
     | T_NUMERIC
     ;

conditional: T_KEYWORD_IF '(' conditional_test ')' '{' blocks '}'
	   { branch_connect($3, $6, ast_new(AST_OP_NOOP, NULL,  NULL));
	     $$ = branch_expand_nodes($3); }
	   | T_KEYWORD_IF '(' conditional_test ')' '{' blocks '}' T_KEYWORD_ELSE '{' blocks '}'
	   { branch_connect($3, $6, $10);
	     $$ = branch_expand_nodes($3); }
	   ;

conditional_test: T_FACT T_KEYWORD_IS value_list
		{ $$ = branch_new($1, $3, 1); }
		| T_FACT T_KEYWORD_IS T_KEYWORD_NOT value_list
		{ $$ = branch_new($1, $4, 0); }
		;

value_list : value
	   { $$ = stringlist_new(NULL);
	     stringlist_add($$, $1); }
	   | '[' explicit_value_list ']'
	   { $$ = $2; }
	   ;

explicit_value_list: value
		   { $$ = stringlist_new(NULL);
		     stringlist_add($$, $1); }
		   | explicit_value_list ',' value
		   { stringlist_add($$, $3); }
		   ;

conditional_inline: T_KEYWORD_MAP '(' T_FACT ')' '{' mapped_value_set mapped_value_default '}'
		  { $$ = map_new($3, NULL, $6[0], $6[1], $7); }
		  ;

mapped_value_set:
		{ $$[0] = stringlist_new(NULL);
		  $$[1] = stringlist_new(NULL); }
		| mapped_value_set mapped_value
		{ stringlist_add($$[0], $2[0]);
		  stringlist_add($$[1], $2[1]); }
		;

mapped_value: qstring ':' value
	    { $$[0] = $1;
	      $$[1] = $3; }
	    ;

mapped_value_default:
		    { $$ = NULL }
		    | T_KEYWORD_ELSE ':' value
		    { $$ = $3; }
		    ;

qstring: T_QSTRING
       | T_IDENTIFIER
       { spec_parser_warning(YYPARSE_PARAM, "unexpected identifier '%s', expected quoted string literal", $1); }
       ;