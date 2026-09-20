#ifndef HW_H
#define HW_H

/* Friendly wrappers over the raw hardware syscalls (sys_io_in and friends,
 * declared in stdio.h).  Compile hw.c together with your program. */

int inb(int port);
int inw(int port);
int inl(int port);
void outb(int port, int v);
void outw(int port, int v);
void outl(int port, int v);

/* PCI configuration space.  bus 0-255, dev 0-31, fn 0-7, off 0-255
 * (rounded down to the containing dword by the kernel). */
int pci_read(int bus, int dev, int fn, int off);
void pci_write(int bus, int dev, int fn, int off, int val);

#endif
