#line 2 "../vmm/ept.c"

#include <vmm/ept.h>
#include <inc/x86.h>
#include <inc/error.h>
#include <inc/memlayout.h>
#include <kern/pmap.h>
#include <inc/string.h>

// Return the physical address of an ept entry
static inline uintptr_t epte_addr(epte_t epte)
{
    return (epte & EPTE_ADDR);
}

// Return the host kernel virtual address of an ept entry
static inline uintptr_t epte_page_vaddr(epte_t epte)
{
    return (uintptr_t) KADDR(epte_addr(epte));
}

// Return the flags from an ept entry
static inline epte_t epte_flags(epte_t epte)
{
    return (epte & EPTE_FLAGS);
}

// Return true if an ept entry's mapping is present
static inline int epte_present(epte_t epte)
{
    return (epte & __EPTE_FULL) > 0;
}

// Find the final ept entry for a given guest physical address,
// creating any missing intermediate extended page tables if create is non-zero.
//
// If epte_out is non-NULL, store the found epte_t* at this address.
//
// Return 0 on success.
//
// Error values:
//    -E_INVAL if eptrt is NULL
//    -E_NO_ENT if create == 0 and the intermediate page table entries are missing.
//    -E_NO_MEM if allocation of intermediate page table entries fails
//
// Hint: Set the permissions of intermediate ept entries to __EPTE_FULL.
//       The hardware ANDs the permissions at each level, so removing a permission
//       bit at the last level entry is sufficient (and the bookkeeping is much simpler).
static int ept_lookup_gpa(epte_t* eptrt, void *gpa,
              int create, epte_t **epte_out) {
    if (!eptrt) {
        return -E_INVAL;
    }

    int i;
    epte_t* dir = eptrt;

    for (i = EPT_LEVELS - 1; i > 0; i--) {
        // get the index into the current ept level based on gpa
        int idx = ADDR_TO_IDX(gpa, i);

        // if dir[idx] isn't present in the page table, need to 
        // create a new page table page for it (or return an error
        // if create is false)
        if (!epte_present(dir[idx])) {
            struct PageInfo* page;

            if (!create) {
                return -E_NO_ENT;
            }

            page = page_alloc(ALLOC_ZERO);
            if (!page) {
                return -E_NO_MEM;
            }
            page->pp_ref++;

            // epte_addr returns the physical address of an ept entry
            // and page2pa returns the physical address of a page. we need to do 
            // both to obtain the address for the new entry, then set the permissions 
            // on it
            dir[idx] = epte_addr(page2pa(page)) | __EPTE_FULL;
        }
        // update dir to the virtual address of the next epte
        // we need to use a virtual address so we can actually 
        // dereference it
        dir = (epte_t*) epte_page_vaddr(dir[idx]);
    }

    // set epte_out to the address of the EPT entry for the actual page
    if (epte_out) {
        *epte_out = &dir[ADDR_TO_IDX(gpa, 0)];
    }

    return 0;
}

int alloc_intermediate_ept_page(epte_t* parent, uint64_t index, int create) {
    struct PageInfo* page = NULL;
    epte_t new_epte;
    if (!create) {
        return -E_NO_ENT;
    }
    page = page_alloc(ALLOC_ZERO);
    if (!page) {
        return -E_NO_MEM;
    }
    page->pp_ref++;
    new_epte = epte_page_vaddr(page2pa(page) | __EPTE_FULL);
    parent[index] = new_epte;
    return 0;
}

void ept_gpa2hva(epte_t* eptrt, void *gpa, void **hva) {
    epte_t* pte;
    int ret = ept_lookup_gpa(eptrt, gpa, 0, &pte);
    if(ret < 0) {
        *hva = NULL;
    } else {
        if(!epte_present(*pte)) {
           *hva = NULL;
        } else {
           *hva = KADDR(epte_addr(*pte));
        }
    }
}

static void free_ept_level(epte_t* eptrt, int level) {
    epte_t* dir = eptrt;
    int i;

    for(i=0; i<NPTENTRIES; ++i) {
        if(level != 0) {
            if(epte_present(dir[i])) {
                physaddr_t pa = epte_addr(dir[i]);
                free_ept_level((epte_t*) KADDR(pa), level-1);
                // free the table.
                page_decref(pa2page(pa));
            }
        } else {
            // Last level, free the guest physical page.
            if(epte_present(dir[i])) {
                physaddr_t pa = epte_addr(dir[i]);
                page_decref(pa2page(pa));
            }
        }
    }
    return;
}

// Free the EPT table entries and the EPT tables.
// NOTE: Does not deallocate EPT PML4 page.
void free_guest_mem(epte_t* eptrt) {
    free_ept_level(eptrt, EPT_LEVELS - 1);
    tlbflush();
}

// Add Page pp to a guest's EPT at guest physical address gpa
//  with permission perm.  eptrt is the EPT root.
//
// This function should increment the reference count of pp on a
//   successful insert.  If you overwrite a mapping, your code should
//   decrement the reference count of the old mapping.
//
// Return 0 on success, <0 on failure.
//
int ept_page_insert(epte_t* eptrt, struct PageInfo* pp, void* gpa, int perm) {

    /* Your code here */
	int r;
    pte_t *pte = NULL;

    if((r = ept_lookup_gpa(eptrt, (void*) gpa, 1, &pte)) < 0)
	{
		return r;
	}
    if (pte == NULL )
	{
		return -E_NO_MEM;
	}
    if(*pte & PTE_P)
	{
		page_decref(pa2page(gpa));
	}
    *pte = ((uint64_t)page2pa(pp)) | perm | __EPTE_IPAT;
	// Success so increment
    pp->pp_ref++;
	
    return 0;
}

// Map host virtual address hva to guest physical address gpa,
// with permissions perm.  eptrt is a pointer to the extended
// page table root.
//
// Return 0 on success.
//
// If the mapping already exists and overwrite is set to 0,
//  return -E_INVAL.
//
// Hint: use ept_lookup_gpa to create the intermediate
//       ept levels, and return the final epte_t pointer.
//       You should set the type to EPTE_TYPE_WB and set __EPTE_IPAT flag.
int ept_map_hva2gpa(epte_t* eptrt, void* hva, void* gpa, int perm,
        int overwrite) {
    epte_t* pte;
    // look up the page table entry for gpa and store it in pte
    int ret = ept_lookup_gpa(eptrt, gpa, 1, &pte);
    if (ret < 0) {
        return ret;
    }

    // if there's already an entry for gpa and overwrite is false, return error
    if (epte_present(*pte) && !overwrite) {
        return -E_INVAL;
    }

    // first have to convert hva to a physical address, since we actually want to map 
    // to physical addresses. take the bitwise OR with the desired permissions and the 
    // EPTE type we want to use.
    // Then, use epte_addr to obtain the physical address for that
    // Finally, bitwise-OR with __EPTE_IPAT
    // IPAT means "ignore PAT memory" - PAT == page attribute table and is a 
    // structure related to caching. not relevant to us, but still needs to be set.
    *pte = epte_addr( PADDR( hva ) ) | perm | __EPTE_TYPE( EPTE_TYPE_WB ) 
        | __EPTE_IPAT;
    // invalidate tlb entry for gpa, since we've changed the mapping
    tlb_invalidate(eptrt, gpa);
    return 0;
}

int ept_alloc_static(epte_t *eptrt, struct VmxGuestInfo *ginfo) {
    physaddr_t i;

    for(i=0x0; i < 0xA0000; i+=PGSIZE) {
        struct PageInfo *p = page_alloc(0);
        p->pp_ref += 1;
        int r = ept_map_hva2gpa(eptrt, page2kva(p), (void *)i, __EPTE_FULL, 0);
    }

    for(i=0x100000; i < ginfo->phys_sz; i+=PGSIZE) {
        struct PageInfo *p = page_alloc(0);
        p->pp_ref += 1;
        int r = ept_map_hva2gpa(eptrt, page2kva(p), (void *)i, __EPTE_FULL, 0);
    }
    return 0;
}

#ifdef TEST_EPT_MAP
#include <kern/env.h>
#include <kern/syscall.h>
int _export_sys_ept_map(envid_t srcenvid, void *srcva,
        envid_t guest, void* guest_pa, int perm);

int test_ept_map(void)
{
    struct Env *srcenv, *dstenv;
    struct PageInfo *pp;
    epte_t *epte;
    int r;
    int pp_ref;
    int i;
    epte_t* dir;
    /* Initialize source env */
    if ((r = env_alloc(&srcenv, 0)) < 0)
        panic("Failed to allocate env (%d)\n", r);
    if (!(pp = page_alloc(ALLOC_ZERO)))
        panic("Failed to allocate page (%d)\n", r);
    if ((r = page_insert(srcenv->env_pml4e, pp, UTEMP, 0)) < 0)
        panic("Failed to insert page (%d)\n", r);
    curenv = srcenv;

    /* Check if sys_ept_map correctly verify the target env */
    if ((r = env_alloc(&dstenv, srcenv->env_id)) < 0)
        panic("Failed to allocate env (%d)\n", r);
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
        cprintf("EPT map to non-guest env failed as expected (%d).\n", r);
    else
        panic("sys_ept_map success on non-guest env.\n");

    /*env_destroy(dstenv);*/

    if ((r = env_guest_alloc(&dstenv, srcenv->env_id)) < 0)
        panic("Failed to allocate guest env (%d)\n", r);
    dstenv->env_vmxinfo.phys_sz = (uint64_t)UTEMP + PGSIZE;

    /* Check if sys_ept_map can verify srcva correctly */
    if ((r = _export_sys_ept_map(srcenv->env_id, (void *)UTOP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
        cprintf("EPT map from above UTOP area failed as expected (%d).\n", r);
    else
        panic("sys_ept_map from above UTOP area success\n");
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP+1, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
        cprintf("EPT map from unaligned srcva failed as expected (%d).\n", r);
    else
        panic("sys_ept_map from unaligned srcva success\n");

    /* Check if sys_ept_map can verify guest_pa correctly */
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP + PGSIZE, __EPTE_READ)) < 0)
        cprintf("EPT map to out-of-boundary area failed as expected (%d).\n", r);
    else
        panic("sys_ept_map success on out-of-boundary area\n");
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP-1, __EPTE_READ)) < 0)
        cprintf("EPT map to unaligned guest_pa failed as expected (%d).\n", r);
    else
        panic("sys_ept_map success on unaligned guest_pa\n");

    /* Check if the sys_ept_map can verify the permission correctly */
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, 0)) < 0)
        cprintf("EPT map with empty perm parameter failed as expected (%d).\n", r);
    else
        panic("sys_ept_map success on empty perm\n");
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_WRITE)) < 0)
        cprintf("EPT map with write perm parameter failed as expected (%d).\n", r);
    else
        panic("sys_ept_map success on write perm\n");

    pp_ref = pp->pp_ref;
    /* Check if the sys_ept_map can succeed on correct setup */
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
        panic("Failed to do sys_ept_map (%d)\n", r);
    else
        cprintf("sys_ept_map finished normally.\n");
    if (pp->pp_ref != pp_ref + 1)
        panic("Failed on checking pp_ref\n");
    else
        cprintf("pp_ref incremented correctly\n");

    /* Check if the sys_ept_map can handle remapping correctly */
    pp_ref = pp->pp_ref;
    if ((r = _export_sys_ept_map(srcenv->env_id, UTEMP, dstenv->env_id, UTEMP, __EPTE_READ)) < 0)
        cprintf("sys_ept_map finished normally.\n");
    else
        panic("sys_ept_map success on remapping the same page\n");
    /* Check if the sys_ept_map reset the pp_ref after failed on remapping the same page */
    if (pp->pp_ref == pp_ref)
        cprintf("sys_ept_map handled pp_ref correctly.\n");
    else
        panic("sys_ept_map failed to handle pp_ref.\n");

    /* Check if ept_lookup_gpa can handle empty eptrt correctly */
    if ((r = ept_lookup_gpa(NULL, UTEMP, 0, &epte)) < 0)
        cprintf("EPT lookup with a null eptrt failed as expected\n");
    else
        panic ("ept_lookup_gpa success on null eptrt\n");


    /* Check if the mapping is valid */
    if ((r = ept_lookup_gpa(dstenv->env_pml4e, UTEMP, 0, &epte)) < 0)
        panic("Failed on ept_lookup_gpa (%d)\n", r);
    if (page2pa(pp) != (epte_addr(*epte)))
        panic("EPT mapping address mismatching (%x vs %x).\n",
                page2pa(pp), epte_addr(*epte));
    else
        cprintf("EPT mapping address looks good: %x vs %x.\n",
                page2pa(pp), epte_addr(*epte));

    /* Check if the map_hva2gpa handle the overwrite correctly */
    if ((r = ept_map_hva2gpa(dstenv->env_pml4e, page2kva(pp), UTEMP, __EPTE_READ, 0)) < 0)
        cprintf("map_hva2gpa handle not overwriting correctly\n");
    else
        panic("map_hva2gpa success on overwriting with non-overwrite parameter\n");

    /* Check if the map_hva2gpa can map a page */
    if ((r = ept_map_hva2gpa(dstenv->env_pml4e, page2kva(pp), UTEMP, __EPTE_READ, 1)) < 0)
        panic ("Failed on mapping a page from kva to gpa\n");
    else
        cprintf("map_hva2gpa success on mapping a page\n");

    /* Check if the map_hva2gpa set permission correctly */
    if ((r = ept_lookup_gpa(dstenv->env_pml4e, UTEMP, 0, &epte)) < 0)
        panic("Failed on ept_lookup_gpa (%d)\n", r);
    if (((uint64_t)*epte & (~EPTE_ADDR)) == (__EPTE_READ | __EPTE_TYPE( EPTE_TYPE_WB ) | __EPTE_IPAT))
        cprintf("map_hva2gpa success on perm check\n");
    else
        panic("map_hva2gpa didn't set permission correctly\n");
    /* Go through the extended page table to check if the immediate mappings are correct */
    dir = dstenv->env_pml4e;
    for ( i = EPT_LEVELS - 1; i > 0; --i ) {
            int idx = ADDR_TO_IDX(UTEMP, i);
            if (!epte_present(dir[idx])) {
                panic("Failed to find page table item at the immediate level %d.", i);
            }
        if (!(dir[idx] & __EPTE_FULL)) {
            panic("Permission check failed at immediate level %d.", i);
        }
        dir = (epte_t *) epte_page_vaddr(dir[idx]);
        }
    cprintf("EPT immediate mapping check passed\n");

    cprintf("Cheers! sys_ept_map seems to work correctly\n");
    /* stop running after test, as this is just a test run. */
    panic("Cheers! sys_ept_map seems to work correctly\n");

    return 0;
}
#endif

