#ifndef STDLIB_H
#define STDLIB_H

#include "errno.h"

void *malloc(int size);
void free(void *p);
void *calloc(int n, int size);
void *realloc(void *p, int size);
int atoi(char *s);
int strtol(char *s, char **endptr, int base);

#endif
