// stddef and err.h must be included prior to this header file.

struct asm_unit;

/* Parse a given FILE, recording errors in the given err_set, with the given
 * string borrowed as the filename. (The given string must live as long as the
 * error set.) */
struct asm_unit *asm_unit_parse(FILE *, struct err_set *, const char *);
void asm_unit_write(FILE *restrict, const struct asm_unit *restrict);

void asm_destroy_unit(struct asm_unit *);
