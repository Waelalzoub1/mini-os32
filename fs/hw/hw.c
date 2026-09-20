#include "stdio.h"
#include "hw.h"

int inb(int port) { return sys_io_in(port, 1) & 0xFF; }
int inw(int port) { return sys_io_in(port, 2) & 0xFFFF; }
int inl(int port) { return sys_io_in(port, 4); }
void outb(int port, int v) { sys_io_out(port, 1, v); }
void outw(int port, int v) { sys_io_out(port, 2, v); }
void outl(int port, int v) { sys_io_out(port, 4, v); }

int pci_read(int bus, int dev, int fn, int off) {
    return sys_pci_read((bus << 16) | (dev << 8) | fn, off);
}

void pci_write(int bus, int dev, int fn, int off, int val) {
    sys_pci_write((bus << 16) | (dev << 8) | fn, off, val);
}
