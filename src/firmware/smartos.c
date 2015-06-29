
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <sys/stat.h>

#include <xhyve/vmm/vmm_api.h>
#include <xhyve/firmware/smartos.h>
#include <xhyve/support/specialreg.h>

#define	PAGESIZE			4096UL
#define	NEXT_PAGE(addr)			((addr + PAGESIZE) & ~(PAGESIZE - 1))

#define	MULTIBOOT_HEADER_REGION		8192
#define	MULTIBOOT_MAGIC			0x1badb002
#define	MULTIBOOT_MAGIC_EAX		0X2badb002

#define	MULTIBOOT_FLAG_PAGEALIGN	(1 << 0)
#define	MULTIBOOT_FLAG_INCLUDE_MEMORY	(1 << 1)
#define	MULTIBOOT_FLAG_VIDEO_MODE	(1 << 2)
#define	MULTIBOOT_FLAG_LOAD_FIELDS	(1 << 16)

typedef struct region {
	uint8_t *rg_vaddr;
	uintptr_t rg_paddr;
	uintptr_t rg_size;
} region_t;

typedef struct smartos_config {
	char *soc_kernel_path;
	char *soc_initrd_path;
	char *soc_cmdline;
} smartos_config_t;

static smartos_config_t g_soc;

typedef struct mb_hdr {
	uint32_t mbh_magic;
	uint32_t mbh_flags;
	uint32_t mbh_checksum;

	/*
	 * These fields are valid when MULTIBOOT_FLAG_LOAD_FIELDS is set:
	 */
	uint32_t mbh_header_addr;
	uint32_t mbh_load_addr;
	uint32_t mbh_load_end_addr;
	uint32_t mbh_bss_end_addr;
	uint32_t mbh_entry_addr;

	/*
	 * These fields are valid when MULTIBOOT_FLAG_VIDEO_MODE is set:
	 */
	uint32_t mbh_mode_type;
	uint32_t mbh_width;
	uint32_t mbh_height;
	uint32_t mbh_depth;
} mb_hdr_t;

#define	MBI_FLAG_MEMORY_INFO	(1 << 0)
#define	MBI_FLAG_BOOT_DEVICE	(1 << 1)
#define	MBI_FLAG_CMDLINE	(1 << 2)
#define	MBI_FLAG_MODULES	(1 << 3)
#define	MBI_FLAG_MEMORY_MAP	(1 << 6)
#define	MBI_FLAG_DRIVES		(1 << 7)
#define	MBI_FLAG_CONFIG_TABLE	(1 << 8)
#define	MBI_FLAG_LOADER_NAME	(1 << 9)

typedef struct mb_info {
	uint32_t mbi_flags;

	/*
	 * MBI_FLAG_MEMORY_INFO:
	 */
	uint32_t mbi_mem_lower;
	uint32_t mbi_mem_upper;

	/*
	 * MBI_FLAG_BOOT_DEVICE:
	 */
	uint32_t mbi_boot_device;

	/*
	 * MBI_FLAG_CMDLINE:
	 */
	uint32_t mbi_cmdline;

	/*
	 * MBI_FLAG_MODULES:
	 */
	uint32_t mbi_mods_count;
	uint32_t mbi_mods_addr;

	uint32_t mbi_syms[3];

	/*
	 * MBI_FLAG_MEMORY_MAP:
	 */
	uint32_t mbi_mmap_length;
	uint32_t mbi_mmap_addr;
} mb_info_t;

typedef struct mb_mod {
	uint32_t mbm_start;
	uint32_t mbm_end;
	uint32_t mbm_string;
	uint32_t mbm_reserved;
} mb_mod_t;


static int debug_enabled = 0;

#define	DLOG(fmt, ...)							\
	do {								\
		if (debug_enabled != 0) {				\
			fprintf(stderr, "SMARTOS: " fmt "\r\n",		\
			    __VA_ARGS__);				\
		}							\
	} while (0)

static void
smartos_print_flags(uint32_t flags, char *buf, int bufsz)
{
	snprintf(buf, bufsz, "[%s%s%s%s] <%x>",
	    flags & MULTIBOOT_FLAG_PAGEALIGN ? "PAGEALIGN" : "",
	    flags & MULTIBOOT_FLAG_INCLUDE_MEMORY ? " INCLUDE_MEMORY" : "",
	    flags & MULTIBOOT_FLAG_VIDEO_MODE ? " VIDEO_MODE" : "",
	    flags & MULTIBOOT_FLAG_LOAD_FIELDS ? " LOAD_FIELDS": "",
	    flags);
}

static int
smartos_load_multiboot_header(FILE *f, off_t foff, mb_hdr_t *mbh)
{
	char buf[128];
	uint32_t sum;

	if (fseeko(f, foff, SEEK_SET) != 0) {
		warn("could not seek to offset %llu of kernel file", foff);
		return (-1);
	}

	if (fread(mbh, sizeof (*mbh), 1, f) != 1) {
		warn("failed to read multiboot header");
		return (-1);
	}

	if (mbh->mbh_magic != MULTIBOOT_MAGIC) {
		warn("wanted magic %x, got magic %x", MULTIBOOT_MAGIC,
		    mbh->mbh_magic);
		return (-1);
	}

	sum = mbh->mbh_magic + mbh->mbh_flags + mbh->mbh_checksum;
	if (sum != 0) {
		warn("wanted checksum 0, got checksum %x", sum);
		return (-1);
	}

	smartos_print_flags(mbh->mbh_flags, buf, sizeof (buf));
	DLOG("flags: %s", buf);

	if (mbh->mbh_flags != (MULTIBOOT_FLAG_PAGEALIGN |
	    MULTIBOOT_FLAG_INCLUDE_MEMORY | MULTIBOOT_FLAG_LOAD_FIELDS)) {
		warn("unexpected flags combination");
		return (-1);
	}

	DLOG("OK, read multiboot header from %llu", foff);
	return (0);
}

static int
smartos_find_multiboot(FILE *f, off_t *foff)
{
	off_t o = 0;

	if (fseeko(f, o, SEEK_SET) != 0) {
		warn("could not seek to offset 0 of kernel file");
		return (-1);
	}

	for (; o < MULTIBOOT_HEADER_REGION; o += 4) {
		uint32_t magic;
		size_t sz = sizeof (magic);

		if (fread(&magic, sz, 1, f) != 1) {
			warn("failed to read from offset %llu", o);
			return (-1);
		}

		if (magic == MULTIBOOT_MAGIC) {
			DLOG("magic found @ %llu", o);
			*foff = o;
			return (0);
		}
	}

	warnx("no magic found in first %u bytes", MULTIBOOT_HEADER_REGION);
	return (-1);
}

static void
region_child(region_t *parent, region_t *child, uintptr_t offset, uintptr_t len)
{
	/*
	 * Check that the child region will fit entirely within the parent
	 * region.
	 */
	if (len >= (parent->rg_size - offset)) {
		errx(1, "child (off %lx len %lx) does not fit in parent "
		    "(len %lx)", offset, len, parent->rg_size);
	}

	child->rg_vaddr = parent->rg_vaddr + offset;
	child->rg_paddr = parent->rg_paddr + offset;
	child->rg_size = len;
}

static uintptr_t
region_last_paddr(region_t *reg)
{
	return (reg->rg_paddr + reg->rg_size - 1);
}

static int
smartos_multiboot_info(region_t *mbinfo, region_t *cmdline, region_t *mbmods,
    region_t *mbstrs, region_t *bootarch)
{
	mb_info_t mbi;
	mb_mod_t mbm;

	memset(&mbi, 0, sizeof (mbi));

	mbi.mbi_flags |= MBI_FLAG_CMDLINE;
	mbi.mbi_cmdline = (uint32_t)cmdline->rg_paddr;

	mbi.mbi_flags |= MBI_FLAG_MEMORY_INFO;
	mbi.mbi_mem_lower = 640;
	mbi.mbi_mem_upper = 1024 * 1024;

	/*
	 * Set up string for module[0]:
	 */
	strcpy((char *)mbstrs->rg_vaddr, "/platform/i86pc/amd64/boot_archive");

	/*
	 * Set up module descriptor:
	 */
	memset(&mbm, 0, sizeof (mbm));
	mbm.mbm_start = (uint32_t)bootarch->rg_paddr;
	mbm.mbm_end = (uint32_t)region_last_paddr(bootarch);
	mbm.mbm_string = (uint32_t)mbstrs->rg_paddr;

	/*
	 * Copy out module descriptor:
	 */
	memset(mbmods->rg_vaddr, 0, PAGESIZE);
	memcpy(mbmods->rg_vaddr, &mbm, sizeof (mbm));

	mbi.mbi_flags |= MBI_FLAG_MODULES;
	mbi.mbi_mods_count = 1;
	mbi.mbi_mods_addr = (uint32_t)mbmods->rg_paddr;

	memcpy(mbinfo->rg_vaddr, &mbi, sizeof (mbi));

	return (0);
}

static int
smartos_load_bootarchive(const char *path, region_t *mem, region_t *bootarch,
    uintptr_t baseaddr)
{
	int ret = -1;
	FILE *f = NULL;
	struct stat st;
	size_t filesz;

	if ((f = fopen(path, "r")) == NULL) {
		warn("could not open \"%s\"", path);
		goto bail;
	}

	if (fstat(fileno(f), &st) != 0) {
		warn("could not fstat(%d)", fileno(f));
		goto bail;
	}
	if (st.st_size < 0) {
		abort();
	}
	filesz = (size_t)st.st_size;

	region_child(mem, bootarch, baseaddr, filesz);

	DLOG("loading boot archive \"%s\" size %lu", path, filesz);
	if (fread(bootarch->rg_vaddr, 1, filesz, f) != filesz) {
		warn("could not read boot_archive");
		goto bail;
	}

	ret = 0;

bail:
	if (f != NULL) {
		fclose(f);
	}
	return (ret);
}

static int
smartos_load_kernel(const char *path, region_t *mem, region_t *kern, uint64_t *rip)
{
	int ret = -1;
	FILE *f = NULL;
	off_t mboff = -1;
	struct stat st;
	off_t filesz;
	mb_hdr_t mbh;

	memset(&mbh, 0, sizeof (mbh));

	if ((f = fopen(path, "r")) == NULL) {
		warn("could not open \"%s\"", path);
		goto bail;
	}

	if (fstat(fileno(f), &st) != 0) {
		warn("could not fstat(%d)", fileno(f));
		goto bail;
	}
	filesz = st.st_size;

	if (smartos_find_multiboot(f, &mboff) != 0) {
		warnx("could not find multiboot header");
		goto bail;
	}

	if (smartos_load_multiboot_header(f, mboff, &mbh) != 0) {
		warnx("could not load multiboot header");
		goto bail;
	}

	DLOG("header_addr    %8x", mbh.mbh_header_addr);
	DLOG("load_addr      %8x", mbh.mbh_load_addr);
	DLOG("load_end_addr  %8x", mbh.mbh_load_end_addr);
	DLOG("bss_end_addr   %8x", mbh.mbh_bss_end_addr);
	DLOG("entry_addr     %8x", mbh.mbh_entry_addr);

	if (mbh.mbh_load_end_addr != 0 || mbh.mbh_bss_end_addr != 0) {
		warn("cannot handle a non-zero load_end_addr or bss_end_addr");
		goto bail;
	}

	off_t load_offset = mboff - (mbh.mbh_header_addr - mbh.mbh_load_addr);
	if (filesz <= 0 || load_offset < 0) {
		abort();
	}
	size_t load_size = (size_t)(filesz - load_offset);
	uintptr_t kernel_end = mbh.mbh_load_addr + load_size;
	uintptr_t mbi_addr = NEXT_PAGE(kernel_end);

	DLOG("%s", "");
	DLOG("file load offs %8llx", load_offset);
	DLOG("file load len  %8llx", filesz - load_offset);
	DLOG("end of kernel  %8lx", kernel_end);
	DLOG("mb info        %8lx", mbi_addr);

	region_child(mem, kern, mbh.mbh_load_addr, load_size);

	if (fseek(f, load_offset, SEEK_SET) != 0) {
		warn("could not seek to load offset %llx", load_offset);
		goto bail;
	}

	if (fread(kern->rg_vaddr, 1, load_size, f) != load_size) {
		warn("could not read kernel into memory (size %lx)", load_size);
		goto bail;
	}

	*rip = mbh.mbh_entry_addr;

	ret = 0;

bail:
	if (f != NULL) {
		fclose(f);
	}
	return (ret);
}

void
smartos_init(char *kernel_path, char *initrd_path, char *cmdline)
{
	const char *dbgval;
	char *buf;

	if ((dbgval = getenv("DEBUG_SMARTOS")) != NULL && dbgval[0] == '1') {
		debug_enabled = 1;
	}

	if ((buf = calloc(1, 4096)) == NULL) {
		err(1, "calloc failure");
	}

	if (kernel_path == NULL || initrd_path == NULL) {
		errx(1, "smartos firmware requires kernel path and initrd "
		    "path");
	}

	/*
	 * Construct command-line.  The first "argument" is the kernel
	 * "filename".
	 */
	strcat(buf, "/platform/i86pc/kernel/amd64/unix ");
	if (cmdline == NULL) {
		cmdline = "-B console=ttya,ttya-mode=\"9600,8,n,1,-\" -kd";
	}
	strcat(buf, cmdline);

	if ((g_soc.soc_kernel_path = strdup(kernel_path)) == NULL ||
	    (g_soc.soc_initrd_path = strdup(initrd_path)) == NULL ||
	    (g_soc.soc_cmdline = strdup(buf)) == NULL) {
		err(1, "smartos: strdup failure");
	}

	free(buf);
}

uint64_t
smartos_load(void)
{
	region_t low;
	region_t mem, kern, mbinfo, cmdline, mbmods, mbstrs, bootarch;
	uint64_t rip;
	region_t *last;

	/*
	 * Map low memory:
	 */
	mem.rg_paddr = 0;
	mem.rg_size = xh_vm_get_lowmem_size();
	if ((mem.rg_vaddr = xh_vm_map_gpa(mem.rg_paddr, mem.rg_size)) == NULL) {
		err(1, "could not map lowmem");
	}

	/*
	 * Load kernel file:
	 */
	memset(&kern, 0, sizeof (kern));
	if (smartos_load_kernel(g_soc.soc_kernel_path, &mem, &kern,
	    &rip) != 0) {
		errx(1, "could not load kernel \"%s\"", g_soc.soc_kernel_path);
	}
	DLOG("kern [%8lx,%8lx)", kern.rg_paddr, region_last_paddr(&kern));
	last = &kern;

	low.rg_paddr = 0;
	low.rg_vaddr = mem.rg_vaddr;
	low.rg_size = kern.rg_paddr - mem.rg_paddr - 1;

	/*
	 * Allocate space for multiboot information.
	 */
	region_child(&low, &mbinfo, 1 * 1024 * 1024, PAGESIZE);
	DLOG("mbinfo @ [%8lx,%8lx)", mbinfo.rg_paddr, region_last_paddr(&mbinfo));
	last = &mbinfo;

	/*
	 * Write command line:
	 */
	region_child(&low, &cmdline, NEXT_PAGE(region_last_paddr(last)), PAGESIZE);
	memset(cmdline.rg_vaddr, 0, PAGESIZE);
	memcpy(cmdline.rg_vaddr, g_soc.soc_cmdline, strlen(g_soc.soc_cmdline));
	DLOG("cmdline @ [%8lx,%8lx)", cmdline.rg_paddr, region_last_paddr(&cmdline));
	DLOG("cmdline: %s", cmdline.rg_vaddr);
	last = &cmdline;

	/*
	 * Create multiboot module array:
	 */
	region_child(&low, &mbmods, NEXT_PAGE(region_last_paddr(last)), PAGESIZE);
	DLOG("mb mods @ [%8lx,%8lx)", mbmods.rg_paddr, region_last_paddr(&mbmods));
	last = &mbmods;

	region_child(&low, &mbstrs, NEXT_PAGE(region_last_paddr(last)), PAGESIZE);
	DLOG("mb strs @ [%8lx,%8lx)", mbstrs.rg_paddr, region_last_paddr(&mbstrs));
	last = &mbstrs;

	/*
	 * Load boot_archive:
	 */
	if (smartos_load_bootarchive(g_soc.soc_initrd_path, &mem, &bootarch,
	    NEXT_PAGE(region_last_paddr(&kern))) != 0) {
		errx(1, "could not load boot_archive");
	}
	DLOG("bootarch @ [%8lx,%8lx)", bootarch.rg_paddr, region_last_paddr(&bootarch));

	if (smartos_multiboot_info(&mbinfo, &cmdline, &mbmods, &mbstrs, &bootarch) != 0) {
		errx(1, "could not write multiboot info");
	}

	xh_vcpu_reset(0);

	xh_vm_set_register(0, VM_REG_GUEST_RAX, MULTIBOOT_MAGIC_EAX);
	xh_vm_set_register(0, VM_REG_GUEST_RBX, mbinfo.rg_paddr);

	xh_vm_set_desc(0, VM_REG_GUEST_CS, 0, 0xffffffff, 0xc09b);
	xh_vm_set_desc(0, VM_REG_GUEST_DS, 0, 0xffffffff, 0xc093);
	xh_vm_set_desc(0, VM_REG_GUEST_ES, 0, 0xffffffff, 0xc093);
	xh_vm_set_desc(0, VM_REG_GUEST_SS, 0, 0xffffffff, 0xc093);

	xh_vm_set_register(0, VM_REG_GUEST_CS, 0x10);
	xh_vm_set_register(0, VM_REG_GUEST_DS, 0x18);
	xh_vm_set_register(0, VM_REG_GUEST_ES, 0x18);
	xh_vm_set_register(0, VM_REG_GUEST_SS, 0x18);

	/*
	 * Enable protected mode (PE).
	 */
	xh_vm_set_register(0, VM_REG_GUEST_CR0, CR0_PE);
	xh_vm_set_register(0, VM_REG_GUEST_RFLAGS, 0x2);

	/*
	 * Set the program counter to the kernel entry point.
	 */
	DLOG("rip = %8llx", rip);
	xh_vm_set_register(0, VM_REG_GUEST_RIP, rip);

	DLOG("******************* %s ***********", "MACHINE BOOT");
	return (rip);
}
