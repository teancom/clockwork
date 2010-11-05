#ifndef _AST_H
#define _AST_H

#include "policy.h"
#include "res_user.h"
#include "res_group.h"
#include "res_file.h"

#define AST_OP_NOOP                0
#define AST_OP_DEFINE_POLICY       1
#define AST_OP_DEFINE_RESOURCE     2
#define AST_OP_SET_ATTRIBUTE       3
#define AST_OP_IF_EQUAL            4

struct ast {
	unsigned int op;
	void *data1, *data2;

	unsigned int size;
	struct ast **nodes;

	unsigned int refs; /* reference counter; when it reaches 0 we can free the structure */
};

struct ast* ast_new(unsigned int op, void *data1, void *data2);
int ast_init(struct ast *ast, unsigned int op, void *data1, void *data2);
int ast_deinit(struct ast *ast);
void ast_free(struct ast *ast);

int ast_add_child(struct ast *parent, struct ast *child);
int ast_rm_child(struct ast *parent, struct ast *child);

struct policy* ast_define_policy(struct ast *ast, struct list *facts);
struct res_user* ast_define_res_user(struct ast *ast, struct list *facts);
struct res_group* ast_define_res_group(struct ast *ast, struct list *facts);
struct res_file* ast_define_res_file(struct ast *ast, struct list *facts);

#endif