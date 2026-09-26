/*
 * hw/pci/pci.h shim for the pinned xemu DSP56300 sources (A4b1 step 2).
 *
 * Pass-through. The pinned gp_ep.h includes this for the PCI device
 * vocabulary; the toolkit's QEMU shim already provides PCIDevice, PCIBus,
 * pci_register_bar and the PCI_* constants used by the APU surface. Nothing
 * is added here.
 */

#ifndef XBOXRECOMP_SHIM_HW_PCI_PCI_H
#define XBOXRECOMP_SHIM_HW_PCI_PCI_H

#include "qemu/osdep.h"

#endif /* XBOXRECOMP_SHIM_HW_PCI_PCI_H */
