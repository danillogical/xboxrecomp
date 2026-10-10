/*
 * ohci.h -- the Xbox USB host controllers, enough of them to be found.
 *
 * A title reaches its gamepad through XAPI, which is statically linked into
 * the image and drives the OHCI controller registers directly rather than
 * going through anything this runtime can shim. Half-Life 2 registers a 4 KB
 * block at 0xFED00000 and takes an interrupt vector for it, then reads
 * HcRevision out of an aperture backed by zeroed RAM, concludes there is a
 * host controller with no root hub ports, and enumerates nothing.
 *
 * This is the register half of the answer: a controller that reports a real
 * revision, a root hub with ports, and the handful of registers that have
 * behaviour rather than storage. It walks the endpoint and transfer descriptor
 * lists -- control, periodic and bulk -- so a driver that finds a port can
 * enumerate the device behind it and poll it for reports.
 *
 * Input to the device comes from src/input: a real host pad through XInput,
 * and (under RECOMP_KEYBOARD) the keyboard through the framebuffer window.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the models up. Safe to call more than once; does nothing unless
 * RECOMP_USB is set, so a title that was working without a controller keeps
 * behaving exactly as it did.
 *
 * On Windows this is self-contained: it makes both register blocks
 * PAGE_NOACCESS and registers its own vectored exception handler, so the
 * faults are answered without the embedder wiring anything. Call it after
 * guest memory is mapped (it disables itself otherwise) and after the
 * embedder's own handler if the embedder has one, because this one runs
 * first and would otherwise swallow its diagnostics. */
void xbox_OhciInit(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Service a trapped access. Returns 1 if the faulting instruction was decoded
 * and stepped over. `ctx` is a PCONTEXT; void * keeps windows.h out of here.
 *
 * Public for a harness that wants to route faults itself. An embedder that
 * just calls xbox_OhciInit does not need it: the model registers its own
 * handler. */
int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

#endif /* XBOX_OHCI_H */
