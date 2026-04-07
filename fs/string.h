#ifndef STRING_H
#define STRING_H

void *memcpy(void *dst, void *src, int n);
void *memset(void *dst, int v, int n);
void *memmove(void *dst, void *src, int n);
int memcmp(void *a, void *b, int n);
int strlen(char *s);
int strcmp(char *a, char *b);
int strncmp(char *a, char *b, int n);
char *strcpy(char *dst, char *src);
char *strncpy(char *dst, char *src, int n);

#endif
