#ifndef CRD_CLIPBOARD_H
#define CRD_CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

int clipboard_init(void);
int clipboard_pump(void);
uint64_t clipboard_selection_serial(void);
int clipboard_has(const char *mime);
int clipboard_set(const char *mime, const void *data, size_t n);
int clipboard_get(const char *mime, void **data, size_t *n);

#endif
