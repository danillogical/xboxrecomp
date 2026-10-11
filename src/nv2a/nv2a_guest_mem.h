/* Guest memory generation: a counter the kernel bumps after any change to host protection or mapping of guest memory. */
#ifndef NV2A_GUEST_MEM_H
#define NV2A_GUEST_MEM_H

/* Call after a guest page protection, free or unmap change; the pushbuffer page-validation cache is dropped on the next read. */
void nv2a_guest_mem_changed(void);

#endif
