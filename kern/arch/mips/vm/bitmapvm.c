#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <cpu.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

/* under dumbvm, always have 72k of user stack */
/* (this must be > 64K so argument blocks of size ARG_MAX will fit) */
#define DUMBVM_STACKPAGES 18

/*
 * Wrap ram_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock mem_lock = SPINLOCK_INITIALIZER;
static struct spinlock initialized_lock = SPINLOCK_INITIALIZER;

static uint32_t *free_pages = NULL;
static size_t *alloc_size = NULL;
static size_t ram_frames = 0;
static uint32_t offset_bits;
static uint32_t offset_mask;

static bool initialized = false;

static inline uint32_t bit_test(uint32_t *array, uint32_t n) {
	return array[n >> offset_bits] & (1U << (n & offset_mask));
}

static inline void bit_set(uint32_t *array, uint32_t n) {
	array[n >> offset_bits] |= 1U << (n & offset_mask);
}

static inline void bit_clear(uint32_t *array, uint32_t n) {
	array[n >> offset_bits] &= ~(1U << (n & offset_mask));
}

static bool is_initialized() {
    bool tmp_initialized;
    spinlock_acquire(&initialized_lock);
    tmp_initialized = initialized;
    spinlock_release(&initialized_lock);
    return tmp_initialized;
}

void vm_bootstrap(void) {
    uint32_t free_pages_sz, x;

    ram_frames = ram_getsize() / PAGE_SIZE;
    free_pages_sz = ram_frames / 8; //Di quanti byte ho bisogno

    /* alloc freeRamFrame and alloc_size */
    free_pages = kmalloc(free_pages_sz);
    if (free_pages == NULL) return;
    alloc_size = kmalloc(sizeof(*alloc_size) * ram_frames);
    if (alloc_size == NULL) {
        /* reset to disable this vm management */
        free_pages = NULL;
        return;
    }

    memset(free_pages, 0, free_pages_sz);
    memset(alloc_size, 0, sizeof(*alloc_size) * ram_frames);

    spinlock_acquire(&initialized_lock);
    initialized = true;
    spinlock_release(&initialized_lock);

    //calcolo log2(sizeof(uint32) * 8)
	x = sizeof(*free_pages) * 8;
	offset_bits = 0;

	while (x >>= 1) offset_bits++; //Dovrebbe essere 5

    offset_mask = (1U << offset_bits) - 1;
}

/*
 * Check if we're in a context that can sleep. While most of the
 * operations in dumbvm don't in fact sleep, in a real VM system many
 * of them would. In those, assert that sleeping is ok. This helps
 * avoid the situation where syscall-layer code that works ok with
 * dumbvm starts blowing up during the VM assignment.
 */
static void dumbvm_can_sleep(void) {
    if (CURCPU_EXISTS()) {
        /* must not hold spinlocks */
        KASSERT(curcpu->c_spinlocks == 0);

        /* must not be in an interrupt handler */
        KASSERT(curthread->t_in_interrupt == 0);
    }
}

/*
 * Vede se ci sono pagine di cui è stata fatta la free
 */
static paddr_t getfreeppages(size_t npages) {
    size_t i, contiguous, start;
    paddr_t address;

    if (!is_initialized()) return 0;

    spinlock_acquire(&mem_lock);

    for (i = contiguous = start = 0; i < ram_frames && contiguous < npages; i++) {
        if (bit_test(free_pages, i)) {  // Se non è usata
            contiguous++;

            if (contiguous == 1) {  // Primo che trovo
                start = i;
            }
        } else {
            contiguous = 0;
        }
    }

    if (contiguous >= npages) {  // Ne ho trovate abbastanza?
        for (i = start; i < start + npages; i++) {
            bit_clear(free_pages, i);  // Lo imposta a usato
        }

        alloc_size[start] = npages;
        address = (paddr_t)start * PAGE_SIZE;
    } else {
        address = 0;
    }

    spinlock_release(&mem_lock);

    return address;
}

static paddr_t getppages(unsigned long npages) {
    paddr_t addr;

    /* try freed pages first */
    addr = getfreeppages(npages);
    if (addr == 0) {
        /* call stealmem */
        spinlock_acquire(&stealmem_lock);
        addr = ram_stealmem(npages);
        spinlock_release(&stealmem_lock);
    }
    if (addr != 0 && is_initialized()) {
        spinlock_acquire(&mem_lock);
        alloc_size[addr / PAGE_SIZE] = npages;
        spinlock_release(&mem_lock);
    }

    return addr;
}

static int freeppages(paddr_t addr, unsigned long npages) {
    uint32_t i;
    paddr_t first;

    if (!is_initialized()) return 0;
    first = addr / PAGE_SIZE;
    KASSERT(alloc_size != NULL);
    KASSERT(ram_frames > first);

    spinlock_acquire(&mem_lock);
    for (i = first; i < first + npages; i++) {
        bit_set(free_pages, i);
    }
    spinlock_release(&mem_lock);

    return 1;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(unsigned npages) {
    paddr_t pa;

    dumbvm_can_sleep();
    pa = getppages(npages);
    if (pa == 0) {
        return 0;
    }
    return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr) {
    if (is_initialized()) {
        paddr_t paddr = addr - MIPS_KSEG0;
        size_t first = paddr / PAGE_SIZE;
        KASSERT(alloc_size != NULL);
        KASSERT(ram_frames > first);
        freeppages(paddr, alloc_size[first]);
    }
}

void vm_tlbshootdown(const struct tlbshootdown *ts) {
    (void)ts;
    panic("dumbvm tried to do tlb shootdown?!\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
    vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
    paddr_t paddr;
    int i;
    uint32_t ehi, elo;
    struct addrspace *as;
    int spl;

    faultaddress &= PAGE_FRAME;

    DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

    switch (faulttype) {
        case VM_FAULT_READONLY:
            /* We always create pages read-write, so we can't get this */
            panic("dumbvm: got VM_FAULT_READONLY\n");
        case VM_FAULT_READ:
        case VM_FAULT_WRITE:
            break;
        default:
            return EINVAL;
    }

    if (curproc == NULL) {
        /*
         * No process. This is probably a kernel fault early
         * in boot. Return EFAULT so as to panic instead of
         * getting into an infinite faulting loop.
         */
        return EFAULT;
    }

    as = proc_getas();
    if (as == NULL) {
        /*
         * No address space set up. This is probably also a
         * kernel fault early in boot.
         */
        return EFAULT;
    }

    /* Assert that the address space has been set up properly. */
    KASSERT(as->as_vbase1 != 0);
    KASSERT(as->as_pbase1 != 0);
    KASSERT(as->as_npages1 != 0);
    KASSERT(as->as_vbase2 != 0);
    KASSERT(as->as_pbase2 != 0);
    KASSERT(as->as_npages2 != 0);
    KASSERT(as->as_stackpbase != 0);
    KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
    KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
    KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
    KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
    KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

    vbase1 = as->as_vbase1;
    vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
    vbase2 = as->as_vbase2;
    vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
    stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
    stacktop = USERSTACK;

    if (faultaddress >= vbase1 && faultaddress < vtop1) {
        paddr = (faultaddress - vbase1) + as->as_pbase1;
    } else if (faultaddress >= vbase2 && faultaddress < vtop2) {
        paddr = (faultaddress - vbase2) + as->as_pbase2;
    } else if (faultaddress >= stackbase && faultaddress < stacktop) {
        paddr = (faultaddress - stackbase) + as->as_stackpbase;
    } else {
        return EFAULT;
    }

    /* make sure it's page-aligned */
    KASSERT((paddr & PAGE_FRAME) == paddr);

    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();

    for (i = 0; i < NUM_TLB; i++) {
        tlb_read(&ehi, &elo, i);
        if (elo & TLBLO_VALID) {
            continue;
        }
        ehi = faultaddress;
        elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
        DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
        tlb_write(ehi, elo, i);
        splx(spl);
        return 0;
    }

    kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
    splx(spl);
    return EFAULT;
}

struct addrspace *as_create(void) {
    struct addrspace *as = kmalloc(sizeof(struct addrspace));
    if (as == NULL) {
        return NULL;
    }

    as->as_vbase1 = 0;
    as->as_pbase1 = 0;
    as->as_npages1 = 0;
    as->as_vbase2 = 0;
    as->as_pbase2 = 0;
    as->as_npages2 = 0;
    as->as_stackpbase = 0;

    return as;
}

void as_destroy(struct addrspace *as) {
    dumbvm_can_sleep();
    freeppages(as->as_pbase1, as->as_npages1);
    freeppages(as->as_pbase2, as->as_npages2);
    freeppages(as->as_stackpbase, DUMBVM_STACKPAGES);
    kfree(as);
}

void as_activate(void) {
    int i, spl;
    struct addrspace *as;

    as = proc_getas();
    if (as == NULL) {
        return;
    }

    /* Disable interrupts on this CPU while frobbing the TLB. */
    spl = splhigh();

    for (i = 0; i < NUM_TLB; i++) {
        tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
    }

    splx(spl);
}

void as_deactivate(void) { /* nothing */
}

int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz, int readable, int writeable, int executable) {
    size_t npages;

    dumbvm_can_sleep();

    /* Align the region. First, the base... */
    sz += vaddr & ~(vaddr_t)PAGE_FRAME;
    vaddr &= PAGE_FRAME;

    /* ...and now the length. */
    sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

    npages = sz / PAGE_SIZE;

    /* We don't use these - all pages are read-write */
    (void)readable;
    (void)writeable;
    (void)executable;

    if (as->as_vbase1 == 0) {
        as->as_vbase1 = vaddr;
        as->as_npages1 = npages;
        return 0;
    }

    if (as->as_vbase2 == 0) {
        as->as_vbase2 = vaddr;
        as->as_npages2 = npages;
        return 0;
    }

    /*
     * Support for more than two regions is not available.
     */
    kprintf("dumbvm: Warning: too many regions\n");
    return ENOSYS;
}

static void as_zero_region(paddr_t paddr, unsigned npages) { bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE); }

int as_prepare_load(struct addrspace *as) {
    KASSERT(as->as_pbase1 == 0);
    KASSERT(as->as_pbase2 == 0);
    KASSERT(as->as_stackpbase == 0);

    dumbvm_can_sleep();

    as->as_pbase1 = getppages(as->as_npages1);
    if (as->as_pbase1 == 0) {
        return ENOMEM;
    }

    as->as_pbase2 = getppages(as->as_npages2);
    if (as->as_pbase2 == 0) {
        return ENOMEM;
    }

    as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
    if (as->as_stackpbase == 0) {
        return ENOMEM;
    }

    as_zero_region(as->as_pbase1, as->as_npages1);
    as_zero_region(as->as_pbase2, as->as_npages2);
    as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

    return 0;
}

int as_complete_load(struct addrspace *as) {
    dumbvm_can_sleep();
    (void)as;
    return 0;
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
    KASSERT(as->as_stackpbase != 0);

    *stackptr = USERSTACK;
    return 0;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
    struct addrspace *new;

    dumbvm_can_sleep();

    new = as_create();
    if (new == NULL) {
        return ENOMEM;
    }

    new->as_vbase1 = old->as_vbase1;
    new->as_npages1 = old->as_npages1;
    new->as_vbase2 = old->as_vbase2;
    new->as_npages2 = old->as_npages2;

    /* (Mis)use as_prepare_load to allocate some physical memory. */
    if (as_prepare_load(new)) {
        as_destroy(new);
        return ENOMEM;
    }

    KASSERT(new->as_pbase1 != 0);
    KASSERT(new->as_pbase2 != 0);
    KASSERT(new->as_stackpbase != 0);

    memmove((void *)PADDR_TO_KVADDR(new->as_pbase1), (const void *)PADDR_TO_KVADDR(old->as_pbase1), old->as_npages1 * PAGE_SIZE);

    memmove((void *)PADDR_TO_KVADDR(new->as_pbase2), (const void *)PADDR_TO_KVADDR(old->as_pbase2), old->as_npages2 * PAGE_SIZE);

    memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase), (const void *)PADDR_TO_KVADDR(old->as_stackpbase), DUMBVM_STACKPAGES * PAGE_SIZE);

    *ret = new;
    return 0;
}
