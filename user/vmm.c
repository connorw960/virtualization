#include <inc/lib.h>
#include <inc/vmx.h>
#include <inc/elf.h>
#include <inc/ept.h>
#include <inc/stdio.h>

#define GUEST_KERN "/vmm/kernel"
#define GUEST_BOOT "/vmm/boot"

#define JOS_ENTRY 0x7000

// Map a region of file fd into the guest at guest physical address gpa.
// The file region to map should start at fileoffset and be length filesz.
// The region to map in the guest should be memsz.  The region can span multiple pages.
//
// Return 0 on success, <0 on failure.
//
// Hint: Call sys_ept_map() for mapping page. 
static int
map_in_guest( envid_t guest, uintptr_t gpa, size_t memsz, 
	      int fd, size_t filesz, off_t fileoffset ) {
	int i, ret;

	i = PGOFF(gpa);
	// if the provided guest physical address is not page-aligned, 
	// adjust our values so we are working with a page-aligned address
	if (i) {
		gpa -= i;
		memsz += i;
		filesz += i;
		fileoffset -= i;
	}

	// walk through the provided region page by page and copy in the file contents
	// the the physical memory region of the guest
	for (i = 0; i < memsz; i += PGSIZE) {
		// allocate a temporary page
		ret = sys_page_alloc(0, UTEMP, PTE_P | PTE_U | PTE_W);
		if (ret < 0) {
			sys_page_unmap(0, UTEMP);
			return ret;
		}
		// seek to the location to write the file contents at
		ret = seek(fd, fileoffset + i);
		if (ret < 0) {
			sys_page_unmap(0, UTEMP);
			return ret;
		}
		// read file contents into the mapped page
		ret = readn(fd, UTEMP, MIN(PGSIZE, filesz-i));
		if (ret < 0) {
			sys_page_unmap(0, UTEMP);
			return ret;
		}
		// map the page in the EPT
		ret = sys_ept_map(0, UTEMP, guest, (void*) (gpa + i), __EPTE_FULL);
		if (ret < 0) {
			sys_page_unmap(0, UTEMP);
			return ret;
		}
		sys_page_unmap(0, UTEMP);
	}

	return 0;
} 

// Read the ELF headers of kernel file specified by fname,
// mapping all valid segments into guest physical memory as appropriate.
//
// Return 0 on success, <0 on error
//
// Hint: compare with ELF parsing in env.c, and use map_in_guest for each segment.
static int
copy_guest_kern_gpa( envid_t guest, char* fname ) {
	// open the specified file
	int fd, i, ret;
	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		cprintf("error opening %s: %e\n", fname, fd);
		return fd;
	}

	int buflen = 512; // large enough to fit the whole Elf struct
	unsigned char elf_buf[buflen];
	struct Elf *elf;
	struct Proghdr *ph;

	// read in the header of file fname
	ret = readn(fd, elf_buf, buflen);
	if (ret != buflen) {
		close(fd);
		cprintf("Failed to read in ELF header\n");
		return -E_NOT_EXEC;
	}

	// point the elf struct at the elf buf
	elf = (struct Elf*) elf_buf;

	// check the ELF header's magic value to check that we are working with a valid ELF binary
	if (elf->e_magic != ELF_MAGIC) {
		close(fd);
		cprintf("ELF magic is %08x, expected %08x\n", elf->e_magic, ELF_MAGIC);
	}

	// point the program header struct at the location indicated by the elf header
	ph = (struct Proghdr* ) (elf_buf + elf->e_phoff);

	// for every entry in the program header table, map the corresponding entry of the file 
	// into the guest's physical address space
	for (i = 0; i < elf->e_phnum; i++, ph++) {
		ret = map_in_guest(guest, ph->p_pa, ph->p_memsz, fd, ph->p_filesz, ph->p_offset);
		if (ret < 0) {
			cprintf("Error mapping EPT page in guest %e\n", ret);
			close(fd);
			return ret;
		}
	}

	close(fd);
	return 0;
}

void
umain(int argc, char **argv) {
	int ret;
	envid_t guest;
	char filename_buffer[50];	//buffer to save the path 
	int vmdisk_number;
	int r;
	if ((ret = sys_env_mkguest( GUEST_MEM_SZ, JOS_ENTRY )) < 0) {
		cprintf("Error creating a guest OS env: %e\n", ret );
		exit();
	}
	guest = ret;

	// Copy the guest kernel code into guest phys mem.
	if((ret = copy_guest_kern_gpa(guest, GUEST_KERN)) < 0) {
		cprintf("Error copying page into the guest - %d\n.", ret);
		exit();
	}

	// Now copy the bootloader.
	int fd;
	if ((fd = open( GUEST_BOOT, O_RDONLY)) < 0 ) {
		cprintf("open %s for read: %e\n", GUEST_BOOT, fd );
		exit();
	}

	// sizeof(bootloader) < 512.
	if ((ret = map_in_guest(guest, JOS_ENTRY, 512, fd, 512, 0)) < 0) {
		cprintf("Error mapping bootloader into the guest - %d\n.", ret);
		exit();
	}
#ifndef VMM_GUEST	
	sys_vmx_incr_vmdisk_number();	//increase the vmdisk number
	//create a new guest disk image
	
	vmdisk_number = sys_vmx_get_vmdisk_number();
	snprintf(filename_buffer, 50, "/vmm/fs%d.img", vmdisk_number);
	
	cprintf("Creating a new virtual HDD at /vmm/fs%d.img\n", vmdisk_number);
        r = copy("vmm/clean-fs.img", filename_buffer);
        
        if (r < 0) {
        	cprintf("Create new virtual HDD failed: %e\n", r);
        	exit();
        }
        
        cprintf("Create VHD finished\n");
#endif
	// Mark the guest as runnable.
	sys_env_set_status(guest, ENV_RUNNABLE);
	wait(guest);
}


