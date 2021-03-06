#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../err.h"
#include "asm.h"
#include "../ins.h"
#include "../register.h"
#include "../symtab.h"

#define INIT_BUF_SIZE 64

static void asmf(struct asm_unit *, FILE *, struct err_set *, const char *);

/* Consume the first instruction read into the given gen_ins struct pointer.
 * Errors are added to given error set. If no errors occured, 0 is returned,
 * otherwise, a non-zero value is returned. Additionally, a template error is
 * passed, which is cloned and overwritten for each error. (Useful for recording
 * line numbers, file paths.) */
static int read_ins(char *, struct ins *, struct err_set *, struct err);
static int read_directive(char *, struct ins *, struct err_set *, struct err);

static char *read_reg(char *, unsigned char *, char *, int, struct err_set *, struct err);
static char *read_dest(char *, struct dest *, struct err_set *, struct err);
static char *read_imdte(char *, long *, char, struct err_set *, struct err);
static char *read_cond(char *, unsigned char *, const char *, struct err_set *);

static char *read_reg_pair(char *, unsigned char *, struct err_set *, struct err);

static char *consume_whitespace(char *);
static void strip_comment(char *);

struct asm_unit *
asm_unit_parse(FILE *restrict f, struct err_set *es, const char *path)
{
	struct asm_unit *x = NULL;

	x = malloc(sizeof(struct asm_unit));
	x->cap = INIT_BUF_SIZE;
	x->ins = malloc(x->cap * sizeof(struct ins));
	x->len = 0;
	x->path = path;

	asmf(x, f, es, path);

	return x;
}

void
asm_unit_write(FILE *restrict o, const struct asm_unit *restrict u)
{
	struct ins *x;
	struct gen_ins *g;
	struct ctf_ins *c;
	char *pad = ""; // a singe 0 byte.
	long written = 0;
	long a, aln;

	for (size_t i = 0; i < u->len; i++) {
		x = &u->ins[i];
		switch (x->type) {
		case I_GEN:
			g = &x->data.gen;
			fwrite(&g->op, 1, 1, o);
			fwrite(&g->reg, 1, 1, o);
			fwrite(&g->imdte, 8, 1, o);
			written += 10;
			break;
		case I_CTF:
			c = &x->data.ctf;
			fwrite(&c->op, 1, 1, o);
			fwrite(&c->dest.adr, 8, 1, o);

			// Pad to 10 bytes.
			fwrite(pad, 1, 1, o);
			written += 10;
			break;
		case I_DIR:
			switch (x->data.dir.dir) {
			case DIR_ALN:
				a = x->data.dir.x;
				aln = (written / a) * a + a - written;
				for (long i = 0; i < aln; i++)
					fwrite(pad, 1, 1, o);

				written += aln;
				break;
			case DIR_POS:
				if (written > x->data.dir.x)
					break;

				for (long i = written; i < x->data.dir.x; i++)
					fwrite(pad, 1, 1, o);

				written += x->data.dir.x - written;

				break;
			case DIR_QUA:
				fwrite(&x->data.dir.x, 8, 1, o);

				// Fill out the remaining 2 bytes to pad to 10.
				fwrite(pad, 1, 1, o);
				fwrite(pad, 1, 1, o);
				written += 10;
				break;
			}

			break;
		}
	}
}

static void
asmf(struct asm_unit *u, FILE *f, struct err_set *es, const char *path)
{
	struct ins g;
	char *ln, *in;
	ssize_t l;
	size_t c;
	long addr;
	struct err e;

	c = l = 0;
	ln = NULL;

	e.ln = 0;
	e.path = path;

	u->st = NULL;

	addr = 0;
	while ((l = getline(&ln, &c, f)) > 0) {
		e.ln++;

		ln[l - 1] = '\0'; // null-terminate where newline is
		if (ln[0] == '\0') continue;

		in = consume_whitespace(ln);
		strip_comment(in);
		l = strlen(in);

		g.ln = e.ln;

		if (ln[0] == '.') {
			if (read_directive(ln + 1, &g, es, e) != 0)
				continue;
		} else if (!isdigit(ln[0]) && ln[l - 1] == ':') {
			// We found a label. Let's add its address to the symbol
			// table.
			ln[l - 1] = '\0';
			u->st = st_append(u->st, ln, addr);
			continue;
		} else {
			addr += 10;
			if (read_ins(in, &g, es, e) != 0)
				continue;
		}

		if (u->len + 1 >= u->cap) {
			u->cap *= 2;
			u->ins = realloc(u->ins,
				u->cap * sizeof(struct ins));
		}

		// Might as well blow up here if allocation failed.
		u->ins[u->len++] = g;
	}

	if (ln) free(ln);
}

static int
read_ins(char *in, struct ins *out, struct err_set *es, struct err e)
{
	size_t oplen = 0;
	int ret = 0;

	out->type = I_GEN;
	out->data.gen.op = 0;
	out->data.gen.reg = 0x00;
	out->data.gen.imdte = 0;

	for (; in[oplen] != '\0' && !isspace(in[oplen]); oplen++);

	if (strncmp(in, "hlt", oplen) == 0) {
		out->data.gen.op = O_HLT;
	}

	else if (strncmp(in, "nop", oplen) == 0) {
		out->data.gen.op = O_NOP;
	}

	else if (strncmp(in, "ret", oplen) == 0) {
		out->data.gen.op = O_RET;
	}

	else if (strncmp(in, "rrmovq", oplen) == 0) {
		out->data.gen.op = O_RRM;
		read_reg_pair(in + 6, &out->data.gen.reg, es, e);
	}

	else if (strncmp(in, "irmovq", oplen) == 0) {
		out->data.gen.op = O_IRM;
		in = read_imdte(in + 6, &out->data.gen.imdte, ',', es, e);
		out->data.gen.reg = RNONE << 4;
		read_reg(in, &out->data.gen.reg, "", 0, es, e);
	}

	else if (strncmp(in, "rmmovq", oplen) == 0) {
		out->data.gen.op = O_RMM;
		in = read_reg(in + 6, &out->data.gen.reg, ",", 1, es, e);
		in = read_imdte(in, &out->data.gen.imdte, '(', es, e);
		in = read_reg(in, &out->data.gen.reg, ")", 0, es, e);
	}

	else if (strncmp(in, "mrmovq", oplen) == 0) {
		out->data.gen.op = O_MRM;
		in = read_imdte(in + 6, &out->data.gen.imdte, '(', es, e);
		in = read_reg(in, &out->data.gen.reg, "),", 1, es, e);
		in = read_reg(in, &out->data.gen.reg, "\0", 0, es, e);
	}

	else if (strncmp(in, "addq", oplen) == 0) {
		out->data.gen.op = O_ART | A_ADD;
		read_reg_pair(in + 4, &out->data.gen.reg, es, e);
	}

	else if (strncmp(in, "subq", oplen) == 0) {
		out->data.gen.op = O_ART | A_SUB;
		read_reg_pair(in + 4, &out->data.gen.reg, es, e);
	}

	else if (strncmp(in, "andq", oplen) == 0) {
		out->data.gen.op = O_ART | A_AND;
		read_reg_pair(in + 4, &out->data.gen.reg, es, e);
	}

	else if (strncmp(in, "xorq", oplen) == 0) {
		out->data.gen.op = O_ART | A_XOR;
		read_reg_pair(in + 4, &out->data.gen.reg, es, e);
	}

	else if (in[0] == 'j') {
		out->type = I_CTF;
		out->data.ctf.op = O_JMP;
		in = read_cond(in + 1, &out->data.ctf.op, "mp", es);
		in = read_dest(in, &out->data.ctf.dest, es, e);
	}

	else if (strncmp(in, "call", oplen) == 0) {
		out->type = I_CTF;
		out->data.ctf.op = O_CLL;
		in = read_dest(in + 4, &out->data.ctf.dest, es, e);
	}

	else if (strncmp(in, "cmov", oplen - 2) == 0) {
		out->type = I_GEN;
		out->data.gen.op = O_CMV;
		in = read_cond(in + 4, &out->data.gen.op, NULL, es);
		read_reg_pair(in, &out->data.gen.reg, es, e);
	}

	else if (strncmp(in, "pushq", oplen) == 0) {
		out->type = I_GEN;
		out->data.gen.op = O_PSH;
		out->data.gen.reg = 0x0f;
		read_reg(in + 5, &out->data.gen.reg, "", 1, es, e);
	}

	else if (strncmp(in, "popq", oplen) == 0) {
		out->type = I_GEN;
		out->data.gen.op = O_POP;
		out->data.gen.reg = 0x0f;
		read_reg(in + 4, &out->data.gen.reg, "", 1, es, e);
	}

	else {
		e.type = RE_NOINS;
		e.data.ins = strndup(in, oplen);
		err_append(es, e);
		ret = 1;
	}

	return ret;
}

static int
read_directive(char *in, struct ins *out, struct err_set *es, struct err e)
{
	out->type = I_DIR;
	out->data.dir.x = 0;

	if (strncmp(in, "align", 5) == 0) {
		out->data.dir.dir = DIR_ALN;
		read_imdte(in + 5, &out->data.dir.x, '\0', es, e);
		return 0;
	} else if (strncmp(in, "pos", 3) == 0) {
		out->data.dir.dir = DIR_POS;
		read_imdte(in + 3, &out->data.dir.x, '\0', es, e);
		return 0;
	} else if (strncmp(in, "long", 4) == 0 || strncmp(in, "quad", 4) == 0) {
		out->data.dir.dir = DIR_QUA;
		read_imdte(in + 4, &out->data.dir.x, '\0', es, e);
		return 0;
	} else
		return 1;
}

static char *
read_reg(char *in, unsigned char *r, char *term, int upper, struct err_set *es,
	struct err e)
{
	char *c;
	size_t oplen, padding;
	in = consume_whitespace(in);

	oplen = padding = 0;
	for (c = in; *c != '\0'; c++, oplen++) {
		if (isspace(*c)) break;
	}

	size_t l = strlen(term);
	if (c >= in + l && strncmp(c - l, term, l) == 0) {
		padding += l;
		oplen -= l;
	}

	if (strncmp(in, "%rax", oplen) == 0) {
		*r |= RAX << (4 * upper);
	} else if (strncmp(in, "%rcx", oplen) == 0) {
		*r |= RCX << (4 * upper);
	} else if (strncmp(in, "%rdx", oplen) == 0) {
		*r |= RDX << (4 * upper);
	} else if (strncmp(in, "%rbx", oplen) == 0) {
		*r |= RBX << (4 * upper);
	} else if (strncmp(in, "%rsp", oplen) == 0) {
		*r |= RSP << (4 * upper);
	} else if (strncmp(in, "%rbp", oplen) == 0) {
		*r |= RBP << (4 * upper);
	} else if (strncmp(in, "%rsi", oplen) == 0) {
		*r |= RSI << (4 * upper);
	} else if (strncmp(in, "%rdi", oplen) == 0) {
		*r |= RDI << (4 * upper);
	} else if (strncmp(in, "%r8", oplen) == 0) {
		*r |= R8 << (4 * upper);
	} else if (strncmp(in, "%r9", oplen) == 0) {
		*r |= R9 << (4 * upper);
	} else if (strncmp(in, "%r10", oplen) == 0) {
		*r |= R10 << (4 * upper);
	} else if (strncmp(in, "%r11", oplen) == 0) {
		*r |= R11 << (4 * upper);
	} else if (strncmp(in, "%r12", oplen) == 0) {
		*r |= R12 << (4 * upper);
	} else if (strncmp(in, "%r13", oplen) == 0) {
		*r |= R13 << (4 * upper);
	} else if (strncmp(in, "%r14", oplen) == 0) {
		*r |= R14 << (4 * upper);
	} else {
		e.type = RE_NOREG;
		e.data.reg = strndup(in, oplen);
		err_append(es, e);
	}

	return in + oplen + padding;
}

static char *
read_dest(char *in, struct dest *x, struct err_set *es, struct err e)
{
	in = consume_whitespace(in);

	// Let's zero dest for predictible behavior if linking goes
	// catastrophically wrong.
	x->adr = 0;

	if (!isdigit(*in) && *in != '-') {
		// if a target doesn't look like a number, parse it as a label.
		// Note that we're looking to see any number, not necessarily
		// a positive number.
		size_t len = 0;
		for (char *c = in; *c != '\0'; c++, len++)
			if (isspace(*c)) break;

		x->label = strndup(in, len);
		in += len;
	} else {
		in = read_imdte(in, &x->adr, '\0', es, e);

		if (x->adr < 0) {
			e.type = RE_NEGATIVE_JMP;
			err_append(es, e);
		}
	}

	return in;
}

static char *
read_imdte(char *in, long *x, char term, struct err_set *es, struct err e)
{
	size_t len = 0;
	char *end;
	in = consume_whitespace(in);

	*x = strtol(in, &end, 0);

	if (*end == term) {
		len = end - in + 1;
	} else if (*end != '\0' && !isspace(*end)) {
		for (; in[len] != '\0'; len++)
			if (isspace(in[len]) || in[len] == term)
				break;

		e.type = RE_BADINT;
		e.data.bint = strndup(in, len);
		err_append(es, e);
	}

	return in + len;
}

static char *
read_cond(char *in, unsigned char *x, const char *uncond, struct err_set *es)
{
	size_t oplen;
	struct err e;

	oplen = 0;
	for (; in[oplen] != '\0'; oplen++) {
		if (isspace(in[oplen]))
			break;
	}

	// Note that order of comparisons is important here. (Matching is done
	// greedily.)
	if (uncond && strncmp(in, uncond, oplen) == 0)
		*x |= C_UNCOND;
	else if (strncmp(in, "l", oplen) == 0)
		*x |= C_L;
	else if (strncmp(in, "le", oplen) == 0)
		*x |= C_LE;
	else if (strncmp(in, "e", oplen) == 0)
		*x |= C_E;
	else if (strncmp(in, "ne", oplen) == 0)
		*x |= C_NE;
	else if (strncmp(in, "g", oplen) == 0)
		*x |= C_G;
	else if (strncmp(in, "ge", oplen) == 0)
		*x |= C_GE;
	else {
		e.type = RE_BADCOND;
		e.data.cond = strndup(in, oplen);

		err_append(es, e);
	}

	return in + oplen;
}

static char *
read_reg_pair(char *in, unsigned char *out, struct err_set *es, struct err e)
{
	in = read_reg(in, out, ",", 1, es, e);
	in = read_reg(in, out, "",  0, es, e);
	return in;
}

static char *
consume_whitespace(char *x)
{
	while (*x != '\0' && isspace(*x)) x++;
	return x;
}

static void
strip_comment(char *x)
{
	for (char *c = x; *c != '\0'; c++)
		if (*c == '#') *c = '\0';
}
