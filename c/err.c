#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "err.h"

void
err_append(struct err_set *es, struct err e)
{
	// XXX: this doesn't handle allocation failures.
	if (es->len >= es->cap) {
		// Roughly double the size. Adding 1 to the initial size lets
		// this work when es->cap == 0.
		es->cap = (es->cap + 1) * 2;
		es->e = realloc(es->e, es->cap * sizeof(struct err));
	}

	es->e[es->len++] = e;
}

void
err_disp(const struct err *e)
{
	fprintf(stderr, "\033[1;31mError:\033[0m ");
	switch (e->type) {
	case RE_FNOOPEN:
		fprintf(stderr, "Couldn't open file %s.\033[0m\n", e->data.path);
		fprintf(stderr, "       \033[0;33mDoes it exist with proper permissions?\n");
		break;
	case RE_NOINS:
		fprintf(stderr, "Didn't recognize instruction %s.\033[0m\n", e->data.ins);
		break;
	case RE_NOREG:
		fprintf(stderr, "Didn't recognize register %s.\033[0m\n", e->data.reg);
		break;
	case RE_BADINT:
		fprintf(stderr, "Expected integer, got %s.\033[0m\n", e->data.bint);
		break;
	case RE_NOERR:
		break;
	}

	fprintf(stderr, "\033[0m");
}

void
err_free_asc(struct err *e)
{
	switch (e->type) {
	case RE_NOINS:
		free(e->data.ins);
	case RE_NOREG:
		free(e->data.reg);
	default:
		break;
	}
}
