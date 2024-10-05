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

	int i, r;

	if (gpa >= UTOP || gpa + memsz > UTOP) {
        return -E_INVAL; // Invalid guest physical address
    }

    for (i = 0; i < memsz; i += PGSIZE)
    {
		// Read data into buffer - find how much (either a full page worth or less)
		size_t readNum = MIN(PGSIZE, filesz - (fileoffset + i));

		// Need to get the data from the file
		if (i < filesz)
		{
			if(readNum > 0)
			{
				if ((r = seek(fd, fileoffset + i)) < 0)
				{
					return r;
				}
				if ((r = readn(fd, UTEMP, readNum)) < 0)
				{
					return r;
				}

				// Check if we filled a full page, if not fill the rest with zeros
				if(readNum < PGSIZE)
				{
					memset((void*)((uintptr_t)UTEMP + readNum), 0, (PGSIZE - readNum));
				}
			}
			else
			{
				// Fill page with no data
				memset(UTEMP, 0, PGSIZE);
			}
			
		} 
		else
		{
			// Fill page with no data
			memset(UTEMP, 0, PGSIZE);
		}

		if ((r = sys_ept_map(thisenv->env_id, UTEMP, guest, (void *) (gpa+i), __EPTE_FULL)) < 0)
		{
			return r;
		}
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
	unsigned char elf_buf[512];
	int fd, i, r;
	struct Elf *elf;
	struct Proghdr *ph, *eph;
	struct Env *e;
	int perm;

	// Open the file and set fd
	if ((fd = open(fname, O_RDONLY)) < 0)
	{
    	return fd;
	}

	// Read the ELF header
    if ((r = readn(fd, elf_buf, sizeof(elf_buf))) != sizeof(elf_buf)) {
        close(fd);
        return -E_NOT_EXEC; // Failed to read the ELF header
    }

	// Set elf
	elf = (struct Elf*) elf_buf;

	// Check e_magic
	if ( elf->e_magic != ELF_MAGIC) 
	{
    	close(fd);
    	return -E_NOT_EXEC;
	}

	// Get program header
	ph = (struct Proghdr*) (elf_buf + elf->e_phoff);
	eph = ph + elf->e_phnum;
	for(;ph < eph; ph++)
	{
		if (ph->p_type == ELF_PROG_LOAD)
		{
			// Map to guest
			if ((r = map_in_guest(guest, ph->p_pa, ph->p_memsz, fd, ph->p_filesz, ph->p_offset)) < 0)
			{
				close(fd);
				return -E_NO_SYS;
			}
		}
	}

	// Get env 
	if((r = envid2env(guest,&e)) < 0)
	{
		return r;
	}

	// Do env stuff
	region_alloc(e, (void*) (USTACKTOP - PGSIZE), PGSIZE);
	e->env_tf.tf_rip    = elf->e_entry;
	e->env_tf.tf_rsp    = USTACKTOP; //keeping stack 8 byte aligned

	// Do debug stuff
	uintptr_t debug_address = USTABDATA;
	struct Secthdr *sh = (struct Secthdr *)(((uint8_t *)elf + elf->e_shoff));
	struct Secthdr *shstr_tab = sh + elf->e_shstrndx;
	struct Secthdr* esh = sh + elf->e_shnum;
	for(;sh < esh; sh++) {
		char* name = (char*)((uint8_t*)elf + shstr_tab->sh_offset) + sh->sh_name;
		if(!strcmp(name, ".debug_info") || !strcmp(name, ".debug_abbrev")
			|| !strcmp(name, ".debug_line") || !strcmp(name, ".eh_frame")
			|| !strcmp(name, ".debug_str")) {
			// Map to guest
			if ((r = map_in_guest(guest, debug_address, sh->sh_size, fd, sh->sh_size, sh->sh_offset)) < 0)
			{
				close(fd);
				return -E_NO_SYS;
			}
			debug_address += sh->sh_size;
		}
	}

	// Close and success
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


