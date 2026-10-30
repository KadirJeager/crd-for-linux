#ifndef CRD_CLIPBOARD_INTERNAL_H
#define CRD_CLIPBOARD_INTERNAL_H

#include <stddef.h>

/* Test harness helpers; not part of the public CRD API. */

const char *clipboard_protocol_name(void);
size_t clipboard_selection_mime_count(void);
const char *clipboard_selection_mime_at(size_t index);

/* Test harness: inject failures and count clipboard_set calls. */
void clipboard_test_reset(void);
void clipboard_test_inject_get_fail(int enable);
unsigned clipboard_test_set_call_count(void);

#endif
