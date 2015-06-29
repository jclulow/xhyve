
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/select.h>
#include <xhyve/support/misc.h>
#include <xhyve/inout.h>
#include <xhyve/pci_lpc.h>

#define	VGA_COLOR_CRTC_INDEX	0x3d4
#define	VGA_COLOR_CRTC_DATA	0x3d5



static int
ignore_handler(UNUSED int vcpu, int in, UNUSED int port, int bytes,
	uint32_t *eax, UNUSED void *arg)
{
	if (bytes == 1 && in) {
		*eax = 0xff;
		return(0);
	}

	return (0);
}

static struct inout_port vga_index = {
	"vga_index",
	VGA_COLOR_CRTC_INDEX,
	1,
	IOPORT_F_INOUT,
	ignore_handler,
	NULL
};
static struct inout_port vga_data = {
	"vga_data",
	VGA_COLOR_CRTC_DATA,
	1,
	IOPORT_F_INOUT,
	ignore_handler,
	NULL
};

void
init_pretend_vga(void)
{
	register_inout(&vga_index);
	register_inout(&vga_data);
}
